#!/usr/bin/env bash
# Поднимает LLM для wallet-ai в LM Studio на хосте: сервер на порту, доступный
# из docker, и модель под стабильным id (LLM_MODEL в wallet-ai/.env).
# Идемпотентен: уже запущенный сервер и уже загруженную модель не трогает.
#
#   bash wallet-ai/scripts/lms-load.sh
#   LMS_MODEL_KEY=qwen/qwen3-8b bash wallet-ai/scripts/lms-load.sh    # другая модель
#
# Переменные (все необязательные):
#   LMS_BIN          путь к lms                    (~/.lmstudio/bin/lms)
#   LMS_MODEL_KEY    модель из `lms ls`            (google/gemma-4-e4b)
#   LMS_IDENTIFIER   id в API = LLM_MODEL          (wallet-parse)
#   LMS_CONTEXT      контекст, токенов             (8192 — = LLM_CONTEXT wallet-ai;
#                    gemma-4-e4b с ним ~4,8 ГБ из 8 на GTX 1070)
#   LMS_GPU          доля на GPU: max/off/0..1     (max)
#   LMS_PORT         порт сервера                  (1234)
#   LMS_BIND         адрес сервера                 (0.0.0.0 — иначе из docker не достучаться;
#                    порт в файрволе открывать только для 172.16.0.0/12)
#   LLM_API_KEY      токен LM Studio для проверки  (если не задан — из LMS_SECRETS_FILE)
#   LMS_SECRETS_FILE файл с LLM_API_KEY            (~/.config/wallet-secrets/prod/wallet-ai.env —
#                    тот же токен, что у wallet-ai контура)
#
# Та же модель, уже загруженная под другим id (например, из GUI LM Studio),
# выгружается перед загрузкой: две копии на 8 ГБ не помещаются — «failed to
# allocate buffer for kv cache».
#
# Модель грузится без --ttl: авто-выгрузка по простою вернула бы JIT-загрузку
# (10–30 с) на первом запросе — дольше таймаута wallet-ai.
set -euo pipefail

LMS_BIN="${LMS_BIN:-$HOME/.lmstudio/bin/lms}"
LMS_MODEL_KEY="${LMS_MODEL_KEY:-google/gemma-4-e4b}"
LMS_IDENTIFIER="${LMS_IDENTIFIER:-wallet-parse}"
LMS_CONTEXT="${LMS_CONTEXT:-8192}"
LMS_GPU="${LMS_GPU:-max}"
LMS_PORT="${LMS_PORT:-1234}"
LMS_BIND="${LMS_BIND:-0.0.0.0}"
LMS_SECRETS_FILE="${LMS_SECRETS_FILE:-$HOME/.config/wallet-secrets/prod/wallet-ai.env}"

[[ -x "$LMS_BIN" ]] || { echo "lms не найден: $LMS_BIN (LMS_BIN=...)" >&2; exit 1; }
command -v jq >/dev/null || { echo "нужен jq" >&2; exit 1; }

# Токен — в подоболочке: остальные переменные файла сюда не попадают,
# значение не печатается.
if [[ -z "${LLM_API_KEY:-}" && -r "$LMS_SECRETS_FILE" ]]; then
    LLM_API_KEY="$(set -a; source "$LMS_SECRETS_FILE" >/dev/null 2>&1; printf '%s' "${LLM_API_KEY:-}")"
    [[ -n "$LLM_API_KEY" ]] && echo "токен LM Studio — из $LMS_SECRETS_FILE"
fi

# 1. Сервер. `lms` сам будит демон LM Studio; первый вызов после загрузки
#    системы может занять до минуты.
if "$LMS_BIN" server status --json 2>/dev/null | grep -q '"running":true'; then
    echo "сервер уже запущен"
else
    echo "запускаю сервер на $LMS_BIND:$LMS_PORT"
    "$LMS_BIN" server start --port "$LMS_PORT" --bind "$LMS_BIND"
fi

# 2. Модель под нужным id.
loaded="$("$LMS_BIN" ps --json 2>/dev/null || echo '[]')"
if jq -e --arg id "$LMS_IDENTIFIER" 'any(.[]; .identifier == $id)' <<<"$loaded" >/dev/null; then
    echo "модель $LMS_IDENTIFIER уже загружена"
else
    # Та же модель под чужим id занимает видеопамять — вторая копия не влезет.
    jq -r --arg key "$LMS_MODEL_KEY" --arg id "$LMS_IDENTIFIER" \
        '.[] | select(.modelKey == $key and .identifier != $id) | .identifier' <<<"$loaded" \
    | while read -r other; do
        echo "выгружаю $LMS_MODEL_KEY под id $other — освобождаю видеопамять"
        "$LMS_BIN" unload "$other"
    done
    echo "загружаю $LMS_MODEL_KEY как $LMS_IDENTIFIER (контекст $LMS_CONTEXT, GPU $LMS_GPU)"
    "$LMS_BIN" load "$LMS_MODEL_KEY" --identifier "$LMS_IDENTIFIER" \
        --context-length "$LMS_CONTEXT" --gpu "$LMS_GPU" --yes
fi

# 3. Проверка. Загрузка — через `lms ps`, токен для этого не нужен.
"$LMS_BIN" ps --json 2>/dev/null \
    | jq -e --arg id "$LMS_IDENTIFIER" 'any(.[]; .identifier == $id)' >/dev/null \
    || { echo "lms ps не показывает $LMS_IDENTIFIER — модель не загрузилась" >&2; exit 1; }

#    Затем тот же запрос, что проба wallet-ai (GET /v1/models): слушает ли
#    сервер порт и подходит ли токен контура. LLM_API_KEY — из окружения или
#    LMS_SECRETS_FILE (см. шапку). Без токена при включённой авторизации
#    LM Studio (401) — только предупреждение: модель загружена, токен не проверен.
auth=()
[[ -n "${LLM_API_KEY:-}" ]] && auth=(-H "Authorization: Bearer $LLM_API_KEY")
resp="$(curl -sS -m 10 -w '\n%{http_code}' "${auth[@]}" \
    "http://127.0.0.1:$LMS_PORT/v1/models" 2>/dev/null || true)"
code="${resp##*$'\n'}"
body="${resp%$'\n'*}"
if [[ "$code" == "200" ]] && grep -q "\"$LMS_IDENTIFIER\"" <<<"$body"; then
    echo "ok: $LMS_IDENTIFIER доступна на :$LMS_PORT"
elif [[ "$code" == "401" && -z "${LLM_API_KEY:-}" ]]; then
    echo "ok: $LMS_IDENTIFIER загружена; токен LM Studio не проверен — нет LLM_API_KEY (LMS_SECRETS_FILE=...)" >&2
elif [[ "$code" == "401" ]]; then
    echo "LM Studio отвергла токен (401) — LLM_API_KEY не тот, что в настройках сервера" >&2
    exit 1
else
    echo "GET /v1/models на :$LMS_PORT: HTTP ${code:-нет ответа}, $LMS_IDENTIFIER не найдена" >&2
    exit 1
fi
