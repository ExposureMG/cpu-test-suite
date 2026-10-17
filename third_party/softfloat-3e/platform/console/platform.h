/* XTS: SoftFloat 3e platform header for the console build (xenon-gcc -m32 -mpowerpc64, big-endian, no __int128). */
#ifdef __GNUC_STDC_INLINE__
#define INLINE inline
#else
#define INLINE extern inline
#endif
#define SOFTFLOAT_BUILTIN_CLZ 1
#include "opts-GCC.h"
