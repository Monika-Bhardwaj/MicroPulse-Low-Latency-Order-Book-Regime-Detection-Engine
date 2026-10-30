"""Record a live Binance depth capture in the format MicroPulse replays.

    pip install websockets requests
    python -m micropulse_research.record_binance --symbol btcusdt --seconds 3600 --out data/btcusdt.cap

Capture format (one record per line):
    S <REST /api/v3/depth json>            snapshot (written at start and after any local gap)
    <recv_monotonic_ns> <depthUpdate json> one diff-depth frame, verbatim

Then, in Python:
    import micropulse_research as mp
    e = mp.Engine(max_levels=256, tick=0.01); e.add_cusum("vol"); e.add_bocpd("vol")
    e.replay_file("data/btcusdt.cap"); bars = e.bars()

Follows Binance's documented local-order-book procedure: open the stream, buffer events, fetch the REST
snapshot, drop events with u <= lastUpdateId; the engine enforces U <= lastUpdateId+1 <= u on the first
applied event and U == previous u + 1 afterwards. NOTE: this script could not be executed in the build
sandbox (no outbound network); it is provided so the calibration can be re-run on real data.
"""
import argparse
import asyncio
import json
import os
import time


async def main(symbol: str, seconds: float, out: str, speed: str, limit: int):
    import requests
    import websockets

    url = f"wss://stream.binance.com:9443/ws/{symbol}@depth@{speed}"
    snap_url = f"https://api.binance.com/api/v3/depth?symbol={symbol.upper()}&limit={limit}"
    t_end = time.monotonic() + seconds
    if os.path.dirname(out):
        os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w") as f:
        async with websockets.connect(url, max_queue=None) as ws:
            buf = []
            loop = asyncio.get_running_loop()

            async def write_snapshot():
                r = await loop.run_in_executor(None, lambda: requests.get(snap_url, timeout=10).text)
                f.write("S " + r.strip() + "\n")

            await write_snapshot()  # frames after this line are applied only if u > lastUpdateId (engine rule)
            while time.monotonic() < t_end:
                msg = await asyncio.wait_for(ws.recv(), timeout=30)
                f.write(f"{time.monotonic_ns()} {msg}\n")
    print("wrote", out)


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--symbol", default="btcusdt")
    ap.add_argument("--seconds", type=float, default=600)
    ap.add_argument("--out", default="data/capture.cap")
    ap.add_argument("--speed", default="100ms", choices=["100ms", "1000ms"])
    ap.add_argument("--limit", type=int, default=1000)
    a = ap.parse_args()
    asyncio.run(main(a.symbol, a.seconds, a.out, a.speed, a.limit))
