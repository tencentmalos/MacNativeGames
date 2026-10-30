// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <string>
namespace mng::diagnostics {
struct FrameStats {
    uint64_t frame{}, gpu_frame{}, draws{}, dispatches{}, pipelines{}, shaders{}, upload_bytes{};
    double frame_ms{}, cpu_process_ms{}, present_ms{}, gpu_ms{-1}, pipeline_ms{}, shader_ms{}, upload_ms{};
    int present_interval{-1};
    uint64_t present_count{};
};
// Called by owners; service requests only read bounded copies via try_lock.
void setIdentity(std::string identity);
void setRuntime(std::string snapshot);
void frame(const FrameStats& stats);
using ThreadSnapshot = int (*)(char*, unsigned);
void setThreadSnapshot(ThreadSnapshot callback);
void setResourceSnapshot(ThreadSnapshot callback);
void setTargetSnapshot(ThreadSnapshot callback);
void setFlushSnapshot(ThreadSnapshot callback);
void setSystemModeSnapshot(ThreadSnapshot callback);
void setAutoloadSnapshot(ThreadSnapshot callback);
using ScheduleControl = int (*)(int adaptive,int batch_ops,int present,int invalidate,int reads,int descriptors,int images,int pipeline,int plans,int flush,char*,unsigned);
void setScheduleControl(ScheduleControl callback);
using FlushImageControl = int (*)(int enabled,char*,unsigned);
void setFlushImageControl(FlushImageControl callback);
void setFrontLockControl(FlushImageControl callback);
void setOrderedInputsControl(FlushImageControl callback);
void setInvalidateControl(FlushImageControl callback);
void setPublicationControl(FlushImageControl callback);
void setLaneCallbacks(FlushImageControl control,void (*present)(uint64_t));
using InputPulse = int (*)(int direction,unsigned milliseconds,char*,unsigned);
void setInputPulse(InputPulse callback);
std::string command(const std::string& request);
}
