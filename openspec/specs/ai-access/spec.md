# ai-access Specification

## Purpose

Кто может пользоваться wallet-ai: владелец access-токена google-auth, адресованного
wallet-ai и дающего scope `ai:use`. Сервис токены не выпускает, только проверяет.

## Requirements

### Requirement: Источник токена
Сервис SHALL брать access-токен из заголовка `Authorization: Bearer`, а при его
отсутствии — из cookie `session`. Подпись проверяется только как RS256 ключом из
JWKS google-auth по `kid`; `exp` обязателен и должен быть в будущем.

#### Scenario: Токен из cookie
- **WHEN** браузер через гейтвей шлёт `GET /api/ai/status` с cookie `session` без заголовка `Authorization`
- **THEN** токен из cookie проверяется так же, как Bearer

#### Scenario: Нет токена
- **WHEN** запрос к `/api/ai/*` без токена или с битой подписью
- **THEN** сервис отвечает 401 `{"error": "требуется вход: Authorization: Bearer или cookie session"}`

### Requirement: Адресат и scope
Для путей `/api/ai/*` сервис SHALL до хендлеров требовать `iss: google-auth`,
`wallet-ai` в `aud` (строка или массив) и `ai:use` в `scope`. Чужой `iss` или
`aud` — 401 `invalid_token` «токен выпущен не для wallet-ai»; нет scope — 403
`insufficient_scope` «нужен scope ai:use».

#### Scenario: Токен для другого сервиса
- **WHEN** токен с верной подписью адресован только `wallet-report`
- **THEN** ответ 401 с заголовком `WWW-Authenticate: Bearer error="invalid_token"`, в лог пишется `jwt_rejected`

#### Scenario: Нет ai:use
- **WHEN** токен адресован wallet-ai, но `scope` без `ai:use`
- **THEN** ответ 403 `insufficient_scope`

### Requirement: Токены старого формата не принимаются
Токен с верной подписью, но без `aud`, SHALL отвергаться 401 `invalid_token` с
текстом «токен старого формата — войдите заново».

#### Scenario: Токен без aud
- **WHEN** приходит токен без claim `aud`
- **THEN** ответ 401, в лог — `jwt_rejected` с `reason: legacy`, метрика `jwt_claims_total{result="legacy"}` растёт

### Requirement: Служебные пути без токена
`/healthz`, `/metrics`, `/version`, `/docs` и `/openapi.yaml` SHALL отвечать без
токена.

#### Scenario: Проверка живости
- **WHEN** `GET /healthz` без токена
- **THEN** ответ 200 `{"status":"ok"}`
