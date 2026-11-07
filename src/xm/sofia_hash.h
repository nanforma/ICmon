// sofia_hash.h - MD5 and the "Sofia" password hash used by the DVRIP login.
#pragma once
#include <array>
#include <cstdint>
#include <string>

namespace xm {

std::array<uint8_t, 16> md5(const std::string& msg);
std::string md5_hex(const std::string& msg);

// MD5, then sum byte pairs mod 62 over [0-9A-Za-z] -> 8 characters. Blank password -> "tlJwpbo6".
// This is a weak, unsalted hash: anyone who sees it on the network can log in. The login is NOT encrypted.
std::string sofia_hash(const std::string& password);

}  // namespace xm
