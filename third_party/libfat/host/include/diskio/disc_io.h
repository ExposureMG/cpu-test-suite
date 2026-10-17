/* Host build of libfat (XTS): libxenon's diskio/disc_io.h interface, unchanged in layout. */
#ifndef OGC_DISC_IO_INCLUDE
#define OGC_DISC_IO_INCLUDE
#include <stdint.h>
#include <xetypes.h>

#define MAXDEVICES 16
#define DISKIO_ERROR_NO_MEDIA -8

#define FEATURE_MEDIUM_CANREAD      0x00000001
#define FEATURE_MEDIUM_CANWRITE     0x00000002
#define FEATURE_XENON_ATA           0x00000100
#define FEATURE_XENON_ATAPI         0x00000200
#define FEATURE_XENON_USB           0x00000400

typedef uint32_t sec_t;

typedef bool (* FN_MEDIUM_STARTUP)(void) ;
typedef bool (* FN_MEDIUM_ISINSERTED)(void) ;
typedef bool (* FN_MEDIUM_READSECTORS)(sec_t sector, sec_t numSectors, void* buffer) ;
typedef bool (* FN_MEDIUM_WRITESECTORS)(sec_t sector, sec_t numSectors, const void* buffer) ;
typedef bool (* FN_MEDIUM_CLEARSTATUS)(void) ;
typedef bool (* FN_MEDIUM_SHUTDOWN)(void) ;
typedef s32 (* FN_MEDIUM_DEVSECTORS)(void) ;

struct DISC_INTERFACE_STRUCT {
	unsigned long			ioType ;
	unsigned long			features ;
	FN_MEDIUM_STARTUP		startup ;
	FN_MEDIUM_ISINSERTED	isInserted ;
	FN_MEDIUM_READSECTORS	readSectors ;
	FN_MEDIUM_WRITESECTORS	writeSectors ;
	FN_MEDIUM_CLEARSTATUS	clearStatus ;
	FN_MEDIUM_SHUTDOWN		shutdown ;
	FN_MEDIUM_DEVSECTORS	sectors;
} ;

typedef struct DISC_INTERFACE_STRUCT DISC_INTERFACE ;
#endif
