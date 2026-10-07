// Linked into mirad, mira and mira-run (cmake: MIRA_STATIC_LIBSTDCXX).
//
// The static libstdc++ was built against a newer glibc than an LTS distro has: it calls the
// glibc 2.38 __isoc23_strto* functions and glibc 2.36's arc4random. Defining them here makes the
// linker use these instead, so the binaries ask only for glibc 2.35. The strto* ones forward to
// the original functions; arc4random reads the kernel's random pool. glibc 2.35's _dl_find_object is
// what the static libgcc's unwinder tries first; answering -1 sends it to dl_iterate_phdr, which is all
// glibc before 2.35 has, so the binaries then run on glibc 2.34 too.
#include <sys/random.h>

#include <cstdint>

extern "C" {

long old_strtol(const char*, char**, int) __asm__("strtol");
long long old_strtoll(const char*, char**, int) __asm__("strtoll");
unsigned long old_strtoul(const char*, char**, int) __asm__("strtoul");
unsigned long long old_strtoull(const char*, char**, int) __asm__("strtoull");

long __isoc23_strtol(const char* s, char** end, int base) { return old_strtol(s, end, base); }
long long __isoc23_strtoll(const char* s, char** end, int base) { return old_strtoll(s, end, base); }
unsigned long __isoc23_strtoul(const char* s, char** end, int base) { return old_strtoul(s, end, base); }
unsigned long long __isoc23_strtoull(const char* s, char** end, int base) { return old_strtoull(s, end, base); }

std::uint32_t arc4random(void) {
  std::uint32_t value = 0;
  while (getrandom(&value, sizeof value, 0) != static_cast<ssize_t>(sizeof value)) {
  }
  return value;
}

int _dl_find_object(void*, void*) { return -1; }

}  // extern "C"
