// Clocks.
//
// CLOCK-DOMAIN POLICY (see README "Clock skew"):
//   * EVENT time  = exchange timestamp carried in the message. Used for rolling windows,
//                   bars and everything statistical. Never mixed with local clocks.
//   * LATENCY     = local monotonic clock (rdtscp cycle counter on x86-64, steady_clock
//                   otherwise). Never compared against exchange timestamps.
//   The gap between the two (exchange->local delay + clock offset) is NOT measurable
//   without a trusted reference; it is exported as `recv - event` for diagnostics only.
#pragma once
#include <chrono>
#include <cstdint>
#if defined(__x86_64__)
#include <x86intrin.h>
#endif

namespace mp {

inline int64_t mono_ns() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

#if defined(__x86_64__)
inline uint64_t cycles() noexcept {
  unsigned aux;
  return __rdtscp(&aux);
}
inline constexpr bool kHaveCycleCounter = true;
#else
inline uint64_t cycles() noexcept { return static_cast<uint64_t>(mono_ns()); }
inline constexpr bool kHaveCycleCounter = false;
#endif

// ns per cycle, calibrated once against steady_clock (50 ms).
inline double ns_per_cycle() {
  static const double v = [] {
    if (!kHaveCycleCounter) return 1.0;
    const int64_t t0 = mono_ns();
    const uint64_t c0 = cycles();
    while (mono_ns() - t0 < 50'000'000) {
    }
    const int64_t t1 = mono_ns();
    const uint64_t c1 = cycles();
    return double(t1 - t0) / double(c1 - c0);
  }();
  return v;
}

}  // namespace mp
