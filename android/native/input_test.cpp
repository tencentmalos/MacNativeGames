// SPDX-License-Identifier: GPL-2.0-or-later
#include "input.h"
#include <cstdlib>
#include <cstdio>
using namespace spatial::input;
#define CHECK(x) do {if(!(x)) {std::fprintf(stderr,"Failed: %s\n",#x);std::abort();}} while(0)
int main() {
    mng::Input input;auto s=input.begin();DeviceCapabilities caps;
    caps.buttons={Button::South,Button::East};caps.axes={{Axis::LeftStickY,-1,1,0}};
    CHECK(input.device(s,7,1,"test",caps));
    InputEvent button;button.button=Button::South;button.pressed=true;
    InputEvent axis;axis.kind=InputEvent::Kind::AxisValue;axis.axis=Axis::LeftStickY;axis.raw_value=-1;
    CHECK(input.packet(s,7,1,1,ControlEvent::None,{button,axis}));
    auto state=input.read();CHECK(state.connected && state.buttons==2 && state.ly==32767);
    CHECK(input.packet(s,7,1,2,ControlEvent::FocusLost,{}));CHECK(input.read().buttons==0);
    CHECK(input.packet(s,7,1,3,ControlEvent::DeviceRemoved,{}));CHECK(!input.read().connected);
    CHECK(input.device(s,7,2,"test",caps));
    CHECK(!input.packet(s,7,1,4,ControlEvent::None,{button}));
    CHECK(!input.device(s,7,1,"late registration",caps));
    CHECK(input.packet(s,7,2,1,ControlEvent::None,{button}));
    input.end(s);CHECK(!input.read().connected);
    auto next=input.begin();CHECK(next!=s);CHECK(!input.device(s,7,3,"old session",caps));
    input.end(s);CHECK(input.device(next,7,3,"new session",caps));
    std::puts("input: position mapping, axes, focus, disconnect, reconnect and session checks passed");
}
