/*
 cache.c
 The cache is not visible to the user. It should be flushed
 when any file is closed or changes are made to the filesystem.

 This cache implements a least-used-page replacement policy. This will
 distribute sectors evenly over the pages, so if less than the maximum
 pages are used at once, they should all eventually remain in the cache.
 This also has the benefit of throwing out old sectors, so as not to keep
 too many stale pages around.

 Copyright (c) 2006 Michael "Chishm" Chisholm

 Redistribution and use in source and binary forms, with or without modification,
 are permitted provided that the following conditions are met:

  1. Redistributions of source code must retain the above copyright notice,
     this list of conditions and the following disclaimer.
  2. Redistributions in binary form must reproduce the above copyright notice,
     this list of conditions and the following disclaimer in the documentation and/or
     other materials provided with the distribution.
  3. The name of the author may not be used to endorse or promote products derived
     from this software without specific prior written permission.

 THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
 AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE
 LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <string.h>
#include <limits.h>

#include "common.h"
#include "cache.h"
#include "disc.h"

#include "mem_allocate.h"
#include "bit_ops.h"
#include "file_allocation_table.h"

#define CACHE_FREE UINT_MAX

CACHE* _FAT_cache_constructor (unsigned int numberOfPages, unsigned int sectorsPerPage, const DISC_INTERFACE* discInterface, sec_t endOfPartition, unsigned int bytesPerSector) {
	CACHE* cache;
	unsigned int i;
	CACHE_ENTRY* cacheEntries;

	if (numberOfPages < 2) {
		numberOfPages = 2;
	}

	if (sectorsPerPage < 8) {
		sectorsPerPage = 8;
	}

	cache = (CACHE*) _FAT_mem_allocate (sizeof(CACHE));
	if (cache == NULL) {
		return NULL;
	}

	cache->disc = discInterface;
	cache->fatFirst = 0;
	cache->fatEnd = 0;
	cache->endOfPartition = endOfPartition;
	cache->numberOfPages = numberOfPages;
	cache->sectorsPerPage = sectorsPerPage;
	cache->bytesPerSector = bytesPerSector;


	cacheEntries = (CACHE_ENTRY*) _FAT_mem_allocate ( sizeof(CACHE_ENTRY) * numberOfPages);
	if (cacheEntries == NULL) {
		_FAT_mem_free (cache);
		return NULL;
	}

	for (i = 0; i < numberOfPages; i++) {
		cacheEntries[i].sector = CACHE_FREE;
		cacheEntries[i].count = 0;
		cacheEntries[i].last_access = 0;
		cacheEntries[i].dirty = false;
		cacheEntries[i].cache = (uint8_t*) _FAT_mem_align ( sectorsPerPage * bytesPerSector );
	}

	cache->cacheEntries = cacheEntries;

	return cache;
}

void _FAT_cache_destructor (CACHE* cache) {
	unsigned int i;
	// Clear out cache before destroying it
	_FAT_cache_flush(cache);

	// Free memory in reverse allocation order
	for (i = 0; i < cache->numberOfPages; i++) {
		_FAT_mem_free (cache->cacheEntries[i].cache);
	}
	_FAT_mem_free (cache->cacheEntries);
	_FAT_mem_free (cache);
}


static u32 accessCounter = 0;

static u32 accessTime(){
	accessCounter++;
	return accessCounter;
}


/*
XTS-PATCH(libfat-g): dirty FAT pages go to the disc before any other dirty page (directory entries, file data), in
_FAT_cache_flush and before a dirty non-FAT page is evicted. Upstream wrote pages in slot order, so an interrupted flush
(crash, power cut, or a write refused after a storage error) could leave a directory entry on the disc that points to a
cluster still free in the FAT - the next allocation hands that cluster out again (cross-link). With FAT pages first, an
interruption leaves at most lost clusters, which checkers free without touching any file.
*/
static bool _FAT_cache_overlapsFat(CACHE *cache, CACHE_ENTRY *e)
{
	return e->sector != CACHE_FREE && e->count && e->sector < cache->fatEnd && e->sector + e->count > cache->fatFirst;
}

static bool _FAT_cache_flushFat(CACHE *cache)
{
	unsigned int i;
	for (i = 0; i < cache->numberOfPages; i++) {
		CACHE_ENTRY *e = &cache->cacheEntries[i];
		if (e->dirty && _FAT_cache_overlapsFat(cache, e)) {
			if (!_FAT_disc_writeSectors(cache->disc, e->sector, e->count, e->cache))
				return false;
			e->dirty = false;
		}
	}
	return true;
}

static CACHE_ENTRY* _FAT_cache_getPage(CACHE *cache,sec_t sector)
{
	unsigned int i;
	CACHE_ENTRY* cacheEntries = cache->cacheEntries;
	unsigned int numberOfPages = cache->numberOfPages;
	unsigned int sectorsPerPage = cache->sectorsPerPage;

	bool foundFree = false;
	unsigned int oldUsed = 0;
	unsigned int oldAccess = UINT_MAX;

	for(i=0;i<numberOfPages;i++) {
		if(sector>=cacheEntries[i].sector && sector<(cacheEntries[i].sector + cacheEntries[i].count)) {
			cacheEntries[i].last_access = accessTime();
			return &(cacheEntries[i]);
		}

		if(foundFree==false && (cacheEntries[i].sector==CACHE_FREE || cacheEntries[i].last_access<oldAccess)) {
			if(cacheEntries[i].sector==CACHE_FREE) foundFree = true;
			oldUsed = i;
			oldAccess = cacheEntries[i].last_access;
		}
	}

	if(foundFree==false && cacheEntries[oldUsed].dirty==true) {
		/* XTS-PATCH(libfat-a): if the write-back of the victim fails, the victim keeps its sector, its data and its
		 * dirty flag and its buffer is NOT reused (unchanged from upstream; the call fails). */
		if(!_FAT_cache_overlapsFat(cache,&cacheEntries[oldUsed]) && !_FAT_cache_flushFat(cache)) return NULL;   // XTS-PATCH(libfat-g)
		if(!_FAT_disc_writeSectors(cache->disc,cacheEntries[oldUsed].sector,cacheEntries[oldUsed].count,cacheEntries[oldUsed].cache)) return NULL;
		cacheEntries[oldUsed].dirty = false;
	}

	sector = (sector/sectorsPerPage)*sectorsPerPage; // align base sector to page size
	sec_t next_page = sector + sectorsPerPage;
	if(next_page > cache->endOfPartition)	next_page = cache->endOfPartition;

	if(!_FAT_disc_readSectors(cache->disc,sector,next_page-sector,cacheEntries[oldUsed].cache)) {
		/* XTS-PATCH(libfat-a): the buffer now holds an unknown mix of the evicted page and the new range. Upstream
		 * returned NULL but left the entry describing the OLD range, so the next access to the old range was served
		 * from the mixed buffer and, once modified, written back over it (a directory page partly replaced by another
		 * cluster's data). Drop the entry. */
		cacheEntries[oldUsed].sector = CACHE_FREE;
		cacheEntries[oldUsed].count = 0;
		cacheEntries[oldUsed].last_access = 0;
		cacheEntries[oldUsed].dirty = false;
		return NULL;
	}

	cacheEntries[oldUsed].sector = sector;
	cacheEntries[oldUsed].count = next_page-sector;
	cacheEntries[oldUsed].last_access = accessTime();

	return &(cacheEntries[oldUsed]);
}

bool _FAT_cache_readSectors(CACHE *cache,sec_t sector,sec_t numSectors,void *buffer)
{
	sec_t sec;
	sec_t secs_to_read;
	CACHE_ENTRY *entry;
	uint8_t *dest = (uint8_t *)buffer;

	while(numSectors>0) {
		entry = _FAT_cache_getPage(cache,sector);
		if(entry==NULL) return false;

		sec = sector - entry->sector;
		secs_to_read = entry->count - sec;
		if(secs_to_read>numSectors) secs_to_read = numSectors;

		memcpy(dest,entry->cache + (sec*cache->bytesPerSector),(secs_to_read*cache->bytesPerSector));

		dest += (secs_to_read*cache->bytesPerSector);
		sector += secs_to_read;
		numSectors -= secs_to_read;
	}

	return true;
}

/*
Reads some data from a cache page, determined by the sector number
*/
bool _FAT_cache_readPartialSector (CACHE* cache, void* buffer, sec_t sector, unsigned int offset, size_t size)
{
	sec_t sec;
	CACHE_ENTRY *entry;

	if (offset + size > cache->bytesPerSector) return false;

	entry = _FAT_cache_getPage(cache,sector);
	if(entry==NULL) return false;

	sec = sector - entry->sector;
	memcpy(buffer,entry->cache + ((sec*cache->bytesPerSector) + offset),size);

	return true;
}

bool _FAT_cache_readLittleEndianValue (CACHE* cache, uint32_t *value, sec_t sector, unsigned int offset, int num_bytes) {
  uint8_t buf[4];
  if (!_FAT_cache_readPartialSector(cache, buf, sector, offset, num_bytes)) return false;

  switch(num_bytes) {
  case 1: *value = buf[0]; break;
  case 2: *value = u8array_to_u16(buf,0); break;
  case 4: *value = u8array_to_u32(buf,0); break;
  default: return false;
  }
  return true;
}

/*
Writes some data to a cache page, making sure it is loaded into memory first.
*/
bool _FAT_cache_writePartialSector (CACHE* cache, const void* buffer, sec_t sector, unsigned int offset, size_t size)
{
	sec_t sec;
	CACHE_ENTRY *entry;

	if (offset + size > cache->bytesPerSector) return false;

	entry = _FAT_cache_getPage(cache,sector);
	if(entry==NULL) return false;

	sec = sector - entry->sector;
	memcpy(entry->cache + ((sec*cache->bytesPerSector) + offset),buffer,size);

	entry->dirty = true;
	return true;
}

bool _FAT_cache_writeLittleEndianValue (CACHE* cache, const uint32_t value, sec_t sector, unsigned int offset, int size) {
  uint8_t buf[4] = {0, 0, 0, 0};

  switch(size) {
  case 1: buf[0] = value; break;
  case 2: u16_to_u8array(buf, 0, value); break;
  case 4: u32_to_u8array(buf, 0, value); break;
  default: return false;
  }

  return _FAT_cache_writePartialSector(cache, buf, sector, offset, size);
}

/*
Writes some data to a cache page, zeroing out the page first
*/
bool _FAT_cache_eraseWritePartialSector (CACHE* cache, const void* buffer, sec_t sector, unsigned int offset, size_t size)
{
	sec_t sec;
	CACHE_ENTRY *entry;

	if (offset + size > cache->bytesPerSector) return false;

	entry = _FAT_cache_getPage(cache,sector);
	if(entry==NULL) return false;

	sec = sector - entry->sector;
	memset(entry->cache + (sec*cache->bytesPerSector),0,cache->bytesPerSector);
	memcpy(entry->cache + ((sec*cache->bytesPerSector) + offset),buffer,size);

	entry->dirty = true;
	return true;
}


bool _FAT_cache_writeSectors (CACHE* cache, sec_t sector, sec_t numSectors, const void* buffer)
{
	sec_t sec;
	sec_t secs_to_write;
	CACHE_ENTRY* entry;
	const uint8_t *src = (const uint8_t *)buffer;

	while(numSectors>0)
	{
		entry = _FAT_cache_getPage(cache,sector);
		if(entry==NULL) return false;

		sec = sector - entry->sector;
		secs_to_write = entry->count - sec;
		if(secs_to_write>numSectors) secs_to_write = numSectors;

		memcpy(entry->cache + (sec*cache->bytesPerSector),src,(secs_to_write*cache->bytesPerSector));

		src += (secs_to_write*cache->bytesPerSector);
		sector += secs_to_write;
		numSectors -= secs_to_write;

		entry->dirty = true;
	}
	return true;
}

/*
Flushes all dirty pages to disc, clearing the dirty flag.
*/
bool _FAT_cache_flush (CACHE* cache) {
	unsigned int i;

	if (!_FAT_cache_flushFat(cache)) {   // XTS-PATCH(libfat-g): the FAT first
		return false;
	}

	for (i = 0; i < cache->numberOfPages; i++) {
		if (cache->cacheEntries[i].dirty) {
			if (!_FAT_disc_writeSectors (cache->disc, cache->cacheEntries[i].sector, cache->cacheEntries[i].count, cache->cacheEntries[i].cache)) {
				return false;
			}
		}
		cache->cacheEntries[i].dirty = false;
	}

	return true;
}

/*
XTS-PATCH(libfat-c): a sector written straight to the disc (FSInfo) also replaces its copy in a cached page, so a later
write-back of that page cannot put the old contents back. The page's dirty flag is left as it is.
*/
void _FAT_cache_updateIfCached (CACHE* cache, sec_t sector, const void* buffer) {
	unsigned int i;
	for (i = 0; i < cache->numberOfPages; i++) {
		CACHE_ENTRY* e = &cache->cacheEntries[i];
		if (e->sector != CACHE_FREE && sector >= e->sector && sector < e->sector + e->count) {
			memcpy(e->cache + (sector - e->sector) * cache->bytesPerSector, buffer, cache->bytesPerSector);
		}
	}
}

void _FAT_cache_invalidate (CACHE* cache) {
	unsigned int i;
	_FAT_cache_flush(cache);
	for (i = 0; i < cache->numberOfPages; i++) {
		cache->cacheEntries[i].sector = CACHE_FREE;
		cache->cacheEntries[i].last_access = 0;
		cache->cacheEntries[i].count = 0;
		cache->cacheEntries[i].dirty = false;
	}
}
