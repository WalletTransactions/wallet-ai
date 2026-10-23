#include "statement/pattern.hpp"

#include <algorithm>
#include <cstdio>
#include <sstream>
#include <vector>

#include "statement/textutil.hpp"
#include "util/crypto.hpp"
#include "util/uuid.hpp"

namespace stmt {

namespace {

// Постоянное пространство имён op_uuid импорта выписок. Менять нельзя:
// сменятся uuid уже записанных операций, и повторный push их задвоит.
constexpr const char* kStatementNamespace = "4f0c6d5e-2b7a-5c41-9e3d-7a1b8c2d9e60";

const char* const kCountries[] = {"RUS", "RU", "KAZ", "KZ", "BLR", "BY", "ARM", "AM", "GEO", "GE", "TUR", "TR",
                                  "UZB", "UZ", "KGZ", "KG", "ARE", "AE", "USA", "US", "GBR", "GB", "DEU", "DE",
                                  "NLD", "NL", "IRL", "IE", "CYP", "CY", "SRB", "RS", "THA", "TH"};

bool is_country(const std::string& t) {
    for (const char* c : kCountries) {
        if (t == c) return true;
    }
    return false;
}

bool latin_word(const std::string& t) {
    if (t.empty()) return false;
    for (char c : t) {
        if (!((c >= 'A' && c <= 'Z') || c == '-' || c == '.')) return false;
    }
    return true;
}

bool long_number(const std::string& t) {
    std::size_t digits = 0;
    std::size_t run = 0;
    for (char c : t) {
        run = (c >= '0' && c <= '9') ? run + 1 : 0;
        digits = std::max(digits, run);
    }
    return digits >= 4;
}

} // namespace

std::string pattern_of(const std::string& description) {
    std::istringstream in(upper(normalize_spaces(description)));
    std::vector<std::string> tokens;
    std::string t;
    while (in >> t) {
        // Номер карты или терминала («4021», «*4021», «TID12345») — не продавец.
        if (long_number(t)) continue;
        tokens.push_back(t);
    }
    // «… MOSCOW RUS»: страна в конце и город перед ней.
    if (tokens.size() >= 2 && is_country(tokens.back())) {
        tokens.pop_back();
        if (tokens.size() >= 2 && latin_word(tokens.back())) tokens.pop_back();
    }
    std::string out;
    for (const auto& tok : tokens) {
        if (!out.empty()) out += ' ';
        out += tok;
    }
    return out;
}

std::string fingerprint(const std::string& wallet_uuid, const std::string& date, const std::string& amount,
                        const std::string& dir, const std::string& pattern, int k) {
    auto digest = crypto::sha256(wallet_uuid + "|" + date + "|" + amount + "|" + dir + "|" + pattern + "|" +
                                 std::to_string(k));
    std::string hex;
    char buf[3];
    for (unsigned char b : digest) {
        std::snprintf(buf, sizeof(buf), "%02x", b);
        hex += buf;
    }
    return hex;
}

std::string op_uuid(const std::string& fp) { return uuidgen::v5(kStatementNamespace, fp); }

} // namespace stmt
