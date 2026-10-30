// SPDX-License-Identifier: MIT
#pragma once
#include <memory>
#include <string>
namespace mng::diagnostics {
// Local, owner-only transport. The shared registry remains the command owner.
// No renderer/session lock is held by this transport's thread.
class SocketDebugBus {
public:
    explicit SocketDebugBus(const std::string& path);
    ~SocketDebugBus();
    SocketDebugBus(const SocketDebugBus&)=delete;
    SocketDebugBus& operator=(const SocketDebugBus&)=delete;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
