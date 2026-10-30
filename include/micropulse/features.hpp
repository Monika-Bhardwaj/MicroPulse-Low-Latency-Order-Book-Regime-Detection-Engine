// Incremental feature engine.
//
// Per book update it maintains (never recomputing from scratch):
//   * touch order-flow imbalance (Cont-Kukanov-Stoikov 2014), rolling in event time
//   * micro-price, spread, touch / top-K depth imbalance
//   * realised volatility over a rolling event-time window (Welford add/remove)
//   * fixed event-time bars (sum r^2, sum OFI, sum OFI^2) that feed the detectors
//
// OFI "ticks" are TOUCH CHANGES (best price or best quantity on either side changed).
// Volatility uses mid returns SAMPLED ON A FIXED EVENT-TIME GRID (default 50 ms), not
// per-touch-change returns: tick-time returns on a 1-tick-wide book are dominated by
// bid/ask-bounce microstructure noise (see reports/CALIBRATION.md), and sparse sampling is
// the standard noise-robust realised-variance estimator. Quiet gaps span several grid
// points and are handled as one longer return (RV is additive, so nothing is lost).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

#include "book.hpp"
#include "types.hpp"
#include "window.hpp"

namespace mp {

struct FeatureConfig {
  Ts window_ns = 1'000'000'000;  // rolling window (event time)
  int depth_levels = 10;         // K for depth imbalance
  Ts bar_ns = 250'000'000;       // detector bar length (event time); keep a multiple of grid_ns
  Ts grid_ns = 50'000'000;       // mid-return sampling grid for realised volatility (event time)
  size_t ring_cap = 1u << 14;    // OFI ring; MUST be a power of two; bounds memory (24 B/slot). A burst
                                 // that overflows it shortens the window and sets F_RING_OVERFLOW.
  uint64_t welford_refresh = 1u << 16;  // exact recompute every N evictions (drift bound)
  double ofi_cap_mult = 8.0;     // winsorise touch qty at mult * EWMA(touch qty); 0 = off
  double ofi_ewma_alpha = 1.0 / 256.0;
};

struct Features {
  Ts ts = 0;               // exchange event time of the update that produced this row
  int64_t seq = 0;         // last update id applied
  double mid = 0, micro = 0, micro_dev_bps = 0, spread_bps = 0;
  double imb_touch = 0, imb_depth = 0;
  double ofi = 0;          // rolling sum over window (quantity units)
  double ofi_norm = 0;     // ofi / sum|e| in [-1,1]
  double rv = 0;           // sqrt(sum r^2) over window (log-return units, not annualised)
  double ret_std = 0;      // Welford std of per-touch-change returns
  int64_t n_window = 0;    // touch-change (OFI) samples in the window
  int64_t n_ret = 0;       // grid returns in the window
  uint32_t flags = 0;
  double vol_score = 0, flow_score = 0;  // latest detector scores (filled by Engine)
  uint8_t vol_alarm = 0, flow_alarm = 0;
};

struct Bar {
  Ts t_end = 0;
  double rv = 0;      // sum of squared grid-sampled mid log returns in the bar
  double ofi = 0;     // sum of OFI (quantity units)
  double ofi_sq = 0;  // sum of squared OFI contributions
  int64_t n = 0;      // touch-change samples in bar
  uint32_t flags = 0;
};

template <class Window>
class BasicFeatureEngine {
 public:
  explicit BasicFeatureEngine(const FeatureConfig& c)
      : cfg_(c),
        win_(c.window_ns, c.ring_cap, c.welford_refresh),
        ret_win_(c.window_ns, ret_ring_cap(c), c.welford_refresh) {}

  // Called when the book was invalidated/re-seeded. The return across the gap is NOT
  // observed (have_prev_=false) and the rolling window is flagged tainted until every
  // pre-gap sample has aged out (exactly window_ns of event time).
  void mark_gap(Ts ts) {
    have_prev_ = false;
    grid_init_ = false;  // never form a return across the gap
    const Ts t = std::max(ts, last_ts_);
    taint_until_ = std::max(taint_until_, t + cfg_.window_ns);
    gap_in_bar_ = true;
  }

  // Returns false if the book has no two-sided touch yet.
  bool on_update(Ts ts_in, const OrderBook& bk, int64_t seq) {
    if (!bk.has_touch()) return false;
    uint32_t fl = 0;
    Ts ts = ts_in;
    if (ts < last_ts_) {  // exchange clock regression: clamp so windows stay monotone
      fl |= F_TS_REGRESSION;
      ts = last_ts_;
    } else {
      last_ts_ = ts;
    }
    if (!bar_open_) open_bar(ts);
    win_.advance(ts);
    ret_win_.advance(ts);
    // --- grid-sampled mid return (uses the book state BEFORE this update) ---
    if (have_prev_) {
      const double pmid = 0.5 * double(pbp_ + pap_);
      if (!grid_init_) {
        grid_init_ = true;
        grid_mid_ = pmid;
        next_grid_ = (ts / cfg_.grid_ns + 1) * cfg_.grid_ns;
      } else if (ts >= next_grid_) {
        const double r = pmid == grid_mid_ ? 0.0 : std::log1p((pmid - grid_mid_) / grid_mid_);
        ret_win_.push(next_grid_, r, 0);
        bar_r2_ += r * r;
        grid_mid_ = pmid;
        next_grid_ = (ts / cfg_.grid_ns + 1) * cfg_.grid_ns;
      }
    }
    if (ts >= bar_end_) finalize_bar(ts);
    if (!started_) { started_ = true; start_ts_ = ts; }

    const Level bb = bk.best_bid(), ba = bk.best_ask();
    if (!have_prev_) {
      set_prev(bb, ba);
      have_prev_ = true;
      if (ewma_q_ <= 0.0) ewma_q_ = 0.5 * double(bb.qty + ba.qty);
    } else if (bb.px != pbp_ || bb.qty != pbq_ || ba.px != pap_ || ba.qty != paq_) {
      Qty bq = bb.qty, aq = ba.qty, pbq = pbq_, paq = paq_;
      if (cfg_.ofi_cap_mult > 0.0) {  // winsorise: a single huge (spoofed) order cannot dominate OFI
        const Qty cap = static_cast<Qty>(cfg_.ofi_cap_mult * ewma_q_);
        bq = std::min(bq, cap); aq = std::min(aq, cap);
        pbq = std::min(pbq, cap); paq = std::min(paq, cap);
        ewma_q_ += cfg_.ofi_ewma_alpha * (0.5 * double(bq + aq) - ewma_q_);
      }
      // e_n = 1{Pb>=Pb'} qb - 1{Pb<=Pb'} qb' - 1{Pa<=Pa'} qa + 1{Pa>=Pa'} qa'
      const int64_t e = (bb.px >= pbp_ ? bq : 0) - (bb.px <= pbp_ ? pbq : 0) -
                        (ba.px <= pap_ ? aq : 0) + (ba.px >= pap_ ? paq : 0);
      win_.push(ts, 0.0, e);
      const double ed = double(e) / kFixedScale;
      bar_e_ += ed;
      bar_e2_ += ed * ed;
      ++bar_n_;
      set_prev(bb, ba);
    }

    // ---- emit row ----
    const double mid_fx = 0.5 * double(bb.px + ba.px);
    const double bq = double(bb.qty), aq = double(ba.qty);
    cur_.ts = ts_in;
    cur_.seq = seq;
    cur_.mid = mid_fx / kFixedScale;
    cur_.micro = (bq + aq > 0.0) ? (double(bb.px) * aq + double(ba.px) * bq) / (bq + aq) / kFixedScale
                                 : cur_.mid;
    cur_.micro_dev_bps = (cur_.micro - cur_.mid) / cur_.mid * 1e4;
    cur_.spread_bps = double(ba.px - bb.px) / mid_fx * 1e4;
    cur_.imb_touch = (bq + aq > 0.0) ? (bq - aq) / (bq + aq) : 0.0;
    const size_t K = static_cast<size_t>(cfg_.depth_levels);
    const double db = double(bk.bids().depth_qty(K)), da = double(bk.asks().depth_qty(K));
    cur_.imb_depth = (db + da > 0.0) ? (db - da) / (db + da) : 0.0;
    const WStats w = win_.stats();
    const WStats wr = ret_win_.stats();
    cur_.ofi = double(w.ofi) / kFixedScale;
    cur_.ofi_norm = w.abs_e > 0 ? double(w.ofi) / double(w.abs_e) : 0.0;
    cur_.rv = wr.rv;
    cur_.ret_std = wr.ret_std;
    cur_.n_window = w.n;
    cur_.n_ret = wr.n;
    if (ts - start_ts_ < cfg_.window_ns) fl |= F_WARMUP;
    if (ts < taint_until_) fl |= F_WINDOW_TAINTED | F_GAP_RECENT;
    if (win_.take_overflow() | ret_win_.take_overflow()) fl |= F_RING_OVERFLOW;
    if (bk.truncated() && (bk.bids().size() < K || bk.asks().size() < K)) fl |= F_TRUNCATED;
    cur_.flags = fl;
    last_mid_ = cur_.mid;
    return true;
  }

  bool take_bar(Bar& b) {
    if (!has_pending_) return false;
    b = pending_;
    has_pending_ = false;
    return true;
  }
  const Features& current() const { return cur_; }
  double last_mid() const { return last_mid_; }
  const FeatureConfig& config() const { return cfg_; }
  Window& window() { return win_; }
  const Window& window() const { return win_; }
  const Window& ret_window() const { return ret_win_; }

 private:
  void set_prev(const Level& bb, const Level& ba) {
    pbp_ = bb.px; pbq_ = bb.qty; pap_ = ba.px; paq_ = ba.qty;
  }
  void open_bar(Ts ts) {
    bar_start_ = (ts / cfg_.bar_ns) * cfg_.bar_ns;
    bar_end_ = bar_start_ + cfg_.bar_ns;
    bar_r2_ = bar_e_ = bar_e2_ = 0.0;
    bar_n_ = 0;
    gap_in_bar_ = false;
    bar_open_ = true;
  }
  void finalize_bar(Ts ts) {
    pending_.t_end = bar_end_;
    pending_.rv = bar_r2_;
    pending_.ofi = bar_e_;
    pending_.ofi_sq = bar_e2_;
    pending_.n = bar_n_;
    pending_.flags = (gap_in_bar_ ? F_GAP_RECENT : 0u) | (ts - bar_end_ >= cfg_.bar_ns ? F_BAR_SKIP : 0u);
    has_pending_ = true;
    open_bar(ts);
  }

  static size_t ret_ring_cap(const FeatureConfig& c) {  // grid returns per window, rounded up to 2^k
    size_t need = static_cast<size_t>(c.window_ns / std::max<Ts>(1, c.grid_ns)) + 8, cap = 64;
    while (cap < need) cap <<= 1;
    return cap;
  }

  FeatureConfig cfg_;
  Window win_;       // touch-change OFI samples
  Window ret_win_;   // grid-sampled mid returns
  Features cur_;
  Bar pending_;
  bool has_pending_ = false;
  bool started_ = false, have_prev_ = false, bar_open_ = false, gap_in_bar_ = false;
  Ts start_ts_ = 0, last_ts_ = 0, taint_until_ = 0, bar_start_ = 0, bar_end_ = 0;
  bool grid_init_ = false;
  Ts next_grid_ = 0;
  double grid_mid_ = 0.0;
  Price pbp_ = 0, pap_ = 0;
  Qty pbq_ = 0, paq_ = 0;
  double ewma_q_ = 0.0;
  double bar_r2_ = 0, bar_e_ = 0, bar_e2_ = 0;
  int64_t bar_n_ = 0;
  double last_mid_ = 0.0;
};

using FeatureEngine = BasicFeatureEngine<RollingWindow>;
using NaiveFeatureEngine = BasicFeatureEngine<NaiveWindow>;

}  // namespace mp
