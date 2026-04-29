/*
 * Compile-time check that ably.hpp parses as valid C++17.
 * This file is never linked into libably.a.
 */
#include <ably/ably.hpp>

/* Instantiate templates to catch any latent type errors. */
static_assert(sizeof(ably::Message) > 0,      "Message struct must have non-zero size");
static_assert(sizeof(ably::RestOptions) > 0,  "RestOptions must have non-zero size");
static_assert(sizeof(ably::RealtimeOptions) > 0, "RealtimeOptions must have non-zero size");
