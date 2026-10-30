// SPDX-License-Identifier: MIT
// Delay a fragment/texture dependency only across draws whose writes remain
// framebuffer-local. Attachment reads still close/store the pass through
// prepare_read or feedback snapshots. External shader accesses, non-draw
// operations and pass changes materialize the pending dependency first.
namespace mng_fragment_barrier {
bool enabled(){static const bool value=[] {const char *v=getenv("MNG_DEFER_FRAGMENT_BARRIER");return v && !strcmp(v,"1");}();return value;}
bool external_access(){
    for(unsigned st=0;st<5;++st)if(auto *shader=state.shader[st]){
        if(!shader->barrier_metadata_known){
            // The existing SPIR-V write analysis marks every storage buffer
            // writable when a write has an unresolved pointer root. Read-only
            // storage bindings cannot publish cross-pixel side effects.
            shader->barrier_external=!shader->storage_writes.empty();
            for(const auto &b:shader->bindings)shader->barrier_external|=
                b.type==VK_DESCRIPTOR_TYPE_STORAGE_IMAGE || b.type==VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
            shader->barrier_metadata_known=true;
        }
        if(shader->barrier_external)return true;
    }
    return false;
}
bool defer(uint32_t bits){
    // All earlier external work already crosses Commands(false)'s full
    // dependency. Until another external operation, only the open framebuffer
    // can acquire new writes. Sampling it closes the pass before the read;
    // ordinary attachment operations retain Vulkan rasterization order.
    // Shader modules/descriptors are immutable host snapshots, not writable
    // device instruction/descriptor memory. Preserve a full dependency at
    // the eventual boundary; this never drops an invalidation request.
    constexpr uint32_t known=NVN_BARRIER_ORDER_PRIMITIVES_BIT|NVN_BARRIER_ORDER_FRAGMENTS_BIT|
        NVN_BARRIER_ORDER_FRAGMENTS_TILED_BIT|NVN_BARRIER_ORDER_INDIRECT_DATA_BIT|
        NVN_BARRIER_INVALIDATE_TEXTURE_BIT|NVN_BARRIER_INVALIDATE_SHADER_BIT|
        NVN_BARRIER_INVALIDATE_TEXTURE_DESCRIPTOR_BIT|NVN_BARRIER_INVALIDATE_ZCULL_BIT;
    if(!enabled() || !bits || (bits&~known) ||
       !open_pass.cmd || open_pass.external_access)return false;
    mng_feedback::boundary();Commands c(true);open_pass.deferred_fragment_barrier=true;
    spatial::LiteTrace::counter("NVN.Barrier.Deferred",bits);return true;
}
void before_draw(){if(open_pass.deferred_fragment_barrier && external_access())end_open_pass(12);}
void after_draw(){open_pass.external_access|=external_access();}
}
