// MicroPulse benchmark harness. Emits markdown tables on stdout.
//   bench_main <parse|core|naive|stuffing|burst|memory|threads|all> [--quick]
//
// Methodology notes (also in reports/BENCHMARK.md):
//   * Per-message service latency = rdtscp before/after Engine::on_delta (or on_frame);
//     the ~20 ns timer overhead is reported and NOT subtracted.
//   * Throughput is measured in a SEPARATE bulk pass with no per-message timers.
//   * Snapshot/resync handling is excluded from service latency and modelled explicitly
//     in the burst simulation.
//   * The burst section is a virtual-time single-server queue driven by MEASURED service
//     times (open-loop arrivals, latency measured from the INTENDED arrival time, i.e.
//     no coordinated omission). It exists because this sandbox has ONE vCPU, which makes a
//     two-thread producer/consumer latency measurement meaningless.
#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <string>
#include <thread>

#include "micropulse/engine.hpp"
#include "micropulse/pipeline.hpp"
#include "micropulse/synth.hpp"

using namespace mp;

static bool g_quick = false;

// ------------------------------------------------------------------ helpers --
struct Pct {
  double p50, p90, p99, p999, max;
};
static Pct pct_ns(std::vector<uint32_t>& cyc) {
  const double npc = ns_per_cycle();
  if (cyc.empty()) return {0, 0, 0, 0, 0};
  std::sort(cyc.begin(), cyc.end());
  auto at = [&](double p) { return double(cyc[std::min(cyc.size() - 1, size_t(p * double(cyc.size())))]) * npc; };
  return {at(0.5), at(0.9), at(0.99), at(0.999), double(cyc.back()) * npc};
}
static double timer_overhead_ns() {
  std::vector<uint32_t> v;
  for (int i = 0; i < 200000; ++i) { const uint64_t a = cycles(); const uint64_t b = cycles(); v.push_back(uint32_t(b - a)); }
  return pct_ns(v).p50;
}
static double now_s() { return double(mono_ns()) * 1e-9; }

static void add_detectors(Engine& e) {
  e.add_detector(Channel::Vol, std::make_unique<CusumDetector>());
  e.add_detector(Channel::Vol, std::make_unique<BocpdDetector>());
  e.add_detector(Channel::Flow, std::make_unique<CusumDetector>());
}

static SynthConfig steady_cfg(double dur, double rate, uint64_t seed = 1) {
  SynthConfig c;
  c.seed = seed; c.duration_s = dur; c.base_rate = rate;
  return c;
}

struct ModeCfg {
  const char* name;
  size_t cap;
};
static const ModeCfg kModes[] = {{"FULL depth (unbounded)", 0}, {"CAPPED 256/side", 256}, {"TOP-32/side", 32}};

// Replays the delivery order. Resync/refill handled OUTSIDE the timed region.
template <class E>
static void replay(E& eng, const Timeline& tl, std::vector<uint32_t>* cyc, const std::vector<uint8_t>* tag_filter_unused = nullptr,
                   std::vector<uint32_t>* cyc_stuff = nullptr) {
  (void)tag_filter_unused;
  SnapshotOwned snap;
  const size_t n = tl.order.size();
  if (cyc) cyc->reserve(n);
  if (cyc_stuff) cyc_stuff->reserve(n);
  for (size_t k = 0; k < n; ++k) {
    const size_t i = tl.order[k];
    const DeltaView d = tl.view(i);
    if (cyc) {
      const uint64_t c0 = cycles();
      eng.on_delta(d);
      const uint64_t c1 = cycles();
      const uint32_t dc = uint32_t(c1 - c0);
      if (cyc_stuff && tl.msgs[i].tag == 1) cyc_stuff->push_back(dc); else cyc->push_back(dc);
    } else {
      eng.on_delta(d);
    }
    if (eng.resync_needed()) { tl.snapshot_for_id(eng.reject_id(), snap); eng.apply_snapshot(snap.view()); }
    else if (eng.refill_requested()) { tl.snapshot_for_id(tl.msgs[i].last_id, snap); eng.apply_snapshot(snap.view()); }
  }
}

template <class E>
static double bulk_throughput(E& eng, const Timeline& tl) {
  SnapshotOwned snap;
  const size_t n = tl.order.size();
  double work = 0;  // wall time spent in on_delta only (chunked so timer overhead is amortised)
  size_t k = 0;
  while (k < n) {
    const size_t end = std::min(n, k + 256);
    const double t0 = now_s();
    size_t j = k;
    for (; j < end; ++j) {
      eng.on_delta(tl.view(tl.order[j]));
      if (eng.resync_needed() || eng.refill_requested()) { ++j; break; }
    }
    work += now_s() - t0;
    if (eng.resync_needed()) { tl.snapshot_for_id(eng.reject_id(), snap); eng.apply_snapshot(snap.view()); }
    else if (eng.refill_requested()) { tl.snapshot_for_id(tl.msgs[tl.order[j - 1]].last_id, snap); eng.apply_snapshot(snap.view()); }
    k = j;
  }
  return double(n) / work;
}

static std::unique_ptr<Engine> make_engine(const Timeline& tl, size_t cap, bool det) {
  EngineConfig ec;
  ec.max_levels = cap;
  auto e = std::make_unique<Engine>(ec);
  if (det) add_detectors(*e);
  e->apply_snapshot(tl.cps[0].view());
  return e;
}

static void row(const char* name, const char* det, double thr, const Pct& p, const char* extra) {
  std::printf("| %s | %s | %.2f M | %.0f | %.0f | %.0f | %.0f | %.0f | %s |\n", name, det, thr / 1e6, p.p50, p.p90, p.p99, p.p999, p.max, extra);
}

// --------------------------------------------------------------------- parse --
static void bench_parse() {
  std::printf("\n## 1. Parser throughput (Binance depthUpdate JSON, hand-written parser)\n\n");
  Timeline tl = build_timeline(steady_cfg(g_quick ? 30 : 100, 2500));
  const size_t n = tl.size();
  std::vector<std::string> frames(n);
  size_t bytes = 0;
  for (size_t i = 0; i < n; ++i) { format_binance_json(tl.view(i), frames[i]); bytes += frames[i].size(); }
  BinanceDepthParser p;
  DeltaView d;
  size_t sink = 0;
  double best = 1e30;
  for (int rep = 0; rep < 3; ++rep) {
    const double t0 = now_s();
    for (size_t i = 0; i < n; ++i) { p.parse(frames[i].data(), frames[i].size(), d); sink += d.n; }
    best = std::min(best, now_s() - t0);
  }
  std::printf("| messages | avg bytes/msg | avg levels/msg | parse ns/msg | msgs/s | MB/s |\n|---|---|---|---|---|---|\n");
  std::printf("| %zu | %.0f | %.2f | %.0f | %.2f M | %.0f |\n", n, double(bytes) / double(n), double(sink) / (3.0 * double(n)),
              best / double(n) * 1e9, double(n) / best / 1e6, double(bytes) / best / 1e6);
  // end-to-end: frame -> parse -> sequence check -> book -> features (no detectors)
  Engine e(EngineConfig{});
  e.set_parser(std::make_unique<BinanceDepthParser>());
  e.apply_snapshot(tl.cps[0].view());
  std::vector<uint32_t> cyc;
  cyc.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    const uint64_t c0 = cycles();
    e.on_frame(frames[i].data(), frames[i].size(), 0);
    cyc.push_back(uint32_t(cycles() - c0));
  }
  const Pct pc = pct_ns(cyc);
  std::printf("\nFrame -> parse -> book -> features (CAPPED 256, no detectors), per-message service latency: p50 %.0f ns, p99 %.0f ns, p99.9 %.0f ns, max %.0f ns (n=%zu, stat resyncs=%llu)\n",
              pc.p50, pc.p99, pc.p999, pc.max, n, (unsigned long long)e.stats().resyncs);
}

// ---------------------------------------------------------------------- core --
static void bench_core() {
  std::printf("\n## 2. Core latency / throughput (parsed deltas -> book -> features [-> detectors])\n\n");
  const double dur = g_quick ? 60 : 400;
  Timeline tl = build_timeline(steady_cfg(dur, 2500));
  std::printf("Stream: %zu messages over %.0f s event time (synthetic, seed 1), %.2f level updates/msg. Latencies in ns, per message.\n\n", tl.size(), dur, double(tl.lvs.size()) / double(tl.size()));
  std::printf("| mode | detectors | throughput (msgs/s) | p50 | p90 | p99 | p99.9 | max | notes |\n|---|---|---|---|---|---|---|---|---|\n");
  for (const ModeCfg& m : kModes) {
    for (int det = 0; det < 2; ++det) {
      std::vector<double> thr;
      for (int rep = 0; rep < 3; ++rep) { auto e = make_engine(tl, m.cap, det); thr.push_back(bulk_throughput(*e, tl)); }
      std::sort(thr.begin(), thr.end());
      auto e = make_engine(tl, m.cap, det);
      std::vector<uint32_t> cyc;
      replay(*e, tl, &cyc);
      const Pct p = pct_ns(cyc);
      char extra[160];
      std::snprintf(extra, sizeof extra, "end levels %zu+%zu, refills %llu, resyncs %llu", e->book().bids().size(), e->book().asks().size(),
                    (unsigned long long)e->stats().refills, (unsigned long long)e->stats().resyncs);
      row(m.name, det ? "CUSUM x2 + BOCPD" : "none", thr[1], p, extra);
    }
  }
  std::printf("\nTimer overhead (back-to-back rdtscp, p50): %.0f ns (included in all latencies above).\n", timer_overhead_ns());
}

// --------------------------------------------------------------- incremental --
static void bench_naive() {
  std::printf("\n## 3. Incremental vs naive recompute-from-scratch features\n\n");
  std::printf("Same book, same stream; only the rolling-window implementation differs (Welford add/remove + exact integer sums vs two-pass recompute over the window every time it changes). No detectors, CAPPED 256. Throughput = messages/s through book + features.\n\n");
  std::printf("| window | touch-change samples in window (avg) | incremental msgs/s | naive msgs/s | **speedup** | incremental ns/msg | naive ns/msg |\n|---|---|---|---|---|---|---|\n");
  Timeline tl = build_timeline(steady_cfg(g_quick ? 20 : 60, 2500));
  const Ts wins[] = {100'000'000, 1'000'000'000, 10'000'000'000};
  for (Ts w : wins) {
    EngineConfig ec;
    ec.max_levels = 256;
    ec.feat.window_ns = w;
    ec.feat.ring_cap = 1u << 17;
    double ti = 0, tn = 0, avgn = 0;
    {
      Engine e(ec);
      e.apply_snapshot(tl.cps[0].view());
      ti = bulk_throughput(e, tl);
      avgn = double(e.features().n_window);
    }
    {
      NaiveEngine e(ec);
      e.apply_snapshot(tl.cps[0].view());
      tn = bulk_throughput(e, tl);
    }
    std::printf("| %.1f s | ~%.0f | %.2f M | %.3f M | **%.1fx** | %.0f | %.0f |\n", double(w) * 1e-9, avgn, ti / 1e6, tn / 1e6, ti / tn, 1e9 / ti, 1e9 / tn);
  }
}

// ------------------------------------------------------------------ stuffing --
static void bench_stuffing() {
  std::printf("\n## 4. Quote-stuffing stress on the hot path (before / after hardening)\n\n");
  std::printf("Attack streams: 1.0 s bursts at 100,000 msg/s (6 per minute) of add/cancel pairs.  DEEP = new worst-price level on a random side (forces a front-insert/erase in a flat sorted array); TOUCH = quantity flicker at the best price.  Background traffic: 2,000 msg/s regime-free churn.  Per-message service latency (ns), split by message class.\n\n");
  std::printf("| attack | engine | class | n | p50 | p90 | p99 | p99.9 | max |\n|---|---|---|---|---|---|---|---|---|\n");
  const char* kAttack[] = {"DEEP", "TOUCH"};
  for (int mode = 0; mode < 2; ++mode) {
    SynthConfig sc = steady_cfg(g_quick ? 30 : 120, 2000, 7);
    sc.stuff.bursts_per_min = 6; sc.stuff.len_s = 1.0; sc.stuff.rate = 100000; sc.stuff.mode = mode;
    Timeline tl = build_timeline(sc);
    const ModeCfg hardened[] = {{"BEFORE: FULL depth, no cap", 0}, {"AFTER: CAPPED 256", 256}, {"AFTER: TOP-32", 32}};
    for (const ModeCfg& m : hardened) {
      auto e = make_engine(tl, m.cap, true);
      std::vector<uint32_t> normal, stuff;
      replay(*e, tl, &normal, nullptr, &stuff);
      const Pct pn = pct_ns(normal), ps = pct_ns(stuff);
      std::printf("| %s | %s | stuffing msgs | %zu | %.0f | %.0f | %.0f | %.0f | %.0f |\n", kAttack[mode], m.name, stuff.size(), ps.p50, ps.p90, ps.p99, ps.p999, ps.max);
      std::printf("| %s | %s | background msgs | %zu | %.0f | %.0f | %.0f | %.0f | %.0f |\n", kAttack[mode], m.name, normal.size(), pn.p50, pn.p90, pn.p99, pn.p999, pn.max);
    }
  }
}

// --------------------------------------------------------------------- burst --
struct SimResult {
  const char* policy = "";
  uint64_t n = 0, dropped = 0, processed = 0, gaps = 0, resyncs = 0;
  size_t max_occ = 0;
  double p50 = 0, p99 = 0, p999 = 0, maxv = 0, legit_p99 = 0, degraded_frac = 0, stall_ms = 0;
};

// Virtual-time single-server queue with MEASURED service times (see header comment).
static SimResult simulate_queue(const Timeline& tl, const std::vector<std::string>& frames, size_t cap_engine, bool drop,
                                size_t qcap, const std::vector<double>& arrival_ns, double resync_rtt_ns) {
  EngineConfig ec;
  ec.max_levels = cap_engine;
  Engine eng(ec);
  add_detectors(eng);
  eng.set_parser(std::make_unique<BinanceDepthParser>());
  eng.apply_snapshot(tl.cps[0].view());
  const double npc = ns_per_cycle();
  const size_t n = frames.size();
  std::deque<double> fin;  // finish times of frames currently occupying the ring
  double last_finish = 0, prev_enq = 0, degraded = 0, deg_start = -1, stall = 0;
  bool pending = false;
  double ready_at = 0;
  std::vector<double> lat, lat_legit;
  lat.reserve(n);
  SimResult r;
  r.policy = drop ? "Drop newest" : "Block";
  r.n = n;
  SnapshotOwned snap;
  for (size_t k = 0; k < n; ++k) {
    const double A = arrival_ns[k];
    double E;
    if (drop) {
      while (!fin.empty() && fin.front() <= A) fin.pop_front();
      if (fin.size() >= qcap) { ++r.dropped; continue; }
      E = A;
    } else {
      const double want = std::max(A, prev_enq);
      double t = want;
      while (!fin.empty() && fin.front() <= t) fin.pop_front();
      while (fin.size() >= qcap) { t = fin.front(); fin.pop_front(); }
      E = t;
      stall += E - want;  // time the producer thread spent blocked on a full ring
    }
    prev_enq = E;
    const double start = std::max(E, last_finish);
    if (pending && start >= ready_at) {  // async REST snapshot arrived: state as of the last message that had arrived by then
      const size_t j = size_t(std::upper_bound(arrival_ns.begin(), arrival_ns.end(), ready_at) - arrival_ns.begin());
      const int64_t id = j == 0 ? 0 : tl.msgs[tl.order[j - 1]].last_id;
      tl.snapshot_for_id(id, snap);
      eng.apply_snapshot(snap.view());
      degraded += start - deg_start;
      pending = false;
    }
    const uint64_t c0 = cycles();
    eng.on_frame(frames[k].data(), frames[k].size(), 0);
    const double S = double(cycles() - c0) * npc;
    const double finish = start + S;
    last_finish = finish;
    fin.push_back(finish);
    r.max_occ = std::max(r.max_occ, fin.size());
    lat.push_back(finish - A);
    if (tl.msgs[tl.order[k]].tag == 0) lat_legit.push_back(finish - A);
    ++r.processed;
    if (!pending && eng.resync_needed()) { pending = true; ready_at = finish + resync_rtt_ns; deg_start = finish; }
  }
  auto q = [](std::vector<double>& v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, size_t(p * double(v.size())))];
  };
  r.p50 = q(lat, 0.5); r.p99 = q(lat, 0.99); r.p999 = q(lat, 0.999); r.maxv = lat.empty() ? 0 : lat.back();
  r.legit_p99 = q(lat_legit, 0.99);
  r.gaps = eng.stats().gaps; r.resyncs = eng.stats().resyncs;
  r.degraded_frac = degraded / std::max(1.0, arrival_ns.back());
  r.stall_ms = stall * 1e-6;
  return r;
}

static double mean_service_ns(const Timeline& tl, const std::vector<std::string>& frames, size_t cap) {
  EngineConfig c;
  c.max_levels = cap;
  Engine e(c);
  add_detectors(e);
  e.set_parser(std::make_unique<BinanceDepthParser>());
  e.apply_snapshot(tl.cps[0].view());
  SnapshotOwned s;
  double work = 0;
  for (size_t k = 0; k < frames.size(); k += 128) {
    const size_t end = std::min(frames.size(), k + 128);
    const double t0 = now_s();
    for (size_t j = k; j < end; ++j) e.on_frame(frames[j].data(), frames[j].size(), 0);
    work += now_s() - t0;
    if (e.resync_needed()) { tl.snapshot_for_id(e.reject_id(), s); e.apply_snapshot(s.view()); }
  }
  return work / double(frames.size()) * 1e9;
}

static void bench_burst() {
  std::printf("\n## 5. Burst load: bounded queue, backpressure vs drop (virtual-time queue, measured service times)\n\n");
  const size_t qcap = 1024;
  const double rtt = 50e6;
  const double dur = g_quick ? 30 : 120;
  // Arrival times are the stream's own EVENT times (1x real time, open loop).
  SynthConfig legit = steady_cfg(dur, 2000, 11);
  legit.act_burst_per_min = 12; legit.act_burst_len_s = 0.02; legit.act_burst_mult = 1500;  // 20 ms bursts at ~3 M msg/s (aggregate of many symbols on one core)
  SynthConfig attack = steady_cfg(dur, 2000, 11);
  attack.stuff.bursts_per_min = 6; attack.stuff.len_s = 1.0; attack.stuff.rate = 100000; attack.stuff.mode = 0;
  struct Scn { const char* name; Timeline tl; std::vector<std::string> frames; };
  std::vector<Scn> scns;
  scns.push_back({"LEGIT burst: 20 ms bursts at ~3 M msg/s, 12 per minute, no manipulation", build_timeline(legit), {}});
  scns.push_back({"ATTACK: 1 s deep quote-stuffing bursts at 100k msg/s, 6 per minute, over 2k msg/s background", build_timeline(attack), {}});
  for (auto& sc : scns) {
    sc.frames.resize(sc.tl.size());
    for (size_t i = 0; i < sc.tl.size(); ++i) format_binance_json(sc.tl.view(i), sc.frames[i]);
  }
  const double s_hard = mean_service_ns(scns[0].tl, scns[0].frames, 256);
  const double s_full = mean_service_ns(scns[1].tl, scns[1].frames, 0), s_hard_a = mean_service_ns(scns[1].tl, scns[1].frames, 256);
  std::printf("Mean service time (JSON parse + book + features + 3 detectors): CAPPED-256 on the legit stream %.0f ns/msg (%.2f M msg/s capacity); on the attack stream FULL-depth %.0f ns/msg (%.3f M msg/s) vs CAPPED-256 %.0f ns/msg (%.2f M msg/s).\n", s_hard, 1e3 / s_hard, s_full, 1e3 / s_full, s_hard_a, 1e3 / s_hard_a);
  std::printf("Open-loop arrivals at event-time pace; ring = %zu slots; resync latency modelled as 50 ms (async REST snapshot; frames arriving meanwhile are dropped as 'unsynced'). Latency is measured from the INTENDED arrival time.\n", qcap);
  for (auto& sc : scns) {
    std::vector<double> A(sc.tl.size());
    const Ts t0 = sc.tl.msgs.front().ts;
    for (size_t k = 0; k < sc.tl.size(); ++k) A[k] = double(sc.tl.msgs[sc.tl.order[k]].ts - t0);
    std::printf("\n### %s  (%zu messages over %.0f s)\n\n", sc.name, sc.tl.size(), A.back() * 1e-9);
    std::printf("| engine | policy | dropped | gaps -> resyncs | time with invalid book | max ring occupancy | e2e p50 | e2e p99 | e2e p99.9 | e2e max | e2e p99, background msgs only | producer stalled |\n|---|---|---|---|---|---|---|---|---|---|---|---|\n");
    const ModeCfg engines[] = {{"FULL depth (no cap)", 0}, {"CAPPED 256", 256}};
    for (const ModeCfg& m : engines) {
      for (int drop = 0; drop < 2; ++drop) {
        const SimResult r = simulate_queue(sc.tl, sc.frames, m.cap, drop, qcap, A, rtt);
        auto fmt = [](double ns) {
          static char b[8][32];
          static int i = 0;
          char* o = b[i++ & 7];
          if (ns < 1e6) std::snprintf(o, 32, "%.1f us", ns / 1e3);
          else if (ns < 1e9) std::snprintf(o, 32, "%.1f ms", ns / 1e6);
          else std::snprintf(o, 32, "%.2f s", ns / 1e9);
          return o;
        };
        std::printf("| %s | %s | %llu (%.1f%%) | %llu -> %llu | %.1f%% | %zu | %s | %s | %s | %s | %s | %.1f ms |\n", m.name, r.policy,
                    (unsigned long long)r.dropped, 100.0 * double(r.dropped) / double(r.n), (unsigned long long)r.gaps, (unsigned long long)r.resyncs,
                    100.0 * r.degraded_frac, r.max_occ, fmt(r.p50), fmt(r.p99), fmt(r.p999), fmt(r.maxv), fmt(r.legit_p99), r.stall_ms);
      }
    }
  }
}

// -------------------------------------------------------------------- memory --
static void bench_memory() {
  std::printf("\n## 6. Memory footprint: full depth vs top-of-book-only\n\n");
  Timeline tl = build_timeline(steady_cfg(g_quick ? 30 : 200, 2500));
  std::printf("Container bytes are exact (vector capacity x 16 B per level); \"worst-case bound\" is what a hostile feed can force. RSS is not reported: page granularity and allocator reuse make it meaningless at this scale.\n\n");
  std::printf("| mode | valid levels at end (bid+ask) | book bytes (steady) | book bytes (worst-case bound) | rolling-window ring | BOCPD arrays | p50 ns | p99 ns | refills |\n|---|---|---|---|---|---|---|---|---|\n");
  for (const ModeCfg& m : kModes) {
    auto e = make_engine(tl, m.cap, true);
    std::vector<uint32_t> cyc;
    replay(*e, tl, &cyc);
    const Pct p = pct_ns(cyc);
    BocpdDetector bo;
    char worst[48];
    if (m.cap) std::snprintf(worst, sizeof worst, "%.1f KB", double(2 * (m.cap + 1) * 16) / 1024.0); else std::snprintf(worst, sizeof worst, "unbounded");
    std::printf("| %s | %zu + %zu | %.1f KB | %s | %.0f KB | %.1f KB | %.0f | %.0f | %llu |\n", m.name, e->book().bids().size(), e->book().asks().size(),
                double(e->book().memory_bytes()) / 1024.0, worst, double(e->feature_engine().window().memory_bytes()) / 1024.0,
                double(bo.memory_bytes()) / 1024.0, p.p50, p.p99, (unsigned long long)e->stats().refills);
  }
}

// ------------------------------------------------------------------- threads --
static void bench_threads() {
  std::printf("\n## 7. Threaded SPSC pipeline smoke test (1 vCPU: throughput only, latency not meaningful)\n\n");
  Timeline tl = build_timeline(steady_cfg(g_quick ? 20 : 60, 2500, 3));
  std::vector<std::string> frames(tl.size());
  for (size_t i = 0; i < tl.size(); ++i) format_binance_json(tl.view(i), frames[i]);
  std::printf("| policy | submitted | processed | dropped | gaps -> resyncs | blocked spins | wall msgs/s |\n|---|---|---|---|---|---|---|\n");
  for (int drop = 0; drop < 2; ++drop) {
    EngineConfig ec;
    Engine eng(ec);
    eng.set_parser(std::make_unique<BinanceDepthParser>());
    eng.apply_snapshot(tl.cps[0].view());
    Pipeline pl(eng, 1024, drop ? OverflowPolicy::Drop : OverflowPolicy::Block, [&](int64_t id, SnapshotOwned& o) { return tl.snapshot_for_id(id, o); });
    pl.start();
    const double t0 = now_s();
    for (size_t i = 0; i < frames.size(); ++i) {
      pl.submit(frames[i].data(), frames[i].size(), mono_ns());
      if (drop && i % 2000 == 1999) std::this_thread::yield();
    }
    pl.drain();
    const double dt = now_s() - t0;
    pl.stop();
    std::printf("| %s | %zu | %llu | %llu | %llu -> %llu | %llu | %.2f M |\n", drop ? "Drop newest" : "Block", frames.size(), (unsigned long long)pl.processed(),
                (unsigned long long)pl.dropped(), (unsigned long long)eng.stats().gaps, (unsigned long long)eng.stats().resyncs, (unsigned long long)pl.blocked_spins(),
                double(frames.size()) / dt / 1e6);
  }
}

int main(int argc, char** argv) {
  std::string cmd = argc > 1 ? argv[1] : "all";
  for (int i = 2; i < argc; ++i) if (!std::strcmp(argv[i], "--quick")) g_quick = true;
  std::printf("# MicroPulse benchmark run\n\n");
  {
    std::ifstream f("/proc/cpuinfo");
    std::string l, model;
    while (std::getline(f, l)) if (l.rfind("model name", 0) == 0) { model = l.substr(l.find(':') + 2); break; }
    std::printf("Hardware: %s, %u logical CPU(s) visible; rdtscp ticks at %.3f GHz. Build: -O3 -std=c++17.%s\n", model.c_str(), std::thread::hardware_concurrency(), 1.0 / ns_per_cycle(), g_quick ? " (QUICK mode: shortened streams)" : "");
  }
  if (cmd == "parse" || cmd == "all") bench_parse();
  if (cmd == "core" || cmd == "all") bench_core();
  if (cmd == "naive" || cmd == "all") bench_naive();
  if (cmd == "stuffing" || cmd == "all") bench_stuffing();
  if (cmd == "burst" || cmd == "all") bench_burst();
  if (cmd == "memory" || cmd == "all") bench_memory();
  if (cmd == "threads" || cmd == "all") bench_threads();
  return 0;
}
