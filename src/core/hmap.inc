/* hmap.inc — open-addressing hash map with Robin Hood probing, generated per
 * key/value pair (Design/02 §3). Include it after defining:
 *
 *   #define HMAP_NAME   SymKeyMap              the map type; functions are SymKeyMapGet, ...
 *   #define HMAP_K      SymKey                 key type, passed by value
 *   #define HMAP_V      u32                    value type
 *   #define HMAP_HASH(k) HashBytes(...)        u64 hash of a key
 *   #define HMAP_EQ(a, b) (...)                key equality
 *   #include "hmap.inc"
 *
 * The parameters are undefined again at the end, so a file can instantiate
 * several maps. A zero-initialized map is empty and valid; HMAP_NAME##Free
 * releases it. The slot array is heap memory from NcXrealloc (R13: it grows).
 *
 * There is deliberately no iteration function. Iteration order depends on
 * hash values and on insertion history, and nothing emitted may depend on
 * either (R15). Code that needs to visit every entry keeps them in a vector
 * as well, in insertion order, and uses the map only to find them. */

#ifndef HMAP_NAME
#   error "hmap.inc: define HMAP_NAME, HMAP_K, HMAP_V, HMAP_HASH and HMAP_EQ first"
#endif

#ifndef NC_HMAP_HELPERS
#   define NC_HMAP_HELPERS
#   include "core.h"
#   define HMAP_CAT2(a, b) a##b
#   define HMAP_CAT(a, b) HMAP_CAT2(a, b)
#endif

#define HMAP_FN(x) HMAP_CAT(HMAP_NAME, x)

typedef struct HMAP_FN(Slot)
{
  HMAP_K key;
  HMAP_V value;
  u32 hash;
  /* Zero for an empty slot; otherwise the probe distance from the key's home
   * slot, plus one, so a key in its home slot has 1. */
  u32 distPlusOne;
} HMAP_FN(Slot);

typedef struct HMAP_NAME
{
  HMAP_FN(Slot) * slots;
  u32 capacity; /* zero or a power of two */
  u32 count;
} HMAP_NAME;

static inline u32 HMAP_FN(HashOf)(HMAP_K _key)
{
  u64 h = HMAP_HASH(_key);
  return (u32)(h ^ (h >> 32));
}

static inline void HMAP_FN(Free)(HMAP_NAME* _map)
{
  NcXfree(_map->slots);
  _map->slots = nullptr;
  _map->capacity = _map->count = 0;
}

/* Returns the slot index holding `_key`, or UINT32_MAX. */
static inline u32 HMAP_FN(Find)(const HMAP_NAME* _map, HMAP_K _key, u32 _hash)
{
  if (_map->capacity == 0)
    return UINT32_MAX;
  u32 mask = _map->capacity - 1;
  u32 i = _hash & mask;
  for (u32 dist = 1;; dist++, i = (i + 1) & mask)
  {
    const HMAP_FN(Slot)* s = &_map->slots[i];
    /* Robin Hood invariant: a resident closer to its home than we are to
     * ours means our key would have displaced it, so it is not here. An
     * empty slot (0) ends the search the same way. */
    if (s->distPlusOne < dist)
      return UINT32_MAX;
    if (s->hash == _hash && HMAP_EQ(s->key, _key))
      return i;
  }
}

/* Places a slot that is known not to be present; returns where it landed.
 * Displaced residents move on, never the new slot, so the index is stable
 * until the next insertion or removal. */
static inline u32 HMAP_FN(Place)(HMAP_NAME* _map, HMAP_FN(Slot) _slot)
{
  u32 mask = _map->capacity - 1;
  u32 i = _slot.hash & mask;
  u32 landed = UINT32_MAX;
  _slot.distPlusOne = 1;
  for (;; i = (i + 1) & mask, _slot.distPlusOne++)
  {
    HMAP_FN(Slot)* s = &_map->slots[i];
    if (s->distPlusOne == 0)
    {
      *s = _slot;
      return landed == UINT32_MAX ? i : landed;
    }
    if (s->distPlusOne < _slot.distPlusOne)
    {
      HMAP_FN(Slot) displaced = *s;
      *s = _slot;
      _slot = displaced;
      if (landed == UINT32_MAX)
        landed = i;
    }
  }
}

static inline void HMAP_FN(Grow)(HMAP_NAME* _map)
{
  u32 oldCapacity = _map->capacity;
  HMAP_FN(Slot)* old = _map->slots;
  if (oldCapacity > UINT32_MAX / 2)
    NcFatal("hash map too large");
  _map->capacity = oldCapacity ? oldCapacity * 2 : 16;
  _map->slots = NcXrealloc(nullptr, (size_t)_map->capacity * sizeof *_map->slots);
  memset(_map->slots, 0, (size_t)_map->capacity * sizeof *_map->slots);
  for (u32 i = 0; i < oldCapacity; i++)
    if (old[i].distPlusOne)
      HMAP_FN(Place)(_map, old[i]);
  NcXfree(old);
}

/* Returns a pointer to the value stored for `_key`, or nullptr. The pointer
 * is valid until the next Put or Remove. */
static inline HMAP_V* HMAP_FN(Get)(const HMAP_NAME* _map, HMAP_K _key)
{
  u32 i = HMAP_FN(Find)(_map, _key, HMAP_FN(HashOf)(_key));
  return i == UINT32_MAX ? nullptr : &_map->slots[i].value;
}

/* Inserts `_key`, or overwrites its value if present; returns a pointer to
 * the stored value, valid until the next Put or Remove. */
static inline HMAP_V* HMAP_FN(Put)(HMAP_NAME* _map, HMAP_K _key, HMAP_V _value)
{
  u32 hash = HMAP_FN(HashOf)(_key);
  u32 i = HMAP_FN(Find)(_map, _key, hash);
  if (i != UINT32_MAX)
  {
    _map->slots[i].value = _value;
    return &_map->slots[i].value;
  }
  /* Maximum load 7/8: Robin Hood keeps probe lengths short up to there. */
  if ((u64)(_map->count + 1) * 8 > (u64)_map->capacity * 7)
    HMAP_FN(Grow)(_map);
  HMAP_FN(Slot) slot = {.key = _key, .value = _value, .hash = hash};
  i = HMAP_FN(Place)(_map, slot);
  _map->count++;
  return &_map->slots[i].value;
}

/* Removes `_key`; returns whether it was present. Backward-shift deletion,
 * so no tombstones accumulate. */
static inline bool HMAP_FN(Remove)(HMAP_NAME* _map, HMAP_K _key)
{
  u32 i = HMAP_FN(Find)(_map, _key, HMAP_FN(HashOf)(_key));
  if (i == UINT32_MAX)
    return false;
  u32 mask = _map->capacity - 1;
  for (u32 next = (i + 1) & mask; _map->slots[next].distPlusOne > 1; i = next, next = (next + 1) & mask)
  {
    _map->slots[i] = _map->slots[next];
    _map->slots[i].distPlusOne--;
  }
  _map->slots[i].distPlusOne = 0;
  _map->count--;
  return true;
}

#undef HMAP_FN
#undef HMAP_NAME
#undef HMAP_K
#undef HMAP_V
#undef HMAP_HASH
#undef HMAP_EQ
