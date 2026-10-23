// Repo поверх Postgres (база ai, migrations/000001_init.up.sql). NULL из базы
// приходит пустой строкой (db::Row), поэтому опциональные поля пишутся через
// NULLIF($n, '') и читаются через coalesce.
#include <sstream>

#include "db.hpp"
#include "statement/repo.hpp"
#include "util/uuid.hpp"

namespace stmt {

namespace {

const char* const kImportColumns =
    "id, user_id, wallet_uuid, status, coalesce(failure, ''), progress_done, progress_total, source_name, "
    "%SOURCE%, accounts_snapshot::text, coalesce(period_from::text, ''), coalesce(period_to::text, ''), "
    "coalesce(extract(epoch from reconciled_at)::bigint::text, ''), coalesce(format_fingerprint, ''), "
    "coalesce(profile_id, ''), coalesce(profile_version, ''), coalesce(profile_source, ''), "
    "coalesce(profile_detect, ''), coalesce(extract(epoch from paused_since)::bigint::text, ''), "
    "extract(epoch from created_at)::bigint, coalesce(extract(epoch from done_at)::bigint::text, ''), "
    "extract(epoch from expires_at)::bigint";

std::string import_columns(bool with_source) {
    std::string s = kImportColumns;
    s.replace(s.find("%SOURCE%"), 8, with_source ? "source_text" : "''");
    return s;
}

const char* const kRowColumns =
    "id, n, lines::text, coalesce(date::text, ''), amount, dir, currency, description, pattern, "
    "comment IS NOT NULL, coalesce(comment, ''), coalesce(category_uuid::text, ''), coalesce(category_source, ''), "
    "coalesce(suggestion, ''), classified, origin, coalesce(\"check\", ''), coalesce(match, ''), "
    "coalesce(match_candidates::text, ''), coalesce(decision, ''), edited, committed, fingerprint, op_uuid";

std::optional<Epoch> opt_epoch(const std::string& s) {
    if (s.empty()) return std::nullopt;
    return std::stoll(s);
}

std::string epoch_param(const std::optional<Epoch>& e) { return e ? std::to_string(*e) : ""; }

bool flag(const std::string& s) { return s == "t" || s == "true"; }

ImportRec import_of(const db::Row& r) {
    const auto& f = r.fields;
    ImportRec i;
    i.id = f[0];
    i.user_id = f[1];
    i.wallet_uuid = f[2];
    i.status = f[3];
    i.failure = f[4];
    i.progress_done = std::stoi(f[5]);
    i.progress_total = std::stoi(f[6]);
    i.source_name = f[7];
    i.source_text = f[8];
    i.accounts = nlohmann::json::parse(f[9]);
    i.period_from = f[10];
    i.period_to = f[11];
    i.reconciled_at = opt_epoch(f[12]);
    i.format_fingerprint = f[13];
    i.profile_id = f[14];
    i.profile_version = f[15];
    i.profile_source = f[16];
    i.profile_detect = f[17];
    i.paused_since = opt_epoch(f[18]);
    i.created_at = std::stoll(f[19]);
    i.done_at = opt_epoch(f[20]);
    i.expires_at = std::stoll(f[21]);
    return i;
}

std::vector<int> parse_int_array(const std::string& s) {
    std::vector<int> out;
    std::string num;
    for (char c : s) {
        if ((c >= '0' && c <= '9') || c == '-') {
            num.push_back(c);
        } else if (!num.empty()) {
            out.push_back(std::stoi(num));
            num.clear();
        }
    }
    if (!num.empty()) out.push_back(std::stoi(num));
    return out;
}

std::string int_array(const std::vector<int>& v) {
    std::string s = "{";
    for (std::size_t i = 0; i < v.size(); ++i) s += (i ? "," : "") + std::to_string(v[i]);
    return s + "}";
}

RowRec row_of(const db::Row& r) {
    const auto& f = r.fields;
    RowRec x;
    x.id = f[0];
    x.n = std::stoi(f[1]);
    x.lines = parse_int_array(f[2]);
    x.date = f[3];
    x.amount = f[4];
    x.dir = f[5];
    x.currency = f[6];
    x.description = f[7];
    x.pattern = f[8];
    if (flag(f[9])) x.comment = f[10];
    x.category_uuid = f[11];
    x.category_source = f[12];
    x.suggestion = f[13];
    x.classified = flag(f[14]);
    x.origin = f[15];
    x.check = f[16];
    x.match = f[17];
    x.match_candidates = f[18].empty() ? nlohmann::json() : nlohmann::json::parse(f[18]);
    x.decision = f[19];
    x.edited = flag(f[20]);
    x.committed = flag(f[21]);
    x.fingerprint = f[22];
    x.op_uuid = f[23];
    return x;
}

const char* const kInsertRow =
    "INSERT INTO statement_row (id, import_id, n, lines, date, amount, dir, currency, description, pattern, "
    "comment, category_uuid, category_source, suggestion, classified, origin, \"check\", match, "
    "match_candidates, decision, edited, committed, fingerprint, op_uuid) VALUES ($1, $2, $3, $4::integer[], "
    "NULLIF($5, '')::date, $6, $7, $8, $9, $10, CASE WHEN $11 = 't' THEN $12 END, NULLIF($13, '')::uuid, "
    "NULLIF($14, ''), NULLIF($15, ''), $16::boolean, $17, NULLIF($18, ''), NULLIF($19, ''), "
    "NULLIF($20, '')::jsonb, NULLIF($21, ''), $22::boolean, $23::boolean, $24, $25::uuid)";

const char* const kUpdateRow =
    "UPDATE statement_row SET lines = $3::integer[], date = NULLIF($4, '')::date, amount = $5, dir = $6, "
    "currency = $7, description = $8, pattern = $9, comment = CASE WHEN $10 = 't' THEN $11 END, "
    "category_uuid = NULLIF($12, '')::uuid, category_source = NULLIF($13, ''), suggestion = NULLIF($14, ''), "
    "classified = $15::boolean, \"check\" = NULLIF($16, ''), match = NULLIF($17, ''), "
    "match_candidates = NULLIF($18, '')::jsonb, decision = NULLIF($19, ''), edited = $20::boolean, "
    "committed = $21::boolean, fingerprint = $22, op_uuid = $23::uuid WHERE import_id = $1 AND id = $2";

std::string b(bool v) { return v ? "t" : "f"; }

std::string candidates_param(const nlohmann::json& j) { return j.is_null() ? "" : j.dump(); }

std::vector<std::string> insert_params(const std::string& import_id, const RowRec& r) {
    return {r.id, import_id, std::to_string(r.n), int_array(r.lines), r.date, r.amount, r.dir, r.currency,
            r.description, r.pattern, b(r.comment.has_value()), r.comment.value_or(""), r.category_uuid,
            r.category_source, r.suggestion, b(r.classified), r.origin, r.check, r.match,
            candidates_param(r.match_candidates), r.decision, b(r.edited), b(r.committed), r.fingerprint,
            r.op_uuid};
}

std::vector<std::string> update_params(const std::string& import_id, const RowRec& r) {
    return {import_id, r.id, int_array(r.lines), r.date, r.amount, r.dir, r.currency, r.description, r.pattern,
            b(r.comment.has_value()), r.comment.value_or(""), r.category_uuid, r.category_source, r.suggestion,
            b(r.classified), r.check, r.match, candidates_param(r.match_candidates), r.decision, b(r.edited),
            b(r.committed), r.fingerprint, r.op_uuid};
}

UserProfileRec profile_of(const db::Row& r) {
    return {r.fields[0], r.fields[1], r.fields[2], r.fields[3], std::stoi(r.fields[4])};
}

const char* const kProfileSelect =
    "SELECT p.id, p.user_id, p.format_fingerprint, p.name, "
    "(SELECT count(*) FROM format_example e WHERE e.profile_id = p.id) FROM format_profile p ";

class PgRepo : public Repo {
public:
    explicit PgRepo(db::Pool& pool) : pool_(pool) {}

    bool ping() override {
        try {
            auto c = pool_.acquire();
            c->exec("SELECT 1");
            return true;
        } catch (const std::exception&) {
            return false;
        }
    }

    void create_import(const ImportRec& i, const std::vector<ChunkRec>& chunks) override {
        auto c = pool_.acquire();
        auto tx = c->begin_transaction();
        tx.exec("INSERT INTO statement_import (id, user_id, wallet_uuid, status, failure, progress_done, "
                "progress_total, source_name, source_text, accounts_snapshot, period_from, period_to, "
                "format_fingerprint, profile_id, profile_version, profile_source, profile_detect, created_at, "
                "expires_at) VALUES ($1, $2, $3, $4, NULLIF($5, ''), $6, $7, $8, $9, $10::jsonb, "
                "NULLIF($11, '')::date, NULLIF($12, '')::date, NULLIF($13, ''), NULLIF($14, ''), NULLIF($15, ''), "
                "NULLIF($16, ''), NULLIF($17, ''), to_timestamp($18), to_timestamp($19))",
                {i.id, i.user_id, i.wallet_uuid, i.status, i.failure, std::to_string(i.progress_done),
                 std::to_string(i.progress_total), i.source_name, i.source_text, i.accounts.dump(), i.period_from,
                 i.period_to, i.format_fingerprint, i.profile_id, i.profile_version, i.profile_source,
                 i.profile_detect, std::to_string(i.created_at), std::to_string(i.expires_at)});
        for (const auto& ch : chunks) {
            tx.exec("INSERT INTO statement_chunk (import_id, n, first_line, last_line, date_context, status, "
                    "attempts) VALUES ($1, $2, $3, $4, $5, $6, $7)",
                    {i.id, std::to_string(ch.n), std::to_string(ch.first_line), std::to_string(ch.last_line),
                     ch.date_context, ch.status, std::to_string(ch.attempts)});
        }
        tx.commit();
    }

    std::optional<ImportRec> get_import(const std::string& id, const std::string& user_id) override {
        if (!uuidgen::looks_valid(id)) return std::nullopt;
        auto c = pool_.acquire();
        auto res = c->exec("SELECT " + import_columns(true) +
                               " FROM statement_import WHERE id = $1 AND ($2 = '' OR user_id::text = $2)",
                           {id, user_id});
        if (res.rows.empty()) return std::nullopt;
        return import_of(res.rows[0]);
    }

    std::vector<ImportRec> list_imports(const std::string& user_id) override {
        auto c = pool_.acquire();
        auto res = c->exec("SELECT " + import_columns(false) +
                               " FROM statement_import WHERE user_id = $1 ORDER BY created_at DESC, id",
                           {user_id});
        std::vector<ImportRec> out;
        for (const auto& r : res.rows) out.push_back(import_of(r));
        return out;
    }

    std::vector<ImportRec> active_imports() override {
        auto c = pool_.acquire();
        auto res = c->exec("SELECT " + import_columns(false) +
                           " FROM statement_import WHERE status IN ('queued', 'extracting', 'classifying') "
                           "ORDER BY created_at, id");
        std::vector<ImportRec> out;
        for (const auto& r : res.rows) out.push_back(import_of(r));
        return out;
    }

    bool save_import(const ImportRec& i) override {
        auto c = pool_.acquire();
        auto res = c->exec(
            "UPDATE statement_import SET status = $2, failure = NULLIF($3, ''), progress_done = $4, "
            "progress_total = $5, period_from = NULLIF($6, '')::date, period_to = NULLIF($7, '')::date, "
            "reconciled_at = CASE WHEN $8 = '' THEN NULL ELSE to_timestamp($8::bigint) END, "
            "paused_since = CASE WHEN $9 = '' THEN NULL ELSE to_timestamp($9::bigint) END, "
            "done_at = CASE WHEN $10 = '' THEN NULL ELSE to_timestamp($10::bigint) END, "
            "expires_at = to_timestamp($11) WHERE id = $1 RETURNING id",
            {i.id, i.status, i.failure, std::to_string(i.progress_done), std::to_string(i.progress_total),
             i.period_from, i.period_to, epoch_param(i.reconciled_at), epoch_param(i.paused_since),
             epoch_param(i.done_at), std::to_string(i.expires_at)});
        return !res.rows.empty();
    }

    bool delete_import(const std::string& id) override {
        auto c = pool_.acquire();
        return !c->exec("DELETE FROM statement_import WHERE id = $1 RETURNING id", {id}).rows.empty();
    }

    int expire(Epoch now) override {
        auto c = pool_.acquire();
        auto res = c->exec("DELETE FROM statement_import WHERE expires_at <= to_timestamp($1) RETURNING id",
                           {std::to_string(now)});
        return static_cast<int>(res.rows.size());
    }

    std::vector<ChunkRec> chunks(const std::string& import_id) override {
        auto c = pool_.acquire();
        auto res = c->exec("SELECT n, first_line, last_line, date_context, status, attempts FROM statement_chunk "
                           "WHERE import_id = $1 ORDER BY n",
                           {import_id});
        std::vector<ChunkRec> out;
        for (const auto& r : res.rows) {
            out.push_back({std::stoi(r.fields[0]), std::stoi(r.fields[1]), std::stoi(r.fields[2]), r.fields[3],
                           r.fields[4], std::stoi(r.fields[5])});
        }
        return out;
    }

    void save_chunk(const std::string& import_id, const ChunkRec& ch) override {
        auto c = pool_.acquire();
        c->exec("UPDATE statement_chunk SET status = $3, attempts = $4 WHERE import_id = $1 AND n = $2",
                {import_id, std::to_string(ch.n), ch.status, std::to_string(ch.attempts)});
    }

    bool complete_chunk(const std::string& import_id, const ChunkRec& ch, const std::vector<RowRec>& rows,
                        int progress_done) override {
        auto c = pool_.acquire();
        auto tx = c->begin_transaction();
        auto locked = tx.exec("SELECT status FROM statement_import WHERE id = $1 FOR UPDATE", {import_id});
        if (locked.rows.empty() || locked.rows[0].fields[0] != "extracting") return false;
        for (const auto& r : rows) tx.exec(kInsertRow, insert_params(import_id, r));
        tx.exec("UPDATE statement_chunk SET status = $3, attempts = $4 WHERE import_id = $1 AND n = $2",
                {import_id, std::to_string(ch.n), ch.status, std::to_string(ch.attempts)});
        tx.exec("UPDATE statement_import SET progress_done = $2 WHERE id = $1",
                {import_id, std::to_string(progress_done)});
        tx.commit();
        return true;
    }

    std::vector<RowRec> rows(const std::string& import_id) override {
        auto c = pool_.acquire();
        auto res = c->exec(std::string("SELECT ") + kRowColumns + " FROM statement_row WHERE import_id = $1 ORDER BY n",
                           {import_id});
        std::vector<RowRec> out;
        for (const auto& r : res.rows) out.push_back(row_of(r));
        return out;
    }

    void insert_row(const std::string& import_id, const RowRec& r) override {
        auto c = pool_.acquire();
        c->exec(kInsertRow, insert_params(import_id, r));
    }

    void save_rows(const std::string& import_id, const std::vector<RowRec>& rows) override {
        if (rows.empty()) return;
        auto c = pool_.acquire();
        auto tx = c->begin_transaction();
        for (const auto& r : rows) tx.exec(kUpdateRow, update_params(import_id, r));
        tx.commit();
    }

    std::map<std::string, std::string> rules(const std::string& user_id) override {
        auto c = pool_.acquire();
        auto res = c->exec("SELECT pattern, category_uuid FROM merchant_rule WHERE user_id = $1", {user_id});
        std::map<std::string, std::string> out;
        for (const auto& r : res.rows) out[r.fields[0]] = r.fields[1];
        return out;
    }

    void upsert_rule(const std::string& user_id, const std::string& pattern, const std::string& category) override {
        auto c = pool_.acquire();
        c->exec("INSERT INTO merchant_rule (user_id, pattern, category_uuid) VALUES ($1, $2, $3) "
                "ON CONFLICT (user_id, pattern) DO UPDATE SET category_uuid = EXCLUDED.category_uuid, "
                "updated_at = now()",
                {user_id, pattern, category});
    }

    std::set<std::string> imported(const std::string& user_id, const std::vector<std::string>& fps) override {
        std::set<std::string> out;
        if (fps.empty()) return out;
        std::string arr = "{";
        for (std::size_t i = 0; i < fps.size(); ++i) arr += (i ? "," : "") + fps[i]; // hex — без кавычек
        arr += "}";
        auto c = pool_.acquire();
        auto res = c->exec("SELECT fingerprint FROM statement_imported WHERE user_id = $1 AND fingerprint = ANY($2::text[])",
                           {user_id, arr});
        for (const auto& r : res.rows) out.insert(r.fields[0]);
        return out;
    }

    void add_imported(const std::string& user_id, const std::vector<std::string>& fps) override {
        if (fps.empty()) return;
        auto c = pool_.acquire();
        auto tx = c->begin_transaction();
        for (const auto& fp : fps) {
            tx.exec("INSERT INTO statement_imported (user_id, fingerprint) VALUES ($1, $2) ON CONFLICT DO NOTHING",
                    {user_id, fp});
        }
        tx.commit();
    }

    std::vector<UserProfileRec> user_profiles(const std::string& user_id) override {
        auto c = pool_.acquire();
        auto res = c->exec(std::string(kProfileSelect) + "WHERE p.user_id = $1 ORDER BY p.created_at", {user_id});
        std::vector<UserProfileRec> out;
        for (const auto& r : res.rows) out.push_back(profile_of(r));
        return out;
    }

    std::optional<UserProfileRec> user_profile(const std::string& id, const std::string& user_id) override {
        if (!uuidgen::looks_valid(id)) return std::nullopt;
        auto c = pool_.acquire();
        auto res = c->exec(std::string(kProfileSelect) + "WHERE p.id = $1 AND p.user_id = $2", {id, user_id});
        if (res.rows.empty()) return std::nullopt;
        return profile_of(res.rows[0]);
    }

    std::optional<UserProfileRec> profile_by_fingerprint(const std::string& user_id, const std::string& fp) override {
        auto c = pool_.acquire();
        auto res = c->exec(std::string(kProfileSelect) + "WHERE p.user_id = $1 AND p.format_fingerprint = $2",
                           {user_id, fp});
        if (res.rows.empty()) return std::nullopt;
        return profile_of(res.rows[0]);
    }

    UserProfileRec create_profile(const std::string& user_id, const std::string& fp, const std::string& name) override {
        auto c = pool_.acquire();
        c->exec("INSERT INTO format_profile (id, user_id, format_fingerprint, name) VALUES ($1, $2, $3, $4) "
                "ON CONFLICT (user_id, format_fingerprint) DO NOTHING",
                {uuidgen::v4(), user_id, fp, name});
        auto res = c->exec(std::string(kProfileSelect) + "WHERE p.user_id = $1 AND p.format_fingerprint = $2",
                           {user_id, fp});
        return profile_of(res.rows.at(0));
    }

    bool rename_profile(const std::string& id, const std::string& user_id, const std::string& name) override {
        if (!uuidgen::looks_valid(id)) return false;
        auto c = pool_.acquire();
        return !c->exec("UPDATE format_profile SET name = $3 WHERE id = $1 AND user_id = $2 RETURNING id",
                        {id, user_id, name})
                    .rows.empty();
    }

    bool delete_profile(const std::string& id, const std::string& user_id) override {
        if (!uuidgen::looks_valid(id)) return false;
        auto c = pool_.acquire();
        return !c->exec("DELETE FROM format_profile WHERE id = $1 AND user_id = $2 RETURNING id", {id, user_id})
                    .rows.empty();
    }

    std::vector<ExampleRec> examples(const std::string& profile_id) override {
        auto c = pool_.acquire();
        auto res = c->exec("SELECT source_lines, expected::text FROM format_example WHERE profile_id = $1 "
                           "ORDER BY created_at DESC, id DESC",
                           {profile_id});
        std::vector<ExampleRec> out;
        for (const auto& r : res.rows) out.push_back({r.fields[0], nlohmann::json::parse(r.fields[1])});
        return out;
    }

    void add_example(const std::string& profile_id, const ExampleRec& e, int keep) override {
        auto c = pool_.acquire();
        auto tx = c->begin_transaction();
        tx.exec("INSERT INTO format_example (profile_id, source_lines, expected) VALUES ($1, $2, $3::jsonb)",
                {profile_id, e.source_lines, e.expected.dump()});
        tx.exec("DELETE FROM format_example WHERE profile_id = $1 AND id NOT IN (SELECT id FROM format_example "
                "WHERE profile_id = $1 ORDER BY created_at DESC, id DESC LIMIT $2)",
                {profile_id, std::to_string(keep)});
        tx.commit();
    }

private:
    db::Pool& pool_;
};

} // namespace

std::unique_ptr<Repo> make_pg_repo(db::Pool& pool) { return std::make_unique<PgRepo>(pool); }

} // namespace stmt
