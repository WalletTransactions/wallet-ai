#include "jobs.hpp"

#include "util/uuid.hpp"

namespace jobs {

const char* to_string(Status s) {
    switch (s) {
    case Status::Queued: return "queued";
    case Status::Running: return "running";
    case Status::Done: return "done";
    case Status::Failed: return "failed";
    case Status::Cancelled: return "cancelled";
    }
    return "unknown";
}

bool is_final(Status s) { return s == Status::Done || s == Status::Failed || s == Status::Cancelled; }

const char* to_string(Lane l) { return l == Lane::Cloud ? "cloud" : "local"; }

const char* to_string(CreateError e) {
    switch (e) {
    case CreateError::None: return "none";
    case CreateError::Busy: return "busy";
    case CreateError::RateLimited: return "rate_limited";
    case CreateError::TooManyActive: return "too_many_active";
    }
    return "unknown";
}

Store::Store(Limits limits, NowFn now) : limits_(limits), now_(std::move(now)) {}

bool Store::user_over_rate(const std::string& user_id, Clock::time_point now) {
    auto& times = created_by_user_[user_id];
    while (!times.empty() && now - times.front() >= std::chrono::seconds(60)) times.pop_front();
    return static_cast<int>(times.size()) >= limits_.max_per_minute;
}

std::size_t Store::active_of(const std::string& user_id) const {
    std::size_t n = 0;
    for (const auto& [id, job] : jobs_) {
        if (job.user_id == user_id && !job.internal && !is_final(job.status)) ++n;
    }
    return n;
}

Created Store::create(const std::string& user_id, const std::string& kind, Priority priority, nlohmann::json input,
                      const std::string& idempotency_key, Lane lane) {
    if (lane == Lane::Cloud) priority = Priority::Interactive;
    std::lock_guard<std::mutex> lock(mu_);
    Created out;
    auto now = now_();

    if (!idempotency_key.empty()) {
        auto it = by_key_.find({user_id, idempotency_key});
        if (it != by_key_.end()) {
            auto job = jobs_.find(it->second);
            if (job != jobs_.end()) {
                out.job = job->second;
                out.reused = true;
                return out;
            }
            by_key_.erase(it); // задание уже истекло — ключ свободен
        }
    }

    const auto& lane_queue = lane == Lane::Cloud ? cloud_ : interactive_;
    if (priority == Priority::Interactive && lane_queue.size() >= limits_.max_queued_interactive) {
        out.error = CreateError::Busy;
        return out;
    }
    if (active_of(user_id) >= limits_.max_active_per_user) {
        out.error = CreateError::TooManyActive;
        return out;
    }
    if (user_over_rate(user_id, now)) {
        out.error = CreateError::RateLimited;
        return out;
    }

    Job job;
    job.id = uuidgen::v4();
    job.user_id = user_id;
    job.kind = kind;
    job.priority = priority;
    job.lane = lane;
    job.idempotency_key = idempotency_key;
    job.input = std::move(input);
    job.created = now;
    created_by_user_[user_id].push_back(now);
    if (!idempotency_key.empty()) by_key_[{user_id, idempotency_key}] = job.id;
    if (lane == Lane::Cloud) {
        cloud_.push_back(job.id);
    } else {
        (priority == Priority::Interactive ? interactive_ : background_).push_back(job.id);
    }
    out.job = job;
    jobs_.emplace(job.id, std::move(job));
    (lane == Lane::Cloud ? cloud_work_cv_ : work_cv_).notify_one();
    return out;
}

std::string Store::create_internal(const std::string& user_id, const std::string& kind, nlohmann::json input) {
    std::lock_guard<std::mutex> lock(mu_);
    Job job;
    job.id = uuidgen::v4();
    job.user_id = user_id;
    job.kind = kind;
    job.priority = Priority::Background;
    job.internal = true;
    job.input = std::move(input);
    job.created = now_();
    background_.push_back(job.id);
    std::string id = job.id;
    jobs_.emplace(id, std::move(job));
    work_cv_.notify_one();
    return id;
}

std::optional<Job> Store::get(const std::string& id, const std::string& user_id, std::chrono::milliseconds wait) {
    std::unique_lock<std::mutex> lock(mu_);
    auto deadline = Clock::now() + wait;
    for (;;) {
        auto it = jobs_.find(id);
        if (it == jobs_.end() || it->second.user_id != user_id) return std::nullopt;
        if (is_final(it->second.status) || wait.count() <= 0) {
            Job snapshot = it->second;
            snapshot.input = nullptr; // клиенту вход не нужен
            return snapshot;
        }
        if (stopped_ || change_cv_.wait_until(lock, deadline) == std::cv_status::timeout) {
            wait = std::chrono::milliseconds(0); // ещё раз взять снимок и вернуть как есть
        }
    }
}

bool Store::cancel(const std::string& id, const std::string& user_id) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = jobs_.find(id);
    if (it == jobs_.end() || it->second.user_id != user_id || is_final(it->second.status)) return false;
    Job& job = it->second;
    if (job.status == Status::Queued) {
        auto& q = job.lane == Lane::Cloud                    ? cloud_
                  : job.priority == Priority::Interactive ? interactive_
                                                          : background_;
        for (auto qi = q.begin(); qi != q.end(); ++qi) {
            if (*qi == id) {
                q.erase(qi);
                break;
            }
        }
    } else {
        // Running: генерацию не прервать, но клиент видит cancelled сразу, а
        // complete() выбросит результат.
        job.cancel_requested = true;
    }
    job.status = Status::Cancelled;
    job.finished = now_();
    job.input = nullptr;
    change_cv_.notify_all();
    return true;
}

std::optional<Job> Store::take(Lane lane) {
    std::unique_lock<std::mutex> lock(mu_);
    if (lane == Lane::Cloud) {
        cloud_work_cv_.wait(lock, [this] { return stopped_ || !cloud_.empty(); });
    } else {
        work_cv_.wait(lock, [this] { return stopped_ || !interactive_.empty() || !background_.empty(); });
    }
    if (stopped_) return std::nullopt;
    auto& q = lane == Lane::Cloud ? cloud_ : (!interactive_.empty() ? interactive_ : background_);
    std::string id = q.front();
    q.pop_front();
    Job& job = jobs_.at(id);
    job.status = Status::Running;
    job.started = now_();
    return job;
}

void Store::complete(const std::string& id, Status status, nlohmann::json result, const std::string& failure) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = jobs_.find(id);
    if (it == jobs_.end()) return;
    Job& job = it->second;
    if (job.cancel_requested) return; // уже cancelled — результат выбрасываем
    job.finished = now_();
    job.input = nullptr;
    job.status = status;
    job.result = std::move(result);
    job.failure = failure;
    change_cv_.notify_all();
}

void Store::sweep() {
    std::lock_guard<std::mutex> lock(mu_);
    auto now = now_();
    for (auto it = jobs_.begin(); it != jobs_.end();) {
        const Job& job = it->second;
        if (is_final(job.status) && now - job.finished >= limits_.ttl) {
            if (!job.idempotency_key.empty()) by_key_.erase({job.user_id, job.idempotency_key});
            it = jobs_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = created_by_user_.begin(); it != created_by_user_.end();) {
        auto& times = it->second;
        while (!times.empty() && now - times.front() >= std::chrono::seconds(60)) times.pop_front();
        it = times.empty() ? created_by_user_.erase(it) : std::next(it);
    }
}

void Store::stop() {
    std::lock_guard<std::mutex> lock(mu_);
    stopped_ = true;
    work_cv_.notify_all();
    cloud_work_cv_.notify_all();
    change_cv_.notify_all();
}

std::size_t Store::queued(Priority p, Lane lane) const {
    std::lock_guard<std::mutex> lock(mu_);
    if (lane == Lane::Cloud) return p == Priority::Interactive ? cloud_.size() : 0;
    return p == Priority::Interactive ? interactive_.size() : background_.size();
}

} // namespace jobs
