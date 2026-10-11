// Минимальный самодельный юнит-тест харнесс — без внешнего фреймворка
// (Catch2/GoogleTest и т.п.). Проект принципиально избегает пакетных
// менеджеров и лишних зависимостей (см. README.md, "Минимум внешних
// зависимостей") — десяток макросов здесь дешевле, чем тащить и собирать
// отдельную библиотеку ради junit-подобного вывода.
#pragma once

#include <iostream>
#include <string>
#include <vector>

namespace testing {

struct TestCase {
    std::string name;
    void (*fn)();
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

struct Registrar {
    Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

// Число упавших CHECK/REQUIRE с начала процесса — test_main.cpp сравнивает
// значение до/после каждого теста, чтобы понять, упал ли именно он (тест не
// останавливается на первом упавшем CHECK, в отличие от REQUIRE).
inline int& failed_checks() {
    static int n = 0;
    return n;
}

inline std::string& current_test_name() {
    static std::string name;
    return name;
}

inline void report_failure(const std::string& expr, const char* file, int line) {
    ++failed_checks();
    std::cerr << "  [FAIL] " << current_test_name() << " (" << file << ":" << line << "): " << expr
               << "\n";
}

// REQUIRE прерывает текущий тест этим исключением (в отличие от CHECK,
// который просто отмечает провал и продолжает) — нужен там, где без
// провалившегося условия дальнейшие строки теста разыменовали бы пустой
// optional/вышли бы за границы. Перехватывается только в test_main.cpp.
struct RequireFailed {};

} // namespace testing

#define TEST(name)                                                              \
    static void name();                                                        \
    static ::testing::Registrar registrar_##name(#name, name);                  \
    static void name()

#define CHECK(cond)                                                             \
    do {                                                                        \
        if (!(cond)) ::testing::report_failure(#cond, __FILE__, __LINE__);      \
    } while (0)

#define CHECK_EQ(a, b)                                                          \
    do {                                                                        \
        if (!((a) == (b)))                                                      \
            ::testing::report_failure(#a " == " #b, __FILE__, __LINE__);        \
    } while (0)

#define CHECK_THROWS(expr)                                                      \
    do {                                                                        \
        bool threw = false;                                                     \
        try {                                                                   \
            (void)(expr);                                                       \
        } catch (...) {                                                         \
            threw = true;                                                       \
        }                                                                       \
        if (!threw) ::testing::report_failure(#expr " should throw", __FILE__, __LINE__); \
    } while (0)

#define REQUIRE(cond)                                                           \
    do {                                                                        \
        if (!(cond)) {                                                          \
            ::testing::report_failure(#cond, __FILE__, __LINE__);               \
            throw ::testing::RequireFailed{};                                   \
        }                                                                       \
    } while (0)
