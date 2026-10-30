// Synthetic L2 market-data generator (NSE-microstructure-style fallback + test fixture).
//
// Model (deliberately simple, fully reproducible from a seed):
//   * A latent fair value f_t follows Brownian motion with piecewise-constant volatility
//     sigma (ticks/sqrt(s)); fair-value updates arrive as a Poisson process and "requote"
//     the touch (quotes are pulled/lifted and refilled at floor(f), floor(f)+1).
//   * Book churn (add / cancel / marketable trade) arrives as a Poisson process whose rate
//     depends on the regime; add/cancel depths are geometric near the touch with a
//     heavy-tailed far component, so the full-depth book holds thousands of levels.
//   * Regime change points are known exactly (labels) -> detector calibration.
//   * Injectable manipulation: spoofing (large resting order, cancelled before any fill),
//     quote stuffing (bursts of add/cancel at deep prices or at the touch).
//   * Feed impairments (drop / duplicate / adjacent swap) are applied to the DELIVERY
//     order only; the truth timeline stays intact so exact snapshots can be served for
//     resync.
// This is NOT real market data: it validates correctness, performance and detector
// mechanics, and cannot substitute for calibration on a recorded Binance capture.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include "book.hpp"
#include "types.hpp"

namespace mp {

class Rng {
 public:
  explicit Rng(uint64_t seed = 1) {
    uint64_t x = seed + 0x9E3779B97F4A7C15ULL;
    for (auto& v : s_) {
      x += 0x9E3779B97F4A7C15ULL;
      uint64_t z = x;
      z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
      z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
      v = z ^ (z >> 31);
    }
  }
  uint64_t next() {
    const uint64_t r = rotl(s_[1] * 5, 7) * 9;
    const uint64_t t = s_[1] << 17;
    s_[2] ^= s_[0]; s_[3] ^= s_[1]; s_[1] ^= s_[2]; s_[0] ^= s_[3];
    s_[2] ^= t;
    s_[3] = rotl(s_[3], 45);
    return r;
  }
  double uni() { return double(next() >> 11) * (1.0 / 9007199254740992.0); }
  double normal() {
    if (have_) { have_ = false; return spare_; }
    double u, v, q;
    do { u = 2 * uni() - 1; v = 2 * uni() - 1; q = u * u + v * v; } while (q >= 1.0 || q == 0.0);
    const double f = std::sqrt(-2.0 * std::log(q) / q);
    spare_ = v * f; have_ = true;
    return u * f;
  }
  double expo(double rate) { return -std::log(1.0 - uni()) / rate; }
  int geometric(double mean) {  // failures before first success, E = mean
    const double p = 1.0 / (1.0 + mean);
    return static_cast<int>(std::floor(std::log(1.0 - uni()) / std::log(1.0 - p)));
  }
  int poisson(double lam) {
    if (lam <= 0) return 0;
    const double L = std::exp(-lam);
    int k = 0;
    double p = 1.0;
    do { ++k; p *= uni(); } while (p > L);
    return k - 1;
  }

 private:
  static uint64_t rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }
  uint64_t s_[4];
  double spare_ = 0;
  bool have_ = false;
};

struct Regime {
  double t_start_s = 0;
  double sigma = 1.0;      // fair-value volatility, ticks / sqrt(second)
  double rate_mult = 1.0;  // multiplies the book-churn message rate
};
struct SpoofCfg {
  double per_min = 0;      // episodes per minute
  int depth_ticks = 0;     // 0 = add to the touch level, k = k ticks behind the touch
  double qty_mult = 50.0;  // spoof size as a multiple of the 0.4-unit median order
  double hold_s = 1.0;     // time before cancellation (never filled)
  int layers = 1;
};
struct StuffCfg {
  double bursts_per_min = 0;
  double len_s = 0.5;
  double rate = 100000.0;  // messages / second during the burst
  int mode = 0;            // 0 = deep (adversarial for the data structure), 1 = touch (flicker qty)
};
struct Impair {
  double p_drop = 0, p_dup = 0, p_swap = 0;
};

struct SynthConfig {
  uint64_t seed = 1;
  double duration_s = 600;
  double base_rate = 1000;        // book-churn messages / s at rate_mult = 1
  double fv_rate = 40;            // fair-value updates / s
  double mid0 = 60000.0;
  Price tick = 1'000'000;         // 0.01
  double levels_per_msg = 2.0;    // mean events merged into one message
  double far_tail_prob = 0.10;
  int far_max_ticks = 20000;
  double near_mean_ticks = 5.0;
  int init_near = 300, init_far = 3000;
  std::vector<Regime> regimes;    // empty => single regime {0,1.0,1.0}
  double sigma_mod_amp = 0.0, sigma_mod_period_s = 600.0;  // slow vol drift (hostile null)
  double act_burst_per_min = 0, act_burst_mult = 3.0, act_burst_len_s = 5.0;  // activity bursts, no vol change
  SpoofCfg spoof;
  StuffCfg stuff;
};

struct MsgHdr {
  Ts ts;
  int64_t first_id, last_id;
  uint32_t off, n;
  uint8_t tag;  // 0 normal, 1 stuffing, 2 spoof
};

class SynthFeed {
 public:
  static constexpr Ts kStartNs = 1'700'000'000'000'000'000LL;

  explicit SynthFeed(const SynthConfig& c) : cfg_(c), rng_(c.seed) {
    regimes_ = c.regimes.empty() ? std::vector<Regime>{Regime{}} : c.regimes;
    std::sort(regimes_.begin(), regimes_.end(), [](const Regime& a, const Regime& b) { return a.t_start_s < b.t_start_s; });
    for (size_t i = 1; i < regimes_.size(); ++i)
      if (regimes_[i].sigma != regimes_[i - 1].sigma || regimes_[i].rate_mult != regimes_[i - 1].rate_mult)
        change_ts_.push_back(ns(regimes_[i].t_start_s));
    tick_real_ = double(c.tick) / kFixedScale;
    f_ = c.mid0 / tick_real_;
    init_book();
    // schedules
    schedule_windows(c.act_burst_per_min, c.act_burst_len_s, bursts_);
    schedule_windows(c.stuff.bursts_per_min, c.stuff.len_s, stuffw_);
    for (auto& w : stuffw_) stuff_windows_.push_back({ns(w.first), ns(w.second)});
    if (c.spoof.per_min > 0) {
      double t = rng_.expo(c.spoof.per_min / 60.0);
      while (t < c.duration_s - c.spoof.hold_s - 0.1) {
        spoof_.emplace(t, SpoofEv{0, 0, 0, 0});
        t += std::max(rng_.expo(c.spoof.per_min / 60.0), c.spoof.hold_s + 0.05);
      }
    }
    next_bg_ = rng_.expo(rate_at(0));
    next_fv_ = rng_.expo(c.fv_rate);
  }

  // Produce the next message. View valid until the next call. Returns false at end.
  bool next(DeltaView& out, uint8_t* tag = nullptr) {
    for (;;) {
      const double INF = std::numeric_limits<double>::infinity();
      double ts = INF;
      if (stuff_active_) ts = next_stuff_;
      else if (sw_i_ < stuffw_.size()) ts = stuffw_[sw_i_].first;
      const double tsp = spoof_.empty() ? INF : spoof_.begin()->first;
      const double tn = std::min(std::min(next_bg_, next_fv_), std::min(ts, tsp));
      if (tn >= cfg_.duration_s) return false;
      uint8_t tg = 0;
      upd_.clear();
      if (tn == tsp) {
        do_spoof(tn);
        tg = 2;
      } else if (tn == ts) {
        if (!stuff_active_) { stuff_active_ = true; next_stuff_ = tn; }
        if (next_stuff_ >= stuffw_[sw_i_].second) { stuff_active_ = false; ++sw_i_; continue; }
        do_stuff();
        next_stuff_ = tn + rng_.expo(cfg_.stuff.rate);
        tg = 1;
      } else if (tn == next_fv_) {
        const double dt = tn - last_fv_;
        last_fv_ = tn;
        f_ += sigma_at(tn) * std::sqrt(dt) * rng_.normal();
        requote();
        next_fv_ = tn + rng_.expo(cfg_.fv_rate);
      } else {
        const int m = 1 + rng_.poisson(cfg_.levels_per_msg - 1.0);
        for (int i = 0; i < m; ++i) churn_event();
        maintain_touch();
        next_bg_ = tn + rng_.expo(rate_at(tn));
      }
      if (finish(tn, out)) {
        if (tag) *tag = tg;
        return true;
      }
    }
  }

  void export_levels(std::vector<Level>& b, std::vector<Level>& a) const {
    b.clear(); a.clear();
    for (const auto& kv : bids_) b.push_back(Level{kv.first * cfg_.tick, kv.second});
    for (const auto& kv : asks_) a.push_back(Level{kv.first * cfg_.tick, kv.second});
  }
  SnapshotOwned initial_snapshot() const {
    SnapshotOwned s;
    s.last_id = 0;
    s.event_ts_ns = kStartNs;
    export_levels(s.bids, s.asks);
    return s;
  }
  const std::vector<Ts>& change_ts() const { return change_ts_; }
  const std::vector<std::pair<Ts, Ts>>& spoof_windows() const { return spoof_windows_; }
  const std::vector<std::pair<Ts, Ts>>& stuff_windows() const { return stuff_windows_; }
  size_t bid_levels() const { return bids_.size(); }
  size_t ask_levels() const { return asks_.size(); }

 private:
  using BidMap = std::map<int64_t, int64_t, std::greater<int64_t>>;
  using AskMap = std::map<int64_t, int64_t>;
  struct SpoofEv { int kind; int side; int64_t ticks; int64_t qty; };

  static Ts ns(double s) { return kStartNs + static_cast<Ts>(s * 1e9); }
  static constexpr int64_t kQtyUnit = 100'000;  // 0.001

  int64_t rand_qty() { return kQtyUnit * std::max<int64_t>(1, static_cast<int64_t>(std::exp(std::log(400.0) + rng_.normal()))); }
  int64_t far_offset() {
    const double u = rng_.uni() + 1e-12;
    return std::min<int64_t>(cfg_.far_max_ticks, 6 + static_cast<int64_t>(6.0 * std::pow(u, -1.0 / 1.1)));
  }

  // Depth (ticks from the touch) at which an order is placed OR cancelled. Using the SAME
  // sampler for adds and cancels makes the per-offset occupancy stationary
  // (p = add/(add + 0.6*cancel) ~ 0.65) instead of decaying: the book neither empties nor grows.
  int64_t sample_offset() {
    const double u = rng_.uni();
    if (u < cfg_.far_tail_prob) return far_offset();
    if (u < cfg_.far_tail_prob + 0.2) return static_cast<int64_t>(rng_.uni() * 200.0);
    return rng_.geometric(cfg_.near_mean_ticks);
  }

  const Regime& regime(double t) {
    while (ri_ + 1 < regimes_.size() && regimes_[ri_ + 1].t_start_s <= t) ++ri_;
    return regimes_[ri_];
  }
  double sigma_at(double t) {
    double s = regime(t).sigma;
    if (cfg_.sigma_mod_amp > 0) s *= 1.0 + cfg_.sigma_mod_amp * std::sin(2.0 * M_PI * t / cfg_.sigma_mod_period_s);
    return s;
  }
  double rate_at(double t) {
    double r = cfg_.base_rate * regime(t).rate_mult;
    while (bi_ < bursts_.size() && bursts_[bi_].second < t) ++bi_;
    if (bi_ < bursts_.size() && bursts_[bi_].first <= t) r *= cfg_.act_burst_mult;
    return r;
  }
  void schedule_windows(double per_min, double len, std::vector<std::pair<double, double>>& out) {
    if (per_min <= 0) return;
    double t = rng_.expo(per_min / 60.0);
    while (t + len < cfg_.duration_s) {
      out.push_back({t, t + len});
      t += len + rng_.expo(per_min / 60.0);
    }
  }

  void init_book() {
    const int64_t tb = static_cast<int64_t>(std::floor(f_));
    for (int k = 0; k < cfg_.init_near; ++k) { bids_[tb - k] = rand_qty(); asks_[tb + 1 + k] = rand_qty(); }
    for (int k = 0; k < cfg_.init_far; ++k) {
      const int64_t off = cfg_.init_near + static_cast<int64_t>(rng_.uni() * (cfg_.far_max_ticks - cfg_.init_near));
      bids_[tb - off] = rand_qty();
      asks_[tb + 1 + off] = rand_qty();
    }
  }

  void set_bid(int64_t t, int64_t q) {
    if (q <= 0) { bids_.erase(t); q = 0; } else bids_[t] = q;
    upd_.push_back(LevelUpdate{t * cfg_.tick, q, Side::Bid});
  }
  void set_ask(int64_t t, int64_t q) {
    if (q <= 0) { asks_.erase(t); q = 0; } else asks_[t] = q;
    upd_.push_back(LevelUpdate{t * cfg_.tick, q, Side::Ask});
  }
  int64_t get(const BidMap& m, int64_t t) const { auto it = m.find(t); return it == m.end() ? 0 : it->second; }
  int64_t get(const AskMap& m, int64_t t) const { auto it = m.find(t); return it == m.end() ? 0 : it->second; }

  void requote() {
    const int64_t tb = static_cast<int64_t>(std::floor(f_)), ta = tb + 1;
    while (!bids_.empty() && bids_.begin()->first > tb) set_bid(bids_.begin()->first, 0);
    while (!asks_.empty() && asks_.begin()->first < ta) set_ask(asks_.begin()->first, 0);
    if (bids_.empty() || bids_.begin()->first < tb) set_bid(tb, rand_qty());
    if (asks_.empty() || asks_.begin()->first > ta) set_ask(ta, rand_qty());
  }

  // Liquidity providers re-quote a depleted touch quickly: keeps the spread at one tick most of
  // the time (as on BTCUSDT) instead of letting sweeps leave a multi-tick hole.
  void maintain_touch() {
    const int64_t tb = static_cast<int64_t>(std::floor(f_)), ta = tb + 1;
    if (bids_.begin()->first < tb && rng_.uni() < 0.85) set_bid(tb, rand_qty());
    if (asks_.begin()->first > ta && rng_.uni() < 0.85) set_ask(ta, rand_qty());
  }

  void churn_event() {
    // Depth is anchored to the FAIR VALUE (not to the current best), so liquidity refills
    // around the true touch even after a sweep. Adds and cancels share one offset sampler.
    const bool bid = rng_.uni() < 0.5;
    const int64_t anchor_b = static_cast<int64_t>(std::floor(f_)), anchor_a = anchor_b + 1;
    const double u = rng_.uni();
    if (u < 0.47) {
      const int64_t off = sample_offset();
      const int64_t px = bid ? anchor_b - off : anchor_a + off;
      if (bid) set_bid(px, get(bids_, px) + rand_qty()); else set_ask(px, get(asks_, px) + rand_qty());
    } else if (u < 0.94) {
      if ((bid ? bids_.size() : asks_.size()) <= 2) return;
      int64_t px = 0;
      bool hit = false;
      for (int tries = 0; tries < 3 && !hit; ++tries) {
        const int64_t off = sample_offset();
        px = bid ? anchor_b - off : anchor_a + off;
        hit = bid ? bids_.count(px) > 0 : asks_.count(px) > 0;
      }
      if (!hit) return;
      const int64_t cur = bid ? get(bids_, px) : get(asks_, px);
      const int64_t rem = rng_.uni() < 0.6 ? cur : static_cast<int64_t>(rng_.uni() * double(cur));
      const int64_t nq = cur - rem;
      if (bid) set_bid(px, nq); else set_ask(px, nq);
    } else {
      // TRADE: aggressor lifts the ask (bid==true -> buyer) or hits the bid
      int64_t q = static_cast<int64_t>(double(rand_qty()) * (0.5 + rng_.uni()));
      if (bid) {
        while (q > 0 && asks_.size() > 1) {
          auto it = asks_.begin();
          if (it->second <= q) { q -= it->second; set_ask(it->first, 0); }
          else { set_ask(it->first, it->second - q); q = 0; }
        }
      } else {
        while (q > 0 && bids_.size() > 1) {
          auto it = bids_.begin();
          if (it->second <= q) { q -= it->second; set_bid(it->first, 0); }
          else { set_bid(it->first, it->second - q); q = 0; }
        }
      }
    }
  }

  void do_spoof(double t) {
    auto it = spoof_.begin();
    const SpoofEv ev = it->second;
    spoof_.erase(it);
    if (ev.kind == 0) {
      const bool bid = rng_.uni() < 0.5;
      const int64_t q = static_cast<int64_t>(cfg_.spoof.qty_mult * 400.0) * kQtyUnit;
      for (int l = 0; l < cfg_.spoof.layers; ++l) {
        const int64_t px = bid ? bids_.begin()->first - cfg_.spoof.depth_ticks - l : asks_.begin()->first + cfg_.spoof.depth_ticks + l;
        if (bid) set_bid(px, get(bids_, px) + q); else set_ask(px, get(asks_, px) + q);
        spoof_.emplace(t + cfg_.spoof.hold_s, SpoofEv{1, bid ? 0 : 1, px, q});
      }
      spoof_windows_.push_back({ns(t), ns(t + cfg_.spoof.hold_s)});
    } else {
      if (ev.side == 0) { const int64_t c = get(bids_, ev.ticks); if (c > 0) set_bid(ev.ticks, c > ev.qty ? c - ev.qty : 0); }
      else { const int64_t c = get(asks_, ev.ticks); if (c > 0) set_ask(ev.ticks, c > ev.qty ? c - ev.qty : 0); }
    }
  }

  void do_stuff() {
    // alternate add / cancel of a tiny order. mode 0: beyond the deepest existing level on a
    // random side (forces front-insertion into a flat sorted array); mode 1: at the touch.
    if (stuff_pending_) {
      if (stuff_side_ == 0) { if (stuff_created_) set_bid(stuff_ticks_, 0); else set_bid(stuff_ticks_, get(bids_, stuff_ticks_) - kQtyUnit); }
      else { if (stuff_created_) set_ask(stuff_ticks_, 0); else set_ask(stuff_ticks_, get(asks_, stuff_ticks_) - kQtyUnit); }
      stuff_pending_ = false;
      return;
    }
    stuff_side_ = rng_.uni() < 0.5 ? 0 : 1;
    if (cfg_.stuff.mode == 0) {
      const int64_t off = cfg_.far_max_ticks + 1 + static_cast<int64_t>(rng_.uni() * 2000.0);
      stuff_ticks_ = stuff_side_ == 0 ? bids_.begin()->first - off : asks_.begin()->first + off;
    } else {
      stuff_ticks_ = stuff_side_ == 0 ? bids_.begin()->first : asks_.begin()->first;
    }
    const int64_t cur = stuff_side_ == 0 ? get(bids_, stuff_ticks_) : get(asks_, stuff_ticks_);
    stuff_created_ = cur == 0;
    if (stuff_side_ == 0) set_bid(stuff_ticks_, cur + kQtyUnit); else set_ask(stuff_ticks_, cur + kQtyUnit);
    stuff_pending_ = true;
  }

  bool finish(double t, DeltaView& out) {
    out_.clear();
    for (size_t i = upd_.size(); i-- > 0;) {
      bool dup = false;
      for (const LevelUpdate& o : out_) if (o.px == upd_[i].px && o.side == upd_[i].side) { dup = true; break; }
      if (!dup) out_.push_back(upd_[i]);
    }
    if (out_.empty()) return false;
    std::reverse(out_.begin(), out_.end());
    Ts ts = ns(t);
    if (ts < last_ts_) ts = last_ts_;
    last_ts_ = ts;
    out.event_ts_ns = ts;
    out.first_id = next_id_;
    out.last_id = next_id_ + static_cast<int64_t>(out_.size()) - 1;
    out.prev_last_id = -1;
    out.lv = out_.data();
    out.n = static_cast<uint32_t>(out_.size());
    next_id_ = out.last_id + 1;
    return true;
  }

  SynthConfig cfg_;
  Rng rng_;
  std::vector<Regime> regimes_;
  size_t ri_ = 0;
  BidMap bids_;
  AskMap asks_;
  double tick_real_ = 0.01, f_ = 0, last_fv_ = 0, next_bg_ = 0, next_fv_ = 0, next_stuff_ = 0;
  std::vector<std::pair<double, double>> bursts_, stuffw_;
  size_t bi_ = 0, sw_i_ = 0;
  bool stuff_active_ = false, stuff_pending_ = false, stuff_created_ = false;
  int stuff_side_ = 0;
  int64_t stuff_ticks_ = 0;
  std::multimap<double, SpoofEv> spoof_;
  std::vector<LevelUpdate> upd_, out_;
  std::vector<Ts> change_ts_;
  std::vector<std::pair<Ts, Ts>> spoof_windows_, stuff_windows_;
  int64_t next_id_ = 1;
  Ts last_ts_ = kStartNs;
};

// ---------------------------------------------------------------------------
// Pre-generated timeline (arena) with checkpoints so an exact snapshot "as of message i"
// can be served for resync.
// ---------------------------------------------------------------------------
struct Timeline {
  static constexpr size_t kCheckpointEvery = 8192;
  Price tick = 1'000'000;
  std::vector<MsgHdr> msgs;
  std::vector<LevelUpdate> lvs;
  std::vector<SnapshotOwned> cps;  // cps[k] = state after k*kCheckpointEvery messages
  std::vector<uint32_t> order;     // delivery order (indices into msgs)
  std::vector<Ts> change_ts;
  std::vector<std::pair<Ts, Ts>> spoof_windows, stuff_windows;

  DeltaView view(size_t i) const {
    const MsgHdr& h = msgs[i];
    DeltaView d;
    d.event_ts_ns = h.ts; d.first_id = h.first_id; d.last_id = h.last_id; d.prev_last_id = -1;
    d.lv = lvs.data() + h.off; d.n = h.n;
    return d;
  }
  size_t size() const { return msgs.size(); }
  size_t bytes() const { return msgs.capacity() * sizeof(MsgHdr) + lvs.capacity() * sizeof(LevelUpdate); }

  // State after message index i (inclusive); i<0 => initial snapshot.
  void snapshot_after(int64_t i, SnapshotOwned& out) const {
    const size_t cnt = static_cast<size_t>(i + 1);
    const size_t k = std::min(cnt / kCheckpointEvery, cps.size() - 1);
    OrderBook scratch(0);
    scratch.load(cps[k].view());
    for (size_t j = k * kCheckpointEvery; j < cnt; ++j) {
      const DeltaView d = view(j);
      for (uint32_t u = 0; u < d.n; ++u) scratch.apply(d.lv[u]);
    }
    out.last_id = i < 0 ? 0 : msgs[static_cast<size_t>(i)].last_id;
    out.event_ts_ns = i < 0 ? SynthFeed::kStartNs : msgs[static_cast<size_t>(i)].ts;
    scratch.bids().copy_levels(out.bids);
    scratch.asks().copy_levels(out.asks);
  }
  // State after the last message whose last_id <= id.
  bool snapshot_for_id(int64_t id, SnapshotOwned& out) const {
    size_t lo = 0, hi = msgs.size();
    while (lo < hi) {
      const size_t mid = (lo + hi) / 2;
      if (msgs[mid].last_id <= id) lo = mid + 1; else hi = mid;
    }
    snapshot_after(static_cast<int64_t>(lo) - 1, out);
    return true;
  }
};

inline Timeline build_timeline(const SynthConfig& cfg, size_t max_msgs = std::numeric_limits<size_t>::max()) {
  Timeline tl;
  tl.tick = cfg.tick;
  SynthFeed feed(cfg);
  tl.cps.push_back(feed.initial_snapshot());
  DeltaView d;
  uint8_t tag;
  std::vector<Level> b, a;
  while (tl.msgs.size() < max_msgs && feed.next(d, &tag)) {
    MsgHdr h{d.event_ts_ns, d.first_id, d.last_id, static_cast<uint32_t>(tl.lvs.size()), d.n, tag};
    tl.lvs.insert(tl.lvs.end(), d.lv, d.lv + d.n);
    tl.msgs.push_back(h);
    if (tl.msgs.size() % Timeline::kCheckpointEvery == 0) {
      SnapshotOwned s;
      s.last_id = d.last_id;
      s.event_ts_ns = d.event_ts_ns;
      feed.export_levels(s.bids, s.asks);
      tl.cps.push_back(std::move(s));
    }
  }
  tl.change_ts = feed.change_ts();
  tl.spoof_windows = feed.spoof_windows();
  tl.stuff_windows = feed.stuff_windows();
  tl.order.resize(tl.msgs.size());
  for (size_t i = 0; i < tl.order.size(); ++i) tl.order[i] = static_cast<uint32_t>(i);
  return tl;
}

// Rewrite the DELIVERY order only: drops, duplicates, adjacent swaps.
inline void apply_impairments(Timeline& tl, const Impair& im, uint64_t seed) {
  Rng rng(seed ^ 0xD1B54A32D192ED03ULL);
  std::vector<uint32_t> o;
  o.reserve(tl.msgs.size() + 16);
  for (size_t i = 0; i < tl.msgs.size(); ++i) {
    if (rng.uni() < im.p_drop) continue;
    o.push_back(static_cast<uint32_t>(i));
    if (rng.uni() < im.p_dup) o.push_back(static_cast<uint32_t>(i));
  }
  for (size_t i = 0; i + 1 < o.size(); ++i)
    if (rng.uni() < im.p_swap) { std::swap(o[i], o[i + 1]); ++i; }
  tl.order.swap(o);
}

// ---- wire formatters -------------------------------------------------------
namespace detail {
inline void append_u(std::string& s, uint64_t v) {
  char t[24];
  int n = 0;
  if (v == 0) t[n++] = '0';
  while (v) { t[n++] = char('0' + v % 10); v /= 10; }
  while (n) s.push_back(t[--n]);
}
inline void append_fixed(std::string& s, int64_t v) {
  append_u(s, static_cast<uint64_t>(v / 100000000LL));
  s.push_back('.');
  int64_t fp = v % 100000000LL;
  char f[8];
  for (int i = 7; i >= 0; --i) { f[i] = char('0' + fp % 10); fp /= 10; }
  s.append(f, 8);
}
}  // namespace detail

// Binance spot/futures depthUpdate JSON (E in ms). futures=true adds "pu".
inline void format_binance_json(const DeltaView& d, std::string& s, bool futures = false, const char* sym = "BTCUSDT") {
  s.clear();
  s += "{\"e\":\"depthUpdate\",\"E\":";
  detail::append_u(s, static_cast<uint64_t>(d.event_ts_ns / 1'000'000));
  s += ",\"s\":\""; s += sym; s += "\",\"U\":";
  detail::append_u(s, static_cast<uint64_t>(d.first_id));
  s += ",\"u\":";
  detail::append_u(s, static_cast<uint64_t>(d.last_id));
  if (futures) { s += ",\"pu\":"; detail::append_u(s, static_cast<uint64_t>(d.first_id - 1)); }
  for (int side = 0; side < 2; ++side) {
    s += side == 0 ? ",\"b\":[" : ",\"a\":[";
    bool first = true;
    for (uint32_t i = 0; i < d.n; ++i) {
      if (static_cast<int>(d.lv[i].side) != side) continue;
      if (!first) s.push_back(',');
      first = false;
      s += "[\"";
      detail::append_fixed(s, d.lv[i].px);
      s += "\",\"";
      detail::append_fixed(s, d.lv[i].qty);
      s += "\"]";
    }
    s.push_back(']');
  }
  s.push_back('}');
}

inline void format_csv_delta(const DeltaView& d, std::string& s) {
  s.clear();
  s += "D,";
  detail::append_u(s, static_cast<uint64_t>(d.event_ts_ns)); s.push_back(',');
  detail::append_u(s, static_cast<uint64_t>(d.first_id)); s.push_back(',');
  detail::append_u(s, static_cast<uint64_t>(d.last_id)); s.push_back(',');
  for (uint32_t i = 0; i < d.n; ++i) {
    if (i) s.push_back(';');
    s += d.lv[i].side == Side::Bid ? "B:" : "A:";
    detail::append_fixed(s, d.lv[i].px); s.push_back(':');
    detail::append_fixed(s, d.lv[i].qty);
  }
}

}  // namespace mp
