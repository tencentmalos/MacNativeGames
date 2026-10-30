// SPDX-License-Identifier: MIT
#pragma once
#include <string>
#include <cstdint>
#include <vulkan/vulkan.h>
#include "spatial/core/utils/LiteTrace.h"
#include "spatial/core/imodules/IProfilerModule.hpp"

namespace mng::profile {
// Initialization and GPU operations run on the native/render owner, never the
// Android Service.dump thread. Debug commands do not acquire renderer locks.
enum class Backend { Vulkan, Metal };
void initialize(const std::string& process,const std::string& directory,const std::string& identity, Backend backend=Backend::Vulkan);
bool detailedCpu();
// Sample whole draws, preserving their nested CPU scopes as one population.
// Basic Draw/Translate scopes and non-draw detail remain complete.
class CpuDetailDrawScope {
public:
    CpuDetailDrawScope();
    ~CpuDetailDrawScope();
    CpuDetailDrawScope(const CpuDetailDrawScope&)=delete;
    CpuDetailDrawScope& operator=(const CpuDetailDrawScope&)=delete;
private:
    bool previous_selected;
};
const char* svcName(unsigned svc);
std::string command(const std::string& text);
void configureGpu(VkInstance,VkPhysicalDevice,VkDevice,VkQueue,uint32_t,bool calibrated);
void shutdownGpu();
using GpuZone=spatial::VulkanGPUProfileZoneCtx;
void beginGpu(GpuZone&,VkCommandBuffer,const char* name);
void endGpu(GpuZone&);
void collectGpu(VkCommandBuffer);
// Drain/reset GPU detail accounting on the renderer owner before releasing it.
// Completion may emit the frame marker later, on a separate thread.
void preparePresent();
void present(uint64_t token,bool prepared=false);
// Fine GPU probes never reset/collect queries inside a render pass.
class GpuCommandScope {
public:
    GpuCommandScope(VkCommandBuffer command,const char* name,bool draw);
    ~GpuCommandScope(){endGpu(zone);}
    GpuCommandScope(const GpuCommandScope&)=delete;
    GpuCommandScope& operator=(const GpuCommandScope&)=delete;
private:
    GpuZone zone{nullptr};
};
}
