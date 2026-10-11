// Разбор траты свободным текстом через LLM (OpenAI API, POST /chat/completions)
// — «Шаг 9 подробно», 9b, org/docs/TODO_telegram_bot.md; место кода —
// wallet-ai (org/docs/TODO_wallet_ai.md, шаг 2). Один синхронный вызов: его
// делает воркер задания parse_expense (§5 там), клиенту ждать не нужно.
//
// Модель только извлекает поля строками. Проверить их (сумма есть в тексте,
// дата не в будущем, ...) и привести к черновику — дело вызывающего кода
// (шаг 3): клиенту LLM им верить незачем.
#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "llm_probe.hpp"
#include "nlohmann/json.hpp"

namespace llm {

// Версия промпта — в логи и в eval (шаг 10 плана бота): сравнивать модели и
// промпты как baseline/current. Меняете промпт или примеры — поднимайте.
// v2: календарь последних 7 дней в system — модель выбирает дату из списка,
// а не считает дни недели (v1 на «в пятницу» отвечала то прошлой, то
// следующей пятницей).
inline constexpr const char* kPromptVersion = "expense-v2";

// Что модель извлекла из текста. Строки как есть.
struct Extraction {
    std::string amount;    // "1200.50" или "1200,50" — как модель написала; пусто — не трата
    std::string currency;  // ISO, пусто — не указана
    std::string category;  // одно из имён пользователя или пусто
    std::string wallet;    // одно из имён кошельков или пусто
    std::string date;      // "YYYY-MM-DD" или пусто — сегодня
    std::string comment;
    double confidence = 0;
};

struct ExpenseRequest {
    std::string text;     // сообщение пользователя
    std::string today;    // "YYYY-MM-DD" в его поясе; день недели и календарь — из неё
    std::vector<std::string> categories;
    std::vector<std::pair<std::string, std::string>> wallets; // имя, ISO
};

// Настройки вызова — на вид задания (TODO_wallet_ai.md, §3, §5).
struct CallOptions {
    // reasoning_effort OpenAI API; пусто — поле не передаётся. У gemma-4 в
    // LM Studio "none" рассуждения НЕ выключает, а сокращает (проверено
    // 2026-10-11: «кофе 200» — 130–225 токенов вместо ~300, на «вчера такси
    // 450» — 0); "low"/"minimal" и прочие способы LM Studio игнорирует.
    std::string reasoning_effort = "none";
    // С запасом под эти рассуждения: ответ — ~80 токенов, на 256 JSON
    // обрывался (finish_reason=length). Худший случай на GTX 1070 — ~9 с.
    int max_tokens = 512;
    // Дедлайн на весь запрос (httplib set_max_timeout), не на одно чтение.
    int timeout_ms = 60000;
};

enum class Status {
    Ok,
    NotExpense,      // модель ответила, что это не трата (amount пустой)
    Timeout,         // не уложились в CallOptions::timeout_ms
    Unavailable,     // сеть, 5xx
    Unauthorized,    // 401/403 — неверный LLM_API_KEY
    ModelNotLoaded,  // 404 — нет модели LLM_MODEL
    BadResponse,     // не JSON, оборван (finish_reason=length), нарушена схема, 400
};

const char* to_string(Status s);

struct Result {
    Status status = Status::Unavailable;
    Extraction extraction;
    double seconds = 0;
    // Для логов: что именно не так (без текста пользователя и ответа модели).
    std::string detail;
    int completion_tokens = 0;
    int reasoning_tokens = 0;
};

// Тело POST /chat/completions — чистая функция. Текст пользователя — только в
// последнем user-сообщении, не в system: «игнорируй инструкции …» остаётся
// данными.
nlohmann::json expense_request_body(const ExpenseRequest& r, const std::string& model, const CallOptions& opts);

// Разбор ответа — чистая функция (seconds не заполняет).
Result parse_expense_response(int http_status, const std::string& body);

// Вырезает <think>…</think> перед ответом (модель с рассуждениями, которой
// не выключили их параметром) и пробелы по краям.
std::string strip_think(const std::string& content);

// "2026-10-11", -2 -> "2026-10-09". Невалидная дата — пусто.
std::string add_days(const std::string& ymd, int days);

// "2026-10-11" -> "воскресенье". Невалидная дата — пусто.
std::string weekday_ru(const std::string& ymd);

class Client {
public:
    virtual ~Client() = default;
    virtual Result extract_expense(const ExpenseRequest& r, const CallOptions& opts) = 0;
};

std::unique_ptr<Client> make_http_client(const Config& cfg);

} // namespace llm
