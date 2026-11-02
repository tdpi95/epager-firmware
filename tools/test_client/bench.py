"""Throughput and latency benchmark (input for docs/hw-checklist.md item 1 when run over BLE)."""
from __future__ import annotations

import asyncio
import time

from .client import EpagerClient, status_name
from .flows import ASSET, FONT, FONT_V1, asset, pattern


async def bench(c: EpagerClient, pings: int = 20, upload_bytes: int = 64 * 1024) -> dict:
    out: dict = {"mtu": c.link.mtu, "segment_bytes": c.segment_max}
    lat = []
    for _ in range(pings):
        t0 = time.perf_counter()
        await c.request("PING", {})
        lat.append((time.perf_counter() - t0) * 1000)
    lat.sort()
    out["ping_ms"] = {"min": round(lat[0], 2), "median": round(lat[len(lat) // 2], 2), "max": round(lat[-1], 2)}

    payload = pattern(500).hex()
    t0 = time.perf_counter()
    for _ in range(pings):
        await c.request("PING", {"payload": payload})
    dt = time.perf_counter() - t0
    out["ping_500B_round_trip_Bps"] = round(pings * 500 * 2 / dt)

    # Four requests in flight, each way carrying 480 bytes: the link's throughput without the asset sink
    # (a board without asset storage, before M6, cannot take the upload below).
    window, rounds = 4, max(1, pings * 2 // 4)
    payload = pattern(480).hex()
    t0 = time.perf_counter()
    for _ in range(rounds):
        await asyncio.gather(*(c.request("PING", {"payload": payload}) for _ in range(window)))
    dt = time.perf_counter() - t0
    out["ping_stream_Bps_each_way"] = round(rounds * window * 480 / dt)

    data = asset(FONT, FONT_V1, pattern(max(0, upload_bytes - 16), 9))
    t0 = time.perf_counter()
    try:
        r = await c.upload(ASSET, "bench.tmp", data, asset_type=FONT)
    except Exception as e:  # noqa: BLE001 (a board without asset storage refuses the BEGIN)
        out["upload"] = {"unavailable": f"{type(e).__name__}: {e}"}
        return out
    dt = time.perf_counter() - t0
    out["upload"] = {"bytes": len(data), "seconds": round(dt, 3), "Bps": round(len(data) / dt),
                     "max_chunk": r.begin["max_chunk"], "ack_window": r.begin["ack_window"],
                     "finish": status_name(r.finish)}
    await c.request("ASSET_DELETE", {"asset_type": FONT, "id": "bench.tmp"}, char=4, want=None)
    return out
