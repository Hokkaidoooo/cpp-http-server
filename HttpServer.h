#pragma once
#include "Config.h"
#include "HttpResponse.h"
#include "Posix.h"
#include "ThreadPool.h"
#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <deque>
#include <iostream>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unordered_map>

class HttpServer {
public:
    using Handler = std::function<HttpResponse(const HttpRequest&)>;
    HttpServer(const Config& config, Handler handler)
        : config_(config), handler_(std::move(handler)), pool_(config.workers, config.queueCapacity) {}

    void run(const volatile std::sig_atomic_t& stopRequested) {
        listen_ = UniqueFd(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
        if (listen_.get() < 0) throw systemFailure("创建监听套接字失败");
        const int yes = 1;
        if (::setsockopt(listen_.get(), SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes) < 0) throw systemFailure("设置监听套接字失败");
        sockaddr_in address{};
        address.sin_family = AF_INET; address.sin_port = htons(config_.port);
        if (::inet_pton(AF_INET, config_.bindAddress.c_str(), &address.sin_addr) != 1) throw std::invalid_argument("BIND_ADDRESS 必须为 IPv4 地址");
        if (::bind(listen_.get(), reinterpret_cast<sockaddr*>(&address), sizeof address) < 0) throw systemFailure("绑定监听端口失败");
        if (::listen(listen_.get(), SOMAXCONN) < 0) throw systemFailure("监听端口失败");
        epoll_ = UniqueFd(::epoll_create1(EPOLL_CLOEXEC));
        wake_ = UniqueFd(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
        if (epoll_.get() < 0 || wake_.get() < 0) throw systemFailure("创建事件循环失败");
        control(EPOLL_CTL_ADD, listen_.get(), EPOLLIN | EPOLLET, listenToken);
        control(EPOLL_CTL_ADD, wake_.get(), EPOLLIN, wakeToken);
        std::cout << "LISTENING port=" << config_.port << " workers=" << config_.workers << " queue=" << config_.queueCapacity << std::endl;
        std::array<epoll_event, 128> events{};
        while (!stopRequested) {
            const int count = ::epoll_wait(epoll_.get(), events.data(), static_cast<int>(events.size()), 100);
            if (count < 0) { if (errno == EINTR) continue; throw systemFailure("epoll_wait 失败"); }
            for (int i = 0; i < count; ++i) {
                const auto token = events[i].data.u64;
                if (token == listenToken) acceptConnections();
                else if (token == wakeToken) receiveCompletions();
                else handleEvent(token, events[i].events);
            }
            expireConnections();
        }
        pool_.shutdown();
        receiveCompletions();
        while (!connections_.empty()) closeConnection(connections_.begin()->first);
        std::cout << "METRICS {\"accepted\":" << accepted_ << ",\"closed\":" << closed_
                  << ",\"responses\":" << responses_ << ",\"dispatched\":" << dispatched_
                  << ",\"queue_rejected\":" << queueRejected_ << ",\"connection_rejected\":" << connectionRejected_
                  << ",\"expired\":" << expired_ << ",\"stale_completions\":" << staleCompletions_
                  << ",\"max_active\":" << maxActive_ << ",\"active\":0}" << std::endl;
    }
private:
    using Clock = std::chrono::steady_clock;
    enum class Phase { Reading, Processing, Writing };
    struct Connection {
        UniqueFd fd;
        HttpRequest request;
        Phase phase = Phase::Reading;
        Clock::time_point progress = Clock::now();
        std::string response;
        std::size_t sent = 0;
        Connection(int fd, HttpLimits limits) : fd(fd), request(limits) {}
    };
    struct Completion { std::uint64_t token; std::string response; };
    static constexpr std::uint64_t listenToken = 1, wakeToken = 2;

    void control(int operation, int fd, std::uint32_t flags, std::uint64_t token) const {
        epoll_event event{};
        event.events = flags; event.data.u64 = token;
        if (::epoll_ctl(epoll_.get(), operation, fd, &event) < 0) throw systemFailure("epoll_ctl 失败");
    }
    void arm(std::uint64_t token, Connection& connection, std::uint32_t direction) const {
        control(EPOLL_CTL_MOD, connection.fd.get(), direction | EPOLLRDHUP | EPOLLET | EPOLLONESHOT, token);
    }
    void acceptConnections() {
        for (;;) {
            UniqueFd fd(::accept4(listen_.get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC));
            if (fd.get() < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) return;
                throw systemFailure("accept4 失败");
            }
            ++accepted_;
            if (connections_.size() >= config_.maxConnections) {
                const auto response = HttpResponse(503, "连接容量已满").toString();
                const auto count = ::send(fd.get(), response.data(), response.size(), MSG_NOSIGNAL);
                if (count != static_cast<ssize_t>(response.size())) std::cerr << "ERROR 连接过载响应未完整发送\n";
                ++connectionRejected_; ++closed_;
                continue;
            }
            if (nextToken_ == std::numeric_limits<std::uint64_t>::max()) throw std::runtime_error("连接标识已耗尽");
            const auto token = nextToken_++;
            control(EPOLL_CTL_ADD, fd.get(), EPOLLIN | EPOLLRDHUP | EPOLLET | EPOLLONESHOT, token);
            connections_.try_emplace(token, fd.release(), config_.httpLimits());
            maxActive_ = std::max(maxActive_, connections_.size());
        }
    }
    void handleEvent(std::uint64_t token, std::uint32_t flags) {
        const auto found = connections_.find(token);
        if (found == connections_.end()) return;
        if (flags & (EPOLLERR | EPOLLHUP)) { closeConnection(token); return; }
        auto& connection = found->second;
        if (connection.phase == Phase::Reading && (flags & (EPOLLIN | EPOLLRDHUP))) readRequest(token, connection);
        else if (connection.phase == Phase::Writing && (flags & EPOLLOUT)) writeResponse(token, connection);
    }
    void readRequest(std::uint64_t token, Connection& connection) {
        std::array<char, 8192> buffer{};
        for (;;) {
            const auto count = ::recv(connection.fd.get(), buffer.data(), buffer.size(), 0);
            if (count > 0) {
                connection.progress = Clock::now();
                connection.request.feed(std::string(buffer.data(), static_cast<std::size_t>(count)));
            } else if (count == 0) connection.request.endInput();
            else if (errno == EINTR) continue;
            else if (errno == EAGAIN || errno == EWOULDBLOCK) { arm(token, connection, EPOLLIN); return; }
            else { closeConnection(token); return; }
            if (connection.request.state() == HttpRequest::State::Failed) {
                prepareResponse(token, connection, HttpResponse(connection.request.errorStatus(), connection.request.errorMessage()).toString());
                return;
            }
            if (connection.request.state() == HttpRequest::State::Complete) { dispatch(token, connection); return; }
        }
    }
    void dispatch(std::uint64_t token, Connection& connection) {
        connection.phase = Phase::Processing; connection.progress = Clock::now();
        // 工作线程从不接触套接字、epoll 或连接容器；关闭和 fd 复用不影响任务身份。
        const bool accepted = pool_.tryEnqueue([this, token, request = std::move(connection.request)] {
            std::string response;
            try { response = handler_(request).toString(); }
            catch (const HttpError& error) { response = HttpResponse(error.status, error.what()).toString(); }
            catch (const std::exception& error) {
                std::lock_guard<std::mutex> lock(logMutex_);
                std::cerr << "ERROR request=" << token << " " << error.what() << '\n';
                response = HttpResponse(500, "请求处理失败，请检查服务器日志").toString();
            }
            {
                std::lock_guard<std::mutex> lock(completionMutex_);
                completions_.push_back({token, std::move(response)});
            }
            const std::uint64_t increment = 1;
            ssize_t count;
            do { count = ::write(wake_.get(), &increment, sizeof increment); } while (count < 0 && errno == EINTR);
            if (count < 0 && errno != EAGAIN) {
                std::cerr << "FATAL 工作线程通知失败 errno=" << errno << '\n'; std::terminate();
            }
        });
        if (accepted) { ++dispatched_; return; }
        ++queueRejected_;
        prepareResponse(token, connection, HttpResponse(503, "任务队列已满，请稍后重试").toString());
    }
    void receiveCompletions() {
        std::uint64_t value;
        for (;;) {
            const auto count = ::read(wake_.get(), &value, sizeof value);
            if (count == static_cast<ssize_t>(sizeof value)) continue;
            if (count < 0 && errno == EINTR) continue;
            if (count < 0 && errno == EAGAIN) break;
            throw systemFailure("读取工作线程通知失败");
        }
        std::deque<Completion> completed;
        {
            std::lock_guard<std::mutex> lock(completionMutex_); completed.swap(completions_);
        }
        for (auto& result : completed) {
            const auto found = connections_.find(result.token);
            if (found == connections_.end() || found->second.phase != Phase::Processing) { ++staleCompletions_; continue; }
            prepareResponse(result.token, found->second, std::move(result.response));
        }
    }
    void prepareResponse(std::uint64_t token, Connection& connection, std::string response) {
        connection.phase = Phase::Writing; connection.progress = Clock::now();
        connection.response = std::move(response); connection.sent = 0;
        arm(token, connection, EPOLLOUT);
    }
    void writeResponse(std::uint64_t token, Connection& connection) {
        while (connection.sent < connection.response.size()) {
            const auto count = ::send(connection.fd.get(), connection.response.data() + connection.sent,
                                      connection.response.size() - connection.sent, MSG_NOSIGNAL);
            if (count > 0) { connection.sent += static_cast<std::size_t>(count); connection.progress = Clock::now(); }
            else if (count < 0 && errno == EINTR) continue;
            else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { arm(token, connection, EPOLLOUT); return; }
            else { closeConnection(token); return; }
        }
        ++responses_; closeConnection(token);
    }
    void expireConnections() {
        const auto now = Clock::now();
        std::vector<std::uint64_t> expired;
        for (const auto& item : connections_) {
            const auto& connection = item.second;
            const auto limit = connection.phase == Phase::Reading ? config_.readTimeoutMs :
                               connection.phase == Phase::Processing ? config_.processingTimeoutMs : config_.writeTimeoutMs;
            if (now - connection.progress >= std::chrono::milliseconds(limit)) expired.push_back(item.first);
        }
        for (const auto token : expired) {
            auto& connection = connections_.at(token); ++expired_;
            if (connection.phase == Phase::Writing) closeConnection(token);
            else {
                const int status = connection.phase == Phase::Reading ? 408 : 503;
                prepareResponse(token, connection, HttpResponse(status, "请求处理超时").toString());
            }
        }
    }
    void closeConnection(std::uint64_t token) {
        const auto found = connections_.find(token);
        if (found == connections_.end()) return;
        control(EPOLL_CTL_DEL, found->second.fd.get(), 0, token);
        connections_.erase(found); ++closed_;
    }

    const Config& config_;
    const Handler handler_;
    UniqueFd listen_, epoll_, wake_;
    std::unordered_map<std::uint64_t, Connection> connections_;
    std::uint64_t nextToken_ = 3, accepted_ = 0, closed_ = 0, responses_ = 0, dispatched_ = 0;
    std::uint64_t queueRejected_ = 0, connectionRejected_ = 0, expired_ = 0, staleCompletions_ = 0;
    std::size_t maxActive_ = 0;
    std::mutex completionMutex_, logMutex_;
    std::deque<Completion> completions_;
    ThreadPool pool_;
};
