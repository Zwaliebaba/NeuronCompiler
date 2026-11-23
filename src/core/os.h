/* os.h — the only host-dependent interface (Design/13 §3). */
#ifndef NC_OS_H
#define NC_OS_H

#include "core.h"

/* Reads a whole file. Returns false if it cannot be read. */
bool OsReadFile(const char* _path, u8** _data, size_t* _size);

/* Writes a file atomically: data goes to a temporary file in the same
 * directory, which is then renamed over the destination, so a crash never
 * leaves a truncated object file behind. */
bool OsWriteFileAtomic(const char* _path, const void* _data, size_t _size);

#endif
