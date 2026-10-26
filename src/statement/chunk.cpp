#include "statement/chunk.hpp"

#include <regex>

#include "statement/textutil.hpp"
#include "statement/verify.hpp"

namespace stmt {

namespace {

// Цена строки — длина того, что уйдёт в LLM (squeeze_spaces, как в numbered_text).
std::size_t cost_of(const std::string& line) {
    std::string t = squeeze_spaces(trim(line));
    return !t.empty() && has_digit(t) ? utf8_length(t) : 0;
}

} // namespace

bool starts_with_date(const std::string& line) {
    static const std::regex re(
        R"(^ *(\d{1,2}[./]\d{1,2}([./]\d{2,4})?|\d{4}-\d{1,2}-\d{1,2}|\d{1,2} (января|февраля|марта|апреля|мая|июня|июля|августа|сентября|октября|ноября|декабря))(?!\d))");
    std::string s = normalize_spaces(lower(line));
    return std::regex_search(s, re) && !dates_in_line(s).empty();
}

std::vector<ChunkPlan> plan_chunks(const std::vector<std::string>& lines, std::size_t max_chars) {
    const int total = static_cast<int>(lines.size());
    std::vector<std::pair<int, int>> ranges;
    auto boundary = [&](int n) { return trim(lines[n - 1]).empty() || starts_with_date(lines[n - 1]); };

    int start = 1;
    std::size_t acc = 0;
    for (int n = 1; n <= total; ++n) {
        std::size_t cost = cost_of(lines[n - 1]);
        if (n > start && acc + cost > max_chars) {
            // Последняя граница внутри текущего фрагмента; нет — режем перед n.
            int cut = n;
            for (int b = n; b > start; --b) {
                if (boundary(b)) {
                    cut = b;
                    break;
                }
            }
            ranges.emplace_back(start, cut - 1);
            start = cut;
            acc = 0;
            for (int k = start; k < n; ++k) acc += cost_of(lines[k - 1]);
        }
        acc += cost;
    }
    if (start <= total) ranges.emplace_back(start, total);

    std::vector<ChunkPlan> out;
    std::string last_header;
    int scanned = 0;
    for (const auto& [first, last] : ranges) {
        for (; scanned < first - 1; ++scanned) {
            if (is_date_header(lines[scanned])) last_header = trim(normalize_spaces(lines[scanned]));
        }
        if (money_lines(lines, first, last) == 0) continue;
        out.push_back({first, last, last_header});
    }
    return out;
}

std::string numbered_text(const std::vector<std::string>& lines, int first, int last) {
    std::string out;
    for (int n = first; n <= last && n <= static_cast<int>(lines.size()); ++n) {
        out += std::to_string(n) + "| " + squeeze_spaces(lines[n - 1]) + "\n";
    }
    return out;
}

int money_lines(const std::vector<std::string>& lines, int first, int last) {
    int n = 0;
    for (int i = first; i <= last && i <= static_cast<int>(lines.size()); ++i) {
        if (money_line(lines[i - 1])) ++n;
    }
    return n;
}

} // namespace stmt
