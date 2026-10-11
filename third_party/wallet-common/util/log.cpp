#include "util/log.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <cstdlib>
#include <random>
#include <sstream>
#include <system_error>

#include "nlohmann/json.hpp"

namespace applog {

namespace {

std::mutex g_mutex; // защищает всё ниже — httplib обрабатывает запросы в пуле потоков
std::ofstream g_file;
std::string g_dir;          // папка для файлов лога; пусто -> stdout/stderr
std::string g_service_name; // короткое имя сервиса — часть имени файла и поле "service"
std::string g_open_date;    // дата (YYYY-MM-DD), под которую сейчас открыт g_file
bool g_use_file = false;

// trace_id запроса, который сейчас обрабатывается ЭТИМ потоком — см.
// комментарий у set_trace_id/clear_trace_id в log.hpp.
thread_local std::string t_trace_id;

// Момент начала обработки текущего запроса ЭТИМ потоком — см.
// start_request_timer/request_duration_ms в log.hpp. steady_clock, не
// system_clock: не зависит от перевода системных часов, только для разницы
// во времени, не для отображения.
thread_local std::chrono::steady_clock::time_point t_request_start;

// См. set_route/set_user в log.hpp — тот же приём, что и t_trace_id выше.
thread_local std::string t_route;
thread_local std::string t_user_id;

std::tm to_tm(std::chrono::system_clock::time_point now, bool utc) {
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    if (utc) gmtime_s(&tm, &t);
    else localtime_s(&tm, &t);
#else
    if (utc) gmtime_r(&t, &tm);
    else localtime_r(&t, &tm);
#endif
    return tm;
}

std::string format(const std::tm& tm, const char* fmt) {
    std::ostringstream oss;
    oss << std::put_time(&tm, fmt);
    return oss.str();
}

// Ранг уровня: чем больше, тем серьёзнее — error=3 ... debug=0.
int level_rank(const std::string& level) {
    if (level == "error") return 3;
    if (level == "warn") return 2;
    if (level == "info") return 1;
    return 0;
}

// Минимальный уровень из LOG_LEVEL (см. log.hpp) — static, читается один раз:
// окружение процесса после старта не меняется, а getenv на каждую строку лога
// незачем. Не задана/не распознана — debug (ранг 0), пишется всё.
int min_level_rank() {
    static const int rank = [] {
        const char* v = std::getenv("LOG_LEVEL");
        return v != nullptr ? level_rank(v) : 0;
    }();
    return rank;
}

// Вызывается под g_mutex, только когда g_use_file уже true. Открывает файл
// под сегодняшнюю (ЛОКАЛЬНУЮ — это только имя файла для человека на
// Windows-хосте, не содержимое строки, см. log.hpp) дату, если он ещё не
// открыт либо дата сменилась с прошлой записи — в этом и есть вся ротация:
// не трогаем/не удаляем старые файлы, просто с полуночи начинаем писать в
// новый.
void ensure_file_for(const std::string& date) {
    if (g_file.is_open() && g_open_date == date) return;

    if (g_file.is_open()) g_file.close();

    std::string path = g_dir + "/" + date + "-" + g_service_name + ".log";
    g_file.open(path, std::ios::app);
    if (g_file.is_open()) {
        g_open_date = date;
    } else {
        std::cerr << "не удалось открыть лог-файл " << path << " — логирую в stdout/stderr\n";
    }
}

// details — необязательные структурированные поля поверх обычной строки
// (см. info/error с details в log.hpp); nullptr у обычных info()/error().
// Подмешиваются В ОБЪЕКТ ПЕРВЫМИ, системные поля ниже пишутся поверх — так
// details не может подделать ts/level/service/trace_id/msg, даже случайно
// использовав то же имя ключа.
void write_line(const std::string& level, const std::string& message,
                 const nlohmann::json* details) {
    if (level_rank(level) < min_level_rank()) return;

    auto now = std::chrono::system_clock::now();
    auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() %
        1000;

    std::lock_guard<std::mutex> lock(g_mutex);

    // ts — всегда UTC (см. log.hpp почему); millisecond-точность вручную —
    // put_time её не умеет.
    std::ostringstream ts;
    ts << format(to_tm(now, /*utc=*/true), "%Y-%m-%dT%H:%M:%S") << '.' << std::setfill('0')
       << std::setw(3) << ms << 'Z';

    // nlohmann::json сам экранирует спецсимволы/юникод в message — строить
    // JSON руками конкатенацией строк здесь опасно (кавычки, переносы строк
    // в сообщении об ошибке сломали бы строку).
    nlohmann::json j;
    if (details != nullptr && details->is_object()) j = *details; // не объект — молча игнорируем
    j["ts"] = ts.str();
    j["level"] = level;
    j["service"] = g_service_name;
    if (!t_trace_id.empty()) j["trace_id"] = t_trace_id;
    if (!t_route.empty()) j["route"] = t_route;
    if (!t_user_id.empty()) j["user_id"] = t_user_id;
    j["msg"] = message;
    // error_handler_t::replace, не strict (умолчание) — details теперь может
    // нести сырое тело HTTP-запроса/ответа (см. debug(..., {"request_body"...})
    // в set_logger, main.cpp), а оно не обязано быть валидным UTF-8 (не
    // текстовый Content-Type — например, blob wallet-api, который гейтвей
    // проксирует как есть, — или оборванная многобайтовая последовательность
    // на границе обрезки). strict уронил бы
    // всю запись лога исключением из-за одного нетекстового поля; replace
    // просто подставляет U+FFFD на невалидных байтах, сама строка не теряется.
    std::string line = j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);

    if (g_use_file) {
        ensure_file_for(format(to_tm(now, /*utc=*/false), "%Y-%m-%d"));
        if (g_file.is_open()) {
            g_file << line << std::endl;
            return;
        }
        // Файл внезапно недоступен (например, диск отвалился) — не молчим,
        // хотя бы попадём в stderr, который на Windows-службе тоже пишется
        // в лог (см. install-service.bat).
    }
    std::ostream& out = (level == "error") ? std::cerr : std::cout;
    out << line << std::endl;
}

} // namespace

void init(const std::string& dir, const std::string& service_name) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file.is_open()) g_file.close();
    g_dir = dir;
    g_service_name = service_name;
    g_open_date.clear();
    g_use_file = false;
    if (dir.empty()) return;

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        std::cerr << "не удалось создать папку логов " << dir << " — логирую в stdout/stderr\n";
        return;
    }
    // Сам файл откроется при первой записи (ensure_file_for) — так init()
    // не должен знать, какая сегодня дата, эта логика в одном месте.
    g_use_file = true;
}

std::string new_trace_id() {
    // thread_local, а не один общий rng под mutex — генерация id не должна
    // сериализовать параллельные запросы ради общего генератора; коллизии
    // между потоками при 128 бит случайности не забота этого кода.
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::uniform_int_distribution<uint64_t> dist;
    char buf[33];
    std::snprintf(buf, sizeof(buf), "%016llx%016llx",
                  static_cast<unsigned long long>(dist(rng)),
                  static_cast<unsigned long long>(dist(rng)));
    return std::string(buf);
}

void set_trace_id(const std::string& trace_id) { t_trace_id = trace_id; }
void clear_trace_id() { t_trace_id.clear(); }
std::string current_trace_id() { return t_trace_id; }

void start_request_timer() { t_request_start = std::chrono::steady_clock::now(); }
double request_duration_ms() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                       t_request_start)
        .count();
}

void set_route(const std::string& route) { t_route = route; }
std::string current_route() { return t_route; }

void set_user(const std::string& user_id) { t_user_id = user_id; }
std::string current_user_id() { return t_user_id; }

void clear_request_context() {
    t_trace_id.clear();
    t_route.clear();
    t_user_id.clear();
}

void error(const std::string& message) { write_line("error", message, nullptr); }
void warn(const std::string& message) { write_line("warn", message, nullptr); }
void info(const std::string& message) { write_line("info", message, nullptr); }
void debug(const std::string& message) { write_line("debug", message, nullptr); }

void error(const std::string& message, const nlohmann::json& details) {
    write_line("error", message, &details);
}
void warn(const std::string& message, const nlohmann::json& details) {
    write_line("warn", message, &details);
}
void info(const std::string& message, const nlohmann::json& details) {
    write_line("info", message, &details);
}
void debug(const std::string& message, const nlohmann::json& details) {
    write_line("debug", message, &details);
}

} // namespace applog
