#include "chat_stream.hpp"

using nlohmann::json;

namespace chat {

namespace {

constexpr const char* kWs = " \t\r\n";
const std::string kThinkOpen = "<think>";
const std::string kThinkClose = "</think>";
constexpr int kMaxBlocks = 64; // защита от мусорного index

bool is_str(const json& j, const char* key) { return j.is_object() && j.contains(key) && j[key].is_string(); }

} // namespace

// --- SSE ---

void SseParser::feed(const char* bytes, std::size_t len, const OnData& on_data) {
    auto handle_line = [&] {
        if (line_.empty()) { // конец события
            if (has_data_) on_data(data_);
            data_.clear();
            has_data_ = false;
            return;
        }
        if (line_[0] == ':') return; // комментарий (`: ping`)
        auto colon = line_.find(':');
        std::string field = line_.substr(0, colon);
        if (field != "data") return;
        std::string value = colon == std::string::npos ? "" : line_.substr(colon + 1);
        if (!value.empty() && value[0] == ' ') value.erase(0, 1);
        if (has_data_) data_ += '\n';
        data_ += value;
        has_data_ = true;
    };
    for (std::size_t i = 0; i < len; ++i) {
        char c = bytes[i];
        if (c == '\n' && prev_cr_) { // вторая половина \r\n
            prev_cr_ = false;
            continue;
        }
        prev_cr_ = c == '\r';
        if (c == '\n' || c == '\r') {
            handle_line();
            line_.clear();
        } else {
            line_ += c;
        }
    }
}

// --- <think> ---

std::string ThinkFilter::push(const std::string& chunk) {
    std::string out;
    std::string in = chunk;
    for (;;) {
        switch (state_) {
        case State::Start: {
            buf_ += in;
            in.clear();
            auto first = buf_.find_first_not_of(kWs);
            if (first == std::string::npos) {
                buf_.clear();
                return out;
            }
            buf_.erase(0, first);
            if (buf_.size() < kThinkOpen.size() && kThinkOpen.compare(0, buf_.size(), buf_) == 0) return out; // может быть тегом
            if (buf_.compare(0, kThinkOpen.size(), kThinkOpen) == 0) {
                state_ = State::Thinking;
                buf_.erase(0, kThinkOpen.size());
            } else {
                state_ = State::Visible;
            }
            in = std::move(buf_);
            buf_.clear();
            continue;
        }
        case State::Thinking: {
            buf_ += in;
            in.clear();
            auto end = buf_.find(kThinkClose);
            if (end == std::string::npos) {
                // Рассуждения не выдаём; держим хвост на случай разорванного тега.
                if (buf_.size() > kThinkClose.size()) buf_.erase(0, buf_.size() - kThinkClose.size());
                return out;
            }
            in = buf_.substr(end + kThinkClose.size());
            buf_.clear();
            state_ = State::Start; // после </think> ведущие пробелы срезаются, как в strip_think
            continue;
        }
        case State::Visible: {
            std::string s = pending_ + in;
            pending_.clear();
            auto last = s.find_last_not_of(kWs);
            if (last == std::string::npos) {
                pending_ = std::move(s); // одни пробелы — выйдут, когда будет текст
                return out;
            }
            out += s.substr(0, last + 1);
            pending_ = s.substr(last + 1);
            return out;
        }
        }
    }
}

// --- OpenAI ---

void OpenAiStreamAssembler::on_data(const std::string& data) {
    if (data == "[DONE]") {
        done_ = true;
        return;
    }
    json j = json::parse(data, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object() || j.contains("error")) {
        bad_ = true;
        return;
    }
    if (j.contains("usage") && j["usage"].is_object()) usage_ = j["usage"];
    if (!j.contains("choices") || !j["choices"].is_array() || j["choices"].empty()) return;
    const json& choice = j["choices"][0];
    if (choice.contains("delta") && choice["delta"].is_object()) {
        const json& d = choice["delta"];
        if (is_str(d, "content")) {
            const std::string& text = d["content"].get_ref<const std::string&>();
            content_ += text;
            std::string visible = filter_.push(text);
            if (!visible.empty() && on_delta_) on_delta_(visible);
        }
        if (d.contains("tool_calls") && d["tool_calls"].is_array()) {
            for (const auto& tc : d["tool_calls"]) {
                if (!tc.is_object()) continue;
                int idx = tc.contains("index") && tc["index"].is_number_integer() ? tc["index"].get<int>()
                                                                                    : static_cast<int>(calls_.size());
                if (idx < 0 || idx >= kMaxBlocks) continue;
                Call& call = calls_[idx];
                if (is_str(tc, "id") && !tc["id"].get<std::string>().empty()) call.id = tc["id"].get<std::string>();
                if (tc.contains("function") && tc["function"].is_object()) {
                    const json& fn = tc["function"];
                    if (is_str(fn, "name")) call.name += fn["name"].get<std::string>();
                    if (is_str(fn, "arguments")) call.args += fn["arguments"].get<std::string>();
                }
            }
        }
    }
    if (is_str(choice, "finish_reason") && !choice["finish_reason"].get<std::string>().empty()) {
        finish_reason_ = choice["finish_reason"].get<std::string>();
        finished_ = true;
    }
}

std::string OpenAiStreamAssembler::response_body() const {
    json msg = {{"role", "assistant"}, {"content", content_}};
    if (!calls_.empty()) {
        json calls = json::array();
        for (const auto& [idx, c] : calls_) {
            json call = {{"type", "function"}, {"function", {{"name", c.name}, {"arguments", c.args}}}};
            if (!c.id.empty()) call["id"] = c.id;
            calls.push_back(std::move(call));
        }
        msg["tool_calls"] = std::move(calls);
    }
    json out = {{"choices", json::array({{{"finish_reason", finish_reason_}, {"message", msg}}})}};
    if (usage_.is_object()) out["usage"] = usage_;
    return out.dump();
}

// --- Anthropic ---

void AnthropicStreamAssembler::on_data(const std::string& data) {
    json j = json::parse(data, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object() || !is_str(j, "type")) {
        bad_ = true;
        return;
    }
    const std::string type = j["type"].get<std::string>();
    auto index = [&]() -> int {
        int i = j.contains("index") && j["index"].is_number_integer() ? j["index"].get<int>() : -1;
        return i >= 0 && i < kMaxBlocks ? i : -1;
    };
    auto block = [&](int i) -> json& {
        while (static_cast<int>(blocks_.size()) <= i) blocks_.push_back(json::object());
        return blocks_[i];
    };
    if (type == "error") {
        bad_ = true;
    } else if (type == "message_start") {
        if (j.contains("message") && j["message"].is_object() && j["message"].contains("usage") &&
            j["message"]["usage"].is_object()) {
            for (auto it = j["message"]["usage"].begin(); it != j["message"]["usage"].end(); ++it) usage_[it.key()] = it.value();
        }
    } else if (type == "content_block_start") {
        int i = index();
        if (i < 0 || !j.contains("content_block") || !j["content_block"].is_object()) return;
        block(i) = j["content_block"];
        if (is_str(blocks_[i], "type") && blocks_[i]["type"] == "tool_use") partial_json_[i] = "";
    } else if (type == "content_block_delta") {
        int i = index();
        if (i < 0 || !j.contains("delta") || !j["delta"].is_object() || !is_str(j["delta"], "type")) return;
        const json& d = j["delta"];
        const std::string dt = d["type"].get<std::string>();
        json& b = block(i);
        auto append = [&](const char* field, const char* src) {
            if (!is_str(d, src)) return std::string();
            std::string add = d[src].get<std::string>();
            b[field] = (is_str(b, field) ? b[field].get<std::string>() : std::string()) + add;
            return add;
        };
        if (dt == "text_delta") {
            std::string add = append("text", "text");
            if (!add.empty() && on_delta_) on_delta_(add);
        } else if (dt == "input_json_delta") {
            if (is_str(d, "partial_json")) partial_json_[i] += d["partial_json"].get<std::string>();
        } else if (dt == "thinking_delta") {
            append("thinking", "thinking");
        } else if (dt == "signature_delta") {
            append("signature", "signature");
        }
    } else if (type == "content_block_stop") {
        int i = index();
        auto it = partial_json_.find(i);
        if (i < 0 || it == partial_json_.end()) return;
        json parsed = it->second.empty() ? json::object() : json::parse(it->second, nullptr, /*allow_exceptions=*/false);
        // Не объект — оставляем строкой: parse_anthropic_response пометит args_ok = false.
        block(i)["input"] = parsed.is_object() ? parsed : json(it->second);
    } else if (type == "message_delta") {
        if (j.contains("delta") && j["delta"].is_object()) {
            if (is_str(j["delta"], "stop_reason")) stop_reason_ = j["delta"]["stop_reason"].get<std::string>();
            if (j["delta"].contains("stop_details")) stop_details_ = j["delta"]["stop_details"];
        }
        if (j.contains("usage") && j["usage"].is_object()) {
            for (auto it = j["usage"].begin(); it != j["usage"].end(); ++it) usage_[it.key()] = it.value();
        }
    } else if (type == "message_stop") {
        stopped_ = true;
    }
}

std::string AnthropicStreamAssembler::response_body() const {
    json out = {{"content", blocks_}, {"stop_reason", stop_reason_}, {"usage", usage_}};
    if (!stop_details_.is_null()) out["stop_details"] = stop_details_;
    return out.dump();
}

} // namespace chat
