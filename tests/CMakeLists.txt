add_executable(wallet_ai_tests
    test_main.cpp
    test_llm_probe.cpp
    test_llm_client.cpp
    test_ai.cpp
)
target_include_directories(wallet_ai_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(wallet_ai_tests PRIVATE wallet_ai_core)

add_test(NAME wallet_ai_unit_tests COMMAND wallet_ai_tests)
