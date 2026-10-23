// Текст выписки из файла (statement-import, «Извлечение текста из файла»):
// PDF — pdftotext через pipe, CSV/TXT — UTF-8 или Windows-1251. Файл на диск
// не пишется.
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace stmt {

struct SourceResult {
    // none | unsupported_file (400) | too_large (413) | bad_base64 (400) |
    // no_text (импорт создаётся сразу в failed) | pdf_failed (импорт failed)
    std::string error;
    std::string text;
    bool csv = false;
};

// Стандартный base64 и base64url, пробелы и переводы строк пропускаются.
std::optional<std::vector<unsigned char>> decode_base64(const std::string& s);

std::string cp1251_to_utf8(const std::string& s);

// UTF-8 как есть (BOM срезается), иначе Windows-1251.
std::string decode_text(const std::string& bytes);

// pdftotext -layout - - с лимитами памяти и времени. nullopt — процесс не
// запустился, упал или не уложился в лимиты.
std::optional<std::string> pdf_to_text(const std::string& pdf, int timeout_secs, std::size_t memory_bytes);

// Имя файла и содержимое base64 → текст. max_bytes — после декодирования.
SourceResult extract_file(const std::string& name, const std::string& content_base64, std::size_t max_bytes);

} // namespace stmt
