// Хранилище импортов выписок (база ai, D1). Интерфейс — чтобы сервис и
// планировщик тестировались без Postgres (tests/mem_repo.hpp); боевая
// реализация — PgRepo поверх db::Pool. Время — секунды Unix.
#pragma once

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace db {
class Pool;
}

namespace stmt {

using Epoch = long long;

struct ImportRec {
    std::string id;
    std::string user_id;
    std::string wallet_uuid;
    std::string status; // queued | extracting | classifying | ready | done | failed | cancelled
    std::string failure;
    int progress_done = 0;
    int progress_total = 0;
    std::string source_name;
    std::string source_text;
    nlohmann::json accounts = nlohmann::json::object();
    std::string period_from;
    std::string period_to;
    std::optional<Epoch> reconciled_at;
    std::string format_fingerprint;
    std::string profile_id;
    std::string profile_version;
    std::string profile_source; // auto | manual | ""
    std::string profile_detect; // none | ambiguous | ""
    std::optional<Epoch> paused_since;
    Epoch created_at = 0;
    std::optional<Epoch> done_at;
    Epoch expires_at = 0;
};

struct ChunkRec {
    int n = 0;
    int first_line = 0;
    int last_line = 0;
    std::string date_context;
    std::string status = "pending"; // pending | done | failed
    int attempts = 0;
};

struct RowRec {
    std::string id;
    int n = 0;
    std::vector<int> lines;
    std::string date;
    std::string amount;
    std::string dir;
    std::string currency;
    std::string description;
    std::string pattern;
    std::optional<std::string> comment;
    std::string category_uuid;
    std::string category_source; // rule | llm | user | ""
    std::string suggestion;
    bool classified = false;
    std::string origin = "llm"; // llm | manual
    std::string check;
    std::string match; // exists | possible_duplicate | imported | ""
    nlohmann::json match_candidates; // null или массив
    std::string decision; // include | skip | duplicate | ""
    bool edited = false;
    bool committed = false;
    std::string fingerprint;
    std::string op_uuid;
};

struct UserProfileRec {
    std::string id;
    std::string user_id;
    std::string format_fingerprint;
    std::string name;
    int examples = 0;
};

struct ExampleRec {
    std::string source_lines;
    nlohmann::json expected;
};

class Repo {
public:
    virtual ~Repo() = default;

    // Доступна ли база прямо сейчас (SELECT 1).
    virtual bool ping() = 0;

    virtual void create_import(const ImportRec& imp, const std::vector<ChunkRec>& chunks) = 0;
    // user_id пуст — без проверки владельца (планировщик).
    virtual std::optional<ImportRec> get_import(const std::string& id, const std::string& user_id) = 0;
    // Импорты пользователя, новые первыми; source_text не заполняется.
    virtual std::vector<ImportRec> list_imports(const std::string& user_id) = 0;
    // В работе: queued, extracting, classifying.
    virtual std::vector<ImportRec> active_imports() = 0;
    // Изменяемые поля импорта (status, failure, progress, period, reconciled_at,
    // paused_since, done_at, expires_at). false — импорта уже нет.
    virtual bool save_import(const ImportRec& imp) = 0;
    virtual bool delete_import(const std::string& id) = 0;
    // Удалить импорты с expires_at <= now; число удалённых.
    virtual int expire(Epoch now) = 0;

    virtual std::vector<ChunkRec> chunks(const std::string& import_id) = 0;
    virtual void save_chunk(const std::string& import_id, const ChunkRec& c) = 0;
    // Строки фрагмента и его done одной транзакцией, только пока импорт в
    // extracting. false — импорт удалён или уже не разбирается.
    virtual bool complete_chunk(const std::string& import_id, const ChunkRec& c, const std::vector<RowRec>& rows,
                                int progress_done) = 0;

    virtual std::vector<RowRec> rows(const std::string& import_id) = 0;
    virtual void insert_row(const std::string& import_id, const RowRec& r) = 0;
    virtual void save_rows(const std::string& import_id, const std::vector<RowRec>& rows) = 0;

    virtual std::map<std::string, std::string> rules(const std::string& user_id) = 0;
    virtual void upsert_rule(const std::string& user_id, const std::string& pattern,
                             const std::string& category_uuid) = 0;

    virtual std::set<std::string> imported(const std::string& user_id, const std::vector<std::string>& fingerprints) = 0;
    virtual void add_imported(const std::string& user_id, const std::vector<std::string>& fingerprints) = 0;

    virtual std::vector<UserProfileRec> user_profiles(const std::string& user_id) = 0;
    virtual std::optional<UserProfileRec> user_profile(const std::string& id, const std::string& user_id) = 0;
    virtual std::optional<UserProfileRec> profile_by_fingerprint(const std::string& user_id,
                                                                 const std::string& fingerprint) = 0;
    virtual UserProfileRec create_profile(const std::string& user_id, const std::string& fingerprint,
                                          const std::string& name) = 0;
    virtual bool rename_profile(const std::string& id, const std::string& user_id, const std::string& name) = 0;
    virtual bool delete_profile(const std::string& id, const std::string& user_id) = 0;
    // Новые первыми, не больше keep; лишние удаляются при вставке.
    virtual std::vector<ExampleRec> examples(const std::string& profile_id) = 0;
    virtual void add_example(const std::string& profile_id, const ExampleRec& e, int keep) = 0;
};

std::unique_ptr<Repo> make_pg_repo(db::Pool& pool);

} // namespace stmt
