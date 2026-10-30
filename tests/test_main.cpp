// Dependency-free test runner. Each TEST prints PASS/FAIL plus the numbers worth quoting.
#include <cassert>
#include <cstdio>
#include <functional>
#include <map>
#include <random>
#include <thread>

#include "micropulse/engine.hpp"
#include "micropulse/pipeline.hpp"
#include "micropulse/synth.hpp"

using namespace mp;

static int g_fail = 0, g_run = 0;
#define CHECK(cond)                                                                   \
  do {                                                                                \
    if (!(cond)) {                                                                    \
      std::printf("    CHECK FAILED %s:%d  %s\n", __FILE__, __LINE__, #cond);        \
      ++g_fail;                                                                       \
      return;                                                                         \
    }                                                                                 \
  } while (0)
#define RUN(fn)                                  \
  do {                                           \
    ++g_run;                                     \
    const int before = g_fail;                   \
    std::printf("[ RUN  ] %s\n", #fn);           \
    fn();                                        \
    std::printf("[ %s ] %s\n", g_fail == before ? " OK " : "FAIL", #fn); \
  } while (0)

static Price P(double x) { return static_cast<Price>(std::llround(x * 1e8)); }

// ---------------------------------------------------------------- Welford ----
static void test_welford_stability() {
  // data with |mean| >> std: the textbook E[x^2]-E[x]^2 formula destroys the variance.
  const int N = 200000;
  std::mt19937_64 g(7);
  std::normal_distribution<double> nd(0.0, 1e-3);
  std::vector<double> x(N);
  for (auto& v : x) v = 1.0e9 + nd(g);
  long double s = 0, s2 = 0;
  for (double v : x) { s += v; s2 += (long double)v * v; }
  long double mean = s / N, ref = 0;
  for (double v : x) ref += ((long double)v - mean) * ((long double)v - mean);
  ref /= (N - 1);  // two-pass in long double = reference
  double ns = 0, ns2 = 0;
  for (double v : x) { ns += v; ns2 += v * v; }
  const double naive = (ns2 - ns * ns / N) / (N - 1);
  Welford w;
  for (double v : x) w.add(v);
  const double rel_w = std::fabs(w.var() - (double)ref) / (double)ref;
  const double rel_n = std::fabs(naive - (double)ref) / (double)ref;
  std::printf("    ref var=%.6e  welford rel.err=%.2e  naive rel.err=%.2e (naive=%.3e)\n", (double)ref, rel_w, rel_n, naive);
  // |mean|/std = 1e12 here: double precision itself bounds ANY one-pass method (~1e-5); the
  // naive formula has no correct digits at all (error > 1e12 relative).
  CHECK(rel_w < 1e-3);
  CHECK(rel_n > 1e3);
}

static void test_sliding_welford_drift() {
  // 5M add/remove steps on a 1000-sample window with a large offset; compare to exact.
  for (uint64_t refresh : {uint64_t(0), uint64_t(65536)}) {
    RollingWindow rw(1'000'000'000, 4096, refresh);
    std::mt19937_64 g(11);
    std::normal_distribution<double> nd(1e3, 1.0);
    double max_rel = 0;
    const int N = 5'000'000;
    Ts t = 0;
    for (int i = 0; i < N; ++i) {
      t += 1'000'000;  // 1 ms -> window holds 1000 samples
      rw.push(t, nd(g), 0);
      if (i % 250000 == 249999) {
        const Welford ex = rw.exact_welford();
        max_rel = std::max(max_rel, std::fabs(rw.welford().m2 - ex.m2) / ex.m2);
      }
    }
    std::printf("    5M steps, refresh=%-6llu : max relative M2 error vs exact recompute = %.2e\n",
                (unsigned long long)refresh, max_rel);
    CHECK(max_rel < 1e-6);
  }
}

// ------------------------------------------------------------------- Book ----
static void test_book_vs_reference() {
  std::mt19937_64 g(3);
  for (size_t cap : {size_t(0), size_t(32), size_t(128)}) {
    OrderBook book(cap);
    std::map<Price, Qty, std::greater<Price>> tb;  // true bids
    std::map<Price, Qty> ta;                       // true asks
    // seed identical state via snapshot
    std::vector<Level> b0, a0;
    for (int i = 0; i < 300; ++i) { b0.push_back({P(100 - i * 0.01), 10 + i}); a0.push_back({P(100.01 + i * 0.01), 10 + i}); tb[b0.back().px] = b0.back().qty; ta[a0.back().px] = a0.back().qty; }
    SnapshotOwned s; s.bids = b0; s.asks = a0;
    book.load(s.view());
    size_t checks = 0;
    for (int it = 0; it < 400000; ++it) {
      const bool bid = g() & 1;
      const int64_t off = (g() % 100 < 90) ? int64_t(g() % 20) : int64_t(g() % 400);
      const Price px = bid ? P(100 - off * 0.01) : P(100.01 + off * 0.01);
      const Qty q = (g() % 3 == 0) ? 0 : Qty(1 + g() % 1000);
      // keep the true book uncrossed & non-empty on each side
      if (q == 0 && (bid ? tb.size() <= 40 : ta.size() <= 40)) continue;
      book.apply(LevelUpdate{px, q, bid ? Side::Bid : Side::Ask});
      if (bid) { if (q) tb[px] = q; else tb.erase(px); } else { if (q) ta[px] = q; else ta.erase(px); }
      if (it % 997 == 0) {
        ++checks;
        // invariant: retained levels == true top-n (n = retained size)
        auto bi = tb.begin();
        for (size_t i = 0; i < book.bids().size(); ++i, ++bi) { CHECK(bi != tb.end()); CHECK(book.bids().at(i).px == bi->first && book.bids().at(i).qty == bi->second); }
        auto ai = ta.begin();
        for (size_t i = 0; i < book.asks().size(); ++i, ++ai) { CHECK(ai != ta.end()); CHECK(book.asks().at(i).px == ai->first && book.asks().at(i).qty == ai->second); }
        if (cap == 0) { CHECK(book.bids().size() == tb.size()); CHECK(book.asks().size() == ta.size()); }
        else { CHECK(book.bids().size() <= cap); }
      }
    }
    std::printf("    cap=%-4zu: %zu invariant checks passed (retained == true top-n)\n", cap, checks);
  }
}

// -------------------------------------------------------------------- OFI ----
static void test_ofi_hand_computed() {
  FeatureConfig fc;
  fc.ofi_cap_mult = 0;  // no winsorisation for the hand calculation
  fc.window_ns = 10'000'000'000LL;
  FeatureEngine fe(fc);
  OrderBook bk(0);
  auto set = [&](double bp, double bq, double ap, double aq) {
    // reset book to a one-level-per-side state
    bk.clear();
    bk.apply({P(bp), P(bq), Side::Bid});
    bk.apply({P(ap), P(aq), Side::Ask});
  };
  Ts t = 1'000'000'000;
  set(100.00, 10, 100.01, 10); CHECK(fe.on_update(t, bk, 1));             // seed
  set(100.00, 15, 100.01, 10); fe.on_update(t += 1000, bk, 2);            // bid qty +5            -> e=+5
  set(100.00, 15, 100.02, 7);  fe.on_update(t += 1000, bk, 3);            // ask price up, qa'=10  -> e=+10
  set(99.99, 4, 100.02, 7);    fe.on_update(t += 1000, bk, 4);            // bid price down, qb'=15 -> e=-15
  set(99.99, 4, 100.01, 3);    fe.on_update(t += 1000, bk, 5);            // ask price down, qa=3  -> e=-3
  const Features f = fe.current();
  std::printf("    OFI over 4 touch changes = %.6f (expected 5+10-15-3 = -3)\n", f.ofi);
  CHECK(std::fabs(f.ofi - (-3.0)) < 1e-9);
  // micro-price: (bid*askQty + ask*bidQty)/(bidQty+askQty)
  const double mp = (99.99 * 3 + 100.01 * 4) / 7.0;
  CHECK(std::fabs(f.micro - mp) < 1e-9);
}

// ---------------------------------------------------------------- Parsers ----
static void test_parsers() {
  BinanceDepthParser p;
  DeltaView d;
  const std::string spot = R"({"e":"depthUpdate","E":1672515782136,"s":"BNBBTC","U":157,"u":160,"b":[["0.0024","10"],["0.0023","0.00000000"]],"a":[["0.0026","100.5"]]})";
  CHECK(p.parse(spot.data(), spot.size(), d));
  CHECK(d.first_id == 157 && d.last_id == 160 && d.prev_last_id == -1 && d.n == 3);
  CHECK(d.event_ts_ns == 1672515782136LL * 1000000LL);
  CHECK(d.lv[0].side == Side::Bid && d.lv[0].px == 240000 && d.lv[0].qty == 1000000000LL);
  CHECK(d.lv[1].qty == 0 && d.lv[2].side == Side::Ask && d.lv[2].qty == 10050000000LL);
  const std::string fut = R"({"e":"depthUpdate","E":1,"T":2,"s":"BTCUSDT","U":10,"u":12,"pu":9,"b":[],"a":[["60000.10","1.5"]]})";
  CHECK(p.parse(fut.data(), fut.size(), d) && d.prev_last_id == 9 && d.n == 1);
  const std::string comb = R"({"stream":"btcusdt@depth@100ms","data":{"e":"depthUpdate","E":5,"s":"BTCUSDT","U":1,"u":1,"b":[["1","2"]],"a":[]}})";
  CHECK(p.parse(comb.data(), comb.size(), d) && d.n == 1 && d.lv[0].px == 100000000LL);
  const std::string reordered = R"({ "a" : [ ["2","1"] ] , "u":9, "U" : 8 , "E": 3, "e":"depthUpdate", "b":[["1","1"]] })";
  CHECK(p.parse(reordered.data(), reordered.size(), d) && d.n == 2 && d.first_id == 8);
  // malformed / wrong event type / missing ids must be rejected
  for (const char* bad : {"{", "", R"({"e":"trade","E":1,"U":1,"u":1})", R"({"e":"depthUpdate","E":1,"b":[]})",
                          R"({"e":"depthUpdate","E":1,"U":5,"u":3})", R"({"e":"depthUpdate","E":1,"U":1,"u":1,"b":[["x","1"]]})",
                          R"({"e":"depthUpdate","E":1,"U":1,"u":1,"b":[["1"]]})"})
    CHECK(!p.parse(bad, std::strlen(bad), d));
  SnapshotOwned s;
  const std::string snap = R"({"lastUpdateId":1027024,"bids":[["4.00000000","431.00000000"],["3.99"," 12"]],"asks":[["4.00000200","12.00000000"]]})";
  // second bid has a space in qty => malformed => reject
  CHECK(!p.parse_snapshot(snap.data(), snap.size(), s));
  const std::string snap2 = R"({"lastUpdateId":1027024,"bids":[["4.00000000","431.00000000"],["3.99","12"]],"asks":[["4.00000200","12.00000000"]]})";
  CHECK(p.parse_snapshot(snap2.data(), snap2.size(), s) && s.last_id == 1027024 && s.bids.size() == 2 && s.asks.size() == 1 && s.asks[0].px == 400000200LL);
  int64_t v;
  CHECK(detail::parse_fixed("0.00000001", "0.00000001" + 10, v) && v == 1);
  CHECK(detail::parse_fixed("100", "100" + 3, v) && v == 10000000000LL);
  CHECK(!detail::parse_fixed("1e-8", "1e-8" + 4, v));
  CHECK(!detail::parse_fixed("", "", v));
}

static void test_pluggable_parser_equivalence() {
  // Same synthetic stream through two different wire formats / parsers => identical features.
  SynthConfig sc; sc.seed = 5; sc.duration_s = 20; sc.base_rate = 800;
  Timeline tl = build_timeline(sc);
  EngineConfig ec; ec.max_levels = 0; ec.record_features = true;
  Engine a(ec), b(ec);
  a.set_parser(std::make_unique<BinanceDepthParser>());
  b.set_parser(std::make_unique<CsvDeltaParser>());
  a.apply_snapshot(tl.cps[0].view()); b.apply_snapshot(tl.cps[0].view());
  std::string s;
  size_t n_ok = 0;
  for (size_t i = 0; i < tl.size(); ++i) {
    const DeltaView d = tl.view(i);
    format_binance_json(d, s); CHECK(a.on_frame(s.data(), s.size(), 0) != Status::ParseError);
    format_csv_delta(d, s);   CHECK(b.on_frame(s.data(), s.size(), 0) != Status::ParseError);
    ++n_ok;
  }
  // Binance E is millisecond-resolution, CSV carries ns: compare book-derived (ts-independent) fields
  const auto& fa = a.recorded_features(); const auto& fb = b.recorded_features();
  CHECK(fa.size() == fb.size() && !fa.empty());
  size_t mism = 0;
  for (size_t i = 0; i < fa.size(); ++i) if (fa[i].mid != fb[i].mid || fa[i].micro != fb[i].micro || fa[i].imb_depth != fb[i].imb_depth) ++mism;
  std::printf("    %zu messages, %zu feature rows, mid/micro/depth-imbalance mismatches between parsers: %zu\n", n_ok, fa.size(), mism);
  CHECK(mism == 0);
}

// ------------------------------------------------- sequence-gap / resync ----
static void test_sequence_handling() {
  SynthConfig sc; sc.seed = 9; sc.duration_s = 60; sc.base_rate = 1500;
  Timeline tl = build_timeline(sc);
  Impair im; im.p_drop = 0.002; im.p_dup = 0.002; im.p_swap = 0.002;
  apply_impairments(tl, im, 77);
  for (size_t cap : {size_t(0), size_t(64)}) {
    EngineConfig ec; ec.max_levels = cap; ec.record_features = true;
    Engine eng(ec);
    eng.apply_snapshot(tl.cps[0].view());
    SnapshotOwned snap;
    size_t checks = 0, bad = 0, ok_msgs = 0;
    int64_t last_applied = -1;
    bool seen_tainted = false;
    for (size_t k = 0; k < tl.order.size(); ++k) {
      const size_t i = tl.order[k];
      const Status st = eng.on_delta(tl.view(i));
      if (st == Status::Ok) { ++ok_msgs; last_applied = (int64_t)i; }
      if (eng.resync_needed()) { tl.snapshot_for_id(eng.reject_id(), snap); eng.apply_snapshot(snap.view()); last_applied = -2; }
      if (eng.refill_requested()) { tl.snapshot_for_id(tl.msgs[i].last_id, snap); eng.apply_snapshot(snap.view()); }
      if (st == Status::Ok && (eng.features().flags & F_WINDOW_TAINTED)) seen_tainted = true;
      // book must equal truth at the last message it has consumed
      if (st == Status::Ok && last_applied >= 0 && k % 53 == 0) {
        SnapshotOwned truth;
        tl.snapshot_after((int64_t)i, truth);
        ++checks;
        // compare top-min(size) levels (capped book holds the true top-n)
        const BookSide& bs = eng.book().bids();
        std::vector<Level> tb = truth.bids;  // worst->best
        for (size_t j = 0; j < bs.size(); ++j) {
          const Level& t = tb[tb.size() - 1 - j];
          if (bs.at(j).px != t.px || bs.at(j).qty != t.qty) { ++bad; break; }
        }
      }
    }
    const EngineStats& s = eng.stats();
    std::printf("    cap=%-3zu msgs=%llu applied=%llu stale=%llu gaps=%llu resyncs=%llu refills=%llu book-vs-truth checks=%zu bad=%zu tainted-seen=%d\n",
                cap, (unsigned long long)s.msgs, (unsigned long long)s.applied, (unsigned long long)s.stale, (unsigned long long)s.gaps,
                (unsigned long long)s.resyncs, (unsigned long long)s.refills, checks, bad, (int)seen_tainted);
    CHECK(bad == 0);
    CHECK(s.gaps > 0 && s.stale > 0);
    CHECK(s.resyncs == s.gaps);
    CHECK(seen_tainted);
  }
}

static void test_gap_flags_and_taint_expiry() {
  SynthConfig sc; sc.seed = 21; sc.duration_s = 30; sc.base_rate = 1000;
  Timeline tl = build_timeline(sc);
  EngineConfig ec; ec.max_levels = 0; ec.record_features = true;
  Engine eng(ec);
  eng.apply_snapshot(tl.cps[0].view());
  SnapshotOwned snap;
  const size_t gap_at = tl.size() / 3;
  Ts gap_ts = 0;
  for (size_t i = 0; i < tl.size(); ++i) {
    if (i == gap_at) { gap_ts = tl.msgs[i].ts; continue; }  // drop one message
    const Status st = eng.on_delta(tl.view(i));
    if (eng.resync_needed()) { tl.snapshot_for_id(eng.reject_id(), snap); eng.apply_snapshot(snap.view()); }
    (void)st;
  }
  CHECK(eng.stats().gaps == 1);
  // rows within window_ns after the gap must be flagged; rows after must be clean
  size_t flagged = 0, clean_after = 0, bad_clean = 0, bad_flag = 0;
  const Ts W = ec.feat.window_ns;
  gap_ts = tl.msgs[gap_at + 1].ts;  // the gap is DETECTED when the next message arrives
  for (const Features& f : eng.recorded_features()) {
    if (f.ts <= gap_ts) continue;
    const bool tainted = (f.flags & F_WINDOW_TAINTED) != 0;
    if (f.ts < gap_ts + W) { if (tainted) ++flagged; else ++bad_flag; }
    else if (f.ts > gap_ts + W + 1'000'000) { ++clean_after; if (tainted) ++bad_clean; }
  }
  std::printf("    gap at msg %zu: flagged rows in window=%zu (unflagged=%zu), clean rows after=%zu (wrongly flagged=%zu); bars skipped=%llu\n",
              gap_at, flagged, bad_flag, clean_after, bad_clean, (unsigned long long)eng.stats().bars_skipped_tainted);
  CHECK(flagged > 0 && bad_flag == 0 && bad_clean == 0 && clean_after > 0);
  CHECK(eng.stats().bars_skipped_tainted >= 1);
}

static void test_crossed_and_oversize() {
  EngineConfig ec; ec.max_levels = 0; ec.max_levels_per_msg = 3;
  Engine eng(ec);
  SnapshotOwned s;
  s.last_id = 10; s.event_ts_ns = 1'000'000'000;
  s.bids = {{P(100), P(1)}}; s.asks = {{P(101), P(1)}};
  eng.apply_snapshot(s.view());
  LevelUpdate crossing[] = {{P(102), P(1), Side::Bid}};
  DeltaView d; d.event_ts_ns = 2'000'000'000; d.first_id = 11; d.last_id = 11; d.lv = crossing; d.n = 1;
  CHECK(eng.on_delta(d) == Status::Crossed);
  CHECK(eng.resync_needed());
  s.last_id = 11; eng.apply_snapshot(s.view());
  LevelUpdate big[4] = {{P(99), P(1), Side::Bid}, {P(98), P(1), Side::Bid}, {P(97), P(1), Side::Bid}, {P(96), P(1), Side::Bid}};
  d.first_id = 12; d.last_id = 15; d.lv = big; d.n = 4;
  CHECK(eng.on_delta(d) == Status::Oversize);
  CHECK(eng.stats().crossed == 1 && eng.stats().oversize == 1);
}

static void test_ts_regression_clamped() {
  EngineConfig ec; ec.max_levels = 0; ec.record_features = true;
  Engine eng(ec);
  SnapshotOwned s; s.last_id = 0; s.event_ts_ns = 1'000'000'000;
  s.bids = {{P(100), P(1)}}; s.asks = {{P(101), P(1)}};
  eng.apply_snapshot(s.view());
  LevelUpdate u1[] = {{P(100), P(2), Side::Bid}}, u2[] = {{P(100), P(3), Side::Bid}};
  DeltaView d; d.event_ts_ns = 5'000'000'000; d.first_id = d.last_id = 1; d.lv = u1; d.n = 1;
  CHECK(eng.on_delta(d) == Status::Ok);
  d.event_ts_ns = 4'000'000'000; d.first_id = d.last_id = 2; d.lv = u2;  // exchange clock went backwards
  CHECK(eng.on_delta(d) == Status::Ok);
  CHECK(eng.features().flags & F_TS_REGRESSION);
}

static void test_depth_refill() {
  // A capped book keeps the TRUE top levels; when deletions shrink it below the required depth
  // the engine asks for a refill instead of silently serving a thin/wrong book.
  EngineConfig ec; ec.max_levels = 16;  // feature depth 10 -> min_valid_levels 12
  Engine eng(ec);
  SnapshotOwned s; s.last_id = 0; s.event_ts_ns = 1'000'000'000;
  for (int i = 0; i < 40; ++i) { s.bids.push_back({P(100.00 - i * 0.01), P(1 + i)}); s.asks.push_back({P(100.01 + i * 0.01), P(1 + i)}); }
  eng.apply_snapshot(s.view());
  CHECK(eng.book().bids().size() == 16 && eng.book().bids().truncated());
  int64_t id = 0;
  size_t requested_at = 0;
  for (int i = 0; i < 10; ++i) {  // cancel the best bid, one level at a time
    LevelUpdate u[] = {{P(100.00 - i * 0.01), 0, Side::Bid}};
    DeltaView d; d.event_ts_ns = 2'000'000'000 + i * 1000; d.first_id = d.last_id = ++id; d.lv = u; d.n = 1;
    CHECK(eng.on_delta(d) == Status::Ok);
    CHECK(eng.book().bids().best().px == P(100.00 - (i + 1) * 0.01));  // still the true best
    if (!requested_at && eng.refill_requested()) requested_at = eng.book().bids().size();
  }
  std::printf("    refill requested when retained depth fell to %zu levels (min_valid=12)\n", requested_at);
  CHECK(requested_at == 11);
  SnapshotOwned t = s; t.last_id = id;
  t.bids.erase(t.bids.begin(), t.bids.begin() + 10);  // truth after 10 cancels
  eng.apply_snapshot(t.view());
  CHECK(!eng.refill_requested() && eng.stats().refills == 1 && eng.book().bids().size() == 16 && eng.stats().resyncs == 0);
}

// ------------------------------------------------------------------- SPSC ----
static void test_spsc_ring() {
  SpscRing<Frame> ring(256);
  const uint64_t N = 300000;
  std::thread prod([&] {
    for (uint64_t i = 0; i < N;) {
      Frame* f = ring.try_acquire();
      if (!f) { std::this_thread::yield(); continue; }
      f->len = 8; std::memcpy(f->data, &i, 8); f->recv_ns = (int64_t)i;
      ring.publish(); ++i;
    }
  });
  uint64_t expect = 0, bad = 0;
  while (expect < N) {
    Frame* f = ring.front();
    if (!f) { std::this_thread::yield(); continue; }
    uint64_t v; std::memcpy(&v, f->data, 8);
    if (v != expect || f->recv_ns != (int64_t)expect) ++bad;
    ring.pop(); ++expect;
  }
  prod.join();
  std::printf("    %llu frames through a 256-slot SPSC ring, out-of-order/corrupt: %llu\n", (unsigned long long)N, (unsigned long long)bad);
  CHECK(bad == 0);
}

static void test_pipeline_threaded_drop_and_resync() {
  SynthConfig sc; sc.seed = 4; sc.duration_s = 40; sc.base_rate = 2000;
  Timeline tl = build_timeline(sc);
  std::vector<std::string> frames(tl.size());
  for (size_t i = 0; i < tl.size(); ++i) format_binance_json(tl.view(i), frames[i]);
  EngineConfig ec; ec.max_levels = 256;
  Engine eng(ec);
  eng.set_parser(std::make_unique<BinanceDepthParser>());
  eng.apply_snapshot(tl.cps[0].view());
  Pipeline pl(eng, 64, OverflowPolicy::Drop, [&](int64_t id, SnapshotOwned& o) { return tl.snapshot_for_id(id, o); });
  pl.start();
  for (size_t i = 0; i < frames.size(); ++i) {  // unpaced bursts of 200 frames into a 64-slot ring: forces drops
    pl.submit(frames[i].data(), frames[i].size(), mono_ns());
    if (i % 200 == 199) std::this_thread::yield();
  }
  pl.drain();
  pl.stop();
  std::printf("    submitted=%zu processed=%llu dropped=%llu gaps=%llu resyncs=%llu final-synced=%d\n", frames.size(),
              (unsigned long long)pl.processed(), (unsigned long long)pl.dropped(), (unsigned long long)eng.stats().gaps,
              (unsigned long long)eng.stats().resyncs, (int)eng.synced());
  CHECK(pl.processed() + pl.dropped() == frames.size());
  if (pl.dropped() > 0) CHECK(eng.stats().gaps > 0);
  CHECK(eng.stats().resyncs == eng.stats().gaps + eng.stats().crossed + eng.stats().oversize);
}

// -------------------------------------------------------------- detectors ----
static void test_detectors_shift_and_null() {
  std::mt19937_64 g(5);
  std::normal_distribution<double> nd(0, 1);
  auto run = [&](Detector& d, int n_pre, int n_post, double shift, double sd_post, int& delay, int& false_alarms) {
    d.reset();
    delay = -1; false_alarms = 0;
    for (int i = 0; i < n_pre; ++i) if (d.update(i, -28.0 + 0.4 * nd(g)).alarm) ++false_alarms;
    for (int i = 0; i < n_post; ++i) {
      if (d.update(n_pre + i, -28.0 + shift + 0.4 * sd_post * nd(g)).alarm && delay < 0) delay = i;
    }
  };
  CusumConfig cc; cc.h = 8.0;
  BocpdConfig bc; bc.hazard_lambda = 240; bc.k_short = 20; bc.thr = 0.9; bc.zclip = 4.0;
  CusumDetector cu(cc); BocpdDetector bo(bc);
  int delay, fa;
  run(cu, 3000, 100, 0.8, 1.0, delay, fa);  // +2 sd mean shift
  std::printf("    CUSUM : +2sd shift detected after %d bars; false alarms in 3000 null bars = %d\n", delay, fa);
  CHECK(delay >= 0 && delay <= 20); CHECK(fa <= 4);
  run(bo, 3000, 100, 0.8, 1.0, delay, fa);
  std::printf("    BOCPD : +2sd shift detected after %d bars; false alarms in 3000 null bars = %d\n", delay, fa);
  CHECK(delay >= 0 && delay <= 20); CHECK(fa <= 3);
  run(bo, 1000, 100, 0.0, 3.0, delay, fa);  // volatility-only change, 3x sd (variance x9)
  std::printf("    BOCPD : 3x sd (variance x9) detected after %d bars\n", delay);
  CHECK(delay >= 0 && delay <= 60);
}

// ----------------------------------------------- incremental == naive -------
static void test_incremental_equals_naive() {
  SynthConfig sc; sc.seed = 13; sc.duration_s = 60; sc.base_rate = 2000;
  Timeline tl = build_timeline(sc);
  EngineConfig ec; ec.max_levels = 0; ec.record_features = true;
  Engine a(ec); NaiveEngine b(ec);
  a.apply_snapshot(tl.cps[0].view()); b.apply_snapshot(tl.cps[0].view());
  for (size_t i = 0; i < tl.size(); ++i) { a.on_delta(tl.view(i)); b.on_delta(tl.view(i)); }
  const auto& fa = a.recorded_features(); const auto& fb = b.recorded_features();
  CHECK(fa.size() == fb.size() && fa.size() > 1000);
  double max_rv = 0, max_std = 0; size_t ofi_bad = 0;
  for (size_t i = 0; i < fa.size(); ++i) {
    if (fa[i].ofi != fb[i].ofi || fa[i].n_window != fb[i].n_window) ++ofi_bad;
    if (fb[i].rv > 0) max_rv = std::max(max_rv, std::fabs(fa[i].rv - fb[i].rv) / fb[i].rv);
    if (fb[i].ret_std > 0) max_std = std::max(max_std, std::fabs(fa[i].ret_std - fb[i].ret_std) / fb[i].ret_std);
  }
  std::printf("    %zu rows: OFI/count mismatches=%zu, max rel.err rv=%.2e ret_std=%.2e (incremental vs recompute)\n", fa.size(), ofi_bad, max_rv, max_std);
  CHECK(ofi_bad == 0); CHECK(max_rv < 1e-9); CHECK(max_std < 1e-5);
}

int main() {
  RUN(test_welford_stability);
  RUN(test_sliding_welford_drift);
  RUN(test_book_vs_reference);
  RUN(test_ofi_hand_computed);
  RUN(test_parsers);
  RUN(test_pluggable_parser_equivalence);
  RUN(test_sequence_handling);
  RUN(test_gap_flags_and_taint_expiry);
  RUN(test_crossed_and_oversize);
  RUN(test_ts_regression_clamped);
  RUN(test_depth_refill);
  RUN(test_spsc_ring);
  RUN(test_pipeline_threaded_drop_and_resync);
  RUN(test_detectors_shift_and_null);
  RUN(test_incremental_equals_naive);
  std::printf("\n%d tests, %d failed checks\n", g_run, g_fail);
  return g_fail ? 1 : 0;
}
