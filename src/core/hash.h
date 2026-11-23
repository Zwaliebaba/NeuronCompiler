/* hash.h — the hash function behind every in-memory table (Design/02 §3).
 *
 * A hash value never reaches the output: no hash map is iterated for output
 * (R15), so the function can change without changing a single object byte.
 * It is not cryptographic; SHA-256 for CodeView checksums is separate. */
#ifndef NC_HASH_H
#define NC_HASH_H

#include "core.h"

/* The splitmix64 finalizer: every input bit affects every output bit, so a
 * table can take its slot index from the low bits. */
static inline u64 HashMix64(u64 _x)
{
  _x ^= _x >> 30;
  _x *= 0xBF58476D1CE4E5B9ull;
  _x ^= _x >> 27;
  _x *= 0x94D049BB133111EBull;
  _x ^= _x >> 31;
  return _x;
}

/* FNV-1a over the bytes, finished with HashMix64. Byte-at-a-time on purpose:
 * the result must not depend on the host's alignment or byte order. */
static inline u64 HashBytes(const void* _data, size_t _sizeBytes)
{
  const u8* p = _data;
  u64 h = 0xCBF29CE484222325ull;
  for (size_t i = 0; i < _sizeBytes; i++)
  {
    h ^= p[i];
    h *= 0x100000001B3ull;
  }
  return HashMix64(h);
}

#endif
