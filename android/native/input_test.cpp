// SPDX-License-Identifier: GPL-2.0-or-later
#include "input.h"
#include <cstdlib>
#include <cstdio>
using namespace spatial::input;
#define CHECK(x) do {if(!(x)) {std::fprintf(stderr,"Failed: %s\n",#x);std::abort();}} while(0)
int main() {
    mng::Input input;auto s=input.begin();DeviceCapabilities caps;
    caps.buttons={Button::South,Button::East,Button::North,Button::West};caps.axes={{Axis::LeftStickY,-1,1,0}};
    CHECK(input.device(s,7,1,"test",caps));
    InputEvent button;button.button=Button::South;button.pressed=true;
    InputEvent axis;axis.kind=InputEvent::Kind::AxisValue;axis.axis=Axis::LeftStickY;axis.raw_value=-1;
    CHECK(input.packet(s,7,1,1,ControlEvent::None,{button,axis}));
    auto state=input.read();CHECK(state.connected && state.buttons==1 && state.ly==32767);
    CHECK(input.packet(s,7,1,2,ControlEvent::FocusLost,{}));CHECK(input.read().buttons==0);
    CHECK(input.packet(s,7,1,3,ControlEvent::DeviceRemoved,{}));CHECK(!input.read().connected);
    CHECK(input.device(s,7,2,"test",caps));
    CHECK(!input.packet(s,7,1,4,ControlEvent::None,{button}));
    CHECK(!input.device(s,7,1,"late registration",caps));
    CHECK(input.packet(s,7,2,1,ControlEvent::None,{button}));
    input.clear();CHECK(input.read().connected && input.read().buttons==0);
    CHECK(input.packet(s,7,2,2,ControlEvent::None,{button}));CHECK(input.read().buttons==1);
    CHECK(!input.packet(s,7,2,2,ControlEvent::None,{button}));
    input.block(true);CHECK(input.read().connected && !input.read().buttons && !input.read().ly);
    input.block(false);CHECK(input.read().buttons==1);
    input.configure(true);CHECK(input.read().buttons==2);
    InputEvent east=button;east.button=Button::East;
    InputEvent north=button;north.button=Button::North;
    InputEvent west=button;west.button=Button::West;
    CHECK(input.packet(s,7,2,3,ControlEvent::None,{east,north,west}));CHECK(input.read().buttons==15);
    button.pressed=false;
    CHECK(input.packet(s,7,2,4,ControlEvent::None,{button}));CHECK(input.read().buttons==13);
    input.configure(false);CHECK(input.read().buttons==14);
    // Android keycodes A/B/X/Y traverse Foundation as South/East/West/North.
    // Verify each guest bit separately, including release and the A/B override.
    uint64_t sequence=5;
    const Button faces[]={Button::South,Button::East,Button::West,Button::North};
    for(bool swap : {false,true}) {
        input.configure(swap);input.clear();
        for(unsigned i=0;i<4;i++) {
            InputEvent face;face.button=faces[i];face.pressed=true;
            CHECK(input.packet(s,7,2,sequence++,ControlEvent::None,{face}));
            const unsigned bit=swap && i<2 ? i^1u : i;
            CHECK(input.read().buttons==(1ull<<bit));
            face.pressed=false;
            CHECK(input.packet(s,7,2,sequence++,ControlEvent::None,{face}));
            CHECK(input.read().buttons==0);
        }
    }
    input.configure(true);input.clear();CHECK(input.read().buttons==0);
    input.end(s);CHECK(!input.read().connected);
    auto next=input.begin();CHECK(next!=s);CHECK(!input.device(s,7,3,"old session",caps));
    input.end(s);CHECK(input.device(next,7,3,"new session",caps));
    button.pressed=true;CHECK(input.packet(next,7,3,1,ControlEvent::None,{button}));CHECK(input.read().buttons==2);
    std::puts("input: default NS face labels, optional A/B swap, axes, focus, disconnect, reconnect and session checks passed");
}
