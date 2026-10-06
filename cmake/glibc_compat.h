// Force-included into every translation unit (cmake: MIRA_GLIBC_COMPAT).
//
// glibc 2.38's headers redirect strtol, strtoll, strtoul and strtoull to
// __isoc23_* versions, which only exist in glibc 2.38 and later. Nothing here
// needs the C23 behavior (it only adds "0b" binary prefixes), so these bind
// to the original symbols instead and the binaries run on older glibc.
#if defined(__linux__) && defined(__x86_64__) && !defined(MIRA_NO_GLIBC_SYMVER)
__asm__(".symver __isoc23_strtol, strtol@GLIBC_2.2.5");
__asm__(".symver __isoc23_strtoll, strtoll@GLIBC_2.2.5");
__asm__(".symver __isoc23_strtoul, strtoul@GLIBC_2.2.5");
__asm__(".symver __isoc23_strtoull, strtoull@GLIBC_2.2.5");
#endif
