-- wallet_ai — собственная база wallet-ai: импорты банковских выписок
-- (openspec/changes/add-statement-import, D1). Не часть домена кошелька: в
-- кошелёк пишет клиент через /sync/push, здесь — только черновики разбора и
-- то, что нужно для следующих импортов (правила продавцов, отпечатки
-- записанных строк, свои форматы).
--
-- Права: роль сервиса (AI_POSTGRES_USER) получает только DML через
-- ALTER DEFAULT PRIVILEGES в infra/db/init-db.sh; DDL — мигратор.
-- Внешних ключей на пользователей и кошельки нет и быть не может — другие базы.

-- Импорт — одна выписка. source_text — извлечённый текст (файл на диск не
-- пишется); нужен разбору и показу строки-источника при проверке.
CREATE TABLE statement_import (
    id                 uuid        PRIMARY KEY,
    user_id            uuid        NOT NULL,
    wallet_uuid        uuid        NOT NULL,
    status             text        NOT NULL CHECK (status IN
                           ('queued', 'extracting', 'classifying', 'ready', 'done', 'failed', 'cancelled')),
    failure            text,
    progress_done      integer     NOT NULL DEFAULT 0,
    progress_total     integer     NOT NULL DEFAULT 0,
    source_name        text        NOT NULL DEFAULT '',
    source_text        text        NOT NULL,
    -- Ответ GET /api/reports/accounts на момент создания: фоновый разбор идёт
    -- без токена пользователя (D7).
    accounts_snapshot  jsonb       NOT NULL,
    period_from        date,
    period_to          date,
    reconciled_at      timestamptz,
    -- Профиль формата (statement-profiles).
    format_fingerprint text,
    profile_id         text,
    profile_version    text,
    profile_source     text CHECK (profile_source IN ('auto', 'manual')),
    profile_detect     text CHECK (profile_detect IN ('none', 'ambiguous')),
    -- Когда проба LLM впервые стала неуспешной посреди разбора: пауза
    -- дольше AI_IMPORT_PAUSE_MAX_MINS — failed: unavailable.
    paused_since       timestamptz,
    created_at         timestamptz NOT NULL DEFAULT now(),
    done_at            timestamptz,
    expires_at         timestamptz NOT NULL
);

CREATE INDEX statement_import_user_id_idx ON statement_import (user_id, created_at DESC);
CREATE INDEX statement_import_expires_at_idx ON statement_import (expires_at);
-- Возобновление после рестарта и планировщик: импорты в работе.
CREATE INDEX statement_import_active_idx ON statement_import (status)
    WHERE status IN ('queued', 'extracting', 'classifying');

-- Фрагмент текста под контекст модели (statement-extraction). Строки
-- first_line..last_line — номера строк source_text с 1.
CREATE TABLE statement_chunk (
    import_id    uuid        NOT NULL REFERENCES statement_import (id) ON DELETE CASCADE,
    n            integer     NOT NULL,
    first_line   integer     NOT NULL,
    last_line    integer     NOT NULL,
    date_context text        NOT NULL DEFAULT '',
    status       text        NOT NULL DEFAULT 'pending' CHECK (status IN ('pending', 'done', 'failed')),
    attempts     integer     NOT NULL DEFAULT 0,
    PRIMARY KEY (import_id, n)
);

-- Строка импорта — одна операция выписки.
CREATE TABLE statement_row (
    id                uuid        PRIMARY KEY,
    import_id         uuid        NOT NULL REFERENCES statement_import (id) ON DELETE CASCADE,
    n                 integer     NOT NULL,
    lines             integer[]   NOT NULL,
    date              date,
    -- Сумма — строка документа после нормализации («1290.00»), не число
    -- модели (D3).
    amount            text        NOT NULL,
    dir               text        NOT NULL CHECK (dir IN ('expense', 'income', 'transfer')),
    currency          text        NOT NULL DEFAULT '',
    description       text        NOT NULL DEFAULT '',
    pattern           text        NOT NULL DEFAULT '',
    comment           text,
    category_uuid     uuid,
    -- rule | llm | user — откуда категория; manual у строки, созданной
    -- пользователем из «не разобрано», хранится в origin.
    category_source   text CHECK (category_source IN ('rule', 'llm', 'user')),
    suggestion        text,
    classified        boolean     NOT NULL DEFAULT false,
    origin            text        NOT NULL DEFAULT 'llm' CHECK (origin IN ('llm', 'manual')),
    "check"           text,
    match             text CHECK (match IN ('exists', 'possible_duplicate', 'imported')),
    match_candidates  jsonb,
    decision          text CHECK (decision IN ('include', 'skip', 'duplicate')),
    edited            boolean     NOT NULL DEFAULT false,
    committed         boolean     NOT NULL DEFAULT false,
    fingerprint       text        NOT NULL,
    op_uuid           uuid        NOT NULL
);

CREATE INDEX statement_row_import_id_idx ON statement_row (import_id, n);

-- Правило продавца: pattern → категория пользователя (statement-categorization).
CREATE TABLE merchant_rule (
    user_id       uuid        NOT NULL,
    pattern       text        NOT NULL,
    category_uuid uuid        NOT NULL,
    updated_at    timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (user_id, pattern)
);

-- Отпечатки записанных строк: переживают удаление импорта, не дают
-- загрузить одну выписку дважды (statement-review).
CREATE TABLE statement_imported (
    user_id      uuid        NOT NULL,
    fingerprint  text        NOT NULL,
    committed_at timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (user_id, fingerprint)
);

-- Свой формат пользователя (statement-profiles): ключ — отпечаток заголовка
-- колонок. TTL импорта на него не действует.
CREATE TABLE format_profile (
    id                 uuid        PRIMARY KEY,
    user_id            uuid        NOT NULL,
    format_fingerprint text        NOT NULL,
    name               text        NOT NULL,
    created_at         timestamptz NOT NULL DEFAULT now(),
    UNIQUE (user_id, format_fingerprint)
);

-- Пример «строки-источники → исправленный JSON», не больше 5 на формат
-- (лишние удаляет сервис при вставке).
CREATE TABLE format_example (
    id           bigserial   PRIMARY KEY,
    profile_id   uuid        NOT NULL REFERENCES format_profile (id) ON DELETE CASCADE,
    source_lines text        NOT NULL,
    expected     jsonb       NOT NULL,
    created_at   timestamptz NOT NULL DEFAULT now()
);

CREATE INDEX format_example_profile_id_idx ON format_example (profile_id, created_at DESC);
