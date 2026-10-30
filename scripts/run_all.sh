#!/bin/sh
# Reproduce every number in reports/: build, test, benchmark, calibrate.
set -e
cd "$(dirname "$0")/.."
make build/test_main build/bench_main python
./build/test_main
./build/bench_main all > reports/benchmark_results.md
( cd python && python3 -m micropulse_research.calibrate 8 && mv reports/calibration_results.json ../reports/ )
python3 python/micropulse_research/report.py reports/calibration_results.json > reports/calibration_results.md
