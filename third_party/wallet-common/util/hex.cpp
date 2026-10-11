#include "util/hex.hpp"

#include <stdexcept>

namespace hex {

namespace {

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

} // namespace

std::string encode(const std::string& bytes) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (unsigned char c : bytes) {
        out += digits[(c >> 4) & 0xF];
        out += digits[c & 0xF];
    }
    return out;
}

std::string decode(const std::string& hex_str) {
    if (hex_str.size() % 2 != 0) {
        throw std::invalid_argument("нечётная длина hex-строки");
    }
    std::string out;
    out.reserve(hex_str.size() / 2);
    for (std::size_t i = 0; i < hex_str.size(); i += 2) {
        int hi = hex_value(hex_str[i]);
        int lo = hex_value(hex_str[i + 1]);
        if (hi < 0 || lo < 0) {
            throw std::invalid_argument("недопустимый символ в hex-строке");
        }
        out += static_cast<char>((hi << 4) | lo);
    }
    return out;
}

bool looks_like_sha256(const std::string& s) {
    if (s.size() != 64) return false;
    for (char c : s) {
        if (hex_value(c) < 0) return false;
    }
    return true;
}

} // namespace hex
