#include "util/dotenv.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>

namespace dotenv {

namespace {

std::string trim(std::string s) {
    auto not_space = [](unsigned char c) { return std::isspace(c) == 0; };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

void set_if_absent(const std::string& key, const std::string& value) {
#ifdef _WIN32
    // _putenv_s всегда перезаписывает — сперва проверяем, не задана ли уже
    // переменная в реальном окружении, чтобы имитировать поведение dotenv.
    size_t len = 0;
    getenv_s(&len, nullptr, 0, key.c_str());
    if (len == 0) {
        _putenv_s(key.c_str(), value.c_str());
    }
#else
    setenv(key.c_str(), value.c_str(), 0); // 0 = не перезаписывать существующую
#endif
}

} // namespace

void load(const std::string& path) {
    std::ifstream f(path);
    if (!f) return;

    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == '#') continue;

        auto eq = trimmed.find('=');
        if (eq == std::string::npos) continue;

        std::string key = trim(trimmed.substr(0, eq));
        std::string value = trim(trimmed.substr(eq + 1));
        if (value.size() >= 2 &&
            ((value.front() == '"' && value.back() == '"') ||
             (value.front() == '\'' && value.back() == '\''))) {
            value = value.substr(1, value.size() - 2);
        }
        if (!key.empty()) set_if_absent(key, value);
    }
}

} // namespace dotenv
