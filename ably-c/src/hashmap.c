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

#include "hashmap.h"

#include <assert.h>
#include <string.h>

/* FNV-1a 32-bit */
static uint32_t fnv1a(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) {
        h ^= (uint8_t)*s++;
        h *= 16777619u;
    }
    return h;
}

void ably_hashmap_init(ably_hashmap_t *map, ably_hashmap_slot_t *slots,
                        size_t capacity)
{
    assert(map != NULL);
    assert(slots != NULL);
    assert(capacity >= 4);
    assert((capacity & (capacity - 1)) == 0); /* must be power of two */

    map->slots    = slots;
    map->capacity = capacity;
    map->count    = 0;
    memset(slots, 0, capacity * sizeof(ably_hashmap_slot_t));
}

int ably_hashmap_put(ably_hashmap_t *map, const char *key, void *value)
{
    assert(map != NULL);
    assert(key != NULL);

    /* Enforce 75% load cap */
    if (map->count * 4 >= map->capacity * 3)
        return -1;

    uint32_t h    = fnv1a(key);
    size_t   mask = map->capacity - 1;
    size_t   i    = h & mask;

    /* Track first tombstone encountered — reuse it if key not found */
    size_t tombstone = (size_t)-1;

    for (size_t probe = 0; probe < map->capacity; probe++) {
        ably_hashmap_slot_t *slot = &map->slots[i];

        if (slot->state == ABLY_SLOT_OCCUPIED) {
            if (strncmp(slot->key, key, ABLY_HASHMAP_KEY_MAX - 1) == 0) {
                slot->value = value;
                return 0;
            }
        } else if (slot->state == ABLY_SLOT_DELETED) {
            if (tombstone == (size_t)-1)
                tombstone = i;
        } else {
            /* EMPTY — key not in map; insert here or at tombstone */
            size_t insert_at = (tombstone != (size_t)-1) ? tombstone : i;
            ably_hashmap_slot_t *dst = &map->slots[insert_at];
            strncpy(dst->key, key, ABLY_HASHMAP_KEY_MAX - 1);
            dst->key[ABLY_HASHMAP_KEY_MAX - 1] = '\0';
            dst->value = value;
            dst->state = ABLY_SLOT_OCCUPIED;
            map->count++;
            return 0;
        }

        i = (i + 1) & mask;
    }

    /* All slots OCCUPIED or DELETED but load check passed — shouldn't happen
     * unless tombstone count is high.  Fall back to tombstone slot. */
    if (tombstone != (size_t)-1) {
        ably_hashmap_slot_t *dst = &map->slots[tombstone];
        strncpy(dst->key, key, ABLY_HASHMAP_KEY_MAX - 1);
        dst->key[ABLY_HASHMAP_KEY_MAX - 1] = '\0';
        dst->value = value;
        dst->state = ABLY_SLOT_OCCUPIED;
        map->count++;
        return 0;
    }

    return -1;
}

void *ably_hashmap_get(const ably_hashmap_t *map, const char *key)
{
    assert(map != NULL);
    assert(key != NULL);

    uint32_t h    = fnv1a(key);
    size_t   mask = map->capacity - 1;
    size_t   i    = h & mask;

    for (size_t probe = 0; probe < map->capacity; probe++) {
        const ably_hashmap_slot_t *slot = &map->slots[i];

        if (slot->state == ABLY_SLOT_OCCUPIED) {
            if (strncmp(slot->key, key, ABLY_HASHMAP_KEY_MAX - 1) == 0)
                return slot->value;
        } else if (slot->state == ABLY_SLOT_EMPTY) {
            return NULL;
        }
        /* DELETED — skip (tombstone keeps probe chain intact) */

        i = (i + 1) & mask;
    }

    return NULL;
}

int ably_hashmap_remove(ably_hashmap_t *map, const char *key)
{
    assert(map != NULL);
    assert(key != NULL);

    uint32_t h    = fnv1a(key);
    size_t   mask = map->capacity - 1;
    size_t   i    = h & mask;

    for (size_t probe = 0; probe < map->capacity; probe++) {
        ably_hashmap_slot_t *slot = &map->slots[i];

        if (slot->state == ABLY_SLOT_OCCUPIED) {
            if (strncmp(slot->key, key, ABLY_HASHMAP_KEY_MAX - 1) == 0) {
                slot->state = ABLY_SLOT_DELETED;
                map->count--;
                return 1;
            }
        } else if (slot->state == ABLY_SLOT_EMPTY) {
            return 0;
        }

        i = (i + 1) & mask;
    }

    return 0;
}

void ably_hashmap_clear(ably_hashmap_t *map)
{
    assert(map != NULL);
    memset(map->slots, 0, map->capacity * sizeof(ably_hashmap_slot_t));
    map->count = 0;
}

void ably_hashmap_iter_init(ably_hashmap_iter_t *iter)
{
    assert(iter != NULL);
    iter->index = 0;
}

int ably_hashmap_iter_next(const ably_hashmap_t *map, ably_hashmap_iter_t *iter,
                            const char **key_out, void **value_out)
{
    assert(map != NULL);
    assert(iter != NULL);

    while (iter->index < map->capacity) {
        const ably_hashmap_slot_t *slot = &map->slots[iter->index];
        iter->index++;

        if (slot->state == ABLY_SLOT_OCCUPIED) {
            if (key_out)   *key_out   = slot->key;
            if (value_out) *value_out = slot->value;
            return 1;
        }
    }

    return 0;
}
