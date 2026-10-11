#include "util/jwt.hpp"

#include <chrono>

#include "util/base64.hpp"
#include "util/crypto.hpp"

using nlohmann::json;

namespace jwt {

namespace {

std::vector<std::string> split_token(const std::string& token) {
    std::vector<std::string> parts;
    size_t start = 0;
    for (size_t i = 0; i <= token.size(); ++i) {
        if (i == token.size() || token[i] == '.') {
            parts.push_back(token.substr(start, i - start));
            start = i + 1;
        }
    }
    return parts;
}

uint64_t now_epoch() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                      std::chrono::system_clock::now().time_since_epoch())
                                      .count());
}

std::optional<json> parse_json_part(const std::string& b64_part) {
    try {
        auto bytes = b64::decode(b64_part);
        std::string text(bytes.begin(), bytes.end());
        return json::parse(text);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

} // namespace

std::string sign_rs256(const json& payload, const crypto::RsaSigningKey& key) {
    json header = {{"alg", "RS256"}, {"typ", "JWT"}, {"kid", key.kid()}};
    std::string h = b64::encode(header.dump());
    std::string p = b64::encode(payload.dump());
    std::string signing_input = h + "." + p;
    auto sig = key.sign(signing_input);
    return signing_input + "." + b64::encode(sig);
}

std::optional<json> verify_rs256(const std::string& token, const std::vector<unsigned char>& n,
                                  const std::vector<unsigned char>& e, VerifyError* error) {
    auto fail = [error](VerifyError why) -> std::optional<json> {
        if (error != nullptr) *error = why;
        return std::nullopt;
    };

    auto parts = split_token(token);
    if (parts.size() != 3) return fail(VerifyError::Malformed);

    std::string signing_input = parts[0] + "." + parts[1];
    std::vector<unsigned char> sig;
    try {
        sig = b64::decode(parts[2]);
    } catch (const std::exception&) {
        return fail(VerifyError::Malformed);
    }

    if (!crypto::rsa_sha256_verify(n, e, signing_input, sig)) return fail(VerifyError::BadSignature);

    auto payload = parse_json_part(parts[1]);
    if (!payload || !payload->is_object()) return fail(VerifyError::Malformed);

    try {
        if (!payload->contains("exp") || payload->at("exp").get<uint64_t>() <= now_epoch()) {
            return fail(VerifyError::Expired);
        }
    } catch (const std::exception&) {
        return fail(VerifyError::Expired); // exp не число
    }
    return payload;
}

std::optional<std::string> peek_kid(const std::string& token) {
    auto parts = split_token(token);
    if (parts.empty()) return std::nullopt;

    auto header = parse_json_part(parts[0]);
    if (!header || !header->contains("kid")) return std::nullopt;

    try {
        return header->at("kid").get<std::string>();
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

} // namespace jwt
