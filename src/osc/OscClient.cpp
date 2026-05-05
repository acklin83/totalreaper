// OscClient.cpp — UDP send for OSC, cross-platform (POSIX + Winsock).

#include "OscClient.h"
#include "../reaper/Console.h"

#include <cstring>

#if defined(_WIN32)
  #include <winsock2.h>
  #include <ws2tcpip.h>
  using socklen_t = int;
  #define TR_CLOSE_SOCKET ::closesocket
  #define TR_INVALID_SOCKET INVALID_SOCKET
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <sys/socket.h>
  #include <unistd.h>
  #define TR_CLOSE_SOCKET ::close
  #define TR_INVALID_SOCKET (-1)
#endif

namespace totalreaper::osc {

namespace {

// On Windows, WSAStartup must be called before any socket use. Counted
// reference so multiple Client instances share the init.
struct WinsockInit {
    WinsockInit() {
#if defined(_WIN32)
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    }
    ~WinsockInit() {
#if defined(_WIN32)
        WSACleanup();
#endif
    }
};

WinsockInit& winsockInit() {
    static WinsockInit instance;
    return instance;
}

} // namespace

Client::Client() {
    winsockInit();
    sockaddr_ = new sockaddr_in{};
}

Client::~Client() {
    disconnect();
    delete static_cast<sockaddr_in*>(sockaddr_);
}

bool Client::connect(const std::string& host, std::uint16_t port) {
    disconnect();

    socket_ = static_cast<int>(::socket(AF_INET, SOCK_DGRAM, 0));
    if (socket_ == TR_INVALID_SOCKET) {
        reaper::log("[OSC] socket() failed");
        socket_ = -1;
        return false;
    }

    auto* addr = static_cast<sockaddr_in*>(sockaddr_);
    std::memset(addr, 0, sizeof(*addr));
    addr->sin_family = AF_INET;
    addr->sin_port = htons(port);

    if (::inet_pton(AF_INET, host.c_str(), &addr->sin_addr) != 1) {
        reaper::log("[OSC] inet_pton failed for host=" + host);
        TR_CLOSE_SOCKET(socket_);
        socket_ = -1;
        return false;
    }

    host_ = host;
    port_ = port;
    reaper::debugLog("[OSC] client connected → " + host + ":" + std::to_string(port));
    return true;
}

bool Client::send(const Message& message) {
    if (socket_ < 0) {
        return false;
    }
    const auto bytes = message.encode();
    const auto* addr = static_cast<sockaddr_in*>(sockaddr_);
    const auto sent = ::sendto(
        socket_,
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<int>(bytes.size()),
        0,
        reinterpret_cast<const sockaddr*>(addr),
        sizeof(*addr)
    );
    if (sent < 0 || static_cast<std::size_t>(sent) != bytes.size()) {
        reaper::log("[OSC] sendto failed for " + message.address());
        return false;
    }
    return true;
}

void Client::disconnect() {
    if (socket_ >= 0) {
        TR_CLOSE_SOCKET(socket_);
        socket_ = -1;
    }
}

} // namespace totalreaper::osc
