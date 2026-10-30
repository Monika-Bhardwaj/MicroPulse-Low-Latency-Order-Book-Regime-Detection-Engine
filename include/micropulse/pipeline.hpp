// Bounded single-producer/single-consumer ingestion queue + threaded pipeline.
//
// The queue is the ONLY place where a message burst can accumulate, and it is bounded:
//   OverflowPolicy::Block : producer waits for a free slot (backpressure). In a live
//                           deployment this translates to TCP backpressure; the venue may
//                           eventually disconnect a slow consumer, so the wait is counted.
//   OverflowPolicy::Drop  : producer drops the NEWEST frame when full and keeps going.
//                           The dropped frame's ids surface as a sequence gap in the
//                           engine, which invalidates the book, flags the windows and
//                           requests a resync. Latency stays bounded; data quality is
//                           flagged rather than silently degraded.
#pragma once
#include <atomic>
#include <cstring>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

#include "clock.hpp"
#include "engine.hpp"

namespace mp {

template <class T>
class SpscRing {
 public:
  explicit SpscRing(size_t cap_pow2) : buf_(cap_pow2), mask_(cap_pow2 - 1) {}
  // producer side
  T* try_acquire() {
    const size_t t = tail_.load(std::memory_order_relaxed);
    if (t - head_cache_ > mask_) {
      head_cache_ = head_.load(std::memory_order_acquire);
      if (t - head_cache_ > mask_) return nullptr;
    }
    return &buf_[t & mask_];
  }
  void publish() { tail_.store(tail_.load(std::memory_order_relaxed) + 1, std::memory_order_release); }
  // consumer side
  T* front() {
    const size_t h = head_.load(std::memory_order_relaxed);
    if (h == tail_cache_) {
      tail_cache_ = tail_.load(std::memory_order_acquire);
      if (h == tail_cache_) return nullptr;
    }
    return &buf_[h & mask_];
  }
  void pop() { head_.store(head_.load(std::memory_order_relaxed) + 1, std::memory_order_release); }
  size_t size_approx() const { return tail_.load(std::memory_order_acquire) - head_.load(std::memory_order_acquire); }
  size_t capacity() const { return mask_ + 1; }

 private:
  std::vector<T> buf_;
  size_t mask_;
  alignas(64) std::atomic<size_t> head_{0};
  alignas(64) std::atomic<size_t> tail_{0};
  alignas(64) size_t head_cache_ = 0;  // producer's cached view of head
  alignas(64) size_t tail_cache_ = 0;  // consumer's cached view of tail
};

inline constexpr size_t kFrameMax = 16384;
struct Frame {
  uint32_t len;
  int64_t recv_ns;  // local monotonic receive stamp (latency only; never compared to event time)
  char data[kFrameMax];
};

enum class OverflowPolicy { Block, Drop };

// Returns a snapshot consistent with "state after the message whose last id is `id`".
using SnapshotProvider = std::function<bool(int64_t id, SnapshotOwned& out)>;

class Pipeline {
 public:
  Pipeline(Engine& eng, size_t ring_slots, OverflowPolicy pol, SnapshotProvider prov)
      : eng_(eng), ring_(ring_slots), pol_(pol), prov_(std::move(prov)) {}
  ~Pipeline() { stop(); }

  void start() {
    run_.store(true);
    th_ = std::thread([this] { loop(); });
  }
  void stop() {
    if (!run_.exchange(false)) return;
    if (th_.joinable()) th_.join();
  }

  // Producer thread. Returns false if the frame was dropped.
  bool submit(const char* d, size_t n, int64_t recv_ns) {
    if (n > kFrameMax) { ++oversize_; return false; }
    Frame* f = ring_.try_acquire();
    if (!f) {
      if (pol_ == OverflowPolicy::Drop) { ++dropped_; return false; }
      do {
        ++blocked_spins_;
        std::this_thread::yield();  // 1-vCPU friendly; a real deployment would pause/spin on a dedicated core
        f = ring_.try_acquire();
      } while (!f);
    }
    f->len = static_cast<uint32_t>(n);
    f->recv_ns = recv_ns;
    std::memcpy(f->data, d, n);
    ring_.publish();
    const size_t q = ring_.size_approx();
    if (q > max_depth_) max_depth_ = q;
    return true;
  }
  // Wait until the consumer has drained everything submitted so far.
  void drain() { while (ring_.size_approx() > 0) std::this_thread::yield(); }

  uint64_t processed() const { return processed_.load(); }
  uint64_t dropped() const { return dropped_; }
  uint64_t blocked_spins() const { return blocked_spins_; }
  size_t max_depth() const { return max_depth_; }
  uint64_t resyncs_served() const { return resyncs_; }
  const std::vector<uint32_t>& e2e_ns() const { return e2e_; }
  void reserve_latency(size_t n) { e2e_.reserve(n); }

 private:
  void loop() {
    const double npc = ns_per_cycle();
    SnapshotOwned snap;
    for (;;) {
      Frame* f = ring_.front();
      if (!f) {
        if (!run_.load(std::memory_order_acquire) && ring_.size_approx() == 0) break;
        std::this_thread::yield();
        continue;
      }
      const uint64_t c0 = cycles();
      eng_.on_frame(f->data, f->len, f->recv_ns);
      (void)c0; (void)npc;
      const int64_t now = mono_ns();
      if (e2e_.size() < e2e_.capacity()) e2e_.push_back(static_cast<uint32_t>(std::min<int64_t>(now - f->recv_ns, 0xFFFFFFFFLL)));
      ring_.pop();
      processed_.fetch_add(1, std::memory_order_relaxed);
      if (prov_ && eng_.resync_needed()) {
        if (prov_(eng_.reject_id(), snap)) {
          eng_.apply_snapshot(snap.view());
          ++resyncs_;
        }
      }
    }
  }

  Engine& eng_;
  SpscRing<Frame> ring_;
  OverflowPolicy pol_;
  SnapshotProvider prov_;
  std::thread th_;
  std::atomic<bool> run_{false};
  std::atomic<uint64_t> processed_{0};
  uint64_t dropped_ = 0, blocked_spins_ = 0, oversize_ = 0, resyncs_ = 0;
  size_t max_depth_ = 0;
  std::vector<uint32_t> e2e_;
};

}  // namespace mp
