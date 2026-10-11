#include "util/uuid.hpp"

#include <openssl/sha.h>

#include <cctype>
#include <cstdio>
#include <random>
#include <stdexcept>

namespace uuidgen {

namespace {

std::string format(const unsigned char* b) {
    char buf[37];
    std::snprintf(buf, sizeof(buf),
                  "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                  b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11],
                  b[12], b[13], b[14], b[15]);
    return std::string(buf);
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

} // namespace

std::string v4() {
    static thread_local std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<int> byte_dist(0, 255);

    unsigned char b[16];
    for (auto& x : b) x = static_cast<unsigned char>(byte_dist(rng));
    b[6] = static_cast<unsigned char>((b[6] & 0x0F) | 0x40); // версия 4
    b[8] = static_cast<unsigned char>((b[8] & 0x3F) | 0x80); // вариант RFC 4122
    return format(b);
}

std::string v5(const std::string& namespace_uuid, const std::string& name) {
    if (!looks_valid(namespace_uuid)) throw std::invalid_argument("uuid v5: invalid namespace uuid");

    std::string input;
    input.reserve(16 + name.size());
    for (std::size_t i = 0; i < namespace_uuid.size();) {
        if (namespace_uuid[i] == '-') {
            ++i;
            continue;
        }
        input.push_back(static_cast<char>(hex_value(namespace_uuid[i]) * 16 + hex_value(namespace_uuid[i + 1])));
        i += 2;
    }
    input += name;

    unsigned char digest[SHA_DIGEST_LENGTH];
    SHA1(reinterpret_cast<const unsigned char*>(input.data()), input.size(), digest);
    digest[6] = static_cast<unsigned char>((digest[6] & 0x0F) | 0x50); // версия 5
    digest[8] = static_cast<unsigned char>((digest[8] & 0x3F) | 0x80); // вариант RFC 4122
    return format(digest);
}

bool looks_valid(const std::string& s) {
    if (s.size() != 36) return false;
    for (size_t i = 0; i < s.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (s[i] != '-') return false;
        } else if (std::isxdigit(static_cast<unsigned char>(s[i])) == 0) {
            return false;
        }
    }
    return true;
}

} // namespace uuidgen
