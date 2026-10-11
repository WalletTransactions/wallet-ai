// Строгий формат траты (org/docs/TODO_telegram_bot.md, §3.1):
//
//   <сумма> [<валюта>] <категория> [@<кошелёк>] [<дата>] [// <комментарий>]
//
// Чистая функция: текст, имена пользователя и «сейчас» в его поясе →
// разобранные поля. Без сети, базы и часов — таблица тестов в
// wallet-telegram/tests/test_strict.cpp.
//
// Парсер только режет текст на поля. Какой это счёт (uuid), что делать с
// неоднозначностью и подходит ли валюта кошельку, решает resolver (§3.2).
// Имена ему всё же нужны: без «//» граница между многословной категорией и
// комментарием видна только по списку имён (самое длинное совпадение).
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace parser {

struct Currency {
    std::string iso_code; // "RUB"
    std::string symbol;   // "₽", может быть пустым
};

struct Names {
    std::vector<std::string> categories;
    std::vector<std::string> wallets;
    // Валюты кошельков пользователя: по ним узнаются ISO-коды и символы
    // помимо встроенных синонимов (руб, евро, $ …).
    std::vector<Currency> currencies;
};

struct Now {
    std::string date; // "YYYY-MM-DD" в поясе пользователя
    std::string time; // "HH:MM:SS" там же
};

struct ParsedExpense {
    // Десятичная строка с точкой, без ведущих нулей и хвостовых нулей
    // дробной части: "1200.5", "350". Всегда > 0.
    std::string amount;
    // Число знаков после точки в amount — resolver сверяет его с precision
    // валюты выбранного кошелька.
    int fraction_digits = 0;
    // ISO-код валюты из сообщения, пусто — не указана. Не поле операции, а
    // подсказка для выбора кошелька (§3.2).
    std::string currency_hint;
    // Текст категории как в сообщении (пусто — не указана, resolver
    // спросит кнопками).
    std::string category;
    // Текст после '@' (пусто — не указан).
    std::string wallet;
    // Дата операции "YYYY-MM-DD HH:MM:SS" (timestamp без TZ): сегодня —
    // текущее время, прошедший день — 12:00.
    std::string datetime;
    std::string comment;
};

struct ParseError {
    enum class Reason {
        NoAmount,   // сообщение не начинается с суммы — это не трата
        BadAmount,  // "1,200.50", "350кафе", слишком длинная сумма
        ZeroAmount, // "0", "0,00"
        BadDate,    // "31.02", "32.13"
        FutureDate, // дата позже сегодняшней
    };
    Reason reason = Reason::NoAmount;
};

// Имя причины для метрики tg_parse_fail_total{reason}.
const char* reason_name(ParseError::Reason reason);

struct Result {
    bool ok = false;
    ParsedExpense expense; // при ok
    ParseError error;      // при !ok
};

Result parse_strict(const std::string& text, const Names& names, const Now& now);

// --- Части строгого разбора для проверки ответа LLM (wallet-ai, задание
// parse_expense, «Шаг 9 подробно», 9c.5). Те же правила, что у parse_strict.

struct Amount {
    std::string amount;     // как ParsedExpense::amount: "1200.5"
    int fraction_digits = 0;
};

// Число без валюты и пробелов: "1200,50", "350", "12.5". Тот же разбор, что у
// суммы в parse_strict: один десятичный разделитель, ≤ 12 знаков до и ≤ 8
// после. nullopt — не число, why — BadAmount или ZeroAmount.
std::optional<Amount> parse_amount(const std::string& core, ParseError::Reason* why = nullptr);

// Все суммы, что встречаются в тексте, нормализованные как Amount::amount:
// "1 200,50 продукты, $12 и 350₽" -> {"1200.5", "12", "350"}. Группы тысяч
// склеиваются, валюта вокруг числа отрезается. Даты ("24.09") тоже попадут
// — для проверки «сумма есть в тексте» это не мешает.
std::vector<std::string> amounts_in_text(const std::string& text, const Names& names);

// ISO-код валюты, которую означает слово ("евро", "₽", "usd", символ или
// код валюты кошелька), или пусто.
std::string currency_of(const std::string& word, const Names& names);

enum class DateCheck { Ok, Bad, Future, TooOld };

// "YYYY-MM-DD" против сегодняшней даты пользователя: Bad — не дата, Future —
// позже сегодня, TooOld — раньше чем max_age_days назад.
DateCheck check_date(const std::string& ymd, const std::string& today, int max_age_days);

// Момент операции, как в ParsedExpense::datetime: сегодня — текущее время,
// прошедший день — 12:00.
std::string datetime_for(const std::string& ymd, const Now& now);

} // namespace parser
