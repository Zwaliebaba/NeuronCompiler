#ifndef NC_CRC32_H
#define NC_CRC32_H

#include "core.h"

/* Raw reflected CRC-32 update (no pre/post inversion). */
u32 nc_crc32_update(u32 crc, const void *data, size_t n);

/* JamCRC: CRC-32 with initial value ~0 and no final inversion. This is the
 * checksum MSVC and LLVM store in COFF COMDAT section auxiliary records. */
u32 nc_jamcrc(const void *data, size_t n);

#endif
