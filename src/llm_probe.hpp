// Доступна ли LLM: GET {LLM_URL}/models (OpenAI API) и есть ли в списке
// LLM_MODEL. Результат — в /api/ai/status и метрике llm_up. Сам разбор трат
// (llm_client, chat/completions) — шаг 2 org/docs/TODO_wallet_ai.md.
//
// Только OpenAI-диалект: LM Studio, Ollama (/v1), llama-server, vLLM
// подменяются одним LLM_URL.
#pragma once

#include <memory>
#include <string>

namespace llm {

struct Config {
    std::string url;      // база OpenAI API, "http://host.docker.internal:1234/v1"; пусто — ИИ выключен
    std::string model;    // id из GET /models ("wallet-parse")
    std::string api_key;  // Authorization: Bearer; пусто — без заголовка
    int timeout_ms = 15000;
};

// "http://host:1234/v1/" -> {"http://host:1234", "/v1"}. httplib::Client
// берёт только схему+хост+порт, путь базы приклеивается к каждому запросу.
// Нет схемы или хоста — origin пустой.
struct BaseUrl {
    std::string origin;
    std::string path;
};
BaseUrl split_base_url(const std::string& url);

enum class ProbeResult {
    Ok,
    Disabled,        // LLM_URL пуст
    Unreachable,     // сеть, таймаут, 5xx
    Unauthorized,    // 401/403 — неверный LLM_API_KEY
    ModelNotLoaded,  // сервер отвечает, LLM_MODEL нет в списке
    BadResponse,     // не JSON / нет data[]
};

const char* to_string(ProbeResult r);

// Чистая функция: статус и тело ответа GET /models -> результат.
ProbeResult parse_models_response(int status, const std::string& body, const std::string& model);

class Probe {
public:
    virtual ~Probe() = default;
    virtual ProbeResult probe() = 0;
};

std::unique_ptr<Probe> make_http_probe(const Config& cfg);

} // namespace llm
