#ifndef NC_CRC32_H
#define NC_CRC32_H

#include "core.h"

/* Raw reflected CRC-32 update (no pre/post inversion). */
u32 Crc32Update(u32 _crc, const void* _data, size_t _n);

/* JamCRC: CRC-32 with initial value ~0 and no final inversion. This is the
 * checksum MSVC and LLVM store in COFF COMDAT section auxiliary records. */
u32 Crc32Jam(const void* _data, size_t _n);

#endif
