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
 * Verify that the custom allocator hooks route through the provided functions.
 */

#include "../src/alloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

static int g_alloc_count   = 0;
static int g_free_count    = 0;
static int g_realloc_count = 0;

static void *counting_malloc(size_t size, void *ud)
{
    (void)ud;
    g_alloc_count++;
    return malloc(size);
}

static void counting_free(void *ptr, void *ud)
{
    (void)ud;
    g_free_count++;
    free(ptr);
}

static void *counting_realloc(void *ptr, size_t size, void *ud)
{
    (void)ud;
    g_realloc_count++;
    return realloc(ptr, size);
}

int main(void)
{
    int failures = 0;

    /* --- System allocator --- */
    {
        ably_allocator_t sys = ably_system_allocator();

        void *p = ably_mem_malloc(&sys, 64);
        if (!p) { fprintf(stderr, "FAIL: system malloc returned NULL\n"); return 1; }
        memset(p, 0xAB, 64);

        p = ably_mem_realloc(&sys, p, 128);
        if (!p) { fprintf(stderr, "FAIL: system realloc returned NULL\n"); return 1; }

        ably_mem_free(&sys, p);
        printf("PASS: system allocator round-trip\n");
    }

    /* --- Custom counting allocator --- */
    {
        ably_allocator_t custom;
        custom.malloc_fn  = counting_malloc;
        custom.free_fn    = counting_free;
        custom.realloc_fn = counting_realloc;
        custom.user_data  = NULL;

        void *p = ably_mem_malloc(&custom, 32);
        assert(g_alloc_count == 1);
        p = ably_mem_realloc(&custom, p, 64);
        assert(g_realloc_count == 1);
        ably_mem_free(&custom, p);
        assert(g_free_count == 1);

        printf("PASS: custom allocator counted %d malloc, %d realloc, %d free\n",
               g_alloc_count, g_realloc_count, g_free_count);
    }

    if (failures == 0) {
        printf("All allocator tests passed.\n");
        return 0;
    }
    fprintf(stderr, "%d test(s) failed.\n", failures);
    return 1;
}
