// Pluggable exchange-feed parsers.
//
// A new venue = one new FeedParser subclass that maps its wire format onto the
// venue-neutral DeltaView / SnapshotOwned. The book, features, detectors and engine do
// not change. Two implementations ship:
//   BinanceDepthParser : Binance spot/futures `depthUpdate` (+ combined-stream wrapper
//                        and REST /depth snapshot). Hand-written, allocation-free in the
//                        steady state, no floating point (decimal strings -> int64 1e-8).
//   CsvDeltaParser     : a trivial CSV L2 format, used for the NSE-style synthetic feed
//                        and to demonstrate/test pluggability.
#pragma once
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "types.hpp"

namespace mp {

class FeedParser {
 public:
  virtual ~FeedParser() = default;
  // On success `out` is valid until the next parse() call on this parser.
  virtual bool parse(const char* s, size_t n, DeltaView& out) = 0;
  virtual bool parse_snapshot(const char* s, size_t n, SnapshotOwned& out) = 0;
  virtual const char* name() const = 0;
};

namespace detail {

// "123.45000000" -> 12345000000. Digits beyond 1e-8 are truncated. Rejects anything else.
inline bool parse_fixed(const char* p, const char* e, int64_t& out) {
  int64_t ip = 0, frac = 0;
  int fd = 0;
  bool any = false;
  while (p < e && *p >= '0' && *p <= '9') {
    ip = ip * 10 + (*p - '0');
    if (ip > 90'000'000'000LL) return false;
    ++p;
    any = true;
  }
  if (p < e && *p == '.') {
    ++p;
    while (p < e && *p >= '0' && *p <= '9') {
      if (fd < 8) { frac = frac * 10 + (*p - '0'); ++fd; }
      ++p;
      any = true;
    }
  }
  if (p != e || !any) return false;
  while (fd < 8) { frac *= 10; ++fd; }
  out = ip * 100'000'000LL + frac;
  return true;
}

struct Cur {
  const char* p;
  const char* e;
  void ws() { while (p < e && (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r')) ++p; }
  bool eat(char c) { ws(); if (p < e && *p == c) { ++p; return true; } return false; }
  bool peek(char c) { ws(); return p < e && *p == c; }
};

inline bool jstr(Cur& c, const char*& s, size_t& n) {
  c.ws();
  if (!(c.p < c.e && *c.p == '"')) return false;
  ++c.p;
  s = c.p;
  while (c.p < c.e && *c.p != '"') {
    if (*c.p == '\\') ++c.p;
    ++c.p;
  }
  if (c.p >= c.e) return false;
  n = static_cast<size_t>(c.p - s);
  ++c.p;
  return true;
}

inline bool jint(Cur& c, int64_t& v) {
  c.ws();
  bool quoted = false;
  if (c.p < c.e && *c.p == '"') { quoted = true; ++c.p; }
  bool neg = false;
  if (c.p < c.e && *c.p == '-') { neg = true; ++c.p; }
  if (!(c.p < c.e && *c.p >= '0' && *c.p <= '9')) return false;
  int64_t x = 0;
  while (c.p < c.e && *c.p >= '0' && *c.p <= '9') { x = x * 10 + (*c.p - '0'); ++c.p; }
  if (quoted) { if (!(c.p < c.e && *c.p == '"')) return false; ++c.p; }
  v = neg ? -x : x;
  return true;
}

inline bool jskip(Cur& c) {
  c.ws();
  if (c.p >= c.e) return false;
  if (*c.p == '"') { const char* s; size_t n; return jstr(c, s, n); }
  if (*c.p == '{' || *c.p == '[') {
    int depth = 0;
    while (c.p < c.e) {
      const char ch = *c.p;
      if (ch == '"') { const char* s; size_t n; if (!jstr(c, s, n)) return false; continue; }
      if (ch == '{' || ch == '[') ++depth;
      else if (ch == '}' || ch == ']') { if (--depth == 0) { ++c.p; return true; } }
      ++c.p;
    }
    return false;
  }
  while (c.p < c.e && *c.p != ',' && *c.p != '}' && *c.p != ']') ++c.p;  // number / true / false / null
  return true;
}

// [["price","qty"],...] -> appended as LevelUpdate
inline bool jlevels(Cur& c, Side side, std::vector<LevelUpdate>& out) {
  if (!c.eat('[')) return false;
  if (c.eat(']')) return true;
  for (;;) {
    if (!c.eat('[')) return false;
    const char *ps, *qs;
    size_t pn, qn;
    if (!jstr(c, ps, pn) || !c.eat(',') || !jstr(c, qs, qn) || !c.eat(']')) return false;
    int64_t px, q;
    if (!parse_fixed(ps, ps + pn, px) || !parse_fixed(qs, qs + qn, q)) return false;
    out.push_back(LevelUpdate{px, q, side});
    if (c.eat(',')) continue;
    return c.eat(']');
  }
}

}  // namespace detail

class BinanceDepthParser final : public FeedParser {
 public:
  BinanceDepthParser() { lv_.reserve(1024); }

  bool parse(const char* s, size_t n, DeltaView& out) override {
    lv_.clear();
    Fields f;
    detail::Cur c{s, s + n};
    if (!obj(c, f, 0)) return false;
    if (!f.have_U || !f.have_u || f.u < f.U) return false;
    out.event_ts_ns = f.E * 1'000'000LL;  // exchange event time, ms -> ns
    out.first_id = f.U;
    out.last_id = f.u;
    out.prev_last_id = f.have_pu ? f.pu : -1;
    out.lv = lv_.data();
    out.n = static_cast<uint32_t>(lv_.size());
    return true;
  }

  // REST /api/v3/depth:  {"lastUpdateId":N,"bids":[["p","q"],...],"asks":[...]}
  bool parse_snapshot(const char* s, size_t n, SnapshotOwned& out) override {
    out.bids.clear();
    out.asks.clear();
    detail::Cur c{s, s + n};
    if (!c.eat('{')) return false;
    bool have_id = false;
    std::vector<LevelUpdate> tmp;
    for (;;) {
      const char* ks;
      size_t kn;
      if (!detail::jstr(c, ks, kn) || !c.eat(':')) return false;
      if (is(ks, kn, "lastUpdateId")) {
        if (!detail::jint(c, out.last_id)) return false;
        have_id = true;
      } else if (is(ks, kn, "bids") || is(ks, kn, "asks")) {
        const bool bid = ks[0] == 'b';
        tmp.clear();
        if (!detail::jlevels(c, bid ? Side::Bid : Side::Ask, tmp)) return false;
        auto& dst = bid ? out.bids : out.asks;
        for (const LevelUpdate& u : tmp) dst.push_back(Level{u.px, u.qty});
      } else if (!detail::jskip(c)) {
        return false;
      }
      if (c.eat(',')) continue;
      if (!c.eat('}')) return false;
      break;
    }
    return have_id;
  }
  const char* name() const override { return "binance-depth"; }

 private:
  struct Fields {
    int64_t E = 0, U = 0, u = 0, pu = 0;
    bool have_U = false, have_u = false, have_pu = false;
  };
  static bool is(const char* k, size_t n, const char* lit) { return n == std::strlen(lit) && std::memcmp(k, lit, n) == 0; }

  bool obj(detail::Cur& c, Fields& f, int depth) {
    if (!c.eat('{')) return false;
    if (c.eat('}')) return true;
    for (;;) {
      const char* ks;
      size_t kn;
      if (!detail::jstr(c, ks, kn) || !c.eat(':')) return false;
      bool ok;
      if (kn == 1 && ks[0] == 'E') ok = detail::jint(c, f.E);
      else if (kn == 1 && ks[0] == 'U') { ok = detail::jint(c, f.U); f.have_U = ok; }
      else if (kn == 1 && ks[0] == 'u') { ok = detail::jint(c, f.u); f.have_u = ok; }
      else if (kn == 2 && ks[0] == 'p' && ks[1] == 'u') { ok = detail::jint(c, f.pu); f.have_pu = ok; }
      else if (kn == 1 && ks[0] == 'b') ok = detail::jlevels(c, Side::Bid, lv_);
      else if (kn == 1 && ks[0] == 'a') ok = detail::jlevels(c, Side::Ask, lv_);
      else if (kn == 4 && depth == 0 && std::memcmp(ks, "data", 4) == 0) ok = obj(c, f, 1);  // combined stream
      else if (kn == 1 && ks[0] == 'e') {
        const char* vs;
        size_t vn;
        ok = detail::jstr(c, vs, vn) && is(vs, vn, "depthUpdate");  // reject other event types
      } else ok = detail::jskip(c);
      if (!ok) return false;
      if (c.eat(',')) continue;
      return c.eat('}');
    }
  }

  std::vector<LevelUpdate> lv_;
};

// CSV L2, one message per line:
//   D,<event_ts_ns>,<first_id>,<last_id>,<B|A>:<px>:<qty>;<B|A>:<px>:<qty>;...
//   S,<last_id>,<event_ts_ns>,<B|A>:<px>:<qty>;...            (snapshot)
class CsvDeltaParser final : public FeedParser {
 public:
  bool parse(const char* s, size_t n, DeltaView& out) override {
    lv_.clear();
    if (n < 2 || s[0] != 'D' || s[1] != ',') return false;
    const char* p = s + 2;
    const char* e = s + n;
    int64_t ts, a, b;
    if (!num(p, e, ts) || !num(p, e, a) || !num(p, e, b)) return false;
    if (!items(p, e, [&](Side sd, Price px, Qty q) { lv_.push_back(LevelUpdate{px, q, sd}); })) return false;
    out.event_ts_ns = ts; out.first_id = a; out.last_id = b; out.prev_last_id = -1;
    out.lv = lv_.data(); out.n = static_cast<uint32_t>(lv_.size());
    return b >= a;
  }
  bool parse_snapshot(const char* s, size_t n, SnapshotOwned& out) override {
    out.bids.clear(); out.asks.clear();
    if (n < 2 || s[0] != 'S' || s[1] != ',') return false;
    const char* p = s + 2;
    const char* e = s + n;
    if (!num(p, e, out.last_id) || !num(p, e, out.event_ts_ns)) return false;
    return items(p, e, [&](Side sd, Price px, Qty q) { (sd == Side::Bid ? out.bids : out.asks).push_back(Level{px, q}); });
  }
  const char* name() const override { return "csv-l2"; }

 private:
  static bool num(const char*& p, const char* e, int64_t& v) {
    char* end;
    v = std::strtoll(p, &end, 10);
    if (end == p || end >= e || *end != ',') return false;
    p = end + 1;
    return true;
  }
  template <class F>
  static bool items(const char* p, const char* e, F&& f) {
    while (p < e) {
      if ((*p != 'B' && *p != 'A') || p + 1 >= e || p[1] != ':') return false;
      const Side sd = *p == 'B' ? Side::Bid : Side::Ask;
      p += 2;
      const char* c1 = static_cast<const char*>(std::memchr(p, ':', size_t(e - p)));
      if (!c1) return false;
      const char* end = static_cast<const char*>(std::memchr(c1, ';', size_t(e - c1)));
      if (!end) end = e;
      int64_t px, q;
      if (!detail::parse_fixed(p, c1, px) || !detail::parse_fixed(c1 + 1, end, q)) return false;
      f(sd, px, q);
      p = end < e ? end + 1 : e;
    }
    return true;
  }
  std::vector<LevelUpdate> lv_;
};

}  // namespace mp
