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

#include "alloc.h"
#include <stdlib.h>

static void *sys_malloc(size_t size, void *ud)
{
    (void)ud;
    return malloc(size);
}

static void sys_free(void *ptr, void *ud)
{
    (void)ud;
    free(ptr);
}

static void *sys_realloc(void *ptr, size_t size, void *ud)
{
    (void)ud;
    return realloc(ptr, size);
}

ably_allocator_t ably_system_allocator(void)
{
    ably_allocator_t a;
    a.malloc_fn  = sys_malloc;
    a.free_fn    = sys_free;
    a.realloc_fn = sys_realloc;
    a.user_data  = NULL;
    return a;
}
