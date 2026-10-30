// Rolling-window statistics over the last `window_ns` of EVENT time.
//
// Two interchangeable implementations of the same interface:
//   RollingWindow : incremental. O(1) amortised per sample. Welford add/remove for the
//                   return statistics, exact int64 running sums for order-flow imbalance.
//   NaiveWindow   : recompute-from-scratch baseline (two-pass variance over the window).
//                   Used (a) as the correctness oracle in tests and (b) as the
//                   denominator of the reported speedup.
#pragma once
#include <cmath>
#include <cstdint>
#include <deque>
#include <limits>
#include <vector>

#include "types.hpp"

namespace mp {

// ---------------------------------------------------------------------------
// Welford's algorithm. add(): classic. remove(): exact algebraic inverse, needed for a
// sliding window:   M2' = M2 - (x - mean_old) * (x - mean_new).
// The naive formulation  var = E[x^2] - E[x]^2  loses ALL significant digits when
// |mean| >> std (e.g. log-prices, or prices ~6e12 in fixed point): see test_welford.
// ---------------------------------------------------------------------------
struct Welford {
  int64_t n = 0;
  double mean = 0.0;
  double m2 = 0.0;

  void reset() { n = 0; mean = 0.0; m2 = 0.0; }
  void add(double x) {
    ++n;
    const double d = x - mean;
    mean += d / double(n);
    m2 += d * (x - mean);
  }
  void remove(double x) {
    if (n <= 1) { reset(); return; }
    const double d = x - mean;
    --n;
    mean -= d / double(n);
    m2 -= d * (x - mean);
    if (m2 < 0.0) m2 = 0.0;  // cancellation guard; periodic exact refresh bounds drift
  }
  double var() const { return n > 1 ? m2 / double(n - 1) : std::numeric_limits<double>::quiet_NaN(); }
};

struct Sample {
  Ts ts;      // event time of the touch change
  double r;   // log mid return caused by this touch change (0 if only quantity changed)
  int64_t e;  // Cont-Kukanov-Stoikov order-flow-imbalance contribution (fixed-point qty)
};

struct WStats {
  int64_t n = 0;       // samples in window
  int64_t ofi = 0;     // sum of e over window (exact integer)
  int64_t abs_e = 0;   // sum of |e| over window (exact integer)
  double rv = 0.0;     // realised volatility: sqrt(sum r^2)  (= sqrt(M2 + n*mean^2))
  double ret_std = 0.0;
};

class RollingWindow {
 public:
  RollingWindow(Ts window_ns, size_t ring_cap_pow2, uint64_t refresh_every)
      : window_ns_(window_ns), buf_(ring_cap_pow2), mask_(ring_cap_pow2 - 1), refresh_(refresh_every) {}

  void advance(Ts now) {
    const Ts cut = now - window_ns_;
    while (count_ && buf_[head_].ts < cut) pop();
  }

  void push(Ts ts, double r, int64_t e) {
    advance(ts);
    if (count_ == buf_.size()) {  // bounded memory: shorten the window instead of growing
      pop();
      overflow_ = true;
    }
    buf_[(head_ + count_) & mask_] = Sample{ts, r, e};
    ++count_;
    w_.add(r);
    ofi_ += e;
    abs_ += e < 0 ? -e : e;
  }

  WStats stats() const {
    WStats s;
    s.n = static_cast<int64_t>(count_);
    s.ofi = ofi_;
    s.abs_e = abs_;
    const double ss = w_.m2 + double(w_.n) * w_.mean * w_.mean;
    s.rv = ss > 0.0 ? std::sqrt(ss) : 0.0;
    s.ret_std = w_.n > 1 ? std::sqrt(w_.m2 / double(w_.n - 1)) : 0.0;
    return s;
  }

  void clear() {
    head_ = count_ = 0;
    w_.reset();
    ofi_ = abs_ = 0;
  }
  bool take_overflow() { const bool o = overflow_; overflow_ = false; return o; }
  size_t size() const { return count_; }
  size_t memory_bytes() const { return buf_.capacity() * sizeof(Sample); }
  // test hook: exact recomputation from the ring (what the periodic refresh does)
  Welford exact_welford() const {
    Welford w;
    for (size_t i = 0; i < count_; ++i) w.add(buf_[(head_ + i) & mask_].r);
    return w;
  }
  const Welford& welford() const { return w_; }

 private:
  void pop() {
    const Sample& s = buf_[head_];
    w_.remove(s.r);
    ofi_ -= s.e;
    abs_ -= s.e < 0 ? -s.e : s.e;
    head_ = (head_ + 1) & mask_;
    --count_;
    if (refresh_ && ++removals_ % refresh_ == 0) {  // bound accumulated floating-point drift
      w_ = exact_welford();
    }
  }

  Ts window_ns_;
  std::vector<Sample> buf_;
  size_t mask_;
  uint64_t refresh_;
  size_t head_ = 0, count_ = 0;
  uint64_t removals_ = 0;
  Welford w_;
  int64_t ofi_ = 0, abs_ = 0;
  bool overflow_ = false;
};

class NaiveWindow {
 public:
  NaiveWindow(Ts window_ns, size_t, uint64_t) : window_ns_(window_ns) {}

  void advance(Ts now) {
    const Ts cut = now - window_ns_;
    while (!d_.empty() && d_.front().ts < cut) {
      d_.pop_front();
      dirty_ = true;
    }
  }
  void push(Ts ts, double r, int64_t e) {
    advance(ts);
    d_.push_back(Sample{ts, r, e});
    dirty_ = true;
  }
  WStats stats() const {
    if (dirty_) recompute();
    return cached_;
  }
  void clear() { d_.clear(); dirty_ = true; }
  bool take_overflow() { return false; }
  size_t size() const { return d_.size(); }

 private:
  void recompute() const {  // O(n) every time the window changed: the baseline
    WStats s;
    s.n = static_cast<int64_t>(d_.size());
    double sum = 0.0, ss = 0.0;
    for (const Sample& x : d_) {
      sum += x.r;
      ss += x.r * x.r;
      s.ofi += x.e;
      s.abs_e += x.e < 0 ? -x.e : x.e;
    }
    s.rv = std::sqrt(ss);
    if (s.n > 1) {  // two-pass variance (the numerically safe naive formulation)
      const double mean = sum / double(s.n);
      double m2 = 0.0;
      for (const Sample& x : d_) m2 += (x.r - mean) * (x.r - mean);
      s.ret_std = std::sqrt(m2 / double(s.n - 1));
    }
    cached_ = s;
    dirty_ = false;
  }
  Ts window_ns_;
  std::deque<Sample> d_;
  mutable WStats cached_;
  mutable bool dirty_ = true;
};

}  // namespace mp
