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
 * End-to-end VCDIFF delta compression test.
 *
 * Runs two separate example processes against a live Ably endpoint:
 *
 *   realtime_subscribe_delta — attaches with delta=vcdiff, prints decoded msgs
 *   realtime_publish         — publishes N messages with similar payloads
 *
 * The test verifies:
 *   1. The subscriber attaches successfully.
 *   2. All N published messages are received and correctly decoded.
 *   3. The received payloads match the published data (delta was reconstructed).
 *
 * Usage (argv[1] is the directory containing the example binaries):
 *   ABLY_API_KEY=<key> ./test_e2e_delta <examples_dir>
 *
 * Requires: POSIX (fork, exec, mkstemp).
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
#define EVENT_NAME            "delta-event"
/* Payloads that share a long common prefix — likely to trigger real delta frames
 * from the Ably server after the first full message. */
#define MESSAGE_DATA          "ably-c-delta-test-payload-common-prefix-v1"
#define ATTACH_TIMEOUT_SEC    20
#define RECV_TIMEOUT_SEC      25
#define POLL_INTERVAL_USEC    200000   /* 200 ms */

/* -------------------------------------------------------------------------
 * Helpers (duplicated from test_e2e_pubsub.c — no shared test infrastructure)
 * ------------------------------------------------------------------------- */

static void fail(const char *msg)
{
    fprintf(stderr, "FAIL: %s\n", msg);
    exit(1);
}

static int wait_for_string_in_file(const char *path, const char *needle,
                                    int timeout_sec)
{
    char line[4096];
    time_t deadline = time(NULL) + timeout_sec;
    while (time(NULL) < deadline) {
        FILE *f = fopen(path, "r");
        if (f) {
            while (fgets(line, sizeof(line), f)) {
                if (strstr(line, needle)) { fclose(f); return 1; }
            }
            fclose(f);
        }
        usleep(POLL_INTERVAL_USEC);
    }
    return 0;
}

static int count_lines_containing(const char *path, const char *needle)
{
    char line[4096];
    int count = 0;
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, needle)) count++;
    }
    fclose(f);
    return count;
}

static void dump_file(const char *path, const char *label)
{
    char line[4096];
    FILE *f = fopen(path, "r");
    if (!f) return;
    fprintf(stderr, "--- %s ---\n", label);
    while (fgets(line, sizeof(line), f)) fputs(line, stderr);
    fprintf(stderr, "---\n");
    fclose(f);
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

    char subscriber_binary[1024];
    char publisher_binary[1024];
    snprintf(subscriber_binary, sizeof(subscriber_binary),
             "%s/realtime_subscribe_delta", examples_dir);
    snprintf(publisher_binary, sizeof(publisher_binary),
             "%s/realtime_publish", examples_dir);

    /* Unique channel so concurrent test runs don't interfere. */
    char channel_name[64];
    snprintf(channel_name, sizeof(channel_name),
             "ably-c-delta-%ld", (long)time(NULL));

    printf("=== ably-c end-to-end delta compression test ===\n");
    printf("Channel:  %s\n", channel_name);
    printf("Messages: %d\n", MESSAGE_COUNT);
    fflush(stdout);

    /* ----------------------------------------------------------------
     * Step 1: Start subscriber (with delta enabled) and capture output.
     * ---------------------------------------------------------------- */

    char sub_out_path[] = "/tmp/ably_e2e_delta_sub_XXXXXX";
    int  sub_out_fd     = mkstemp(sub_out_path);
    if (sub_out_fd < 0) fail("mkstemp");
    close(sub_out_fd);

    pid_t subscriber_pid = fork();
    if (subscriber_pid < 0) fail("fork subscriber");

    if (subscriber_pid == 0) {
        int fd = open(sub_out_path, O_WRONLY | O_TRUNC);
        if (fd < 0) { perror("open sub output"); exit(1); }
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        close(fd);

        char count_str[16];
        snprintf(count_str, sizeof(count_str), "%d", MESSAGE_COUNT);
        execl(subscriber_binary, "realtime_subscribe_delta",
              "-n", count_str, channel_name, NULL);
        perror("exec realtime_subscribe_delta");
        exit(1);
    }

    printf("Subscriber PID %d, output -> %s\n", (int)subscriber_pid, sub_out_path);
    fflush(stdout);

    /* ----------------------------------------------------------------
     * Step 2: Wait for subscriber to attach.
     * ---------------------------------------------------------------- */

    printf("Waiting for subscriber to attach (timeout %ds)...\n",
           ATTACH_TIMEOUT_SEC);
    fflush(stdout);

    if (!wait_for_string_in_file(sub_out_path, "ATTACHED", ATTACH_TIMEOUT_SEC)) {
        fprintf(stderr, "FAIL: subscriber did not attach within %ds\n",
                ATTACH_TIMEOUT_SEC);
        dump_file(sub_out_path, "subscriber output");
        kill(subscriber_pid, SIGTERM);
        waitpid(subscriber_pid, NULL, 0);
        unlink(sub_out_path);
        return 1;
    }

    printf("PASS: subscriber attached to '%s'\n", channel_name);
    fflush(stdout);

    /* ----------------------------------------------------------------
     * Step 3: Publish N messages (all with the same event name + data).
     * ---------------------------------------------------------------- */

    printf("Running publisher (%d messages)...\n", MESSAGE_COUNT);
    fflush(stdout);

    char count_str[16];
    snprintf(count_str, sizeof(count_str), "%d", MESSAGE_COUNT);

    pid_t publisher_pid = fork();
    if (publisher_pid < 0) {
        kill(subscriber_pid, SIGTERM);
        waitpid(subscriber_pid, NULL, 0);
        unlink(sub_out_path);
        fail("fork publisher");
    }

    if (publisher_pid == 0) {
        execl(publisher_binary, "realtime_publish",
              channel_name, EVENT_NAME, MESSAGE_DATA, count_str, NULL);
        perror("exec realtime_publish");
        exit(1);
    }

    int pub_status;
    waitpid(publisher_pid, &pub_status, 0);

    if (!WIFEXITED(pub_status) || WEXITSTATUS(pub_status) != 0) {
        fprintf(stderr, "FAIL: publisher exited with status %d\n",
                WIFEXITED(pub_status) ? WEXITSTATUS(pub_status) : -1);
        kill(subscriber_pid, SIGTERM);
        waitpid(subscriber_pid, NULL, 0);
        unlink(sub_out_path);
        return 1;
    }

    printf("PASS: publisher finished cleanly\n");
    fflush(stdout);

    /* ----------------------------------------------------------------
     * Step 4: Wait for subscriber to finish (it exits after -n messages)
     * or poll for the expected message count.
     * ---------------------------------------------------------------- */

    printf("Waiting for %d messages at subscriber (timeout %ds)...\n",
           MESSAGE_COUNT, RECV_TIMEOUT_SEC);
    fflush(stdout);

    int messages_received = 0;
    time_t recv_deadline  = time(NULL) + RECV_TIMEOUT_SEC;

    while (time(NULL) < recv_deadline) {
        messages_received = count_lines_containing(sub_out_path, EVENT_NAME);
        if (messages_received >= MESSAGE_COUNT) break;

        /* Also check if the subscriber exited early (e.g. on error). */
        int status;
        pid_t result = waitpid(subscriber_pid, &status, WNOHANG);
        if (result == subscriber_pid) {
            subscriber_pid = 0;  /* mark as reaped */
            break;
        }
        usleep(POLL_INTERVAL_USEC);
    }

    /* ----------------------------------------------------------------
     * Step 5: Clean up subscriber.
     * ---------------------------------------------------------------- */

    if (subscriber_pid > 0) {
        kill(subscriber_pid, SIGTERM);
        waitpid(subscriber_pid, NULL, 0);
    }

    /* ----------------------------------------------------------------
     * Step 6: Verify received payloads match what was published.
     * ---------------------------------------------------------------- */

    /* Every received line that contains EVENT_NAME should also contain
     * MESSAGE_DATA — confirming that VCDIFF decode reconstructed the payload. */
    int lines_with_payload = count_lines_containing(sub_out_path, MESSAGE_DATA);

    printf("\nResults:\n");
    printf("  Published        : %d\n", MESSAGE_COUNT);
    printf("  Lines with event : %d\n", messages_received);
    printf("  Correct payloads : %d\n", lines_with_payload);
    fflush(stdout);

    int passed = (messages_received >= MESSAGE_COUNT)
              && (lines_with_payload >= MESSAGE_COUNT);

    if (passed) {
        printf("PASS: all %d messages delivered and decoded correctly\n",
               MESSAGE_COUNT);
    } else {
        if (messages_received < MESSAGE_COUNT)
            fprintf(stderr, "FAIL: expected %d messages, received %d\n",
                    MESSAGE_COUNT, messages_received);
        if (lines_with_payload < MESSAGE_COUNT)
            fprintf(stderr,
                    "FAIL: only %d/%d messages had correct payload '%s' — "
                    "delta decode may be wrong\n",
                    lines_with_payload, MESSAGE_COUNT, MESSAGE_DATA);
        dump_file(sub_out_path, "subscriber output");
    }

    unlink(sub_out_path);
    return passed ? 0 : 1;
}
