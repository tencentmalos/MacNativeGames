// SPDX-License-Identifier: MIT
#include "audio_queue.h"
#include <cassert>
#include <cmath>
#include <cstdio>
using namespace Foundation::Audio;
int main() {
    CallbackMixer mixer;
    auto source=mixer.CreateSource(mng::AudioBlockQueue::Capacity);
    unsigned generated{};
    auto pull=[](void *context,float *pcm,unsigned frames) {
        auto& next=*static_cast<unsigned*>(context);
        for(unsigned i=0;i<frames;++i,++next)
            pcm[2*i]=pcm[2*i+1]=float(next%1024+1)/1025;
    };
    mng::AudioBlockQueue queue(*source,240,pull,&generated);
    unsigned consumed{};
    for(unsigned n=0;n<4000;++n) {
        assert(queue.fill());
        // Device bursts cross 5-ms guest block boundaries, including 288/240.
        const unsigned sizes[]{288,192,144,480};
        const auto frames=sizes[n%4];std::vector<float> out(frames*2);
        mixer.Render(out);
        for(unsigned i=0;i<frames;++i,++consumed) {
            const auto expected=float(consumed%1024+1)/1025;
            assert(out[2*i]==expected && out[2*i+1]==expected);
        }
    }
    const auto stats=mixer.Snapshot();
    assert(stats.starved_frames==0 && stats.consumed_frames==consumed);
    source->RequestStop();assert(!queue.fill());
    printf("PASS: %u continuous stereo frames across variable callbacks; no missing/repeated PCM or borrowed-buffer reuse; cancellation quiesced\n",consumed);
}
