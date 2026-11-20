// Потоковый разбор ответов провайдеров для задания chat (change
// chat-sse-events, D3): SSE-кадры, фильтр <think>, сборщики OpenAI и Anthropic.
//
// Сборщики не строят Turn сами: они складывают куски в тот же JSON, который
// провайдер отдал бы без stream, и отдают его прежним parse_*_response. Так
// текст и tool_calls завершённого шага совпадают с обычным ответом по
// построению (спека ai-providers, «Потоковая генерация для chat»).
#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace chat {

// Разбор text/event-stream: кормим байтами как пришли, на каждое событие
// (пустая строка) вызывается on_data с data-строками, склеенными через \n.
// Комментарии (`: ping`) и поля, кроме data, пропускаются.
class SseParser {
public:
    using OnData = std::function<void(const std::string& data)>;
    void feed(const char* bytes, std::size_t len, const OnData& on_data);
    // Хвост без завершающей пустой строки (оборванный поток) не событие.

private:
    std::string line_;
    std::string data_;
    bool has_data_ = false;
    bool prev_cr_ = false;
};

// Прячет рассуждения <think>…</think> в начале ответа и пробелы по краям так
// же, как llm::strip_think: склейка выданных кусков равна strip_think(всего).
class ThinkFilter {
public:
    // Возвращает видимую часть куска (может быть пустой — часть удержана).
    std::string push(const std::string& chunk);

private:
    enum class State { Start, Thinking, Visible };
    State state_ = State::Start;
    std::string buf_;     // Start/Thinking: ещё не решённое
    std::string pending_; // Visible: пробелы в конце, выйдут только перед текстом
};

using OnDelta = std::function<void(const std::string& text)>;

// OpenAI-совместимый chat/completions со stream:true.
class OpenAiStreamAssembler {
public:
    explicit OpenAiStreamAssembler(OnDelta on_delta) : on_delta_(std::move(on_delta)) {}
    void on_data(const std::string& data); // одно событие SSE
    bool complete() const { return done_ || finished_; }
    bool bad() const { return bad_; } // кадр не JSON или {"error":…}
    // Тело, которое отдал бы запрос без stream — для parse_openai_response.
    std::string response_body() const;

private:
    OnDelta on_delta_;
    ThinkFilter filter_;
    std::string content_;
    std::string finish_reason_;
    struct Call {
        std::string id, name, args;
    };
    std::map<int, Call> calls_;
    nlohmann::json usage_;
    bool done_ = false, finished_ = false, bad_ = false;
};

// Anthropic Messages API со stream:true.
class AnthropicStreamAssembler {
public:
    explicit AnthropicStreamAssembler(OnDelta on_delta) : on_delta_(std::move(on_delta)) {}
    void on_data(const std::string& data);
    bool complete() const { return stopped_; }
    bool bad() const { return bad_; }
    // Тело, которое отдал бы запрос без stream — для parse_anthropic_response.
    std::string response_body() const;

private:
    OnDelta on_delta_;
    nlohmann::json blocks_ = nlohmann::json::array();
    std::map<int, std::string> partial_json_; // index блока tool_use -> куски input
    nlohmann::json usage_ = nlohmann::json::object();
    std::string stop_reason_;
    nlohmann::json stop_details_;
    bool stopped_ = false, bad_ = false;
};

} // namespace chat
