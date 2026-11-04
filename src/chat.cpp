#include "chat.hpp"

#include <algorithm>
#include <cctype>
#include <set>

#include "chat_metrics.hpp"
#include "util/log.hpp"

using nlohmann::json;

namespace chat {

namespace {

bool matches(const std::string& s, const char* pattern) {
    // pattern: '9' — цифра, остальное — буквально ("9999-99-99").
    std::size_t n = 0;
    for (; pattern[n] != '\0'; ++n) {
        if (n >= s.size()) return false;
        if (pattern[n] == '9' ? std::isdigit(static_cast<unsigned char>(s[n])) == 0 : s[n] != pattern[n]) return false;
    }
    return n == s.size();
}

std::string str(const json& obj, const char* key) {
    if (!obj.is_object()) return {};
    auto it = obj.find(key);
    if (it == obj.end() || !it->is_string()) return {};
    return it->get<std::string>();
}

void replace_all(std::string& s, const std::string& from, const std::string& to) {
    for (std::size_t pos = s.find(from); pos != std::string::npos; pos = s.find(from, pos + to.size())) {
        s.replace(pos, from.size(), to);
    }
}

const char* kTokensHelp = "Токены вызовов модели в заданиях chat по провайдеру и направлению.";
const char* kJobsHelp = "Завершённые задания chat по провайдеру и исходу.";
const char* kToolCallsHelp = "Вызовы инструментов в заданиях chat по исходу.";

void count_tool(const std::string& tool, const char* result) {
    chat_metrics::add("ai_chat_tool_calls_total", kToolCallsHelp, {{"tool", tool}, {"result", result}});
}

// Отказ задания: failure для клиента, detail — в журнал.
parse_expense::Outcome failed(parse_expense::Outcome out, const std::string& failure, const std::string& detail) {
    out.status = jobs::Status::Failed;
    out.failure = failure;
    if (!detail.empty()) out.log["detail"] = detail;
    return out;
}

} // namespace

bool valid_conversation_id(const std::string& id) {
    if (id.empty() || id.size() > kMaxConversationIdChars) return false;
    for (char c : id) {
        if (std::isalnum(static_cast<unsigned char>(c)) == 0 && c != '_' && c != ':' && c != '-') return false;
    }
    return true;
}

std::string validate_input(const json& input) {
    if (!input.is_object()) return "input должен быть объектом";
    if (!input.contains("text") || !input["text"].is_string()) return "input.text обязателен";
    std::string text = input["text"].get<std::string>();
    if (text.find_first_not_of(" \t\r\n") == std::string::npos) return "input.text обязателен";
    if (text.size() > kMaxTextBytes) return "input.text длиннее " + std::to_string(kMaxTextBytes) + " байт";
    if (input.contains("conversation_id") && !input["conversation_id"].is_null()) {
        if (!input["conversation_id"].is_string() || !valid_conversation_id(input["conversation_id"].get<std::string>())) {
            return "input.conversation_id — 1..64 символа [A-Za-z0-9_:-]";
        }
    }
    if (input.contains("now") && !input["now"].is_null()) {
        if (!input["now"].is_object()) return "input.now — {date: YYYY-MM-DD, time: HH:MM:SS} в поясе пользователя";
        if (!matches(str(input["now"], "date"), "9999-99-99")) return "input.now.date — YYYY-MM-DD";
        if (!matches(str(input["now"], "time"), "99:99:99")) return "input.now.time — HH:MM:SS";
    }
    return {};
}

json make_job_input(const json& client_input, const std::string& conversation_id, const std::string& provider,
                    const std::string& mcp_token) {
    json in = {{"text", str(client_input, "text")},
               {"conversation_id", conversation_id},
               {"provider", provider},
               {"mcp_token", mcp_token}};
    if (client_input.contains("now") && client_input["now"].is_object()) in["now"] = client_input["now"];
    return in;
}

std::string render_system_prompt(const llm::Prompt& prompt, const std::string& date, const std::string& time,
                                 const std::string& mcp_instructions) {
    std::string s = prompt.text;
    replace_all(s, "{today}", date);
    replace_all(s, "{weekday}", llm::weekday_ru(date));
    replace_all(s, "{time}", time);
    replace_all(s, "{mcp_instructions}", mcp_instructions.empty() ? "(нет)" : mcp_instructions);
    return s;
}

std::pair<std::string, std::string> prompt_now(const json& job_input, std::time_t server_now) {
    if (job_input.contains("now") && job_input["now"].is_object()) {
        std::string d = str(job_input["now"], "date");
        std::string t = str(job_input["now"], "time");
        if (!d.empty() && !t.empty()) return {d, t};
    }
    std::tm tm{};
    gmtime_r(&server_now, &tm);
    char d[16];
    char t[16];
    std::strftime(d, sizeof d, "%Y-%m-%d", &tm);
    std::strftime(t, sizeof t, "%H:%M:%S", &tm);
    return {d, std::string(t) + " UTC"};
}

// --- Conversations ---

std::vector<Conversations::Turn> Conversations::get(const std::string& user_id, const std::string& conversation_id,
                                                    Clock::time_point now) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = entries_.find({user_id, conversation_id});
    if (it == entries_.end()) return {};
    if (now - it->second.last_used >= ttl_) {
        entries_.erase(it);
        return {};
    }
    it->second.last_used = now;
    return {it->second.turns.begin(), it->second.turns.end()};
}

void Conversations::append(const std::string& user_id, const std::string& conversation_id,
                           const std::string& question, const std::string& answer, Clock::time_point now) {
    if (max_turns_ == 0) return;
    std::lock_guard<std::mutex> lock(mu_);
    Entry& e = entries_[{user_id, conversation_id}];
    if (!e.turns.empty() && now - e.last_used >= ttl_) e.turns.clear();
    e.turns.emplace_back(question, answer);
    while (e.turns.size() > max_turns_) e.turns.pop_front();
    e.last_used = now;
}

void Conversations::sweep(Clock::time_point now) {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto it = entries_.begin(); it != entries_.end();) {
        it = now - it->second.last_used >= ttl_ ? entries_.erase(it) : std::next(it);
    }
}

std::size_t Conversations::size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return entries_.size();
}

// --- Цикл ---

parse_expense::Outcome run(const jobs::Job& job, Agent& agent) {
    using Clock = std::chrono::steady_clock;
    const Clock::time_point deadline = agent.now() + std::chrono::milliseconds(agent.cfg.timeout_ms);
    auto remaining_ms = [&]() -> int {
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - agent.now()).count();
        return left > 0 ? static_cast<int>(left) : 0;
    };

    const json& in = job.input;
    const std::string provider = str(in, "provider");
    const std::string question = str(in, "text");
    const std::string conversation_id = str(in, "conversation_id");

    parse_expense::Outcome out;
    json tools_called = json::array();
    int steps = 0;
    Usage total;
    out.log = {{"provider", provider}, {"steps", 0}, {"tools", tools_called}, {"input_tokens", 0}, {"output_tokens", 0}};
    // Обновляет счётчики в журнале и метрику исхода — на любом выходе.
    auto finish = [&](parse_expense::Outcome o) {
        o.log["steps"] = steps;
        o.log["tools"] = tools_called;
        o.log["input_tokens"] = total.input_tokens;
        o.log["output_tokens"] = total.output_tokens;
        chat_metrics::add("ai_chat_jobs_total", kJobsHelp,
                          {{"provider", provider}, {"result", o.status == jobs::Status::Done ? "done" : o.failure}});
        if (steps > 0) chat_metrics::observe_steps(steps);
        return o;
    };

    chat::Model* model = agent.registry != nullptr ? agent.registry->model(provider) : nullptr;
    if (model == nullptr || agent.mcp == nullptr || agent.credentials == nullptr) {
        return finish(failed(out, "internal", "no_model_for_provider"));
    }

    // wallet-mcp: initialize (instructions) и tools/list — на каждое задание.
    mcp::Session session;
    session.token = str(in, "mcp_token");
    mcp::InitResult init = agent.mcp->initialize(session, remaining_ms());
    if (init.status != mcp::Status::Ok) return finish(failed(out, mcp::failure_code(init.status), init.detail));
    mcp::ToolsResult listed = agent.mcp->list_tools(session, remaining_ms());
    if (listed.status != mcp::Status::Ok) return finish(failed(out, mcp::failure_code(listed.status), listed.detail));
    std::set<std::string> known;
    for (const auto& t : listed.tools) known.insert(t.name);

    auto [date, time] = prompt_now(in, agent.wall_now());
    const std::string system = render_system_prompt(agent.prompt, date, time, init.instructions);

    std::vector<Message> messages;
    if (agent.conversations != nullptr && !conversation_id.empty()) {
        for (const auto& [q, a] : agent.conversations->get(job.user_id, conversation_id, agent.now())) {
            messages.push_back({Role::User, q, {}, {}, nullptr});
            messages.push_back({Role::Assistant, a, {}, {}, nullptr});
        }
    }
    // Текст пользователя — только в последнем сообщении user.
    messages.push_back({Role::User, question, {}, {}, nullptr});

    const int max_steps = std::max(1, agent.cfg.max_steps);
    for (;;) {
        const bool allow_tools = steps < max_steps;
        if (remaining_ms() <= 0) return finish(failed(out, "timeout", "deadline"));
        auto credential = agent.credentials->resolve(job.user_id, provider);
        Turn turn = model->turn(system, messages, listed.tools, allow_tools, credential, remaining_ms());
        ++steps;
        total.input_tokens += turn.usage.input_tokens;
        total.output_tokens += turn.usage.output_tokens;
        if (turn.usage.input_tokens > 0) {
            chat_metrics::add("ai_chat_tokens_total", kTokensHelp, {{"provider", provider}, {"direction", "input"}},
                              static_cast<std::uint64_t>(turn.usage.input_tokens));
        }
        if (turn.usage.output_tokens > 0) {
            chat_metrics::add("ai_chat_tokens_total", kTokensHelp, {{"provider", provider}, {"direction", "output"}},
                              static_cast<std::uint64_t>(turn.usage.output_tokens));
        }

        switch (turn.kind) {
        case TurnKind::Error:
            switch (turn.error) {
            case ModelError::AuthFailed: {
                chat::Credential who = credential.value_or(chat::Credential{});
                if (who.owner == Owner::Instance) {
                    // Ключ стека отклонён — это видит администратор.
                    json fields = providers::credential_log_fields(provider, who);
                    fields["event"] = "provider_auth_failed";
                    fields["job_id"] = job.id;
                    fields["detail"] = turn.detail;
                    applog::error("provider rejected API key", fields);
                }
                out.log["owner"] = to_string(who.owner);
                return finish(failed(out, "provider_auth_failed", turn.detail));
            }
            case ModelError::Unavailable: return finish(failed(out, "unavailable", turn.detail));
            case ModelError::Timeout: return finish(failed(out, "timeout", turn.detail));
            case ModelError::BadResponse:
            case ModelError::None: return finish(failed(out, "internal", turn.detail));
            }
            return finish(failed(out, "internal", turn.detail));
        case TurnKind::Refused: return finish(failed(out, "provider_refused", turn.detail));
        case TurnKind::Truncated: return finish(failed(out, "internal", "truncated " + turn.detail));
        case TurnKind::ToolCalls:
            if (!allow_tools) {
                // Последний вызов всё равно просит инструменты: берём текст, если есть.
                if (turn.message.text.empty()) return finish(failed(out, "internal", "tool_calls_after_limit"));
                turn.kind = TurnKind::Text;
                break;
            }
            break;
        case TurnKind::Text: break;
        }

        if (turn.kind == TurnKind::Text) {
            out.status = jobs::Status::Done;
            out.result = {{"text", turn.message.text},
                          {"conversation_id", conversation_id},
                          {"provider", provider},
                          {"steps", steps},
                          {"tools", tools_called}};
            out.log["outcome"] = "answer";
            if (agent.conversations != nullptr && !conversation_id.empty()) {
                agent.conversations->append(job.user_id, conversation_id, question, turn.message.text, agent.now());
            }
            return finish(out);
        }

        // Вызовы инструментов: ответ ассистента — в историю как есть (append-only).
        Message results;
        results.role = Role::Tool;
        for (const auto& call : turn.message.tool_calls) {
            ToolResult r;
            r.id = call.id;
            if (known.count(call.name) == 0) {
                // Выдуманный инструмент в wallet-mcp не уходит; имя — не метка метрики.
                r.content = "Нет такого инструмента: " + call.name + ". Доступны только инструменты из списка.";
                r.is_error = true;
                count_tool("unknown", "unknown_tool");
            } else if (!call.args_ok) {
                r.content = "Аргументы вызова " + call.name + " — не JSON-объект. Повтори вызов с аргументами по схеме.";
                r.is_error = true;
                count_tool(call.name, "bad_args");
            } else {
                if (remaining_ms() <= 0) return finish(failed(out, "timeout", "deadline"));
                mcp::CallResult c =
                    agent.mcp->call_tool(session, call.name, call.args, agent.cfg.tool_result_max_chars, remaining_ms());
                tools_called.push_back(call.name);
                if (c.status == mcp::Status::Ok) {
                    r.content = c.content;
                    r.is_error = c.is_error;
                    count_tool(call.name, c.is_error ? "error" : "ok");
                } else if (c.status == mcp::Status::RpcError) {
                    // Ошибка протокола у одного вызова — модели, цикл продолжается.
                    r.content = c.message.empty() ? "Ошибка инструмента." : c.message;
                    r.is_error = true;
                    count_tool(call.name, "rpc_error");
                } else {
                    count_tool(call.name, mcp::to_string(c.status));
                    return finish(failed(out, mcp::failure_code(c.status), c.detail));
                }
            }
            results.tool_results.push_back(std::move(r));
        }
        if (steps >= max_steps) results.text = kFinalCallNote;
        messages.push_back(std::move(turn.message));
        messages.push_back(std::move(results));
    }
}

} // namespace chat
