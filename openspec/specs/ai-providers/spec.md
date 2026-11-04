# ai-providers Specification

## Purpose
Какая модель отвечает в чате пользователя: перечень провайдеров (локальная
LM Studio, Claude, позже своя модель), их доступность, выбор и согласие
пользователя, ключ на каждый вызов и учёт расхода по провайдеру.

## Requirements

### Requirement: Перечень провайдеров
Сервис SHALL знать провайдеров `lmstudio` (OpenAI-совместимый API по
`LLM_URL`) и `claude` (Anthropic Messages API). `lmstudio` доступен, когда
задан `LLM_URL` и последняя проба LLM `ok`. `claude` доступен пользователю,
когда задан `ANTHROPIC_API_KEY` и пользователь входит в
`ANTHROPIC_ALLOWED_USERS` (список `user_id` через запятую или `*`).

#### Scenario: Claude не настроен
- **WHEN** `ANTHROPIC_API_KEY` пуст
- **THEN** в настройках `claude` значится недоступным с `reason: not_configured`

#### Scenario: Пользователь не в списке
- **WHEN** `ANTHROPIC_API_KEY` задан, а `user_id` нет в `ANTHROPIC_ALLOWED_USERS`
- **THEN** у этого пользователя `claude` недоступен с `reason: not_allowed`

### Requirement: Чтение настроек ИИ
`GET /api/ai/settings` SHALL возвращать выбранного провайдера пользователя и
список `providers` с `id`, `title`, `cloud`, `available`, `reason` (если
недоступен), `requires_consent` и `consented` (схема — `openapi.yaml`,
`AiSettings`). Без сохранённого выбора выбран `AI_DEFAULT_PROVIDER`
(`lmstudio`).

#### Scenario: Новый пользователь
- **WHEN** пользователь ни разу не выбирал провайдера
- **THEN** `provider` = `lmstudio`, у `claude` `consented: false`

### Requirement: Выбор провайдера
`PUT /api/ai/settings` `{provider, consent?}` SHALL сохранять выбор
пользователя в базе ai. Недоступный или неизвестный провайдер — 400
`{"error":"provider_unavailable"}`. Облачный провайдер без сохранённого или
переданного `consent: true` — 409 `{"error":"consent_required"}`. Без
`DATABASE_URL` — 503 `{"error":"settings_unavailable"}`.

#### Scenario: Выбор Claude с согласием
- **WHEN** пользователь из списка шлёт `{"provider":"claude","consent":true}`
- **THEN** ответ 200 с новыми настройками, `consented: true` и время согласия сохранены

#### Scenario: Выбор Claude без согласия
- **WHEN** пользователь без сохранённого согласия шлёт `{"provider":"claude"}`
- **THEN** ответ 409 `{"error":"consent_required"}`, выбор не меняется

### Requirement: Отзыв согласия
`PUT /api/ai/settings` с `consent: false` SHALL стирать согласие на облачного
провайдера. Если он был выбран, выбор переходит на `AI_DEFAULT_PROVIDER`.

#### Scenario: Пользователь передумал
- **WHEN** у пользователя выбран `claude`, и он шлёт `{"provider":"claude","consent":false}`
- **THEN** согласие стёрто, `provider` = `lmstudio`

### Requirement: Провайдер задания фиксируется при создании
Задание `chat` SHALL выполняться провайдером, выбранным пользователем на
момент создания. Если тот недоступен, создание отклоняется 503
`{"error":"provider_unavailable","provider":"<id>"}`. Сервис SHALL NOT
молча переключать задание на другого провайдера.

#### Scenario: LM Studio выключена, выбран lmstudio
- **WHEN** проба LLM не `ok`, у пользователя выбран `lmstudio`, и он создаёт `chat`
- **THEN** ответ 503 `provider_unavailable`, Claude не используется

#### Scenario: Выбран Claude, LM Studio выключена
- **WHEN** проба LLM не `ok`, у пользователя выбран `claude` с согласием
- **THEN** задание `chat` создаётся и выполняется Claude

### Requirement: Ключ провайдера на каждый вызов
Ключ облачного провайдера SHALL определяться на каждый вызов модели по паре
(пользователь, провайдер), вместе с его владельцем (`instance` — ключ стека).
Ключ не хранится в задании, не попадает в журнал и в ответы API.

#### Scenario: Ключ сменили в .env
- **WHEN** `ANTHROPIC_API_KEY` поменяли и сервис перезапустили
- **THEN** новые задания используют новый ключ, в журнале и `GET /api/ai/settings` ключа нет

### Requirement: Отказ ключа провайдера
Ответ провайдера 401/403 SHALL завершать задание `failed:
provider_auth_failed`. Если владелец ключа `instance`, сервис пишет в журнал
ошибку `provider_auth_failed` с провайдером и владельцем, чтобы её увидел
администратор.

#### Scenario: Ключ Anthropic отозван
- **WHEN** Anthropic отвечает 401 на вызов модели
- **THEN** задание `failed` с `failure: provider_auth_failed`, в журнале ошибка с `provider: claude`, `owner: instance`

### Requirement: Учёт расхода по провайдеру
Сервис SHALL учитывать токены каждого вызова модели в
`ai_chat_tokens_total{provider,direction}` (`input`/`output`) и писать в
журнал завершения задания `user_id`, `provider` и суммы токенов.

#### Scenario: Вопрос к Claude
- **WHEN** задание `chat` с Claude сделало 3 вызова модели
- **THEN** счётчики `ai_chat_tokens_total{provider="claude"}` выросли на сумму `usage` трёх ответов
