"""The Python reference must agree with the contract vectors and with real Unicode NFC."""
from __future__ import annotations

import json
import subprocess
import sys
import unicodedata
import zlib
from pathlib import Path

import pytest

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
from epager_proto import ancs, assets, codec, config, segments, text, transfer  # noqa: E402
from epager_proto import registry as R  # noqa: E402

REG = R.load()
VEC = R.CONTRACTS / "test-vectors"


def load(name: str) -> dict:
    return json.loads((VEC / name).read_text())


@pytest.mark.parametrize("case", load("messages.json")["cases"], ids=lambda c: c["name"])
def test_message_vectors(case):
    b = bytes.fromhex(case["hex"])
    try:
        got, status = codec.decode_message(REG, b), "OK"
    except codec.ProtoError as e:
        got, status = None, e.status
    assert status == case["expect"]
    if "logical" in case:
        assert got == case["logical"]
        if not case.get("decode_only"):
            assert codec.encode_message(REG, case["logical"]) == b


def test_segment_vectors():
    v = load("segments.json")
    for c in v["split"]:
        assert [s.hex() for s in segments.split(bytes.fromhex(c["message"]), c["max_segment"])] == c["segments"]
    for c in v["reassemble"]:
        r = segments.Reassembler(c["max_message"])
        ev = []
        for s in c["segments"]:
            for kind, val in r.feed(bytes.fromhex(s)):
                ev.append({"message": val.hex()} if kind == "message" else {"error": val})
        assert ev == c["events"], c["name"]


def test_config_vectors():
    for c in load("config_values.json")["cases"]:
        assert config.validate(REG, c["key"], bytes.fromhex(c["value_hex"]), c["variant"]) == c["expect"], c["name"]


def test_text_vectors():
    for c in load("text.json")["cases"]:
        assert text.normalize(REG, bytes.fromhex(c["input_hex"]), c.get("budget")) == c["output"], c["name"]


def test_text_matches_nfc_for_every_vietnamese_letter():
    """Every precomposed letter in the renderable set, decomposed and in reversed mark order,
    composes back to itself (what real NFC gives)."""
    r = text.rules(REG)
    checked = 0
    for a, b in r.ranges:
        for cp in range(a, b + 1):
            ch = chr(cp)
            nfd = unicodedata.normalize("NFD", ch)
            if len(nfd) < 2 or not unicodedata.category(ch).startswith("L"):
                continue
            base, marks = nfd[0], nfd[1:]
            for variant in (nfd, base + marks[::-1]):
                if unicodedata.normalize("NFC", variant) == ch:
                    assert text.normalize(REG, variant.encode()) == ch, (hex(cp), variant)
                    checked += 1
    assert checked > 400


def test_ancs_vectors():
    v = load("ancs.json")
    for c in v["notification_source"]:
        if c["expect"] == "OK":
            assert ancs.parse_notification_source(bytes.fromhex(c["hex"])) == c["parsed"]
    for c in v["dates"]:
        if c["expect"] == "OK":
            assert ancs.parse_date(c["input"].encode()) == c["local_epoch_s"]


def test_crc32_vectors():
    for c in load("crc32.json")["cases"]:
        assert f"{zlib.crc32(bytes.fromhex(c['hex'])):08x}" == c["crc32"]


def test_app_icon_vectors():
    v = load("app_icons.json")
    for c in v["ids"]:
        assert c["app_id"].encode().hex() == c["app_id_hex"], c["name"]
        assert assets.app_icon_id(c["app_id"]) == c["icon_id"], c["name"]
    for c in v["icons"]:
        assert assets.app_icon(c["rows"]).hex() == c["hex"], c["name"]
        assert (len(c["rows"][0]), len(c["rows"])) == (c["width"], c["height"]), c["name"]


def test_icon_removal_vectors():
    from epager_proto import apps
    v = load("app_icons.json")["removal"]
    assert v["cases"]
    for c in v["cases"]:
        m = apps.SeenApps(v["seen_apps"])
        for st in c["steps"]:
            {"seen": lambda: m.seen(st["app_id"]), "icon": lambda: m.install_icon(st["app_id"]),
             "policy": lambda: m.set_policy(st["app_id"], st["policy"]),
             "forget": lambda: m.forget(st["app_ids"])}[st["op"]]()
        assert sorted(m.icons) == c["icons_after"], c["name"]
        assert sorted(assets.app_icon_id(a) for a in c["icon_app_ids_after"]) == c["icons_after"], c["name"]
        assert set(m.apps) == set(c["apps_after"]), c["name"]


def test_contract_check_passes():
    r = subprocess.run([sys.executable, str(TOOLS / "contract_check.py")], capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr


def _merge(base: dict, over: dict) -> dict:
    out = dict(base)
    for k, v in over.items():
        out[k] = _merge(out[k], v) if isinstance(v, dict) and isinstance(out.get(k), dict) else v
    return out


@pytest.mark.parametrize("scenario", load("transfer.json")["scenarios"], ids=lambda s: s["name"])
def test_transfer_scenarios_replay(scenario):
    """Replaying each scenario through a fresh reference board reproduces every expected message."""
    doc = load("transfer.json")
    board = transfer.BoardModel(REG, _merge(doc["board"], scenario.get("board", {})))
    for step in scenario["steps"]:
        op = step["op"]
        if op == "request":
            assert codec.encode_message(REG, step["logical"]).hex() == step["hex"]
            got = board.handle(codec.decode_message(REG, bytes.fromhex(step["hex"])))
            assert [codec.encode_message(REG, m).hex() for m in got] == [e["hex"] for e in step["expect"]]
            for e in step["expect"]:
                assert codec.decode_message(REG, bytes.fromhex(e["hex"])) == e["logical"]
        elif op == "set_battery":
            board.set_battery(step["battery"])
        else:
            getattr(board, op)()


@pytest.mark.parametrize("older,newer", [
    ("0.1.0", "0.1.1"), ("0.9.9", "0.10.0"), ("1.0.0-alpha", "1.0.0"), ("1.0.0-alpha", "1.0.0-alpha.1"),
    ("1.0.0-alpha.1", "1.0.0-alpha.beta"), ("1.0.0-alpha.beta", "1.0.0-beta"), ("1.0.0-beta.2", "1.0.0-beta.11"),
    ("1.0.0-beta.11", "1.0.0-rc.1"), ("1.0.0-rc.1", "1.0.0"), ("1.0.0-01", "1.0.0-1"),
])
def test_semver_precedence(older, newer):
    """semver.org section 11 examples, plus leading zeros ordered by text."""
    a, b = transfer.parse_version(older), transfer.parse_version(newer)
    assert transfer.compare_versions(a, b) == -1 and transfer.compare_versions(b, a) == 1
    assert transfer.compare_versions(a, a) == 0


@pytest.mark.parametrize("bad", ["", "1", "1.2", "1.2.3.4", "v1.2.3", "1.2.3-", "1.2.3+", "1.2.3-a..b",
                                 "1.2.3-a b", "1234567890.0.0", "1.2.3-1234567890", "1.2.3-é", "١.2.3"])
def test_semver_invalid(bad):
    assert transfer.parse_version(bad) is None
