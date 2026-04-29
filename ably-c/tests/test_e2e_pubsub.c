/*
 * Copyright 2024 Ben Gamble
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * End-to-end pub/sub test.
 *
 * Runs two separate example processes against a live Ably endpoint:
 *
 *   realtime_subscribe  — attaches to a channel, prints received messages
 *   realtime_publish    — connects, attaches, publishes N messages, exits
 *
 * The test orchestrates both processes:
 *   1. Start the subscriber and wait until it prints "ATTACHED".
 *   2. Run the publisher; wait for it to finish.
 *   3. Wait up to RECV_TIMEOUT_SEC for all N messages to appear in the
 *      subscriber's stdout.
 *   4. Kill the subscriber.
 *   5. Report PASS if message_count >= expected, FAIL otherwise.
 *
 * Usage (argv[1] is the directory containing the example binaries):
 *   ABLY_API_KEY=<key> ./test_e2e_pubsub <examples_dir>
 *
 * Requires: POSIX (fork, exec, mkstemp, poll).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>

/* -------------------------------------------------------------------------
 * Configuration
 * ------------------------------------------------------------------------- */

#define MESSAGE_COUNT         5
#define EVENT_NAME            "e2e-event"
#define MESSAGE_DATA          "hello-e2e"
#define ATTACH_TIMEOUT_SEC    20
#define RECV_TIMEOUT_SEC      20
#define POLL_INTERVAL_USEC    200000   /* 200 ms */

/* -------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

static void fail(const char *msg)
{
    fprintf(stderr, "FAIL: %s\n", msg);
    exit(1);
}

/*
 * Scan file at path for needle.  Returns 1 if found, 0 if not within
 * timeout_sec seconds (polling every POLL_INTERVAL_USEC microseconds).
 */
static int wait_for_string_in_file(const char *path, const char *needle,
                                    int timeout_sec)
{
    char line[4096];
    time_t deadline = time(NULL) + timeout_sec;

    while (time(NULL) < deadline) {
        FILE *file = fopen(path, "r");
        if (file) {
            while (fgets(line, sizeof(line), file)) {
                if (strstr(line, needle)) {
                    fclose(file);
                    return 1;
                }
            }
            fclose(file);
        }
        usleep(POLL_INTERVAL_USEC);
    }
    return 0;
}

/*
 * Count lines in file that contain needle.
 */
static int count_lines_containing(const char *path, const char *needle)
{
    char line[4096];
    int count = 0;
    FILE *file = fopen(path, "r");
    if (!file) return 0;
    while (fgets(line, sizeof(line), file)) {
        if (strstr(line, needle)) count++;
    }
    fclose(file);
    return count;
}

/*
 * Print file contents to stderr for diagnostics on failure.
 */
static void dump_file(const char *path, const char *label)
{
    char line[4096];
    FILE *file = fopen(path, "r");
    if (!file) return;
    fprintf(stderr, "--- %s ---\n", label);
    while (fgets(line, sizeof(line), file)) fputs(line, stderr);
    fprintf(stderr, "---\n");
    fclose(file);
}

/* -------------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------------- */

int main(int argc, char *argv[])
{
    const char *api_key = getenv("ABLY_API_KEY");
    if (!api_key || *api_key == '\0') {
        fprintf(stderr, "SKIP: ABLY_API_KEY not set\n");
        return 77;  /* ctest treats exit code 77 as SKIP */
    }

    const char *examples_dir = argc > 1 ? argv[1] : ".";

    /* Build absolute paths to the two example binaries. */
    char subscriber_binary[1024];
    char publisher_binary[1024];
    snprintf(subscriber_binary, sizeof(subscriber_binary),
             "%s/realtime_subscribe", examples_dir);
    snprintf(publisher_binary,  sizeof(publisher_binary),
             "%s/realtime_publish",   examples_dir);

    /* Unique channel name so concurrent test runs don't interfere. */
    char channel_name[64];
    snprintf(channel_name, sizeof(channel_name),
             "ably-c-e2e-%ld", (long)time(NULL));

    printf("=== ably-c end-to-end pub/sub test ===\n");
    printf("Channel:  %s\n", channel_name);
    printf("Messages: %d\n", MESSAGE_COUNT);
    fflush(stdout);

    /* ----------------------------------------------------------------
     * Step 1: Start subscriber, redirect its stdout to a temp file.
     * ---------------------------------------------------------------- */

    char subscriber_output_path[] = "/tmp/ably_e2e_sub_XXXXXX";
    int  subscriber_output_fd     = mkstemp(subscriber_output_path);
    if (subscriber_output_fd < 0) fail("mkstemp");
    close(subscriber_output_fd);

    pid_t subscriber_pid = fork();
    if (subscriber_pid < 0) fail("fork subscriber");

    if (subscriber_pid == 0) {
        /* Child: redirect stdout+stderr to the temp file, then exec subscriber. */
        int output_fd = open(subscriber_output_path, O_WRONLY | O_TRUNC);
        if (output_fd < 0) { perror("open subscriber output"); exit(1); }
        dup2(output_fd, STDOUT_FILENO);
        dup2(output_fd, STDERR_FILENO);
        close(output_fd);

        execl(subscriber_binary, "realtime_subscribe", channel_name, NULL);
        perror("exec realtime_subscribe");
        exit(1);
    }

    printf("Subscriber PID %d, output -> %s\n", (int)subscriber_pid,
           subscriber_output_path);
    fflush(stdout);

    /* ----------------------------------------------------------------
     * Step 2: Wait for subscriber to attach.
     * ---------------------------------------------------------------- */

    printf("Waiting for subscriber to attach (timeout %ds)...\n",
           ATTACH_TIMEOUT_SEC);
    fflush(stdout);

    if (!wait_for_string_in_file(subscriber_output_path, "ATTACHED",
                                  ATTACH_TIMEOUT_SEC)) {
        fprintf(stderr, "FAIL: subscriber did not attach within %ds\n",
                ATTACH_TIMEOUT_SEC);
        dump_file(subscriber_output_path, "subscriber output");
        kill(subscriber_pid, SIGTERM);
        waitpid(subscriber_pid, NULL, 0);
        unlink(subscriber_output_path);
        return 1;
    }

    printf("PASS: subscriber attached to '%s'\n", channel_name);
    fflush(stdout);

    /* ----------------------------------------------------------------
     * Step 3: Run publisher (blocks until it finishes publishing).
     * ---------------------------------------------------------------- */

    printf("Running publisher (%d messages)...\n", MESSAGE_COUNT);
    fflush(stdout);

    char message_count_str[16];
    snprintf(message_count_str, sizeof(message_count_str), "%d", MESSAGE_COUNT);

    pid_t publisher_pid = fork();
    if (publisher_pid < 0) {
        kill(subscriber_pid, SIGTERM);
        waitpid(subscriber_pid, NULL, 0);
        unlink(subscriber_output_path);
        fail("fork publisher");
    }

    if (publisher_pid == 0) {
        execl(publisher_binary, "realtime_publish",
              channel_name, EVENT_NAME, MESSAGE_DATA, message_count_str, NULL);
        perror("exec realtime_publish");
        exit(1);
    }

    int publisher_status;
    waitpid(publisher_pid, &publisher_status, 0);

    if (!WIFEXITED(publisher_status) || WEXITSTATUS(publisher_status) != 0) {
        fprintf(stderr, "FAIL: publisher exited with status %d\n",
                WIFEXITED(publisher_status) ? WEXITSTATUS(publisher_status) : -1);
        kill(subscriber_pid, SIGTERM);
        waitpid(subscriber_pid, NULL, 0);
        unlink(subscriber_output_path);
        return 1;
    }

    printf("PASS: publisher finished cleanly\n");
    fflush(stdout);

    /* ----------------------------------------------------------------
     * Step 4: Wait for all messages to appear at the subscriber.
     * ---------------------------------------------------------------- */

    printf("Waiting for %d messages at subscriber (timeout %ds)...\n",
           MESSAGE_COUNT, RECV_TIMEOUT_SEC);
    fflush(stdout);

    int messages_received = 0;
    time_t recv_deadline  = time(NULL) + RECV_TIMEOUT_SEC;

    while (time(NULL) < recv_deadline) {
        messages_received = count_lines_containing(subscriber_output_path,
                                                    EVENT_NAME);
        if (messages_received >= MESSAGE_COUNT) break;
        usleep(POLL_INTERVAL_USEC);
    }

    /* ----------------------------------------------------------------
     * Step 5: Clean up subscriber process.
     * ---------------------------------------------------------------- */

    kill(subscriber_pid, SIGTERM);
    waitpid(subscriber_pid, NULL, 0);

    /* ----------------------------------------------------------------
     * Step 6: Report results.
     * ---------------------------------------------------------------- */

    printf("\nResults:\n");
    printf("  Published : %d\n", MESSAGE_COUNT);
    printf("  Received  : %d\n", messages_received);
    fflush(stdout);

    int passed = messages_received >= MESSAGE_COUNT;

    if (passed) {
        printf("PASS: all %d messages delivered end-to-end\n", MESSAGE_COUNT);
    } else {
        fprintf(stderr, "FAIL: expected %d messages, received %d\n",
                MESSAGE_COUNT, messages_received);
        dump_file(subscriber_output_path, "subscriber output");
    }

    unlink(subscriber_output_path);
    return passed ? 0 : 1;
}
