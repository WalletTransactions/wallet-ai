// Чистые функции текста для ответов бота и сопоставления имён — без сети и
// базы, покрыты wallet-telegram/tests/test_text.cpp.
#pragma once

#include <string>
#include <vector>

namespace text {

// Имя для сравнения (org/docs/TODO_telegram_bot.md, §3.1): нижний регистр
// (ASCII и кириллица, UTF-8), ё → е, пробелы по краям убраны, внутренние
// схлопнуты в один. Прочие символы — как есть.
std::string normalize_name(const std::string& s);

struct Named {
    std::string uuid;
    std::string name;
};

struct Match {
    enum class Kind { Found, Ambiguous, NotFound };
    Kind kind = Kind::NotFound;
    Named found;                   // при Found
    std::vector<Named> candidates; // при Ambiguous
};

// Сначала точное совпадение нормализованного имени, затем уникальный
// префикс. Несколько префиксных совпадений — Ambiguous со всеми кандидатами.
Match resolve_name(const std::string& query, const std::vector<Named>& items);

// "12345.6", precision "2", "₽" -> "12 345,60 ₽". Десятичная строка как есть
// (numeric из wallet-report), без double. Разделитель тысяч — узкий
// неразрывный пробел, чтобы сумма не переносилась. Пустой symbol — без него.
// Непарсящаяся строка возвращается как есть.
std::string format_amount(const std::string& amount, const std::string& precision, const std::string& symbol);

// "2026-09-28T12:00:00" / "2026-09-28 12:00:00" / "2026-09-28" -> "28.09".
std::string format_day_month(const std::string& iso);

// Обрезка по границе UTF-8 до max_bytes с "…" — Telegram режет сообщения
// длиннее 4096 символов.
std::string truncate_utf8(const std::string& s, std::size_t max_bytes);

} // namespace text
