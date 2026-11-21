#include "net/pool.hpp"

namespace nettls {

namespace {

// Ошибки, после которых переиспользованное соединение считается протухшим.
bool stale_connection(httplib::Error e) {
    return e == httplib::Error::Read || e == httplib::Error::Write || e == httplib::Error::Connection;
}

} // namespace

ClientPool::ClientPool(std::string base_url, Config cfg, int connect_timeout_sec, int read_timeout_sec,
                       std::size_t max_idle)
    : base_url_(std::move(base_url)), cfg_(std::move(cfg)), connect_timeout_sec_(connect_timeout_sec),
      read_timeout_sec_(read_timeout_sec), max_idle_(max_idle) {}

std::unique_ptr<httplib::Client> ClientPool::make_fresh() {
    std::string err;
    auto client = make_client(base_url_, cfg_, &err);
    if (!client) {
        std::lock_guard<std::mutex> lock(mu_);
        error_ = err;
        return nullptr;
    }
    client->set_keep_alive(true);
    client->set_connection_timeout(connect_timeout_sec_, 0);
    client->set_read_timeout(read_timeout_sec_, 0);
    client->set_write_timeout(read_timeout_sec_, 0);
    return client;
}

std::unique_ptr<httplib::Client> ClientPool::acquire(bool& fresh) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!idle_.empty()) {
            auto client = std::move(idle_.back());
            idle_.pop_back();
            ++stats_.reused;
            fresh = false;
            return client;
        }
    }
    fresh = true;
    auto client = make_fresh();
    if (client) {
        std::lock_guard<std::mutex> lock(mu_);
        ++stats_.created;
    }
    return client;
}

void ClientPool::release(std::unique_ptr<httplib::Client> client) {
    std::lock_guard<std::mutex> lock(mu_);
    if (idle_.size() < max_idle_) idle_.push_back(std::move(client));
}

httplib::Result ClientPool::call(const std::function<httplib::Result(httplib::Client&)>& fn) {
    bool fresh = false;
    auto client = acquire(fresh);
    if (!client) return httplib::Result{nullptr, httplib::Error::Connection};
    httplib::Result res = fn(*client);
    if (!res && !fresh && stale_connection(res.error())) {
        // Сервер закрыл простаивающее соединение — один повтор на свежем клиенте.
        {
            std::lock_guard<std::mutex> lock(mu_);
            ++stats_.retried;
        }
        client = make_fresh();
        if (!client) return httplib::Result{nullptr, httplib::Error::Connection};
        {
            std::lock_guard<std::mutex> lock(mu_);
            ++stats_.created;
        }
        res = fn(*client);
    }
    // Битое соединение в пул не возвращаем.
    if (res) release(std::move(client));
    return res;
}

ClientPool::Stats ClientPool::stats() const {
    std::lock_guard<std::mutex> lock(mu_);
    return stats_;
}

std::string ClientPool::error() const {
    std::lock_guard<std::mutex> lock(mu_);
    return error_;
}

} // namespace nettls
