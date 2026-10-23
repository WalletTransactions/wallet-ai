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

std::string render_example(const Example& e) {
    auto src = split_lines(trim(e.source));
    std::string out;
    nlohmann::json row = e.expected;
    nlohmann::json lines = nlohmann::json::array();
    for (std::size_t i = 0; i < src.size(); ++i) {
        out += std::to_string(i + 1) + "| " + normalize_spaces(src[i]) + "\n";
        lines.push_back(static_cast<int>(i + 1));
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
            if (section != "signature" && section != "hints" && section != "examples" && section != "check") {
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
            else throw std::runtime_error("неизвестный ключ [check] " + key);
        }
    }
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
    for (const auto* e : examples) {
        std::string block = (header ? "" : "\nПримеры строк этого формата:\n") + render_example(*e);
        if (estimate_tokens(out + block) > max_tokens) break;
        out += block;
        header = true;
    }
    return out;
}

} // namespace stmt
