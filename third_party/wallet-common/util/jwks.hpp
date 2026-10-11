// Клиент публичных ключей auth-сервиса (GET /.well-known/jwks.json).
// Кэширует их в памяти — так проверка токена не требует похода в auth на
// каждый запрос (см. README, "Как убрали общий секрет"). Не хранит НИКАКИХ
// секретов — только публичные n/e.
#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace jwks {

struct PublicKey {
    std::vector<unsigned char> n;
    std::vector<unsigned char> e;
};

class Cache {
public:
    explicit Cache(std::string auth_service_url);

    // nullopt — ключ с таким kid не нашёлся даже после повторного похода за
    // JWKS (auth-сервис недоступен, либо это правда неизвестный/битый kid).
    std::optional<PublicKey> find(const std::string& kid);

private:
    // Под mutex_. Сеть — намеренно внутри лока: это редкий путь (старт
    // процесса + смена kid при ротации ключа на auth-сервисе), а не на
    // каждый запрос — усложнять двойной проверкой вне лока не стоит.
    void refresh();

    std::string auth_service_url_;
    std::mutex mutex_;
    std::unordered_map<std::string, PublicKey> keys_;
};

} // namespace jwks
