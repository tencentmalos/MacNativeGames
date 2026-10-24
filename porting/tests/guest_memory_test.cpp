// SPDX-License-Identifier: MIT
#include "guest_memory.h"
#include <cstdio>
#include <stdexcept>
using namespace mng::port;
void need(bool value) {
    if (!value)
        throw std::runtime_error("guest memory assertion failed");
}
int main() {
    constexpr uint64_t mib = 1ull << 20;
    GuestMemory m({2 * mib, 8 * mib, 16 * mib, 8 * mib, 2 * mib});
    need(m.base() + m.size() <= 1ull << 39);
    need(*m.info(12) == m.base() && *m.info(13) == m.size());
    need(*m.info(4) == m.heap() && *m.info(5) == 8 * mib && *m.info(7) == 2 * mib);
    need(m.query(m.base()).state == 0 && !m.info(999));
    need(m.query(0).state == 0x10 && m.query(1ull << 39).state == 0x10);
    need(m.set_heap_size(2 * mib) == 0 && m.heap_size() == 2 * mib);
    auto *heap = reinterpret_cast<uint8_t *>(m.heap());
    heap[0] = 0x55;
    heap[2 * mib - 1] = 0x66;
    need(m.accessible(m.heap(), 2 * mib, 3) && !m.accessible(m.heap(), 2 * mib + 1, 3));
    need(m.set_heap_size(4096) != 0 && m.heap_size() == 2 * mib && heap[0] == 0x55);
    need(m.set_heap_size(0) == 0 && m.query(m.heap()).state == 0);
    need(m.set_heap_size(2 * mib) == 0 && heap[0] == 0 && heap[2 * mib - 1] == 0);
    const auto base = m.alias();
    auto *alias = reinterpret_cast<uint8_t *>(base);
    need(m.map_physical(base, 8192) == 0);
    alias[0] = 0x11;
    alias[4096] = 0x22;
    need(m.map_physical(base + 4096, 8192) == 0 && m.alias_bytes() == 12288);
    need(alias[0] == 0x11 && alias[4096] == 0x22 && alias[8192] == 0);
    alias[8192] = 0x33;
    need(m.map_physical(base + 4096, 4096, true) == 0 && m.alias_bytes() == 8192);
    need(m.query(base + 4096).state == 0 && !m.accessible(base + 4096, 1, 1));
    need(alias[0] == 0x11 && alias[8192] == 0x33);
    need(m.map_physical(base + 4096, 4096) == 0 && alias[4096] == 0);
    const auto used = *m.info(7);
    need(m.map_physical(base, 8 * mib) != 0 && *m.info(7) == used && alias[0] == 0x11);
    need(m.map_physical(UINT64_MAX - 4095, 8192) != 0 && *m.info(7) == used);
    need(m.map_physical(m.heap(), 4096) != 0 && m.map_physical(base + 1, 4096) != 0);
    need(m.map_physical(base, 16384, true) == 0 && m.alias_bytes() == 0);
    need(m.map_physical(base, 16384) == 0 && alias[0] == 0 && alias[8192] == 0);
    need(m.set_heap_size(6 * mib) != 0); // Shared heap/alias budget, not independent limits.
    heap[4096] = 0x73;
    heap[8192] = 0x99;
    need(m.map_stack(m.stack(), m.heap() + 4096, 4096) == 0);
    auto *stack = reinterpret_cast<uint8_t *>(m.stack());
    need(stack[0] == 0x73);
    need(m.query(m.heap() + 4096).attribute == 1 && !m.accessible(m.heap() + 4096, 4096, 1));
    need(m.set_heap_size(0) != 0 && heap[8192] == 0x99 && *m.info(7) == 2 * mib + 2 * mib + 16384);
    need(m.map_stack(m.stack() + 4096, m.heap() + 4096, 4096) != 0);
    need(m.map_stack(m.stack(), m.heap() + 8192, 4096, true) != 0);
    stack[0] = 0x42;
    need(m.map_stack(m.stack(), m.heap() + 4096, 4096, true) == 0);
    need(heap[4096] == 0x42 && heap[8192] == 0x99 && m.query(m.stack()).state == 0);
    need(m.set_permission(m.heap() + 4096, 4096, 1) == 0 &&
         m.query(m.heap() + 4096).permission == 1);
    need(m.set_permission(m.heap() + 4096, 4096, 7) != 0);
    need(m.set_permission(m.heap() + 4096, 4096, 3) == 0);
    heap[4096] = 0x64;
    puts("PASS: reservations, heap/alias accounting, zero fill, stack copy-back, locked-source "
         "rejection, permissions and bounds");
}
