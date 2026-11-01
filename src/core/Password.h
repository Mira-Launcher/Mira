#pragma once

#include <string>

#include "core/Result.h"

namespace mira::password {

// A crypt(3) hash of `password`: yescrypt where the system's libcrypt has it, else SHA-512 crypt.
// libcrypt is loaded at run time, since its soname is libcrypt.so.2 on most distributions but
// libcrypt.so.1 on Ubuntu and Debian. PBKDF2-SHA256 on macOS, whose crypt(3) only has DES.
Result<std::string> Hash(const std::string& password);

// Whether `password` is the one `hash` was made from.
bool Matches(const std::string& password, const std::string& hash);

}  // namespace mira::password
