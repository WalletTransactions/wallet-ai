cmake_minimum_required(VERSION 3.16)
project(wallet_ai CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Общий код сервисов (util/: jwt, jwks, crypto, log, metrics, ...) — копия
# репозитория wallet-common, обновляется его scripts/vendor.sh; руками не править.
set(WALLET_COMMON_DIR ${CMAKE_SOURCE_DIR}/third_party/wallet-common)

# OpenSSL — проверка подписи access-токена (RS256 по JWKS google-auth).
find_package(OpenSSL REQUIRED)
# PostgreSQL — своя база ai (импорт выписок, add-statement-import).
find_package(PostgreSQL REQUIRED)
find_package(Threads REQUIRED)

# Версия — тот же приём, что у остальных сервисов (см. CMakeLists.txt
# wallet-report, "Версия приложения"): номер из VERSION, хэш — из
# -DGIT_COMMIT (Containerfile) или из git при нативной сборке.
file(STRINGS ${CMAKE_SOURCE_DIR}/VERSION APP_VERSION_LINES LIMIT_COUNT 1)
list(GET APP_VERSION_LINES 0 APP_VERSION)
string(STRIP "${APP_VERSION}" APP_VERSION)

if(NOT DEFINED GIT_COMMIT OR "${GIT_COMMIT}" STREQUAL "")
    find_program(GIT_EXECUTABLE git)
    set(GIT_COMMIT "unknown")
    if(GIT_EXECUTABLE)
        execute_process(
            COMMAND ${GIT_EXECUTABLE} describe --tags --always --dirty --abbrev=8
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            OUTPUT_VARIABLE GIT_COMMIT
            OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE GIT_DESCRIBE_RESULT
            ERROR_QUIET
        )
        if(NOT GIT_DESCRIBE_RESULT EQUAL 0 OR "${GIT_COMMIT}" STREQUAL "")
            set(GIT_COMMIT "unknown")
        endif()
    endif()
endif()
message(STATUS "wallet_ai version: ${APP_VERSION} (${GIT_COMMIT})")

configure_file(
    ${CMAKE_SOURCE_DIR}/src/version.hpp.in
    ${CMAKE_BINARY_DIR}/generated/version.hpp
    @ONLY
)

# Всё, кроме main.cpp и docs.cpp, — библиотека: её же линкуют юнит-тесты.
add_library(wallet_ai_core STATIC
    src/access.cpp
    src/ai.cpp
    src/auth_exchange.cpp
    src/db.cpp
    src/expense_check.cpp
    src/import_runner.cpp
    src/imports.cpp
    src/imports_http.cpp
    src/jobs.cpp
    src/llm_client.cpp
    src/llm_probe.cpp
    src/mcp_client.cpp
    src/parse_expense.cpp
    src/session.cpp
    src/statement/chunk.cpp
    src/statement/pattern.cpp
    src/statement/pg_repo.cpp
    src/statement/profiles.cpp
    src/statement/source.cpp
    src/statement/textutil.cpp
    src/statement/verify.cpp
    ${WALLET_COMMON_DIR}/expense/resolver.cpp
    ${WALLET_COMMON_DIR}/expense/strict.cpp
    ${WALLET_COMMON_DIR}/expense/text.cpp
    ${WALLET_COMMON_DIR}/clients/reports_client.cpp
    ${WALLET_COMMON_DIR}/util/base64.cpp
    ${WALLET_COMMON_DIR}/util/crypto.cpp
    ${WALLET_COMMON_DIR}/util/dotenv.cpp
    ${WALLET_COMMON_DIR}/util/jwks.cpp
    ${WALLET_COMMON_DIR}/util/jwt.cpp
    ${WALLET_COMMON_DIR}/util/jwt_claims.cpp
    ${WALLET_COMMON_DIR}/util/log.cpp
    ${WALLET_COMMON_DIR}/util/metrics.cpp
    ${WALLET_COMMON_DIR}/util/uuid.cpp
)
target_include_directories(wallet_ai_core PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party
    ${WALLET_COMMON_DIR}
    ${CMAKE_BINARY_DIR}/generated
)
target_compile_definitions(wallet_ai_core PUBLIC
    $<$<CXX_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>
)
target_compile_options(wallet_ai_core PUBLIC
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
)
target_link_libraries(wallet_ai_core PUBLIC
    OpenSSL::Crypto
    PostgreSQL::PostgreSQL
    Threads::Threads
)
if(WIN32)
    target_link_libraries(wallet_ai_core PUBLIC ws2_32 crypt32)
endif()

add_executable(wallet_ai
    src/main.cpp
    src/docs.cpp
)
target_link_libraries(wallet_ai PRIVATE wallet_ai_core)

add_custom_command(TARGET wallet_ai POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/openapi.yaml
            $<TARGET_FILE_DIR:wallet_ai>/openapi.yaml
    COMMENT "Копирую openapi.yaml рядом с wallet_ai (GET /openapi.yaml, /docs)"
)
add_custom_command(TARGET wallet_ai POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/prompts
            $<TARGET_FILE_DIR:wallet_ai>/prompts
    COMMENT "Копирую prompts/ рядом с wallet_ai (читаются при старте, LLM_PROMPT_DIR)"
)
add_custom_command(TARGET wallet_ai POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/third_party/swagger-ui
            $<TARGET_FILE_DIR:wallet_ai>/vendor/swagger-ui
    COMMENT "Копирую вендоренный Swagger UI рядом с wallet_ai (GET /docs)"
)

# Юнит-тесты — без внешней сети: чистые функции, проба с пустым LLM_URL,
# клиенты auth_exchange и mcp — против фейковых серверов на 127.0.0.1. В образе
# запускаются стадией `test` Containerfile:
#   docker build --target test -f wallet-ai/Containerfile wallet-ai
option(WALLET_AI_BUILD_TESTS "Собирать юнит-тесты wallet_ai_tests" ON)
if(WALLET_AI_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# wallet_ai_llm_try — ручная проверка разбора траты против живой LLM
# (подбор промпта и модели), см. tools/llm_try.cpp. В образ не входит.
option(WALLET_AI_BUILD_TOOLS "Собирать wallet_ai_llm_try" OFF)
if(WALLET_AI_BUILD_TOOLS)
    add_executable(wallet_ai_llm_try tools/llm_try.cpp)
    target_link_libraries(wallet_ai_llm_try PRIVATE wallet_ai_core)
endif()
