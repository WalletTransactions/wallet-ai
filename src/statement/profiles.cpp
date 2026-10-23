#include "statement/profiles.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include "statement/textutil.hpp"
#include "util/crypto.hpp"
#include "util/log.hpp"

namespace stmt {

namespace {

std::vector<std::string> split_list(const std::string& value) {
    std::vector<std::string> out;
    std::string item;
    std::istringstream in(value);
    while (std::getline(in, item, ',')) {
        item = trim(item);
        if (!item.empty()) out.push_back(item);
    }
    return out;
}

bool split_key(const std::string& line, std::string& key, std::string& value) {
    auto colon = line.find(':');
    if (colon == std::string::npos) return false;
    key = trim(line.substr(0, colon));
    value = trim(line.substr(colon + 1));
    return !key.empty();
}

// Номера строк примера — как в середине выписки, а не с 1: на прогоне ТБанка
// модель уже путала номер из «N|» с порядковым номером строки во фрагменте.
std::string render_example(const Example& e, int first_line) {
    auto src = split_lines(trim(e.source));
    std::string out;
    nlohmann::json row = e.expected;
    nlohmann::json lines = nlohmann::json::array();
    for (std::size_t i = 0; i < src.size(); ++i) {
        int n = first_line + static_cast<int>(i);
        out += std::to_string(n) + "| " + normalize_spaces(src[i]) + "\n";
        lines.push_back(n);
    }
    row["lines"] = lines;
    if (!row.contains("currency")) row["currency"] = "";
    out += "Ответ: " + nlohmann::json{{"rows", nlohmann::json::array({row})}}.dump() + "\n";
    return out;
}

const char* const kHeaderStems[] = {"дата", "сумм", "описан", "операц", "валют", "остат"};

int header_words(const std::string& line) {
    std::string s = lower(line);
    int n = 0;
    for (const char* stem : kHeaderStems) {
        if (s.find(stem) != std::string::npos) ++n;
    }
    return n;
}

} // namespace

Profile parse_profile(const std::string& content) {
    Profile p;
    std::string section;
    std::vector<std::string> example_lines;
    std::istringstream in(content);
    std::string raw;
    while (std::getline(in, raw)) {
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();
        std::string line = trim(raw);
        if (line.rfind("# version:", 0) == 0) {
            p.version = trim(line.substr(10));
            continue;
        }
        if (!line.empty() && line[0] == '#') continue;
        if (line.size() > 2 && line.front() == '[' && line.back() == ']') {
            section = line.substr(1, line.size() - 2);
            if (section != "signature" && section != "hints" && section != "examples" && section != "check" &&
                section != "columns" && section != "types") {
                throw std::runtime_error("неизвестная секция [" + section + "]");
            }
            continue;
        }
        std::string key;
        std::string value;
        if (section.empty()) {
            if (line.empty()) continue;
            if (!split_key(line, key, value)) throw std::runtime_error("ожидалось «ключ: значение»: " + line);
            if (key == "id") p.id = value;
            else if (key == "name") p.name = value;
            else throw std::runtime_error("неизвестный ключ " + key);
        } else if (section == "signature") {
            if (!line.empty()) p.signature.push_back(lower(normalize_spaces(line)));
        } else if (section == "hints") {
            p.hints += raw + "\n";
        } else if (section == "examples") {
            if (line.empty()) continue;
            if (line.rfind("=>", 0) == 0) {
                if (example_lines.empty()) throw std::runtime_error("у примера нет строк выписки");
                Example e;
                for (const auto& l : example_lines) e.source += l + "\n";
                try {
                    e.expected = nlohmann::json::parse(line.substr(2));
                } catch (const std::exception&) {
                    throw std::runtime_error("ответ примера — не JSON: " + line);
                }
                for (const char* f : {"date", "amount", "dir", "description"}) {
                    if (!e.expected.contains(f) || !e.expected[f].is_string()) {
                        throw std::runtime_error(std::string("в ответе примера нет строки ") + f);
                    }
                }
                p.examples.push_back(std::move(e));
                example_lines.clear();
            } else {
                example_lines.push_back(raw);
            }
        } else if (section == "check") {
            if (line.empty()) continue;
            if (!split_key(line, key, value)) throw std::runtime_error("ожидалось «ключ: значение»: " + line);
            if (key == "stop_words") p.check.stop_words = split_list(value);
            else if (key == "date_formats") p.check.date_formats = split_list(value);
            else if (key == "expense_word") p.check.expense_word = value;
            else if (key == "transfer_words") p.check.transfer_words = split_list(value);
            else throw std::runtime_error("неизвестный ключ [check] " + key);
        } else if (section == "columns") {
            if (line.empty()) continue;
            if (!split_key(line, key, value)) throw std::runtime_error("ожидалось «ключ: значение»: " + line);
            auto& c = p.columns;
            if (key == "delimiter") {
                c.delimiter = value == "tab" ? '\t' : (value.size() == 1 ? value[0] : '\0');
                if (c.delimiter == '\0') throw std::runtime_error("разделитель — один символ или tab: " + value);
            } else if (key == "date") {
                // «Completed Date | DD-MM-YYYY HH:MM:SS»
                auto bar = value.find('|');
                c.date = trim(value.substr(0, bar));
                c.date_format = bar == std::string::npos ? "" : trim(value.substr(bar + 1));
                if (c.date_format.empty()) throw std::runtime_error("у колонки даты нет формата");
            } else if (key == "amount") c.amount = value;
            else if (key == "description") c.description = value;
            else if (key == "type") c.type = value;
            else if (key == "currency") c.currency = value;
            else throw std::runtime_error("неизвестный ключ [columns] " + key);
        } else if (section == "types") {
            if (line.empty()) continue;
            if (!split_key(line, key, value)) throw std::runtime_error("ожидалось «тип: направление»: " + line);
            if (value != "expense" && value != "income" && value != "transfer") {
                throw std::runtime_error("направление типа — expense, income или transfer: " + line);
            }
            p.types[lower(key)] = value;
        }
    }
    if (!p.columns.date.empty() || !p.columns.amount.empty() || !p.columns.description.empty()) {
        if (p.columns.date.empty() || p.columns.amount.empty() || p.columns.description.empty()) {
            throw std::runtime_error("в [columns] нужны date, amount и description");
        }
    }
    if (!p.types.empty() && p.columns.type.empty()) throw std::runtime_error("[types] без колонки type");
    if (p.version.empty()) throw std::runtime_error("нет строки \"# version: ...\"");
    if (p.id.empty() || p.name.empty()) throw std::runtime_error("нужны id и name");
    if (p.signature.empty()) throw std::runtime_error("пустая секция [signature]");
    if (!example_lines.empty()) throw std::runtime_error("у последнего примера нет строки «=> {json}»");
    if (p.examples.size() > 3) throw std::runtime_error("больше 3 примеров");
    p.hints = trim(p.hints);
    return p;
}

std::vector<Profile> load_profiles(const std::string& dir) {
    std::vector<Profile> out;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return out;
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.path().extension() == ".txt") files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    for (const auto& path : files) {
        std::ifstream f(path, std::ios::binary);
        std::ostringstream buf;
        buf << f.rdbuf();
        try {
            Profile p = parse_profile(buf.str());
            if (find_profile(out, p.id) != nullptr) throw std::runtime_error("повтор id " + p.id);
            out.push_back(std::move(p));
        } catch (const std::exception& e) {
            applog::warn("statement profile skipped", {{"event", "profile_invalid"},
                                                        {"file", path.filename().string()},
                                                        {"error", e.what()}});
        }
    }
    return out;
}

const Profile* find_profile(const std::vector<Profile>& profiles, const std::string& id) {
    for (const auto& p : profiles) {
        if (p.id == id) return &p;
    }
    return nullptr;
}

std::vector<std::string> matching_profiles(const std::vector<Profile>& profiles,
                                           const std::vector<std::string>& lines) {
    std::string head;
    for (std::size_t i = 0; i < lines.size() && i < 40; ++i) head += lower(normalize_spaces(lines[i])) + "\n";
    std::vector<std::string> out;
    for (const auto& p : profiles) {
        bool all = true;
        for (const auto& sig : p.signature) all = all && head.find(sig) != std::string::npos;
        if (all) out.push_back(p.id);
    }
    return out;
}

std::string format_fingerprint(const std::vector<std::string>& lines, bool csv) {
    std::string header;
    for (const auto& line : lines) {
        if (trim(line).empty()) continue;
        if (csv || header_words(line) >= 3) {
            header = line;
            break;
        }
    }
    if (header.empty()) return {};
    // Цифры (и с ними даты) отбрасываются, пробелы схлопываются.
    std::string norm;
    for (char c : lower(normalize_spaces(header))) {
        if (c >= '0' && c <= '9') continue;
        if (c == ' ' && (norm.empty() || norm.back() == ' ')) continue;
        norm.push_back(c);
    }
    norm = trim(norm);
    if (norm.empty()) return {};
    auto digest = crypto::sha256(norm);
    std::string hex;
    char buf[3];
    for (unsigned char b : digest) {
        std::snprintf(buf, sizeof(buf), "%02x", b);
        hex += buf;
    }
    return hex;
}

std::size_t estimate_tokens(const std::string& s) { return (utf8_length(s) + 2) / 3; }

std::string profile_prompt(const Profile* builtin, const std::vector<Example>& own, std::size_t max_tokens) {
    std::string out;
    if (builtin != nullptr && !builtin->hints.empty()) {
        out = "Особенности этого формата выписки:\n" + builtin->hints + "\n";
        if (estimate_tokens(out) > max_tokens) out = utf8_prefix(out, max_tokens * 3);
    }
    std::vector<const Example*> examples;
    for (const auto& e : own) examples.push_back(&e);
    if (builtin != nullptr) {
        for (const auto& e : builtin->examples) examples.push_back(&e);
    }
    bool header = false;
    int first_line = 37;
    for (const auto* e : examples) {
        std::string block = (header ? "" : "\nПримеры строк этого формата:\n") + render_example(*e, first_line);
        first_line += 23;
        if (estimate_tokens(out + block) > max_tokens) break;
        out += block;
        header = true;
    }
    return out;
}

namespace {

// Поля строки CSV; кавычки "…" вокруг поля (и "" внутри) снимаются.
std::vector<std::string> split_csv(const std::string& line, char delim) {
    std::vector<std::string> out;
    std::string cur;
    bool quoted = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        char ch = line[i];
        if (quoted) {
            if (ch == '"' && i + 1 < line.size() && line[i + 1] == '"') {
                cur.push_back('"');
                ++i;
            } else if (ch == '"') {
                quoted = false;
            } else {
                cur.push_back(ch);
            }
        } else if (ch == '"' && trim(cur).empty()) {
            cur.clear();
            quoted = true;
        } else if (ch == delim) {
            out.push_back(trim(cur));
            cur.clear();
        } else {
            cur.push_back(ch);
        }
    }
    out.push_back(trim(cur));
    return out;
}

// Дата поля по формату профиля: берётся часть до пробела («DD-MM-YYYY»),
// разделители должны совпасть, время после даты не проверяется.
std::string parse_column_date(const std::string& field, const std::string& format) {
    std::string f = format.substr(0, format.find(' '));
    if (field.size() < f.size()) return {};
    if (field.size() > f.size() && field[f.size()] != ' ' && field[f.size()] != 'T') return {};
    int y = 0;
    int m = 0;
    int d = 0;
    int year_digits = 0;
    for (std::size_t i = 0; i < f.size(); ++i) {
        char fc = f[i];
        char c = field[i];
        if (fc == 'D' || fc == 'M' || fc == 'Y') {
            if (c < '0' || c > '9') return {};
            int& slot = fc == 'D' ? d : (fc == 'M' ? m : y);
            slot = slot * 10 + (c - '0');
            if (fc == 'Y') ++year_digits;
        } else if (fc != c) {
            return {};
        }
    }
    if (year_digits == 2) y += 2000;
    std::string iso = to_iso(y, m, d);
    Date check;
    return parse_iso(iso, check) ? iso : std::string{};
}

} // namespace

std::optional<std::vector<ColumnRow>> parse_by_columns(const Profile& p, const std::vector<std::string>& lines) {
    const auto& c = p.columns;
    if (c.date.empty()) return std::nullopt;
    std::size_t header = 0;
    while (header < lines.size() && trim(lines[header]).empty()) ++header;
    if (header == lines.size()) return std::nullopt;

    // Колонки — по имени из заголовка: банк может переставить их местами.
    auto names = split_csv(lines[header], c.delimiter);
    auto index_of = [&](const std::string& name) -> int {
        if (name.empty()) return -1;
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (lower(names[i]) == lower(name)) return static_cast<int>(i);
        }
        return -2;
    };
    int i_date = index_of(c.date);
    int i_amount = index_of(c.amount);
    int i_desc = index_of(c.description);
    int i_type = index_of(c.type);
    int i_cur = index_of(c.currency);
    for (int i : {i_date, i_amount, i_desc, i_type, i_cur}) {
        if (i == -2) return std::nullopt;
    }

    std::vector<ColumnRow> out;
    for (std::size_t n = header + 1; n < lines.size(); ++n) {
        if (trim(lines[n]).empty()) continue;
        auto fields = split_csv(lines[n], c.delimiter);
        auto field = [&](int i) { return i >= 0 && i < static_cast<int>(fields.size()) ? fields[i] : std::string{}; };
        std::string date = parse_column_date(field(i_date), c.date_format);
        std::string raw = field(i_amount);
        auto cents = parse_amount(raw);
        // Не разобрались дата или сумма — строка остаётся «не разобранной».
        if (date.empty() || !cents) continue;
        bool minus = !raw.empty() && (raw[0] == '-' || raw.rfind("\xE2\x88\x92", 0) == 0);

        std::string dir = minus ? "expense" : "income";
        auto t = p.types.find(lower(field(i_type)));
        if (t != p.types.end()) {
            dir = t->second;
            // Плюс у типа-расхода — возврат, минус у типа-дохода — списание.
            if (dir == "expense" && !minus) dir = "income";
            else if (dir == "income" && minus) dir = "expense";
        }
        ColumnRow r;
        r.line = static_cast<int>(n) + 1;
        r.candidate = {{r.line}, date, format_cents(*cents), dir, upper(field(i_cur)), field(i_desc)};
        out.push_back(std::move(r));
    }
    return out;
}

} // namespace stmt
