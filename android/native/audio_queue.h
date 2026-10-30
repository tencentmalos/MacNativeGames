// SPDX-License-Identifier: MIT
#pragma once
#include "platform.h"
#include <foundation/audio/callback_mixer.h>
#include <array>
#include <vector>

namespace mng {
// Foundation borrows submitted PCM. Reuse a slot only after its ticket retires;
// the owner must quiesce the source before destroying this queue.
class AudioBlockQueue {
public:
    static constexpr unsigned Capacity=3;
    AudioBlockQueue(Foundation::Audio::AudioSource& source,unsigned frames,MngAudioPull pull,void *context)
        : source(source),frames(frames),pull(pull),context(context) {
        for(auto& block:blocks)block.resize(size_t(frames)*2);
    }
    bool fill() {
        for(unsigned n=0;n<Capacity;++n) {
            const auto slot=next%Capacity;
            if(tickets[slot]>source.ConsumedBlocks())break;
            if(source.Error())return false;
            pull(context,blocks[slot].data(),frames);
            auto ticket=source.TrySubmit(blocks[slot]);
            if(!ticket)return false;
            tickets[slot]=ticket;++next;
        }
        return true;
    }
private:
    Foundation::Audio::AudioSource& source;
    unsigned frames;
    MngAudioPull pull;
    void *context;
    uint64_t next{};
    std::array<uint64_t,Capacity> tickets{};
    std::array<std::vector<float>,Capacity> blocks;
};
}
