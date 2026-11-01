#include "core/Password.h"

#include <dlfcn.h>

#include <array>
#include <cstdlib>
#include <format>
#include <vector>

#ifdef __APPLE__
#include <CommonCrypto/CommonKeyDerivation.h>
#include <stdlib.h>
#endif

namespace mira::password {
namespace {

#ifdef __APPLE__
// macOS's crypt(3) only has DES, so it's PBKDF2-SHA256 there:
// "$pbkdf2-sha256$<rounds>$<salt>$<key>" in hex.
constexpr unsigned kRounds = 600000;
constexpr std::string_view kPbkdf2 = "$pbkdf2-sha256$";

std::string Hex(const unsigned char* bytes, size_t size) {
  std::string out;
  for (size_t i = 0; i < size; ++i) out += std::format("{:02x}", bytes[i]);
  return out;
}

std::string Pbkdf2(const std::string& password, const std::string& salt_hex, unsigned rounds) {
  std::array<unsigned char, 32> key{};
  if (CCKeyDerivationPBKDF(kCCPBKDF2, password.data(), password.size(),
                           reinterpret_cast<const uint8_t*>(salt_hex.data()), salt_hex.size(),
                           kCCPRFHmacAlgSHA256, rounds, key.data(), key.size()) != 0)
    return {};
  return std::format("{}{}${}${}", kPbkdf2, rounds, salt_hex, Hex(key.data(), key.size()));
}
#endif

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
#ifdef __APPLE__
  std::array<unsigned char, 16> salt{};
  arc4random_buf(salt.data(), salt.size());
  if (std::string hash = Pbkdf2(password, Hex(salt.data(), salt.size()), kRounds); !hash.empty())
    return hash;
  return Err("password_hash_failed", "couldn't hash the password");
#endif
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
  std::string again;
#ifdef __APPLE__
  if (hash.starts_with(kPbkdf2)) {
    const size_t rounds_end = hash.find('$', kPbkdf2.size());
    const size_t salt_end =
        rounds_end == std::string::npos ? rounds_end : hash.find('$', rounds_end + 1);
    if (salt_end == std::string::npos) return false;
    const unsigned rounds = unsigned(std::strtoul(hash.c_str() + kPbkdf2.size(), nullptr, 10));
    again = Pbkdf2(password, hash.substr(rounds_end + 1, salt_end - rounds_end - 1), rounds);
  }
#endif
  if (again.empty()) {
    const Crypt* crypt = Load();
    if (crypt == nullptr || hash.empty()) return false;
    again = Run(*crypt, password, hash);
  }
  if (again.size() != hash.size()) return false;
  unsigned char differ = 0;  // constant time, so timing doesn't tell how much matched
  for (size_t i = 0; i < hash.size(); ++i) differ |= static_cast<unsigned char>(again[i] ^ hash[i]);
  return differ == 0;
}

}  // namespace mira::password
