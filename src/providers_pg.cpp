// SettingsRepo поверх Postgres (база ai, migrations/000002_ai_user_settings).
#include "db.hpp"
#include "providers.hpp"

namespace providers {

namespace {

class PgSettingsRepo final : public SettingsRepo {
public:
    explicit PgSettingsRepo(db::Pool& pool) : pool_(pool) {}

    UserSettings get(const std::string& user_id) override {
        auto conn = pool_.acquire();
        UserSettings s;
        auto r = conn->exec("SELECT provider FROM ai_user_settings WHERE user_id = $1", {user_id});
        if (!r.rows.empty()) s.provider = r.rows[0].fields.at(0);
        auto c = conn->exec(
            "SELECT provider, extract(epoch from consented_at)::bigint FROM ai_provider_consent WHERE user_id = $1",
            {user_id});
        for (const auto& row : c.rows) s.consents[row.fields.at(0)] = std::stoll(row.fields.at(1));
        return s;
    }

    void set_provider(const std::string& user_id, const std::string& provider) override {
        pool_.acquire()->exec(
            "INSERT INTO ai_user_settings (user_id, provider) VALUES ($1, $2) "
            "ON CONFLICT (user_id) DO UPDATE SET provider = EXCLUDED.provider, updated_at = now()",
            {user_id, provider});
    }

    void set_consent(const std::string& user_id, const std::string& provider, Epoch at) override {
        pool_.acquire()->exec(
            "INSERT INTO ai_provider_consent (user_id, provider, consented_at) VALUES ($1, $2, to_timestamp($3)) "
            "ON CONFLICT (user_id, provider) DO UPDATE SET consented_at = EXCLUDED.consented_at",
            {user_id, provider, std::to_string(at)});
    }

    void revoke_consent(const std::string& user_id, const std::string& provider) override {
        pool_.acquire()->exec("DELETE FROM ai_provider_consent WHERE user_id = $1 AND provider = $2",
                              {user_id, provider});
    }

private:
    db::Pool& pool_;
};

} // namespace

std::unique_ptr<SettingsRepo> make_pg_settings_repo(db::Pool& pool) { return std::make_unique<PgSettingsRepo>(pool); }

} // namespace providers
