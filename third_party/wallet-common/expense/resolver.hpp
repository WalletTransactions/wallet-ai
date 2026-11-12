// Разрешение разобранной траты в счета пользователя
// (org/docs/TODO_telegram_bot.md, §3.2 и «Подшаг 2 подробно»): какой это
// кошелёк и какая категория (uuid), а если однозначно не выбрать — кого
// предложить кнопками.
//
// Чистая функция, как и парсер: разобранная трата, справочник счетов и
// кошелёк по умолчанию → решение. Без сети и базы — таблица тестов в
// wallet-telegram/tests/test_resolver.cpp.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "expense/strict.hpp"
#include "nlohmann/json.hpp"

namespace resolver {

struct Wallet {
    std::string uuid;
    std::string name;
    std::string iso_code;
    std::string symbol;
    int precision = 2;
};

struct Category {
    std::string uuid;
    std::string name;
};

// Справочник счетов пользователя из GET /api/reports/accounts: только живые
// и не архивные, категории — без системных (Transfer и т.п.), кошельки — без
// долговых (counterparty_uuid не null, change debts). Порядок — как в ответе
// (по имени), в нём же показываются кандидаты.
struct Accounts {
    std::vector<Wallet> wallets;
    std::vector<Category> categories;
};

// Сколько кандидатов показывать кнопками; остальные — под «Другая…».
constexpr std::size_t kMaxCandidates = 6;

struct CategoryPick {
    enum class Kind { Found, Ask };
    // При Ask — почему спрашиваем (текст вопроса).
    enum class Why { None, Missing, NotFound, Ambiguous };
    Kind kind = Kind::Ask;
    Why why = Why::None;
    Category found;                   // при Found
    std::vector<Category> candidates; // при Ask, ≤ kMaxCandidates, в порядке показа
};

struct WalletPick {
    enum class Kind { Found, Ask, Reject };
    enum class Why {
        None,
        NotGiven,           // Ask: кошелёк не указан, по умолчанию не выбран
        NotFound,           // Ask: «@имя» не нашлось
        Ambiguous,          // Ask: «@имя» подходит к нескольким
        CurrencyMismatch,   // Ask: кошелёк однозначен, валюта в сообщении другая
        NoWalletInCurrency, // Reject: кошелёк не указан, кошельков валюты из сообщения нет
        NoWallets,          // Reject: кошельков нет вовсе
    };
    Kind kind = Kind::Ask;
    Why why = Why::None;
    Wallet found;                   // при Found
    bool by_currency = false;       // Found выбран по currency_hint — назвать в карточке
    std::vector<Wallet> candidates; // при Ask
    std::optional<Wallet> force;    // «Всё равно в <кошелёк>» (CurrencyMismatch)
    bool offer_remember = false;    // «Запомнить как основной»: кошелька по умолчанию нет
};

struct Resolution {
    CategoryPick category;
    WalletPick wallet;
};

// default_wallet_uuid, которого нет среди wallets (кошелёк удалили или
// отправили в архив), считается «не задан».
Resolution resolve(const parser::ParsedExpense& e, const Accounts& a, const std::string& default_wallet_uuid);

// Знаков после запятой в сумме не больше precision валюты кошелька (§3.2:
// проверяется после выбора кошелька). Округления нет — это деньги.
bool fits_precision(const parser::ParsedExpense& e, const Wallet& w);

// Имена и валюты для parse_strict: без имён парсер не отделит многословную
// категорию от комментария.
parser::Names names_for_parser(const Accounts& a);

// Ответ GET /api/reports/accounts (без kind — кошельки и категории разом).
// Системные категории (Transfer, Transfer commission) и долговые кошельки
// (counterparty_uuid не null; поля нет — не долговой) — не для трат.
Accounts parse_accounts(const nlohmann::json& body);

} // namespace resolver
