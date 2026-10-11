#include "test_framework.hpp"

#include <exception>

int main() {
    int total = 0;
    int failed_tests = 0;

    for (auto& t : testing::registry()) {
        ++total;
        testing::current_test_name() = t.name;
        int before = testing::failed_checks();
        try {
            t.fn();
        } catch (const testing::RequireFailed&) {
            // Уже залогировано в report_failure() в момент самого REQUIRE.
        } catch (const std::exception& e) {
            testing::report_failure(std::string("необработанное исключение: ") + e.what(), __FILE__,
                                     __LINE__);
        } catch (...) {
            testing::report_failure("необработанное исключение неизвестного типа", __FILE__, __LINE__);
        }

        if (testing::failed_checks() == before) {
            std::cout << "  [ OK ] " << t.name << "\n";
        } else {
            ++failed_tests;
        }
    }

    std::cout << "\n" << total << " тестов, упало " << failed_tests << " ("
               << testing::failed_checks() << " проваленных проверок)\n";
    return failed_tests == 0 ? 0 : 1;
}
