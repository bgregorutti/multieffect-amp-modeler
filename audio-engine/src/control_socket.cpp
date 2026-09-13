#include "audio_engine/control_socket.hpp"

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstring>
#include <stdexcept>

namespace audio_engine {

using nlohmann::json;

ControlSocketServer::ControlSocketServer() = default;

ControlSocketServer::~ControlSocketServer() { stop(); }

void ControlSocketServer::start(const std::string& socketPath, CommandHandler handler) {
    if (running_.load()) throw std::runtime_error("ControlSocketServer already running");

    socketPath_ = socketPath;
    handler_ = std::move(handler);

    listenFd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listenFd_ < 0) throw std::runtime_error(std::string("socket() failed: ") + std::strerror(errno));

    ::unlink(socketPath_.c_str());  // remove any stale socket file from a previous run

    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (socketPath_.size() >= sizeof(addr.sun_path)) {
        ::close(listenFd_);
        listenFd_ = -1;
        throw std::runtime_error("socket path too long: " + socketPath_);
    }
    std::strncpy(addr.sun_path, socketPath_.c_str(), sizeof(addr.sun_path) - 1);

    if (::bind(listenFd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::string err = std::strerror(errno);
        ::close(listenFd_);
        listenFd_ = -1;
        throw std::runtime_error("bind(" + socketPath_ + ") failed: " + err);
    }

    if (::listen(listenFd_, /*backlog=*/16) < 0) {
        std::string err = std::strerror(errno);
        ::close(listenFd_);
        listenFd_ = -1;
        ::unlink(socketPath_.c_str());
        throw std::runtime_error("listen() failed: " + err);
    }

    running_.store(true);
    acceptThread_ = std::thread(&ControlSocketServer::acceptLoop, this);
}

void ControlSocketServer::acceptLoop() {
    while (running_.load()) {
        struct pollfd pfd;
        pfd.fd = listenFd_;
        pfd.events = POLLIN;
        pfd.revents = 0;

        int ret = ::poll(&pfd, 1, /*timeout_ms=*/100);
        if (ret <= 0) continue;  // timeout, or interrupted -- re-check running_
        if (!(pfd.revents & POLLIN)) continue;

        int clientFd = ::accept(listenFd_, nullptr, nullptr);
        if (clientFd < 0) continue;

        std::thread t(&ControlSocketServer::serveConnection, this, clientFd);
        {
            std::lock_guard<std::mutex> lock(clientThreadsMutex_);
            clientThreads_.push_back(std::move(t));
        }
    }
}

void ControlSocketServer::serveConnection(int clientFd) {
    std::string pending;
    char buf[4096];

    while (running_.load()) {
        struct pollfd pfd;
        pfd.fd = clientFd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int pret = ::poll(&pfd, 1, 100);
        if (pret == 0) continue;  // timeout -- re-check running_
        if (pret < 0) break;
        if (!(pfd.revents & POLLIN)) continue;

        ssize_t n = ::recv(clientFd, buf, sizeof(buf), 0);
        if (n <= 0) break;  // peer closed, or error
        pending.append(buf, static_cast<std::size_t>(n));

        std::size_t newlinePos;
        while ((newlinePos = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, newlinePos);
            pending.erase(0, newlinePos + 1);
            if (line.empty()) continue;

            json reply;
            try {
                json command = json::parse(line);
                reply = handler_(command);
            } catch (const json::exception& e) {
                reply = json{{"ok", false}, {"code", "validation_error"},
                             {"message", std::string("invalid JSON: ") + e.what()}};
            } catch (const std::exception& e) {
                reply = json{{"ok", false}, {"code", "internal_error"}, {"message", e.what()}};
            }

            std::string out = reply.dump() + "\n";
            std::size_t written = 0;
            while (written < out.size()) {
                ssize_t sent = ::send(clientFd, out.data() + written, out.size() - written, 0);
                if (sent <= 0) {
                    written = out.size();  // give up silently; client went away
                    break;
                }
                written += static_cast<std::size_t>(sent);
            }
        }
    }

    ::close(clientFd);
}

void ControlSocketServer::stop() {
    if (!running_.exchange(false)) {
        // Wasn't running; still make sure we don't leak a socket file if
        // start() partially succeeded then threw before setting running_.
        return;
    }

    if (acceptThread_.joinable()) acceptThread_.join();

    std::vector<std::thread> threads;
    {
        std::lock_guard<std::mutex> lock(clientThreadsMutex_);
        threads.swap(clientThreads_);
    }
    for (auto& t : threads) {
        if (t.joinable()) t.join();
    }

    if (listenFd_ >= 0) {
        ::close(listenFd_);
        listenFd_ = -1;
    }
    if (!socketPath_.empty()) {
        ::unlink(socketPath_.c_str());
    }
}

}  // namespace audio_engine
