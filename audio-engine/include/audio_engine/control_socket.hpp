// Unix domain socket server: the local IPC channel the control daemon
// drives this engine over. control-daemon's
// UnixSocketAudioEngineClient (see
// control-daemon/src/control_daemon/audio_engine_client.py) is the real
// client of this server; NullAudioEngineClient remains the daemon's
// default when no engine socket is configured (e.g. its own test suite).
//
// Wire protocol: newline-delimited JSON, one command per line in, one
// reply per line out. See engine_state.hpp for the exact command/reply
// shapes (load_preset, set_bypass, crossfade_ms, register_asset,
// get_state) and audio-engine/README.md for a worked example transcript.
//
// Transport choice: a Unix domain socket (SOCK_STREAM), not TCP/loopback
// or the control daemon's own WebSocket protocol. Both processes always
// run on the same machine (the Pi), so a filesystem-path-addressed local
// socket avoids any port-allocation/binding concerns, gets free
// OS-enforced access control (filesystem permissions on the socket path),
// and needs no HTTP/WS framing layer -- newline-delimited JSON is the
// simplest thing that can carry the same small command vocabulary
// end to end. This deliberately does not reuse control-daemon's WS
// protocol/message shapes: those are daemon<->UI-client messages
// (hello/role, state_snapshot/state_changed broadcasts) for a very
// different fan-out; the engine has exactly one controller (the daemon)
// and no broadcast concept.
#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

namespace audio_engine {

using CommandHandler = std::function<nlohmann::json(const nlohmann::json&)>;

class ControlSocketServer {
public:
    ControlSocketServer();
    ~ControlSocketServer();

    ControlSocketServer(const ControlSocketServer&) = delete;
    ControlSocketServer& operator=(const ControlSocketServer&) = delete;

    // Binds `socketPath` (removing any stale file left over at that path
    // first) and starts accepting connections on a background thread.
    // Every newline-delimited JSON command line received on any
    // connection is passed to `handler`, whose returned JSON value is
    // serialized (compact, no embedded newlines expected) and written
    // back followed by '\n'. Throws std::runtime_error on a socket/bind
    // failure.
    void start(const std::string& socketPath, CommandHandler handler);

    // Stops accepting new connections, closes existing ones, joins the
    // background thread(s), and unlinks the socket file. Safe to call
    // more than once, and called automatically by the destructor.
    void stop();

    bool isRunning() const { return running_.load(); }

private:
    void acceptLoop();
    void serveConnection(int clientFd);

    std::string socketPath_;
    int listenFd_ = -1;
    std::atomic<bool> running_{false};
    CommandHandler handler_;
    std::thread acceptThread_;

    std::mutex clientThreadsMutex_;
    std::vector<std::thread> clientThreads_;
};

}  // namespace audio_engine
