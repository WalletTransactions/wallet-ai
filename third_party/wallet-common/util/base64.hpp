// Base64URL (RFC 4648 §5) без padding — формат, которого требуют JWT и JWK.
#pragma once

#include <string>
#include <vector>

namespace b64 {

std::string encode(const std::vector<unsigned char>& data);
std::string encode(const std::string& data);

// Бросает std::invalid_argument при недопустимом символе/длине.
std::vector<unsigned char> decode(const std::string& input);

} // namespace b64
