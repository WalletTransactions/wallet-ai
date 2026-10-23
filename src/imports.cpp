#include "imports.hpp"

#include <algorithm>
#include <cstdio>

#include "expense/resolver.hpp"
#include "statement/chunk.hpp"
#include "statement/pattern.hpp"
#include "statement/source.hpp"
#include "statement/textutil.hpp"
#include "statement/verify.hpp"
#include "util/log.hpp"
#include "util/metrics.hpp"
#include "util/uuid.hpp"

using nlohmann::json;

namespace imports {

namespace {

constexpr stmt::Epoch kDay = 86400;

Reply error(int status, const std::string& code) { return {status, {{"error", code}}}; }

std::string iso_date(stmt::Epoch t) {
    long z = static_cast<long>(t / kDay) + 719468;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    long doe = z - era * 146097;
    long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long mp = (5 * doy + 2) / 153;
    int d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    int m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    int y = static_cast<int>(yoe + era * 400 + (m <= 2 ? 1 : 0));
    return stmt::to_iso(y, m, d);
}

std::string iso_time(stmt::Epoch t) {
    char buf[16];
    stmt::Epoch s = ((t % kDay) + kDay) % kDay;
    std::snprintf(buf, sizeof(buf), "T%02lld:%02lld:%02lldZ", s / 3600, (s / 60) % 60, s % 60);
    return iso_date(t) + buf;
}

stmt::Epoch epoch_of(const std::string& iso) {
    stmt::Date d;
    if (!stmt::parse_iso(iso, d)) return 0;
    return static_cast<stmt::Epoch>(stmt::days_from_civil(d.y, d.m, d.d)) * kDay;
}

bool active_status(const std::string& s) { return s == "queued" || s == "extracting" || s == "classifying"; }

std::string wallet_currency(const json& accounts, const std::string& wallet_uuid) {
    for (const auto& w : resolver::parse_accounts(accounts).wallets) {
        if (w.uuid == wallet_uuid) return w.iso_code;
    }
    return {};
}

bool known_category(const json& accounts, const std::string& uuid) {
    for (const auto& c : resolver::parse_accounts(accounts).categories) {
        if (c.uuid == uuid) return true;
    }
    return false;
}

// Строка закрыта: записана или её не пишут (пропущена, дубль, доход).
bool closed(const stmt::RowRec& r) {
    if (r.committed) return true;
    if (r.decision == "skip" || r.decision == "duplicate") return true;
    std::string g = group_of(r);
    return g == "exists" || g == "imported" || g == "income_transfer";
}

std::string trim_comment(const std::string& s) { return stmt::utf8_prefix(stmt::trim(s), 255); }

void count_import(const std::string& status) {
    metrics::inc_counter("ai_statement_imports_total", "Импорты выписок по итоговому статусу.", "status", status);
}

} // namespace

std::string group_of(const stmt::RowRec& r) {
    if (r.dir != "expense") return "income_transfer";
    if (!r.check.empty()) return "check";
    if (r.decision == "duplicate") return "exists";
    if (r.decision != "include") {
        if (r.match == "imported") return "imported";
        if (r.match == "exists") return "exists";
        if (r.match == "possible_duplicate") return "possible_duplicate";
    }
    if (r.category_uuid.empty()) return "needs_category";
    return "ready";
}

void assign_fingerprints(std::vector<stmt::RowRec>& rows, const std::string& wallet_uuid) {
    std::map<std::string, int> seen;
    for (auto& r : rows) {
        std::string key = r.date + "|" + r.amount + "|" + r.dir + "|" + r.pattern;
        int k = seen[key]++;
        if (r.committed) continue; // записанная строка держит свой uuid
        r.fingerprint = stmt::fingerprint(wallet_uuid, r.date, r.amount, r.dir, r.pattern, k);
        r.op_uuid = stmt::op_uuid(r.fingerprint);
    }
}

void reconcile(std::vector<stmt::RowRec>& rows, const std::vector<WalletOp>& ops,
               const std::set<std::string>& imported_fps) {
    std::set<std::string> own; // операции, записанные из этого импорта
    for (const auto& r : rows) {
        if (r.committed) own.insert(r.op_uuid);
    }
    std::vector<bool> used(ops.size(), false);
    for (std::size_t i = 0; i < ops.size(); ++i) used[i] = own.count(ops[i].uuid) != 0;

    std::vector<stmt::RowRec*> order;
    for (auto& r : rows) {
        if (r.committed) continue;
        r.match.clear();
        r.match_candidates = json();
        if (imported_fps.count(r.fingerprint) != 0) {
            r.match = "imported";
            continue;
        }
        if (r.dir == "expense") order.push_back(&r);
    }
    std::stable_sort(order.begin(), order.end(), [](const stmt::RowRec* a, const stmt::RowRec* b) {
        return a->date < b->date;
    });
    for (auto* r : order) {
        auto cents = stmt::parse_amount(r->amount);
        if (!cents || r->date.empty()) continue;
        stmt::Epoch day = epoch_of(r->date);
        std::vector<std::size_t> cand;
        for (std::size_t i = 0; i < ops.size(); ++i) {
            if (used[i] || ops[i].cents != *cents) continue;
            stmt::Epoch diff = epoch_of(ops[i].date) - day;
            if (diff >= -kDay && diff <= kDay) cand.push_back(i);
        }
        if (cand.size() == 1) {
            r->match = "exists";
            r->match_candidates = json::array({{{"uuid", ops[cand[0]].uuid}, {"date", ops[cand[0]].date}}});
            used[cand[0]] = true;
        } else if (cand.size() > 1) {
            r->match = "possible_duplicate";
            r->match_candidates = json::array();
            for (auto i : cand) r->match_candidates.push_back({{"uuid", ops[i].uuid}, {"date", ops[i].date}});
        }
    }
}

Service::Service(stmt::Repo& repo, reports::ReportsApi& reports, std::vector<stmt::Profile> profiles, Config cfg,
                 NowFn now)
    : repo_(repo), reports_(reports), profiles_(std::move(profiles)), cfg_(cfg), now_(std::move(now)) {}

Reply Service::create(const User& u, const json& body) {
    if (!body.is_object()) return error(400, "bad_request");
    bool has_text = body.contains("text") && !body["text"].is_null();
    bool has_file = body.contains("file") && !body["file"].is_null();
    if (has_text && has_file) return error(400, "one_source");
    if (!has_text && !has_file) return error(400, "empty");
    std::string wallet_uuid = body.value("wallet_uuid", std::string{});

    // Профиль: auto (по умолчанию), none, id встроенного или своего формата.
    std::string profile = body.contains("profile") && body["profile"].is_string() ? body["profile"].get<std::string>()
                                                                                    : "auto";
    if (profile != "auto" && profile != "none" && stmt::find_profile(profiles_, profile) == nullptr &&
        !repo_.user_profile(profile, u.user_id)) {
        return error(400, "unknown_profile");
    }

    // Один импорт в работе на пользователя, AI_IMPORTS_MAX_ACTIVE всего.
    auto active = repo_.active_imports();
    for (const auto& imp : active) {
        if (imp.user_id == u.user_id) return {429, {{"error", "import_in_progress"}, {"import_id", imp.id}}};
    }
    if (static_cast<int>(active.size()) >= cfg_.max_active) return error(429, "busy");

    // Кошелёк — из справочника пользователя на этот момент (его токеном).
    auto accounts = reports_.get("/api/reports/accounts", u.token);
    if (accounts.status == 401 || accounts.status == 403) return error(401, "wallet-report не принял токен");
    if (accounts.status != 200) return error(502, "wallet-report недоступен");
    std::string currency = wallet_currency(accounts.body, wallet_uuid);
    if (wallet_uuid.empty() || currency.empty()) return error(400, "unknown_wallet");

    stmt::ImportRec imp;
    imp.id = uuidgen::v4();
    imp.user_id = u.user_id;
    imp.wallet_uuid = wallet_uuid;
    imp.accounts = accounts.body;
    imp.created_at = now_();
    imp.expires_at = imp.created_at + cfg_.ttl_days * kDay;
    imp.status = "queued";

    std::string text;
    bool csv = false;
    if (has_text) {
        if (!body["text"].is_string()) return error(400, "empty");
        text = body["text"].get<std::string>();
        imp.source_name = body.value("source_name", std::string{});
    } else {
        const json& file = body["file"];
        if (!file.is_object() || !file.contains("name") || !file["name"].is_string() ||
            !file.contains("content_base64") || !file["content_base64"].is_string()) {
            return error(400, "unsupported_file");
        }
        imp.source_name = stmt::utf8_prefix(file["name"].get<std::string>(), 200);
        auto src = stmt::extract_file(imp.source_name, file["content_base64"].get<std::string>(), cfg_.file_max_bytes);
        if (src.error == "unsupported_file" || src.error == "bad_base64") return error(400, src.error);
        if (src.error == "too_large") return error(413, "too_large");
        if (src.error == "no_text" || src.error == "pdf_failed") {
            // Скан или битый PDF: импорт сразу failed, чтобы клиент показал причину.
            if (src.error == "pdf_failed") {
                applog::warn("pdftotext failed", {{"event", "pdf_failed"}, {"import_id", imp.id}});
            }
            imp.status = "failed";
            imp.failure = "no_text";
            repo_.create_import(imp, {});
            count_import("failed");
            return {202, {{"import_id", imp.id}, {"status", imp.status}, {"failure", imp.failure}}};
        }
        text = src.text;
        csv = src.csv;
    }

    if (stmt::trim(stmt::normalize_spaces(text)).empty()) return error(400, "empty");
    if (stmt::utf8_length(text) > cfg_.max_chars) return error(413, "too_large");
    imp.source_text = text;
    auto lines = stmt::split_lines(text);

    auto period = stmt::find_period(lines);
    imp.period_from = period.from;
    imp.period_to = period.to;
    imp.format_fingerprint = stmt::format_fingerprint(lines, csv);

    // Профиль: ручной выбор или автоопределение (statement-profiles).
    if (profile == "none") {
        imp.profile_source = "manual";
    } else if (profile != "auto") {
        imp.profile_id = profile;
        imp.profile_source = "manual";
    } else {
        auto builtin = stmt::matching_profiles(profiles_, lines);
        std::optional<stmt::UserProfileRec> own;
        if (!imp.format_fingerprint.empty()) own = repo_.profile_by_fingerprint(u.user_id, imp.format_fingerprint);
        if (builtin.size() == 1) {
            imp.profile_id = builtin[0];
        } else if (builtin.empty() && own) {
            imp.profile_id = own->id;
        } else {
            imp.profile_detect = builtin.empty() ? "none" : "ambiguous";
        }
        if (!imp.profile_id.empty()) imp.profile_source = "auto";
    }
    const stmt::Profile* builtin = stmt::find_profile(profiles_, imp.profile_id);
    if (builtin != nullptr) imp.profile_version = builtin->version;

    // CSV известного формата — по колонкам профиля, без заданий LLM (D11):
    // строки сразу в базу, планировщик переходит к категориям.
    std::optional<std::vector<stmt::ColumnRow>> by_columns;
    if (csv && builtin != nullptr) by_columns = stmt::parse_by_columns(*builtin, lines);
    if (by_columns) {
        imp.status = "extracting";
        imp.progress_total = 0;
        repo_.create_import(imp, {});
        stmt::VerifyContext ctx;
        ctx.lines = &lines;
        ctx.chunk_first = 1;
        ctx.chunk_last = static_cast<int>(lines.size());
        ctx.today = iso_date(imp.created_at);
        ctx.period = period;
        ctx.wallet_currency = currency;
        ctx.rules = builtin->check;
        int n = 0;
        for (const auto& cr : *by_columns) {
            auto v = stmt::verify_row(cr.candidate, ctx);
            if (!v) continue;
            stmt::RowRec r;
            r.id = uuidgen::v4();
            r.n = ++n;
            r.lines = v->lines;
            r.date = v->date;
            r.amount = v->amount;
            r.dir = v->dir;
            r.currency = v->currency;
            r.description = v->description;
            r.pattern = stmt::pattern_of(v->description);
            r.check = v->check;
            r.classified = r.dir != "expense";
            // Окончательные отпечатки — в finish_extraction, как у строк модели.
            r.fingerprint = stmt::fingerprint(imp.wallet_uuid, r.date, r.amount, r.dir, r.pattern, 0);
            r.op_uuid = stmt::op_uuid(r.fingerprint);
            repo_.insert_row(imp.id, r);
        }
        applog::info("import created", {{"event", "import_created"},
                                        {"import_id", imp.id},
                                        {"chunks", 0},
                                        {"lines", lines.size()},
                                        {"by_columns", true},
                                        {"profile", imp.profile_id},
                                        {"profile_detect", imp.profile_detect}});
        if (on_change) on_change();
        return {202, {{"import_id", imp.id}, {"status", imp.status}}};
    }

    // Профиль съедает часть контекста — фрагмент меньше на тот же бюджет.
    std::size_t profile_tokens = stmt::estimate_tokens(profile_prompt_for(imp));
    std::size_t chunk_chars = cfg_.chunk_chars > profile_tokens * 3 + cfg_.min_chunk_chars
                                  ? cfg_.chunk_chars - profile_tokens * 3
                                  : cfg_.min_chunk_chars;
    auto plans = stmt::plan_chunks(lines, chunk_chars);
    if (static_cast<int>(plans.size()) > cfg_.max_chunks) return error(413, "too_large");

    std::vector<stmt::ChunkRec> chunks;
    for (std::size_t i = 0; i < plans.size(); ++i) {
        stmt::ChunkRec c;
        c.n = static_cast<int>(i) + 1;
        c.first_line = plans[i].first_line;
        c.last_line = plans[i].last_line;
        c.date_context = plans[i].date_context;
        chunks.push_back(c);
    }
    imp.progress_total = static_cast<int>(chunks.size());
    repo_.create_import(imp, chunks);
    applog::info("import created", {{"event", "import_created"},
                                    {"import_id", imp.id},
                                    {"chunks", chunks.size()},
                                    {"lines", lines.size()},
                                    {"profile", imp.profile_id},
                                    {"profile_detect", imp.profile_detect}});
    if (on_change) on_change();
    return {202, {{"import_id", imp.id}, {"status", imp.status}}};
}

Reply Service::list(const User& u) {
    json items = json::array();
    for (const auto& imp : repo_.list_imports(u.user_id)) items.push_back(view(imp, nullptr, false));
    return {200, {{"imports", items}}};
}

Reply Service::get(const User& u, const std::string& id, bool force_reconcile) {
    auto imp = repo_.get_import(id, u.user_id);
    if (!imp) return error(404, "импорта нет или он истёк");
    if (imp->status != "ready" && imp->status != "done") return {200, view(*imp, nullptr, false)};

    auto rows = repo_.rows(imp->id);
    bool reconciled = true;
    stmt::Epoch t = now_();
    bool stale = !imp->reconciled_at || t - *imp->reconciled_at >= cfg_.reconcile_cache_secs;
    if (imp->status == "ready" && (stale || force_reconcile)) {
        std::vector<WalletOp> ops;
        if (fetch_ops(u, *imp, rows, ops)) {
            std::vector<std::string> fps;
            for (const auto& r : rows) fps.push_back(r.fingerprint);
            reconcile(rows, ops, repo_.imported(u.user_id, fps));
            repo_.save_rows(imp->id, rows);
            imp->reconciled_at = t;
            repo_.save_import(*imp);
        } else {
            reconciled = false;
        }
    }
    return {200, view(*imp, &rows, reconciled)};
}

bool Service::fetch_ops(const User& u, const stmt::ImportRec& imp, const std::vector<stmt::RowRec>& rows,
                        std::vector<WalletOp>& out) {
    std::string from = imp.period_from;
    std::string to = imp.period_to;
    for (const auto& r : rows) {
        if (r.date.empty()) continue;
        if (from.empty() || r.date < from) from = r.date;
        if (to.empty() || r.date > to) to = r.date;
    }
    if (from.empty()) return true; // нечего сверять
    std::string eq = wallet_currency(imp.accounts, imp.wallet_uuid);
    std::string q = "/api/reports/operations?from=" + reports::url_encode(iso_date(epoch_of(from) - 2 * kDay) + " 00:00:00") +
                    "&to=" + reports::url_encode(iso_date(epoch_of(to) + 2 * kDay) + " 23:59:59") +
                    "&eq_currency=" + reports::url_encode(eq) + "&limit=1000";
    std::string cursor;
    for (int page = 0; page < 50; ++page) {
        auto res = reports_.get(q + cursor, u.token);
        if (res.status != 200 || !res.body.contains("operations")) {
            applog::warn("reconcile failed", {{"event", "reconcile_failed"}, {"import_id", imp.id},
                                              {"status", res.status}});
            return false;
        }
        for (const auto& op : res.body["operations"]) {
            if (op.value("kind", "") != "expense" || op.value("wallet_uuid", "") != imp.wallet_uuid) continue;
            auto cents = stmt::parse_amount(op.value("amount", ""));
            std::string date = op.value("date", "").substr(0, 10);
            if (cents) out.push_back({op.value("operation_uuid", ""), date, *cents});
        }
        if (!res.body.value("has_more", false)) return true;
        cursor = "&cursor_date=" + reports::url_encode(res.body.value("next_cursor_date", "")) +
                 "&cursor_uuid=" + reports::url_encode(res.body.value("next_cursor_uuid", ""));
    }
    return true;
}

json Service::profile_json(const stmt::ImportRec& imp) {
    if (imp.profile_id.empty()) return nullptr;
    std::string name;
    if (const auto* p = stmt::find_profile(profiles_, imp.profile_id)) {
        name = p->name;
    } else if (auto own = repo_.user_profile(imp.profile_id, imp.user_id)) {
        name = own->name;
    } else {
        return nullptr; // свой формат удалили
    }
    return {{"id", imp.profile_id}, {"name", name}, {"source", imp.profile_source}};
}

json Service::view(const stmt::ImportRec& imp, const std::vector<stmt::RowRec>* rows, bool reconciled) {
    json j = {{"import_id", imp.id},
              {"status", imp.status},
              {"failure", imp.failure.empty() ? json(nullptr) : json(imp.failure)},
              {"wallet_uuid", imp.wallet_uuid},
              {"source_name", imp.source_name},
              {"created_at", iso_time(imp.created_at)},
              {"expires_at", iso_time(imp.expires_at)},
              {"period", imp.period_from.empty() ? json(nullptr)
                                                 : json{{"from", imp.period_from}, {"to", imp.period_to}}},
              {"profile", profile_json(imp)},
              {"profile_detect", imp.profile_detect.empty() ? json(nullptr) : json(imp.profile_detect)}};
    if (imp.status == "extracting" || imp.status == "classifying") {
        j["progress"] = {{"done", imp.progress_done}, {"total", imp.progress_total}};
    }
    if (imp.paused_since) j["paused_since"] = iso_time(*imp.paused_since);
    if (rows == nullptr) return j;

    auto lines = stmt::split_lines(imp.source_text);
    auto source = [&](const std::vector<int>& ns) {
        json out = json::array();
        for (int n : ns) {
            if (n >= 1 && n <= static_cast<int>(lines.size())) out.push_back({{"n", n}, {"text", lines[n - 1]}});
        }
        return out;
    };
    std::map<std::string, int> groups = {{"ready", 0},     {"needs_category", 0}, {"possible_duplicate", 0},
                                         {"check", 0},     {"exists", 0},         {"imported", 0},
                                         {"income_transfer", 0}, {"unparsed", 0}};
    json items = json::array();
    json operations = json::array();
    std::set<int> covered;
    for (const auto& r : *rows) {
        std::string g = group_of(r);
        ++groups[g];
        if (r.check.empty()) covered.insert(r.lines.begin(), r.lines.end());
        items.push_back({{"row_id", r.id},
                         {"group", g},
                         {"lines", source(r.lines)},
                         {"date", r.date},
                         {"amount", r.amount},
                         {"dir", r.dir},
                         {"currency", r.currency},
                         {"description", r.description},
                         {"pattern", r.pattern},
                         {"comment", r.comment ? json(*r.comment) : json(nullptr)},
                         {"category_uuid", r.category_uuid.empty() ? json(nullptr) : json(r.category_uuid)},
                         {"source", r.origin == "manual"          ? json("manual")
                                    : r.category_source.empty() ? json(nullptr)
                                                                : json(r.category_source)},
                         {"suggestion", r.suggestion.empty() ? json(nullptr) : json(r.suggestion)},
                         {"check", r.check.empty() ? json(nullptr) : json(r.check)},
                         {"match", r.match.empty() ? json(nullptr) : json(r.match)},
                         {"match_candidates", r.match_candidates},
                         {"decision", r.decision.empty() ? json(nullptr) : json(r.decision)},
                         {"edited", r.edited},
                         {"committed", r.committed}});
        if (g == "ready" && r.decision != "skip" && !r.committed) {
            operations.push_back({{"uuid", r.op_uuid},
                                  {"row_id", r.id},
                                  {"date", r.date + " 12:00:00"},
                                  {"acc_fr_uuid", imp.wallet_uuid},
                                  {"acc_to_uuid", r.category_uuid},
                                  {"amount", r.amount},
                                  {"comment", trim_comment(r.comment ? *r.comment : r.description)}});
        }
    }
    json unparsed = json::array();
    auto rules = check_rules_for(imp);
    for (int n : stmt::uncovered_lines(lines, 1, static_cast<int>(lines.size()), covered, rules.stop_words)) {
        unparsed.push_back({{"n", n}, {"text", lines[n - 1]}});
    }
    groups["unparsed"] = static_cast<int>(unparsed.size());
    j["groups"] = groups;
    j["rows"] = items;
    j["unparsed"] = unparsed;
    j["operations"] = operations;
    j["reconciled"] = reconciled;
    return j;
}

Reply Service::remove(const User& u, const std::string& id) {
    auto imp = repo_.get_import(id, u.user_id);
    if (!imp) return error(404, "импорта нет или он истёк");
    if (on_delete) on_delete(imp->id);
    repo_.delete_import(imp->id);
    if (active_status(imp->status)) count_import("cancelled");
    applog::info("import deleted", {{"event", "import_deleted"}, {"import_id", imp->id}, {"status", imp->status}});
    return {204, nullptr};
}

Reply Service::retry(const User& u, const std::string& id) {
    auto imp = repo_.get_import(id, u.user_id);
    if (!imp) return error(404, "импорта нет или он истёк");
    if (imp->status != "failed" || imp->failure == "no_text") return error(409, "not_retryable");
    for (const auto& other : repo_.active_imports()) {
        if (other.user_id == u.user_id) return {429, {{"error", "import_in_progress"}, {"import_id", other.id}}};
    }
    for (auto c : repo_.chunks(imp->id)) {
        if (c.status == "pending" && c.attempts > 0) {
            c.attempts = 0;
            repo_.save_chunk(imp->id, c);
        }
    }
    imp->status = "extracting"; // планировщик сам перейдёт дальше, если фрагменты разобраны
    imp->failure.clear();
    imp->paused_since.reset();
    repo_.save_import(*imp);
    if (on_change) on_change();
    return {202, {{"import_id", imp->id}, {"status", imp->status}}};
}

void Service::learn_format(const User& u, const stmt::ImportRec& imp, const stmt::RowRec& row) {
    if (imp.format_fingerprint.empty()) return;
    auto lines = stmt::split_lines(imp.source_text);
    std::string source;
    for (int n : row.lines) {
        if (n >= 1 && n <= static_cast<int>(lines.size())) source += lines[n - 1] + "\n";
    }
    if (source.empty()) return;
    auto profile = repo_.profile_by_fingerprint(u.user_id, imp.format_fingerprint);
    if (!profile) {
        std::string name = imp.source_name.empty() ? "Мой формат" : imp.source_name;
        profile = repo_.create_profile(u.user_id, imp.format_fingerprint, name);
    }
    json expected = {{"date", row.date}, {"amount", row.amount}, {"dir", row.dir}, {"description", row.description}};
    if (!row.currency.empty()) expected["currency"] = row.currency;
    repo_.add_example(profile->id, {source, expected}, cfg_.examples_per_format);
}

void Service::maybe_done(stmt::ImportRec& imp, const std::vector<stmt::RowRec>& rows) {
    if (imp.status != "ready") return;
    for (const auto& r : rows) {
        if (!closed(r)) return;
    }
    imp.status = "done";
    imp.done_at = now_();
    imp.expires_at = std::min(*imp.done_at + cfg_.done_ttl_days * kDay, imp.created_at + cfg_.ttl_days * kDay);
    count_import("done");
}

Reply Service::patch_rows(const User& u, const std::string& id, const json& body) {
    auto imp = repo_.get_import(id, u.user_id);
    if (!imp) return error(404, "импорта нет или он истёк");
    if (imp->status != "ready") return error(409, "not_ready");
    if (!body.is_object() || !body.contains("rows") || !body["rows"].is_array()) return error(400, "bad_request");
    auto rows = repo_.rows(imp->id);
    std::string today = iso_date(now_());

    // Сначала проверка всей пачки — правки применяются все или ни одной.
    for (const auto& p : body["rows"]) {
        if (!p.is_object() || !p.contains("row_id") || !p["row_id"].is_string()) return error(400, "bad_request");
        auto it = std::find_if(rows.begin(), rows.end(), [&](const stmt::RowRec& r) { return r.id == p["row_id"]; });
        if (it == rows.end()) return error(400, "unknown_row");
        if (it->committed) return error(409, "row_committed");
        if (p.contains("category_uuid") && !p["category_uuid"].is_null() &&
            (!p["category_uuid"].is_string() || !known_category(imp->accounts, p["category_uuid"]))) {
            return error(400, "unknown_category");
        }
        if (p.contains("amount")) {
            auto cents = p["amount"].is_string() ? stmt::parse_amount(p["amount"]) : std::nullopt;
            if (!cents || *cents <= 0) return error(400, "bad_amount");
        }
        if (p.contains("date")) {
            stmt::Date d;
            if (!p["date"].is_string() || !stmt::parse_iso(p["date"], d) || p["date"].get<std::string>() > today) {
                return error(400, "bad_date");
            }
        }
        if (p.contains("dir") && (!p["dir"].is_string() || (p["dir"] != "expense" && p["dir"] != "income" &&
                                                             p["dir"] != "transfer"))) {
            return error(400, "bad_dir");
        }
        if (p.contains("decision") && !p["decision"].is_null() &&
            (!p["decision"].is_string() ||
             (p["decision"] != "include" && p["decision"] != "skip" && p["decision"] != "duplicate"))) {
            return error(400, "bad_decision");
        }
        if (p.contains("comment") && !p["comment"].is_null() && !p["comment"].is_string()) {
            return error(400, "bad_comment");
        }
    }

    for (const auto& p : body["rows"]) {
        auto& row = *std::find_if(rows.begin(), rows.end(), [&](const stmt::RowRec& r) { return r.id == p["row_id"]; });
        bool format_edit = false;
        if (p.contains("amount")) {
            std::string amount = stmt::format_cents(*stmt::parse_amount(p["amount"]));
            format_edit = format_edit || amount != row.amount;
            row.amount = amount;
            if (row.check == "amount_not_in_text" || row.check == "currency_mismatch") row.check.clear();
            row.currency.clear(); // сумма теперь в валюте кошелька
            row.edited = true;
        }
        if (p.contains("date")) {
            format_edit = format_edit || p["date"] != row.date;
            row.date = p["date"];
            if (row.check == "date_not_in_text") row.check.clear();
            row.edited = true;
        }
        if (p.contains("dir")) {
            format_edit = format_edit || p["dir"] != row.dir;
            row.dir = p["dir"];
            row.edited = true;
        }
        if (p.contains("comment")) row.comment = p["comment"].is_null() ? std::nullopt
                                                                        : std::optional<std::string>(trim_comment(p["comment"]));
        if (p.contains("decision")) row.decision = p["decision"].is_null() ? "" : p["decision"].get<std::string>();
        if (p.contains("category_uuid")) {
            row.category_uuid = p["category_uuid"].is_null() ? "" : p["category_uuid"].get<std::string>();
            row.category_source = row.category_uuid.empty() ? "" : "user";
            // Правило продавца и соседние строки с тем же pattern (statement-categorization).
            if (!row.category_uuid.empty() && !row.pattern.empty()) {
                repo_.upsert_rule(u.user_id, row.pattern, row.category_uuid);
                for (auto& other : rows) {
                    if (&other == &row || other.committed || other.pattern != row.pattern ||
                        other.category_source == "user") {
                        continue;
                    }
                    other.category_uuid = row.category_uuid;
                    other.category_source = "rule";
                }
            }
        }
        if (format_edit) learn_format(u, *imp, row);
    }
    assign_fingerprints(rows, imp->wallet_uuid);
    repo_.save_rows(imp->id, rows);
    maybe_done(*imp, rows);
    repo_.save_import(*imp);
    return {200, view(*imp, &rows, true)};
}

Reply Service::add_row(const User& u, const std::string& id, const json& body) {
    auto imp = repo_.get_import(id, u.user_id);
    if (!imp) return error(404, "импорта нет или он истёк");
    if (imp->status != "ready") return error(409, "not_ready");
    auto lines = stmt::split_lines(imp->source_text);
    if (!body.is_object() || !body.contains("line") || !body["line"].is_number_integer()) {
        return error(400, "bad_line");
    }
    int line = body["line"];
    if (line < 1 || line > static_cast<int>(lines.size())) return error(400, "bad_line");
    auto cents = body.contains("amount") && body["amount"].is_string() ? stmt::parse_amount(body["amount"])
                                                                       : std::nullopt;
    if (!cents || *cents <= 0) return error(400, "bad_amount");
    stmt::Date d;
    std::string date = body.value("date", std::string{});
    if (!stmt::parse_iso(date, d) || date > iso_date(now_())) return error(400, "bad_date");
    std::string category = body.value("category_uuid", std::string{});
    if (!category.empty() && !known_category(imp->accounts, category)) return error(400, "unknown_category");

    auto rows = repo_.rows(imp->id);
    stmt::RowRec r;
    r.id = uuidgen::v4();
    r.n = rows.empty() ? 1 : rows.back().n + 1;
    r.lines = {line};
    r.date = date;
    r.amount = stmt::format_cents(*cents);
    r.dir = "expense";
    r.description = stmt::trim(body.value("description", std::string{}));
    if (r.description.empty()) r.description = stmt::trim(stmt::normalize_spaces(lines[line - 1]));
    r.pattern = stmt::pattern_of(r.description);
    if (body.contains("comment") && body["comment"].is_string()) r.comment = trim_comment(body["comment"]);
    r.category_uuid = category;
    r.category_source = category.empty() ? "" : "user";
    r.classified = true;
    r.origin = "manual";
    r.edited = true;
    rows.push_back(r);
    assign_fingerprints(rows, imp->wallet_uuid);
    repo_.insert_row(imp->id, rows.back());
    if (!category.empty() && !r.pattern.empty()) repo_.upsert_rule(u.user_id, r.pattern, category);
    repo_.save_rows(imp->id, rows);
    imp->reconciled_at.reset(); // новая строка проходит ту же сверку при следующем GET
    repo_.save_import(*imp);
    return {201, view(*imp, &rows, true)};
}

Reply Service::committed(const User& u, const std::string& id, const json& body) {
    auto imp = repo_.get_import(id, u.user_id);
    if (!imp) return error(404, "импорта нет или он истёк");
    if (imp->status != "ready") return error(409, "not_ready");
    if (!body.is_object() || !body.contains("row_ids") || !body["row_ids"].is_array()) return error(400, "bad_request");
    auto rows = repo_.rows(imp->id);
    std::vector<std::string> fps;
    for (const auto& rid : body["row_ids"]) {
        auto it = std::find_if(rows.begin(), rows.end(), [&](const stmt::RowRec& r) { return r.id == rid; });
        if (it == rows.end()) return error(400, "unknown_row");
        if (it->committed) continue;
        it->committed = true;
        fps.push_back(it->fingerprint);
    }
    repo_.add_imported(u.user_id, fps);
    repo_.save_rows(imp->id, rows);
    maybe_done(*imp, rows);
    repo_.save_import(*imp);
    applog::info("import rows committed", {{"event", "import_committed"},
                                           {"import_id", imp->id},
                                           {"rows", fps.size()},
                                           {"status", imp->status}});
    return {200, view(*imp, &rows, true)};
}

Reply Service::profiles(const User& u) {
    json out = json::array();
    for (const auto& p : profiles_) out.push_back({{"id", p.id}, {"name", p.name}, {"kind", "builtin"}});
    for (const auto& p : repo_.user_profiles(u.user_id)) {
        out.push_back({{"id", p.id}, {"name", p.name}, {"kind", "own"}, {"examples", p.examples}});
    }
    return {200, {{"profiles", out}}};
}

Reply Service::patch_profile(const User& u, const std::string& id, const json& body) {
    if (stmt::find_profile(profiles_, id) != nullptr) return error(403, "builtin_profile");
    if (!body.is_object() || !body.contains("name") || !body["name"].is_string() ||
        stmt::trim(body["name"]).empty()) {
        return error(400, "bad_name");
    }
    if (!repo_.rename_profile(id, u.user_id, stmt::utf8_prefix(stmt::trim(body["name"]), 100))) {
        return error(404, "формата нет");
    }
    auto p = repo_.user_profile(id, u.user_id);
    return {200, {{"id", p->id}, {"name", p->name}, {"kind", "own"}, {"examples", p->examples}}};
}

Reply Service::delete_profile(const User& u, const std::string& id) {
    if (stmt::find_profile(profiles_, id) != nullptr) return error(403, "builtin_profile");
    if (!repo_.delete_profile(id, u.user_id)) return error(404, "формата нет");
    return {204, nullptr};
}

std::string Service::profile_prompt_for(const stmt::ImportRec& imp) {
    if (imp.profile_id.empty()) return {};
    const stmt::Profile* builtin = stmt::find_profile(profiles_, imp.profile_id);
    std::optional<stmt::UserProfileRec> own;
    if (builtin == nullptr) {
        own = repo_.user_profile(imp.profile_id, imp.user_id);
    } else if (!imp.format_fingerprint.empty()) {
        // Свой формат поверх встроенного: свои примеры раньше встроенных.
        own = repo_.profile_by_fingerprint(imp.user_id, imp.format_fingerprint);
    }
    std::vector<stmt::Example> examples;
    if (own) {
        for (const auto& e : repo_.examples(own->id)) examples.push_back({e.source_lines, e.expected});
    }
    return stmt::profile_prompt(builtin, examples, cfg_.profile_max_tokens);
}

stmt::CheckRules Service::check_rules_for(const stmt::ImportRec& imp) const {
    if (const auto* p = stmt::find_profile(profiles_, imp.profile_id)) return p->check;
    return {};
}

} // namespace imports
