// SPDX-License-Identifier: MIT
// Exercise production cache removal with a retirement slot that resolves a
// pending writeback. Its range lookup rebuilds the index during retirement.
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <map>
#include <memory>
#include <unordered_set>
#include <vector>
#include <cstdio>
struct Image {
    struct {void *mem;struct {size_t size;} L;} source{};
    uint64_t storage_last_use{};
};
struct ImageStorage {std::shared_ptr<Image> owner;size_t serials;};
std::map<uint64_t,std::shared_ptr<Image>> images;
std::map<Image*,ImageStorage> image_storage;
std::vector<Image*> image_index,img_cache;
std::vector<std::shared_ptr<Image>> retired;
bool image_index_dirty=true;
unsigned images_epoch=0,memo_check_gen=0,memo_epoch=0;
struct {uint64_t submissions=1000;} stats;
void last_pass_reset() {}
void rebuild_index() {
    image_index.clear();
    for (auto &[image,storage]:image_storage) image_index.push_back(image);
    image_index_dirty=false;
}
void retire_object(std::shared_ptr<Image> image) {
    if (image_index_dirty) rebuild_index();
    // This is what resolve_slot_writebacks -> invalidate_cpu_range observes.
    for(auto *indexed:image_index) assert(image_storage.count(indexed));
    retired.push_back(std::move(image));
}
#include "../nvn_vulkan/image_storage_engine.inc"
int main() {
    auto kept=std::make_shared<Image>();kept->source={reinterpret_cast<void*>(0x3000),{64}};
    image_storage.emplace(kept.get(),ImageStorage{kept,1});images.emplace(3,kept);
    std::weak_ptr<Image> removed;
    for(unsigned i=0;i<2;++i){
        auto image=std::make_shared<Image>();image->source={reinterpret_cast<void*>(0x1000+i*64),{64}};
        image_storage.emplace(image.get(),ImageStorage{image,1});images.emplace(i,image);removed=image;
    }
    rebuild_index();
    mng_storage_drop_pool(0x1000,128);
    assert(image_storage.size()==1 && images.size()==1 && !removed.expired());
    retired.clear();assert(removed.expired());
    if(image_index_dirty)rebuild_index();
    assert(image_index.size()==1 && image_index[0]==kept.get());
    puts("Retirement-time index rebuild and fence-deferred ownership passed");
}
