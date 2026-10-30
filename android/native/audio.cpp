// SPDX-License-Identifier: MIT
#include "platform.h"
#include "audio_queue.h"
#include <chrono>
#include <cstdio>
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
    mng::AudioBlockQueue queue;
    std::jthread worker;
    Output(int rate, MngAudioPull pull, void *ctx)
        : endpoint(AndroidOutputConfig{.sample_rate=rate,.observe=[](const DeviceSnapshot& d){
            static thread_local uint64_t last{};
            if(d.mixer.output_frames>=last+uint64_t(d.sample_rate)*5 || !last) {
                last=d.mixer.output_frames;
                fprintf(stderr,"[android-audio] rate=%d burst=%d output=%llu consumed=%llu starved=%llu queued=%llu xruns=%d generation=%llu\n",
                    d.sample_rate,d.burst,(unsigned long long)d.mixer.output_frames,
                    (unsigned long long)d.mixer.consumed_frames,(unsigned long long)d.mixer.starved_frames,
                    (unsigned long long)d.mixer.queued_frames,d.xruns,(unsigned long long)d.generation);
            }
        }}), source(endpoint.CreateSource(mng::AudioBlockQueue::Capacity)),
          queue(*checked_source(),unsigned(rate/200),pull,ctx) {
        if (!source) throw std::runtime_error("audio source allocation failed");
        worker=std::jthread([this](std::stop_token stop) {
            while(!stop.stop_requested() && queue.fill())
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        });
    }
    AudioSource* checked_source() {
        if(!source)throw std::runtime_error("audio source allocation failed");
        return source.get();
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
