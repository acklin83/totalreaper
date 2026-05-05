// OscServer.h — UDP receive-only OSC server.
//
// Listens on a UDP port in a background thread, decodes incoming OSC
// messages, and dispatches them to a callback. Used to mirror TotalMix's
// state into TotalReaper.

#pragma once

#include "OscMessage.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>

namespace totalreaper::osc {

// Callback invoked from the receive thread for every parsed message.
// Implementations must be thread-safe (this fires on a background thread,
// not on REAPER's main thread).
using MessageHandler = std::function<void(const Message&)>;

class Server {
public:
    Server();
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Bind to UDP port and start the receive thread. Returns false if the
    // port can't be bound. Idempotent — calling start() while running is
    // a no-op.
    bool start(std::uint16_t port, MessageHandler handler);

    // Signal the receive thread to exit and join it.
    void stop();

    bool isRunning() const noexcept { return running_.load(); }

private:
    void receiveLoop();

    int socket_ = -1;
    std::uint16_t port_ = 0;
    std::atomic<bool> running_{false};
    std::thread thread_;
    MessageHandler handler_;
};

} // namespace totalreaper::osc
