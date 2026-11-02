"""Reference board rules for WIFI_SET (protocol.md section 5, Wi-Fi and weather), applied after the codec
has validated the message (lengths, UTF-8, required fields)."""
from __future__ import annotations

PASSWORD_MIN = 8  # WPA2/WPA3 passphrase; empty = open network


def set_status(fields: dict, has_wifi: bool) -> str:
    """Status a board answers to a decoded WIFI_SET. Order: feature, forget, ssid, password."""
    if not has_wifi:
        return "UNSUPPORTED"
    if fields.get("forget"):
        return "OK"
    if "ssid" not in fields:
        return "MISSING_FIELD"
    pw = fields.get("password", "").encode("utf-8")
    if 0 < len(pw) < PASSWORD_MIN:
        return "OUT_OF_RANGE"
    return "OK"


def profiles_after(fields: dict, before: list[str]) -> list[str]:
    """The saved names after an OK WIFI_SET, least to most recently used/saved."""
    names = list(before)
    if fields.get("forget"):
        return [name for name in names if name != fields["ssid"]] if "ssid" in fields else []
    name = fields["ssid"]
    if name in names:
        names.remove(name)
    elif len(names) == 3:
        names.pop(0)
    names.append(name)
    return names
