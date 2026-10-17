/* XTS extensions to libfat (third_party/libfat/XTS-PATCHES.md). Not part of upstream fat.h, whose API is unchanged. */
#ifndef _LIBFAT_XTS_H
#define _LIBFAT_XTS_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Sync every open file of the volume "name:" (directory entry + data), flush the cache and write FSInfo if it
 * changed. true = everything reached the disc interface without an error. */
bool fatFlush (const char* name);

/* Where a file's data lives on the disc, for raw-sector access (crash reports): first sector (absolute, as passed to
 * the DISC_INTERFACE), number of sectors covering its size, sector size. 0 = one contiguous cluster run covering the
 * whole size; -1 = no such file / not a libfat volume; -2 = empty, not contiguous or broken chain. Do not write raw
 * sectors of a file libfat has open or cached (the cache would put its own copy back). */
int fatFileExtent (const char* path, uint32_t* firstSector, uint32_t* numSectors, uint32_t* bytesPerSector);

#ifdef __cplusplus
}
#endif
#endif
