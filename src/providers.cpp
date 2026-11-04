#include "providers.hpp"

#include <ctime>

using nlohmann::json;

namespace providers {

namespace {

std::string trim(const std::string& s) {
    auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

} // namespace

AllowList parse_allowed_users(const std::string& value) {
    AllowList out;
    std::size_t start = 0;
    while (start <= value.size()) {
        auto comma = value.find(',', start);
        std::string item = trim(value.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
        if (item == "*") {
            out.all = true;
        } else if (!item.empty()) {
            out.users.insert(item);
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return out;
}

Registry::Registry(Config cfg) : cfg_(std::move(cfg)) {
    list_ = {
        {kLmStudio, "Локальная модель (LM Studio)", /*cloud=*/false, /*requires_consent=*/false},
        {kClaude, "Claude (Anthropic)", /*cloud=*/true, /*requires_consent=*/true},
    };
    const ProviderInfo* def = find(cfg_.default_provider);
    if (def == nullptr || def->cloud) cfg_.default_provider = kLmStudio; // по умолчанию — только локальный
}

const ProviderInfo* Registry::find(const std::string& id) const {
    for (const auto& p : list_) {
        if (p.id == id) return &p;
    }
    return nullptr;
}

Availability Registry::availability(const std::string& user_id, const std::string& id, llm::ProbeResult probe) const {
    Availability a;
    if (id == kLmStudio) {
        if (!cfg_.llm_configured) {
            a.reason = "not_configured";
        } else if (probe != llm::ProbeResult::Ok) {
            a.reason = llm::to_string(probe);
        } else {
            a.available = true;
        }
        return a;
    }
    if (id == kClaude) {
        if (!cfg_.claude_configured) {
            a.reason = "not_configured";
        } else if (!cfg_.claude_allowed.contains(user_id)) {
            a.reason = "not_allowed";
        } else {
            a.available = true;
        }
        return a;
    }
    a.reason = "unknown";
    return a;
}

chat::Model* Registry::model(const std::string& id) const {
    auto it = models_.find(id);
    return it == models_.end() ? nullptr : it->second.get();
}

std::optional<chat::Credential> CredentialResolver::resolve(const std::string& /*user_id*/,
                                                            const std::string& provider) const {
    if (provider == kLmStudio) return chat::Credential{llm_key_, chat::Owner::Instance};
    if (provider == kClaude && !anthropic_key_.empty()) return chat::Credential{anthropic_key_, chat::Owner::Instance};
    return std::nullopt;
}

json credential_log_fields(const std::string& provider, const chat::Credential& credential) {
    return {{"provider", provider}, {"owner", chat::to_string(credential.owner)}};
}

std::string iso_utc(Epoch t) {
    std::time_t tt = static_cast<std::time_t>(t);
    std::tm tm{};
    gmtime_r(&tt, &tm);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::string effective_provider(const Registry& reg, const UserSettings& s) {
    const ProviderInfo* p = s.provider.empty() ? nullptr : reg.find(s.provider);
    if (p == nullptr) return reg.default_provider();
    if (p->requires_consent && s.consents.count(p->id) == 0) return reg.default_provider();
    return p->id;
}

json settings_json(const Registry& reg, const std::string& user_id, const UserSettings& s, llm::ProbeResult probe) {
    json list = json::array();
    for (const auto& p : reg.list()) {
        Availability a = reg.availability(user_id, p.id, probe);
        auto consent = s.consents.find(p.id);
        bool consented = consent != s.consents.end();
        json item = {{"id", p.id},
                     {"title", p.title},
                     {"cloud", p.cloud},
                     {"available", a.available},
                     {"requires_consent", p.requires_consent},
                     {"consented", consented},
                     {"consented_at", consented ? json(iso_utc(consent->second)) : json(nullptr)}};
        if (!a.available) item["reason"] = a.reason;
        list.push_back(item);
    }
    return {{"provider", effective_provider(reg, s)}, {"providers", list}};
}

Reply get_settings(const Registry& reg, SettingsRepo* repo, const std::string& user_id, llm::ProbeResult probe) {
    UserSettings s;
    if (repo != nullptr) s = repo->get(user_id);
    return {200, settings_json(reg, user_id, s, probe)};
}

Reply put_settings(const Registry& reg, SettingsRepo* repo, const std::string& user_id, const json& body,
                   llm::ProbeResult probe, Epoch now) {
    if (repo == nullptr) return {503, {{"error", "settings_unavailable"}}};
    if (!body.is_object()) return {400, {{"error", "bad_request"}}};
    if (!body.contains("provider") || !body["provider"].is_string()) return {400, {{"error", "provider_unavailable"}}};
    std::string id = body["provider"].get<std::string>();
    const ProviderInfo* p = reg.find(id);
    if (p == nullptr) return {400, {{"error", "provider_unavailable"}}};
    std::optional<bool> consent;
    if (body.contains("consent") && !body["consent"].is_null()) {
        if (!body["consent"].is_boolean()) return {400, {{"error", "bad_request"}}};
        consent = body["consent"].get<bool>();
    }

    UserSettings current = repo->get(user_id);
    if (p->requires_consent && consent.has_value() && !*consent) {
        // Отзыв согласия — даже если провайдер стал недоступен.
        repo->revoke_consent(user_id, id);
        if (current.provider == id) repo->set_provider(user_id, reg.default_provider());
        return {200, settings_json(reg, user_id, repo->get(user_id), probe)};
    }
    if (!reg.availability(user_id, id, probe).available) return {400, {{"error", "provider_unavailable"}}};
    if (p->requires_consent) {
        if (consent.value_or(false)) {
            repo->set_consent(user_id, id, now);
        } else if (current.consents.count(id) == 0) {
            return {409, {{"error", "consent_required"}}};
        }
    }
    repo->set_provider(user_id, id);
    return {200, settings_json(reg, user_id, repo->get(user_id), probe)};
}

} // namespace providers
