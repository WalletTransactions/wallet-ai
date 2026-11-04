// Провайдеры моделей чата (change add-chat-agent, D7–D9; спека ai-providers):
// перечень, доступность для пользователя, настройки и согласие, ключ на
// каждый вызов.
//
//   lmstudio — OpenAI-совместимый API по LLM_URL; доступен, когда LLM_URL
//              задан и последняя проба LLM ok;
//   claude   — Anthropic Messages API; доступен, когда задан
//              ANTHROPIC_API_KEY и пользователь входит в
//              ANTHROPIC_ALLOWED_USERS (user_id через запятую или *).
//
// Настройки хранятся в базе ai (migrations/000002_ai_user_settings). Логика
// GET/PUT /api/ai/settings — чистые функции над SettingsRepo (тесты — на
// репозитории в памяти, tests/mem_repo.hpp).
#pragma once

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "chat_models.hpp"
#include "llm_probe.hpp"
#include "nlohmann/json.hpp"

namespace db {
class Pool;
}

namespace providers {

inline constexpr const char* kLmStudio = "lmstudio";
inline constexpr const char* kClaude = "claude";

using Epoch = long long;

// ANTHROPIC_ALLOWED_USERS: "*" — все; иначе user_id через запятую (пробелы
// по краям не считаются). Пусто — никто.
struct AllowList {
    bool all = false;
    std::set<std::string> users;
    bool contains(const std::string& user_id) const { return all || users.count(user_id) != 0; }
};
AllowList parse_allowed_users(const std::string& value);

struct ProviderInfo {
    std::string id;
    std::string title;
    bool cloud = false;
    bool requires_consent = false; // облачный: данные уходят наружу
};

struct Availability {
    bool available = false;
    std::string reason; // not_configured, not_allowed, результат пробы LLM (unreachable, ...)
};

struct Config {
    std::string default_provider = kLmStudio; // AI_DEFAULT_PROVIDER, только локальный
    bool llm_configured = false;              // LLM_URL задан
    bool claude_configured = false;           // ANTHROPIC_API_KEY задан
    AllowList claude_allowed;                 // ANTHROPIC_ALLOWED_USERS
};

class Registry {
public:
    explicit Registry(Config cfg);

    const std::vector<ProviderInfo>& list() const { return list_; }
    const ProviderInfo* find(const std::string& id) const;
    const std::string& default_provider() const { return cfg_.default_provider; }

    Availability availability(const std::string& user_id, const std::string& id, llm::ProbeResult probe) const;

    // Адаптер провайдера (фабрика — main.cpp, в тестах — FakeModel).
    void set_model(const std::string& id, std::shared_ptr<chat::Model> model) { models_[id] = std::move(model); }
    chat::Model* model(const std::string& id) const;

private:
    Config cfg_;
    std::vector<ProviderInfo> list_;
    std::map<std::string, std::shared_ptr<chat::Model>> models_;
};

// Ключ на каждый вызов модели (D9). Сейчас — ключи стека (owner instance):
// LLM_API_KEY для lmstudio (может быть пустым — без заголовка) и
// ANTHROPIC_API_KEY для claude. Вариант b добавит ветку owner user.
class CredentialResolver {
public:
    CredentialResolver(std::string llm_api_key, std::string anthropic_api_key)
        : llm_key_(std::move(llm_api_key)), anthropic_key_(std::move(anthropic_api_key)) {}
    virtual ~CredentialResolver() = default;
    virtual std::optional<chat::Credential> resolve(const std::string& user_id, const std::string& provider) const;

private:
    std::string llm_key_;
    std::string anthropic_key_;
};

// Поля журнала об отказе ключа: провайдер и владелец, без самого ключа.
nlohmann::json credential_log_fields(const std::string& provider, const chat::Credential& credential);

// --- Настройки пользователя ---

struct UserSettings {
    std::string provider;               // сохранённый выбор; пусто — не выбирал
    std::map<std::string, Epoch> consents; // провайдер -> когда дано согласие (секунды Unix)
};

class SettingsRepo {
public:
    virtual ~SettingsRepo() = default;
    virtual UserSettings get(const std::string& user_id) = 0;
    virtual void set_provider(const std::string& user_id, const std::string& provider) = 0;
    virtual void set_consent(const std::string& user_id, const std::string& provider, Epoch at) = 0;
    virtual void revoke_consent(const std::string& user_id, const std::string& provider) = 0;
};

std::unique_ptr<SettingsRepo> make_pg_settings_repo(db::Pool& pool);

// Провайдер, которым пойдёт задание: сохранённый, если он известен и (для
// облачного) есть согласие; иначе AI_DEFAULT_PROVIDER. Доступность не
// проверяет — это делает создание задания (без молчаливой подмены).
std::string effective_provider(const Registry& reg, const UserSettings& s);

// Тело GET/PUT /api/ai/settings (схема AiSettings в openapi.yaml). Ключей нет.
nlohmann::json settings_json(const Registry& reg, const std::string& user_id, const UserSettings& s,
                             llm::ProbeResult probe);

struct Reply {
    int status = 200;
    nlohmann::json body;
};

// GET /api/ai/settings. repo == nullptr (нет DATABASE_URL или база не
// отвечает) — провайдер по умолчанию.
Reply get_settings(const Registry& reg, SettingsRepo* repo, const std::string& user_id, llm::ProbeResult probe);

// PUT /api/ai/settings {provider, consent?}: 400 provider_unavailable,
// 409 consent_required, 503 settings_unavailable; consent:false у облачного
// провайдера — отзыв, выбор переходит на AI_DEFAULT_PROVIDER. Исключение
// репозитория вызывающий превращает в 503 settings_unavailable.
Reply put_settings(const Registry& reg, SettingsRepo* repo, const std::string& user_id, const nlohmann::json& body,
                   llm::ProbeResult probe, Epoch now);

// "2026-11-04T10:00:00Z".
std::string iso_utc(Epoch t);

} // namespace providers
