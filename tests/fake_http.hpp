// Локальный HTTP-сервер для тестов клиентов (auth_exchange, mcp): httplib на
// 127.0.0.1 со случайным портом, в своём потоке. Хендлер задаёт тест; каждый
// принятый запрос копируется в requests() — проверить заголовки и тело.
#pragma once

#include <functional>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "httplib.h"

namespace testing {

class FakeHttpServer {
public:
    using Handler = std::function<void(const httplib::Request&, httplib::Response&)>;

    FakeHttpServer(const std::string& path, Handler handler) : handler_(std::move(handler)) {
        svr_.Post(path, [this](const httplib::Request& req, httplib::Response& res) {
            {
                std::lock_guard<std::mutex> lock(mu_);
                requests_.push_back(req);
            }
            handler_(req, res);
        });
        port_ = svr_.bind_to_any_port("127.0.0.1");
        thread_ = std::thread([this] { svr_.listen_after_bind(); });
        svr_.wait_until_ready();
    }

    ~FakeHttpServer() {
        svr_.stop();
        if (thread_.joinable()) thread_.join();
    }

    FakeHttpServer(const FakeHttpServer&) = delete;
    FakeHttpServer& operator=(const FakeHttpServer&) = delete;

    std::string origin() const { return "http://127.0.0.1:" + std::to_string(port_); }

    std::vector<httplib::Request> requests() {
        std::lock_guard<std::mutex> lock(mu_);
        return requests_;
    }

private:
    httplib::Server svr_;
    Handler handler_;
    int port_ = 0;
    std::thread thread_;
    std::mutex mu_;
    std::vector<httplib::Request> requests_;
};

// Порт, на котором точно никто не слушает: заняли и сразу освободили.
inline int closed_port() {
    httplib::Server s;
    int port = s.bind_to_any_port("127.0.0.1");
    s.stop(); // закрывает сокет и без listen_after_bind
    return port;
}

// Перехват stdout/stderr (туда пишет applog) на время жизни объекта.
class CaptureOutput {
public:
    CaptureOutput() : old_out_(std::cout.rdbuf(out_.rdbuf())), old_err_(std::cerr.rdbuf(err_.rdbuf())) {}
    ~CaptureOutput() {
        std::cout.rdbuf(old_out_);
        std::cerr.rdbuf(old_err_);
    }
    std::string text() const { return out_.str() + err_.str(); }

private:
    std::ostringstream out_;
    std::ostringstream err_;
    std::streambuf* old_out_;
    std::streambuf* old_err_;
};

} // namespace testing
