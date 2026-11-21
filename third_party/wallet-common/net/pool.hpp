// Пул постоянных клиентов к одному апстриму (change add-internal-mtls, design D6).
//
// Зачем: при mTLS каждое новое соединение — рукопожатие. На горячих связях (гейтвей → api,
// report, ai, mcp, web) клиенты держатся в пуле с keep-alive, и рукопожатие делается не на
// каждый запрос. httplib::Client не потокобезопасен, поэтому клиента берёт один поток на время
// запроса, потом возвращает.
//
// Сервер закрывает простаивающее соединение (keep-alive около 5 с). Запрос на «протухшем»
// клиенте падает на чтении/записи — пул один раз повторяет его на свежем соединении.
// Редкие вызовы (JWKS, /internal/*, справочник счетов) пул не используют: рукопожатие ECDSA
// стоит долей миллисекунды. SSE-потоки (чат) — отдельный клиент мимо пула.
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "net/tls.hpp"

namespace nettls {

class ClientPool {
public:
    // base_url — http://host:port или https://…; при включённой cfg схема приводится к https
    // (как у make_client). max_idle — сколько простаивающих клиентов держать.
    ClientPool(std::string base_url, Config cfg, int connect_timeout_sec = 3, int read_timeout_sec = 30,
               std::size_t max_idle = 16);

    // fn получает клиента и возвращает результат запроса. Клиент — эксклюзивно на время вызова.
    // Сбой на переиспользованном клиенте (Read, Write, Connection) — один повтор на свежем.
    httplib::Result call(const std::function<httplib::Result(httplib::Client&)>& fn);

    struct Stats {
        std::size_t created = 0; // новых клиентов (≈ новых соединений)
        std::size_t reused = 0;  // запросов на уже открытом клиенте
        std::size_t retried = 0; // повторов после сбоя переиспользованного клиента
    };
    Stats stats() const;

    // Ошибка создания клиента (TLS включён, но файлы/ключ неверны) — пусто, если всё хорошо.
    std::string error() const;

private:
    std::unique_ptr<httplib::Client> make_fresh();
    std::unique_ptr<httplib::Client> acquire(bool& fresh);
    void release(std::unique_ptr<httplib::Client> client);

    std::string base_url_;
    Config cfg_;
    int connect_timeout_sec_;
    int read_timeout_sec_;
    std::size_t max_idle_;

    mutable std::mutex mu_;
    std::vector<std::unique_ptr<httplib::Client>> idle_;
    Stats stats_;
    std::string error_;
};

} // namespace nettls
