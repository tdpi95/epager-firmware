"""Reference model of the board's seen-apps list and the icons that follow it (protocol.md 5, 6.4).

Only what the removal vectors need: the list with its LRU eviction (apps with an explicit policy are
kept), APPS_FORGET, and the rule that an app's icon is deleted when the app leaves the list.
"""
from __future__ import annotations

from . import assets


class SeenApps:
    def __init__(self, capacity: int):
        self.capacity = capacity
        self.apps: dict[str, dict] = {}   # app_id -> {"policy": int, "seq": int}
        self.icons: set[str] = set()      # installed app_icon ids
        self.seq = 0

    def seen(self, app_id: str) -> None:
        self.seq += 1
        if app_id in self.apps:
            self.apps[app_id]["seq"] = self.seq
            return
        if len(self.apps) >= self.capacity:
            victims = [a for a, v in self.apps.items() if v["policy"] == 0]
            assert victims, "list full of explicit policies"
            self._remove(min(victims, key=lambda a: self.apps[a]["seq"]))
        self.apps[app_id] = {"policy": 0, "seq": self.seq}

    def set_policy(self, app_id: str, policy: int) -> None:
        if app_id not in self.apps:
            self.seen(app_id)
        self.apps[app_id]["policy"] = policy

    def install_icon(self, app_id: str) -> None:
        self.icons.add(assets.app_icon_id(app_id))

    def forget(self, app_ids: list[str]) -> None:
        """No ids = all; ids that are not in the list are ignored."""
        for a in list(self.apps) if not app_ids else [a for a in app_ids if a in self.apps]:
            self._remove(a)

    def _remove(self, app_id: str) -> None:
        del self.apps[app_id]
        self.icons.discard(assets.app_icon_id(app_id))
