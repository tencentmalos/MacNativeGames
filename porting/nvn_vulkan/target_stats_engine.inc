// SPDX-License-Identifier: MIT
// Included after Pass. Bounded per-frame pass extents; never retain resources.
namespace mng_targets {
struct Key {
    uint32_t width,height,samples,colors,depth;
    bool operator<(const Key& b)const{return std::tie(width,height,samples,colors,depth)<std::tie(b.width,b.height,b.samples,b.colors,b.depth);}
};
std::map<Key,uint64_t> current;
uint64_t omitted{};
std::mutex cache_mutex;
std::string cached="render_state=no_present_yet\n";
void pass(const Pass& p) {
    bool depth=false;unsigned colors{};
    for(const auto *image:p.images)if(image->aspects==VK_IMAGE_ASPECT_COLOR_BIT)++colors;else depth=true;
    const Key key{p.width,p.height,uint32_t(p.samples),colors,uint32_t(depth)};
    auto it=current.find(key);
    if(it!=current.end())++it->second;
    else if(current.size()<128)current.emplace(key,1);
    else ++omitted;
}
void present(const win_t *window,const tex_t *texture) {
    std::string value="schema=mng.targets.v1\nframe="+std::to_string(stats.presents.load()+1)+
        "\npresent_width="+std::to_string(texture->d.width)+"\npresent_height="+std::to_string(texture->d.height)+
        "\ncrop_x="+std::to_string(window->crop[0])+"\ncrop_y="+std::to_string(window->crop[1])+
        "\ncrop_width="+std::to_string(window->crop[2]?window->crop[2]:texture->d.width)+
        "\ncrop_height="+std::to_string(window->crop[3]?window->crop[3]:texture->d.height)+
        "\ninternal_scale=1\nomitted_passes="+std::to_string(omitted)+"\n";
    value+="guest_clock=submit_wait_midpoint\nguest_clock_anchor_gpu="+std::to_string(mng_clock::anchor_gpu)+
        "\nguest_clock_anchor_host_ns="+std::to_string(mng_clock::anchor_host)+
        "\nguest_clock_deviation_ns="+std::to_string(mng_clock::deviation)+"\n";
    unsigned row{};
    for(const auto &[k,count]:current) {
        char line[180];snprintf(line,sizeof line,"target_%u=%ux%u samples:%u colors:%u depth:%u render_passes:%llu\n",
            row++,k.width,k.height,k.samples,k.colors,k.depth,(unsigned long long)count);
        value+=line;
    }
    {std::lock_guard lock(cache_mutex);cached=std::move(value);}
    current.clear();omitted=0;
}
}
