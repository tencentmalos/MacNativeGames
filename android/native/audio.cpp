// SPDX-License-Identifier: MIT
#include "platform.h"
#include <foundation/audio/oboe_output.h>
#include <android/log.h>
#include <thread>
#include <vector>
#include <memory>
#include <exception>
#include <stdexcept>
using namespace Foundation::Audio;
namespace {
struct Output {
    AndroidOutput endpoint;
    std::unique_ptr<AudioSource> source;
    std::jthread worker;
    Output(int rate, MngAudioPull pull, void *ctx)
        : endpoint(AndroidOutputConfig{.sample_rate=rate}), source(endpoint.CreateSource(3)) {
        if (!source) throw std::runtime_error("audio source allocation failed");
        worker=std::jthread([this,rate,pull,ctx](std::stop_token stop) {
            std::vector<float> pcm(size_t(rate/200)*2);
            while(!stop.stop_requested()) {
                pull(ctx,pcm.data(),unsigned(pcm.size()/2));
                if(source->SubmitAndWait(pcm,stop)!=SubmitResult::Consumed) break;
            }
        });
    }
    ~Output() { worker.request_stop(); source->RequestStop(); worker.join(); }
};
}
extern "C" void *mng_audio_open(int rate,MngAudioPull pull,void *ctx) {
    if(!pull || (rate!=32000 && rate!=48000)) return nullptr;
    try { return new Output(rate,pull,ctx); }
    catch(const std::exception& e) { __android_log_print(ANDROID_LOG_ERROR,"MacNative","Audio: %s",e.what()); return nullptr; }
}
extern "C" void mng_audio_close(void *p) { delete static_cast<Output*>(p); }
extern "C" void mng_audio_reopen(void *p) { if(p) static_cast<Output*>(p)->endpoint.RequestReopen(); }
extern "C" uint64_t mng_audio_consumed(void *p) { return p ? static_cast<Output*>(p)->source->ConsumedFrames() : 0; }
