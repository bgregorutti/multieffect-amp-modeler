// Integration test: builds and launches the REAL audio_engine executable
// as a subprocess bound to a temp Unix domain socket path, connects a
// plain POSIX socket client (independent of ControlSocketServer's own
// code), sends newline-delimited JSON commands, and asserts on the acks /
// resulting state -- exactly as control-daemon would eventually do once
// it grows a real (non-Null) AudioEngineClient over this same socket.

#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <thread>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

using nlohmann::json;

namespace {

std::string uniqueSocketPath() {
    std::ostringstream ss;
    ss << "/tmp/audio_engine_test_" << getpid() << "_" << std::chrono::steady_clock::now().time_since_epoch().count()
       << ".sock";
    return ss.str();
}

class SubprocessEngine {
public:
    explicit SubprocessEngine(const std::string& socketPath) : socketPath_(socketPath) {
        pid_ = fork();
        if (pid_ < 0) throw std::runtime_error("fork() failed");
        if (pid_ == 0) {
            // Child: exec the real audio_engine binary.
            execl(AUDIO_ENGINE_EXECUTABLE_PATH, AUDIO_ENGINE_EXECUTABLE_PATH, socketPath.c_str(), (char*)nullptr);
            _exit(127);  // exec failed
        }
        // Parent: wait for the socket file to appear (bounded retry loop).
        for (int i = 0; i < 200; ++i) {
            if (access(socketPath_.c_str(), F_OK) == 0) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        throw std::runtime_error("audio_engine subprocess never created its control socket");
    }

    ~SubprocessEngine() {
        if (pid_ > 0) {
            kill(pid_, SIGTERM);
            int status = 0;
            for (int i = 0; i < 100; ++i) {
                if (waitpid(pid_, &status, WNOHANG) == pid_) return;
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            kill(pid_, SIGKILL);
            waitpid(pid_, &status, 0);
        }
    }

private:
    std::string socketPath_;
    pid_t pid_ = -1;
};

class SocketClient {
public:
    explicit SocketClient(const std::string& socketPath) {
        fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd_ < 0) throw std::runtime_error("socket() failed");

        struct sockaddr_un addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, socketPath.c_str(), sizeof(addr.sun_path) - 1);

        // Retry connect briefly: the server's accept loop polls at 100ms
        // granularity, so the socket file can exist slightly before
        // listen() is actually being serviced.
        bool connected = false;
        for (int i = 0; i < 100; ++i) {
            if (::connect(fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0) {
                connected = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (!connected) throw std::runtime_error("could not connect to control socket");
    }

    ~SocketClient() {
        if (fd_ >= 0) close(fd_);
    }

    json sendCommand(const json& command) {
        std::string line = command.dump() + "\n";
        std::size_t written = 0;
        while (written < line.size()) {
            ssize_t n = ::send(fd_, line.data() + written, line.size() - written, 0);
            if (n <= 0) throw std::runtime_error("send() failed");
            written += static_cast<std::size_t>(n);
        }
        return readOneLine();
    }

private:
    json readOneLine() {
        while (true) {
            std::size_t newlinePos = pending_.find('\n');
            if (newlinePos != std::string::npos) {
                std::string line = pending_.substr(0, newlinePos);
                pending_.erase(0, newlinePos + 1);
                return json::parse(line);
            }
            char buf[4096];
            ssize_t n = ::recv(fd_, buf, sizeof(buf), 0);
            if (n <= 0) throw std::runtime_error("recv() failed or connection closed before a full line arrived");
            pending_.append(buf, static_cast<std::size_t>(n));
        }
    }

    int fd_ = -1;
    std::string pending_;
};

}  // namespace

TEST(ControlSocketIntegration, LoadPresetSetBypassCrossfadeAndGetState) {
    std::string socketPath = uniqueSocketPath();
    SubprocessEngine engine(socketPath);
    SocketClient client(socketPath);

    // get_state before anything is loaded.
    {
        json reply = client.sendCommand(json{{"cmd", "get_state"}});
        EXPECT_TRUE(reply.value("ok", false));
        EXPECT_TRUE(reply["state"]["current_preset_id"].is_null());
        EXPECT_FALSE(reply["state"]["bypass"].get<bool>());
    }

    // load_preset with a realistic preset shape (no nam/ir asset ids, so
    // no filesystem access is needed for this test -- a preset with a
    // gain block only).
    {
        json preset = {{"id", "p1"},
                       {"name", "Test Preset"},
                       {"blocks", json::array({json{{"type", "gain"}, {"enabled", true}, {"params", {{"gain_db", 3.0}}}}})},
                       {"nam_asset_id", nullptr},
                       {"ir_asset_id", nullptr}};
        json reply = client.sendCommand(json{{"cmd", "load_preset"}, {"preset", preset}});
        EXPECT_TRUE(reply.value("ok", false)) << reply.dump();
        EXPECT_EQ(reply["preset_id"], "p1");
    }

    // set_bypass
    {
        json reply = client.sendCommand(json{{"cmd", "set_bypass"}, {"bypass", true}});
        EXPECT_TRUE(reply.value("ok", false));
        EXPECT_TRUE(reply["bypass"].get<bool>());
    }

    // crossfade_ms
    {
        json reply = client.sendCommand(json{{"cmd", "crossfade_ms"}, {"value", 75}});
        EXPECT_TRUE(reply.value("ok", false));
        EXPECT_EQ(reply["value"].get<int>(), 75);
    }

    // get_state reflects everything above.
    {
        json reply = client.sendCommand(json{{"cmd", "get_state"}});
        EXPECT_TRUE(reply.value("ok", false));
        auto state = reply["state"];
        EXPECT_EQ(state["current_preset_id"].get<std::string>(), "p1");
        EXPECT_TRUE(state["bypass"].get<bool>());
        EXPECT_EQ(state["crossfade_ms"].get<int>(), 75);
    }
}

TEST(ControlSocketIntegration, UnknownCommandGetsTypedError) {
    std::string socketPath = uniqueSocketPath();
    SubprocessEngine engine(socketPath);
    SocketClient client(socketPath);

    json reply = client.sendCommand(json{{"cmd", "not_a_real_command"}});
    EXPECT_FALSE(reply.value("ok", true));
    EXPECT_EQ(reply["code"], "unknown_command");
}

TEST(ControlSocketIntegration, MalformedPresetGetsValidationError) {
    std::string socketPath = uniqueSocketPath();
    SubprocessEngine engine(socketPath);
    SocketClient client(socketPath);

    json reply = client.sendCommand(json{{"cmd", "load_preset"}, {"preset", json{{"id", "missing-name"}}}});
    EXPECT_FALSE(reply.value("ok", true));
    EXPECT_EQ(reply["code"], "validation_error");
}

TEST(ControlSocketIntegration, LoadPresetWithUnregisteredAssetGetsError) {
    std::string socketPath = uniqueSocketPath();
    SubprocessEngine engine(socketPath);
    SocketClient client(socketPath);

    json preset = {{"id", "p2"}, {"name", "Needs Assets"}, {"blocks", json::array()},
                   {"nam_asset_id", "does-not-exist"}, {"ir_asset_id", nullptr}};
    json reply = client.sendCommand(json{{"cmd", "load_preset"}, {"preset", preset}});
    EXPECT_FALSE(reply.value("ok", true));
    // Unknown asset id maps to internal_error today (a plain
    // std::runtime_error from ResourceManager) -- still a typed, safe
    // reply rather than a crash either way.
    EXPECT_TRUE(reply["code"] == "internal_error" || reply["code"] == "not_found");
}

TEST(ControlSocketIntegration, MultipleSequentialCommandsOnOneConnection) {
    std::string socketPath = uniqueSocketPath();
    SubprocessEngine engine(socketPath);
    SocketClient client(socketPath);

    for (int i = 0; i < 10; ++i) {
        json reply = client.sendCommand(json{{"cmd", "crossfade_ms"}, {"value", i * 10}});
        EXPECT_TRUE(reply.value("ok", false));
        EXPECT_EQ(reply["value"].get<int>(), i * 10);
    }
}
