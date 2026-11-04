add_executable(wallet_ai_tests
    test_main.cpp
    test_llm_probe.cpp
    test_llm_client.cpp
    test_ai.cpp
    test_expense_check.cpp
    test_jobs.cpp
    test_parse_expense.cpp
    test_statement.cpp
    test_imports.cpp
    test_auth_exchange.cpp
    test_mcp_client.cpp
)
target_include_directories(wallet_ai_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# Тесты читают настоящий prompts/expense.txt из исходников.
target_compile_definitions(wallet_ai_tests PRIVATE WALLET_AI_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(wallet_ai_tests PRIVATE wallet_ai_core)

add_test(NAME wallet_ai_unit_tests COMMAND wallet_ai_tests)
