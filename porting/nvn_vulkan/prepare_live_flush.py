# SPDX-License-Identifier: MIT
"""Track live output ownership without modifying the pinned D2R checkout."""
import re


def adapt(text, adapter):
    def replace(old, new, count=1):
        nonlocal text
        if text.count(old) != count:
            raise SystemExit(f"Pinned live-flush patch mismatch: {old[:100]}")
        text = text.replace(old, new)

    text = '#include "' + str(adapter / '../runtime/live_ranges.h') + '"\n' + text
    # Default member initializers retain actual output ranges at construction.
    # Vector copies/moves keep the same lease through retirement/writeback.
    replace('std::shared_ptr<const mng::port::ReportPool> mng_output_pool;};',
            'std::shared_ptr<const mng::port::ReportPool> mng_output_pool;'
            'mng::port::LiveRanges::Lease mng_live=mng::port::flush_dependencies().retain(addr,sizeof(NVNcounterData),mng::port::LiveRanges::Report);};')
    replace('struct PendingEvent {uint64_t addr;uint32_t mode,v;const void *slot;};',
            'struct PendingEvent {uint64_t addr;uint32_t mode,v;const void *slot;'
            'mng::port::LiveRanges::Lease mng_live=mng::port::flush_dependencies().retain(addr,4,mng::port::LiveRanges::Event);};')
    replace('struct PendingWriteback {uint64_t addr,bytes;Buffer buffer;const void *slot;};',
            'struct PendingWriteback {uint64_t addr,bytes;Buffer buffer;const void *slot;'
            'mng::port::LiveRanges::Lease mng_live=mng::port::flush_dependencies().retain(addr,bytes,mng::port::LiveRanges::Writeback);};')
    replace('struct Image : std::enable_shared_from_this<Image> {', '''struct Image : std::enable_shared_from_this<Image> {
    mng::port::LiveRanges::Lease mng_live_output;
    void mng_set_gpu_written(bool value) {
        if(value && !mng_live_output && source.mem && source.L.size)
            mng_live_output=mng::port::flush_dependencies().retain(uint64_t(source.mem),source.L.size,mng::port::LiveRanges::Image);
        if(!value)mng_live_output.reset();
        gpu_written=value;
    }''')
    # Initial image declarations stay false; every actual producer assignment
    # publishes an output owner, including alias-preserving render-pass reuse.
    text, count = re.subn(r'\b(\w+)(\.|->)gpu_written\s*=\s*(true|false)',
                         lambda m: m[1]+m[2]+'mng_set_gpu_written('+m[3]+')', text)
    if count != 15:
        raise SystemExit(f"Pinned image-write surface changed: {count}")
    # Storage outputs exist already while descriptor construction is in
    # progress. Keep that lease until the descriptor's GPU slot retires too.
    replace('struct Descriptors {', 'struct Descriptors {\n    std::vector<mng::port::LiveRanges::Lease> mng_live_outputs;')
    replace('mng_nvn_note_gpu_range(base,bytes);out.written.push_back(',
            'mng_nvn_note_gpu_range(base,bytes);out.mng_live_outputs.push_back(mng::port::flush_dependencies().retain(base,bytes,mng::port::LiveRanges::Writeback));out.written.push_back(', 2)
    replace('d->written.clear();', 'd->written.clear();d->mng_live_outputs.clear();')
    return text
