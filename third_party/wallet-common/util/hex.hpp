// Hex (base16) кодек для двоичного содержимого blob/icon.content_sha256
// (org/docs/TODO_mobile_sync.md, Фаза 2 — иконки). Нужен свой, не base64.hpp:
// SQL-параметры db::Database::exec — ТЕКСТОВЫЕ (см. db.hpp, PQexecParams с
// paramLengths=nullptr — любой параметр трактуется как null-terminated
// C-строка), а сырые байты изображения почти наверняка содержат 0x00 внутри —
// такой параметр молча обрезался бы на первом же нулевом байте. Hex-строка
// того же содержимого — чистый ASCII без 0x00, безопасна как текстовый
// параметр; сама вставка/чтение в/из bytea-колонки идёт через
// decode($n,'hex')/encode(col,'hex') на стороне SQL (см. sync_repo.cpp).
#pragma once

#include <string>

namespace hex {

// Байты -> hex-строка (нижний регистр, без разделителей), ровно вдвое длиннее.
std::string encode(const std::string& bytes);

// Hex-строка -> байты. Бросает std::invalid_argument при нечётной длине или
// недопустимом символе (не [0-9a-fA-F]).
std::string decode(const std::string& hex_str);

// Ровно 64 hex-символа — формат SHA-256 (32 байта). Не бросает — для
// валидации входного параметра пути ПЕРЕД тем, как передать его в SQL/decode().
bool looks_like_sha256(const std::string& s);

} // namespace hex
