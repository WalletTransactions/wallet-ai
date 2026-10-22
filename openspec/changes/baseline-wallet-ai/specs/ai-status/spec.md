# Spec Delta

## Purpose

Клиенты решают, показывать ли функции ИИ, по `GET /api/ai/status`. Ответ строится
по результату фоновой пробы LLM, а не по запросу к модели на каждый вызов.

## ADDED Requirements

### Requirement: Фоновая проба LLM
Сервис SHALL проверять LLM запросом `GET {LLM_URL}/models` при старте и затем
каждые `LLM_PROBE_INTERVAL_SECS` (по умолчанию 60). Модель считается доступной,
если в `data[]` есть элемент с `id == LLM_MODEL`. Таймаут пробы — connect 2 с,
чтение `LLM_TIMEOUT_MS` (15 000 мс).

#### Scenario: Модель загружена
- **WHEN** `GET /models` вернул 200 со списком, где есть `wallet-parse`, а `LLM_MODEL=wallet-parse`
- **THEN** результат пробы — `ok`, метрика `llm_up` равна 1

### Requirement: Причины недоступности
Результат пробы SHALL быть одним из: `disabled` (`LLM_URL` пуст), `unreachable`
(сеть, таймаут, не-2xx кроме 401/403, некорректный `LLM_URL`), `unauthorized`
(401/403), `model_not_loaded` (модели нет в списке), `bad_response` (не JSON или
нет массива `data`).

#### Scenario: Неверный ключ LM Studio
- **WHEN** LM Studio отвечает на `GET /models` 401
- **THEN** результат — `unauthorized`, в лог пишется ошибка «LLM answered 401/403 — check LLM_API_KEY»

### Requirement: Лог только при смене состояния
Сервис SHALL писать в лог результат пробы только когда он изменился по сравнению
с прошлым, а метрику `llm_up` обновлять после каждой пробы.

#### Scenario: Стабильное состояние
- **WHEN** десять проб подряд дают `ok`
- **THEN** в логе одна запись «LLM available»

### Requirement: Ответ status
`GET /api/ai/status` SHALL отвечать 200 с `enabled` (проба `ok`), `model`
(`LLM_MODEL` или `null`), `reason` (`null` или результат пробы) и `features`:
`["parse_expense"]` при включённом ИИ, пустой массив при выключенном.

#### Scenario: ИИ выключен
- **WHEN** последняя проба дала `unreachable`
- **THEN** ответ `{"enabled": false, "model": null, "reason": "unreachable", "features": []}`

#### Scenario: ИИ включён
- **WHEN** последняя проба дала `ok`
- **THEN** ответ `{"enabled": true, "model": "wallet-parse", "reason": null, "features": ["parse_expense"]}`

### Requirement: LLM необязательна для старта
Недоступная или не настроенная LLM SHALL NOT мешать старту сервиса: он
поднимается и отвечает `enabled: false`.

#### Scenario: Старт без LM Studio
- **WHEN** сервис стартует с `LLM_URL`, по которому никто не отвечает
- **THEN** HTTP-сервер слушает порт, `/healthz` отвечает 200, status — `enabled: false`
