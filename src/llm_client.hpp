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

// System-промпт — файл prompts/expense.txt (читается при старте, см. его
// шапку): строки с # — комментарии, "# version: X" — версия для логов и
// eval, {today} {weekday} {recent_days} {year} {categories} {wallets} —
// подстановки.
struct Prompt {
    std::string version;
    std::string text; // без комментариев, с подстановками {…}
};

// Чистая функция; нет версии или текста — std::runtime_error.
Prompt parse_prompt(const std::string& content);
// Читает файл; нет файла — std::runtime_error.
Prompt load_prompt(const std::string& path);

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

// System-промпт с подставленными датами и именами пользователя.
std::string render_system_prompt(const Prompt& prompt, const ExpenseRequest& r);

// Тело POST /chat/completions — чистая функция. Текст пользователя — только в
// последнем user-сообщении, не в system: «игнорируй инструкции …» остаётся
// данными.
nlohmann::json expense_request_body(const ExpenseRequest& r, const Prompt& prompt, const std::string& model,
                                    const CallOptions& opts);

// Разбор ответа — чистая функция (seconds не заполняет).
Result parse_expense_response(int http_status, const std::string& body);

// Вырезает <think>…</think> перед ответом (модель с рассуждениями, которой
// не выключили их параметром) и пробелы по краям.
std::string strip_think(const std::string& content);

// "2026-10-11", -2 -> "2026-10-09". Невалидная дата — пусто.
std::string add_days(const std::string& ymd, int days);

// "2026-10-11" -> "воскресенье". Невалидная дата — пусто.
std::string weekday_ru(const std::string& ymd);

// --- Импорт выписки (openspec/changes/add-statement-import) ---

// extract_rows: пронумерованные строки фрагмента → операции со ссылками на
// номера строк. Суммы и даты потом проверяет код (statement/verify).
struct RowsRequest {
    std::string numbered_text;   // «17| 12.09 YANDEX*GO … -450,00»
    std::string date_context;    // заголовок даты из прошлого фрагмента или пусто
    std::string today;           // дата создания импорта, ГГГГ-ММ-ДД
    std::string period;          // «2026-09-01 — 2026-09-30» или пусто
    std::string wallet_currency; // ISO
    std::string profile;         // подсказки и примеры профиля формата (statement/profiles)
};

struct ExtractedRow {
    std::vector<int> lines;
    std::string date;
    std::string amount;
    std::string dir;
    std::string currency;
    std::string description;
};

struct RowsResult {
    Status status = Status::Unavailable;
    std::vector<ExtractedRow> rows;
    double seconds = 0;
    std::string detail;
    int completion_tokens = 0;
    int reasoning_tokens = 0;
};

// classify_rows: только описания продавцов и имена категорий — без сумм и дат.
struct ClassifyRequest {
    std::vector<std::string> patterns;
    std::vector<std::string> categories;
};

struct Classified {
    int i = 0;
    std::string category; // имя из списка или пусто
    double confidence = 0;
};

struct ClassifyResult {
    Status status = Status::Unavailable;
    std::vector<Classified> items;
    double seconds = 0;
    std::string detail;
    int completion_tokens = 0;
    int reasoning_tokens = 0;
};

nlohmann::json rows_request_body(const RowsRequest& r, const Prompt& prompt, const std::string& model,
                                 const CallOptions& opts);
RowsResult parse_rows_response(int http_status, const std::string& body);
nlohmann::json classify_request_body(const ClassifyRequest& r, const Prompt& prompt, const std::string& model,
                                     const CallOptions& opts);
ClassifyResult parse_classify_response(int http_status, const std::string& body);

// max_tokens extract_rows (D4): 200 + 100 на строку с суммой + символы
// фрагмента / 6, не больше 2400. 100 — замер на ТБанке: LM Studio отдаёт JSON
// с отступами, 85–100 токенов на операцию.
int rows_max_tokens(int money_lines, std::size_t chunk_chars);

class Client {
public:
    virtual ~Client() = default;
    virtual Result extract_expense(const ExpenseRequest& r, const CallOptions& opts) = 0;
    // Версия промпта, с которым клиент ходит в LLM, — для логов и eval.
    virtual const std::string& prompt_version() const = 0;

    virtual RowsResult extract_rows(const RowsRequest& r, const CallOptions& opts) = 0;
    virtual ClassifyResult classify_rows(const ClassifyRequest& r, const CallOptions& opts) = 0;
};

// Промпты импорта; пустые — импорт выключен (extract_rows/classify_rows
// отвечают Unavailable).
struct StatementPrompts {
    Prompt rows;
    Prompt classify;
};

std::unique_ptr<Client> make_http_client(const Config& cfg, Prompt prompt, StatementPrompts statement = {});

} // namespace llm
