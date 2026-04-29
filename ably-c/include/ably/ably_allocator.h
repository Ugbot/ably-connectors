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

#ifndef ABLY_ALLOCATOR_H
#define ABLY_ALLOCATOR_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Custom allocator interface.
 *
 * All allocation inside ably-c routes through these function pointers so
 * callers can substitute arena allocators, pool allocators, or RTOS heap
 * functions.  The library only allocates during client creation (init path);
 * no allocation occurs on any hot path (publish, receive, dispatch).
 *
 * Pass NULL to any _create() function to use the system malloc/free/realloc.
 */
typedef struct {
    void *(*malloc_fn) (size_t size,            void *user_data);
    void  (*free_fn)   (void *ptr,              void *user_data);
    void *(*realloc_fn)(void *ptr, size_t size, void *user_data);
    void  *user_data;
} ably_allocator_t;

#ifdef __cplusplus
}
#endif
#endif /* ABLY_ALLOCATOR_H */
