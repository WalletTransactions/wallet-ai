// Планировщик импорта выписок (add-statement-import, D5) и обработчики
// фоновых заданий extract_rows / classify_rows, которые вызывает воркер.
#include <algorithm>
#include <chrono>

#include "expense/resolver.hpp"
#include "imports.hpp"
#include "statement/chunk.hpp"
#include "statement/pattern.hpp"
#include "statement/textutil.hpp"
#include "statement/verify.hpp"
#include "util/log.hpp"
#include "util/metrics.hpp"
#include "util/uuid.hpp"

using nlohmann::json;

namespace imports {

namespace {

std::string date_of(stmt::Epoch t) {
    long z = static_cast<long>(t / 86400) + 719468;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    long doe = z - era * 146097;
    long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long mp = (5 * doy + 2) / 153;
    int d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    int m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    return stmt::to_iso(static_cast<int>(yoe + era * 400 + (m <= 2 ? 1 : 0)), m, d);
}

bool needs_category(const stmt::RowRec& r) {
    return r.dir == "expense" && r.category_uuid.empty() && !r.classified && !r.committed;
}

// Следующая пачка classify_rows: уникальные pattern без категории.
std::vector<std::string> next_batch(const std::vector<stmt::RowRec>& rows, int size) {
    std::set<std::string> uniq;
    for (const auto& r : rows) {
        if (needs_category(r) && !r.pattern.empty()) uniq.insert(r.pattern);
    }
    std::vector<std::string> out(uniq.begin(), uniq.end());
    if (static_cast<int>(out.size()) > size) out.resize(static_cast<std::size_t>(size));
    return out;
}

parse_expense::Outcome skipped(const std::string& why) {
    parse_expense::Outcome out;
    out.status = jobs::Status::Done;
    out.result = {{"outcome", "skipped"}};
    out.log = {{"outcome", "skipped"}, {"reason", why}};
    return out;
}

parse_expense::Outcome failed(const std::string& failure, json log) {
    parse_expense::Outcome out;
    out.status = jobs::Status::Failed;
    out.failure = failure;
    out.log = std::move(log);
    return out;
}

std::string last_header(const std::vector<std::string>& lines, int first, int last, const std::string& fallback) {
    for (int n = last; n >= first; --n) {
        if (stmt::is_date_header(lines[n - 1])) return stmt::trim(stmt::normalize_spaces(lines[n - 1]));
    }
    return fallback;
}

} // namespace

Runner::Runner(stmt::Repo& repo, jobs::Store& store, llm::Client& llm, Service& service,
               std::function<bool()> llm_ok, llm::CallOptions extract_opts, llm::CallOptions classify_opts)
    : repo_(repo),
      store_(store),
      llm_(llm),
      service_(service),
      llm_ok_(std::move(llm_ok)),
      extract_opts_(extract_opts),
      classify_opts_(classify_opts) {}

void Runner::tick() {
    std::vector<stmt::ImportRec> active;
    try {
        active = repo_.active_imports();
    } catch (const std::exception& e) {
        applog::warn("import runner: db unavailable", {{"event", "import_runner_db"}, {"error", e.what()}});
        return;
    }
    bool llm_ok = llm_ok_();
    stmt::Epoch now = service_.now();
    for (auto& imp : active) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (inflight_.count(imp.id) != 0) continue;
        }
        try {
            step(imp, llm_ok, now);
        } catch (const std::exception& e) {
            applog::error("import step failed", {{"event", "import_step_failed"}, {"import_id", imp.id},
                                                 {"error", e.what()}});
        }
    }
}

void Runner::step(stmt::ImportRec& imp, bool llm_ok, stmt::Epoch now) {
    const Config& cfg = service_.config();
    if (imp.status == "classifying" && !llm_ok) {
        // Разметка без LLM не нужна: строки остаются без категории (спека
        // statement-categorization, «ИИ недоступен во время разметки»).
        auto rows = repo_.rows(imp.id);
        for (auto& r : rows) {
            if (needs_category(r)) r.classified = true;
        }
        repo_.save_rows(imp.id, rows);
        finish_ready(imp);
        return;
    }
    if (!llm_ok) {
        // Пауза, а не провал: проба снова успешна — разбор продолжается.
        if (!imp.paused_since) {
            imp.paused_since = now;
            repo_.save_import(imp);
            applog::info("import paused", {{"event", "import_paused"}, {"import_id", imp.id}});
        } else if (now - *imp.paused_since >= static_cast<stmt::Epoch>(cfg.pause_max_mins) * 60) {
            fail(imp, "unavailable");
        }
        return;
    }
    if (imp.paused_since || imp.status == "queued") {
        imp.paused_since.reset();
        if (imp.status == "queued") imp.status = "extracting";
        repo_.save_import(imp);
    }
    if (imp.status == "extracting") {
        for (const auto& c : repo_.chunks(imp.id)) {
            if (c.status == "pending") {
                enqueue(imp, "extract_rows", {{"import_id", imp.id}, {"n", c.n}});
                return;
            }
        }
        finish_extraction(imp);
    }
    if (imp.status == "classifying") {
        auto batch = next_batch(repo_.rows(imp.id), cfg.classify_batch);
        if (!batch.empty()) {
            enqueue(imp, "classify_rows", {{"import_id", imp.id}, {"patterns", batch}});
            return;
        }
        finish_ready(imp);
    }
}

void Runner::enqueue(const stmt::ImportRec& imp, const std::string& kind, json input) {
    std::lock_guard<std::mutex> lock(mu_);
    std::string job_id = store_.create_internal(imp.user_id, kind, std::move(input));
    inflight_[imp.id] = {job_id, imp.user_id};
}

void Runner::finish_extraction(stmt::ImportRec& imp) {
    const Config& cfg = service_.config();
    auto rows = repo_.rows(imp.id);
    assign_fingerprints(rows, imp.wallet_uuid);
    // Правила пользователя — без LLM (statement-categorization).
    auto rules = repo_.rules(imp.user_id);
    auto accounts = resolver::parse_accounts(imp.accounts);
    std::set<std::string> live;
    for (const auto& c : accounts.categories) live.insert(c.uuid);
    std::set<std::string> patterns;
    for (auto& r : rows) {
        if (!needs_category(r)) continue;
        auto rule = rules.find(r.pattern);
        if (!r.pattern.empty() && rule != rules.end() && live.count(rule->second) != 0) {
            r.category_uuid = rule->second;
            r.category_source = "rule";
            r.classified = true;
        } else if (r.pattern.empty() || accounts.categories.empty()) {
            r.classified = true;
        } else {
            patterns.insert(r.pattern);
        }
    }
    repo_.save_rows(imp.id, rows);
    if (patterns.empty()) {
        finish_ready(imp);
        return;
    }
    imp.status = "classifying";
    imp.progress_done = 0;
    imp.progress_total = static_cast<int>((patterns.size() + cfg.classify_batch - 1) / cfg.classify_batch);
    repo_.save_import(imp);
}

void Runner::finish_ready(stmt::ImportRec& imp) {
    imp.status = "ready";
    if (!repo_.save_import(imp)) return;
    // Счётчики качества и лог — без описаний и сумм (Приватность).
    auto full = repo_.get_import(imp.id, "");
    auto rows = repo_.rows(imp.id);
    int verified = 0;
    int check_failed = 0;
    std::set<int> covered;
    for (const auto& r : rows) {
        if (r.check.empty()) {
            ++verified;
            covered.insert(r.lines.begin(), r.lines.end());
        } else {
            ++check_failed;
        }
    }
    int unparsed = 0;
    if (full) {
        auto lines = stmt::split_lines(full->source_text);
        unparsed = static_cast<int>(stmt::uncovered_lines(lines, 1, static_cast<int>(lines.size()), covered,
                                                          service_.check_rules_for(*full).stop_words)
                                        .size());
    }
    // inc_counter wallet-common прибавляет по одному.
    auto count_rows = [](const char* result, int n) {
        for (int i = 0; i < n; ++i) {
            metrics::inc_counter("ai_statement_rows_total", "Строки импортов выписок по результату проверки.",
                                 "result", result);
        }
    };
    count_rows("verified", verified);
    count_rows("check_failed", check_failed);
    count_rows("unparsed", unparsed);
    metrics::inc_counter("ai_statement_imports_total", "Импорты выписок по итоговому статусу.", "status", "ready");
    applog::info("import ready", {{"event", "import_finished"},
                                  {"import_id", imp.id},
                                  {"rows", rows.size()},
                                  {"verified", verified},
                                  {"check_failed", check_failed},
                                  {"unparsed", unparsed},
                                  {"duration_ms", (service_.now() - imp.created_at) * 1000}});
}

void Runner::fail(stmt::ImportRec& imp, const std::string& failure) {
    imp.status = "failed";
    imp.failure = failure;
    imp.paused_since.reset();
    if (!repo_.save_import(imp)) return;
    metrics::inc_counter("ai_statement_imports_total", "Импорты выписок по итоговому статусу.", "status", "failed");
    applog::warn("import failed", {{"event", "import_failed"}, {"import_id", imp.id}, {"failure", failure}});
}

void Runner::done_with(const std::string& import_id) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        inflight_.erase(import_id);
    }
    wake();
}

parse_expense::Outcome Runner::handle_extract(const jobs::Job& job) {
    std::string import_id = job.input.value("import_id", std::string{});
    try {
        auto out = extract(job);
        done_with(import_id);
        return out;
    } catch (...) {
        done_with(import_id);
        throw;
    }
}

parse_expense::Outcome Runner::handle_classify(const jobs::Job& job) {
    std::string import_id = job.input.value("import_id", std::string{});
    try {
        auto out = classify(job);
        done_with(import_id);
        return out;
    } catch (...) {
        done_with(import_id);
        throw;
    }
}

parse_expense::Outcome Runner::extract(const jobs::Job& job) {
    const Config& cfg = service_.config();
    std::string import_id = job.input.value("import_id", std::string{});
    int n = job.input.value("n", 0);
    auto imp = repo_.get_import(import_id, "");
    if (!imp || imp->status != "extracting") return skipped("not_extracting");
    auto chunks = repo_.chunks(import_id);
    auto chunk = std::find_if(chunks.begin(), chunks.end(), [&](const stmt::ChunkRec& c) { return c.n == n; });
    if (chunk == chunks.end() || chunk->status != "pending") return skipped("chunk_done");

    auto lines = stmt::split_lines(imp->source_text);
    std::string today = date_of(imp->created_at);
    std::string currency;
    for (const auto& w : resolver::parse_accounts(imp->accounts).wallets) {
        if (w.uuid == imp->wallet_uuid) currency = w.iso_code;
    }
    std::string profile = service_.profile_prompt_for(*imp);
    std::string period = imp->period_from.empty() ? "" : imp->period_from + " — " + imp->period_to;

    auto started = std::chrono::steady_clock::now();
    int tokens = 0;
    auto call = [&](int first, int last, const std::string& date_context) {
        llm::RowsRequest req;
        req.numbered_text = stmt::numbered_text(lines, first, last);
        req.date_context = date_context;
        req.today = today;
        req.period = period;
        req.wallet_currency = currency;
        req.profile = profile;
        llm::CallOptions opts = extract_opts_;
        opts.max_tokens = llm::rows_max_tokens(stmt::money_lines(lines, first, last));
        auto res = llm_.extract_rows(req, opts);
        tokens += res.completion_tokens;
        return res;
    };

    auto res = call(chunk->first_line, chunk->last_line, chunk->date_context);
    std::vector<llm::ExtractedRow> got;
    bool gave_up = false;
    json log = {{"import_id", import_id}, {"chunk", n}, {"llm_status", llm::to_string(res.status)}};
    if (!res.detail.empty()) log["llm_detail"] = res.detail;
    if (res.status == llm::Status::Ok) {
        got = res.rows;
    } else if (res.status == llm::Status::BadResponse) {
        // Битый или оборванный ответ: фрагмент пополам, по разу; не вышло —
        // его строки с суммами уйдут в «не разобрано» (D4).
        int mid = (chunk->first_line + chunk->last_line) / 2;
        auto a = call(chunk->first_line, mid, chunk->date_context);
        auto b = call(mid + 1, chunk->last_line, last_header(lines, chunk->first_line, mid, chunk->date_context));
        for (const auto* half : {&a, &b}) {
            if (half->status == llm::Status::Ok) got.insert(got.end(), half->rows.begin(), half->rows.end());
        }
        gave_up = a.status != llm::Status::Ok && b.status != llm::Status::Ok;
        log["split"] = true;
    } else {
        // Таймаут или LLM недоступна. Пока проба неуспешна — не считаем:
        // планировщик стоит на паузе и повторит фрагмент позже.
        if (llm_ok_()) {
            chunk->attempts += 1;
            repo_.save_chunk(import_id, *chunk);
            if (chunk->attempts >= cfg.chunk_max_failures) {
                fail(*imp, res.status == llm::Status::Timeout ? "timeout" : "unavailable");
            }
        }
        log["attempts"] = chunk->attempts;
        return failed(res.status == llm::Status::Timeout ? "timeout" : "unavailable", log);
    }

    // Проверка кодом и схлопывание строк с одинаковыми lines.
    stmt::VerifyContext ctx;
    ctx.lines = &lines;
    ctx.chunk_first = chunk->first_line;
    ctx.chunk_last = chunk->last_line;
    ctx.date_context = chunk->date_context;
    ctx.today = today;
    ctx.period = {imp->period_from, imp->period_to};
    ctx.wallet_currency = currency;
    ctx.rules = service_.check_rules_for(*imp);
    std::vector<stmt::RowRec> rows;
    std::set<std::vector<int>> seen;
    for (const auto& g : got) {
        stmt::Candidate c{g.lines, g.date, g.amount, g.dir, g.currency, g.description};
        auto v = stmt::verify_row(c, ctx);
        if (!v || !seen.insert(v->lines).second) continue;
        stmt::RowRec r;
        r.id = uuidgen::v4();
        r.n = n * 1000 + static_cast<int>(rows.size());
        r.lines = v->lines;
        r.date = v->date;
        r.amount = v->amount;
        r.dir = v->dir;
        r.currency = v->currency;
        r.description = v->description;
        r.pattern = stmt::pattern_of(v->description);
        r.check = v->check;
        r.classified = r.dir != "expense";
        // Окончательные отпечатки — после всех фрагментов (k по импорту).
        r.fingerprint = stmt::fingerprint(imp->wallet_uuid, r.date, r.amount, r.dir, r.pattern, 0);
        r.op_uuid = stmt::op_uuid(r.fingerprint);
        rows.push_back(std::move(r));
    }

    chunk->status = gave_up ? "failed" : "done";
    int done = 0;
    for (const auto& c : chunks) done += c.status != "pending" ? 1 : 0;
    bool saved = repo_.complete_chunk(import_id, *chunk, rows, done);
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    metrics::observe_duration("ai_statement_chunk_duration_seconds", "Разбор одного фрагмента выписки.", "result",
                              gave_up ? "gave_up" : "ok", secs);
    log["rows"] = rows.size();
    log["completion_tokens"] = tokens;
    log["outcome"] = saved ? (gave_up ? "gave_up" : "extracted") : "discarded";
    parse_expense::Outcome out;
    out.status = jobs::Status::Done;
    out.result = {{"outcome", log["outcome"]}, {"rows", rows.size()}};
    out.log = log;
    return out;
}

parse_expense::Outcome Runner::classify(const jobs::Job& job) {
    const Config& cfg = service_.config();
    std::string import_id = job.input.value("import_id", std::string{});
    auto imp = repo_.get_import(import_id, "");
    if (!imp || imp->status != "classifying") return skipped("not_classifying");
    std::vector<std::string> patterns = job.input.value("patterns", std::vector<std::string>{});

    auto accounts = resolver::parse_accounts(imp->accounts);
    llm::ClassifyRequest req;
    req.patterns = patterns;
    std::map<std::string, std::string> uuid_of;
    for (const auto& c : accounts.categories) {
        req.categories.push_back(c.name);
        uuid_of[c.name] = c.uuid;
    }
    auto res = llm_.classify_rows(req, classify_opts_);
    if (res.status == llm::Status::BadResponse) res = llm_.classify_rows(req, classify_opts_);

    std::map<std::string, std::pair<std::string, double>> answer; // pattern → (имя, уверенность)
    if (res.status == llm::Status::Ok) {
        for (const auto& item : res.items) {
            if (item.i >= 0 && item.i < static_cast<int>(patterns.size())) {
                answer[patterns[static_cast<std::size_t>(item.i)]] = {item.category, item.confidence};
            }
        }
    }
    // Сбой разметки — строки пачки просто остаются без категории.
    auto rows = repo_.rows(import_id);
    std::set<std::string> batch(patterns.begin(), patterns.end());
    int accepted = 0;
    for (auto& r : rows) {
        if (!needs_category(r) || batch.count(r.pattern) == 0) continue;
        r.classified = true;
        auto a = answer.find(r.pattern);
        if (a == answer.end() || a->second.first.empty()) continue;
        auto uuid = uuid_of.find(a->second.first);
        if (uuid != uuid_of.end() && a->second.second >= cfg.classify_min_confidence) {
            r.category_uuid = uuid->second;
            r.category_source = "llm";
            ++accepted;
        } else {
            r.suggestion = a->second.first;
        }
    }
    repo_.save_rows(import_id, rows);
    imp->progress_done += 1;
    repo_.save_import(*imp);

    parse_expense::Outcome out;
    out.status = jobs::Status::Done;
    out.result = {{"outcome", "classified"}};
    out.log = {{"import_id", import_id},
               {"patterns", patterns.size()},
               {"accepted", accepted},
               {"llm_status", llm::to_string(res.status)},
               {"completion_tokens", res.completion_tokens},
               {"outcome", res.status == llm::Status::Ok ? "classified" : "unclassified"}};
    return out;
}

void Runner::cancel_import(const std::string& import_id) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = inflight_.find(import_id);
    if (it == inflight_.end()) return;
    store_.cancel(it->second.first, it->second.second);
    inflight_.erase(it);
}

void Runner::wake() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        woken_ = true;
    }
    cv_.notify_one();
}

int Runner::sweep() {
    int n = repo_.expire(service_.now());
    if (n > 0) applog::info("imports expired", {{"event", "imports_expired"}, {"count", n}});
    return n;
}

void Runner::run(const std::function<bool()>& db_up) {
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait_for(lock, std::chrono::seconds(5), [this] { return woken_; });
            woken_ = false;
        }
        if (!db_up()) continue;
        tick();
        stmt::Epoch now = service_.now();
        if (now - last_sweep_ >= 3600) {
            try {
                sweep();
                last_sweep_ = now;
            } catch (const std::exception& e) {
                applog::warn("imports sweep failed", {{"event", "imports_sweep_failed"}, {"error", e.what()}});
            }
        }
    }
}

} // namespace imports
