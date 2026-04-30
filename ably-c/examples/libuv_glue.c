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
 * libuv event-loop integration for ably-c.
 *
 * This example shows how to drive an ably-c realtime client from within an
 * existing libuv event loop without spawning the library's internal service
 * thread.  The key APIs used are:
 *
 *   ably_rt_client_connect_async(client)
 *     — Performs the TLS+WebSocket handshake synchronously then returns.
 *       Does NOT start the internal service thread.
 *
 *   ably_rt_client_fd(client)
 *     — Returns the underlying socket file descriptor so libuv can watch it.
 *
 *   ably_rt_step(client, timeout_ms)
 *     — Drains one outbound frame and receives one inbound frame.
 *       Returns 1 if work was done, 0 on timeout, -1 on error/disconnect.
 *
 * Usage:
 *   ABLY_API_KEY=appId.keyId:secret ./libuv_glue [channel]
 *
 * Build requirements: libuv >= 1.x must be installed.
 * CMakeLists.txt wires this only when find_package(libuv) succeeds.
 */

#include <ably/ably_realtime.h>

#include <uv.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* -------------------------------------------------------------------------- */

typedef struct {
    ably_rt_client_t *client;
    ably_channel_t   *channel;
    uv_poll_t         poll;      /* watches the Ably socket fd */
    uv_timer_t        watchdog;  /* reconnect / timeout timer  */
    uv_loop_t        *loop;
    int               reconnect_ms;
} ably_uv_ctx_t;

/* Forward declarations. */
static void do_connect(ably_uv_ctx_t *ctx);
static void poll_cb(uv_poll_t *handle, int status, int events);
static void reconnect_timer_cb(uv_timer_t *handle);

/* --------------------------------------------------------------------------
 * Message subscriber (called by ably_rt_step → dispatch → channel)
 * -------------------------------------------------------------------------- */

static void on_message(ably_channel_t *ch, const ably_message_t *msg,
                        void *user_data)
{
    (void)ch; (void)user_data;
    printf("[msg] name=%-20s  data=%s\n",
           msg->name ? msg->name : "(null)",
           msg->data ? msg->data : "");
}

/* --------------------------------------------------------------------------
 * Poll callback: called by libuv when the socket has data to read.
 * -------------------------------------------------------------------------- */

static void poll_cb(uv_poll_t *handle, int status, int events)
{
    (void)events;
    ably_uv_ctx_t *ctx = handle->data;

    if (status < 0) {
        fprintf(stderr, "[libuv] poll error: %s — reconnecting\n",
                uv_strerror(status));
        uv_poll_stop(&ctx->poll);
        /* Schedule reconnect. */
        uv_timer_start(&ctx->watchdog, reconnect_timer_cb,
                       (uint64_t)ctx->reconnect_ms, 0);
        return;
    }

    int rc = ably_rt_step(ctx->client, 0);
    if (rc < 0) {
        fprintf(stderr, "[ably] connection dropped — reconnecting\n");
        uv_poll_stop(&ctx->poll);
        uv_timer_start(&ctx->watchdog, reconnect_timer_cb,
                       (uint64_t)ctx->reconnect_ms, 0);
        /* Exponential backoff (cap at 30 s). */
        ctx->reconnect_ms = ctx->reconnect_ms < 30000
                            ? ctx->reconnect_ms * 2 : 30000;
    }
}

/* --------------------------------------------------------------------------
 * Reconnect timer: fired after a connection drop or failed connect attempt.
 * -------------------------------------------------------------------------- */

static void reconnect_timer_cb(uv_timer_t *handle)
{
    ably_uv_ctx_t *ctx = handle->data;
    do_connect(ctx);
}

/* --------------------------------------------------------------------------
 * Connect (or reconnect) the Ably client and register a poll watcher.
 * -------------------------------------------------------------------------- */

static void do_connect(ably_uv_ctx_t *ctx)
{
    ably_error_t err = ably_rt_client_connect_async(ctx->client);
    if (err != ABLY_OK) {
        fprintf(stderr, "[ably] connect_async failed (%d) — retrying in %dms\n",
                (int)err, ctx->reconnect_ms);
        uv_timer_start(&ctx->watchdog, reconnect_timer_cb,
                       (uint64_t)ctx->reconnect_ms, 0);
        ctx->reconnect_ms = ctx->reconnect_ms < 30000
                            ? ctx->reconnect_ms * 2 : 30000;
        return;
    }

    ctx->reconnect_ms = 1000; /* reset backoff on successful connect */

    /* Attach the channel. */
    ably_channel_attach(ctx->channel);

    /* Register libuv poll on the Ably socket fd. */
    int fd = ably_rt_client_fd(ctx->client);
    assert(fd >= 0);

    uv_poll_init(ctx->loop, &ctx->poll, fd);
    ctx->poll.data = ctx;
    uv_poll_start(&ctx->poll, UV_READABLE, poll_cb);

    printf("[libuv] connected, watching fd=%d\n", fd);
}

/* --------------------------------------------------------------------------
 * main
 * -------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    const char *api_key = getenv("ABLY_API_KEY");
    if (!api_key || !*api_key) {
        fprintf(stderr, "Set ABLY_API_KEY=appId.keyId:secret\n");
        return 1;
    }
    const char *channel_name = (argc > 1) ? argv[1] : "libuv-demo";

    /* Create Ably client (no service thread — we drive it via ably_rt_step). */
    ably_rt_client_t *client = ably_rt_client_create(api_key, NULL, NULL);
    if (!client) { fprintf(stderr, "failed to create client\n"); return 1; }

    ably_channel_t *ch = ably_rt_channel_get(client, channel_name);
    if (!ch) { fprintf(stderr, "failed to get channel\n"); return 1; }
    ably_channel_subscribe(ch, NULL, on_message, NULL);

    /* Set up libuv context. */
    uv_loop_t *loop = uv_default_loop();

    ably_uv_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.client       = client;
    ctx.channel      = ch;
    ctx.loop         = loop;
    ctx.reconnect_ms = 1000;

    uv_timer_init(loop, &ctx.watchdog);
    ctx.watchdog.data = &ctx;

    /* Initiate first connection. */
    do_connect(&ctx);

    printf("[libuv] running event loop (Ctrl-C to exit)\n");
    uv_run(loop, UV_RUN_DEFAULT);

    uv_loop_close(loop);
    ably_rt_client_destroy(client);
    return 0;
}
