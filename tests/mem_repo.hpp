// stmt::Repo в памяти — для тестов сервиса импорта и планировщика без Postgres.
#pragma once

#include <algorithm>

#include "statement/repo.hpp"
#include "util/uuid.hpp"

class MemRepo : public stmt::Repo {
public:
    bool up = true;
    std::map<std::string, stmt::ImportRec> imports;
    std::map<std::string, std::vector<stmt::ChunkRec>> chunk_map;
    std::map<std::string, std::vector<stmt::RowRec>> row_map;
    std::map<std::pair<std::string, std::string>, std::string> rule_map;
    std::set<std::pair<std::string, std::string>> imported_set;
    std::map<std::string, stmt::UserProfileRec> profile_map;
    std::map<std::string, std::vector<stmt::ExampleRec>> example_map; // новые первыми

    bool ping() override { return up; }

    void create_import(const stmt::ImportRec& imp, const std::vector<stmt::ChunkRec>& chunks) override {
        imports[imp.id] = imp;
        chunk_map[imp.id] = chunks;
    }
    std::optional<stmt::ImportRec> get_import(const std::string& id, const std::string& user_id) override {
        auto it = imports.find(id);
        if (it == imports.end() || (!user_id.empty() && it->second.user_id != user_id)) return std::nullopt;
        return it->second;
    }
    std::vector<stmt::ImportRec> list_imports(const std::string& user_id) override {
        std::vector<stmt::ImportRec> out;
        for (const auto& [id, imp] : imports) {
            if (imp.user_id == user_id) out.push_back(imp);
        }
        std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.created_at > b.created_at; });
        return out;
    }
    std::vector<stmt::ImportRec> active_imports() override {
        std::vector<stmt::ImportRec> out;
        for (const auto& [id, imp] : imports) {
            if (imp.status == "queued" || imp.status == "extracting" || imp.status == "classifying") out.push_back(imp);
        }
        std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.created_at < b.created_at; });
        return out;
    }
    bool save_import(const stmt::ImportRec& imp) override {
        auto it = imports.find(imp.id);
        if (it == imports.end()) return false;
        std::string text = it->second.source_text;
        it->second = imp;
        it->second.source_text = text; // как в Postgres: текст не меняется
        return true;
    }
    bool delete_import(const std::string& id) override {
        chunk_map.erase(id);
        row_map.erase(id);
        return imports.erase(id) != 0;
    }
    int expire(stmt::Epoch now) override {
        std::vector<std::string> dead;
        for (const auto& [id, imp] : imports) {
            if (imp.expires_at <= now) dead.push_back(id);
        }
        for (const auto& id : dead) delete_import(id);
        return static_cast<int>(dead.size());
    }
    std::vector<stmt::ChunkRec> chunks(const std::string& import_id) override { return chunk_map[import_id]; }
    void save_chunk(const std::string& import_id, const stmt::ChunkRec& c) override {
        for (auto& x : chunk_map[import_id]) {
            if (x.n == c.n) x = c;
        }
    }
    bool complete_chunk(const std::string& import_id, const stmt::ChunkRec& c, const std::vector<stmt::RowRec>& rows,
                        int progress_done) override {
        auto it = imports.find(import_id);
        if (it == imports.end() || it->second.status != "extracting") return false;
        for (const auto& r : rows) row_map[import_id].push_back(r);
        save_chunk(import_id, c);
        it->second.progress_done = progress_done;
        return true;
    }
    std::vector<stmt::RowRec> rows(const std::string& import_id) override {
        auto out = row_map[import_id];
        std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.n < b.n; });
        return out;
    }
    void insert_row(const std::string& import_id, const stmt::RowRec& r) override { row_map[import_id].push_back(r); }
    void save_rows(const std::string& import_id, const std::vector<stmt::RowRec>& rows) override {
        for (const auto& r : rows) {
            for (auto& x : row_map[import_id]) {
                if (x.id == r.id) x = r;
            }
        }
    }
    std::map<std::string, std::string> rules(const std::string& user_id) override {
        std::map<std::string, std::string> out;
        for (const auto& [k, v] : rule_map) {
            if (k.first == user_id) out[k.second] = v;
        }
        return out;
    }
    void upsert_rule(const std::string& user_id, const std::string& pattern, const std::string& category) override {
        rule_map[{user_id, pattern}] = category;
    }
    std::set<std::string> imported(const std::string& user_id, const std::vector<std::string>& fps) override {
        std::set<std::string> out;
        for (const auto& fp : fps) {
            if (imported_set.count({user_id, fp}) != 0) out.insert(fp);
        }
        return out;
    }
    void add_imported(const std::string& user_id, const std::vector<std::string>& fps) override {
        for (const auto& fp : fps) imported_set.insert({user_id, fp});
    }
    std::vector<stmt::UserProfileRec> user_profiles(const std::string& user_id) override {
        std::vector<stmt::UserProfileRec> out;
        for (auto [id, p] : profile_map) {
            if (p.user_id != user_id) continue;
            p.examples = static_cast<int>(example_map[id].size());
            out.push_back(p);
        }
        return out;
    }
    std::optional<stmt::UserProfileRec> user_profile(const std::string& id, const std::string& user_id) override {
        auto it = profile_map.find(id);
        if (it == profile_map.end() || it->second.user_id != user_id) return std::nullopt;
        auto p = it->second;
        p.examples = static_cast<int>(example_map[id].size());
        return p;
    }
    std::optional<stmt::UserProfileRec> profile_by_fingerprint(const std::string& user_id,
                                                               const std::string& fp) override {
        for (const auto& [id, p] : profile_map) {
            if (p.user_id == user_id && p.format_fingerprint == fp) return user_profile(id, user_id);
        }
        return std::nullopt;
    }
    stmt::UserProfileRec create_profile(const std::string& user_id, const std::string& fp,
                                        const std::string& name) override {
        if (auto p = profile_by_fingerprint(user_id, fp)) return *p;
        stmt::UserProfileRec p{uuidgen::v4(), user_id, fp, name, 0};
        profile_map[p.id] = p;
        return p;
    }
    bool rename_profile(const std::string& id, const std::string& user_id, const std::string& name) override {
        auto it = profile_map.find(id);
        if (it == profile_map.end() || it->second.user_id != user_id) return false;
        it->second.name = name;
        return true;
    }
    bool delete_profile(const std::string& id, const std::string& user_id) override {
        auto it = profile_map.find(id);
        if (it == profile_map.end() || it->second.user_id != user_id) return false;
        profile_map.erase(it);
        example_map.erase(id);
        return true;
    }
    std::vector<stmt::ExampleRec> examples(const std::string& profile_id) override { return example_map[profile_id]; }
    void add_example(const std::string& profile_id, const stmt::ExampleRec& e, int keep) override {
        auto& v = example_map[profile_id];
        v.insert(v.begin(), e);
        if (static_cast<int>(v.size()) > keep) v.resize(static_cast<std::size_t>(keep));
    }
};

#include <stdexcept>

#include "providers.hpp"

// providers::SettingsRepo в памяти — для тестов /api/ai/settings и выбора
// провайдера задания chat без Postgres. up = false — как упавшая база.
class MemSettingsRepo : public providers::SettingsRepo {
public:
    bool up = true;
    std::map<std::string, std::string> provider_by_user;
    std::map<std::pair<std::string, std::string>, providers::Epoch> consent; // (user, provider) -> время

    providers::UserSettings get(const std::string& user_id) override {
        check();
        providers::UserSettings s;
        auto it = provider_by_user.find(user_id);
        if (it != provider_by_user.end()) s.provider = it->second;
        for (const auto& [k, at] : consent) {
            if (k.first == user_id) s.consents[k.second] = at;
        }
        return s;
    }
    void set_provider(const std::string& user_id, const std::string& provider) override {
        check();
        provider_by_user[user_id] = provider;
    }
    void set_consent(const std::string& user_id, const std::string& provider, providers::Epoch at) override {
        check();
        consent[{user_id, provider}] = at;
    }
    void revoke_consent(const std::string& user_id, const std::string& provider) override {
        check();
        consent.erase({user_id, provider});
    }

private:
    void check() const {
        if (!up) throw std::runtime_error("база ai недоступна");
    }
};
