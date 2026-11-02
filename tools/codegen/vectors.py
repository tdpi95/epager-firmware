#!/usr/bin/env python3
"""Generate contracts/test-vectors/*.json.

Every case declares its expected result; the generator recomputes it with the Python
reference (tools/epager_proto) and refuses to write anything if the two disagree.
Usage: vectors.py [--check]
"""
from __future__ import annotations

import argparse
import copy
import hashlib
import json
import struct
import sys
import unicodedata
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from epager_proto import ancs, apps, assets, codec, config, dispatch, releases, segments, text, transfer, wifi  # noqa: E402
from epager_proto import registry as R  # noqa: E402

OUT = R.CONTRACTS / "test-vectors"
reg = R.load()
H = codec.header
tlv = codec.tlv


def u8(v): return struct.pack("<B", v)
def u16(v): return struct.pack("<H", v)
def u32(v): return struct.pack("<I", v)
def i16(v): return struct.pack("<h", v)


SHA = hashlib.sha256(b"epager").hexdigest()
# Paper app icon: 24x24 mono1 ('#' black, '.' white), asymmetric so bit-order mistakes show
BUBBLE = [
    "........................",
    "....################....",
    "..##................##..",
    ".#....................#.",
    ".#....................#.",
    "#......................#",
    "#......................#",
    "#......................#",
    "#....##....##....##....#",
    "#....##....##....##....#",
    "#......................#",
    "#......................#",
    "#......................#",
    ".#....................#.",
    ".#....................#.",
    "..###..............###..",
    "....#...############....",
    "....#..#................",
    "....#.#.................",
    "....##..................",
    "....#...................",
    "........................",
    "........................",
    "........................",
]
ICON = assets.app_icon(BUBBLE)
ZALO_ICON_ID = assets.app_icon_id("com.zing.zalo")
assert len(ICON) == 88
CAPS = {"contract_version": reg.contract_version, "device_name": "ePager-1A2B",
        "board_id": "waveshare_s3_epaper_154_v2", "variant": 1, "firmware_version": "0.1.0",
        "display": {"type": 1, "width": 200, "height": 200, "color_depth": 1, "refresh_class": 1},
        "features": 0b110000111111, "transports": 1, "storage_total": 1966080, "storage_free": 1900000,
        "asset_support": [{"asset_type": 1, "format": 3, "max_size": 262144},
                          {"asset_type": 2, "format": 1, "max_size": 5016},
                          {"asset_type": 3, "format": 4, "max_size": 4096},
                          {"asset_type": 5, "format": 1, "max_size": 88, "width": 24, "height": 24}],
        "max_mtu": 517, "config_revision": 3, "pairing_mode": False}


def _lcd_icon_pixels() -> list[list[tuple[int, int, int, int]]]:
    """32x32 test icon: a colour gradient disc, opaque inside, half-transparent rim, transparent corners."""
    rows = []
    for y in range(32):
        row = []
        for x in range(32):
            d2 = (x - 15.5) ** 2 + (y - 15.5) ** 2
            a = 255 if d2 <= 14 ** 2 else 128 if d2 <= 15.5 ** 2 else 0
            row.append((x * 8, y * 8, 160, a) if a else (0, 0, 0, 0))
        rows.append(row)
    return rows


LCD_ICON = assets.app_icon_rgb565a8(_lcd_icon_pixels())
assert len(LCD_ICON) == 3088
LCD_CAPS = {"contract_version": reg.contract_version, "device_name": "ePager-3C4D", "board_id": "ostb_xiaozhi_3st",
                "variant": 2, "firmware_version": "0.1.0",
                "display": {"type": 2, "width": 296, "height": 240, "color_depth": 16, "refresh_class": 2},
                "features": 0b11011000011111, "transports": 1, "storage_total": 8257536, "storage_free": 8200000,
                "asset_support": [{"asset_type": 2, "format": 2, "max_size": 142096},
                                  {"asset_type": 5, "format": 6, "max_size": 3088, "width": 32, "height": 32}],
                "max_mtu": 517, "config_revision": 1, "pairing_mode": True}

PAPER_WEATHER_CAPS = {**CAPS, "device_name": "ePager-5E6F", "features": 0b11110000111111}

# --------------------------------------------------------------------------- messages

VALID = [
    ("ping_empty", "PING with no payload", "PING", 1, {}),
    ("ping_payload", "PING with payload", "PING", 2, {"payload": "00ff10"}),
    ("ping_rsp", "", "PING_RSP", 2, {"status": 0, "payload": "00ff10"}),
    ("req_id_max", "req_id 65535 round-trips", "PING", 65535, {}),
    ("capabilities_paper", "Full Paper capabilities", "CAPABILITIES", 0, CAPS),
    ("capabilities_paper_weather", "Paper with Wi-Fi and the weather (since 0.9.1)", "CAPABILITIES", 0,
     PAPER_WEATHER_CAPS),
    ("capabilities_lcd", "LCD capabilities (since 0.8.0): colour LCD, touch, rgb565 background and "
     "32x32 rgb565a8 app icons; weather and wifi features (since 0.9.0)", "CAPABILITIES", 0, LCD_CAPS),
    ("config_get_all", "No keys = all", "CONFIG_GET", 10, {}),
    ("config_get_keys", "Repeated u16 keys", "CONFIG_GET", 11, {"key": [1, 3, 16]}),
    ("config_get_rsp", "", "CONFIG_GET_RSP", 11, {"status": 0, "revision": 7, "entry": [
        {"key": 1, "value": "01"}, {"key": 3, "value": "0a"}, {"key": 64, "value": ""}]}),
    ("config_set", "Batch with expected_revision", "CONFIG_SET", 12, {"expected_revision": 7, "entry": [
        {"key": 1, "value": "01"}, {"key": 33, "value": "2805"}, {"key": 48, "value": "a401"}]}),
    ("config_set_rsp_partial_unknown", "Unknown key reported, others applied", "CONFIG_SET_RSP", 12,
     {"status": 0, "revision": 8, "key_status": [{"key": 1, "status": 0}, {"key": 999, "status": 9}]}),
    ("config_set_rsp_failed", "Validation failure: status only", "CONFIG_SET_RSP", 13, {"status": 8}),
    ("config_reset", "", "CONFIG_RESET", 14, {}),
    ("config_reset_rsp", "", "CONFIG_RESET_RSP", 14, {"status": 0, "revision": 9}),
    ("time_sync_vn", "UTC+7", "TIME_SYNC", 15, {"utc_s": 1791700215, "tz_offset_min": 420}),
    ("time_sync_negative_tz", "UTC-5 and the range edges", "TIME_SYNC", 16, {"utc_s": 0, "tz_offset_min": -720}),
    ("time_sync_max_tz", "", "TIME_SYNC", 17, {"utc_s": 4294967295, "tz_offset_min": 840}),
    ("time_sync_rsp", "", "TIME_SYNC_RSP", 17, {"status": 0}),
    ("wifi_set", "Network with a WPA2 passphrase (since 0.9.0)", "WIFI_SET", 18,
     {"ssid": "Quán Cafe 5G", "password": "matkhau123"}),
    ("wifi_set_open", "Open network: no password", "WIFI_SET", 19, {"ssid": "CafeFree"}),
    ("wifi_set_max_lengths", "32-byte name, 63-byte passphrase", "WIFI_SET", 25,
     {"ssid": "S" * 32, "password": "p" * 63}),
        ("wifi_set_forget", "Forget all saved networks", "WIFI_SET", 26, {"forget": True}),
    ("wifi_set_rsp", "", "WIFI_SET_RSP", 26, {"status": 0}),
    ("apps_list", "", "APPS_LIST", 20, {"offset": 0, "limit": 16}),
    ("apps_list_rsp", "Vietnamese app name", "APPS_LIST_RSP", 20, {"status": 0, "total": 2, "app": [
        {"app_id": "com.zing.zalo", "name": "Zalo", "policy": 1, "source": 2, "last_seen": 1791700000},
        {"app_id": "vn.momo.app", "name": "Ví MoMo", "policy": 0, "source": 1, "last_seen": 0}]}),
    ("apps_set_policy", "", "APPS_SET_POLICY", 21, {"entry": [
        {"app_id": "com.facebook.orca", "policy": 2}, {"app_id": "com.zing.zalo", "policy": 1}]}),
    ("apps_set_policy_rsp", "", "APPS_SET_POLICY_RSP", 21, {"status": 0, "revision": 10}),
    ("apps_forget_all", "No ids = all", "APPS_FORGET", 22, {}),
    ("apps_forget_some", "", "APPS_FORGET", 23, {"app_id": ["com.a", "com.b"]}),
    ("apps_forget_rsp", "", "APPS_FORGET_RSP", 23, {"status": 0, "revision": 11}),
    ("notif_post_full", "Vietnamese text, all fields", "NOTIF_POST", 30, {
        "key": "0|com.zing.zalo|1001|null|10123", "app_id": "com.zing.zalo", "app_name": "Zalo",
        "title": "Nguyễn Văn An", "subtitle": "Nhóm gia đình", "body": "Tối nay mấy giờ về ăn cơm? 😀",
        "timestamp": 1791700215, "category": 4, "priority": 2, "flags": 0b00010}),
    ("notif_post_minimal", "Only required fields", "NOTIF_POST", 31, {"key": "k", "app_id": "a"}),
    ("notif_post_max_body", "Body exactly max_len (1024 bytes) with 3-byte characters",
     "NOTIF_POST", 32, {"key": "k2", "app_id": "a", "body": "ệ" * 341 + "a"}),
    ("notif_post_unknown_enum", "Unknown category/priority values are accepted (receiver falls back)",
     "NOTIF_POST", 33, {"key": "k3", "app_id": "a", "category": 200, "priority": 9, "flags": 255}),
    ("notif_post_rsp", "", "NOTIF_POST_RSP", 30, {"status": 0, "notif_id": 42, "shown": True}),
    ("notif_post_rsp_filtered", "", "NOTIF_POST_RSP", 31, {"status": 0, "notif_id": 43, "shown": False}),
    ("notif_remove", "", "NOTIF_REMOVE", 34, {"key": "k"}),
    ("notif_remove_rsp_not_found", "", "NOTIF_REMOVE_RSP", 34, {"status": 24}),
    ("notif_clear", "", "NOTIF_CLEAR", 35, {}),
    ("notif_clear_rsp", "", "NOTIF_CLEAR_RSP", 35, {"status": 0}),
    ("xfer_begin_asset", "", "XFER_BEGIN", 40, {"kind": 1, "id": "bg-cat_01.v2", "asset_type": 2, "size": 5016,
                                               "crc32": 0x1234ABCD, "sha256": SHA, "version": "1.0.0"}),
    ("xfer_begin_app_icon", "App icon upload (asset-formats.md app_icon): id = app_icon_id(\"com.zing.zalo\"), "
     "88-byte 24x24 mono1 file", "XFER_BEGIN", 49, {"kind": 1, "id": ZALO_ICON_ID, "asset_type": 5, "size": len(ICON),
                                                   "crc32": zlib.crc32(ICON), "sha256": hashlib.sha256(ICON).hexdigest()}),
    ("xfer_begin_firmware_restart", "", "XFER_BEGIN", 41, {"kind": 2, "id": "firmware", "size": 1500000,
                                                          "crc32": 0xDEADBEEF, "sha256": SHA, "version": "0.2.0", "restart": True}),
    ("xfer_begin_rsp_resume", "", "XFER_BEGIN_RSP", 41, {"status": 0, "handle": 1, "offset": 65536,
                                                        "prefix_crc32": 0x0BADF00D, "max_chunk": 495, "ack_window": 8192}),
    ("xfer_begin_rsp_low_battery", "", "XFER_BEGIN_RSP", 42, {"status": 22}),
    ("xfer_chunk_max", "1024 data bytes", "XFER_CHUNK", 0, {"handle": 1, "offset": 65536, "data": bytes(range(256)).hex() * 4}),
    ("xfer_chunk_small", "", "XFER_CHUNK", 0, {"handle": 1, "offset": 0, "data": "e9"}),
    ("xfer_status", "", "XFER_STATUS", 43, {"handle": 1}),
    ("xfer_status_rsp", "", "XFER_STATUS_RSP", 43, {"status": 0, "state": 1, "offset": 4096, "prefix_crc32": 1}),
    ("xfer_finish", "", "XFER_FINISH", 44, {"handle": 1}),
    ("xfer_finish_rsp_hash", "", "XFER_FINISH_RSP", 44, {"status": 19}),
    ("xfer_abort", "", "XFER_ABORT", 45, {"handle": 1}),
    ("xfer_abort_rsp", "", "XFER_ABORT_RSP", 45, {"status": 0}),
    ("storage_info", "", "STORAGE_INFO", 46, {}),
    ("storage_info_rsp", "", "STORAGE_INFO_RSP", 46, {"status": 0, "total": 1966080, "free": 1000000, "asset_count": 3}),
    ("asset_list", "", "ASSET_LIST", 47, {"offset": 2, "limit": 1}),
    ("asset_list_rsp", "", "ASSET_LIST_RSP", 47, {"status": 0, "total": 3, "asset": [
        {"asset_type": 2, "id": "bg-cat_01.v2", "size": 5016, "version": "1.0.0"}]}),
    ("asset_delete", "", "ASSET_DELETE", 48, {"asset_type": 2, "id": "bg-cat_01.v2"}),
    ("asset_delete_rsp", "", "ASSET_DELETE_RSP", 48, {"status": 0}),
    ("event_config_changed", "", "CONFIG_CHANGED", 0, {"revision": 8, "key": [1, 33]}),
    ("event_config_changed_policy_only", "Policy change: no keys", "CONFIG_CHANGED", 0, {"revision": 9}),
    ("event_notif_dismissed_android", "Dismissed on the board (since 0.8.0): Android carries its key",
     "NOTIF_DISMISSED", 0, {"notif_id": 42, "source": 2, "key": "0|com.zing.zalo|1001|null|10123"}),
    ("event_notif_dismissed_ancs", "ANCS: no key", "NOTIF_DISMISSED", 0, {"notif_id": 7, "source": 1}),
    ("event_xfer_ack", "", "XFER_ACK", 0, {"handle": 1, "offset": 73728, "status": 0}),
    ("event_xfer_ack_bad_offset", "", "XFER_ACK", 0, {"handle": 1, "offset": 4096, "status": 16}),
    ("event_app_seen", "", "APP_SEEN", 0, {"app": {"app_id": "com.apple.MobileSMS", "name": "Tin nhắn", "policy": 0, "source": 1}}),
    ("event_device_status", "", "DEVICE_STATUS", 0, {"battery_pct": 87, "external_power": 0, "uptime_s": 3600,
                                                    "notif_count": 4, "bonded_count": 2, "pairing_mode": False, "ota_state": 3}),
    ("event_device_status_no_battery", "", "DEVICE_STATUS", 0, {"battery_pct": 255, "external_power": 2}),
    ("event_device_status_wifi", "Wi-Fi and weather states (since 0.9.0)", "DEVICE_STATUS", 0,
     {"battery_pct": 64, "external_power": 1, "uptime_s": 7200, "notif_count": 0, "bonded_count": 1,
      "pairing_mode": False, "ota_state": 0, "wifi_state": 1, "wifi_ssid": "Quán Cafe 5G", "weather_state": 2}),
    ("event_device_status_wifi_unconfigured", "No network stored: no wifi_ssid", "DEVICE_STATUS", 0,
     {"battery_pct": 90, "wifi_state": 0, "weather_state": 1}),
    ("error_unknown_message", "", "ERROR", 99, {"status": 1, "msg_type": 94}),
]


def msg(name, req_id=0, ver=None):
    return H(reg, name, req_id, ver)


def caps_bytes():
    return codec.encode_message(reg, {"type": "CAPABILITIES", "fields": CAPS})


# (name, description, bytes, expected status, logical or None). Decode-only.
DECODE_ONLY = [
    ("reverse_tag_order", "Fields in descending tag order decode the same", msg("TIME_SYNC", 15) + tlv(2, i16(420)) + tlv(1, u32(1791700215)),
     "OK", {"type": "TIME_SYNC", "req_id": 15, "fields": {"utc_s": 1791700215, "tz_offset_min": 420}}),
    ("unknown_tag_skipped", "Unknown top-level tag 0xEE skipped", msg("XFER_STATUS", 5) + tlv(0xEE, b"future") + tlv(1, u8(3)),
     "OK", {"type": "XFER_STATUS", "req_id": 5, "fields": {"handle": 3}}),
    ("unknown_empty_tag_skipped", "Unknown tag with zero length", msg("XFER_STATUS", 5) + tlv(1, u8(3)) + tlv(0x80, b""),
     "OK", {"type": "XFER_STATUS", "req_id": 5, "fields": {"handle": 3}}),
    ("unknown_nested_tag_skipped", "Unknown tag inside a struct is skipped",
     msg("CONFIG_SET", 6) + tlv(2, tlv(1, u16(1)) + tlv(9, b"\x00\x01") + tlv(2, b"\x01")),
     "OK", {"type": "CONFIG_SET", "req_id": 6, "fields": {"entry": [{"key": 1, "value": "01"}]}}),
    ("text_invalid_utf8_accepted", "text fields are not UTF-8 validated on the wire",
     msg("NOTIF_POST", 7) + tlv(1, b"k") + tlv(2, b"a") + tlv(4, b"ok\xff\xfe"), "OK", None),
    ("unknown_status_code", "Unknown status values are carried as numbers", msg("TIME_SYNC_RSP", 1) + tlv(1, u8(200)),
     "OK", {"type": "TIME_SYNC_RSP", "req_id": 1, "fields": {"status": 200}}),
]

LONG = "x" * 65
INVALID = [
    ("empty", "", b"", "MALFORMED"),
    ("short_header", "3 bytes", b"\x00\x01\x00", "MALFORMED"),
    ("bad_version", "ver 1", msg("PING", 1, ver=1), "UNSUPPORTED_VERSION"),
    ("bad_version_and_unknown_type", "version is checked before type", b"\x01\x5e\x01\x00", "UNSUPPORTED_VERSION"),
    ("unknown_type", "", b"\x00\x5e\x01\x00", "UNKNOWN_MESSAGE"),
    ("reserved_type_7f", "", b"\x00\x7f\x01\x00", "UNKNOWN_MESSAGE"),
    ("truncated_tlv_header", "2 bytes left", msg("PING", 1) + b"\x01\x00", "MALFORMED"),
    ("value_past_end", "", msg("PING", 1) + b"\x01\x05\x00abc", "MALFORMED"),
    ("tag_zero", "", msg("PING", 1) + tlv(0, b""), "MALFORMED"),
    ("tag_zero_after_unknown", "", msg("PING", 1) + tlv(0x99, b"") + tlv(0, b"x"), "MALFORMED"),
    ("duplicate_field", "", msg("PING", 1) + tlv(1, b"a") + tlv(1, b"b"), "MALFORMED"),
    ("u8_wrong_width", "", msg("XFER_STATUS", 1) + tlv(1, u16(1)), "MALFORMED"),
    ("u32_wrong_width", "", msg("TIME_SYNC", 1) + tlv(1, b"\x00\x00\x00") + tlv(2, i16(0)), "MALFORMED"),
    ("bool_value_2", "", msg("XFER_BEGIN", 1) + tlv(8, b"\x02"), "MALFORMED"),
    ("bool_empty", "", msg("NOTIF_POST_RSP", 1) + tlv(1, u8(0)) + tlv(3, b""), "MALFORMED"),
    ("str_too_long", "key 65 bytes", msg("NOTIF_POST", 1) + tlv(1, LONG.encode()) + tlv(2, b"a"), "TOO_LARGE"),
    ("text_too_long", "body 1025 bytes", msg("NOTIF_POST", 1) + tlv(1, b"k") + tlv(2, b"a") + tlv(6, b"y" * 1025), "TOO_LARGE"),
    ("bytes_too_long", "payload 513 bytes", msg("PING", 1) + tlv(1, bytes(513)), "TOO_LARGE"),
    ("str_below_min_len", "empty key", msg("NOTIF_POST", 1) + tlv(1, b"") + tlv(2, b"a"), "MALFORMED"),
    ("sha256_short", "31 bytes", msg("XFER_BEGIN", 1) + tlv(6, bytes(31)), "MALFORMED"),
    ("sha256_long", "33 bytes", msg("XFER_BEGIN", 1) + tlv(6, bytes(33)), "TOO_LARGE"),
    ("chunk_empty_data", "", msg("XFER_CHUNK") + tlv(1, u8(1)) + tlv(2, u32(0)) + tlv(3, b""), "MALFORMED"),
    ("chunk_data_too_long", "", msg("XFER_CHUNK") + tlv(1, u8(1)) + tlv(2, u32(0)) + tlv(3, bytes(1025)), "TOO_LARGE"),
    ("utf8_invalid_byte", "", msg("NOTIF_POST", 1) + tlv(1, b"k") + tlv(2, b"\xff"), "INVALID_UTF8"),
    ("utf8_overlong", "C0 AF", msg("NOTIF_POST", 1) + tlv(1, b"k") + tlv(2, b"\xc0\xaf"), "INVALID_UTF8"),
    ("utf8_overlong_3byte", "E0 80 AF", msg("NOTIF_POST", 1) + tlv(1, b"k") + tlv(2, b"\xe0\x80\xaf"), "INVALID_UTF8"),
    ("utf8_surrogate", "ED A0 80", msg("NOTIF_POST", 1) + tlv(1, b"k") + tlv(2, b"\xed\xa0\x80"), "INVALID_UTF8"),
    ("utf8_above_max", "F4 90 80 80", msg("NOTIF_POST", 1) + tlv(1, b"k") + tlv(2, b"\xf4\x90\x80\x80"), "INVALID_UTF8"),
    ("utf8_truncated", "E2 82", msg("NOTIF_POST", 1) + tlv(1, b"k") + tlv(2, b"a\xe2\x82"), "INVALID_UTF8"),
    ("utf8_lone_continuation", "", msg("NOTIF_POST", 1) + tlv(1, b"\x80") + tlv(2, b"a"), "INVALID_UTF8"),
    ("utf8_f5", "F5 lead byte", msg("NOTIF_POST", 1) + tlv(1, b"k") + tlv(2, b"\xf5\x80\x80\x80"), "INVALID_UTF8"),
    ("id_uppercase", "", msg("ASSET_DELETE", 1) + tlv(1, u8(2)) + tlv(2, b"Font"), "INVALID_ID"),
    ("id_leading_dot", "", msg("ASSET_DELETE", 1) + tlv(1, u8(2)) + tlv(2, b".hidden"), "INVALID_ID"),
    ("id_path_traversal", "", msg("ASSET_DELETE", 1) + tlv(1, u8(2)) + tlv(2, b"../x"), "INVALID_ID"),
    ("id_non_ascii", "valid UTF-8 but not allowed in ids", msg("ASSET_DELETE", 1) + tlv(1, u8(2)) + tlv(2, "phông".encode()), "INVALID_ID"),
    ("id_invalid_utf8", "UTF-8 is checked before the id charset", msg("ASSET_DELETE", 1) + tlv(1, u8(2)) + tlv(2, b"a\xff"), "INVALID_UTF8"),
    ("id_too_long", "33 bytes", msg("ASSET_DELETE", 1) + tlv(1, u8(2)) + tlv(2, b"a" * 33), "TOO_LARGE"),
    ("id_empty", "", msg("ASSET_DELETE", 1) + tlv(1, u8(2)) + tlv(2, b""), "MALFORMED"),
    ("wifi_ssid_empty", "", msg("WIFI_SET", 1) + tlv(1, b""), "MALFORMED"),
    ("wifi_ssid_33_bytes", "", msg("WIFI_SET", 1) + tlv(1, b"s" * 33), "TOO_LARGE"),
    ("wifi_password_64_bytes", "a 64-hex PSK is not accepted", msg("WIFI_SET", 1) + tlv(1, b"s") + tlv(2, b"a" * 64), "TOO_LARGE"),
    ("wifi_ssid_invalid_utf8", "", msg("WIFI_SET", 1) + tlv(1, b"\xff"), "INVALID_UTF8"),
    ("tz_above_max", "", msg("TIME_SYNC", 1) + tlv(1, u32(0)) + tlv(2, i16(841)), "OUT_OF_RANGE"),
    ("tz_below_min", "", msg("TIME_SYNC", 1) + tlv(1, u32(0)) + tlv(2, i16(-721)), "OUT_OF_RANGE"),
    ("limit_zero", "", msg("APPS_LIST", 1) + tlv(2, u8(0)), "OUT_OF_RANGE"),
    ("limit_17", "", msg("ASSET_LIST", 1) + tlv(2, u8(17)), "OUT_OF_RANGE"),
    ("xfer_size_zero", "", msg("XFER_BEGIN", 1) + tlv(1, u8(1)) + tlv(4, u32(0)), "OUT_OF_RANGE"),
    ("missing_required", "NOTIF_POST without app_id", msg("NOTIF_POST", 1) + tlv(1, b"k"), "MISSING_FIELD"),
    ("missing_repeated_required", "CONFIG_SET without entries", msg("CONFIG_SET", 1) + tlv(1, u32(1)), "MISSING_FIELD"),
    ("missing_nested_required", "config entry without value", msg("CONFIG_SET", 1) + tlv(2, tlv(1, u16(1))), "MISSING_FIELD"),
    ("missing_all", "XFER_CHUNK with empty body", msg("XFER_CHUNK"), "MISSING_FIELD"),
    ("missing_status_in_response", "", msg("CONFIG_SET_RSP", 1) + tlv(2, u32(1)), "MISSING_FIELD"),
    ("nested_malformed", "truncated TLV inside a struct", msg("CONFIG_SET", 1) + tlv(2, b"\x01\x02"), "MALFORMED"),
    ("nested_missing_before_outer_error", "nested struct is fully validated when reached",
     msg("CONFIG_SET", 1) + tlv(2, tlv(2, b"\x01")) + tlv(1, b"\x00"), "MISSING_FIELD"),
    ("order_wire_first_error_wins", "OUT_OF_RANGE before a later truncated TLV",
     msg("TIME_SYNC", 1) + tlv(2, i16(900)) + b"\x01\x04", "OUT_OF_RANGE"),
    ("order_field_error_before_missing", "INVALID_UTF8 reported before the missing required field",
     msg("NOTIF_POST", 1) + tlv(2, b"\xff"), "INVALID_UTF8"),
    ("order_duplicate_before_value", "duplicate detected before validating the second value",
     msg("TIME_SYNC", 1) + tlv(2, i16(1)) + tlv(2, i16(5000)), "MALFORMED"),
    ("struct_in_struct_field_wrong_width", "u16 key encoded with 1 byte inside entry",
     msg("CONFIG_GET_RSP", 1) + tlv(1, u8(0)) + tlv(3, tlv(1, u8(1)) + tlv(2, b"")), "MALFORMED"),
]


def gen_messages() -> dict:
    cases = []
    for name, desc, mtype, req_id, fields in VALID:
        logical = {"ver": reg.proto_major, "type": mtype, "req_id": req_id, "fields": fields}
        b = codec.encode_message(reg, logical)
        got = codec.decode_message(reg, b)
        assert got == logical, (name, got, logical)
        cases.append({"name": name, "description": desc, "hex": b.hex(), "expect": "OK", "logical": logical})
    caps = caps_bytes()
    assert len(caps) <= reg.limits["capabilities_max"], len(caps)
    for name, desc, b, expect, logical in DECODE_ONLY:
        try:
            got = codec.decode_message(reg, b)
            status = "OK"
        except codec.ProtoError as e:
            status, got = e.status, None
        assert status == expect, (name, status)
        if logical is not None:
            logical = {"ver": reg.proto_major, **logical}
            assert got == logical, (name, got)
        c = {"name": name, "description": desc, "hex": b.hex(), "expect": expect, "decode_only": True}
        if logical is not None:
            c["logical"] = logical
        cases.append(c)
    for name, desc, b, expect in INVALID:
        try:
            codec.decode_message(reg, b)
            status = "OK"
        except codec.ProtoError as e:
            status = e.status
        assert status == expect, (name, status, expect)
        cases.append({"name": name, "description": desc, "hex": b.hex(), "expect": expect})
    return {"description": "Message codec vectors (protocol.md 2.2-2.4). OK cases with 'logical' and without "
                           "'decode_only' must also encode to exactly 'hex'. Error cases give the first error "
                           "per the validation order.", "scope": "app+board", "cases": cases}


# --------------------------------------------------------------------------- segments

def gen_segments() -> dict:
    split_cases = []
    for name, msg_bytes, maxseg in [
        ("fits_one", bytes.fromhex("00010100"), 20),
        ("exact_fit", bytes(range(19)), 20),
        ("two_segments", bytes(range(20)), 20),
        ("min_mtu_23", caps_bytes(), 20),
        ("seq_wraps", bytes(i & 0xFF for i in range(1300)), 20),
    ]:
        segs = segments.split(msg_bytes, maxseg)
        split_cases.append({"name": name, "message": msg_bytes.hex(), "max_segment": maxseg,
                            "segments": [s.hex() for s in segs]})
    S = lambda h, p=b"": bytes([h]) + p  # noqa: E731
    FL, F, L = 0xC0, 0x80, 0x40
    many = segments.split(bytes(i & 0xFF for i in range(65 * 3)), 4)  # 65 segments
    reasm = [
        ("single", 64, [S(FL, b"abcd")]),
        ("three", 64, [S(F | 0, b"ab"), S(1, b"cd"), S(L | 2, b"ef")]),
        ("empty_middle_segment", 64, [S(F, b"ab"), S(1), S(L | 2, b"cd")]),
        ("seq_wrap", 1024, many),
        ("seq_gap", 64, [S(F, b"ab"), S(2, b"cd"), S(L | 3, b"ef"), S(FL, b"ok")]),
        ("continuation_when_idle", 64, [S(1, b"xx"), S(L | 2, b"yy"), S(FL, b"ok")]),
        ("first_mid_message", 64, [S(F, b"ab"), S(F, b"cd"), S(L | 1, b"ef")]),
        ("first_with_nonzero_seq", 64, [S(F | 5, b"ab"), S(L | 6, b"cd"), S(FL, b"ok")]),
        ("first_last_nonzero_seq", 64, [S(FL | 1, b"ab")]),
        ("too_large", 8, [S(F, b"12345"), S(1, b"6789"), S(L | 2, b"0"), S(FL, b"ok")]),
        ("exactly_max", 8, [S(F, b"1234"), S(L | 1, b"5678")]),
        ("too_large_single", 3, [S(FL, b"abcd"), S(FL, b"abc")]),
        ("empty_message", 64, [S(FL)]),
        ("zero_length_write", 64, [b"", S(FL, b"ok")]),
        ("zero_length_mid_message", 64, [S(F, b"ab"), b"", S(L | 1, b"cd"), S(FL, b"ok")]),
        ("first_nonzero_seq_mid_message", 64, [S(F, b"ab"), S(F | 3, b"cd"), S(L | 4, b"ef"), S(FL, b"ok")]),
        ("zero_length_while_discarding", 64, [S(1, b"xx"), b"", S(L | 2, b"yy"), S(FL, b"ok")]),
    ]
    reasm_cases = []
    for name, maxm, segs in reasm:
        r = segments.Reassembler(maxm)
        events = []
        for s in segs:
            for kind, val in r.feed(s):
                events.append({"message": val.hex()} if kind == "message" else {"error": val})
        reasm_cases.append({"name": name, "max_message": maxm, "segments": [s.hex() for s in segs], "events": events})
    exp = {"single": [{"message": "61626364"}],
           "first_mid_message": [{"error": "SEGMENT_ERROR"}, {"message": b"cdef".hex()}],
           "too_large": [{"error": "TOO_LARGE"}, {"message": b"ok".hex()}],
           "empty_message": [{"error": "SEGMENT_ERROR"}],
           "first_nonzero_seq_mid_message": [{"error": "SEGMENT_ERROR"}, {"error": "SEGMENT_ERROR"}, {"message": b"ok".hex()}],
           "zero_length_while_discarding": [{"error": "SEGMENT_ERROR"}, {"error": "SEGMENT_ERROR"}, {"message": b"ok".hex()}]}
    for c in reasm_cases:
        if c["name"] in exp:
            assert c["events"] == exp[c["name"]], c
    return {"description": "Segmentation (protocol.md 2.1). 'split': a sender with max_segment bytes per write must "
                           "produce exactly 'segments'. 'reassemble': feeding 'segments' in order to a receiver with "
                           "max_message must produce exactly 'events'.",
            "scope": "app+board", "split": split_cases, "reassemble": reasm_cases}


# --------------------------------------------------------------------------- dispatch

def gen_dispatch() -> dict:
    E = lambda name, rid, fields: codec.encode_message(reg, {"type": name, "req_id": rid, "fields": fields})  # noqa: E731
    one = lambda m: bytes([0xC0]) + m  # noqa: E731  single-segment write
    ping = lambda rid, payload=b"": E("PING", rid, {"payload": payload.hex()} if payload else {})  # noqa: E731
    big = bytes((i * 7) & 0xFF for i in range(400))
    cases = [
        # (name, description, mtu, secure, [(char, segment bytes)], expected statuses/types per write)
        ("ping_each_channel", "PING is accepted on every request characteristic", 517, True,
         [("config", one(ping(1))), ("notification", one(ping(2, b"\x01"))), ("transfer", one(ping(3)))]),
        ("ping_response_segmented_mtu23", "A 400-byte PING payload answered in 20-byte segments (MTU 23)",
         23, True, [("config", s) for s in segments.split(ping(4, big), 20)]),
        ("ping_response_segmented_mtu185", "The same answer at MTU 185", 185, True,
         [("notification", one(ping(5, big)))]),
        ("response_written", "A response type written by the app: ERROR UNKNOWN_MESSAGE", 517, True,
         [("config", one(E("PING_RSP", 6, {"status": 0})))]),
        ("event_written", "An event type written by the app", 517, True,
         [("config", one(E("CONFIG_CHANGED", 7, {"revision": 1})))]),
        ("read_value_written", "CAPABILITIES written by the app", 517, True,
         [("config", one(codec.encode_message(reg, {"type": "CAPABILITIES", "req_id": 8, "fields": CAPS})))]),
        ("error_written", "ERROR written by the app", 517, True,
         [("transfer", one(E("ERROR", 9, {"status": 1})))]),
        ("wrong_channel", "Requests on another request characteristic: ERROR UNKNOWN_MESSAGE there", 517, True,
         [("notification", one(E("CONFIG_GET", 10, {}))),
          ("config", one(E("NOTIF_CLEAR", 11, {}))),
          ("config", one(E("XFER_STATUS", 12, {"handle": 1}))),
          ("transfer", one(E("CONFIG_RESET", 13, {})))]),
        ("wrong_channel_invalid_body", "Routing is checked before the body", 517, True,
         [("notification", one(H(reg, "CONFIG_SET", 14) + b"\x02\x05"))]),
        ("insecure_requests", "Link not secure: status-only INSUFFICIENT_SECURITY answers", 517, False,
         [("config", one(E("CONFIG_GET", 15, {}))), ("config", one(ping(16))),
          ("notification", one(E("NOTIF_CLEAR", 17, {}))),
          ("transfer", one(E("XFER_CHUNK", 0, {"handle": 3, "offset": 0, "data": "00"})))]),
        ("insecure_checked_before_body", "The security gate comes before body validation", 517, False,
         [("config", one(H(reg, "CONFIG_SET", 18) + b"\x02\x05"))]),
        ("insecure_wrong_channel", "Routing comes before the security gate", 517, False,
         [("transfer", one(E("CONFIG_GET", 19, {})))]),
        ("header_errors", "Short header, wrong version, unknown type", 517, True,
         [("config", one(b"\x00\x01\x05")), ("config", one(b"\x07")),
          ("config", one(H(reg, "PING", 20, ver=reg.proto_major + 1))),
          ("notification", one(bytes([reg.proto_major, 0x5E]) + (21).to_bytes(2, "little")))]),
        ("body_errors", "Invalid bodies: the request's response type with only the status", 517, True,
         [("config", one(H(reg, "CONFIG_GET", 22) + b"\x01\x02")),
          ("config", one(H(reg, "PING", 23) + tlv(1, bytes(513)))),
          ("notification", one(E("NOTIF_REMOVE", 24, {}))),
          ("transfer", one(H(reg, "XFER_FINISH", 25) + tlv(1, b"\x01\x02")))]),
        ("chunk_errors", "Invalid XFER_CHUNKs are acked with the handle as sent (0 if unreadable)", 517, True,
         [("transfer", one(H(reg, "XFER_CHUNK", 0) + tlv(1, b"\x05") + tlv(2, u32(0)))),
          ("transfer", one(H(reg, "XFER_CHUNK", 0) + tlv(1, b"\x05\x00") + tlv(2, u32(0)) + tlv(3, b"\x00"))),
          ("transfer", one(H(reg, "XFER_CHUNK", 0) + b"\x01\x01"))]),
        ("segment_errors", "Reassembly errors are reported with ERROR on the characteristic, req_id 0", 517, True,
         [("config", bytes([0x80]) + ping(26)[:3]), ("config", bytes([0x42]) + ping(26)[3:]),
          ("config", one(ping(27))),
          ("notification", bytes([0x41]) + b"zz"),
          ("transfer", bytes([0xC0]))]),
        ("too_large", "A Config message above max_message_config", 517, True,
         [("config", s) for s in segments.split(H(reg, "PING", 28) + tlv(1, bytes(1100)), 512)]
         + [("config", one(ping(29)))]),
        ("interleaved", "Each characteristic reassembles independently", 23, True,
         [("config", segments.split(ping(30, bytes(30)), 20)[0]),
          ("notification", segments.split(ping(31, bytes(30)), 20)[0]),
          ("config", segments.split(ping(30, bytes(30)), 20)[1]),
          ("notification", segments.split(ping(31, bytes(30)), 20)[1])]),
    ]
    expect = {
        "ping_each_channel": [["PING_RSP:OK"], ["PING_RSP:OK"], ["PING_RSP:OK"]],
        "response_written": [["ERROR:UNKNOWN_MESSAGE"]],
        "wrong_channel": [["ERROR:UNKNOWN_MESSAGE"]] * 4,
        "wrong_channel_invalid_body": [["ERROR:UNKNOWN_MESSAGE"]],
        "insecure_requests": [["CONFIG_GET_RSP:INSUFFICIENT_SECURITY"], ["PING_RSP:INSUFFICIENT_SECURITY"],
                              ["NOTIF_CLEAR_RSP:INSUFFICIENT_SECURITY"], ["XFER_ACK:INSUFFICIENT_SECURITY"]],
        "insecure_checked_before_body": [["CONFIG_SET_RSP:INSUFFICIENT_SECURITY"]],
        "insecure_wrong_channel": [["ERROR:UNKNOWN_MESSAGE"]],
        "header_errors": [["ERROR:MALFORMED"], ["ERROR:MALFORMED"], ["ERROR:UNSUPPORTED_VERSION"],
                          ["ERROR:UNKNOWN_MESSAGE"]],
        "body_errors": [["CONFIG_GET_RSP:MALFORMED"], ["PING_RSP:TOO_LARGE"], ["NOTIF_REMOVE_RSP:MISSING_FIELD"],
                        ["XFER_FINISH_RSP:MALFORMED"]],
        "chunk_errors": [["XFER_ACK:MISSING_FIELD"], ["XFER_ACK:MALFORMED"], ["XFER_ACK:MALFORMED"]],
        "segment_errors": [[], ["ERROR:SEGMENT_ERROR"], ["PING_RSP:OK"], ["ERROR:SEGMENT_ERROR"],
                           ["ERROR:SEGMENT_ERROR"]],
        "too_large": [[], [], ["ERROR:TOO_LARGE"], ["PING_RSP:OK"]],
        "interleaved": [[], [], ["PING_RSP:OK"], ["PING_RSP:OK"]],
    }
    out = []
    for name, desc, mtu, secure, writes in cases:
        d = dispatch.Dispatcher(reg, mtu=mtu, secure=secure)
        steps, summary = [], []
        for char, seg in writes:
            notes = d.write(char, seg)
            msgs, r = [], segments.Reassembler(1 << 16)
            for c, s in notes:
                assert c == char and len(s) <= dispatch.segment_max(mtu), (name, c, len(s))
                for kind, val in r.feed(s):
                    assert kind == "message", (name, kind, val)
                    m = codec.decode_message(reg, val)
                    msgs.append(f"{m['type']}:{reg.status_by_id[m['fields']['status']]}")
            summary.append(msgs)
            steps.append({"write": {"char": char, "hex": seg.hex()},
                          "expect": [{"char": c, "hex": s.hex()} for c, s in notes]})
        if name in expect:
            assert summary == expect[name], (name, summary)
        out.append({"name": name, "description": desc, "mtu": mtu, "secure": secure, "steps": steps})
    return {"description": "Board dispatch (protocol.md 2.1 and 2.4: reassembly, header checks, routing, "
                           "security gate, body validation, answers segmented at the link MTU). Each case starts "
                           "on a fresh connection with the given ATT MTU and security. For every 'write' to a "
                           "characteristic the board must notify exactly the 'expect' segments, in order. No "
                           "transfer session exists and only PING reaches a handler.",
            "scope": "board", "cases": out}


# --------------------------------------------------------------------------- config values

def gen_config_values() -> dict:
    cases = []
    for k in reg.config_keys:
        b = config.encode_value(k, k["default"])
        variant = k.get("variants", ["paper"])[0]
        assert config.validate(reg, k["id"], b, variant) == "OK"
        cases.append({"name": f"default_{k['name']}", "key": k["id"], "variant": variant, "value_hex": b.hex(),
                      "value": k["default"], "expect": "OK"})
    K = {k["name"]: k for k in reg.config_keys}

    def add(name, key, value_bytes, expect, variant="paper", value=None):
        got = config.validate(reg, key, value_bytes, variant)
        assert got == expect, (name, got, expect)
        c = {"name": name, "key": key, "variant": variant, "value_hex": value_bytes.hex(), "expect": expect}
        if value is not None:
            c["value"] = value
        cases.append(c)

    add("language_vi", 1, b"\x01", "OK", value=1)
    add("language_unknown_enum", 1, b"\x02", "OUT_OF_RANGE")
    add("orientation_max", 2, b"\x03", "OK", value=3)
    add("orientation_above_max", 2, b"\x04", "OUT_OF_RANGE")
    add("full_refresh_zero", 3, b"\x00", "OUT_OF_RANGE")
    add("full_refresh_on_lcd", 3, b"\x0a", "UNKNOWN_KEY", variant="lcd")
    add("show_body_false", 4, b"\x00", "OK", value=False)
    add("mask_numbers_on", 10, b"\x01", "OK", value=True)
    add("mask_numbers_value_2", 10, b"\x02", "OUT_OF_RANGE")
    add("clock_12h", 5, b"\x00", "OK", value=False)
    add("clock_24h_value_2", 5, b"\x02", "OUT_OF_RANGE")
    add("temp_unit_fahrenheit", 6, b"\x01", "OK", value=1)
    add("temp_unit_unknown_enum", 6, b"\x02", "OUT_OF_RANGE")
    add("temp_unit_on_lcd", 6, b"\x00", "OK", variant="lcd", value=0)
    add("bool_value_2", 4, b"\x02", "OUT_OF_RANGE")
    add("bool_two_bytes", 4, b"\x01\x00", "TYPE_MISMATCH")
    add("category_mask_all_bits", 17, u16(0xFFFF), "OK", value=0xFFFF)
    add("category_mask_one_byte", 17, b"\xff", "TYPE_MISMATCH")
    add("min_priority_urgent", 18, b"\x03", "OK", value=3)
    add("min_priority_unknown", 18, b"\x04", "OUT_OF_RANGE")
    add("quiet_start_max", 33, u16(1439), "OK", value=1439)
    add("quiet_start_1440", 33, u16(1440), "OUT_OF_RANGE")
    add("tz_negative", 48, i16(-300), "OK", value=-300)
    add("tz_below_min", 48, i16(-721), "OUT_OF_RANGE")
    add("tz_three_bytes", 48, b"\x00\x00\x00", "TYPE_MISMATCH")
    add("theme_id_set", 64, b"dark-cat", "OK", value="dark-cat")
    add("theme_id_32_bytes", 64, b"a" * 32, "OK", value="a" * 32)
    add("theme_id_33_bytes", 64, b"a" * 33, "TOO_LARGE")
    add("theme_id_invalid_utf8", 64, b"\xc3", "INVALID_UTF8")
    add("background_id_set", 66, b"paper-01", "OK", value="paper-01")
    add("background_id_33_bytes", 66, b"a" * 33, "TOO_LARGE")
    add("background_id_on_lcd", 66, b"", "OK", variant="lcd", value="")
    add("brightness_off", 7, b"\x00", "OK", variant="lcd", value=0)
    add("brightness_max", 7, b"\x64", "OK", variant="lcd", value=100)
    add("brightness_101", 7, b"\x65", "OUT_OF_RANGE", variant="lcd")
    add("brightness_on_paper", 7, b"\x46", "UNKNOWN_KEY")
    add("idle_dim_never", 8, u16(0), "OK", variant="lcd", value=0)
    add("idle_dim_one_byte", 8, b"\x1e", "TYPE_MISMATCH", variant="lcd")
    add("idle_off_max", 9, u16(65535), "OK", variant="lcd", value=65535)
    add("idle_off_on_paper", 9, u16(300), "UNKNOWN_KEY")
    add("orientation_flipped_on_lcd", 2, b"\x02", "OK", variant="lcd", value=2)
    add("weather_enabled_off", 80, b"\x00", "OK", variant="lcd", value=False)
    add("weather_enabled_on_paper", 80, b"\x01", "OK", value=True)
    add("weather_city_on_paper", 81, "Hà Nội".encode(), "OK", value="Hà Nội")
    add("weather_interval_on_paper", 82, u16(240), "OK", value=240)
    add("weather_city_vietnamese", 81, "Hà Nội".encode(), "OK", variant="lcd", value="Hà Nội")
    add("weather_city_64_bytes", 81, b"c" * 64, "OK", variant="lcd", value="c" * 64)
    add("weather_city_65_bytes", 81, b"c" * 65, "TOO_LARGE", variant="lcd")
    add("weather_city_invalid_utf8", 81, b"Ha\xcc", "INVALID_UTF8", variant="lcd")
    add("weather_interval_min", 82, u16(15), "OK", variant="lcd", value=15)
    add("weather_interval_14", 82, u16(14), "OUT_OF_RANGE", variant="lcd")
    add("weather_interval_max", 82, u16(240), "OK", variant="lcd", value=240)
    add("weather_interval_241", 82, u16(241), "OUT_OF_RANGE", variant="lcd")
    add("unknown_key", 0x9999, b"\x01", "UNKNOWN_KEY")
    assert K
    return {"description": "Config value validation (protocol.md 5). A board of the given variant must return "
                           "'expect' for value_hex written to key. 'value' is the typed logical value.",
            "scope": "app+board", "cases": cases}


# --------------------------------------------------------------------------- wifi

def gen_wifi() -> dict:
    # (name, description, has_wifi, fields or raw bytes)
    defs = [
        ("set_wpa2", "Network with a passphrase", True, {"ssid": "Quán Cafe 5G", "password": "matkhau123"}),
        ("set_open", "Open network", True, {"ssid": "CafeFree"}),
        ("set_open_empty_password", "Empty password = open network", True, {"ssid": "CafeFree", "password": ""}),
        ("set_password_8", "Shortest passphrase", True, {"ssid": "Home", "password": "12345678"}),
        ("set_password_7", "Passphrase shorter than 8 bytes", True, {"ssid": "Home", "password": "1234567"}),
        ("set_password_1", "", True, {"ssid": "Home", "password": "x"}),
        ("set_no_ssid", "No ssid and no forget", True, {"password": "12345678"}),
        ("set_forget_false_no_ssid", "forget false needs an ssid", True, {"forget": False}),
        ("set_second", "A second saved network", True, {"ssid": "Office", "password": "12345678"}),
        ("set_third", "A third saved network", True, {"ssid": "Phone", "password": "12345678"}),
        ("set_fourth_evicts_lru", "A fourth network evicts the least recently used one", True,
         {"ssid": "Travel", "password": "12345678"}),
        ("update_moves_to_mru", "Updating a saved SSID keeps three profiles and refreshes its recency", True,
         {"ssid": "Office", "password": "87654321"}),
        ("forget_one", "Forget one saved network; password is ignored", True,
         {"ssid": "Phone", "password": "1", "forget": True}),
        ("forget", "Forget all saved networks", True, {"forget": True}),
        ("no_wifi_feature", "A board without the wifi feature", False, {"ssid": "Home", "password": "12345678"}),
        ("no_wifi_feature_forget", "", False, {"forget": True}),
    ]
    cases, profiles = [], []
    for name, desc, has_wifi, fields in defs:
        b = codec.encode_message(reg, {"type": "WIFI_SET", "req_id": 1, "fields": fields})
        st = wifi.set_status(codec.decode_message(reg, b)["fields"], has_wifi)
        c = {"name": name, "description": desc, "has_wifi": has_wifi, "hex": b.hex(), "expect": st}
        if st == "OK":
            profiles = wifi.profiles_after(fields, profiles)
            c["saved_ssids"] = profiles
            if not fields.get("forget"):
                c["stored_ssid"] = fields["ssid"]
            elif "ssid" not in fields:
                c["stored_ssid"] = None
        cases.append(c)
    return {"description": "Board rules for WIFI_SET after message validation (protocol.md 5, Wi-Fi and weather). "
                           "On a secure link, a board with (has_wifi) or without the wifi feature answers WIFI_SET_RSP "
                           "with 'expect'. Cases run in order on one board: after an OK, saved_ssids is the LRU-to-MRU "
                           "profile list (at most three); DEVICE_STATUS.wifi_ssid is 'stored_ssid' when that operation "
                           "selects a network (null = absent). The password is never readable.",
            "scope": "board", "cases": cases}


# --------------------------------------------------------------------------- text

def gen_text() -> dict:
    VI = "Tiếng Việt: ăn, ơi, ừ, ạ, ỹ, Đ đ, Ở đây"
    raw_cases = [
        ("ascii", b"Hello, world!", None),
        ("empty", b"", None),
        ("vietnamese_nfc", VI.encode(), None),
        ("vietnamese_nfd", unicodedata.normalize("NFD", VI).encode(), None),
        ("marks_out_of_order", "ệ ệ ớ ử".encode(), None),
        ("combining_on_uncomposable_base", "q̃ x́".encode(), None),
        ("intermediate_composition", "E\u0306\u0327 e\u0327\u0306".encode(), None),
        ("mark_at_start", "́abc".encode(), None),
        ("zalgo_capped", ("e" + "́̂̃̄̆̇̈̉̊̋" * 3 + "!").encode(), None),
        ("emoji_single", "Hi 😀 there".encode(), None),
        ("emoji_run", "😀😀😀 ok".encode(), None),
        ("emoji_zwj_family", "a 👨‍👩‍👧 b".encode(), None),
        ("emoji_skin_tone", "👍🏽".encode(), None),
        ("flag", "🇻🇳 VN".encode(), None),
        ("keycap", "1️⃣".encode(), None),
        ("emoji_common", "Hi 😀 🎉 ❤️ ✅ ⭐ 🔥 👍".encode(), None),
        ("emoji_vietnamese_text", "Chúc mừng sinh nhật 🎂🎁".encode(), None),
        ("emoji_variation_selectors", "❤️ ☺︎ ✔️".encode(), None),
        ("emoji_zwj_with_variation", "👩‍❤️‍👨!".encode(), None),
        ("emoji_zwj_then_plain", "👩‍x a‍b".encode(), None),
        ("emoji_zwj_dangling", "👨‍".encode(), None),
        ("emoji_rainbow_flag", "🏳️‍🌈".encode(), None),
        ("emoji_subdivision_flag", "🏴\U000e0067\U000e0062\U000e0065\U000e006e\U000e0067\U000e007f".encode(), None),
        ("flags_two", "🇻🇳🇺🇸".encode(), None),
        ("flag_odd_indicator", "🇻🇳🇺 x".encode(), None),
        ("emoji_unassigned", "a\U0001faffb".encode(), None),
        ("emoji_then_cjk", "😀你😀".encode(), None),
        ("emoji_budget_truncates", "😀😀😀😀".encode(), 10),
        ("emoji_budget_exact", "😀😀".encode(), 8),
        ("cjk", "你好 world".encode(), None),
        ("cyrillic_and_greek", "Привет Ωmega".encode(), None),
        ("invalid_bytes", b"ok\xff\xfeok \xe1\x80b", None),
        ("controls", b"a\tb\x01c\x7fd\r\ne\rf\n", None),
        ("c1_control_nel", "a\u0085b".encode(), None),
        ("format_chars_removed", "soft­hyphen zero​width ﻿bom a‍b".encode(), None),
        ("dong_and_punctuation", "Giá: 50.000₫ – “ok” … € ™".encode(), None),
        ("literal_placeholder", "□😀".encode(), None),
        ("nbsp_kept", "a b".encode(), None),
        ("budget_no_truncation", b"exactly", 7),
        ("budget_truncates_ascii", b"Hello, world!", 10),
        ("budget_truncates_before_multibyte", "Đây là một tin nhắn dài".encode(), 16),
        ("budget_after_composition", unicodedata.normalize("NFD", "Việt Nam").encode(), 9),
        ("budget_tiny", "ệệệ".encode(), 2),
        ("budget_three", "ệệệ".encode(), 3),
        ("budget_zero", b"abc", 0),
    ]
    cases = []
    for name, data, budget in raw_cases:
        out = text.normalize(reg, data, budget)
        c = {"name": name, "input_hex": data.hex()}
        try:
            c["input"] = data.decode("utf-8")
        except UnicodeDecodeError:
            pass
        if budget is not None:
            c["budget"] = budget
        c["output"] = out
        c["output_hex"] = out.encode().hex()
        cases.append(c)
    # sanity: the reference equals real NFC on clean Vietnamese
    assert text.normalize(reg, unicodedata.normalize("NFD", VI).encode()) == unicodedata.normalize("NFC", VI)
    r = text.rules(reg)
    return {"description": "Board text normalisation (protocol.md 8) against the reference renderable set in "
                           "registry text.renderable_ranges and text.emoji_ranges. Board scope: the app need not implement this.",
            "scope": "board", "placeholder": f"U+{r.placeholder:04X}", "cases": cases}


# --------------------------------------------------------------------------- ANCS

def gen_ancs() -> dict:
    ns = []
    for name, pkt, expect in [
        ("added_social", bytes([0, 0, 4, 2]) + u32(0x01020304), "OK"),
        ("removed", bytes([2, 0, 6, 0]) + u32(7), "OK"),
        ("all_flags_call", bytes([0, 0x1F, 1, 1]) + u32(0xFFFFFFFF), "OK"),
        ("modified_preexisting", bytes([1, 0x04, 0, 9]) + u32(1), "OK"),
        ("reserved_category", bytes([0, 0, 200, 1]) + u32(5), "OK"),
        ("longer_packet_ok", bytes([0, 0, 4, 2]) + u32(9) + b"\x00", "OK"),
        ("short_packet", bytes([0, 0, 4, 2, 1, 0, 0]), "MALFORMED"),
    ]:
        c = {"name": name, "hex": pkt.hex(), "expect": expect}
        try:
            c["parsed"] = ancs.parse_notification_source(pkt)
            got = "OK"
        except ancs.AncsError as e:
            got = e.status
        assert got == expect
        ns.append(c)

    req_attrs = [(ancs.ATTR_APP_ID, None), (ancs.ATTR_TITLE, 128), (ancs.ATTR_SUBTITLE, 128),
                 (ancs.ATTR_MESSAGE, 1024), (ancs.ATTR_DATE, None)]
    requests = [
        {"name": "get_notification_attributes", "command": 0, "uid": 0x01020304,
         "attributes": [{"id": a, **({"max_len": m} if m else {})} for a, m in req_attrs],
         "hex": ancs.build_get_notif_attrs(0x01020304, req_attrs).hex()},
        {"name": "get_app_attributes", "command": 1, "app_id": "com.apple.MobileSMS", "attributes": [{"id": 0}],
         "hex": ancs.build_get_app_attrs(b"com.apple.MobileSMS", [0]).hex()},
        {"name": "perform_negative_action", "command": 2, "uid": 7, "action_id": 1,
         "hex": ancs.build_perform_action(7, 1).hex()},
    ]
    assert requests[0]["hex"] == "00040302010001800002800003000405"

    ids = [a for a, _ in req_attrs]
    attrs = {0: b"com.zing.zalo", 1: "Nguyễn Văn An".encode(), 2: b"", 3: "Tối nay ăn gì?".encode(), 5: b"20261011T093015"}
    full = bytes([0]) + u32(0x01020304) + b"".join(bytes([a]) + u16(len(attrs[a])) + attrs[a] for a in ids)
    app_rsp = bytes([1]) + b"com.apple.MobileSMS\x00" + bytes([0]) + u16(len("Tin nhắn".encode())) + "Tin nhắn".encode()

    def ds(name, frags, expect, command=0, uid=0x01020304, app_id=None, attr_ids=ids, capacity=2048):
        r = ancs.DataSourceReassembler(command, attr_ids, uid=uid if command == 0 else None,
                                       app_id=app_id.encode() if app_id else None, capacity=capacity)
        got, result, done_at = "NEED_MORE", None, None
        for i, f in enumerate(frags):
            try:
                res = r.feed(f)
            except ancs.AncsError as e:
                got = e.status
                done_at = i
                break
            if res is not None and result is None:
                result, got, done_at = res, "OK", i
        assert got == expect, (name, got, expect)
        c = {"name": name, "request": {"command": command, "attribute_ids": attr_ids, "capacity": capacity},
             "fragments": [f.hex() for f in frags], "expect": expect}
        if command == 0:
            c["request"]["uid"] = uid
        else:
            c["request"]["app_id"] = app_id
        if done_at is not None:
            c["at_fragment"] = done_at
        if result is not None:
            c["attributes"] = {str(k): v.hex() for k, v in result.items()}
        return c

    dsc = [
        ds("single_fragment", [full], "OK"),
        ds("split_in_header", [full[:3], full[3:]], "OK"),
        ds("split_in_length", [full[:7], full[7:]], "OK"),
        ds("byte_by_byte", [full[i:i + 1] for i in range(len(full))], "OK"),
        ds("mtu_20_fragments", [full[i:i + 20] for i in range(0, len(full), 20)], "OK"),
        ds("trailing_after_complete_ignored", [full + b"\x09\x09", b"\x01\x02"], "OK"),
        ds("incomplete", [full[:-1]], "NEED_MORE"),
        ds("wrong_uid", [bytes([0]) + u32(99) + full[5:]], "MALFORMED"),
        ds("wrong_command", [bytes([1]) + full[1:]], "MALFORMED"),
        ds("attribute_order_mismatch", [full[:5] + bytes([1]) + full[6:]], "MALFORMED"),
        ds("capacity_exceeded", [full[:40], full[40:]], "TOO_LARGE", capacity=40),
        ds("app_attributes", [app_rsp[:10], app_rsp[10:]], "OK", command=1, app_id="com.apple.MobileSMS", attr_ids=[0]),
        ds("app_attributes_wrong_app", [app_rsp], "MALFORMED", command=1, app_id="com.other", attr_ids=[0]),
    ]
    dates = []
    for name, s, expect in [
        ("valid", b"20261011T093015", "OK"), ("leap_day", b"20280229T235959", "OK"),
        ("epoch_2000", b"20000101T000000", "OK"), ("not_leap", b"20270229T120000", "OUT_OF_RANGE"),
        ("month_13", b"20261311T093015", "OUT_OF_RANGE"), ("hour_24", b"20261011T243015", "OUT_OF_RANGE"),
        ("year_1999", b"19991231T235959", "OUT_OF_RANGE"),
        ("missing_t", b"20261011 093015", "MALFORMED"), ("non_digit", b"2026101aT093015", "MALFORMED"),
        ("too_short", b"20261011T0930", "MALFORMED"), ("too_long", b"20261011T0930150", "MALFORMED"),
    ]:
        c = {"name": name, "input": s.decode(), "expect": expect}
        try:
            c["local_epoch_s"] = ancs.parse_date(s)
            got = "OK"
        except ancs.AncsError as e:
            got = e.status
        assert got == expect, (name, got)
        dates.append(c)
    return {"description": "ANCS client helpers (Apple ANCS spec). notification_source: 8-byte packets. requests: "
                           "exact Control Point bytes. data_source: feeding 'fragments' to a reassembler for 'request' "
                           "yields 'expect' at fragment index 'at_fragment' (NEED_MORE = still incomplete). dates: "
                           "yyyyMMdd'T'HHmmSS to seconds since 1970-01-01 local time.",
            "scope": "board", "notification_source": ns, "requests": requests, "data_source": dsc, "dates": dates}


# --------------------------------------------------------------------------- CRC32

def gen_crc32() -> dict:
    cases = []
    for name, data in [("empty", b""), ("check_123456789", b"123456789"), ("single_a", b"a"),
                       ("all_bytes_x4", bytes(range(256)) * 4), ("zeros_32", bytes(32))]:
        cases.append({"name": name, "hex": data.hex(), "crc32": f"{zlib.crc32(data):08x}"})
    assert cases[1]["crc32"] == "cbf43926"
    return {"description": "CRC-32 (IEEE 802.3 / zlib). Implementations must also support incremental update: "
                           "crc32(a+b) == update(crc32(a), b).", "scope": "app+board", "cases": cases}


# --------------------------------------------------------------------------- app icons

def gen_app_icons() -> dict:
    ids = [("android", "Android package name", "com.zing.zalo"),
           ("ios_upper_case", "iOS bundle id with upper case", "com.apple.MobileSMS"),
           ("case_sensitive", "Same letters in lower case: a different id (no case folding)", "com.apple.mobilesms"),
           ("ios_vietnamese_bank", "iOS bundle id", "com.vietinbank.ipay"),
           ("one_byte", "Shortest app_id", "a"),
           ("max_len", "64-byte app_id (app_entry.app_id max_len)", "vn.com.example.notifications." + "x" * 35),
           ("non_ascii", "app_id is `str`: hashed as its UTF-8 bytes", "vn.ứng-dụng.tin-nhắn")]
    cases = []
    for name, desc, app_id in ids:
        icon_id = assets.app_icon_id(app_id)
        assert len(icon_id) == 32 and set(icon_id) <= set("0123456789abcdef")
        cases.append({"name": name, "description": desc, "app_id": app_id,
                      "app_id_hex": app_id.encode().hex(), "icon_id": icon_id})
    assert len(ids[5][2].encode()) == 64
    assert cases[1]["icon_id"] != cases[2]["icon_id"]
    pad = ["#..........#", ".##########.", "#.#.#.#.#.#."]
    icons = [{"name": "paper_24", "description": "Paper app icon (24x24, 72-byte payload, 88-byte file); "
                                                   "transfer.json `app_icon_install` uploads this file",
              "rows": BUBBLE, "hex": ICON.hex()},
             {"name": "row_padding", "description": "Packing only (not a Paper icon size): 12 pixels per row = "
                                                     "2 bytes, the 4 padding bits are 0",
              "rows": pad, "hex": assets.app_icon(pad).hex()}]
    for ic in icons:
        ic["width"], ic["height"] = len(ic["rows"][0]), len(ic["rows"])
    small = [[(255, 0, 0, 255), (0, 255, 0, 128), (0, 0, 255, 0)], [(255, 255, 255, 255), (8, 4, 8, 1), (0, 0, 0, 255)]]
    colour = [{"name": "packing_3x2", "description": "Packing only: 3x2 pixels, RGB565 plane (little-endian) then "
                                                     "the alpha plane; (8, 4, 8) truncates to 0x0821",
               "pixels": small, "hex": assets.app_icon_rgb565a8(small).hex()},
              {"name": "lcd_32", "description": "LCD app icon (32x32, 3,072-byte payload, 3,088-byte file); "
                                                    "transfer.json `lcd_app_icon` uploads this file",
               "pixels": _lcd_icon_pixels(), "hex": LCD_ICON.hex()}]
    for ic in colour:
        ic["width"], ic["height"] = len(ic["pixels"][0]), len(ic["pixels"])
        ic["pixels"] = [["%02x%02x%02x%02x" % px for px in row] for row in ic["pixels"]]
    return {"description": "App icons (asset-formats.md `app_icon`). `removal` (since 0.12.0, board only; the app "
                           "ignores it): the board deletes an app's icon when the app leaves the seen list "
                           "(protocol.md 6.4); see `removal.description`. `ids`: the asset id of an app's icon is "
                           "app_icon_id(app_id) = lower-case hex of the first 16 bytes of SHA-256 over the "
                           "app_id's UTF-8 bytes (`app_id_hex`), exactly as the board reports the app_id, without "
                           "case folding. Both sides compute it. `icons`: `app_icon` + `mono1` files; `rows` are "
                           "top to bottom, '#' = black (bit 0), '.' = white (bit 1); encoding them must give "
                           "exactly `hex` (16-byte container header + payload). `rgb565a8_icons` (since 0.8.0): "
                           "`app_icon` + `rgb565a8` files; `pixels` are rows top to bottom of 'rrggbbaa' (8-bit "
                           "channels); RGB565 keeps the top 5/6/5 bits; encoding them must give exactly `hex`.",
            "scope": "app+board", "ids": cases, "icons": icons, "rgb565a8_icons": colour,
            "removal": gen_icon_removal()}


def gen_icon_removal() -> dict:
    """Board scenarios (protocol.md 5 and 6.4): which icons are installed after the steps. The reference model
    computes `icons_after` and `apps_after`; each case also states its expectation, so a model change cannot
    silently rewrite the vectors."""
    n = 16
    fill = [f"com.app{i:02d}" for i in range(n)]
    A, B, Z = "com.a", "com.b", "com.never.seen"

    def seen(a):
        return {"op": "seen", "app_id": a}

    def icon(a):
        return {"op": "icon", "app_id": a}

    def forget(*ids):
        return {"op": "forget", "app_ids": list(ids)}

    cases = [
        ("forget_one", "APPS_FORGET names one app: its icon goes, the other app keeps its icon",
         [seen(A), seen(B), icon(A), icon(B), forget(A)], [B], [B]),
        ("forget_all", "APPS_FORGET with no ids forgets every app and their icons",
         [seen(A), seen(B), icon(A), icon(B), forget()], [], []),
        ("forget_without_icon", "An app that never got an icon is forgotten without any error",
         [seen(A), forget(A)], [], []),
        ("forget_unknown_app_keeps_its_icon", "An icon uploaded for an app that is not in the list (a curated pack) "
         "is not touched by APPS_FORGET naming that app",
         [seen(A), icon(A), icon(Z), forget(Z)], [A, Z], [A]),
        ("forget_all_keeps_icons_of_unseen_apps", "Forgetting all deletes the icons of the listed apps only",
         [seen(A), icon(A), icon(Z), forget()], [Z], []),
        ("forget_app_with_policy", "An app with an explicit policy is forgotten like any other, icon included",
         [seen(A), {"op": "policy", "app_id": A, "policy": 2}, icon(A), forget(A)], [], []),
        ("seen_again_has_no_icon", "After a forget the app is new again: no icon until it is uploaded again",
         [seen(A), icon(A), forget(A), seen(A)], [], [A]),
        ("evicted_app_loses_its_icon", "The list is full: the least recently seen app is evicted for a new "
         "one and its icon is deleted",
         [seen(a) for a in fill] + [icon(a) for a in fill] + [seen("com.new")], fill[1:], fill[1:] + ["com.new"]),
        ("recently_seen_app_is_not_evicted", "A notification renews an app's recency, so the next one in line "
         "is evicted",
         [seen(a) for a in fill] + [icon(a) for a in fill] + [seen(fill[0]), seen("com.new")],
         fill[:1] + fill[2:], fill[2:] + [fill[0], "com.new"]),
        ("evicted_skips_explicit_policy", "An app with an explicit policy is never evicted: the next app in "
         "line loses its icon",
         [seen(a) for a in fill] + [{"op": "policy", "app_id": fill[0], "policy": 1}] + [icon(a) for a in fill] +
         [seen("com.new")], fill[:1] + fill[2:], fill[:1] + fill[2:] + ["com.new"]),
    ]
    out = []
    for name, desc, steps, want_icons, want_apps in cases:
        m = apps.SeenApps(n)
        for st in steps:
            if st["op"] == "seen":
                m.seen(st["app_id"])
            elif st["op"] == "icon":
                m.install_icon(st["app_id"])
            elif st["op"] == "policy":
                m.set_policy(st["app_id"], st["policy"])
            else:
                m.forget(st["app_ids"])
        assert m.icons == {assets.app_icon_id(a) for a in want_icons}, name
        assert set(m.apps) == set(want_apps), name
        out.append({"name": name, "description": desc, "steps": steps,
                    "icons_after": sorted(m.icons), "icon_app_ids_after": want_icons,
                    "apps_after": want_apps})
    return {"description": "Board only. A board whose seen list holds `seen_apps` entries starts empty. Steps: "
                           "`seen` = a notification from the app (NOTIF_POST on the notification channel, so the "
                           "app is added and may evict the least recently seen app without an explicit policy), "
                           "`icon` = the app's icon is installed (a completed upload; the asset id is "
                           "app_icon_id(app_id)), `policy` = APPS_SET_POLICY, `forget` = APPS_FORGET (no ids = "
                           "all). After the steps ASSET_LIST (asset_type app_icon) lists exactly `icons_after`, and "
                           "APPS_LIST exactly `apps_after` (order not significant).",
            "seen_apps": n, "cases": out}


# --------------------------------------------------------------------------- transfer scenarios

def _merge(base: dict, over: dict) -> dict:
    out = copy.deepcopy(base)
    for k, v in over.items():
        out[k] = _merge(out[k], v) if isinstance(v, dict) and isinstance(out.get(k), dict) else copy.deepcopy(v)
    return out


class Scenario:
    """Client-side script; the reference board model computes every board message, and each step
    declares the statuses it expects so a model change cannot silently rewrite the vectors."""

    def __init__(self, name: str, description: str, board: dict | None = None):
        self.case: dict = {"name": name, "description": description}
        if board:
            self.case["board"] = board
        self.model = transfer.BoardModel(reg, _merge(transfer.DEFAULT_BOARD, board or {}))
        self.steps: list[dict] = []
        self.req_id = 0

    def req(self, type_name: str, fields: dict, want: str | None) -> list[dict]:
        if type_name == "XFER_CHUNK":
            req_id = 0
        else:
            self.req_id += 1
            req_id = self.req_id
        logical = {"ver": reg.proto_major, "type": type_name, "req_id": req_id, "fields": fields}
        raw = codec.encode_message(reg, logical)
        assert codec.decode_message(reg, raw) == logical, (self.case["name"], logical)
        outs = self.model.handle(logical)
        got = [reg.status_by_id[o["fields"]["status"]] for o in outs]
        if want is not ANY_STATUS:  # chunk helpers check (offset, status) themselves
            expected = [] if want is None else [want] if isinstance(want, str) else want
            assert got == expected, (self.case["name"], type_name, got, expected)
        self.steps.append({"op": "request", "logical": logical, "hex": raw.hex(),
                           "expect": [{"logical": o, "hex": codec.encode_message(reg, o).hex()} for o in outs]})
        return outs

    def op(self, name: str, **kw) -> None:
        getattr(self.model, name)(**kw)
        self.steps.append({"op": name, **kw})

    def begin(self, kind: int, ident: str, data: bytes, want: str = "OK", **extra) -> dict:
        f = {"kind": kind, "id": ident, "size": len(data), "crc32": zlib.crc32(data),
             "sha256": hashlib.sha256(data).hexdigest(), **extra}
        f = {k: v for k, v in f.items() if v is not None}
        return self.req("XFER_BEGIN", f, want)[0]["fields"]

    def asset(self, ident: str, data: bytes, want: str = "OK", **extra) -> dict:
        return self.begin(1, ident, data, want, asset_type=THEME, **extra)

    def chunks(self, handle: int, data: bytes, start: int, end: int, acks: list[tuple[int, str]]) -> None:
        """Send data[start:end] in max_chunk pieces; `acks` are the expected (offset, status) acks."""
        got = []
        step = self.model.b["max_chunk"]
        for off in range(start, end, step):
            piece = data[off:min(off + step, end)]
            for o in self.req("XFER_CHUNK", {"handle": handle, "offset": off, "data": piece.hex()}, ANY_STATUS):
                got.append((o["fields"]["offset"], reg.status_by_id[o["fields"]["status"]]))
        assert got == acks, (self.case["name"], got, acks)

    def chunk(self, handle: int, offset: int, piece: bytes, want: tuple[int, str] | None) -> None:
        outs = self.req("XFER_CHUNK", {"handle": handle, "offset": offset, "data": piece.hex()}, ANY_STATUS)
        got = [(o["fields"]["offset"], reg.status_by_id[o["fields"]["status"]]) for o in outs]
        assert got == ([want] if want else []), (self.case["name"], got, want)

    def finish(self, handle: int, want: str) -> None:
        self.req("XFER_FINISH", {"handle": handle}, want)

    def status(self, handle: int, want: str = "OK", **expect) -> None:
        f = self.req("XFER_STATUS", {"handle": handle}, want)[0]["fields"]
        for k, v in expect.items():
            assert f[k] == v, (self.case["name"], k, f[k], v)

    def storage(self, free: int, count: int) -> None:
        f = self.req("STORAGE_INFO", {}, "OK")[0]["fields"]
        assert (f["free"], f["asset_count"]) == (free, count), (self.case["name"], f)

    def listing(self, ids: list[str], total: int | None = None, **fields) -> list[dict]:
        f = self.req("ASSET_LIST", fields, "OK")[0]["fields"]
        assert [a["id"] for a in f.get("asset", [])] == ids, (self.case["name"], f)
        assert f["total"] == (len(ids) if total is None else total)
        return f.get("asset", [])

    def done(self) -> dict:
        return {**self.case, "steps": self.steps}


ANY_STATUS = "*"
THEME, THEME_V1 = 3, 4
APP_ICON, MONO1 = assets.ASSET_TYPE_APP_ICON, transfer.FORMAT_MONO1
BACKGROUND, RGB565, RGB565A8 = transfer.ASSET_TYPE_BACKGROUND, transfer.FORMAT_RGB565, transfer.FORMAT_RGB565A8


def _icon(w: int, h: int) -> bytes:
    """A w x h app_icon file with a checkerboard."""
    return assets.app_icon(["".join("#." [(x + y) & 1] for x in range(w)) for y in range(h)])


def _blob(n: int, seed: int) -> bytes:
    return bytes((i * 37 + seed) & 0xFF for i in range(n))


def gen_transfer() -> dict:
    A = transfer.container(THEME, THEME_V1, _blob(184, 11))        # 200 bytes
    A2 = transfer.container(THEME, THEME_V1, _blob(150, 99))       # 166 bytes, same id, new content
    B = transfer.container(THEME, THEME_V1, _blob(24, 5))          # 40 bytes
    FW = _blob(300, 3)
    crcA, shaA = zlib.crc32(A), hashlib.sha256(A).hexdigest()
    out = []

    s = Scenario("asset_install", "Install a theme asset: acks every ack_window bytes and at the end, "
                 "then it is listed with its version.")
    s.storage(2048, 0)
    h = s.asset("night", A, version="1.0.0")["handle"]
    s.chunks(h, A, 0, 200, [(128, "OK"), (200, "OK")])
    s.status(h, state=transfer.ST_RECEIVING, offset=200, prefix_crc32=crcA)
    s.finish(h, "OK")
    s.status(h, state=transfer.ST_COMPLETE, offset=200, prefix_crc32=crcA)
    s.listing(["night"])
    s.storage(2048 - 200 - 5, 1)
    out.append(s.done())

    s = Scenario("resume_after_disconnect", "A disconnect journals the offset; the old handle is dead and "
                 "XFER_BEGIN with the same parameters resumes with the prefix CRC.")
    h = s.asset("night", A)["handle"]
    s.chunks(h, A, 0, 128, [(128, "OK")])
    s.op("disconnect")
    s.chunk(h, 128, A[128:192], (0, "NO_SESSION"))
    r = s.asset("night", A)
    assert (r["handle"], r["offset"], r["prefix_crc32"]) == (2, 128, zlib.crc32(A[:128]))
    s.chunks(2, A, 128, 200, [(200, "OK")])
    s.finish(2, "OK")
    out.append(s.done())

    s = Scenario("resume_after_reboot", "After a reboot only the journaled offset (every journal_every bytes) "
                 "survives; data after it is sent again.")
    h = s.asset("night", A)["handle"]
    s.chunks(h, A, 0, 192, [(128, "OK")])
    s.op("reboot")
    r = s.asset("night", A)
    assert (r["handle"], r["offset"], r["prefix_crc32"]) == (1, 128, zlib.crc32(A[:128]))
    s.chunk(1, 192, A[192:], (128, "BAD_OFFSET"))
    s.chunks(1, A, 128, 200, [(200, "OK")])
    s.finish(1, "OK")
    s.listing(["night"])
    out.append(s.done())

    s = Scenario("resume_restart_flag", "restart=true discards the journaled data (e.g. prefix_crc32 did not "
                 "match the app's file) and starts at 0.")
    h = s.asset("night", A)["handle"]
    s.chunks(h, A, 0, 128, [(128, "OK")])
    s.op("disconnect")
    r = s.asset("night", A, restart=True)
    assert (r["offset"], r["prefix_crc32"]) == (0, 0)
    s.chunks(r["handle"], A, 0, 200, [(128, "OK"), (200, "OK")])
    s.finish(r["handle"], "OK")
    out.append(s.done())

    s = Scenario("changed_file_starts_over", "Same id but a different size/crc32/sha256: no resume, the old "
                 "data is discarded.")
    h = s.asset("night", A)["handle"]
    s.chunks(h, A, 0, 128, [(128, "OK")])
    s.op("disconnect")
    r = s.asset("night", A2, version="2.0.0")
    assert r["offset"] == 0
    s.chunks(r["handle"], A2, 0, len(A2), [(128, "OK"), (166, "OK")])
    s.finish(r["handle"], "OK")
    a = s.listing(["night"])
    assert a[0]["size"] == 166 and a[0]["version"] == "2.0.0"
    out.append(s.done())

    s = Scenario("other_target_discards_journal", "BEGIN for another asset discards the journaled transfer.")
    h = s.asset("night", A)["handle"]
    s.chunks(h, A, 0, 128, [(128, "OK")])
    s.op("disconnect")
    h = s.asset("day", B)["handle"]
    s.req("XFER_ABORT", {"handle": h}, "OK")
    assert s.asset("night", A)["offset"] == 0
    s.storage(2048, 0)
    out.append(s.done())

    s = Scenario("bad_offset", "Chunks must arrive in order; the ack carries the expected offset.")
    h = s.asset("night", A)["handle"]
    s.chunk(h, 64, A[64:128], (0, "BAD_OFFSET"))
    s.chunk(h, 0, A[0:64], None)
    s.chunk(h, 0, A[0:64], (64, "BAD_OFFSET"))
    s.chunks(h, A, 64, 200, [(128, "OK"), (200, "OK")])
    s.finish(h, "OK")
    out.append(s.done())

    s = Scenario("chunk_past_end", "Data beyond size is refused with TOO_LARGE; after FINISH chunks get "
                 "NO_SESSION.")
    h = s.asset("day", B)["handle"]
    s.chunk(h, 0, B + b"\x00", (0, "TOO_LARGE"))
    s.chunk(h, 0, B, (40, "OK"))
    s.chunk(h, 40, b"\x00", (40, "TOO_LARGE"))
    s.finish(h, "OK")
    s.chunk(h, 40, b"\x00", (0, "NO_SESSION"))
    out.append(s.done())

    s = Scenario("finish_early", "FINISH before all bytes arrived is BAD_OFFSET and the session continues.")
    h = s.asset("night", A)["handle"]
    s.chunks(h, A, 0, 64, [])
    s.finish(h, "BAD_OFFSET")
    s.status(h, state=transfer.ST_RECEIVING, offset=64, prefix_crc32=zlib.crc32(A[:64]))
    s.chunks(h, A, 64, 200, [(128, "OK"), (200, "OK")])
    s.finish(h, "OK")
    out.append(s.done())

    s = Scenario("crc_mismatch", "Declared crc32 does not match the data: CRC_MISMATCH, nothing installed, "
                 "the session is failed and its data deleted.")
    h = s.begin(1, "night", A, asset_type=THEME, crc32=crcA ^ 1)["handle"]
    s.chunks(h, A, 0, 200, [(128, "OK"), (200, "OK")])
    s.finish(h, "CRC_MISMATCH")
    s.status(h, state=transfer.ST_FAILED, offset=200, prefix_crc32=crcA)
    s.finish(h, "NO_SESSION")
    s.listing([])
    s.storage(2048, 0)
    out.append(s.done())

    s = Scenario("sha_mismatch", "Declared sha256 does not match: HASH_MISMATCH; a correct retry installs.")
    bad_sha = hashlib.sha256(A + b"x").hexdigest()
    h = s.begin(1, "night", A, asset_type=THEME, sha256=bad_sha)["handle"]
    s.chunks(h, A, 0, 200, [(128, "OK"), (200, "OK")])
    s.finish(h, "HASH_MISMATCH")
    r = s.asset("night", A)
    assert r["offset"] == 0
    s.chunks(r["handle"], A, 0, 200, [(128, "OK"), (200, "OK")])
    s.finish(r["handle"], "OK")
    out.append(s.done())

    s = Scenario("image_invalid", "Integrity is fine but the asset container header is wrong (asset-formats.md).")
    for name, bad in [("magic", transfer.container(THEME, THEME_V1, _blob(24, 5), magic=b"ESPB")),
                      ("payload_len", transfer.container(THEME, THEME_V1, _blob(24, 5), payload_len=23)),
                      ("type", transfer.container(1, THEME_V1, _blob(24, 5))),
                      ("format", transfer.container(THEME, 3, _blob(24, 5))),
                      ("flags", transfer.container(THEME, THEME_V1, _blob(24, 5), flags=1)),
                      ("short", b"ESPA\x01\x03")]:
        h = s.asset("bad-" + name, bad)["handle"]
        s.chunks(h, bad, 0, len(bad), [(len(bad), "OK")])
        s.finish(h, "IMAGE_INVALID")
    s.listing([])
    s.storage(2048, 0)
    out.append(s.done())

    s = Scenario("app_icon_install", "App icons (asset_type app_icon, 24x24 mono1 as advertised in asset_support) "
                 "named app_icon_id(app_id) install like any asset, are listed, and ASSET_DELETE removes them.")
    sms = assets.app_icon_id("com.apple.MobileSMS")
    for ident in (ZALO_ICON_ID, sms):
        h = s.begin(1, ident, ICON, asset_type=APP_ICON)["handle"]
        s.chunks(h, ICON, 0, len(ICON), [(88, "OK")])
        s.finish(h, "OK")
    a = s.listing(sorted([ZALO_ICON_ID, sms]))
    assert all(e == {"asset_type": APP_ICON, "id": e["id"], "size": 88} for e in a)
    s.storage(2048 - 2 * 88, 2)
    s.req("ASSET_DELETE", {"asset_type": APP_ICON, "id": sms}, "OK")
    s.req("ASSET_DELETE", {"asset_type": THEME, "id": ZALO_ICON_ID}, "NOT_FOUND")
    s.listing([ZALO_ICON_ID])
    out.append(s.done())

    s = Scenario("app_icon_wrong_size", "An app_icon must be exactly the advertised width x height (24x24): other "
                 "sizes, or a payload that does not match them, are IMAGE_INVALID at FINISH; a file above max_size "
                 "is TOO_LARGE at BEGIN.")
    for name, bad in [("16x16", _icon(16, 16)), ("24x16", _icon(24, 16)), ("16x24", _icon(16, 24)),
                      ("payload_short", transfer.container(APP_ICON, MONO1, bytes(48), 24, 24)),
                      ("format", transfer.container(APP_ICON, transfer.FORMAT_FONT_V1, bytes(72), 24, 24))]:
        h = s.begin(1, "bad-" + name, bad, asset_type=APP_ICON)["handle"]
        s.chunks(h, bad, 0, len(bad), [(len(bad), "OK")])
        s.finish(h, "IMAGE_INVALID")
    s.begin(1, "big", _icon(32, 32), "TOO_LARGE", asset_type=APP_ICON)
    s.listing([])
    s.storage(2048, 0)
    out.append(s.done())

    lcd = {"storage_total": 8192, "max_chunk": 512, "ack_window": 1024, "journal_every": 1024,
               "display": {"width": 296, "height": 240},
               "asset_support": [{"asset_type": 5, "format": RGB565A8, "max_size": 3088, "width": 32, "height": 32}]}
    s = Scenario("lcd_app_icon", "Since 0.8.0: an LCD app icon (32x32 rgb565a8) installs; other sizes, a "
                 "payload that does not match 3 * width * height, and a format the board does not list are "
                 "IMAGE_INVALID at FINISH.", board=lcd)
    h = s.begin(1, ZALO_ICON_ID, LCD_ICON, asset_type=APP_ICON)["handle"]
    s.chunks(h, LCD_ICON, 0, len(LCD_ICON), [(1024, "OK"), (2048, "OK"), (3072, "OK"), (3088, "OK")])
    s.finish(h, "OK")
    for name, bad in [("24x24", transfer.container(APP_ICON, RGB565A8, bytes(3 * 24 * 24), 24, 24)),
                      ("payload_rgb565_only", transfer.container(APP_ICON, RGB565A8, bytes(2 * 32 * 32), 32, 32)),
                      ("mono1", _icon(32, 32))]:
        h = s.begin(1, "bad-" + name, bad, asset_type=APP_ICON)["handle"]
        s.chunks(h, bad, 0, len(bad), [(a, "OK") for a in range(1024, len(bad), 1024)] + [(len(bad), "OK")])
        s.finish(h, "IMAGE_INVALID")
    s.listing([ZALO_ICON_ID])
    out.append(s.done())

    tiny = {"display": {"width": 8, "height": 4}, "asset_support": [{"asset_type": BACKGROUND, "format": RGB565,
                                                                     "max_size": 80}]}
    s = Scenario("background_rgb565", "Since 0.8.0: a `background` + `rgb565` image is exactly the display size, "
                 "2 bytes per pixel (a tiny 8x4 display keeps the vector small; LCD is 296x240).", board=tiny)
    good = transfer.container(BACKGROUND, RGB565, assets.rgb565_pack([[(x * 32, y * 64, 255 - x * 32) for x in range(8)]
                                                                    for y in range(4)]), 8, 4)
    h = s.begin(1, "sky", good, asset_type=BACKGROUND)["handle"]
    s.chunks(h, good, 0, len(good), [(len(good), "OK")])
    s.finish(h, "OK")
    for name, bad in [("4x8", transfer.container(BACKGROUND, RGB565, bytes(64), 4, 8)),
                      ("payload_63", transfer.container(BACKGROUND, RGB565, bytes(63), 8, 4))]:
        h = s.begin(1, "bad-" + name, bad, asset_type=BACKGROUND)["handle"]
        s.chunks(h, bad, 0, len(bad), [(len(bad), "OK")])
        s.finish(h, "IMAGE_INVALID")
    s.listing(["sky"])
    out.append(s.done())

    s = Scenario("abort", "XFER_ABORT discards the data; the handle is dead afterwards.")
    h = s.asset("night", A)["handle"]
    s.chunks(h, A, 0, 128, [(128, "OK")])
    s.req("XFER_ABORT", {"handle": h}, "OK")
    s.chunk(h, 128, A[128:192], (0, "NO_SESSION"))
    s.status(h, "NO_SESSION")
    s.req("XFER_ABORT", {"handle": h}, "NO_SESSION")
    s.storage(2048, 0)
    assert s.asset("night", A)["offset"] == 0
    out.append(s.done())

    s = Scenario("begin_rejections", "XFER_BEGIN checks in protocol.md 7.2 order; rejected requests change nothing.")
    s.begin(0, "x", B, "UNSUPPORTED", asset_type=THEME)
    s.begin(3, "x", B, "UNSUPPORTED")
    s.begin(1, "x", B, "MISSING_FIELD")
    s.begin(1, "x", B, "UNSUPPORTED", asset_type=2)
    s.begin(1, "x", _blob(513, 1), "TOO_LARGE", asset_type=THEME)
    s.begin(2, "fw", FW, "INVALID_ID", version="0.2.0")
    s.begin(2, "firmware", _blob(1025, 1), "TOO_LARGE", version="0.2.0")
    s.storage(2048, 0)
    out.append(s.done())

    s = Scenario("no_space", "size above free storage is NO_SPACE; bytes of a journaled transfer that would be "
                 "discarded count as free.", board={"storage_total": 300})
    h = s.asset("night", A)["handle"]
    s.chunks(h, A, 0, 200, [(128, "OK"), (200, "OK")])
    s.finish(h, "OK")
    s.asset("big", _blob(101, 1), "NO_SPACE")
    s.asset("day", B)
    s.op("disconnect")
    s.req("ASSET_DELETE", {"asset_type": THEME, "id": "night"}, "OK")
    h = s.asset("x", _blob(250, 2))["handle"]
    s.chunks(h, _blob(250, 2), 0, 128, [(128, "OK")])
    s.op("disconnect")
    s.storage(300 - 128, 0)
    s.asset("y", _blob(301, 3), "NO_SPACE")
    s.asset("y", _blob(250, 3))  # 250 > free (172), but x's 128 journaled bytes are reclaimed
    out.append(s.done())

    s = Scenario("busy", "Another target while receiving is BUSY; the same target resumes on a new handle.")
    h = s.asset("night", A)["handle"]
    s.chunks(h, A, 0, 64, [])
    s.asset("day", B, "BUSY")
    r = s.asset("night", A)
    assert (r["handle"], r["offset"]) == (2, 64)
    s.chunk(h, 64, A[64:128], (0, "NO_SESSION"))
    s.req("XFER_ABORT", {"handle": 2}, "OK")
    s.asset("day", B)
    out.append(s.done())

    s = Scenario("replace_and_delete", "Installing the same type+id replaces it atomically; without a version "
                 "none is listed; ASSET_DELETE removes it.")
    h = s.asset("night", A, version="1.0.0")["handle"]
    s.chunks(h, A, 0, 200, [(128, "OK"), (200, "OK")])
    s.finish(h, "OK")
    h = s.asset("night", A2)["handle"]
    s.chunks(h, A2, 0, len(A2), [(128, "OK"), (166, "OK")])
    s.status(h, state=transfer.ST_RECEIVING, offset=166, prefix_crc32=zlib.crc32(A2))
    s.listing(["night"])
    s.finish(h, "OK")
    a = s.listing(["night"])
    assert a[0] == {"asset_type": THEME, "id": "night", "size": 166}
    s.storage(2048 - 166, 1)
    s.req("ASSET_DELETE", {"asset_type": THEME, "id": "night"}, "OK")
    s.req("ASSET_DELETE", {"asset_type": THEME, "id": "night"}, "NOT_FOUND")
    s.storage(2048, 0)
    out.append(s.done())

    s = Scenario("asset_list_paging", "ASSET_LIST pages by offset/limit in a stable order.")
    for ident in ("c", "a", "b"):
        h = s.asset(ident, B, version="1")["handle"]
        s.chunks(h, B, 0, 40, [(40, "OK")])
        s.finish(h, "OK")
    s.listing(["a", "b"], 3, offset=0, limit=2)
    s.listing(["c"], 3, offset=2, limit=2)
    s.listing([], 3, offset=5)
    s.listing(["a", "b", "c"])
    out.append(s.done())

    s = Scenario("firmware_update", "Firmware transfer: newer version, battery fine, image accepted.")
    h = s.begin(2, "firmware", FW, version="0.2.0")["handle"]
    s.chunks(h, FW, 0, 300, [(128, "OK"), (256, "OK"), (300, "OK")])
    s.finish(h, "OK")
    s.status(h, state=transfer.ST_COMPLETE, offset=300, prefix_crc32=zlib.crc32(FW))
    out.append(s.done())

    s = Scenario("firmware_version_rejected", "Same, older, missing or invalid versions are refused "
                 "(semver precedence: a pre-release is older than its release).")
    for v in ("0.1.0", "0.0.9", None, "1.0", "0.1.0-rc.1", "0.1.0+build.7", "v0.2.0"):
        s.begin(2, "firmware", FW, "VERSION_REJECTED", version=v)
    s.begin(2, "firmware", FW, version="0.1.1-dev.1")
    out.append(s.done())

    s = Scenario("firmware_downgrade_allowed", "A development board configured to allow downgrades.",
                 board={"firmware": {"allow_downgrade": True}})
    s.begin(2, "firmware", FW, version="0.0.9")
    out.append(s.done())

    s = Scenario("firmware_battery_gate", "LOW_BATTERY below the threshold unless on external power; FINISH "
                 "rechecks and can be retried.")
    s.op("set_battery", battery={"pct": 29, "external_power": 0})
    s.begin(2, "firmware", FW, "LOW_BATTERY", version="0.2.0")
    s.op("set_battery", battery={"pct": 29, "external_power": 1})
    h = s.begin(2, "firmware", FW, version="0.2.0")["handle"]
    s.chunks(h, FW, 0, 300, [(128, "OK"), (256, "OK"), (300, "OK")])
    s.op("set_battery", battery={"pct": 29, "external_power": 2})
    s.finish(h, "LOW_BATTERY")
    s.status(h, state=transfer.ST_RECEIVING, offset=300, prefix_crc32=zlib.crc32(FW))
    s.op("set_battery", battery={"pct": 30, "external_power": 0})
    s.finish(h, "OK")
    out.append(s.done())

    s = Scenario("firmware_battery_unknown", "A board with a battery reader that cannot read the level refuses.",
                 board={"firmware": {"battery": {"pct": None, "external_power": 2}}})
    s.begin(2, "firmware", FW, "LOW_BATTERY", version="0.2.0")
    out.append(s.done())

    s = Scenario("firmware_no_battery_reader", "Boards without a battery reader skip the battery gate.",
                 board={"firmware": {"battery": None}})
    s.begin(2, "firmware", FW, version="0.2.0")
    out.append(s.done())

    s = Scenario("firmware_image_rejected", "The image/signature check fails at FINISH.",
                 board={"firmware": {"image_status": "SIGNATURE_INVALID"}})
    h = s.begin(2, "firmware", FW, version="0.2.0")["handle"]
    s.chunks(h, FW, 0, 300, [(128, "OK"), (256, "OK"), (300, "OK")])
    s.finish(h, "SIGNATURE_INVALID")
    s.status(h, state=transfer.ST_FAILED, offset=300, prefix_crc32=zlib.crc32(FW))
    out.append(s.done())

    s = Scenario("firmware_resume_after_reboot", "Firmware resumes from the journal after a reboot.")
    h = s.begin(2, "firmware", FW, version="0.2.0")["handle"]
    s.chunks(h, FW, 0, 256, [(128, "OK"), (256, "OK")])
    s.op("reboot")
    r = s.begin(2, "firmware", FW, version="0.2.0")
    assert (r["offset"], r["prefix_crc32"]) == (256, zlib.crc32(FW[:256]))
    s.chunks(r["handle"], FW, 256, 300, [(300, "OK")])
    s.finish(r["handle"], "OK")
    out.append(s.done())

    s = Scenario("firmware_unsupported", "A board without OTA support.", board={"firmware": None})
    s.begin(2, "firmware", FW, "UNSUPPORTED", version="0.2.0")
    out.append(s.done())

    names = [c["name"] for c in out]
    assert len(set(names)) == len(names)
    return {"description": "Transfer state-machine scenarios (protocol.md 7). Replay `steps` in order against a "
                           "board configured with `board` (deep-merged with the scenario's `board` overrides; null "
                           "removes a feature). `request` steps: send `hex` (decoded: `logical`); the board must "
                           "send exactly the `expect` messages, in order (responses and XFER_ACK events). Other "
                           "ops: `disconnect`, `reboot` (power cut: RAM lost, storage kept), `set_battery` "
                           "(`battery` null = no battery reader; pct null = reading failed). Storage accounting "
                           "of the reference board: free = storage_total - sum of stored file sizes, where an "
                           "installed asset with a version also stores the version string (its length in "
                           "bytes). Firmware `image_status` is what the board's image/signature check returns. "
                           "The app can replay its side of each scenario: encode every request to `hex` and "
                           "decode every expected message.",
            "scope": "app+board", "board": transfer.DEFAULT_BOARD, "scenarios": out}


# --------------------------------------------------------------------------- releases

def gen_releases() -> dict:
    board_id = "waveshare_s3_epaper_154_v2"

    def image(bid=board_id, version="0.2.0", **extra):
        data = f"{bid}-{version}".encode()  # stand-in for the image bytes
        return {"board_id": bid, "variant": "paper", "file": f"epager-{bid}-{version}.bin",
                "size": 1234567, "sha256": hashlib.sha256(data).hexdigest(), **extra}

    def manifest(version="0.2.0", images=None, **extra):
        return {"manifest_version": 1, "version": version, "contract_version": "0.7.0",
                "images": images if images is not None else [image(version=version)], **extra}

    def edit(m, **over):
        m = copy.deepcopy(m)
        m.update(over)
        return m

    def edit_image(m, **over):
        m = copy.deepcopy(m)
        m["images"][0].update(over)
        return m

    base = manifest(notes_url="https://example.invalid/epager/releases/v0.2.0")
    no_images = manifest()
    del no_images["images"]
    manifests = [
        ("valid", base, None),
        ("valid_minimal", manifest(), None),
        ("valid_prerelease_two_boards",
         manifest("0.3.0-rc.1", [image(version="0.3.0-rc.1"), image("sim_epaper_154", "0.3.0-rc.1")]), None),
        ("valid_unknown_fields_ignored",
         edit(edit_image(base, future_image_field=1), future_field={"x": 1}), None),
        ("valid_min_app_contract", edit_image(base, min_app_contract="0.7.0"), None),
        ("missing_images", no_images, "schema"),
        ("empty_images", manifest(images=[]), "schema"),
        ("manifest_version_2", edit(base, manifest_version=2), "schema"),
        ("version_build_metadata", manifest("0.2.0+abc"), "schema"),
        ("version_leading_zero", manifest("0.02.0"), "schema"),
        ("contract_version_prerelease", edit(base, contract_version="0.7.0-rc.1"), "schema"),
        ("sha256_upper_case", edit_image(base, sha256=base["images"][0]["sha256"].upper()), "schema"),
        ("sha256_short", edit_image(base, sha256="ab" * 31), "schema"),
        ("size_zero", edit_image(base, size=0), "schema"),
        ("size_string", edit_image(base, size="1234567"), "schema"),
        ("board_id_upper_case", edit_image(base, board_id="Waveshare"), "schema"),
        ("image_missing_variant", manifest(images=[{k: v for k, v in image().items() if k != "variant"}]), "schema"),
        ("notes_url_http", edit(base, notes_url="http://example.invalid/notes"), "schema"),
        ("duplicate_board_id", manifest(images=[image(), image()]), "duplicate_board_id"),
        ("file_name_other_version", edit_image(base, file=f"epager-{board_id}-0.1.0.bin"), "file_name"),
        ("file_name_other_board", edit_image(base, file="epager-sim_epaper_154-0.2.0.bin"), "file_name"),
    ]
    manifest_cases = []
    for name, m, want in manifests:
        got = releases.manifest_problem(m)
        assert got == want, (name, got, want)
        manifest_cases.append({"name": name, "manifest": m, "problem": want})

    board = {"board_id": board_id, "variant": "paper", "firmware_version": "0.1.0"}
    pre = manifest("0.3.0-rc.1", [image(version="0.3.0-rc.1")])
    selects = [
        ("offer", base, board, "0.7.0", False, {"image": f"epager-{board_id}-0.2.0.bin"}),
        ("invalid_manifest", manifest(images=[image(), image()]), board, "0.7.0", False, {"refuse": "invalid_manifest"}),
        ("prerelease_hidden", pre, board, "0.7.0", False, {"refuse": "prerelease"}),
        ("prerelease_developer_mode", pre, board, "0.7.0", True, {"image": f"epager-{board_id}-0.3.0-rc.1.bin"}),
        ("no_image_for_board", base, {**board, "board_id": "other_board"}, "0.7.0", False, {"refuse": "no_image"}),
        ("variant_mismatch", base, {**board, "variant": "lcd"}, "0.7.0", False, {"refuse": "variant_mismatch"}),
        ("contract_major_newer", edit(base, contract_version="1.0.0"), board, "0.7.0", False, {"refuse": "contract_major"}),
        ("contract_minor_newer", edit(base, contract_version="0.9.0"), board, "0.7.0", False,
         {"image": f"epager-{board_id}-0.2.0.bin"}),
        ("app_too_old", edit_image(base, min_app_contract="0.8.0"), board, "0.7.0", False, {"refuse": "app_too_old"}),
        ("app_at_min_contract", edit_image(base, min_app_contract="0.7.0"), board, "0.7.0", False,
         {"image": f"epager-{board_id}-0.2.0.bin"}),
        ("same_version", base, {**board, "firmware_version": "0.2.0"}, "0.7.0", False, {"refuse": "not_newer"}),
        ("older_version", base, {**board, "firmware_version": "0.10.0"}, "0.7.0", False, {"refuse": "not_newer"}),
        ("release_after_its_prerelease", base, {**board, "firmware_version": "0.2.0-rc.2"}, "0.7.0", False,
         {"image": f"epager-{board_id}-0.2.0.bin"}),
        ("running_build_metadata_ignored", base, {**board, "firmware_version": "0.2.0+dirty"}, "0.7.0", False,
         {"refuse": "not_newer"}),
        ("running_version_unparsable", base, {**board, "firmware_version": "dev"}, "0.7.0", False,
         {"image": f"epager-{board_id}-0.2.0.bin"}),
    ]
    select_cases = []
    for name, m, b, app, dev, want in selects:
        got = releases.select(m, b, app, dev)
        assert got == want, (name, got, want)
        select_cases.append({"name": name, "manifest": m, "board": b, "app_contract": app,
                             "developer_mode": dev, "expect": want})
    return {"description": "Firmware release manifests (releases.md). 'manifests': 'problem' is null for a valid "
                           "manifest, else 'schema' (fails release-manifest.schema.json), 'duplicate_board_id' or "
                           "'file_name'. 'select': with the board's Capabilities fields, the app's contract version "
                           "and developer mode, the app must offer exactly 'expect' (an image file name, or the first "
                           "refusal reason in releases.md order).",
            "scope": "app", "manifests": manifest_cases, "select": select_cases}


def outputs() -> dict[Path, str]:
    files = {"messages.json": gen_messages(), "segments.json": gen_segments(), "config_values.json": gen_config_values(),
             "text.json": gen_text(), "ancs.json": gen_ancs(), "crc32.json": gen_crc32(),
             "transfer.json": gen_transfer(), "dispatch.json": gen_dispatch(), "app_icons.json": gen_app_icons(),
             "releases.json": gen_releases(), "wifi.json": gen_wifi()}
    out = {}
    for name, doc in files.items():
        doc = {"contract_version": reg.contract_version, **doc}
        out[OUT / name] = json.dumps(doc, indent=1, ensure_ascii=False) + "\n"
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()
    stale = []
    for path, content in outputs().items():
        if args.check:
            if not path.exists() or path.read_text() != content:
                stale.append(path.relative_to(R.REPO))
        else:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content)
    if stale:
        print("stale vectors (run tools/codegen/vectors.py):", *stale, sep="\n  ")
        return 1
    print("vectors up to date" if args.check else f"wrote {len(outputs())} vector files")
    return 0


if __name__ == "__main__":
    sys.exit(main())
