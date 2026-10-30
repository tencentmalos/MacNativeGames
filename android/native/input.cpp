// SPDX-License-Identifier: GPL-2.0-or-later
#include "input.h"
#include <algorithm>
#include <cmath>
using namespace spatial::input;
namespace mng {
Input input;
void Input::configure(bool value) { std::lock_guard lock(mutex); swap_ab=value; }
uint64_t Input::begin() { std::lock_guard lock(mutex); devices.clear(); return token=hub.BeginSession(); }
void Input::end(uint64_t session) {
    std::lock_guard lock(mutex); if(session!=token) return;
    hub.EndSession(token); devices.clear(); token=0;
}
bool Input::device(uint64_t session,int64_t id,uint64_t epoch,const std::string& profile,const DeviceCapabilities& caps) {
    std::lock_guard lock(mutex); if(!session || session!=token || !epoch) return false;
    auto it=devices.find(id);
    if(it!=devices.end()) {
        if(epoch<it->second.epoch) return false;
        if(epoch==it->second.epoch) return hub.UpdateCapabilities(it->second.identity,caps)==InputResult::Ok;
        hub.RemoveDevice(it->second.identity);
    }
    auto nativeEpoch=hub.RegisterDevice(token,Source::AndroidGamepad,id,profile,caps);
    if(!nativeEpoch) return false;
    devices[id]={epoch,{Source::AndroidGamepad,id,nativeEpoch,profile}}; return true;
}
bool Input::packet(uint64_t session,int64_t id,uint64_t epoch,uint64_t seq,ControlEvent control,std::vector<InputEvent> events) {
    std::lock_guard lock(mutex); auto it=devices.find(id);
    if(!session || session!=token || it==devices.end() || epoch!=it->second.epoch) return false;
    if(!seq || seq<=it->second.incoming_sequence)return false;
    it->second.incoming_sequence=seq;
    InputPacket packet;packet.session_token=token;packet.device=it->second.identity;
    packet.sequence=++it->second.hub_sequence;packet.control=control;packet.events=std::move(events);
    return hub.Submit(packet)==InputResult::Ok;
}
void Input::block(bool value) {std::lock_guard lock(mutex);blocked=value;}
void Input::clear() {
    std::lock_guard lock(mutex);
    for(auto &[id,device]:devices) {
        InputPacket packet;packet.session_token=token;packet.device=device.identity;
        packet.sequence=++device.hub_sequence;packet.control=ControlEvent::Reset;
        (void)hub.Submit(packet);
    }
}
RuntimeGamepadState Input::read() {
    std::lock_guard lock(mutex); RuntimeGamepadState out{};
    for(const auto& [id,device]:devices) {
        auto state=hub.Snapshot(device.identity); if(!state || !state->connected) continue;
        // Preserve Android A/B/X/Y labels for the handheld's NS layout.
        // Foundation encodes those keycodes as South/East/West/North; applying
        // another positional Switch conversion here reversed both face pairs.
        constexpr int bits[]={0,1,2,3,13,15,12,14,6,7,8,9,4,5,10,11,-1,-1};
        for(size_t i=0;i<std::size(bits);i++) if(bits[i]>=0 && state->buttons[i]) out.buttons|=uint64_t(1)<<bits[i];
        if(state->axes[4]>0.5f) out.buttons|=1ull<<8;
        if(state->axes[5]>0.5f) out.buttons|=1ull<<9;
        auto stick=[](float v){return int32_t(std::lround(std::clamp(v,-1.f,1.f)*32767));};
        out.lx=stick(state->axes[0]);out.ly=stick(-state->axes[1]);
        out.rx=stick(state->axes[2]);out.ry=stick(-state->axes[3]);out.connected=1;
        // Apply the host preference to the final snapshot so press/release,
        // simultaneous buttons and reconnects all use the same mapping.
        if(swap_ab) out.buttons=(out.buttons&~uint64_t(3))|((out.buttons&1)<<1)|((out.buttons&2)>>1);
        break; // Deterministic single-player device selection.
    }
    if(blocked){out.buttons=0;out.lx=out.ly=out.rx=out.ry=0;}
    return out;
}
}
extern "C" void runtime_gamepad_read(RuntimeGamepadState *out) { if(out) *out=mng::input.read(); }
