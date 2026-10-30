// Python bindings (pybind11). The hot path stays in C++; Python receives columnar numpy
// arrays (features / bars / alarms) for calibration and analysis with pandas / sklearn.
#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <fstream>

#include "micropulse/engine.hpp"
#include "micropulse/synth.hpp"

namespace py = pybind11;
using namespace mp;

namespace {

template <class T>
py::array_t<T> to_np(const std::vector<T>& v) {
  py::array_t<T> a(static_cast<py::ssize_t>(v.size()));
  if (!v.empty()) std::memcpy(a.mutable_data(), v.data(), v.size() * sizeof(T));
  return a;
}

template <class T, class F>
py::array_t<T> col(size_t n, F&& f) {
  py::array_t<T> a(static_cast<py::ssize_t>(n));
  T* p = a.mutable_data();
  for (size_t i = 0; i < n; ++i) p[i] = f(i);
  return a;
}

struct PyEngine {
  Engine eng;
  explicit PyEngine(const EngineConfig& c) : eng(c) {}
};

EngineConfig make_cfg(size_t max_levels, double window_s, double bar_ms, int depth_levels, const std::string& flow_stat,
                      bool record_features, bool record_bars, double tick, double ofi_cap_mult) {
  EngineConfig c;
  c.max_levels = max_levels;
  c.feat.window_ns = static_cast<Ts>(window_s * 1e9);
  c.feat.bar_ns = static_cast<Ts>(bar_ms * 1e6);
  c.feat.depth_levels = depth_levels;
  c.feat.ofi_cap_mult = ofi_cap_mult;
  c.flow_stat = flow_stat == "raw" ? FlowStat::RawSum : FlowStat::TStat;
  c.record_features = record_features;
  c.record_bars = record_bars;
  c.tick = static_cast<Price>(tick * kFixedScale + 0.5);
  return c;
}

py::dict features_dict(const Features& f) {
  py::dict d;
  d["ts_ns"] = f.ts; d["seq"] = f.seq; d["mid"] = f.mid; d["micro"] = f.micro; d["micro_dev_bps"] = f.micro_dev_bps;
  d["spread_bps"] = f.spread_bps; d["imb_touch"] = f.imb_touch; d["imb_depth"] = f.imb_depth; d["ofi"] = f.ofi;
  d["ofi_norm"] = f.ofi_norm; d["rv"] = f.rv; d["ret_std"] = f.ret_std; d["n_window"] = f.n_window; d["flags"] = f.flags;
  d["vol_score"] = f.vol_score; d["flow_score"] = f.flow_score; d["vol_alarm"] = int(f.vol_alarm); d["flow_alarm"] = int(f.flow_alarm);
  return d;
}

SynthConfig synth_from_dict(const py::dict& d) {
  SynthConfig c;
  auto get = [&](const char* k, auto def) { return d.contains(k) ? py::cast<decltype(def)>(d[k]) : def; };
  c.seed = get("seed", uint64_t(1));
  c.duration_s = get("duration_s", 600.0);
  c.base_rate = get("base_rate", 1000.0);
  c.fv_rate = get("fv_rate", 40.0);
  c.mid0 = get("mid0", 60000.0);
  c.sigma_mod_amp = get("sigma_mod_amp", 0.0);
  c.sigma_mod_period_s = get("sigma_mod_period_s", 600.0);
  c.act_burst_per_min = get("act_burst_per_min", 0.0);
  c.act_burst_mult = get("act_burst_mult", 3.0);
  c.act_burst_len_s = get("act_burst_len_s", 5.0);
  c.spoof.per_min = get("spoof_per_min", 0.0);
  c.spoof.depth_ticks = get("spoof_depth_ticks", 0);
  c.spoof.qty_mult = get("spoof_qty_mult", 50.0);
  c.spoof.hold_s = get("spoof_hold_s", 1.0);
  c.spoof.layers = get("spoof_layers", 1);
  c.stuff.bursts_per_min = get("stuff_per_min", 0.0);
  c.stuff.len_s = get("stuff_len_s", 0.5);
  c.stuff.rate = get("stuff_rate", 100000.0);
  c.stuff.mode = get("stuff_mode", 0);
  if (d.contains("regimes")) {
    for (auto item : d["regimes"].cast<py::list>()) {
      auto t = item.cast<py::tuple>();
      c.regimes.push_back(Regime{t[0].cast<double>(), t[1].cast<double>(), t[2].cast<double>()});
    }
  }
  return c;
}

}  // namespace

PYBIND11_MODULE(_micropulse, m) {
  m.doc() = "MicroPulse: C++ limit-order-book reconstruction and real-time feature engine";
  m.attr("FLAG_WARMUP") = int(F_WARMUP);
  m.attr("FLAG_GAP_RECENT") = int(F_GAP_RECENT);
  m.attr("FLAG_WINDOW_TAINTED") = int(F_WINDOW_TAINTED);
  m.attr("FLAG_BOOK_STALE") = int(F_BOOK_STALE);
  m.attr("FLAG_TRUNCATED") = int(F_TRUNCATED);
  m.attr("FLAG_RING_OVERFLOW") = int(F_RING_OVERFLOW);
  m.attr("FLAG_TS_REGRESSION") = int(F_TS_REGRESSION);
  m.attr("FLAG_BAR_SKIP") = int(F_BAR_SKIP);

  py::class_<PyEngine>(m, "Engine")
      .def(py::init([](size_t max_levels, double window_s, double bar_ms, int depth_levels, const std::string& flow_stat,
                       bool record_features, bool record_bars, double tick, double ofi_cap_mult, const std::string& parser) {
             auto e = std::make_unique<PyEngine>(make_cfg(max_levels, window_s, bar_ms, depth_levels, flow_stat, record_features, record_bars, tick, ofi_cap_mult));
             if (parser == "csv") e->eng.set_parser(std::make_unique<CsvDeltaParser>());
             else e->eng.set_parser(std::make_unique<BinanceDepthParser>());
             return e;
           }),
           py::arg("max_levels") = 256, py::arg("window_s") = 1.0, py::arg("bar_ms") = 250.0, py::arg("depth_levels") = 10,
           py::arg("flow_stat") = "tstat", py::arg("record_features") = false, py::arg("record_bars") = true,
           py::arg("tick") = 0.01, py::arg("ofi_cap_mult") = 8.0, py::arg("parser") = "binance")
      .def("add_cusum", [](PyEngine& e, const std::string& ch, double k, double h, int warmup, int rebase) {
             CusumConfig c; c.k = k; c.h = h; c.warmup = warmup; c.rebase = rebase;
             return e.eng.add_detector(ch == "flow" ? Channel::Flow : Channel::Vol, std::make_unique<CusumDetector>(c));
           }, py::arg("channel") = "vol", py::arg("k") = 0.5, py::arg("h") = 17.0, py::arg("warmup") = 120, py::arg("rebase") = 60)
      .def("add_bocpd", [](PyEngine& e, const std::string& ch, double hazard_lambda, int rmax, int k_short, double thr, int warmup, double zclip) {
             BocpdConfig c; c.hazard_lambda = hazard_lambda; c.rmax = rmax; c.k_short = k_short; c.thr = thr; c.warmup = warmup; c.zclip = zclip;
             return e.eng.add_detector(ch == "flow" ? Channel::Flow : Channel::Vol, std::make_unique<BocpdDetector>(c));
           }, py::arg("channel") = "vol", py::arg("hazard_lambda") = 1000.0, py::arg("rmax") = 200, py::arg("k_short") = 80,
           py::arg("thr") = 0.999, py::arg("warmup") = 40, py::arg("zclip") = 2.5)
      .def("apply_snapshot_json", [](PyEngine& e, const std::string& s) {
        SnapshotOwned snap;
        if (!e.eng.parser()->parse_snapshot(s.data(), s.size(), snap)) throw std::runtime_error("snapshot parse error");
        e.eng.apply_snapshot(snap.view());
      })
      .def("on_frame", [](PyEngine& e, const py::bytes& b) {
        std::string_view v = b;
        return static_cast<int>(e.eng.on_frame(v.data(), v.size(), 0));
      })
      .def("resync_needed", [](PyEngine& e) { return e.eng.resync_needed(); })
      .def("refill_requested", [](PyEngine& e) { return e.eng.refill_requested(); })
      .def("reject_id", [](PyEngine& e) { return e.eng.reject_id(); })
      .def("features", [](PyEngine& e) { return features_dict(e.eng.features()); })
      .def("stats", [](PyEngine& e) {
        const EngineStats& s = e.eng.stats();
        py::dict d;
        d["msgs"] = s.msgs; d["applied"] = s.applied; d["stale"] = s.stale; d["gaps"] = s.gaps; d["crossed"] = s.crossed;
        d["oversize"] = s.oversize; d["parse_errors"] = s.parse_errors; d["dropped_unsynced"] = s.dropped_unsynced;
        d["resyncs"] = s.resyncs; d["refills"] = s.refills; d["ignored_deep"] = s.ignored_deep; d["phantom_deletes"] = s.phantom_deletes;
        d["level_updates"] = s.level_updates; d["bars"] = s.bars; d["bars_skipped_tainted"] = s.bars_skipped_tainted;
        d["alarms_vol"] = s.alarms[0]; d["alarms_flow"] = s.alarms[1];
        return d;
      })
      .def("features_table", [](PyEngine& e) {
        const auto& r = e.eng.recorded_features();
        const size_t n = r.size();
        py::dict d;
        d["ts_ns"] = col<int64_t>(n, [&](size_t i) { return r[i].ts; });
        d["mid"] = col<double>(n, [&](size_t i) { return r[i].mid; });
        d["micro"] = col<double>(n, [&](size_t i) { return r[i].micro; });
        d["micro_dev_bps"] = col<double>(n, [&](size_t i) { return r[i].micro_dev_bps; });
        d["spread_bps"] = col<double>(n, [&](size_t i) { return r[i].spread_bps; });
        d["imb_touch"] = col<double>(n, [&](size_t i) { return r[i].imb_touch; });
        d["imb_depth"] = col<double>(n, [&](size_t i) { return r[i].imb_depth; });
        d["ofi"] = col<double>(n, [&](size_t i) { return r[i].ofi; });
        d["ofi_norm"] = col<double>(n, [&](size_t i) { return r[i].ofi_norm; });
        d["rv"] = col<double>(n, [&](size_t i) { return r[i].rv; });
        d["ret_std"] = col<double>(n, [&](size_t i) { return r[i].ret_std; });
        d["n_window"] = col<int64_t>(n, [&](size_t i) { return r[i].n_window; });
        d["flags"] = col<uint32_t>(n, [&](size_t i) { return r[i].flags; });
        return d;
      })
      .def("bars", [](PyEngine& e) {
        const auto& r = e.eng.recorded_bars();
        const size_t n = r.size();
        py::dict d;
        d["t_end_ns"] = col<int64_t>(n, [&](size_t i) { return r[i].t_end; });
        d["rv"] = col<double>(n, [&](size_t i) { return r[i].rv; });
        d["ofi"] = col<double>(n, [&](size_t i) { return r[i].ofi; });
        d["ofi_sq"] = col<double>(n, [&](size_t i) { return r[i].ofi_sq; });
        d["n"] = col<int64_t>(n, [&](size_t i) { return r[i].n; });
        d["flags"] = col<uint32_t>(n, [&](size_t i) { return r[i].flags; });
        d["x_vol"] = col<double>(n, [&](size_t i) { return r[i].x_vol; });
        d["x_flow"] = col<double>(n, [&](size_t i) { return r[i].x_flow; });
        for (int s = 0; s < 4; ++s) {
          d[("score" + std::to_string(s)).c_str()] = col<double>(n, [&](size_t i) { return r[i].score[s]; });
          d[("alarm" + std::to_string(s)).c_str()] = col<uint8_t>(n, [&](size_t i) { return r[i].alarm[s]; });
        }
        return d;
      })
      .def("alarms", [](PyEngine& e) {
        const auto& r = e.eng.alarms();
        const size_t n = r.size();
        py::dict d;
        d["ts_ns"] = col<int64_t>(n, [&](size_t i) { return r[i].ts; });
        d["slot"] = col<int32_t>(n, [&](size_t i) { return r[i].slot; });
        d["channel"] = col<int32_t>(n, [&](size_t i) { return r[i].channel; });
        d["score"] = col<double>(n, [&](size_t i) { return r[i].score; });
        d["dir"] = col<int32_t>(n, [&](size_t i) { return r[i].dir; });
        return d;
      })
      .def("clear_records", [](PyEngine& e) { e.eng.clear_records(); })
      .def("book_levels", [](PyEngine& e) { return std::make_pair(e.eng.book().bids().size(), e.eng.book().asks().size()); })
      .def("memory_bytes", [](PyEngine& e) { return e.eng.memory_bytes(); })
      // Replay a capture written by python/micropulse_research/record_binance.py
      //   "S <rest depth json>"   snapshot line;   "<recv_ns> <depthUpdate json>"  frame line
      .def("replay_file", [](PyEngine& e, const std::string& path, const std::string& snapshot_provider_note) {
        std::ifstream f(path);
        if (!f) throw std::runtime_error("cannot open " + path);
        std::string line;
        SnapshotOwned snap;
        size_t n = 0;
        std::string last_snapshot;
        while (std::getline(f, line)) {
          if (line.size() > 2 && line[0] == 'S' && line[1] == ' ') {
            if (e.eng.parser()->parse_snapshot(line.data() + 2, line.size() - 2, snap)) { e.eng.apply_snapshot(snap.view()); last_snapshot = line; }
            continue;
          }
          const size_t sp = line.find(' ');
          if (sp == std::string::npos) continue;
          e.eng.on_frame(line.data() + sp + 1, line.size() - sp - 1, 0);
          ++n;
        }
        return n;
      }, py::arg("path"), py::arg("snapshot_provider_note") = "")
      ;

  // Run the synthetic feed through an Engine entirely in C++ (fast path for calibration).
  m.def("simulate", [](const py::dict& cfg, PyEngine& e) {
    SynthConfig sc = synth_from_dict(cfg);
    SynthFeed feed(sc);
    e.eng.apply_snapshot(feed.initial_snapshot().view());
    DeltaView d;
    uint64_t n = 0;
    {
      py::gil_scoped_release rel;
      SnapshotOwned snap;
      while (feed.next(d)) {
        e.eng.on_delta(d);
        ++n;
        if (e.eng.resync_needed() || e.eng.refill_requested()) {  // serve snapshots from the generator's exact truth (instant REST)
          snap.last_id = d.last_id;
          snap.event_ts_ns = d.event_ts_ns;
          feed.export_levels(snap.bids, snap.asks);
          e.eng.apply_snapshot(snap.view());
        }
      }
    }
    py::dict out;
    out["messages"] = n;
    out["change_ts_ns"] = to_np(feed.change_ts());
    auto windows = [](const std::vector<std::pair<Ts, Ts>>& w) {
      py::array_t<int64_t> a({static_cast<py::ssize_t>(w.size()), py::ssize_t(2)});
      auto r = a.mutable_unchecked<2>();
      for (size_t i = 0; i < w.size(); ++i) { r(i, 0) = w[i].first; r(i, 1) = w[i].second; }
      return a;
    };
    out["spoof_windows_ns"] = windows(feed.spoof_windows());
    out["stuff_windows_ns"] = windows(feed.stuff_windows());
    out["start_ns"] = SynthFeed::kStartNs;
    return out;
  });

  // Replay a bar-level series through a detector (calibration sweeps without re-simulating).
  m.def("run_cusum", [](py::array_t<double, py::array::c_style | py::array::forcecast> x, double k, double h, int warmup, int rebase) {
    CusumConfig c; c.k = k; c.h = h; c.warmup = warmup; c.rebase = rebase;
    CusumDetector d(c);
    const size_t n = static_cast<size_t>(x.size());
    std::vector<double> score(n);
    std::vector<uint8_t> alarm(n);
    for (size_t i = 0; i < n; ++i) { const DetectorOutput o = d.update(Ts(i), x.data()[i]); score[i] = o.score; alarm[i] = o.alarm; }
    return py::make_tuple(to_np(score), to_np(alarm));
  }, py::arg("x"), py::arg("k") = 0.5, py::arg("h") = 17.0, py::arg("warmup") = 120, py::arg("rebase") = 60);
  m.def("run_bocpd", [](py::array_t<double, py::array::c_style | py::array::forcecast> x, double hazard_lambda, int rmax, int k_short, double thr, int warmup, double zclip) {
    BocpdConfig c; c.hazard_lambda = hazard_lambda; c.rmax = rmax; c.k_short = k_short; c.thr = thr; c.warmup = warmup; c.zclip = zclip;
    BocpdDetector d(c);
    const size_t n = static_cast<size_t>(x.size());
    std::vector<double> score(n);
    std::vector<uint8_t> alarm(n);
    for (size_t i = 0; i < n; ++i) { const DetectorOutput o = d.update(Ts(i), x.data()[i]); score[i] = o.score; alarm[i] = o.alarm; }
    return py::make_tuple(to_np(score), to_np(alarm));
  }, py::arg("x"), py::arg("hazard_lambda") = 1000.0, py::arg("rmax") = 200, py::arg("k_short") = 80, py::arg("thr") = 0.999, py::arg("warmup") = 40, py::arg("zclip") = 2.5);
}
