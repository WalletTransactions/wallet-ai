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

bool all_digits(const std::string& t) {
    return !t.empty() && std::all_of(t.begin(), t.end(), [](char c) { return c >= '0' && c <= '9'; });
}

// Буквы (латиница и всё не-ASCII — кириллица) без цифр и знаков.
std::size_t letters(const std::string& t) {
    std::size_t n = 0;
    for (char c : t) {
        auto u = static_cast<unsigned char>(c);
        if ((u >= 'A' && u <= 'Z') || (u & 0xC0) == 0xC0) ++n; // ведущий байт UTF-8 — одна буква
    }
    return n;
}

std::string without_digits(const std::string& t) {
    std::string out;
    for (char c : t) {
        if (c < '0' || c > '9') out.push_back(c);
    }
    return out;
}

// Wirex: «CARD 1111 : ПРОДАВЕЦ,ГОРОД,MNE» → «ПРОДАВЕЦ». Хвост после первой
// запятой отбрасывается, если последняя часть — код страны из 2–3 букв.
std::string strip_card_and_place(std::string s) {
    if (s.rfind("CARD ", 0) == 0) {
        auto colon = s.find(" : ");
        if (colon != std::string::npos && colon <= 12) s = s.substr(colon + 3);
    }
    auto last = s.rfind(',');
    auto first = s.find(',');
    if (last != std::string::npos) {
        std::string tail = trim(s.substr(last + 1));
        if ((tail.size() == 2 || tail.size() == 3) && latin_word(tail) && tail.find('.') == std::string::npos) {
            s = s.substr(0, first);
        }
    }
    return s;
}

} // namespace

std::string pattern_of(const std::string& description) {
    std::istringstream in(strip_card_and_place(upper(normalize_spaces(description))));
    std::vector<std::string> tokens;
    std::string t;
    while (in >> t) {
        // Номер карты или терминала («4021», «*4021», «TID12345») — не продавец;
        // у названия с приклеенным номером («SDEK0737») остаётся название.
        if (long_number(t)) {
            t = without_digits(t);
            if (letters(t) < 4) continue;
        }
        tokens.push_back(t);
    }
    // «… MOSCOW RUS»: страна в конце и город перед ней.
    if (tokens.size() >= 2 && is_country(tokens.back())) {
        tokens.pop_back();
        if (tokens.size() >= 2 && latin_word(tokens.back())) tokens.pop_back();
    }
    // Номер магазина и кассы сети: «AROMA 107 KASA 1», «SVJEZE 7», «VOLI10»
    // (D12). Число с точкой («APTEKA 36.6») — часть названия, не трогается.
    if (tokens.size() >= 3 && tokens[tokens.size() - 2] == "KASA" && all_digits(tokens.back())) {
        tokens.resize(tokens.size() - 2);
    }
    if (tokens.size() >= 2 && all_digits(tokens.back()) && tokens.back().size() <= 3) tokens.pop_back();
    if (!tokens.empty()) {
        auto& last = tokens.back();
        std::size_t cut = last.size();
        while (cut > 0 && last[cut - 1] >= '0' && last[cut - 1] <= '9') --cut;
        if (cut < last.size() && last.size() - cut <= 3 && letters(last.substr(0, cut)) >= 2 &&
            latin_word(last.substr(0, cut))) {
            last.resize(cut);
        }
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
