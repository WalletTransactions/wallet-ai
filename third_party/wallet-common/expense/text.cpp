#include "expense/text.hpp"

#include <cctype>

namespace text {

namespace {

// Кодовая точка UTF-8 -> строка.
void append_utf8(std::string& out, unsigned cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

unsigned lower_cp(unsigned cp) {
    if (cp >= 'A' && cp <= 'Z') return cp + 32;
    if (cp >= 0x0410 && cp <= 0x042F) return cp + 0x20; // А-Я -> а-я
    if (cp == 0x0401 || cp == 0x0451) return 0x0435;    // Ё/ё -> е
    return cp;
}

bool is_digits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (std::isdigit(static_cast<unsigned char>(c)) == 0) return false;
    }
    return true;
}

} // namespace

std::string normalize_name(const std::string& s) {
    std::string out;
    bool pending_space = false;
    for (std::size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        // Двухбайтовые последовательности — единственные, что нам нужно
        // понижать (кириллица); остальное копируется байтами как есть.
        if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) {
            unsigned cp = ((c & 0x1F) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3F);
            if (pending_space && !out.empty()) out.push_back(' ');
            pending_space = false;
            append_utf8(out, lower_cp(cp));
            i += 2;
            continue;
        }
        if (std::isspace(c) != 0) {
            pending_space = true;
            ++i;
            continue;
        }
        if (pending_space && !out.empty()) out.push_back(' ');
        pending_space = false;
        out.push_back(static_cast<char>(c < 0x80 ? lower_cp(c) : c));
        ++i;
    }
    return out;
}

Match resolve_name(const std::string& query, const std::vector<Named>& items) {
    Match m;
    std::string q = normalize_name(query);
    if (q.empty()) return m;
    for (const auto& item : items) {
        if (normalize_name(item.name) == q) {
            m.kind = Match::Kind::Found;
            m.found = item;
            return m;
        }
    }
    for (const auto& item : items) {
        if (normalize_name(item.name).compare(0, q.size(), q) == 0) m.candidates.push_back(item);
    }
    if (m.candidates.size() == 1) {
        m.kind = Match::Kind::Found;
        m.found = m.candidates[0];
        m.candidates.clear();
    } else if (m.candidates.size() > 1) {
        m.kind = Match::Kind::Ambiguous;
    }
    return m;
}

std::string format_amount(const std::string& amount, const std::string& precision, const std::string& symbol) {
    std::string s = amount;
    std::string sign;
    if (!s.empty() && (s[0] == '-' || s[0] == '+')) {
        if (s[0] == '-') sign = "\xE2\x88\x92"; // U+2212 минус
        s = s.substr(1);
    }
    std::size_t dot = s.find('.');
    std::string int_part = dot == std::string::npos ? s : s.substr(0, dot);
    std::string frac_part = dot == std::string::npos ? std::string{} : s.substr(dot + 1);
    if (!is_digits(int_part) || (!frac_part.empty() && !is_digits(frac_part))) return amount;

    int digits = is_digits(precision) ? std::stoi(precision) : 2;
    if (static_cast<int>(frac_part.size()) < digits) frac_part.append(digits - frac_part.size(), '0');
    // Лишние знаки не округляем (это деньги, округление — дело сервера):
    // срезаем только хвостовые нули сверх precision.
    while (static_cast<int>(frac_part.size()) > digits && frac_part.back() == '0') frac_part.pop_back();

    while (int_part.size() > 1 && int_part[0] == '0') int_part.erase(0, 1);
    std::string grouped;
    int count = 0;
    for (auto it = int_part.rbegin(); it != int_part.rend(); ++it) {
        if (count > 0 && count % 3 == 0) grouped.insert(0, "\xE2\x80\xAF"); // U+202F узкий неразрывный пробел
        grouped.insert(grouped.begin(), *it);
        ++count;
    }

    std::string out = sign + grouped;
    if (!frac_part.empty()) out += "," + frac_part;
    if (!symbol.empty()) out += "\xC2\xA0" + symbol; // неразрывный пробел перед символом валюты
    return out;
}

std::string format_day_month(const std::string& iso) {
    if (iso.size() < 10 || iso[4] != '-' || iso[7] != '-') return iso;
    return iso.substr(8, 2) + "." + iso.substr(5, 2);
}

std::string truncate_utf8(const std::string& s, std::size_t max_bytes) {
    if (s.size() <= max_bytes) return s;
    std::size_t cut = max_bytes;
    // Не рвём многобайтовый символ: отступаем до начала последовательности.
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
    return s.substr(0, cut) + "\xE2\x80\xA6";
}

} // namespace text
