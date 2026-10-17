/* Host build of libfat (XTS): the one newlib reentrancy member libfat uses. */
#ifndef XTS_HOST_SYS_REENT_H
#define XTS_HOST_SYS_REENT_H
#include <sys/types.h>
struct _reent { int _errno; };
#endif
