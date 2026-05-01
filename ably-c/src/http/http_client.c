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
 * Minimal HTTPS client for Ably REST API.
 *
 * Protocol: HTTP/1.1 with "Connection: close".  Each request opens a new
 * TLS connection and closes it on completion.  Ably's REST tier is stateless
 * and the volume of REST calls from a typical library is low (order of
 * magnitude: tens per second at most), so connection-per-request is simpler
 * and more portable than connection pooling.
 *
 * TigerStyle: all buffers are fixed-size, allocated once at create time.
 * No allocation occurs during a request.
 */

#include "http_client.h"
#include "base64.h"
#include "tls_ca.h"

#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* ---------------------------------------------------------------------------
 * Internal structure
 * --------------------------------------------------------------------------- */

struct ably_http_client_s {
    /* TLS context — re-used across requests (reset per-connection). */
    mbedtls_ssl_context      ssl;
    mbedtls_ssl_config       ssl_conf;
    mbedtls_entropy_context  entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_x509_crt         ca_chain;  /* empty if tls_verify_peer=0 */

    /* Connection options (fixed at create time). */
    char     host[ABLY_HTTP_HOST_MAX];
    char     port_str[8];               /* "443" */
    long     timeout_ms;
    int      tls_verify_peer;

    /* Pre-built Authorization header value (including the header name). */
    char auth_header[ABLY_HTTP_AUTH_HEADER_MAX];

    /* Link: header value from the last GET response (for pagination). */
    char link_header[512];

    /* Pre-allocated I/O buffers. */
    char *request_buf;                  /* ABLY_HTTP_REQUEST_BUF_SIZE  */
    char *response_buf;                 /* ABLY_HTTP_RESPONSE_BUF_SIZE */

    ably_allocator_t alloc;
    ably_log_ctx_t   log;
};

/* ---------------------------------------------------------------------------
 * Lifecycle
 * --------------------------------------------------------------------------- */

ably_http_client_t *ably_http_client_create(const ably_http_options_t *opts,
                                              const char                *auth_header,
                                              const ably_allocator_t    *alloc,
                                              const ably_log_ctx_t      *log)
{
    assert(opts != NULL);
    assert(auth_header != NULL);

    ably_allocator_t a = alloc ? *alloc : ably_system_allocator();

    ably_http_client_t *c = ably_mem_malloc(&a, sizeof(*c));
    if (!c) return NULL;
    memset(c, 0, sizeof(*c));

    c->alloc = a;
    if (log) c->log = *log;

    /* Copy options. */
    snprintf(c->host, sizeof(c->host), "%s", opts->host ? opts->host : "rest.ably.io");
    snprintf(c->port_str, sizeof(c->port_str), "%u",
             opts->port ? (unsigned)opts->port : 443u);
    c->timeout_ms     = opts->timeout_ms > 0 ? opts->timeout_ms : 10000;
    c->tls_verify_peer = opts->tls_verify_peer;

    /* Copy auth header. */
    snprintf(c->auth_header, sizeof(c->auth_header), "%s", auth_header);

    /* Pre-allocate I/O buffers. */
    c->request_buf = ably_mem_malloc(&a, ABLY_HTTP_REQUEST_BUF_SIZE);
    if (!c->request_buf) goto fail;
    c->response_buf = ably_mem_malloc(&a, ABLY_HTTP_RESPONSE_BUF_SIZE);
    if (!c->response_buf) goto fail;

    /* Initialise mbedTLS objects. */
    mbedtls_ssl_init(&c->ssl);
    mbedtls_ssl_config_init(&c->ssl_conf);
    mbedtls_entropy_init(&c->entropy);
    mbedtls_ctr_drbg_init(&c->ctr_drbg);
    mbedtls_x509_crt_init(&c->ca_chain);

    const char *pers = "ably_http";
    int ret = mbedtls_ctr_drbg_seed(&c->ctr_drbg, mbedtls_entropy_func,
                                      &c->entropy,
                                      (const unsigned char *)pers, strlen(pers));
    if (ret != 0) {
        ABLY_LOG_E(&c->log, "mbedtls_ctr_drbg_seed failed: %d", ret);
        goto fail_tls;
    }

    ret = mbedtls_ssl_config_defaults(&c->ssl_conf,
                                       MBEDTLS_SSL_IS_CLIENT,
                                       MBEDTLS_SSL_TRANSPORT_STREAM,
                                       MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
        ABLY_LOG_E(&c->log, "mbedtls_ssl_config_defaults failed: %d", ret);
        goto fail_tls;
    }

    mbedtls_ssl_conf_rng(&c->ssl_conf, mbedtls_ctr_drbg_random, &c->ctr_drbg);

    if (c->tls_verify_peer) {
        if (opts->ca_cert_pem_path && opts->ca_cert_pem_path[0]) {
            int ca_ret = mbedtls_x509_crt_parse_file(&c->ca_chain,
                                                      opts->ca_cert_pem_path);
            if (ca_ret != 0) {
                ABLY_LOG_E(&c->log, "Failed to load CA cert from '%s': %d",
                           opts->ca_cert_pem_path, ca_ret);
                goto fail_tls;
            }
        } else {
            ably_tls_load_system_ca(&c->ca_chain, &c->log);
        }
        mbedtls_ssl_conf_authmode(&c->ssl_conf, MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_ca_chain(&c->ssl_conf, &c->ca_chain, NULL);
    } else {
        mbedtls_ssl_conf_authmode(&c->ssl_conf, MBEDTLS_SSL_VERIFY_NONE);
    }

    return c;

fail_tls:
    mbedtls_ssl_free(&c->ssl);
    mbedtls_ssl_config_free(&c->ssl_conf);
    mbedtls_entropy_free(&c->entropy);
    mbedtls_ctr_drbg_free(&c->ctr_drbg);
    mbedtls_x509_crt_free(&c->ca_chain);
fail:
    if (c->request_buf)  ably_mem_free(&a, c->request_buf);
    if (c->response_buf) ably_mem_free(&a, c->response_buf);
    ably_mem_free(&a, c);
    return NULL;
}

void ably_http_client_destroy(ably_http_client_t *client)
{
    if (!client) return;
    mbedtls_ssl_free(&client->ssl);
    mbedtls_ssl_config_free(&client->ssl_conf);
    mbedtls_entropy_free(&client->entropy);
    mbedtls_ctr_drbg_free(&client->ctr_drbg);
    mbedtls_x509_crt_free(&client->ca_chain);
    ably_mem_free(&client->alloc, client->request_buf);
    ably_mem_free(&client->alloc, client->response_buf);
    ably_mem_free(&client->alloc, client);
}

const char *ably_http_last_link_header(const ably_http_client_t *client)
{
    assert(client != NULL);
    return client->link_header;
}

/* ---------------------------------------------------------------------------
 * Send all bytes in buf, handling partial writes.
 * --------------------------------------------------------------------------- */
static ably_error_t send_all(mbedtls_ssl_context *ssl,
                               const unsigned char *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        int ret = mbedtls_ssl_write(ssl, buf + sent, len - sent);
        if (ret == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (ret <= 0) return ABLY_ERR_NETWORK;
        sent += (size_t)ret;
    }
    return ABLY_OK;
}

/* ---------------------------------------------------------------------------
 * Read HTTP response headers, extract status code and Content-Length.
 * Returns the offset into response_buf where the body starts.
 * --------------------------------------------------------------------------- */
/* Case-insensitive comparison of the first n chars of s against a lowercase ref. */
static int hdr_name_matches(const char *s, size_t s_len, const char *ref, size_t ref_len)
{
    if (s_len < ref_len) return 0;
    for (size_t k = 0; k < ref_len; k++) {
        char c = s[k];
        if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
        if (c != ref[k]) return 0;
    }
    return 1;
}

static int parse_response_headers(const char *buf, size_t len,
                                   long *status_out, size_t *content_length_out,
                                   char *link_out, size_t link_out_len)
{
    *status_out = 0;
    *content_length_out = 0;
    if (link_out && link_out_len > 0) link_out[0] = '\0';

    /* Parse status line: "HTTP/1.1 201 Created\r\n" */
    if (len < 12) return -1;
    if (strncmp(buf, "HTTP/", 5) != 0) return -1;

    const char *sp = memchr(buf, ' ', len < 20 ? len : 20);
    if (!sp) return -1;
    *status_out = strtol(sp + 1, NULL, 10);

    /* Find end of headers ("\r\n\r\n"). */
    const char *hdr_end = NULL;
    for (size_t i = 0; i + 3 < len; i++) {
        if (buf[i] == '\r' && buf[i+1] == '\n' && buf[i+2] == '\r' && buf[i+3] == '\n') {
            hdr_end = buf + i + 4;
            break;
        }
    }
    if (!hdr_end) return -1;

    /* Walk header lines and extract Content-Length and Link. */
    const char *p = buf;
    while (p < hdr_end) {
        const char *eol = memchr(p, '\r', (size_t)(hdr_end - p));
        if (!eol) break;
        size_t line_len = (size_t)(eol - p);

        const char *colon = memchr(p, ':', line_len);
        if (colon) {
            size_t name_len = (size_t)(colon - p);
            const char *val = colon + 1;
            /* Skip leading whitespace in value. */
            while (val < eol && (*val == ' ' || *val == '\t')) val++;
            size_t val_len = (size_t)(eol - val);

            if (hdr_name_matches(p, name_len, "content-length", 14)) {
                *content_length_out = (size_t)strtoul(val, NULL, 10);
            } else if (link_out && link_out_len > 0 &&
                       hdr_name_matches(p, name_len, "link", 4)) {
                size_t copy = val_len < link_out_len - 1 ? val_len : link_out_len - 1;
                memcpy(link_out, val, copy);
                link_out[copy] = '\0';
            }
        }
        p = eol + 2;  /* skip \r\n */
    }

    return (int)(hdr_end - buf);
}

/* ---------------------------------------------------------------------------
 * ably_http_post
 * --------------------------------------------------------------------------- */

ably_error_t ably_http_post(ably_http_client_t *client,
                              const char         *path,
                              const char         *content_type,
                              const uint8_t      *body,
                              size_t              body_len,
                              long               *http_status)
{
    assert(client != NULL);
    assert(path != NULL);
    assert(http_status != NULL);

    *http_status = 0;

    /* Build the HTTP request into the pre-allocated request buffer. */
    int hdr_len = snprintf(client->request_buf, ABLY_HTTP_REQUEST_BUF_SIZE,
        "POST %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "%s\r\n"                        /* Authorization: Basic ... */
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        path,
        client->host,
        client->auth_header,
        content_type ? content_type : "application/json",
        body_len);

    if (hdr_len < 0 || (size_t)hdr_len >= ABLY_HTTP_REQUEST_BUF_SIZE - body_len) {
        ABLY_LOG_E(&client->log, "HTTP request too large for buffer");
        return ABLY_ERR_NETWORK;
    }

    /* Append body to request buffer. */
    if (body && body_len > 0) {
        if ((size_t)hdr_len + body_len >= ABLY_HTTP_REQUEST_BUF_SIZE) {
            ABLY_LOG_E(&client->log, "HTTP body too large for request buffer");
            return ABLY_ERR_NETWORK;
        }
        memcpy(client->request_buf + hdr_len, body, body_len);
    }
    size_t request_len = (size_t)hdr_len + body_len;

    /* Connect. */
    mbedtls_net_context net;
    mbedtls_net_init(&net);

    ABLY_LOG_D(&client->log, "HTTP POST %s%s", client->host, path);

    int ret = mbedtls_net_connect(&net, client->host, client->port_str,
                                   MBEDTLS_NET_PROTO_TCP);
    if (ret != 0) {
        ABLY_LOG_E(&client->log, "connect to %s:%s failed: %d",
                   client->host, client->port_str, ret);
        mbedtls_net_free(&net);
        return ABLY_ERR_NETWORK;
    }

    /* Set non-blocking with timeout via mbedtls_net_set_nonblock + poll.
     * Simpler: use blocking mode and rely on the OS timeout. */
    mbedtls_net_set_block(&net);

    /* TLS handshake. */
    ret = mbedtls_ssl_setup(&client->ssl, &client->ssl_conf);
    if (ret != 0) {
        ABLY_LOG_E(&client->log, "ssl_setup failed: %d", ret);
        mbedtls_net_free(&net);
        return ABLY_ERR_NETWORK;
    }

    mbedtls_ssl_set_bio(&client->ssl, &net,
                         mbedtls_net_send, mbedtls_net_recv,
                         mbedtls_net_recv_timeout);
    mbedtls_ssl_set_hostname(&client->ssl, client->host);
    mbedtls_ssl_conf_read_timeout(&client->ssl_conf, (uint32_t)client->timeout_ms);

    do { ret = mbedtls_ssl_handshake(&client->ssl); }
    while (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE);

    if (ret != 0) {
        ABLY_LOG_E(&client->log, "TLS handshake failed: %d", ret);
        mbedtls_ssl_free(&client->ssl);
        mbedtls_ssl_init(&client->ssl);
        mbedtls_net_free(&net);
        return ABLY_ERR_NETWORK;
    }

    /* Send request. */
    ably_error_t err = send_all(&client->ssl,
                                 (const unsigned char *)client->request_buf,
                                 request_len);
    if (err != ABLY_OK) {
        ABLY_LOG_E(&client->log, "send failed");
        goto close;
    }

    /* Read response into response buffer. */
    size_t total_recv = 0;
    while (total_recv < ABLY_HTTP_RESPONSE_BUF_SIZE - 1) {
        ret = mbedtls_ssl_read(&client->ssl,
                                (unsigned char *)client->response_buf + total_recv,
                                ABLY_HTTP_RESPONSE_BUF_SIZE - 1 - total_recv);
        if (ret == MBEDTLS_ERR_SSL_WANT_READ) continue;
        if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || ret == 0) break;
        if (ret < 0) {
            ABLY_LOG_E(&client->log, "ssl_read failed: %d", ret);
            err = ABLY_ERR_NETWORK;
            goto close;
        }
        total_recv += (size_t)ret;
    }
    client->response_buf[total_recv] = '\0';

    /* Parse status line. */
    size_t content_length = 0;
    int body_offset = parse_response_headers(client->response_buf, total_recv,
                                              http_status, &content_length,
                                              NULL, 0);
    if (body_offset < 0) {
        ABLY_LOG_E(&client->log, "Failed to parse HTTP response headers");
        err = ABLY_ERR_PROTOCOL;
        goto close;
    }

    ABLY_LOG_D(&client->log, "HTTP %ld (body offset=%d content-length=%zu)",
               *http_status, body_offset, content_length);

close:
    mbedtls_ssl_close_notify(&client->ssl);
    mbedtls_ssl_free(&client->ssl);
    mbedtls_ssl_init(&client->ssl);   /* reset for next use */
    mbedtls_net_free(&net);
    return err;
}

/* ---------------------------------------------------------------------------
 * ably_http_get
 * --------------------------------------------------------------------------- */

ably_error_t ably_http_get(ably_http_client_t  *client,
                             const char          *path,
                             long                *http_status,
                             const char         **resp_body_out,
                             size_t              *resp_body_len)
{
    assert(client      != NULL);
    assert(path        != NULL);
    assert(http_status != NULL);

    *http_status = 0;
    if (resp_body_out) *resp_body_out = NULL;
    if (resp_body_len) *resp_body_len = 0;

    int hdr_len = snprintf(client->request_buf, ABLY_HTTP_REQUEST_BUF_SIZE,
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "%s\r\n"
        "Accept: application/json\r\n"
        "Connection: close\r\n"
        "\r\n",
        path,
        client->host,
        client->auth_header);

    if (hdr_len < 0 || (size_t)hdr_len >= ABLY_HTTP_REQUEST_BUF_SIZE) {
        ABLY_LOG_E(&client->log, "HTTP GET request too large for buffer");
        return ABLY_ERR_NETWORK;
    }

    mbedtls_net_context net;
    mbedtls_net_init(&net);

    ABLY_LOG_D(&client->log, "HTTP GET %s%s", client->host, path);

    int ret = mbedtls_net_connect(&net, client->host, client->port_str,
                                   MBEDTLS_NET_PROTO_TCP);
    if (ret != 0) {
        ABLY_LOG_E(&client->log, "connect to %s:%s failed: %d",
                   client->host, client->port_str, ret);
        mbedtls_net_free(&net);
        return ABLY_ERR_NETWORK;
    }
    mbedtls_net_set_block(&net);

    ret = mbedtls_ssl_setup(&client->ssl, &client->ssl_conf);
    if (ret != 0) {
        ABLY_LOG_E(&client->log, "ssl_setup failed: %d", ret);
        mbedtls_net_free(&net);
        return ABLY_ERR_NETWORK;
    }

    mbedtls_ssl_set_bio(&client->ssl, &net,
                         mbedtls_net_send, mbedtls_net_recv,
                         mbedtls_net_recv_timeout);
    mbedtls_ssl_set_hostname(&client->ssl, client->host);
    mbedtls_ssl_conf_read_timeout(&client->ssl_conf, (uint32_t)client->timeout_ms);

    do { ret = mbedtls_ssl_handshake(&client->ssl); }
    while (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE);

    if (ret != 0) {
        ABLY_LOG_E(&client->log, "TLS handshake failed: %d", ret);
        mbedtls_ssl_free(&client->ssl);
        mbedtls_ssl_init(&client->ssl);
        mbedtls_net_free(&net);
        return ABLY_ERR_NETWORK;
    }

    ably_error_t err = send_all(&client->ssl,
                                 (const unsigned char *)client->request_buf,
                                 (size_t)hdr_len);
    if (err != ABLY_OK) {
        ABLY_LOG_E(&client->log, "send failed");
        goto get_close;
    }

    size_t total_recv = 0;
    while (total_recv < ABLY_HTTP_RESPONSE_BUF_SIZE - 1) {
        ret = mbedtls_ssl_read(&client->ssl,
                                (unsigned char *)client->response_buf + total_recv,
                                ABLY_HTTP_RESPONSE_BUF_SIZE - 1 - total_recv);
        if (ret == MBEDTLS_ERR_SSL_WANT_READ) continue;
        if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || ret == 0) break;
        if (ret < 0) {
            ABLY_LOG_E(&client->log, "ssl_read failed: %d", ret);
            err = ABLY_ERR_NETWORK;
            goto get_close;
        }
        total_recv += (size_t)ret;
    }
    client->response_buf[total_recv] = '\0';

    size_t content_length = 0;
    client->link_header[0] = '\0';
    int body_offset = parse_response_headers(client->response_buf, total_recv,
                                              http_status, &content_length,
                                              client->link_header,
                                              sizeof(client->link_header));
    if (body_offset < 0) {
        ABLY_LOG_E(&client->log, "Failed to parse HTTP response headers");
        err = ABLY_ERR_PROTOCOL;
        goto get_close;
    }

    ABLY_LOG_D(&client->log, "HTTP GET %ld (body offset=%d content-length=%zu)",
               *http_status, body_offset, content_length);

    if (resp_body_out) *resp_body_out = client->response_buf + body_offset;
    if (resp_body_len) {
        size_t body_available = total_recv - (size_t)body_offset;
        *resp_body_len = (content_length > 0 && content_length < body_available)
                         ? content_length : body_available;
    }

get_close:
    mbedtls_ssl_close_notify(&client->ssl);
    mbedtls_ssl_free(&client->ssl);
    mbedtls_ssl_init(&client->ssl);
    mbedtls_net_free(&net);
    return err;
}
