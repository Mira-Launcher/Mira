#include "core/Password.h"

#include <dlfcn.h>

#include <array>
#include <vector>

namespace mira::password {
namespace {

// libxcrypt's own signatures; `data` is a struct crypt_data, which is opaque here.
using CryptR = char* (*)(const char* phrase, const char* setting, void* data);
using GensaltRn = char* (*)(const char* prefix, unsigned long count, const char* rbytes,
                            int nrbytes, char* output, int output_size);

struct Crypt {
  CryptR crypt_r = nullptr;
  GensaltRn gensalt_rn = nullptr;
};

const Crypt* Load() {
  static const Crypt* loaded = []() -> const Crypt* {
    for (const char* name : {"libcrypt.so.2", "libcrypt.so.1"}) {
      void* handle = dlopen(name, RTLD_NOW | RTLD_LOCAL);
      if (handle == nullptr) continue;
      Crypt crypt{reinterpret_cast<CryptR>(dlsym(handle, "crypt_r")),
                  reinterpret_cast<GensaltRn>(dlsym(handle, "crypt_gensalt_rn"))};
      if (crypt.crypt_r != nullptr && crypt.gensalt_rn != nullptr) return new Crypt(crypt);
      dlclose(handle);
    }
    return nullptr;
  }();
  return loaded;
}

// Room for any libcrypt's struct crypt_data (libxcrypt's is 32 KiB, glibc's was about 128 KiB).
constexpr size_t kDataSize = 256 * 1024;

std::string Run(const Crypt& crypt, const std::string& password, const std::string& setting) {
  std::vector<char> data(kDataSize, 0);
  const char* out = crypt.crypt_r(password.c_str(), setting.c_str(), data.data());
  // A failed call returns null or a string starting with '*'.
  return out == nullptr || out[0] == '*' ? std::string() : std::string(out);
}

}  // namespace

Result<std::string> Hash(const std::string& password) {
  const Crypt* crypt = Load();
  if (crypt == nullptr) {
    return Err("libcrypt_missing", "couldn't load the system's libcrypt to hash the password",
               "Install libxcrypt (libcrypt1 on Debian and Ubuntu).");
  }
  for (const char* prefix : {"$y$", "$6$"}) {
    std::array<char, 128> setting{};
    // A null rbytes asks libcrypt for random bytes from the system.
    if (crypt->gensalt_rn(prefix, 0, nullptr, 0, setting.data(),
                          static_cast<int>(setting.size())) == nullptr)
      continue;
    if (std::string hash = Run(*crypt, password, setting.data()); !hash.empty()) return hash;
  }
  return Err("password_hash_failed", "libcrypt couldn't hash the password");
}

bool Matches(const std::string& password, const std::string& hash) {
  const Crypt* crypt = Load();
  if (crypt == nullptr || hash.empty()) return false;
  const std::string again = Run(*crypt, password, hash);
  if (again.size() != hash.size()) return false;
  unsigned char differ = 0;  // constant time, so timing doesn't tell how much matched
  for (size_t i = 0; i < hash.size(); ++i) differ |= static_cast<unsigned char>(again[i] ^ hash[i]);
  return differ == 0;
}

}  // namespace mira::password
