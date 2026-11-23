/* os.h — the only host-dependent interface (Design/13 §3). */
#ifndef NC_OS_H
#define NC_OS_H

#include "core.h"

/* Reads a whole file. Returns false if it cannot be read. */
bool os_read_file(const char *path, u8 **data, size_t *size);

/* Writes a file atomically: data goes to a temporary file in the same
 * directory, which is then renamed over the destination, so a crash never
 * leaves a truncated object file behind. */
bool os_write_file_atomic(const char *path, const void *data, size_t size);

#endif
