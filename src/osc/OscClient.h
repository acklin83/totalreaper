// OscClient.h — UDP send-only OSC client.
//
// Sends pre-encoded OSC messages to a UDP target. No retry, no buffering —
// OSC over UDP is fire-and-forget by spec.

#pragma once

#include "OscMessage.h"

#include <cstdint>
#include <string>

namespace totalreaper::osc {

class Client {
public:
    Client();
    ~Client();

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    Client(Client&&) = delete;
    Client& operator=(Client&&) = delete;

    // Open a UDP socket pointing at host:port. Returns false on failure.
    // Subsequent calls reconfigure the target (cheap — UDP is connectionless).
    bool connect(const std::string& host, std::uint16_t port);

    // True after a successful connect().
    bool isConnected() const noexcept { return socket_ >= 0; }

    // Send an encoded message. Returns false on send error.
    bool send(const Message& message);

    // Tear down the socket.
    void disconnect();

private:
    int socket_ = -1;
    std::string host_;
    std::uint16_t port_ = 0;

    struct Impl;
    void* sockaddr_ = nullptr; // opaque sockaddr_in storage
};

} // namespace totalreaper::osc
