// SPDX-License-Identifier: MIT
// Included in the serialized engine. No guest addresses or title patches.
namespace mng_clock {
mng::port::GpuClock clock;
uint64_t anchor_host{}, anchor_gpu{}, deviation{}, suspend_offset{}, reports{};
uint64_t host_now() {
    const auto value=mng_host_clock_ns(0);need(value,"Guest clock unavailable");return value;
}
uint64_t boot_now() {
    const auto value=mng_host_clock_ns(1);need(value,"Continuous clock unavailable");return value;
}
// Like Foundation's fenced midpoint fallback, calibrate using an actual query
// from this queue, not an unvalidated driver extension or the raw GPU epoch.
// Call only with a quiescent queue; use the narrowest of eight measured bounds.
void calibrate() {
    spatial::LiteTraceScope calibration_scope("Host.GpuClock.Calibrate");
    need(timestamp_bits>0 && timestamp_bits<=64,"Guest GPU timestamps unavailable");
    VkCommandPool pool{}; VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.queueFamilyIndex=cfg.queue_family; pi.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    check(vkCreateCommandPool(cfg.device,&pi,nullptr,&pool),"clock command pool");
    VkCommandBuffer cmd{}; VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool=pool; ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount=1;
    check(vkAllocateCommandBuffers(cfg.device,&ai,&cmd),"clock command buffer");
    VkQueryPool queries{}; VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qi.queryType=VK_QUERY_TYPE_TIMESTAMP; qi.queryCount=1;
    check(vkCreateQueryPool(cfg.device,&qi,nullptr,&queries),"clock query pool");
    VkFence fence{}; VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    check(vkCreateFence(cfg.device,&fi,nullptr,&fence),"clock fence");
    deviation=UINT64_MAX;
    for(unsigned attempt=0;attempt<8;++attempt) {
        check(vkResetCommandBuffer(cmd,0),"clock reset command");
        check(vkResetFences(cfg.device,1,&fence),"clock reset fence");
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(cmd,&bi),"clock begin");
        vkCmdResetQueryPool(cmd,queries,0,1);
        vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,queries,0);
        check(vkEndCommandBuffer(cmd),"clock end");
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount=1; si.pCommandBuffers=&cmd;
        const auto before=host_now();
        check(vkQueueSubmit(cfg.queue,1,&si,fence),"clock submit");
        check(vkWaitForFences(cfg.device,1,&fence,VK_TRUE,cfg.fence_timeout_ns),"clock wait");
        const auto after=host_now(); uint64_t tick{};
        check(vkGetQueryPoolResults(cfg.device,queries,0,1,sizeof tick,&tick,sizeof tick,VK_QUERY_RESULT_64_BIT),"clock result");
        if(after-before<deviation){deviation=after-before;anchor_host=before+deviation/2;anchor_gpu=tick;}
    }
    need(clock.anchor(anchor_gpu,anchor_host,properties.limits.timestampPeriod,timestamp_bits),"Invalid GPU clock anchor");
    suspend_offset=boot_now()-host_now();
    vkDestroyFence(cfg.device,fence,nullptr); vkDestroyQueryPool(cfg.device,queries,nullptr);
    vkDestroyCommandPool(cfg.device,pool,nullptr);
    xbg_logf("NVN guest clock mode=submit_wait_midpoint gpu=%llu host_ns=%llu deviation_ns=%llu period=%.9g bits=%u",
        (unsigned long long)anchor_gpu,(unsigned long long)anchor_host,(unsigned long long)deviation,properties.limits.timestampPeriod,timestamp_bits);
}
bool refresh_needed() {
    const auto now=host_now(), offset=boot_now()-now;
    const auto suspend_delta=offset>suspend_offset?offset-suspend_offset:suspend_offset-offset;
    // Refresh at the present boundary. Stay within a quarter wrap even on
    // devices with fewer timestamp bits; drain reports before changing anchors.
    const double refresh_ns=std::min(60000000000.0,std::ldexp(double(properties.limits.timestampPeriod),int(timestamp_bits)-2));
    return !anchor_host || double(now-anchor_host)>refresh_ns || suspend_delta>10000000;
}
uint64_t ticks(uint64_t raw) {
    uint64_t ns{}; need(clock.nanoseconds(raw,ns),"GPU timestamp outside calibrated range");
    if(reports++<8)xbg_logf("NVN clock report raw=%llu mapped_ns=%llu host_ns=%llu",
        (unsigned long long)raw,(unsigned long long)ns,(unsigned long long)host_now());
    return mng::port::GpuClock::guestTicks(ns);
}
}
