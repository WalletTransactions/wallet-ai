// Профили формата выписки (statement-profiles, D10): подсказки и примеры
// под банк или шаблон документа для промпта extract_rows и параметры проверки.
// Встроенные — файлы prompts/banks/*.txt, свои форматы пользователя — в базе.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"
#include "statement/verify.hpp"

namespace stmt {

// Пример «строки-источники → строка ответа модели» (без lines: их номера
// проставляются при сборке промпта).
struct Example {
    std::string source;       // одна или несколько строк выписки
    nlohmann::json expected;  // {date, amount, dir, currency?, description}
};

// Колонки CSV (секция [columns], D11): имена из строки заголовка.
struct Columns {
    char delimiter = ';';
    std::string date;        // имя колонки
    std::string date_format; // «DD-MM-YYYY HH:MM:SS»; время не разбирается
    std::string amount;      // сумма со знаком
    std::string description;
    std::string type;        // необязательна; значения — в [types]
    std::string currency;    // необязательна
};

struct Profile {
    std::string id;
    std::string name;
    std::string version;
    std::vector<std::string> signature; // все должны найтись в первых 40 строках
    std::string hints;
    std::vector<Example> examples;      // до 3
    CheckRules check;
    Columns columns;                           // date пусто — разбор только через LLM
    std::map<std::string, std::string> types;  // тип (lower) → expense | income | transfer
};

// Строка данных CSV, разобранная по колонкам: candidate идёт в ту же проверку
// D3, что и строка от модели.
struct ColumnRow {
    int line = 0; // номер строки в тексте, с 1
    Candidate candidate;
};

// Разбор CSV по [columns] без LLM. nullopt — у профиля нет колонок или в
// заголовке (первая непустая строка) нет нужной колонки: тогда импорт идёт
// через LLM. Строки, где не разобрались дата или сумма, в ответ не попадают.
std::optional<std::vector<ColumnRow>> parse_by_columns(const Profile& p, const std::vector<std::string>& lines);

// Разбор файла профиля:
//   # version: tinkoff-1
//   id: tinkoff
//   name: Тинькофф
//   [signature]   — по строке на признак (подстрока, регистр не важен)
//   [hints]       — текст подсказки модели
//   [examples]    — строки выписки, затем строка «=> {json}»; до 3 примеров
//   [check]       — stop_words: …, date_formats: …, expense_word: …, transfer_words: …
//   [columns]     — delimiter, date: Имя | DD-MM-YYYY, amount, description, type, currency
//   [types]       — «Значение колонки type: expense|income|transfer»
// Строки с # — комментарии. Ошибка формата — std::runtime_error.
Profile parse_profile(const std::string& content);

// *.txt из каталога; битый файл пропускается с applog warn profile_invalid.
std::vector<Profile> load_profiles(const std::string& dir);

const Profile* find_profile(const std::vector<Profile>& profiles, const std::string& id);

// id встроенных профилей, все признаки которых есть в первых 40 строках.
std::vector<std::string> matching_profiles(const std::vector<Profile>& profiles,
                                           const std::vector<std::string>& lines);

// Отпечаток формата: sha256 нормализованной строки заголовка колонок. CSV —
// первая непустая строка; текст и PDF — первая строка с тремя и более словами
// словаря заголовков («дата», «сумма», «описание», «операция», «валюта»,
// «остаток»). Пусто — заголовка нет.
std::string format_fingerprint(const std::vector<std::string>& lines, bool csv);

// Часть системного промпта extract_rows: подсказки встроенного профиля и
// примеры — сначала свои, затем встроенные — пока оценка (символы / 3) не
// превысит max_tokens. builtin может быть nullptr.
std::string profile_prompt(const Profile* builtin, const std::vector<Example>& own, std::size_t max_tokens);

// Оценка токенов, которой пользуются бюджеты профиля и фрагмента.
std::size_t estimate_tokens(const std::string& s);

} // namespace stmt
