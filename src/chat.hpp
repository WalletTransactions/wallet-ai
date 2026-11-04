// Задание chat (change add-chat-agent; спека ai-chat): вопрос свободным
// текстом → ответ модели, собранный агентным циклом.
//
//   модель → tool_calls → tools/call wallet-mcp → результаты модели → … → текст
//
// Цикл не зависит от провайдера (адаптеры — chat_models.hpp). Инструменты
// исполняет только wallet-ai через wallet-mcp токеном обмена (D4): модель не
// видит ни токенов, ни адресов, ни user_id.
//
// Вход клиента (ChatInput в openapi.yaml):
//   {"text": "сколько за няню", "conversation_id": "tg-42-1",
//    "now": {"date": "2026-11-04", "time": "14:05:00"}}   // conversation_id и now необязательны
// Вход задания (в памяти, стирается при завершении): то же + provider и
// mcp_token. Результат (ChatResult):
//   {"text", "conversation_id", "provider", "steps", "tools": [...]}
#pragma once

#include <chrono>
#include <cstddef>
#include <ctime>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "chat_models.hpp"
#include "jobs.hpp"
#include "llm_client.hpp"
#include "mcp_client.hpp"
#include "nlohmann/json.hpp"
#include "parse_expense.hpp"
#include "providers.hpp"

namespace chat {

inline constexpr const char* kKind = "chat";
inline constexpr std::size_t kMaxTextBytes = 1000;
inline constexpr std::size_t kMaxConversationIdChars = 64;

// Приписка к результатам инструментов перед последним вызовом без
// инструментов (предел AI_CHAT_MAX_STEPS).
inline constexpr const char* kFinalCallNote =
    "Предел вызовов инструментов исчерпан. Ответь пользователю по уже полученным данным, без новых вызовов.";

// Пусто — вход годится; иначе текст ошибки для 400.
std::string validate_input(const nlohmann::json& input);
bool valid_conversation_id(const std::string& id);

// Вход задания. conversation_id — из клиента или новый (UUID).
nlohmann::json make_job_input(const nlohmann::json& client_input, const std::string& conversation_id,
                              const std::string& provider, const std::string& mcp_token);

// System-промпт prompts/chat.txt (D10): {today} {weekday} {time}
// {mcp_instructions}. Текст пользователя сюда не попадает.
std::string render_system_prompt(const llm::Prompt& prompt, const std::string& date, const std::string& time,
                                 const std::string& mcp_instructions);

// Дата и время для промпта: input.now, иначе UTC сервера (server_now).
std::pair<std::string, std::string> prompt_now(const nlohmann::json& job_input, std::time_t server_now);

// История диалога в памяти (D6): по паре (пользователь, conversation_id) —
// пары «вопрос — итоговый ответ», не больше max_turns; TTL — от последнего
// обращения. Результаты инструментов сюда не попадают.
class Conversations {
public:
    using Clock = std::chrono::steady_clock;
    using Turn = std::pair<std::string, std::string>; // вопрос, ответ

    Conversations(std::size_t max_turns, std::chrono::seconds ttl) : max_turns_(max_turns), ttl_(ttl) {}

    // Пары диалога; чужой, неизвестный или истёкший — пусто. Продлевает TTL.
    std::vector<Turn> get(const std::string& user_id, const std::string& conversation_id, Clock::time_point now);
    void append(const std::string& user_id, const std::string& conversation_id, const std::string& question,
                const std::string& answer, Clock::time_point now);
    // Удаляет истёкшие диалоги (фоновый поток main.cpp).
    void sweep(Clock::time_point now);
    std::size_t size() const;

private:
    struct Entry {
        std::deque<Turn> turns;
        Clock::time_point last_used;
    };
    std::size_t max_turns_;
    std::chrono::seconds ttl_;
    mutable std::mutex mu_;
    std::map<std::pair<std::string, std::string>, Entry> entries_;
};

struct Config {
    int max_steps = 5;                         // AI_CHAT_MAX_STEPS
    int timeout_ms = 120000;                   // AI_JOB_TIMEOUT_SECS_CHAT
    std::size_t tool_result_max_chars = 6000;  // AI_CHAT_TOOL_RESULT_MAX_CHARS
};

// Всё, что нужно циклу; живёт в AppState.
struct Agent {
    mcp::Client* mcp = nullptr;
    const providers::Registry* registry = nullptr;
    const providers::CredentialResolver* credentials = nullptr;
    Conversations* conversations = nullptr;
    llm::Prompt prompt;
    Config cfg;
    std::function<std::chrono::steady_clock::time_point()> now = [] { return std::chrono::steady_clock::now(); };
    std::function<std::time_t()> wall_now = [] { return std::time(nullptr); };
};

// Выполняет задание chat. Outcome::log — для журнала завершения: provider,
// steps, tools, input_tokens, output_tokens, детали отказа; без текстов.
parse_expense::Outcome run(const jobs::Job& job, Agent& agent);

} // namespace chat
