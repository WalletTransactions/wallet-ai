#include "util/jwks.hpp"

#include "httplib.h"
#include "nlohmann/json.hpp"
#include "util/base64.hpp"
#include "util/log.hpp"

using nlohmann::json;

namespace jwks {

Cache::Cache(std::string auth_service_url) : auth_service_url_(std::move(auth_service_url)) {
    // Загружаем сразу при старте (fail fast) — не тихо отклонять все токены
    // до первого промаха, а сразу увидеть в логе, что auth-сервис
    // недоступен/не отдаёт JWKS. Не фатально (см. refresh()): не удалось —
    // просто следующий find() попробует ещё раз.
    std::lock_guard<std::mutex> lock(mutex_);
    refresh();
}

void Cache::refresh() {
    httplib::Client cli(auth_service_url_);
    cli.set_connection_timeout(5, 0);
    cli.set_read_timeout(5, 0);

    // Если refresh() случился посреди обработки запроса (неизвестный kid —
    // см. find() ниже), applog::current_trace_id() вернёт trace_id ЭТОГО
    // запроса (thread_local, тот же поток) — тогда в логах auth-сервиса
    // будет видно, что именно его обращение к JWKS вызвало. При старте
    // процесса (конструктор Cache) id ещё не выставлен — заголовок просто
    // не добавляется.
    httplib::Headers headers;
    std::string trace_id = applog::current_trace_id();
    if (!trace_id.empty()) headers.emplace("X-Trace-Id", trace_id);

    auto res = cli.Get("/.well-known/jwks.json", headers);
    if (!res || res->status != 200) {
        applog::error("не удалось получить JWKS с " + auth_service_url_ +
                      " — проверка токенов будет отклонять всё, пока auth-сервис не станет доступен");
        return;
    }

    try {
        json body = json::parse(res->body);
        for (const auto& key : body.value("keys", json::array())) {
            std::string kid = key.at("kid").get<std::string>();
            PublicKey pk;
            pk.n = b64::decode(key.at("n").get<std::string>());
            pk.e = b64::decode(key.at("e").get<std::string>());
            keys_[kid] = std::move(pk);
        }
    } catch (const std::exception& ex) {
        applog::error(std::string("не удалось разобрать JWKS с ") + auth_service_url_ + ": " +
                      ex.what());
    }
}

std::optional<PublicKey> Cache::find(const std::string& kid) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = keys_.find(kid);
    if (it != keys_.end()) return it->second;

    // Незнакомый kid — возможно, ключ на auth-сервисе сменился (ротация)
    // уже после того, как мы закэшировали JWKS. Пробуем один раз обновить
    // кэш, прежде чем сдаться.
    refresh();
    it = keys_.find(kid);
    if (it != keys_.end()) return it->second;
    return std::nullopt;
}

} // namespace jwks
