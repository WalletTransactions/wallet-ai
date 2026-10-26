// Мелочи UTF-8 для разбора выписок: регистр кириллицы и латиницы, пробелы
// (включая неразрывные), разбиение на строки. Без ICU — выписки русские и
// латинские, этого хватает.
#pragma once

#include <string>
#include <vector>

namespace stmt {

// Верхний/нижний регистр для ASCII и кириллицы (А–Я, Ё); остальное как есть.
std::string upper(const std::string& s);
std::string lower(const std::string& s);

// Неразрывный (U+00A0), узкий неразрывный (U+202F) и тонкий (U+2009) пробелы
// и табуляция — в обычный пробел.
std::string normalize_spaces(const std::string& s);
// normalize_spaces, затем серии из 3+ пробелов → два, хвостовые пробелы убраны:
// текст для LLM. pdftotext -layout выравнивает колонки пробелами, и строка
// таблицы на 80 % из них состоит — это токены и время генерации. Двух пробелов
// хватает, чтобы колонки читались раздельно.
std::string squeeze_spaces(const std::string& s);

std::string trim(const std::string& s);

// Строки текста без \r; последняя пустая строка после \n не добавляется.
std::vector<std::string> split_lines(const std::string& text);

bool has_digit(const std::string& s);

// Число кодовых точек UTF-8 (лимиты в символах, а не байтах).
std::size_t utf8_length(const std::string& s);

// Корректный ли UTF-8 (CSV/TXT: иначе читаем как Windows-1251).
bool valid_utf8(const std::string& s);

// Обрезка до max_chars символов по границе кодовой точки.
std::string utf8_prefix(const std::string& s, std::size_t max_chars);

} // namespace stmt
