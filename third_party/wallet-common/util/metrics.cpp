#include "util/metrics.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <mutex>
#include <sstream>
#include <tuple>

namespace metrics {
namespace {

// Границы бакетов гистограммы, секунды — тот же диапазон, что дефолтные
// бакеты client_golang (0.005..10s): от быстрого JSON-ответа до заметно
// подвисшего запроса (например, ожидание в очереди db::Pool::acquire() под
// нагрузкой, см. db.hpp). p50/p95/p99 в Grafana считаются на стороне
// Prometheus (histogram_quantile по этим бакетам) — клиенту достаточно
// отдать сами бакеты, не приближение квантиля.
constexpr std::array<double, 11> kBucketsSeconds = {0.005, 0.01, 0.025, 0.05, 0.1,
                                                     0.25,  0.5,  1,     2.5,  5, 10};

struct HistogramState {
    // bucket_counts[i] — КУМУЛЯТИВНОЕ число наблюдений с value <= kBucketsSeconds[i]
    // (ровно то, что требует формат Prometheus для *_bucket{le="..."}: не
    // счётчик "внутри этого интервала", а "не больше этого") — заполняется
    // уже кумулятивным в observe_http_request(), render() просто печатает
    // как есть.
    std::array<std::uint64_t, kBucketsSeconds.size()> bucket_counts{};
    std::uint64_t count = 0;
    double sum = 0.0;
};

struct RouteKey {
    std::string method;
    std::string route;
    bool operator<(const RouteKey& other) const {
        return std::tie(method, route) < std::tie(other.method, other.route);
    }
};

struct StatusKey {
    std::string method;
    std::string route;
    int status;
    bool operator<(const StatusKey& other) const {
        return std::tie(method, route, status) < std::tie(other.method, other.route, other.status);
    }
};

// Один общий мьютекс на оба map — запросов у нас на порядки меньше, чем на
// критичном пути было бы оправдано разбивать на несколько локов (тот же
// компромисс, что и у db::Database — один mutex_ на всё соединение).
std::mutex g_mutex;
std::map<StatusKey, std::uint64_t> g_requests_total;
std::map<RouteKey, HistogramState> g_duration;

// Заполняется ОДИН раз set_build_info() из main() — до старта svr.listen(),
// т.е. до первого запроса и до первого параллельного обращения к render().
// g_service пуст, пока set_build_info() не вызван (юнит-тесты этого модуля
// его не вызывают — тогда render() просто не печатает блок build_info, см.
// ниже).
std::string g_service;
std::string g_build_version;
std::string g_git_commit;

struct CounterFamily {
    std::string help;
    std::string label;
    std::map<std::string, std::uint64_t> values; // label_value -> count
};
std::map<std::string, CounterFamily> g_counters;

struct Gauge {
    std::string help;
    double value = 0.0;
};
std::map<std::string, Gauge> g_gauges;

struct HistogramFamily {
    std::string help;
    std::string label;
    std::map<std::string, HistogramState> values; // label_value -> гистограмма
};
std::map<std::string, HistogramFamily> g_histograms;

void observe(HistogramState& h, double seconds) {
    h.count++;
    h.sum += seconds;
    for (std::size_t i = 0; i < kBucketsSeconds.size(); ++i) {
        if (seconds <= kBucketsSeconds[i]) h.bucket_counts[i]++;
    }
}

// Единственные спецсимволы, значимые в значении лейбла текстового формата
// экспозиции — обратный слэш и двойная кавычка. Перевод строки в
// method/route не встречается — это литералы из кода (HTTP-метод и
// applog::current_route()), не пользовательский ввод.
std::string escape_label(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        if (c == '\\' || c == '"') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

}  // namespace

void observe_http_request(const std::string& method, const std::string& route, int status,
                           double duration_ms) {
    double seconds = duration_ms / 1000.0;
    std::lock_guard<std::mutex> lock(g_mutex);

    g_requests_total[{method, route, status}]++;

    observe(g_duration[{method, route}], seconds);
}

void observe_duration(const std::string& name, const std::string& help, const std::string& label,
                      const std::string& label_value, double seconds) {
    std::lock_guard<std::mutex> lock(g_mutex);
    HistogramFamily& f = g_histograms[name];
    f.help = help;
    f.label = label;
    observe(f.values[label_value], seconds);
}

void set_build_info(const std::string& service, const std::string& version,
                     const std::string& git_commit) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_service = service;
    g_build_version = version;
    g_git_commit = git_commit;
}

void inc_counter(const std::string& name, const std::string& help, const std::string& label,
                 const std::string& label_value) {
    std::lock_guard<std::mutex> lock(g_mutex);
    CounterFamily& f = g_counters[name];
    f.help = help;
    f.label = label;
    f.values[label_value]++;
}

void set_gauge(const std::string& name, const std::string& help, double value) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_gauges[name] = Gauge{help, value};
}

std::string render() {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::ostringstream out;

    for (const auto& [name, f] : g_counters) {
        out << "# HELP " << name << " " << f.help << "\n";
        out << "# TYPE " << name << " counter\n";
        for (const auto& [value, count] : f.values) {
            out << name << "{" << f.label << "=\"" << escape_label(value) << "\"} " << count << "\n";
        }
    }
    for (const auto& [name, f] : g_histograms) {
        out << "# HELP " << name << " " << f.help << "\n";
        out << "# TYPE " << name << " histogram\n";
        for (const auto& [value, h] : f.values) {
            std::string labels = f.label + "=\"" + escape_label(value) + "\"";
            for (std::size_t i = 0; i < kBucketsSeconds.size(); ++i) {
                out << name << "_bucket{" << labels << ",le=\"" << kBucketsSeconds[i] << "\"} " << h.bucket_counts[i]
                    << "\n";
            }
            out << name << "_bucket{" << labels << ",le=\"+Inf\"} " << h.count << "\n";
            out << name << "_sum{" << labels << "} " << h.sum << "\n";
            out << name << "_count{" << labels << "} " << h.count << "\n";
        }
    }
    for (const auto& [name, g] : g_gauges) {
        out << "# HELP " << name << " " << g.help << "\n";
        out << "# TYPE " << name << " gauge\n";
        out << name << " " << std::fixed << g.value << "\n";
        out.unsetf(std::ios::floatfield);
    }

    // build_info — тот же приём, что kube-state-metrics/большинство
    // Prometheus-клиентов: не отдельная метрика на каждое поле, а один
    // gauge==1 с версией/коммитом в лейблах — Grafana строит по нему таблицу
    // "какой билд сейчас где крутится" (join по service), см. GET /version
    // (docs.cpp) — та же тройка полей, для человека и curl без PromQL.
    if (!g_service.empty()) {
        out << "# HELP build_info Информация о собранной версии сервиса (см. GET /version).\n";
        out << "# TYPE build_info gauge\n";
        out << "build_info{service=\"" << escape_label(g_service) << "\",version=\""
            << escape_label(g_build_version) << "\",git_commit=\"" << escape_label(g_git_commit)
            << "\"} 1\n";
    }

    out << "# HELP http_requests_total Total number of HTTP requests processed.\n";
    out << "# TYPE http_requests_total counter\n";
    for (const auto& [key, value] : g_requests_total) {
        out << "http_requests_total{method=\"" << escape_label(key.method) << "\",route=\""
            << escape_label(key.route) << "\",status=\"" << key.status << "\"} " << value << "\n";
    }

    out << "# HELP http_request_duration_seconds HTTP request latency in seconds — весь запрос "
           "целиком (routing + хендлер + все SQL внутри), см. applog::request_duration_ms().\n";
    out << "# TYPE http_request_duration_seconds histogram\n";
    for (const auto& [key, h] : g_duration) {
        for (std::size_t i = 0; i < kBucketsSeconds.size(); ++i) {
            out << "http_request_duration_seconds_bucket{method=\"" << escape_label(key.method)
                << "\",route=\"" << escape_label(key.route) << "\",le=\"" << kBucketsSeconds[i]
                << "\"} " << h.bucket_counts[i] << "\n";
        }
        out << "http_request_duration_seconds_bucket{method=\"" << escape_label(key.method)
            << "\",route=\"" << escape_label(key.route) << "\",le=\"+Inf\"} " << h.count << "\n";
        out << "http_request_duration_seconds_sum{method=\"" << escape_label(key.method)
            << "\",route=\"" << escape_label(key.route) << "\"} " << h.sum << "\n";
        out << "http_request_duration_seconds_count{method=\"" << escape_label(key.method)
            << "\",route=\"" << escape_label(key.route) << "\"} " << h.count << "\n";
    }

    return out.str();
}

}  // namespace metrics
