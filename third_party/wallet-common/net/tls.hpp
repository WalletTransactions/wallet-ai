// Взаимный TLS между сервисами контура (change add-internal-mtls, wallet-infra,
// D1): одно место, где httplib получает серверный и клиентский сертификат.
//
// Включение — тремя переменными окружения сервиса:
//   TLS_CERT — сертификат сервиса (PEM, цепочка; он же клиентский),
//   TLS_KEY  — его закрытый ключ,
//   TLS_CA   — корень CA контура: им проверяются И клиенты сервера, И серверы клиента.
// Ни одной не задано — режим выключен, всё остаётся обычным HTTP, как раньше.
// Задана часть — ошибка конфигурации (молча выключенный TLS хуже отказа на старте).
//
// Модуль не знает про сервисы и пути; права на эндпоинты (по CN клиента) решает
// сервис через peer_service().
//
// Нужен CPPHTTPLIB_OPENSSL_SUPPORT. Без него модуль собирается, но при включённой
// конфигурации make_server/make_client отказывают с понятным сообщением.
#pragma once

#include <memory>
#include <string>

#include "httplib.h"

namespace nettls {

struct Config {
    std::string cert;
    std::string key;
    std::string ca;

    bool enabled() const { return !cert.empty(); }
};

struct LoadResult {
    Config config;
    std::string error; // пусто — конфигурация корректна (в т.ч. «выключено»)
};

// Проверяет сочетание трёх значений: все пустые — выключено, все заданы — включено,
// иначе error с перечнем недостающих.
LoadResult config_from(const std::string& cert, const std::string& key, const std::string& ca);

// TLS_CERT, TLS_KEY, TLS_CA из окружения процесса.
LoadResult config_from_env();

// Что сервис делает с TLS (change add-internal-mtls). Материал (TLS_CERT, TLS_KEY, TLS_CA) один,
// а включают его два разных переключателя:
//   - сам материал (config.enabled()) — mTLS к служебному порту google-auth (пилот, шаги 4–5):
//     так включено у бота, wallet-mcp и wallet-ai, пока контур целиком на HTTP;
//   - INTERNAL_MTLS=true (задаёт compose.mtls.yaml, весь контур, шаг 7) — сервер сервиса
//     требует клиентский сертификат (serve), а вызовы к другим сервисам контура идут по
//     https с сертификатом (call_internal).
// Без разделения пилот на test включил бы TLS на серверах ai и mcp и сломал гейтвей.
struct Settings {
    Config config;
    bool serve = false;         // свой сервер — mTLS
    bool call_internal = false; // вызовы к сервисам контура — mTLS
    std::string error;          // не пусто — конфигурация неверна, сервис не стартует
};

// TLS_CERT, TLS_KEY, TLS_CA и INTERNAL_MTLS из окружения. INTERNAL_MTLS=true без полного
// набора TLS_* — ошибка (молча выключенная защита хуже отказа на старте).
Settings settings_from_env();

// Конфигурация для своего сервера / для вызовов внутри контура: пустая (HTTP), если
// соответствующий переключатель выключен.
inline Config server_config(const Settings& s) { return s.serve ? s.config : Config{}; }
inline Config internal_client_config(const Settings& s) { return s.call_internal ? s.config : Config{}; }

// Сервер: выключено — httplib::Server; включено — SSLServer, который требует
// клиентский сертификат от CA (рукопожатие без него не проходит). nullptr и error —
// если файлы не читаются, ключ не подходит к сертификату или нет поддержки OpenSSL.
std::unique_ptr<httplib::Server> make_server(const Config& cfg, std::string* error = nullptr);

// Клиент к base_url (http://host:port или https://host:port). Выключено — как есть;
// включено — схема приводится к https, предъявляется клиентский сертификат, сертификат
// сервера проверяется по CA И по имени хоста из URL (SAN).
std::unique_ptr<httplib::Client> make_client(const std::string& base_url, const Config& cfg,
                                             std::string* error = nullptr);

// То же по значению — для кода, где клиент создаётся прямо в вызове (`httplib::Client cli(url)`).
// Бросает std::runtime_error, если TLS включён, а клиент создать нельзя (неверные файлы): на
// старте сервисы это проверяют, поэтому в рантайме это редкая авария, а не штатный путь.
httplib::Client client_value(const std::string& base_url, const Config& cfg);

// http://… → https://…; остальное без изменений.
std::string to_https(const std::string& base_url);

// CN клиентского сертификата вызывающего (имя сервиса); пусто — plain HTTP или
// сертификата нет. Вызывать из обработчика запроса.
std::string peer_service(const httplib::Request& req);

} // namespace nettls
