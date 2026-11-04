#include "chat_metrics.hpp"

#include <array>
#include <map>
#include <mutex>
#include <sstream>

namespace chat_metrics {

namespace {

struct Family {
    std::string help;
    std::map<Labels, std::uint64_t> values;
};

constexpr std::array<int, 6> kStepBuckets = {1, 2, 3, 4, 5, 6};

std::mutex g_mu;
std::map<std::string, Family> g_counters;
std::array<std::uint64_t, kStepBuckets.size()> g_step_buckets{};
std::uint64_t g_step_count = 0;
std::uint64_t g_step_sum = 0;

std::string escape(const std::string& v) {
    std::string out;
    for (char c : v) {
        if (c == '\\' || c == '"') out.push_back('\\');
        if (c == '\n') {
            out += "\\n";
            continue;
        }
        out.push_back(c);
    }
    return out;
}

std::string labels_text(const Labels& labels) {
    std::string out;
    for (const auto& [k, v] : labels) {
        if (!out.empty()) out += ",";
        out += k + "=\"" + escape(v) + "\"";
    }
    return out;
}

} // namespace

void add(const std::string& name, const std::string& help, const Labels& labels, std::uint64_t value) {
    std::lock_guard<std::mutex> lock(g_mu);
    Family& f = g_counters[name];
    f.help = help;
    f.values[labels] += value;
}

void observe_steps(int steps) {
    std::lock_guard<std::mutex> lock(g_mu);
    ++g_step_count;
    g_step_sum += static_cast<std::uint64_t>(steps < 0 ? 0 : steps);
    for (std::size_t i = 0; i < kStepBuckets.size(); ++i) {
        if (steps <= kStepBuckets[i]) ++g_step_buckets[i];
    }
}

std::uint64_t value(const std::string& name, const Labels& labels) {
    std::lock_guard<std::mutex> lock(g_mu);
    auto f = g_counters.find(name);
    if (f == g_counters.end()) return 0;
    auto v = f->second.values.find(labels);
    return v == f->second.values.end() ? 0 : v->second;
}

std::string render() {
    std::lock_guard<std::mutex> lock(g_mu);
    std::ostringstream out;
    for (const auto& [name, f] : g_counters) {
        out << "# HELP " << name << " " << f.help << "\n";
        out << "# TYPE " << name << " counter\n";
        for (const auto& [labels, v] : f.values) out << name << "{" << labels_text(labels) << "} " << v << "\n";
    }
    if (g_step_count > 0) {
        out << "# HELP ai_chat_steps Вызовов модели на задание chat.\n";
        out << "# TYPE ai_chat_steps histogram\n";
        for (std::size_t i = 0; i < kStepBuckets.size(); ++i) {
            out << "ai_chat_steps_bucket{le=\"" << kStepBuckets[i] << "\"} " << g_step_buckets[i] << "\n";
        }
        out << "ai_chat_steps_bucket{le=\"+Inf\"} " << g_step_count << "\n";
        out << "ai_chat_steps_sum " << g_step_sum << "\n";
        out << "ai_chat_steps_count " << g_step_count << "\n";
    }
    return out.str();
}

} // namespace chat_metrics
