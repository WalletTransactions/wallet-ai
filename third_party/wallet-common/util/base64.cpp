#include "util/base64.hpp"

#include <cstdint>
#include <stdexcept>

namespace b64 {

namespace {

constexpr char kAlphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

int decode_char(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

} // namespace

std::string encode(const std::vector<unsigned char>& data) {
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);
    size_t i = 0;
    while (i + 3 <= data.size()) {
        uint32_t n = (static_cast<uint32_t>(data[i]) << 16) |
                     (static_cast<uint32_t>(data[i + 1]) << 8) |
                     static_cast<uint32_t>(data[i + 2]);
        out += kAlphabet[(n >> 18) & 0x3F];
        out += kAlphabet[(n >> 12) & 0x3F];
        out += kAlphabet[(n >> 6) & 0x3F];
        out += kAlphabet[n & 0x3F];
        i += 3;
    }
    size_t rem = data.size() - i;
    if (rem == 1) {
        uint32_t n = static_cast<uint32_t>(data[i]) << 16;
        out += kAlphabet[(n >> 18) & 0x3F];
        out += kAlphabet[(n >> 12) & 0x3F];
    } else if (rem == 2) {
        uint32_t n = (static_cast<uint32_t>(data[i]) << 16) |
                     (static_cast<uint32_t>(data[i + 1]) << 8);
        out += kAlphabet[(n >> 18) & 0x3F];
        out += kAlphabet[(n >> 12) & 0x3F];
        out += kAlphabet[(n >> 6) & 0x3F];
    }
    return out; // без '=' — так принято в JWT/JWK
}

std::string encode(const std::string& data) {
    return encode(std::vector<unsigned char>(data.begin(), data.end()));
}

std::vector<unsigned char> decode(const std::string& input) {
    std::vector<int> vals;
    vals.reserve(input.size());
    for (unsigned char c : input) {
        if (c == '=') continue; // терпимо относимся к паддингу, если он есть
        int v = decode_char(c);
        if (v < 0) throw std::invalid_argument("invalid base64url character");
        vals.push_back(v);
    }

    std::vector<unsigned char> out;
    out.reserve((vals.size() * 3) / 4 + 3);
    size_t i = 0;
    while (i + 4 <= vals.size()) {
        uint32_t n = (static_cast<uint32_t>(vals[i]) << 18) |
                     (static_cast<uint32_t>(vals[i + 1]) << 12) |
                     (static_cast<uint32_t>(vals[i + 2]) << 6) |
                     static_cast<uint32_t>(vals[i + 3]);
        out.push_back(static_cast<unsigned char>((n >> 16) & 0xFF));
        out.push_back(static_cast<unsigned char>((n >> 8) & 0xFF));
        out.push_back(static_cast<unsigned char>(n & 0xFF));
        i += 4;
    }
    size_t rem = vals.size() - i;
    if (rem == 1) {
        throw std::invalid_argument("invalid base64url length");
    } else if (rem == 2) {
        uint32_t n = (static_cast<uint32_t>(vals[i]) << 18) |
                     (static_cast<uint32_t>(vals[i + 1]) << 12);
        out.push_back(static_cast<unsigned char>((n >> 16) & 0xFF));
    } else if (rem == 3) {
        uint32_t n = (static_cast<uint32_t>(vals[i]) << 18) |
                     (static_cast<uint32_t>(vals[i + 1]) << 12) |
                     (static_cast<uint32_t>(vals[i + 2]) << 6);
        out.push_back(static_cast<unsigned char>((n >> 16) & 0xFF));
        out.push_back(static_cast<unsigned char>((n >> 8) & 0xFF));
    }
    return out;
}

} // namespace b64
