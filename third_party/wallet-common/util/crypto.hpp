// Тонкая обёртка над OpenSSL: SHA-256, случайные байты, подпись access-токенов
// (RSA-SHA256/RS256, см. RsaSigningKey — нужна только google-auth) и проверка
// подписи по ключу из JWKS (rsa_sha256_verify — все сервисы).
#pragma once

#include <cstddef>
#include <string>
#include <vector>

// Форвард-декларация вместо #include <openssl/types.h> здесь — чтобы этот
// заголовок (а с ним и весь openssl/evp.h) не тянулся во все .cpp, которые
// просто передают RsaSigningKey дальше (тот же приём, что и с PGconn в
// db.hpp). Полный заголовок нужен только crypto.cpp.
extern "C" {
typedef struct evp_pkey_st EVP_PKEY;
}

namespace crypto {

std::vector<unsigned char> sha256(const std::string& data);

// n криптографически стойких случайных байт (OpenSSL RAND_bytes) — для
// секретов вроде refresh-токенов, где предсказуемость недопустима (в
// отличие от CSRF state/PKCE verifier в google.cpp — те коротко живут и
// одноразовые, там хватает mt19937). Бросает std::runtime_error, если ОС не
// смогла отдать достаточно энтропии (крайне маловероятно, но тихо
// продолжать с плохим генератором для секретов нельзя).
std::vector<unsigned char> random_bytes(std::size_t n);

// Сравнение за постоянное время — чтобы проверка подписи не давала
// атакующему таймингового оракула.
bool constant_time_equal(const std::vector<unsigned char>& a,
                          const std::vector<unsigned char>& b);

// Проверяет подпись RSASSA-PKCS1-v1_5 SHA-256 по сырым модулю/экспоненте
// RSA-ключа — ровно в том формате, в котором их отдаёт JWKS (base64url,
// уже декодированный в байты вызывающим кодом): id_token Google в google-auth,
// наши access-токены во всех сервисах. Возвращает false при любой ошибке
// (битый ключ, неверная подпись и т.п.), исключений не бросает.
bool rsa_sha256_verify(const std::vector<unsigned char>& n,
                        const std::vector<unsigned char>& e,
                        const std::string& signed_data,
                        const std::vector<unsigned char>& signature);

// RSA-ключ, которым auth-сервис подписывает СВОИ access-токены (RS256) —
// см. jwt::sign_rs256. Приватная часть не покидает этот объект; наружу
// (JWKS, см. auth.cpp: GET /.well-known/jwks.json) отдаются только n/e
// публичной части через modulus()/exponent().
//
// RAII, как db::Database: конструктор грузит PEM и бросает
// std::runtime_error при ошибке (файла нет, битый формат, не RSA-ключ и
// т.п.), деструктор освобождает EVP_PKEY. Не копируется — одного
// экземпляра на процесс достаточно (см. AppState::jwt_signing_key).
class RsaSigningKey {
public:
    explicit RsaSigningKey(const std::string& pem_path);
    ~RsaSigningKey();
    RsaSigningKey(const RsaSigningKey&) = delete;
    RsaSigningKey& operator=(const RsaSigningKey&) = delete;

    // RSASSA-PKCS1-v1_5 SHA-256 — тот же формат, что rsa_sha256_verify()
    // ожидает на другом конце.
    std::vector<unsigned char> sign(const std::string& data) const;

    std::vector<unsigned char> modulus() const;
    std::vector<unsigned char> exponent() const;

    // Стабильный id ключа — для заголовка JWT ("kid") и для JWKS, чтобы
    // верифицирующая сторона (wallet-api/wallet-gateway) знала, каким
    // публичным ключом проверять именно этот токен (актуально при ротации
    // ключа — JWKS может отдавать сразу несколько). Выводится из самого
    // ключа (sha256 от DER публичной части), а не из отдельного конфига —
    // не может рассинхронизироваться с файлом ключа.
    const std::string& kid() const { return kid_; }

private:
    EVP_PKEY* pkey_;
    std::string kid_;
};

} // namespace crypto
