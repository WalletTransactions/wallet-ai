#include "statement/textutil.hpp"

namespace stmt {

namespace {

// Кириллица в UTF-8: А–П = D0 90..9F, Р–Я = D0 A0..AF, а–п = D0 B0..BF,
// р–я = D1 80..8F, Ё = D0 81, ё = D1 91.
std::string change_case(const std::string& s, bool to_upper) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        auto c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            if (to_upper && c >= 'a' && c <= 'z') c = static_cast<unsigned char>(c - 32);
            if (!to_upper && c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c + 32);
            out.push_back(static_cast<char>(c));
            continue;
        }
        if (i + 1 < s.size() && (c == 0xD0 || c == 0xD1)) {
            auto n = static_cast<unsigned char>(s[i + 1]);
            unsigned code = ((c & 0x1F) << 6) | (n & 0x3F);
            if (to_upper) {
                if (code >= 0x430 && code <= 0x44F) code -= 0x20;
                else if (code == 0x451) code = 0x401;
            } else {
                if (code >= 0x410 && code <= 0x42F) code += 0x20;
                else if (code == 0x401) code = 0x451;
            }
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
            ++i;
            continue;
        }
        out.push_back(static_cast<char>(c));
    }
    return out;
}

} // namespace

std::string upper(const std::string& s) { return change_case(s, true); }
std::string lower(const std::string& s) { return change_case(s, false); }

std::string normalize_spaces(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        auto c = static_cast<unsigned char>(s[i]);
        if (c == '\t') {
            out.push_back(' ');
        } else if (c == 0xC2 && i + 1 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0xA0) {
            out.push_back(' ');
            ++i;
        } else if (c == 0xE2 && i + 2 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0x80 &&
                   (static_cast<unsigned char>(s[i + 2]) == 0xAF || static_cast<unsigned char>(s[i + 2]) == 0x89)) {
            out.push_back(' ');
            i += 2;
        } else {
            out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

std::string trim(const std::string& s) {
    auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start < text.size()) {
        auto nl = text.find('\n', start);
        std::string line = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        out.push_back(std::move(line));
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return out;
}

bool has_digit(const std::string& s) {
    for (char c : s) {
        if (c >= '0' && c <= '9') return true;
    }
    return false;
}

std::size_t utf8_length(const std::string& s) {
    std::size_t n = 0;
    for (char c : s) {
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++n;
    }
    return n;
}

bool valid_utf8(const std::string& s) {
    std::size_t i = 0;
    while (i < s.size()) {
        auto c = static_cast<unsigned char>(s[i]);
        std::size_t len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
        if (len == 0 || i + len > s.size()) return false;
        if (len == 2 && c < 0xC2) return false; // overlong
        for (std::size_t k = 1; k < len; ++k) {
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
        }
        i += len;
    }
    return true;
}

std::string utf8_prefix(const std::string& s, std::size_t max_chars) {
    std::size_t chars = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) {
            if (chars == max_chars) return s.substr(0, i);
            ++chars;
        }
    }
    return s;
}

} // namespace stmt
