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

#ifndef ABLY_TLS_CA_H
#define ABLY_TLS_CA_H

#include "mbedtls/x509_crt.h"
#include "log.h"

/*
 * Load the system CA certificate bundle into an mbedtls_x509_crt chain.
 *
 * Tries platform-specific paths in order and loads the first one found.
 * If no bundle is found, logs a warning and leaves the chain empty (which
 * will cause TLS verification to fail — the caller should handle this).
 *
 * Returns the number of certificates loaded (0 on failure).
 */
int ably_tls_load_system_ca(mbedtls_x509_crt *ca_chain,
                              const ably_log_ctx_t *log);

#endif /* ABLY_TLS_CA_H */
