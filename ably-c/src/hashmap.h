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
 * Fixed-capacity open-addressing hashmap with string keys and void* values.
 *
 * TigerStyle: the caller supplies the slot array — no allocation occurs inside
 * this module.  All operations are O(1) amortised (linear probing with FNV-1a
 * hashing).  Deletions use tombstones so probe chains remain intact.
 *
 * Capacity must be a power of two.  Maximum load is capped at 75% to keep
 * probe chains short.
 *
 * Iteration is O(capacity) — walks the slot array and yields OCCUPIED entries.
 * Because the slot array is a plain C array, the call-site can also walk it
 * directly for typed access (e.g. for presence member iteration).
 */

#ifndef ABLY_HASHMAP_H
#define ABLY_HASHMAP_H

#include <stddef.h>
#include <stdint.h>

#ifndef ABLY_HASHMAP_KEY_MAX
#  define ABLY_HASHMAP_KEY_MAX 256
#endif

typedef enum {
    ABLY_SLOT_EMPTY    = 0,
    ABLY_SLOT_OCCUPIED = 1,
    ABLY_SLOT_DELETED  = 2,   /* tombstone — keeps probe chains intact */
} ably_hashmap_slot_state_t;

typedef struct {
    char                     key[ABLY_HASHMAP_KEY_MAX];
    void                    *value;
    ably_hashmap_slot_state_t state;
} ably_hashmap_slot_t;

typedef struct {
    ably_hashmap_slot_t *slots;
    size_t               capacity;   /* number of slots — must be power of two */
    size_t               count;      /* occupied entries (excluding tombstones) */
} ably_hashmap_t;

typedef struct {
    size_t index;   /* current position in slot array */
} ably_hashmap_iter_t;

/*
 * Initialise the map with a caller-supplied slot array.
 * capacity must be a power of two and >= 4.
 * The slots array is zeroed (all slots become EMPTY).
 */
void ably_hashmap_init(ably_hashmap_t *map, ably_hashmap_slot_t *slots,
                        size_t capacity);

/*
 * Insert or update key → value.
 * Returns  0 on success.
 * Returns -1 if the map is at the 75% load limit.
 */
int ably_hashmap_put(ably_hashmap_t *map, const char *key, void *value);

/*
 * Look up key.  Returns the stored value, or NULL if not found.
 */
void *ably_hashmap_get(const ably_hashmap_t *map, const char *key);

/*
 * Remove key.  Returns 1 if the key was found and removed, 0 otherwise.
 */
int ably_hashmap_remove(ably_hashmap_t *map, const char *key);

/*
 * Remove all entries (resets every slot to EMPTY).
 */
void ably_hashmap_clear(ably_hashmap_t *map);

/* Number of occupied entries. */
static inline size_t ably_hashmap_count(const ably_hashmap_t *map)
{
    return map->count;
}

/*
 * Forward iterator.
 *
 * Usage:
 *   ably_hashmap_iter_t it;
 *   ably_hashmap_iter_init(&it);
 *   const char *key; void *val;
 *   while (ably_hashmap_iter_next(map, &it, &key, &val)) { ... }
 *
 * key_out and value_out may be NULL if not needed.
 * Returns 1 for each occupied slot, 0 when exhausted.
 */
void ably_hashmap_iter_init(ably_hashmap_iter_t *iter);
int  ably_hashmap_iter_next(const ably_hashmap_t *map, ably_hashmap_iter_t *iter,
                             const char **key_out, void **value_out);

#endif /* ABLY_HASHMAP_H */
