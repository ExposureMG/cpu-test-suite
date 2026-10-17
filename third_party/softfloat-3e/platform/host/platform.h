/* XTS: SoftFloat 3e platform header for the host build (x86-64 / any little-endian gcc target). */
#define LITTLEENDIAN 1
#ifdef __GNUC_STDC_INLINE__
#define INLINE inline
#else
#define INLINE extern inline
#endif
#define SOFTFLOAT_BUILTIN_CLZ 1
#if defined(__SIZEOF_INT128__)
#define SOFTFLOAT_INTRINSIC_INT128 1
#endif
#include "opts-GCC.h"
