// Лимит одновременных потоков событий (change chat-sse-events, D5): каждый
// открытый /events занимает поток пула httplib, поэтому число потоков
// ограничено — всего и на пользователя.
#pragma once

#include <map>
#include <mutex>
#include <string>

namespace aiapi {

class StreamGate {
public:
    StreamGate(int max_total, int max_per_user) : max_total_(max_total), max_per_user_(max_per_user) {}

    bool try_acquire(const std::string& user_id) {
        std::lock_guard<std::mutex> lock(mu_);
        if (total_ >= max_total_ || per_user_[user_id] >= max_per_user_) return false;
        ++total_;
        ++per_user_[user_id];
        return true;
    }

    void release(const std::string& user_id) {
        std::lock_guard<std::mutex> lock(mu_);
        if (total_ > 0) --total_;
        auto it = per_user_.find(user_id);
        if (it != per_user_.end() && --it->second <= 0) per_user_.erase(it);
    }

    int open() const {
        std::lock_guard<std::mutex> lock(mu_);
        return total_;
    }

private:
    mutable std::mutex mu_;
    int max_total_;
    int max_per_user_;
    int total_ = 0;
    std::map<std::string, int> per_user_;
};

} // namespace aiapi
