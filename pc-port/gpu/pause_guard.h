// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Global pause guard. While the game is paused (eboot counter at +0x1D1F7FC: sub_27DB10
// increments it, sub_27E210 decrements it; it is also 1 in the start menus) and for
// Three seconds after it, every native path is off and the command processor
// draws everything as the original engine does. Native work executed in the pause hung the GPU
// (TDR), see out/experimental/20261017-native-passes-v2/CORRECAO-HANG-PAUSE.md.
// The guard must stay on: native work in the pause hangs the GPU.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include "gpu_blackbox.h"

namespace iss::pause_guard {

inline constexpr std::uint64_t CounterOffset = 0x1D1F7FC;
inline const volatile std::uint32_t* counter{};
inline std::int64_t hold_ms = 3000;
inline std::atomic<std::int64_t> last_paused_ms{};
inline std::atomic<bool> active{};
inline std::atomic<std::uint64_t> engaged{};

inline std::int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

inline void Install(std::uint64_t image_base) {
    counter = reinterpret_cast<const volatile std::uint32_t*>(image_base + CounterOffset);
    std::fprintf(stderr, "PAUSE_GUARD on: counter eboot+0x%llx = %u, hold %lld ms\n",
                 (unsigned long long)CounterOffset, *counter, (long long)hold_ms);
}

// True while every native path must stay off. One volatile load when the game is running.
inline bool Blocked() {
    if (!counter) return false;
    if (*counter == 0 && !active.load(std::memory_order_relaxed)) return false;
    const std::int64_t now = NowMs();
    if (*counter != 0) {
        last_paused_ms.store(now, std::memory_order_relaxed);
        if (!active.exchange(true, std::memory_order_relaxed)) {
            engaged.fetch_add(1, std::memory_order_relaxed);
            std::fprintf(stderr, "PAUSE_GUARD engaged: native paths off\n");
            iss::blackbox::Event(iss::blackbox::General, "PAUSE: entrou (nativo desligado)");
        }
        return true;
    }
    if (now - last_paused_ms.load(std::memory_order_relaxed) < hold_ms) return true;
    if (active.exchange(false, std::memory_order_relaxed)) {
        std::fprintf(stderr, "PAUSE_GUARD released: native paths on\n");
        iss::blackbox::Event(iss::blackbox::General, "PAUSE: saiu ha 3 s (nativo religado)");
    }
    return false;
}

} // namespace iss::pause_guard
