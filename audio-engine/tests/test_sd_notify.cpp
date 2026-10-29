#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "audio_engine/sd_notify.hpp"

using audio_engine::sdNotify;

TEST(SdNotify, NoopWithoutNotifySocket) {
    ::unsetenv("NOTIFY_SOCKET");
    EXPECT_FALSE(sdNotify("READY=1"));
}

TEST(SdNotify, SendsStateToNotifySocket) {
    // Short path: AF_UNIX paths are capped at ~104 bytes on macOS.
    char dir[] = "/tmp/sdnXXXXXX";
    ASSERT_NE(::mkdtemp(dir), nullptr);
    const std::string path = std::string(dir) + "/n.sock";

    const int server = ::socket(AF_UNIX, SOCK_DGRAM, 0);
    ASSERT_GE(server, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    ASSERT_EQ(::bind(server, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);

    ::setenv("NOTIFY_SOCKET", path.c_str(), 1);
    EXPECT_TRUE(sdNotify("READY=1"));
    char buf[32] = {};
    const ssize_t n = ::recv(server, buf, sizeof(buf) - 1, 0);
    EXPECT_EQ(std::string(buf, n > 0 ? static_cast<std::size_t>(n) : 0), "READY=1");

    ::unsetenv("NOTIFY_SOCKET");
    ::close(server);
    ::unlink(path.c_str());
    ::rmdir(dir);
}

TEST(SdNotify, UnreachableSocketFailsOpen) {
    ::setenv("NOTIFY_SOCKET", "/nonexistent/notify.sock", 1);
    EXPECT_FALSE(sdNotify("READY=1"));
    ::unsetenv("NOTIFY_SOCKET");
}
