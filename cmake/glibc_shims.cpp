// Linked into mirad, mira and mira-run (cmake: MIRA_STATIC_LIBSTDCXX).
//
// The static libstdc++ was built against a newer glibc than an LTS distro has: it calls the
// glibc 2.38 __isoc23_strto* functions and glibc 2.36's arc4random. Defining them here makes the
// linker use these instead, so the binaries ask only for glibc 2.35. The strto* ones forward to
// the original functions; arc4random reads the kernel's random pool. The static libgcc's unwinder
// finds a frame's unwind info through glibc 2.35's _dl_find_object and has no fallback when it
// fails, so it's answered here from dl_iterate_phdr, and the binaries then run on glibc 2.34 too.
#include <dlfcn.h>
#include <link.h>
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

namespace {

struct FindObject {
  std::uintptr_t pc;
  dl_find_object* result;
};

// Fills the result for the loaded object whose segments hold the pc. Only dlfo_eh_frame is read
// by the unwinder; dlfo_link_map stays null.
int FindObjectIn(dl_phdr_info* info, size_t, void* data) {
  auto* find = static_cast<FindObject*>(data);
  const ElfW(Phdr)* eh_frame = nullptr;
  bool holds_pc = false;
  std::uintptr_t start = UINTPTR_MAX;
  std::uintptr_t end = 0;
  for (int i = 0; i < info->dlpi_phnum; ++i) {
    const ElfW(Phdr) & phdr = info->dlpi_phdr[i];
    if (phdr.p_type == PT_LOAD) {
      const std::uintptr_t segment_start = info->dlpi_addr + phdr.p_vaddr;
      const std::uintptr_t segment_end = segment_start + phdr.p_memsz;
      if (segment_start < start) start = segment_start;
      if (segment_end > end) end = segment_end;
      if (find->pc >= segment_start && find->pc < segment_end) holds_pc = true;
    } else if (phdr.p_type == PT_GNU_EH_FRAME) {
      eh_frame = &phdr;
    }
  }
  if (!holds_pc) return 0;
  *find->result = {};
  find->result->dlfo_map_start = reinterpret_cast<void*>(start);
  find->result->dlfo_map_end = reinterpret_cast<void*>(end);
  if (eh_frame != nullptr)
    find->result->dlfo_eh_frame = reinterpret_cast<void*>(info->dlpi_addr + eh_frame->p_vaddr);
  return 1;
}

}  // namespace

int _dl_find_object(void* pc, dl_find_object* result) {
  FindObject find{reinterpret_cast<std::uintptr_t>(pc), result};
  return dl_iterate_phdr(FindObjectIn, &find) != 0 ? 0 : -1;
}

}  // extern "C"
