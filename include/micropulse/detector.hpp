// Pluggable online change detectors.
//
// Contract: update() is called once per BAR (event-time aggregation done by the
// feature engine) with one scalar observation. Ingestion, book and features never see
// which detector is attached. Swapping CUSUM for BOCPD is a one-line change at
// Engine::add_detector().
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

#include "types.hpp"
#include "window.hpp"

namespace mp {

struct DetectorOutput {
  double score = 0.0;  // CUSUM: max(S+,S-) in sd units; BOCPD: P(run length <= k_short)
  bool alarm = false;
  int dir = 0;         // +1 upward shift, -1 downward (CUSUM only), 0 unknown
};

class Detector {
 public:
  virtual ~Detector() = default;
  virtual DetectorOutput update(Ts ts, double x) = 0;
  virtual void reset() = 0;
  virtual const char* name() const = 0;
};

// ---------------------------------------------------------------------------
// Two-sided standardised CUSUM (Page 1954). Baseline mean/variance are learned over
// `warmup` bars, then tracked with a slow EWMA only while the statistic is in-control,
// so a persistent shift cannot be absorbed into the baseline before it alarms.
// Observations are clipped at +-zclip sd so one wild bar cannot alarm on its own.
// After an alarm the statistic resets and the baseline is re-learned for `rebase` bars
// (a documented blind period).
// ---------------------------------------------------------------------------
struct CusumConfig {
  double k = 0.5;          // reference value (sd units); optimal for a ~1-sd shift
  double h = 17.0;         // decision threshold (sd units) -- calibrated, see reports/CALIBRATION.md (textbook h=5 gives ~70 false alarms/hour on this data)
  int warmup = 120;  // baseline error ~ 1/sqrt(warmup) sd; too short a warm-up erodes the effective k
  int rebase = 60;
  double ewma_alpha = 1.0 / 300.0;
  double adapt_frac = 0.25;  // adapt baseline only while max(S) < adapt_frac * h
  double zclip = 6.0;
};

class CusumDetector final : public Detector {
 public:
  explicit CusumDetector(CusumConfig c = {}) : c_(c) { reset(); }

  DetectorOutput update(Ts, double x) override {
    if (n_warm_ < need_) {
      w_.add(x);
      if (++n_warm_ == need_) {
        mu_ = w_.mean;
        var_ = std::max(w_.var(), 1e-8);
      }
      return {};
    }
    const double sd = std::sqrt(var_);
    double z = (x - mu_) / sd;
    z = std::max(-c_.zclip, std::min(c_.zclip, z));
    shi_ = std::max(0.0, shi_ + z - c_.k);
    slo_ = std::max(0.0, slo_ - z - c_.k);
    const double s = std::max(shi_, slo_);
    DetectorOutput o;
    o.score = s;
    o.alarm = s >= c_.h;
    o.dir = shi_ >= slo_ ? +1 : -1;
    if (o.alarm) {
      shi_ = slo_ = 0.0;
      w_.reset();
      n_warm_ = 0;
      need_ = c_.rebase;
    } else if (s < c_.adapt_frac * c_.h) {
      const double d = x - mu_;
      mu_ += c_.ewma_alpha * d;
      var_ = std::max((1.0 - c_.ewma_alpha) * (var_ + c_.ewma_alpha * d * d), 1e-8);
    }
    return o;
  }
  void reset() override {
    w_.reset();
    n_warm_ = 0;
    need_ = c_.warmup;
    shi_ = slo_ = 0.0;
    mu_ = 0.0;
    var_ = 1.0;
  }
  const char* name() const override { return "cusum"; }
  const CusumConfig& config() const { return c_; }

 private:
  CusumConfig c_;
  Welford w_;
  int n_warm_ = 0, need_ = 0;
  double shi_ = 0, slo_ = 0, mu_ = 0, var_ = 1;
};

// ---------------------------------------------------------------------------
// Bayesian online changepoint detection (Adams & MacKay 2007).
//   * Observation model: Gaussian with unknown mean and variance, Normal-Inverse-Gamma
//     conjugate prior => Student-t posterior predictive, closed-form sufficient-stat updates.
//   * Constant hazard H = 1/hazard_lambda.
//   * Run-length posterior truncated to rmax+1 hypotheses (O(rmax) time/memory per bar,
//     bounded); mass beyond rmax is dropped and renormalised, i.e. each hypothesis uses
//     at most the last rmax bars. All arithmetic in log space.
//   * Prior mean/scale come from a warm-up sample and then track slow EWMAs, so the
//     detector needs no hand-set units.
//   * Output statistic: P(r_t <= k_short | x_1..t). Under constant hazard P(r_t = 0) is
//     identically H (uninformative), so mass on SHORT run lengths is the usable signal.
//   * Alarm = rising edge through `thr`; re-armed once the statistic falls below `rearm`.
// ---------------------------------------------------------------------------
struct BocpdConfig {
  double hazard_lambda = 1000.0; // expected run length in bars (calibrated)
  int rmax = 200;
  int warmup = 40;
  int k_short = 80;              // calibrated; short windows cannot see subtle shifts (see reports)
  double thr = 0.999;            // alarm threshold -- calibrated, see reports
  double rearm = 0.2;
  double kappa0 = 1.0;
  double alpha0 = 2.0;
  double prior_alpha = 1.0 / 600.0;  // EWMA rate for prior mean/variance
  double zclip = 2.5;            // clip observations at +-zclip prior-sd (0 = off): one outlier bar must not
                                 // spawn a short-run hypothesis that lingers in P(r<=k) for k bars
};

class BocpdDetector final : public Detector {
 public:
  explicit BocpdDetector(BocpdConfig c = {}) : c_(c) {
    const size_t m = static_cast<size_t>(c_.rmax) + 2;
    lp_.assign(m, 0.0); nlp_.assign(m, 0.0); w_.assign(m, 0.0);
    mu_.assign(m, 0.0); nmu_.assign(m, 0.0); beta_.assign(m, 0.0); nbeta_.assign(m, 0.0);
    lgc_.assign(m, 0.0);
    for (size_t r = 0; r < m; ++r) {
      const double a = c_.alpha0 + 0.5 * double(r);
      lgc_[r] = std::lgamma(a + 0.5) - std::lgamma(a) - 0.5 * std::log(2.0 * a * M_PI);
    }
    log_h_ = std::log(1.0 / c_.hazard_lambda);
    log_1mh_ = std::log1p(-1.0 / c_.hazard_lambda);
    reset();
  }

  DetectorOutput update(Ts, double x) override {
    if (n_warm_ < c_.warmup) {
      w0_.add(x);
      if (++n_warm_ == c_.warmup) {
        mu0_ = w0_.mean;
        var0_ = std::max(w0_.var(), 1e-8);
        init_posterior();
      }
      return {};
    }
    if (c_.zclip > 0) {
      const double lim = c_.zclip * std::sqrt(var0_);
      x = std::max(mu0_ - lim, std::min(mu0_ + lim, x));
    }
    const int R = R_;
    // --- predictive log-prob of x under each run-length hypothesis (Student-t) ---
    double M = -std::numeric_limits<double>::infinity();
    for (int r = 0; r < R; ++r) {
      const double kappa = c_.kappa0 + r;
      const double alpha = c_.alpha0 + 0.5 * r;
      const double scale2 = beta_[r] * (kappa + 1.0) / (alpha * kappa);
      const double d = x - mu_[r];
      const double lpi = lgc_[r] - 0.5 * std::log(scale2) - (alpha + 0.5) * std::log1p(d * d / (2.0 * alpha * scale2));
      w_[r] = lp_[r] + lpi;
      M = std::max(M, w_[r]);
    }
    double S = 0.0;
    for (int r = 0; r < R; ++r) S += std::exp(w_[r] - M);
    const double lse = M + std::log(S);

    // --- growth and changepoint probabilities ---
    const int newR = std::min(R + 1, c_.rmax + 1);
    nlp_[0] = lse + log_h_;
    for (int r = 0; r + 1 < newR; ++r) nlp_[r + 1] = w_[r] + log_1mh_;
    // --- sufficient statistics ---
    nmu_[0] = mu0_;
    nbeta_[0] = var0_ * (c_.alpha0 - 1.0);  // E[sigma^2] = beta/(alpha-1) = var0
    for (int r = 0; r + 1 < newR; ++r) {
      const double kappa = c_.kappa0 + r;
      const double d = x - mu_[r];
      nmu_[r + 1] = (kappa * mu_[r] + x) / (kappa + 1.0);
      nbeta_[r + 1] = beta_[r] + kappa * d * d / (2.0 * (kappa + 1.0));
    }
    // --- normalise ---
    double M2 = -std::numeric_limits<double>::infinity();
    for (int r = 0; r < newR; ++r) M2 = std::max(M2, nlp_[r]);
    double S2 = 0.0;
    for (int r = 0; r < newR; ++r) S2 += std::exp(nlp_[r] - M2);
    const double norm = M2 + std::log(S2);
    for (int r = 0; r < newR; ++r) nlp_[r] -= norm;
    lp_.swap(nlp_); mu_.swap(nmu_); beta_.swap(nbeta_);
    R_ = newR;

    double cp = 0.0;
    const int kmax = std::min(c_.k_short, R_ - 1);
    for (int r = 0; r <= kmax; ++r) cp += std::exp(lp_[r]);

    // slowly adapt the prior to the recent typical regime
    {
      const double d = x - mu0_;
      mu0_ += c_.prior_alpha * d;
      var0_ = std::max((1.0 - c_.prior_alpha) * (var0_ + c_.prior_alpha * d * d), 1e-8);
    }

    DetectorOutput o;
    o.score = cp;
    if (armed_ && cp >= c_.thr) {
      o.alarm = true;
      armed_ = false;
    } else if (!armed_ && cp < c_.rearm) {
      armed_ = true;
    }
    return o;
  }

  void reset() override {
    w0_.reset();
    n_warm_ = 0;
    R_ = 1;
    armed_ = false;  // the initial posterior is all mass at r=0: stay disarmed until it relaxes below `rearm`
    mu0_ = 0.0;
    var0_ = 1.0;
  }
  const char* name() const override { return "bocpd"; }
  const BocpdConfig& config() const { return c_; }
  size_t memory_bytes() const { return 8 * sizeof(double) * lp_.capacity(); }  // 8 working arrays

 private:
  void init_posterior() {
    R_ = 1;
    lp_[0] = 0.0;
    mu_[0] = mu0_;
    beta_[0] = var0_ * (c_.alpha0 - 1.0);
  }
  BocpdConfig c_;
  Welford w0_;
  int n_warm_ = 0, R_ = 1;
  bool armed_ = true;
  double mu0_ = 0, var0_ = 1, log_h_ = 0, log_1mh_ = 0;
  std::vector<double> lp_, nlp_, w_, mu_, nmu_, beta_, nbeta_, lgc_;
};

}  // namespace mp
