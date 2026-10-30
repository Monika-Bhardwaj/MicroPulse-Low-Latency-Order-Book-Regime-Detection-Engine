// Engine: parser -> sequence check -> book -> features -> bars -> detectors.
//
// FAILURE HANDLING (the book is never silently wrong):
//   stale/duplicate (last_id <= applied)   : dropped, counted, no state change
//   gap / out-of-order (id discontinuity)  : book INVALIDATED, deltas dropped until a
//                                            snapshot is applied, rolling windows flagged
//   crossed book after a message           : treated as a gap
//   oversize message (hardening limit)     : treated as a gap
//   parse error                            : counted; the missing ids surface as a gap on
//                                            the next message
//   truncated book shrinks below min depth : REFILL requested; features stay exact because
//                                            the retained levels are the true top levels
//
// Snapshot sync rule (Binance): drop deltas with u <= snapshot.lastUpdateId; the first
// applied delta must satisfy U <= lastUpdateId+1 <= u; afterwards U == prev_u+1
// (or pu == prev_u on futures streams).
#pragma once
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include "book.hpp"
#include "detector.hpp"
#include "features.hpp"
#include "parser.hpp"
#include "types.hpp"

namespace mp {

enum class Status : uint8_t { Ok, Stale, Gap, Crossed, Oversize, ParseError, Unsynced, NoTouch };
enum class Channel : uint8_t { Vol = 0, Flow = 1 };
// Flow channel statistic fed to the detector:
//   RawSum : x = sum of OFI in the bar (what a naive implementation would use)
//   TStat  : x = sum(e) / sqrt(sum(e^2)). Scale-free, and a single huge order can move
//            it by at most +-1 -> robust to spoofed size.
enum class FlowStat : uint8_t { RawSum = 0, TStat = 1 };

struct EngineConfig {
  size_t max_levels = 256;        // 0 = full depth. See reports/BENCHMARK.md for the tradeoff.
  size_t min_valid_levels = 0;    // 0 => feature depth + 2
  size_t max_levels_per_msg = 4096;  // hardening: reject absurd messages
  FeatureConfig feat;
  FlowStat flow_stat = FlowStat::TStat;
  Price tick = 1'000'000;         // price tick (fixed-point); sets the log-RV floor
  bool record_features = false;
  bool record_bars = false;
};

struct EngineStats {
  uint64_t msgs = 0, applied = 0, stale = 0, gaps = 0, crossed = 0, oversize = 0, parse_errors = 0;
  uint64_t dropped_unsynced = 0, resyncs = 0, refills = 0;
  uint64_t ignored_deep = 0, phantom_deletes = 0, level_updates = 0;
  uint64_t bars = 0, bars_skipped_tainted = 0;
  uint64_t alarms[2] = {0, 0};
};

struct BarRec {
  Ts t_end = 0;
  double rv = 0, ofi = 0, ofi_sq = 0, x_vol = 0, x_flow = 0;
  int64_t n = 0;
  uint32_t flags = 0;
  double score[4] = {0, 0, 0, 0};
  uint8_t alarm[4] = {0, 0, 0, 0};
};

struct AlarmRec {
  Ts ts;
  int slot;
  int channel;
  double score;
  int dir;
};

template <class FE>
class BasicEngine {
 public:
  explicit BasicEngine(const EngineConfig& c)
      : cfg_(c), book_(c.max_levels), fe_(c.feat) {
    if (cfg_.min_valid_levels == 0) cfg_.min_valid_levels = static_cast<size_t>(cfg_.feat.depth_levels) + 2;
  }

  void set_parser(std::unique_ptr<FeedParser> p) { parser_ = std::move(p); }
  FeedParser* parser() { return parser_.get(); }

  // Attach a detector to a channel. Up to 4 slots. Ingestion code is unaware of the type.
  int add_detector(Channel ch, std::unique_ptr<Detector> d) {
    slots_.push_back(Slot{ch, std::move(d)});
    return static_cast<int>(slots_.size()) - 1;
  }

  Status on_frame(const char* data, size_t n, Ts /*recv_ns*/) {
    if (!parser_) return Status::ParseError;
    DeltaView dv;
    if (!parser_->parse(data, n, dv)) {
      ++stats_.msgs;
      ++stats_.parse_errors;
      return Status::ParseError;
    }
    return on_delta(dv);
  }

  Status on_delta(const DeltaView& d) {
    ++stats_.msgs;
    if (!synced_) {
      ++stats_.dropped_unsynced;
      last_event_ts_ = std::max(last_event_ts_, d.event_ts_ns);
      reject_id_ = d.last_id;
      return Status::Unsynced;
    }
    if (d.last_id <= last_id_) {
      ++stats_.stale;
      return Status::Stale;
    }
    bool contiguous;
    if (first_after_snap_) contiguous = d.first_id <= last_id_ + 1;
    else if (d.prev_last_id >= 0) contiguous = d.prev_last_id == last_id_;
    else contiguous = d.first_id == last_id_ + 1;
    if (!contiguous) return invalidate(d, Status::Gap);
    if (d.n > cfg_.max_levels_per_msg) return invalidate(d, Status::Oversize);
    first_after_snap_ = false;

    for (uint32_t i = 0; i < d.n; ++i) {
      const BookSide::Result r = book_.apply(d.lv[i]);
      if (r == BookSide::Result::IgnoredDeep) ++stats_.ignored_deep;
      else if (r == BookSide::Result::Phantom) ++stats_.phantom_deletes;
    }
    stats_.level_updates += d.n;
    last_id_ = d.last_id;
    last_event_ts_ = std::max(last_event_ts_, d.event_ts_ns);
    ++stats_.applied;

    if (book_.crossed()) {  // feed corruption signature: never publish features from it
      ++stats_.crossed;
      synced_ = false;
      reject_id_ = d.last_id;
      fe_.mark_gap(d.event_ts_ns);
      return Status::Crossed;
    }
    if (cfg_.max_levels && book_.truncated() &&
        (book_.bids().size() < cfg_.min_valid_levels || book_.asks().size() < cfg_.min_valid_levels)) {
      refill_ = true;
    }
    if (!fe_.on_update(d.event_ts_ns, book_, d.last_id)) return Status::NoTouch;
    cur_ = fe_.current();
    Bar b;
    if (fe_.take_bar(b)) on_bar(b);
    cur_.vol_score = det_score_[0]; cur_.flow_score = det_score_[1];
    cur_.vol_alarm = det_alarm_[0]; cur_.flow_alarm = det_alarm_[1];
    if (cfg_.record_features) rec_.push_back(cur_);
    return Status::Ok;
  }

  // Apply a snapshot (initial sync, gap resync, or depth refill).
  void apply_snapshot(const SnapshotView& s) {
    const bool was_synced = synced_;
    if (was_synced && s.last_id < last_id_) return;  // stale refill snapshot: ignore
    book_.load(s);
    last_id_ = s.last_id;
    synced_ = true;
    first_after_snap_ = true;
    if (!was_synced) {
      if (started_) ++stats_.resyncs;
      fe_.mark_gap(std::max(last_event_ts_, s.event_ts_ns));
    } else if (refill_) {
      ++stats_.refills;
    }
    started_ = true;
    refill_ = false;
  }

  bool resync_needed() const { return !synced_; }   // book invalid: snapshot required
  bool refill_requested() const { return synced_ && refill_; }  // truncated book running low
  int64_t reject_id() const { return reject_id_; }  // last_id of the message that was NOT applied
  bool synced() const { return synced_; }

  Features features() const {
    Features f = cur_;
    if (!synced_) f.flags |= F_BOOK_STALE;
    return f;
  }
  const EngineStats& stats() const { return stats_; }
  const OrderBook& book() const { return book_; }
  FE& feature_engine() { return fe_; }
  const EngineConfig& config() const { return cfg_; }
  const std::vector<Features>& recorded_features() const { return rec_; }
  const std::vector<BarRec>& recorded_bars() const { return bars_; }
  const std::vector<AlarmRec>& alarms() const { return alarms_; }
  void clear_records() { rec_.clear(); bars_.clear(); alarms_.clear(); }
  size_t memory_bytes() const {
    return book_.memory_bytes() + fe_.window().memory_bytes() + rec_.capacity() * sizeof(Features);
  }

 private:
  struct Slot {
    Channel ch;
    std::unique_ptr<Detector> det;
  };

  Status invalidate(const DeltaView& d, Status why) {
    if (why == Status::Gap) ++stats_.gaps;
    else if (why == Status::Oversize) ++stats_.oversize;
    synced_ = false;
    reject_id_ = d.last_id;
    last_event_ts_ = std::max(last_event_ts_, d.event_ts_ns);
    fe_.mark_gap(d.event_ts_ns);
    return why;
  }

  void on_bar(const Bar& b) {
    ++stats_.bars;
    const double mid = fe_.last_mid() > 0 ? fe_.last_mid() : 1.0;
    const double tick_real = double(cfg_.tick) / kFixedScale;
    const double eps = 0.25 * (tick_real / mid) * (tick_real / mid);
    BarRec rec;
    rec.t_end = b.t_end; rec.rv = b.rv; rec.ofi = b.ofi; rec.ofi_sq = b.ofi_sq; rec.n = b.n; rec.flags = b.flags;
    rec.x_vol = std::log(b.rv + eps);
    rec.x_flow = cfg_.flow_stat == FlowStat::TStat ? (b.ofi_sq > 0 ? b.ofi / std::sqrt(b.ofi_sq) : 0.0) : b.ofi;
    if (b.flags & F_GAP_RECENT) {
      ++stats_.bars_skipped_tainted;  // flagged, NOT fed to detectors
    } else {
      for (size_t i = 0; i < slots_.size() && i < 4; ++i) {
        const double x = slots_[i].ch == Channel::Vol ? rec.x_vol : rec.x_flow;
        const DetectorOutput o = slots_[i].det->update(b.t_end, x);
        rec.score[i] = o.score;
        rec.alarm[i] = o.alarm;
        const int c = static_cast<int>(slots_[i].ch);
        det_score_[c] = o.score;
        det_alarm_[c] = o.alarm;
        if (o.alarm) {
          ++stats_.alarms[c];
          if (alarms_.size() < 1'000'000) alarms_.push_back(AlarmRec{b.t_end, static_cast<int>(i), c, o.score, o.dir});
        }
      }
    }
    if (cfg_.record_bars) bars_.push_back(rec);
  }

  EngineConfig cfg_;
  OrderBook book_;
  FE fe_;
  std::unique_ptr<FeedParser> parser_;
  std::vector<Slot> slots_;
  Features cur_;
  EngineStats stats_;
  std::vector<Features> rec_;
  std::vector<BarRec> bars_;
  std::vector<AlarmRec> alarms_;
  double det_score_[2] = {0, 0};
  uint8_t det_alarm_[2] = {0, 0};
  int64_t last_id_ = 0, reject_id_ = 0;
  Ts last_event_ts_ = 0;
  bool synced_ = false, first_after_snap_ = false, refill_ = false, started_ = false;
};

using Engine = BasicEngine<FeatureEngine>;
using NaiveEngine = BasicEngine<NaiveFeatureEngine>;

}  // namespace mp
