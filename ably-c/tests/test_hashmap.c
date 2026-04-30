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

#include "../src/hashmap.h"

#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#define CAPACITY 16   /* power of two; 75% load cap = 12 entries */

static void check(int cond, const char *msg)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", msg);
        exit(1);
    }
}

/* Simple LCG for deterministic-but-varied keys */
static uint32_t lcg_state;
static void lcg_seed(uint32_t s) { lcg_state = s; }
static uint32_t lcg_next(void) {
    lcg_state = lcg_state * 1664525u + 1013904223u;
    return lcg_state;
}

/* Generate a key of the form "key_XXXXXXXX" */
static void make_key(char *buf, size_t buf_len, uint32_t n)
{
    snprintf(buf, buf_len, "key_%08x", n);
}

/* ------------------------------------------------------------------ */

static void test_basic_insert_lookup(void)
{
    ably_hashmap_slot_t slots[CAPACITY];
    ably_hashmap_t map;
    ably_hashmap_init(&map, slots, CAPACITY);

    lcg_seed(0xdeadbeef);

    int values[8];
    char keys[8][32];

    for (int i = 0; i < 8; i++) {
        make_key(keys[i], sizeof(keys[i]), lcg_next());
        values[i] = i * 37 + 1;
        int rc = ably_hashmap_put(&map, keys[i], &values[i]);
        check(rc == 0, "insert should succeed");
    }
    check(ably_hashmap_count(&map) == 8, "count should be 8");

    for (int i = 0; i < 8; i++) {
        void *v = ably_hashmap_get(&map, keys[i]);
        check(v != NULL, "lookup should find inserted key");
        check(*(int *)v == values[i], "lookup should return correct value");
    }

    /* miss */
    check(ably_hashmap_get(&map, "no_such_key") == NULL, "miss should return NULL");

    printf("PASS: basic_insert_lookup\n");
}

static void test_update_existing_key(void)
{
    ably_hashmap_slot_t slots[CAPACITY];
    ably_hashmap_t map;
    ably_hashmap_init(&map, slots, CAPACITY);

    int v1 = 100, v2 = 200;

    lcg_seed(0xcafe);
    char key[32];
    make_key(key, sizeof(key), lcg_next());

    ably_hashmap_put(&map, key, &v1);
    check(*(int *)ably_hashmap_get(&map, key) == v1, "initial value");

    ably_hashmap_put(&map, key, &v2);
    check(ably_hashmap_count(&map) == 1, "update must not increment count");
    check(*(int *)ably_hashmap_get(&map, key) == v2, "updated value");

    printf("PASS: update_existing_key\n");
}

static void test_remove_and_reinsert(void)
{
    ably_hashmap_slot_t slots[CAPACITY];
    ably_hashmap_t map;
    ably_hashmap_init(&map, slots, CAPACITY);

    lcg_seed(0xbabe);
    int vals[4];
    char keys[4][32];
    for (int i = 0; i < 4; i++) {
        make_key(keys[i], sizeof(keys[i]), lcg_next());
        vals[i] = i;
        ably_hashmap_put(&map, keys[i], &vals[i]);
    }

    int rc = ably_hashmap_remove(&map, keys[1]);
    check(rc == 1, "remove should return 1 for found key");
    check(ably_hashmap_count(&map) == 3, "count after remove");
    check(ably_hashmap_get(&map, keys[1]) == NULL, "removed key must not be found");

    /* re-insert the same key */
    int new_val = 999;
    ably_hashmap_put(&map, keys[1], &new_val);
    check(ably_hashmap_count(&map) == 4, "count after reinsert");
    check(*(int *)ably_hashmap_get(&map, keys[1]) == 999, "reinserted value");

    /* remove miss */
    rc = ably_hashmap_remove(&map, "not_here");
    check(rc == 0, "remove miss should return 0");

    printf("PASS: remove_and_reinsert\n");
}

static void test_iteration_covers_all(void)
{
    ably_hashmap_slot_t slots[CAPACITY];
    ably_hashmap_t map;
    ably_hashmap_init(&map, slots, CAPACITY);

    lcg_seed(0x1234);
    int vals[6];
    char keys[6][32];
    for (int i = 0; i < 6; i++) {
        make_key(keys[i], sizeof(keys[i]), lcg_next());
        vals[i] = i;
        ably_hashmap_put(&map, keys[i], &vals[i]);
    }
    /* remove one to leave a tombstone */
    ably_hashmap_remove(&map, keys[2]);

    ably_hashmap_iter_t it;
    ably_hashmap_iter_init(&it);
    int seen[6] = {0};
    int count = 0;
    const char *k; void *v;
    while (ably_hashmap_iter_next(&map, &it, &k, &v)) {
        count++;
        for (int i = 0; i < 6; i++) {
            if (strcmp(k, keys[i]) == 0) {
                seen[i] = 1;
                check(*(int *)v == vals[i], "iterator value matches");
            }
        }
    }
    check(count == 5, "iterator should yield 5 occupied entries");
    for (int i = 0; i < 6; i++) {
        if (i == 2) check(seen[2] == 0, "removed key must not appear in iteration");
        else        check(seen[i] == 1, "all live keys must appear in iteration");
    }

    printf("PASS: iteration_covers_all\n");
}

static void test_load_cap(void)
{
    /* capacity=16, 75% cap = 12 entries allowed */
    ably_hashmap_slot_t slots[CAPACITY];
    ably_hashmap_t map;
    ably_hashmap_init(&map, slots, CAPACITY);

    lcg_seed(0xfeed);
    int vals[13];
    char keys[13][32];
    int i;
    for (i = 0; i < 12; i++) {
        make_key(keys[i], sizeof(keys[i]), lcg_next());
        vals[i] = i;
        int rc = ably_hashmap_put(&map, keys[i], &vals[i]);
        check(rc == 0, "insert within cap must succeed");
    }
    check(ably_hashmap_count(&map) == 12, "count at capacity");

    /* 13th insert must fail */
    make_key(keys[12], sizeof(keys[12]), lcg_next());
    vals[12] = 99;
    int rc = ably_hashmap_put(&map, keys[12], &vals[12]);
    check(rc == -1, "insert beyond 75% cap must return -1");
    check(ably_hashmap_count(&map) == 12, "count unchanged after failed insert");

    printf("PASS: load_cap\n");
}

static void test_tombstone_probe_chain(void)
{
    /*
     * Force a collision chain: insert three keys that hash to the same slot.
     * Remove the middle one (leaves tombstone).
     * Ensure lookup still finds the third entry via the tombstone probe.
     *
     * We find colliding keys by brute force over the known FNV-1a hash.
     */
    ably_hashmap_slot_t slots[CAPACITY];
    ably_hashmap_t map;
    ably_hashmap_init(&map, slots, CAPACITY);

    /* Find three keys that land in slot 0 (h & (CAPACITY-1) == 0). */
    char chain[3][32];
    int  found = 0;
    for (uint32_t n = 0; n < 0xffffffff && found < 3; n++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "c%u", n);
        /* FNV-1a inline */
        uint32_t h = 2166136261u;
        for (const char *p = buf; *p; p++) { h ^= (uint8_t)*p; h *= 16777619u; }
        if ((h & (CAPACITY - 1)) == 0) {
            strncpy(chain[found], buf, sizeof(chain[found]) - 1);
            found++;
        }
    }
    check(found == 3, "must find 3 colliding keys");

    int v0 = 10, v1 = 20, v2 = 30;
    ably_hashmap_put(&map, chain[0], &v0);
    ably_hashmap_put(&map, chain[1], &v1);
    ably_hashmap_put(&map, chain[2], &v2);

    /* Remove the middle key → tombstone */
    ably_hashmap_remove(&map, chain[1]);

    /* chain[2] must still be reachable through the tombstone */
    void *found_val = ably_hashmap_get(&map, chain[2]);
    check(found_val != NULL, "key after tombstone must be found");
    check(*(int *)found_val == 30, "key after tombstone must have correct value");

    /* chain[1] must be gone */
    check(ably_hashmap_get(&map, chain[1]) == NULL, "removed key must be absent");

    printf("PASS: tombstone_probe_chain\n");
}

static void test_clear(void)
{
    ably_hashmap_slot_t slots[CAPACITY];
    ably_hashmap_t map;
    ably_hashmap_init(&map, slots, CAPACITY);

    lcg_seed(0xabcd);
    int v = 42;
    char key[32];
    make_key(key, sizeof(key), lcg_next());
    ably_hashmap_put(&map, key, &v);
    check(ably_hashmap_count(&map) == 1, "count before clear");

    ably_hashmap_clear(&map);
    check(ably_hashmap_count(&map) == 0, "count after clear");
    check(ably_hashmap_get(&map, key) == NULL, "cleared key must not be found");

    printf("PASS: clear\n");
}

int main(void)
{
    test_basic_insert_lookup();
    test_update_existing_key();
    test_remove_and_reinsert();
    test_iteration_covers_all();
    test_load_cap();
    test_tombstone_probe_chain();
    test_clear();

    printf("All hashmap tests passed.\n");
    return 0;
}
