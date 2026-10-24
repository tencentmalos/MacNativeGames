// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <spatial/input/input_hub.h>
#include <map>
#include <mutex>
#include "../../games/totk/runtime/gamepad_state.h"
namespace mng {
class Input {
public:
    uint64_t begin();
    void end(uint64_t session);
    bool device(uint64_t session,int64_t id,uint64_t epoch,const std::string& profile,
                const spatial::input::DeviceCapabilities& caps);
    bool packet(uint64_t session,int64_t id,uint64_t epoch,uint64_t sequence,
                spatial::input::ControlEvent control,std::vector<spatial::input::InputEvent> events);
    RuntimeGamepadState read();
private:
    std::mutex mutex;
    spatial::input::InputHub hub;
    uint64_t token{};
    struct Device { uint64_t epoch; spatial::input::DeviceIdentity identity; };
    std::map<int64_t,Device> devices;
};
extern Input input;
}
