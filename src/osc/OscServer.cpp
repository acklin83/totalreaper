// OscServer.cpp — UDP receive loop, cross-platform.

#include "OscServer.h"
#include "../reaper/Console.h"

#include <array>
#include <cstring>

#if defined(_WIN32)
  #include <winsock2.h>
  #include <ws2tcpip.h>
  using socklen_t = int;
  #define TR_CLOSE_SOCKET ::closesocket
  #define TR_INVALID_SOCKET INVALID_SOCKET
  #define TR_LAST_ERROR WSAGetLastError()
#else
  #include <arpa/inet.h>
  #include <fcntl.h>
  #include <netinet/in.h>
  #include <sys/socket.h>
  #include <sys/time.h>
  #include <unistd.h>
  #define TR_CLOSE_SOCKET ::close
  #define TR_INVALID_SOCKET (-1)
  #define TR_LAST_ERROR errno
#endif

namespace totalreaper::osc {

namespace {

// Set a 250 ms recv timeout so the loop can check `running_` regularly
// without blocking forever. We want stop() to be responsive.
bool setRecvTimeout(int sock, int millis) {
#if defined(_WIN32)
    DWORD t = static_cast<DWORD>(millis);
    return ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
                        reinterpret_cast<const char*>(&t), sizeof(t)) == 0;
#else
    timeval tv{};
    tv.tv_sec = millis / 1000;
    tv.tv_usec = (millis % 1000) * 1000;
    return ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0;
#endif
}

constexpr std::size_t kRecvBufferSize = 64 * 1024; // OSC messages can be large

// Dispatch a raw OSC packet — either a single message or a bundle. Bundles
// nest (#bundle marker + timetag + repeated <int32 size BE><element>), and
// each element is itself a packet, so we recurse. TotalMix wraps essentially
// all of its outbound traffic in bundles, so this is the hot path.
void dispatchPacket(const std::uint8_t* data,
                    std::size_t size,
                    const MessageHandler& handler) {
    if (size >= 8 && std::memcmp(data, "#bundle\0", 8) == 0) {
        // Skip bundle header (8) + timetag (8). Then iterate elements.
        std::size_t offset = 16;
        while (offset + 4 <= size) {
            std::uint32_t elementSize = 0;
            std::memcpy(&elementSize, data + offset, 4);
            elementSize = ntohl(elementSize);
            offset += 4;
            if (elementSize == 0 || offset + elementSize > size) break;
            dispatchPacket(data + offset, elementSize, handler);
            offset += elementSize;
        }
        return;
    }

    Message message;
    if (Message::decode(data, size, message) && handler) {
        handler(message);
    }
}

} // namespace

Server::Server() = default;

Server::~Server() {
    stop();
}

bool Server::start(std::uint16_t port, MessageHandler handler) {
    if (running_.load()) {
        return true; // already running
    }

    handler_ = std::move(handler);
    port_ = port;

    socket_ = static_cast<int>(::socket(AF_INET, SOCK_DGRAM, 0));
    if (socket_ == TR_INVALID_SOCKET) {
        reaper::log("[OSC] server socket() failed");
        socket_ = -1;
        return false;
    }

    // Allow rebinding after a quick stop/start cycle.
    int yes = 1;
    ::setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&yes), sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (::bind(socket_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        reaper::log("[OSC] bind() failed on port " + std::to_string(port));
        TR_CLOSE_SOCKET(socket_);
        socket_ = -1;
        return false;
    }

    if (!setRecvTimeout(socket_, 250)) {
        reaper::log("[OSC] warning: recv timeout could not be set");
    }

    running_.store(true);
    thread_ = std::thread(&Server::receiveLoop, this);
    reaper::log("[OSC] server listening on UDP " + std::to_string(port));
    return true;
}

void Server::stop() {
    if (!running_.load()) {
        return;
    }
    running_.store(false);
    if (thread_.joinable()) {
        thread_.join();
    }
    if (socket_ >= 0) {
        TR_CLOSE_SOCKET(socket_);
        socket_ = -1;
    }
    reaper::log("[OSC] server stopped");
}

void Server::receiveLoop() {
    std::array<std::uint8_t, kRecvBufferSize> buffer{};
    while (running_.load()) {
        sockaddr_in from{};
        socklen_t fromLen = sizeof(from);
        const auto received = ::recvfrom(
            socket_,
            reinterpret_cast<char*>(buffer.data()),
            static_cast<int>(buffer.size()),
            0,
            reinterpret_cast<sockaddr*>(&from),
            &fromLen
        );
        if (received < 0) {
            // Likely just the timeout — loop back and re-check `running_`.
            continue;
        }
        if (received == 0) {
            continue;
        }

        dispatchPacket(buffer.data(),
                       static_cast<std::size_t>(received),
                       handler_);
    }
}

} // namespace totalreaper::osc
