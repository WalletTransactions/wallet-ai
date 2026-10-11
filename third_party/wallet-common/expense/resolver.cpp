#include "expense/resolver.hpp"

#include <algorithm>
#include <cctype>
#include <unordered_set>

#include "expense/text.hpp"

namespace resolver {

namespace {

template <typename T>
std::vector<T> first_n(std::vector<T> items, std::size_t n = kMaxCandidates) {
    if (items.size() > n) items.resize(n);
    return items;
}

template <typename T>
std::vector<text::Named> to_named(const std::vector<T>& items) {
    std::vector<text::Named> out;
    for (const auto& i : items) out.push_back({i.uuid, i.name});
    return out;
}

// Кандидаты resolve_name обратно в полные записи — в исходном порядке.
template <typename T>
std::vector<T> by_uuid(const std::vector<T>& items, const std::vector<text::Named>& picked) {
    std::unordered_set<std::string> uuids;
    for (const auto& p : picked) uuids.insert(p.uuid);
    std::vector<T> out;
    for (const auto& i : items) {
        if (uuids.count(i.uuid) != 0) out.push_back(i);
    }
    return out;
}

template <typename T>
const T* find_uuid(const std::vector<T>& items, const std::string& uuid) {
    if (uuid.empty()) return nullptr;
    for (const auto& i : items) {
        if (i.uuid == uuid) return &i;
    }
    return nullptr;
}

std::vector<Wallet> of_currency(const std::vector<Wallet>& wallets, const std::string& iso) {
    std::vector<Wallet> out;
    for (const auto& w : wallets) {
        if (w.iso_code == iso) out.push_back(w);
    }
    return out;
}

CategoryPick ask_category(CategoryPick::Why why, std::vector<Category> candidates) {
    CategoryPick p;
    p.why = why;
    p.candidates = first_n(std::move(candidates));
    return p;
}

WalletPick found_wallet(const Wallet& w, bool by_currency = false) {
    WalletPick p;
    p.kind = WalletPick::Kind::Found;
    p.found = w;
    p.by_currency = by_currency;
    return p;
}

WalletPick ask_wallet(WalletPick::Why why, std::vector<Wallet> candidates) {
    WalletPick p;
    p.why = why;
    p.candidates = std::move(candidates);
    return p;
}

WalletPick reject_wallet(WalletPick::Why why) {
    WalletPick p;
    p.kind = WalletPick::Kind::Reject;
    p.why = why;
    return p;
}

CategoryPick pick_category(const parser::ParsedExpense& e, const std::vector<Category>& cats) {
    if (e.category.empty()) return ask_category(CategoryPick::Why::Missing, cats);

    text::Match m = text::resolve_name(e.category, to_named(cats));
    if (m.kind == text::Match::Kind::Found) {
        CategoryPick p;
        p.kind = CategoryPick::Kind::Found;
        p.found = *find_uuid(cats, m.found.uuid);
        return p;
    }
    if (m.kind == text::Match::Kind::Ambiguous) {
        return ask_category(CategoryPick::Why::Ambiguous, by_uuid(cats, m.candidates));
    }
    // Не префикс — может быть, середина имени: «рест» → «Кафе и рестораны».
    std::string q = text::normalize_name(e.category);
    std::vector<Category> containing;
    for (const auto& c : cats) {
        if (text::normalize_name(c.name).find(q) != std::string::npos) containing.push_back(c);
    }
    return ask_category(CategoryPick::Why::NotFound, std::move(containing));
}

// Кошелёк указан в сообщении («@имя»).
WalletPick pick_named_wallet(const parser::ParsedExpense& e, const std::vector<Wallet>& wallets) {
    const std::string& hint = e.currency_hint;
    text::Match m = text::resolve_name(e.wallet, to_named(wallets));

    if (m.kind == text::Match::Kind::Found) {
        const Wallet& w = *find_uuid(wallets, m.found.uuid);
        if (hint.empty() || w.iso_code == hint) return found_wallet(w);
        // Валюта не та: не отказ, а вопрос — кошельки этой валюты и «Всё
        // равно в <кошелёк>». Кошельков этой валюты может не быть вовсе.
        WalletPick p = ask_wallet(WalletPick::Why::CurrencyMismatch, of_currency(wallets, hint));
        p.force = w;
        return p;
    }
    if (m.kind == text::Match::Kind::Ambiguous) {
        // «Тинькофф евро» и «Тинькофф рубли» при «@тинькофф 12 евро» —
        // валюта выбирает.
        std::vector<Wallet> candidates = by_uuid(wallets, m.candidates);
        if (!hint.empty()) {
            std::vector<Wallet> same = of_currency(candidates, hint);
            if (same.size() == 1) return found_wallet(same[0], /*by_currency=*/true);
            if (!same.empty()) return ask_wallet(WalletPick::Why::Ambiguous, std::move(same));
        }
        return ask_wallet(WalletPick::Why::Ambiguous, std::move(candidates));
    }
    std::vector<Wallet> same = hint.empty() ? std::vector<Wallet>{} : of_currency(wallets, hint);
    return ask_wallet(WalletPick::Why::NotFound, same.empty() ? wallets : std::move(same));
}

// Кошелёк не указан: по умолчанию, по валюте или вопрос.
WalletPick pick_default_wallet(const parser::ParsedExpense& e, const std::vector<Wallet>& wallets,
                               const std::string& default_wallet_uuid) {
    const std::string& hint = e.currency_hint;
    const Wallet* def = find_uuid(wallets, default_wallet_uuid);
    // Единственный кошелёк — он и есть кошелёк по умолчанию, спрашивать не о чем.
    if (def == nullptr && wallets.size() == 1) def = &wallets[0];

    if (hint.empty()) {
        if (def != nullptr) return found_wallet(*def);
        WalletPick p = ask_wallet(WalletPick::Why::NotGiven, wallets);
        p.offer_remember = true;
        return p;
    }
    if (def != nullptr && def->iso_code == hint) return found_wallet(*def);
    std::vector<Wallet> same = of_currency(wallets, hint);
    if (same.empty()) return reject_wallet(WalletPick::Why::NoWalletInCurrency);
    if (same.size() == 1) return found_wallet(same[0], /*by_currency=*/true);
    WalletPick p = ask_wallet(WalletPick::Why::NotGiven, std::move(same));
    p.offer_remember = def == nullptr;
    return p;
}

} // namespace

Resolution resolve(const parser::ParsedExpense& e, const Accounts& a, const std::string& default_wallet_uuid) {
    Resolution r;
    r.category = pick_category(e, a.categories);
    if (a.wallets.empty()) {
        r.wallet = reject_wallet(WalletPick::Why::NoWallets);
    } else if (!e.wallet.empty()) {
        r.wallet = pick_named_wallet(e, a.wallets);
    } else {
        r.wallet = pick_default_wallet(e, a.wallets, default_wallet_uuid);
    }
    return r;
}

bool fits_precision(const parser::ParsedExpense& e, const Wallet& w) { return e.fraction_digits <= w.precision; }

parser::Names names_for_parser(const Accounts& a) {
    parser::Names names;
    for (const auto& c : a.categories) names.categories.push_back(c.name);
    std::unordered_set<std::string> seen;
    for (const auto& w : a.wallets) {
        names.wallets.push_back(w.name);
        if (!w.iso_code.empty() && seen.insert(w.iso_code).second) {
            names.currencies.push_back({w.iso_code, w.symbol});
        }
    }
    return names;
}

namespace {

std::string str(const nlohmann::json& obj, const char* key) {
    auto it = obj.find(key);
    if (it == obj.end() || !it->is_string()) return {};
    return it->get<std::string>();
}

int precision_of(const std::string& s) {
    if (s.empty() || s.size() > 2 || !std::all_of(s.begin(), s.end(), [](unsigned char c) {
            return std::isdigit(c) != 0;
        })) {
        return 2;
    }
    return std::stoi(s);
}

} // namespace

Accounts parse_accounts(const nlohmann::json& body) {
    Accounts out;
    auto it = body.find("accounts");
    if (it == body.end() || !it->is_array()) return out;
    for (const auto& a : *it) {
        std::string uuid = str(a, "uuid");
        if (uuid.empty()) continue;
        std::string kind = str(a, "kind");
        if (kind == "wallet") {
            out.wallets.push_back(
                {uuid, str(a, "name"), str(a, "iso_code"), str(a, "symbol"), precision_of(str(a, "precision"))});
        } else if (kind == "category" && a.value("system_kind", 0) == 0) {
            out.categories.push_back({uuid, str(a, "name")});
        }
    }
    return out;
}

} // namespace resolver
