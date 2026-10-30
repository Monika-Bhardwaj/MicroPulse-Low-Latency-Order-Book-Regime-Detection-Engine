// MicroPulse core types.
//
// Prices and quantities are int64 fixed-point with 1e-8 resolution (the finest
// precision Binance publishes). All book arithmetic is integer; conversion to
// double happens only when features are emitted.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace mp {

using Price = int64_t;  // fixed-point, units of 1e-8
using Qty = int64_t;    // fixed-point, units of 1e-8
using Ts = int64_t;     // nanoseconds

inline constexpr double kFixedScale = 1e8;

enum class Side : uint8_t { Bid = 0, Ask = 1 };

// One absolute price-level update: "level at `px` now holds `qty`" (qty==0 deletes).
struct LevelUpdate {
  Price px;
  Qty qty;
  Side side;
};

struct Level {
  Price px;
  Qty qty;
};

// Venue-neutral parsed incremental message. `lv` is owned by whoever produced the
// view (parser scratch buffer, synthetic timeline arena) and is only valid until
// the producer is next invoked.
struct DeltaView {
  Ts event_ts_ns = 0;        // EXCHANGE event time -> used for all windowing/bars
  int64_t first_id = 0;      // first update id covered by this message
  int64_t last_id = 0;       // last update id covered by this message
  int64_t prev_last_id = -1; // venue-provided previous last id (futures "pu"), -1 if absent
  const LevelUpdate* lv = nullptr;
  uint32_t n = 0;
};

struct SnapshotView {
  int64_t last_id = 0;
  Ts event_ts_ns = 0;
  const Level* bids = nullptr;
  size_t n_bids = 0;
  const Level* asks = nullptr;
  size_t n_asks = 0;
};

struct SnapshotOwned {
  int64_t last_id = 0;
  Ts event_ts_ns = 0;
  std::vector<Level> bids, asks;
  SnapshotView view() const {
    return SnapshotView{last_id, event_ts_ns, bids.data(), bids.size(), asks.data(), asks.size()};
  }
};

// Data-quality flags attached to every emitted feature row / bar. The engine never
// silently computes statistics over a corrupted book: affected windows are flagged.
enum Flag : uint32_t {
  F_WARMUP = 1u << 0,         // rolling window not yet full since start
  F_GAP_RECENT = 1u << 1,     // sequence gap / resync occurred (bar-level: inside this bar)
  F_WINDOW_TAINTED = 1u << 2, // rolling window still contains pre-gap data horizon
  F_BOOK_STALE = 1u << 3,     // book invalid, awaiting resync; features are last-good values
  F_CROSSED = 1u << 4,        // (reserved) crossed book observed
  F_TRUNCATED = 1u << 5,      // truncated book has fewer valid levels than feature depth
  F_RING_OVERFLOW = 1u << 6,  // rolling ring overflowed (window effectively shortened)
  F_TS_REGRESSION = 1u << 7,  // exchange timestamp went backwards (clamped)
  F_BAR_SKIP = 1u << 8,       // >=1 whole bar elapsed with no events before this bar
};

}  // namespace mp
