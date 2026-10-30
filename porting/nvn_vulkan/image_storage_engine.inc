// SPDX-License-Identifier: MIT
// Included after the renderer's image and descriptor caches are defined.
// The serial map is a lookup cache; image_storage owns allocation contents.
void mng_storage_invalidate() {
    ++images_epoch;
    ++memo_check_gen;
    ++memo_epoch;
    img_cache.clear();
    last_pass_reset();
}

template<class Predicate> void mng_storage_drop(Predicate pick) {
    std::unordered_set<Image*> gone;
    for (const auto &[image, storage] : image_storage)
        if (pick(*image, storage)) gone.insert(image);
    if (gone.empty()) return;
    for (auto it = images.begin(); it != images.end();)
        if (gone.count(it->second.get())) it = images.erase(it); else ++it;
    mng_storage_invalidate();
    image_index_dirty = true;
    for (auto *image : gone) {
        // The current slot is ordered after earlier readers on this queue.
        // Keep images/views/framebuffers alive until its fence completes.
        retire_object(image_storage.at(image).owner);
        image_storage.erase(image);
        // Acquiring the retirement slot can resolve a writeback, rebuilding
        // this index while the owner is still in image_storage. Invalidate
        // again after removal so the next lookup cannot resurrect a raw pointer.
        image_index_dirty = true;
    }
}

void mng_storage_drop_pool(uintptr_t address, size_t bytes) {
    mng_storage_drop([&](const Image &image, const ImageStorage &) {
        const auto begin = uintptr_t(image.source.mem);
        const auto size = image.source.L.size;
        return begin >= address ? begin - address < bytes : address - begin < size;
    });
}

void mng_storage_sweep() {
    // Replay may first encounter a texture after its temporary Guest copy was
    // finalized. Bound such orphan lookup identities without losing storage.
    if (images.size() > std::max<size_t>(8192, image_storage.size() * 4)) {
        images.clear();
        for (auto &[image, storage] : image_storage) {
            storage.serials = 0;
            image->storage_last_use = stats.submissions;
        }
        mng_storage_invalidate();
    }
    const uint64_t submission = stats.submissions;
    mng_storage_drop([&](const Image &image, const ImageStorage &storage) {
        return !storage.serials && submission > image.storage_last_use &&
               submission - image.storage_last_use > 600;
    });
}
