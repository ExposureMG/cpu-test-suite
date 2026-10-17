/* Forced include (-include) for the HOST build of libfat only (XTS replay harness, host tests): maps the newlib
 * struct stat members libfat fills that glibc does not have. The libfat sources stay identical to the console's. */
#ifndef XTS_HOST_LIBFAT_H
#define XTS_HOST_LIBFAT_H
#include <sys/types.h>
#include <sys/stat.h>
#define st_spare1 st_atim.tv_nsec
#define st_spare2 st_mtim.tv_nsec
#define st_spare3 st_ctim.tv_nsec
#define st_spare4 __glibc_reserved
#endif
