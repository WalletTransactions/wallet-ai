// Профили формата выписки (statement-profiles, D10): подсказки и примеры
// под банк или шаблон документа для промпта extract_rows и параметры проверки.
// Встроенные — файлы prompts/banks/*.txt, свои форматы пользователя — в базе.
#pragma once

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

struct Profile {
    std::string id;
    std::string name;
    std::string version;
    std::vector<std::string> signature; // все должны найтись в первых 40 строках
    std::string hints;
    std::vector<Example> examples;      // до 3
    CheckRules check;
};

// Разбор файла профиля:
//   # version: tinkoff-1
//   id: tinkoff
//   name: Тинькофф
//   [signature]   — по строке на признак (подстрока, регистр не важен)
//   [hints]       — текст подсказки модели
//   [examples]    — строки выписки, затем строка «=> {json}»; до 3 примеров
//   [check]       — stop_words: …, date_formats: …, expense_word: …
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
