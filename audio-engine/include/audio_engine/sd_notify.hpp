#pragma once

// Minimal systemd sd_notify() -- one datagram to $NOTIFY_SOCKET, so the
// build gains no libsystemd dependency.
//
// audio-engine.service is Type=notify (spec section 5.3): systemd holds the
// unit in "activating" until READY=1 arrives, which is how the onboard
// display's boot progress knows the engine is actually up (control socket
// listening, audio stream open) rather than merely forked.
//
// Returns false, doing nothing, when NOTIFY_SOCKET is unset -- i.e. any
// run not started by systemd (dev machine, the test suite).

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace audio_engine {

inline bool sdNotify(const char* state) {
    const char* path = std::getenv("NOTIFY_SOCKET");
    if (path == nullptr || path[0] == '\0') {
        return false;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    const std::size_t len = std::strlen(path);
    if (len >= sizeof(addr.sun_path)) {
        return false;
    }
    std::memcpy(addr.sun_path, path, len);
    if (addr.sun_path[0] == '@') {
        addr.sun_path[0] = '\0';  // abstract namespace socket
    }
    const int fd = ::socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) {
        return false;
    }
    const auto addrLen = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + len);
    const ssize_t sent = ::sendto(fd, state, std::strlen(state), 0,
                                  reinterpret_cast<const sockaddr*>(&addr), addrLen);
    ::close(fd);
    return sent >= 0;
}

}  // namespace audio_engine
