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
 * System CA certificate bundle loader for mbedTLS.
 *
 * Tries well-known paths on each supported platform.  The first path that
 * successfully parses at least one certificate is used.
 *
 * Platform lookup order:
 *   macOS / *BSD  : /etc/ssl/cert.pem
 *   Debian/Ubuntu : /etc/ssl/certs/ca-certificates.crt
 *   RHEL/Fedora   : /etc/pki/tls/certs/ca-bundle.crt
 *   Alpine/musl   : /etc/ssl/certs/ca-certificates.crt  (same path)
 *   OpenSUSE      : /etc/ssl/ca-bundle.pem
 *   Android       : /etc/security/cacerts/  (directory — not handled here;
 *                   on Android disable peer verify or link the system bundle)
 *   Windows       : use mbedtls_x509_crt_parse (Windows cert store not
 *                   directly accessible from mbedTLS without platform hooks)
 */

#include "tls_ca.h"
#include "log.h"

#include "mbedtls/x509_crt.h"

#include <string.h>

#ifdef _WIN32
int ably_tls_load_system_ca(mbedtls_x509_crt *ca_chain,
                              const ably_log_ctx_t *log)
{
    /* Windows: mbedTLS does not directly access the Windows cert store.
     * Callers that need peer verification on Windows should either:
     *   a) provide a PEM file via ABLY_CA_BUNDLE_PATH environment variable, or
     *   b) set tls_verify_peer = 0 for development builds.
     * For production use, bundle a trusted CA PEM and load it here. */
    const char *env_path = getenv("ABLY_CA_BUNDLE_PATH");
    if (env_path) {
        int ret = mbedtls_x509_crt_parse_file(ca_chain, env_path);
        if (ret == 0) {
            ABLY_LOG_I(log, "Loaded CA bundle from ABLY_CA_BUNDLE_PATH: %s",
                       env_path);
            return 1;
        }
    }
    ABLY_LOG_W(log,
        "No CA bundle found on Windows. "
        "Set ABLY_CA_BUNDLE_PATH or use tls_verify_peer=0.");
    return 0;
}
#else

/* Platform CA bundle search paths, in order of preference. */
static const char * const k_ca_paths[] = {
    "/etc/ssl/cert.pem",                       /* macOS, FreeBSD            */
    "/etc/ssl/certs/ca-certificates.crt",      /* Debian, Ubuntu, Alpine    */
    "/etc/pki/tls/certs/ca-bundle.crt",        /* RHEL, CentOS, Fedora      */
    "/etc/ssl/ca-bundle.pem",                  /* OpenSUSE                  */
    "/usr/local/share/certs/ca-root-nss.crt",  /* FreeBSD ports (NSS)       */
    "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem", /* RHEL 7+        */
    NULL
};

int ably_tls_load_system_ca(mbedtls_x509_crt *ca_chain,
                              const ably_log_ctx_t *log)
{
    /* Allow override via environment variable. */
    const char *env_path = getenv("ABLY_CA_BUNDLE_PATH");
    if (env_path) {
        int ret = mbedtls_x509_crt_parse_file(ca_chain, env_path);
        if (ret == 0) {
            ABLY_LOG_I(log, "Loaded CA bundle from ABLY_CA_BUNDLE_PATH: %s",
                       env_path);
            return 1;
        }
        ABLY_LOG_W(log, "ABLY_CA_BUNDLE_PATH set but failed to parse: %s "
                   "(mbedTLS ret=%d)", env_path, ret);
    }

    for (int i = 0; k_ca_paths[i] != NULL; i++) {
        int ret = mbedtls_x509_crt_parse_file(ca_chain, k_ca_paths[i]);
        if (ret == 0) {
            ABLY_LOG_I(log, "Loaded CA bundle: %s", k_ca_paths[i]);
            return 1;
        }
        if (ret > 0) {
            /* Partial success: some certs parsed but some failed (ret = count
             * of failed certs).  Accept this — it's a real bundle. */
            ABLY_LOG_I(log,
                       "Loaded CA bundle: %s (%d cert(s) skipped)",
                       k_ca_paths[i], ret);
            return 1;
        }
    }

    ABLY_LOG_W(log,
        "No system CA bundle found. "
        "Set ABLY_CA_BUNDLE_PATH to a PEM file, or use tls_verify_peer=0.");
    return 0;
}
#endif /* _WIN32 */
