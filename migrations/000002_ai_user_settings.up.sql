-- Настройки ИИ пользователя (openspec/changes/add-chat-agent, D7): какой
-- провайдер отвечает в чате и согласия на передачу данных облачному
-- провайдеру. Без строки в ai_user_settings выбран AI_DEFAULT_PROVIDER.
--
-- Согласие — отдельной таблицей на провайдера: вторая облачная модель
-- схему не меняет. Ключей провайдеров здесь нет (вариант b добавит свою
-- таблицу ai_user_keys).
--
-- Права: DML роли сервиса — через ALTER DEFAULT PRIVILEGES в
-- infra/db/init-db.sh, как у 000001. Внешних ключей на пользователей нет —
-- другая база.

CREATE TABLE ai_user_settings (
    user_id    uuid        PRIMARY KEY,
    provider   text        NOT NULL,
    updated_at timestamptz NOT NULL DEFAULT now()
);

CREATE TABLE ai_provider_consent (
    user_id      uuid        NOT NULL,
    provider     text        NOT NULL,
    consented_at timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (user_id, provider)
);
