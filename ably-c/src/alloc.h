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

#ifndef ABLY_ALLOC_H
#define ABLY_ALLOC_H

#include <stddef.h>
#include "ably/ably_allocator.h"

/*
 * Internal allocation helpers.
 *
 * All library code calls ably_mem_malloc / ably_mem_free / ably_mem_realloc
 * rather than calling malloc/free directly.  Each call site passes the
 * allocator pointer stored in the owning client struct.
 *
 * Allocation only occurs on the init path (client creation).  Hot paths
 * (publish, receive, dispatch) must not call any of these.
 */

static inline void *ably_mem_malloc(const ably_allocator_t *alloc, size_t size)
{
    return alloc->malloc_fn(size, alloc->user_data);
}

static inline void ably_mem_free(const ably_allocator_t *alloc, void *ptr)
{
    alloc->free_fn(ptr, alloc->user_data);
}

static inline void *ably_mem_realloc(const ably_allocator_t *alloc,
                                      void *ptr, size_t size)
{
    return alloc->realloc_fn(ptr, size, alloc->user_data);
}

/* Returns a fully-initialised allocator that wraps system malloc/free/realloc. */
ably_allocator_t ably_system_allocator(void);

#endif /* ABLY_ALLOC_H */
