// Метрики Prometheus — счётчик HTTP-запросов + гистограмма их латентности.
// Единственные потребители: set_logger в main.cpp (то же место, что уже
// пишет access-лог "request_finished" — см. util/log.hpp) вызывает
// observe_http_request() на каждый запрос, а GET /metrics (см. docs.cpp)
// вызывает render().
//
// route — НЕ реальный путь запроса (там бывают id, см. applog::set_route в
// log.hpp — "по нему перцентили/среднее не сгруппировать, каждый вызов был
// бы своей группой"), а applog::current_route(): тот же литерал, что и
// паттерн регистрации маршрута ("DELETE /api/wallets/:id"), уже выставляемый
// каждым хендлером для access-лога. Кардинальность лейбла route ограничена
// числом зарегистрированных ручек, а не числом когда-либо запрошенных id.
//
// Не тянет никакой внешней библиотеки (ни prometheus-cpp, ни клиент через
// FFI) — текстовый формат экспозиции Prometheus 0.0.4 достаточно прост,
// чтобы собрать его вручную (тот же принцип, что и у db.hpp — тонкая прямая
// обёртка вместо тяжёлой зависимости ради нескольких типов метрик).
#pragma once

#include <string>

namespace metrics {

// method — "GET"/"POST"/... (req.method как есть). route — см. комментарий
// выше. status — код ответа. duration_ms — то же значение, что уже легло в
// access-лог как duration_ms (applog::request_duration_ms()) — единой
// точкой отсчёта для обоих проще сверять лог и метрику по одному и тому же
// запросу при разборе инцидента.
void observe_http_request(const std::string& method, const std::string& route, int status,
                           double duration_ms);

// Версия приложения (см. GET /version, docs.cpp, и version.hpp, сгенерённый
// CMake из VERSION + git-хэша) — вызывается ОДИН раз в main() до
// svr.listen(), значения неизменны до конца жизни процесса. Отдельная
// функция, не аргумент конструктора модуля — сам модуль ничего не строит,
// как и observe_http_request()/render() выше, только копит состояние в
// анонимном namespace .cpp.
//
// build — тег образа (`0.1.1-rc.2`, CMake APP_BUILD; локальная сборка —
// `<VERSION>-dev`), version — номер из VERSION: по version сверяет выкат
// smoke в wallet-infra deploy.sh, поэтому тег идёт отдельным полем.
// Момент вызова запоминается как время запуска процесса (started_at в
// GET /version, process_start_time_seconds в /metrics): вызов стоит в main()
// до listen(), отставание от реального старта — миллисекунды.
void set_build_info(const std::string& service, const std::string& version,
                     const std::string& build, const std::string& git_commit);

// Время вызова set_build_info() в ISO-8601 UTC (`2026-11-03T19:24:28Z`);
// пустая строка, если set_build_info() не вызывался.
std::string started_at_iso();

// Счётчик с одним лейблом — события, для которых observe_http_request не
// подходит: метрики бота (tg_updates_total{type}), проверка claims токена
// (jwt_claims_total{result}, см. util/jwt_claims.hpp) и т.п. label_value —
// только литералы из кода, не пользовательский ввод: иначе кардинальность
// ряда не ограничена.
void inc_counter(const std::string& name, const std::string& help, const std::string& label,
                 const std::string& label_value);

// Гистограмма длительности с одним лейблом — вызовы внешних API бота
// (tg_telegram_api_duration_seconds{method}, tg_wallet_push_duration_seconds
// {status}). Бакеты — те же, что у http_request_duration_seconds. label_value
// — только литералы из кода, как у inc_counter.
void observe_duration(const std::string& name, const std::string& help, const std::string& label,
                      const std::string& label_value, double seconds);

// Gauge без лейблов (tg_last_poll_ok_timestamp_seconds и т.п.).
void set_gauge(const std::string& name, const std::string& help, double value);

// Тело ответа GET /metrics — текстовый формат экспозиции Prometheus 0.0.4
// (см. handle_metrics в docs.cpp). Потокобезопасно, можно звать из
// произвольного потока httplib. Первой строкой — build_info (см.
// set_build_info выше), если он был вызван; не был (например, в юнит-тестах
// этого модуля) — блок build_info просто отсутствует.
std::string render();

}  // namespace metrics
