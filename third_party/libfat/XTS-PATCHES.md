# Vendored libfat (fat-xenon) with XTS patches

## What this is
- Source: https://github.com/Free60Project/fat-xenon at `0f2092a` (2021-06-09), the libfat that
  `/usr/local/xenon/usr/lib/libfat.a` was built from (same objects, same "fat Mount %s" string, gcc 16 `-Os`).
  BSD license, see `LICENSE` (the text from every source file header).
- `source/` patched sources; `include/fat.h` = the libxenon variant of the header, byte-identical to the installed
  `<libfat/fat.h>` (the public API - `fatInit`, `fatInitDefault`, `fatMountSimple`, `fatMount`, `fatUnmount`,
  `fatGetVolumeLabel` - is unchanged); `include/fat_xts.h` declares two new functions (below).
- `upstream/` the untouched copy, built for the host only (`make host-fatreplay` compares the two).
- `host/` what the HOST build needs instead of newlib/libxenon headers (`xetypes.h`, `diskio/disc_io.h`,
  `sys/iosupport.h`, `sys/reent.h`, and a forced include mapping newlib's `st_spare*` stat members). No libfat source
  is changed for the host: the int-sized file handle is handled by allocating FILE_STRUCTs below 2 GB
  (`src/plat/fatio_host.c`).
- `xts-libfat.patch` the complete diff `upstream/` -> here. Every changed place is marked `XTS-PATCH(libfat-<x>)`.

Build (Makefile): console objects with XTS's machine flags (`-O2 $(XMACHDEP) -DLIBXENON -DXENON`, warnings `-Wall`
minus two upstream-noise warnings) into `$(BUILD)/console/libfat.a`, linked instead of `-lfat`; host objects with the
sanitizers into `$(BUILD)/host/libfat.a` (and `libfat-upstream.a`).

## Patches
Patches a-c were asked for; d is "everything else unchanged"; e-g were found by the replay harness
(`make host-fatreplay`, `host/fatreplay.c`) and are needed for the guarantees below.

**a. cache.c: drop a cache page whose (re)load failed.** `_FAT_cache_getPage` evicts the least recently used page,
writes it back if dirty, then reads the new range into the SAME buffer. When that read failed, upstream returned NULL
but left the entry describing the OLD range: the next access to the old range was served from a buffer now holding an
unknown mix of the old page and the new range and, once modified, written back over the old range - a directory page
partly replaced by another cluster's data, the incident's pattern. Now the entry is invalidated (`sector = CACHE_FREE`,
`count = 0`, `dirty = false`). If the write-back of a dirty victim fails, the victim keeps its data and dirty flag and
its buffer is not reused (that part was already right; now commented).

**b. FAT mirroring.** Upstream wrote FAT entries to FAT #1 only, so FAT #2 went stale with every allocation (the
incident's "FAT1 != FAT2 in 3 entries"; chkdsk/fsck report it). Every FAT entry write now goes, through the cache, to
every copy (`fat.fatCopies`), unless BPB_ExtFlags bit 7 says only one FAT is active: then only that FAT (bits 0-3) is
read and written. Upstream had that bit test inverted.

**c. FSInfo written on sync.** Upstream wrote FSInfo (free cluster count, last allocated cluster) only in the partition
destructor (`fatUnmount`), which XTS never calls, so the count on the stick was always stale. Now
`_FAT_partition_flush` (cache flush, then FSInfo if either value changed since it was last read or written) is used by
fsync/close (`_FAT_syncToDisc`), unlink, rename and mkdir, and by the new `fatFlush`. FSInfo is still a one-sector
direct write; a cached copy of that sector (layouts whose first cache page holds FSInfo and FAT sectors) is updated too,
so a later write-back of the page cannot restore the old FSInfo (the same applies to the sector written when FSInfo is
created at mount).

**e. A failed FAT read is an error, not a free cluster.** `_FAT_fat_nextCluster` returned its initial value
`CLUSTER_FREE` when the cache read failed. One failed FAT read during an allocation therefore made the allocator take an
IN-USE cluster as free: in the replay a single read error (honest, reported by the driver) while `mkdir("xts")` scanned
the FAT gave the root directory's own cluster to the new directory and zeroed it - every file on the stick gone (runs
B2 #17/#19 with upstream, and C2 #17 with only patches a-c). Now `CLUSTER_ERROR`, and every caller treats it as an error:
`_FAT_fat_linkFreeCluster` fails instead of overwriting a link it could not read or scanning past it; the new cluster
is marked end-of-chain BEFORE the old end links to it (an interrupted allocation leaves at most a lost cluster);
failed FAT-entry writes and a failed clearing of a new directory cluster fail the allocation; `_FAT_fat_lastCluster`,
`_FAT_fat_trimChain`, the seek loop and directory iteration stop at an unreadable link instead of following
0xFFFFFFFF.

**f. Directory gap search.** `_FAT_directory_findEntryGap` restarted the gap at the end-of-directory marker even when
deleted entries right before it had already started one. `dataStart`/`dataEnd` then covered fewer entries than
`_FAT_directory_addEntry` wrote: the file's later syncs updated an LFN entry (attribute becomes 0x2F) instead of its short
entry, whose cluster and size stayed 0 (a lost cluster, a corrupt directory). XTS hit this on EVERY second run on a stick:
`xts/<tag>/current.txt` is deleted at the end of a run and created again by the next one (seen with upstream libfat: three
fault-free runs in a row leave 2 directory errors, 2 lost clusters and a stale FAT #2).

**g. FAT pages are written first.** Upstream flushed dirty pages in slot order, so an interrupted flush (crash, power
cut, or a write the guard refused) could leave a directory entry on the stick pointing to a cluster still free in the
FAT - the incident's "xts entry points to cluster 13, free in both FATs" - and the next allocation hands that cluster out
again (cross-link). `_FAT_cache_flush` and the eviction of a dirty non-FAT page now write dirty FAT pages first; an
interruption leaves lost clusters at worst. In the replay, "entry points to a free cluster" went from 1-2 per 40 runs
of a scenario to 0.

**d. Everything else is unchanged**, including upstream behaviours XTS works around instead: `fatMount` prints
"fat Mount <name>"; `statvfs` rescans the whole FAT (and rewrites FSInfo) if `f_flag` happens to read "SCAN" (XTS
zeroes the struct); FAT32 entries are written as full 32-bit values (the reserved top 4 bits are not preserved); data
and directory pages are not ordered among themselves (after an interruption a file's size can cover data that never
reached the stick - inside XTS's own files only).

## XTS extensions (`include/fat_xts.h`, appended to `source/libfat.c`)
- `bool fatFlush(const char *name)` - sync every open file of the volume, flush the cache, write FSInfo if changed.
  XTS calls it before the final halt.
- `int fatFileExtent(const char *path, uint32_t *firstSector, uint32_t *numSectors, uint32_t *bytesPerSector)` - where a
  file's data lies on the device (0 = one contiguous run covering its size), for raw-sector access by a crash reporter
  through `xts_disc_write_marked()` (src/plat/disc_guard.h).

## Evidence
`make host-fatreplay` (see host/fatreplay.c and CONTRIBUTING.md "Storage"): the smoke profile's dry run through the real
sinks/journal/libfat on pre-populated FAT32 images, upstream libfat vs this one, with and without the disc guard, under
injected USB faults; the numbers are in the final report / `$(BUILD)/fatreplay-smoke/summary.txt`.
