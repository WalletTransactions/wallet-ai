// Проверка строк выписки кодом (add-statement-import, D3): модель только
// указывает, где операция, а сумма и дата должны буквально найтись в её
// строках-источниках. Чистые функции — табличные тесты в test_statement.cpp.
#pragma once

#include <optional>
#include <set>
#include <string>
#include <vector>

namespace stmt {

struct Date {
    int y = 0; // 0 — год в тексте не указан («12.09», «12 сентября»)
    int m = 0;
    int d = 0;
};

bool parse_iso(const std::string& s, Date& out);
std::string to_iso(int y, int m, int d);
long days_from_civil(int y, int m, int d);

// Сумма в строке. cents — модуль в копейках; sign — '-', '+' или 0.
struct Amount {
    long long cents = 0;
    char sign = 0;
    bool two_decimals = false; // «1 290,00», а не «36.6» или «4021»
};

// Даты «ДД.ММ.ГГГГ», «ДД.ММ.ГГ», «ДД.ММ», «ГГГГ-ММ-ДД», «12 сентября [2026]».
// formats — разрешённые профилем (DD.MM.YYYY, DD.MM.YY, DD.MM, YYYY-MM-DD,
// D MONTH); пусто — все.
std::vector<Date> dates_in_line(const std::string& line, const std::vector<std::string>& formats = {});

// Суммы строки; даты и время из неё перед этим вырезаются, чтобы «12.09» в
// начале строки не стало суммой 12.09.
std::vector<Amount> amounts_in_line(const std::string& line);

// Сумма модели («450», «450.00», «1 290,00», «1,290.00») → модуль в копейках.
std::optional<long long> parse_amount(const std::string& s);
std::string format_cents(long long cents); // 129000 → «1290.00»

// Есть ли в строке денежная сумма — число с двумя знаками после разделителя.
bool money_line(const std::string& line);

// Строка — заголовок группы операций по дате («12 сентября 2026», «12.09.2026, суббота»).
bool is_date_header(const std::string& line);

// Итоги и остатки («Итого», «Остаток», «Баланс» + стоп-слова профиля).
bool is_total_line(const std::string& line, const std::vector<std::string>& extra_stop_words);

// Период выписки: первая из первых 40 строк с двумя полными датами.
struct Period {
    std::string from; // ISO или пусто
    std::string to;
};
Period find_period(const std::vector<std::string>& lines);

// Параметры проверки из профиля формата (секция [check]).
struct CheckRules {
    std::vector<std::string> stop_words;
    std::vector<std::string> date_formats;
    std::string expense_word; // «Списание»: строка с этим словом — расход
};

// Строка-кандидат от модели.
struct Candidate {
    std::vector<int> lines;
    std::string date;
    std::string amount;
    std::string dir;
    std::string currency;
    std::string description;
};

struct VerifyContext {
    const std::vector<std::string>* lines = nullptr; // весь текст, строка N — (*lines)[N-1]
    int chunk_first = 1;
    int chunk_last = 0;
    std::string date_context; // заголовок даты из прошлого фрагмента
    std::string today;        // дата создания импорта, ISO
    Period period;
    std::string wallet_currency;
    CheckRules rules;
};

struct Verified {
    std::vector<int> lines;   // отсортированы, без повторов, в пределах фрагмента
    std::string date;         // ISO; пусто — дата модели не разобралась
    std::string amount;       // «1290.00» — из документа после нормализации
    std::string dir;          // expense | income | transfer (знак исходника перекрывает модель)
    std::string currency;
    std::string description;
    std::string check;        // пусто | amount_not_in_text | date_not_in_text | currency_mismatch
};

// nullopt — у строки нет ни одной строки-источника внутри фрагмента: её
// нечем подтвердить и некуда показать, такая строка отбрасывается.
std::optional<Verified> verify_row(const Candidate& c, const VerifyContext& ctx);

// Строки first..last с денежной суммой, не вошедшие в covered и не итоги.
std::vector<int> uncovered_lines(const std::vector<std::string>& lines, int first, int last,
                                 const std::set<int>& covered, const std::vector<std::string>& stop_words);

} // namespace stmt
