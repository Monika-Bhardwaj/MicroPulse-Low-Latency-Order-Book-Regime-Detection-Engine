// Price-level aggregated limit order book.
//
// Each side is a contiguous vector sorted so that the BEST level is at the BACK:
// we store key = sign*price (bids sign=+1, asks sign=-1) in ascending key order, so
// both sides share one code path and "best" is always v.back().
//
// Update cost (n = levels held on that side, k = #levels better than the updated one):
//   search : O(log k)   galloping (exponential) search from the touch
//   mutate : O(k)       memmove of the tail
// Real feeds concentrate updates near the touch, so k is tiny in practice. The
// adversarial case (quote stuffing at deep prices) is k ~ n: this is the reason the
// book can be CAPPED to `max_levels` (see below) and why the cap is the default.
//
// max_levels == 0  -> FULL DEPTH: every level is retained (unbounded memory).
// max_levels  > 0  -> CAPPED: only the best `max_levels` levels are retained.
//   * an update worse than the worst retained level while full is rejected in O(1)
//   * the retained set is always exactly the true top-`size()` levels (proof sketch:
//     inserts evict the worst; deletes remove an element; rejected updates are worse
//     than everything held), so features over the top K <= size() levels are exact.
//   * when deletions shrink a truncated side below the consumer's required depth the
//     engine requests a snapshot refill (see Engine::refill_requested).
#pragma once
#include <algorithm>
#include <cstring>
#include <vector>

#include "types.hpp"

namespace mp {

class BookSide {
 public:
  enum class Result : uint8_t { Applied, IgnoredDeep, Phantom };

  void init(bool is_bid, size_t max_levels) {
    sign_ = is_bid ? 1 : -1;
    cap_ = max_levels;
    truncated_ = false;
    v_.clear();
    v_.reserve(cap_ ? cap_ + 1 : 1024);
  }

  Result apply(Price px, Qty qty) {
    const int64_t key = sign_ * px;
    const size_t n = v_.size();
    size_t idx;
    if (n == 0 || key > v_[n - 1].key) {
      idx = n;  // strictly better than the current touch (or first level)
    } else {
      // Galloping search from the back: finds the first index with v[idx].key >= key.
      size_t lo = 0, hi = n, step = 1;
      while (step <= n) {
        const size_t p = n - step;
        if (v_[p].key < key) {
          lo = p + 1;
          break;
        }
        hi = p;  // v[p].key >= key  =>  idx <= p
        step <<= 1;
      }
      idx = static_cast<size_t>(
          std::lower_bound(v_.begin() + lo, v_.begin() + hi, key,
                           [](const Entry& e, int64_t k) { return e.key < k; }) -
          v_.begin());
    }
    const bool found = idx < n && v_[idx].key == key;
    if (found) {
      if (qty == 0) {
        v_.erase(v_.begin() + idx);
      } else {
        v_[idx].qty = qty;
      }
      return Result::Applied;
    }
    if (cap_ && (n >= cap_ || truncated_) && idx == 0) {
      // Beyond our horizon (worse than every retained level): O(1) reject. Also applies
      // when a truncated side has shrunk below cap: levels may exist between the worst
      // retained level and this price that we never saw, so inserting would break the
      // "retained == true top-n" invariant.
      truncated_ = true;
      return Result::IgnoredDeep;
    }
    if (qty == 0) return Result::Phantom;  // delete of a level we never saw
    if (cap_ && n >= cap_) {
      // evict the worst (index 0) and insert at idx-1: one memmove of the head region
      std::memmove(&v_[0], &v_[1], (idx - 1) * sizeof(Entry));
      v_[idx - 1] = Entry{key, qty};
      truncated_ = true;
      return Result::Applied;
    }
    v_.insert(v_.begin() + idx, Entry{key, qty});
    return Result::Applied;
  }

  size_t size() const { return v_.size(); }
  bool empty() const { return v_.empty(); }
  bool truncated() const { return truncated_; }
  size_t cap() const { return cap_; }

  Level best() const {
    const Entry& e = v_.back();
    return Level{sign_ * e.key, e.qty};
  }
  Level at(size_t i) const {  // i-th best level, 0 = touch
    const Entry& e = v_[v_.size() - 1 - i];
    return Level{sign_ * e.key, e.qty};
  }
  // Sum of quantity over the best k levels (O(k)).
  Qty depth_qty(size_t k) const {
    const size_t n = std::min(k, v_.size());
    Qty s = 0;
    for (size_t i = 0; i < n; ++i) s += v_[v_.size() - 1 - i].qty;
    return s;
  }

  void clear() {
    v_.clear();
    truncated_ = false;
  }

  // Replace contents from a snapshot (any order). Applies the depth cap. In capped mode the
  // container keeps its cap+1 reservation (a snapshot never inflates steady-state memory).
  void load(const Level* lv, size_t n) {
    v_.clear();
    truncated_ = false;
    auto by_key = [](const Entry& a, const Entry& b) { return a.key < b.key; };
    if (!cap_) {
      for (size_t i = 0; i < n; ++i)
        if (lv[i].qty > 0) v_.push_back(Entry{sign_ * lv[i].px, lv[i].qty});
      std::sort(v_.begin(), v_.end(), by_key);
      return;
    }
    std::vector<Entry> tmp;  // off the hot path (snapshot only)
    tmp.reserve(n);
    for (size_t i = 0; i < n; ++i)
      if (lv[i].qty > 0) tmp.push_back(Entry{sign_ * lv[i].px, lv[i].qty});
    std::sort(tmp.begin(), tmp.end(), by_key);
    const size_t keep = std::min(cap_, tmp.size());
    truncated_ = tmp.size() > cap_;
    v_.assign(tmp.end() - static_cast<std::ptrdiff_t>(keep), tmp.end());
  }

  void copy_levels(std::vector<Level>& out) const {  // worst -> best
    out.clear();
    out.reserve(v_.size());
    for (const Entry& e : v_) out.push_back(Level{sign_ * e.key, e.qty});
  }

  size_t memory_bytes() const { return v_.capacity() * sizeof(Entry); }

 private:
  struct Entry {
    int64_t key;
    Qty qty;
  };
  std::vector<Entry> v_;
  int64_t sign_ = 1;
  size_t cap_ = 0;
  bool truncated_ = false;
};

class OrderBook {
 public:
  explicit OrderBook(size_t max_levels = 0) : max_levels_(max_levels) {
    bids_.init(true, max_levels);
    asks_.init(false, max_levels);
  }

  BookSide::Result apply(const LevelUpdate& u) {
    return u.side == Side::Bid ? bids_.apply(u.px, u.qty) : asks_.apply(u.px, u.qty);
  }

  void load(const SnapshotView& s) {
    bids_.load(s.bids, s.n_bids);
    asks_.load(s.asks, s.n_asks);
  }
  void clear() {
    bids_.clear();
    asks_.clear();
  }

  bool has_touch() const { return !bids_.empty() && !asks_.empty(); }
  bool crossed() const { return has_touch() && bids_.best().px >= asks_.best().px; }
  Level best_bid() const { return bids_.best(); }
  Level best_ask() const { return asks_.best(); }
  const BookSide& bids() const { return bids_; }
  const BookSide& asks() const { return asks_; }
  bool truncated() const { return bids_.truncated() || asks_.truncated(); }
  size_t max_levels() const { return max_levels_; }
  size_t memory_bytes() const { return bids_.memory_bytes() + asks_.memory_bytes(); }

 private:
  size_t max_levels_;
  BookSide bids_, asks_;
};

}  // namespace mp
