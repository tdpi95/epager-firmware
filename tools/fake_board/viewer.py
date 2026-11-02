"""Tk viewer for the simulated Paper or LCD panel (Python stdlib only).

Keys: Paper b = BOOT short, B = BOOT long, p = PWR short, P = PWR long; LCD u / U = volume up
short / long, d / D = volume down short / long, and the mouse is a finger (click = tap, drag = swipe);
s = snapshot, q = quit, 1-6 = scale, + / - = scale up / down (the scale buttons do the same; typing in
the post form does not trigger the keys).
The "Post notification" form injects Android or iOS (ANCS) notifications like the console's
`post` / `ancs` commands.
The status line shows the refresh in progress, refresh counts, battery, link and firmware.
"""
from __future__ import annotations

import time
import tkinter as tk
from typing import Callable

from . import png
from .board import FakeBoard
from .console import PRESETS
from .native import BTN_BOOT, BTN_DOWN, BTN_PWR, BTN_UP

ZOOMS = range(1, 7)
CATEGORIES = ("other", "incoming_call", "missed_call", "voicemail", "social", "schedule", "email", "news",
              "health_and_fitness", "business_and_finance", "location", "entertainment")


def _pnm(px: bytes, w: int, h: int, channels: int) -> bytes:
    return (b"P5" if channels == 1 else b"P6") + b" %d %d 255\n" % (w, h) + px


class Viewer:
    def __init__(self, board: FakeBoard, zoom: int = 2, on_close: Callable[[], None] | None = None):
        self.board = board
        self.zoom = zoom
        self.on_close = on_close
        self.root = tk.Tk()
        self.root.title(f"ePager fake board: {board.cfg.device_name}")
        self.root.configure(bg="#555")
        self.canvas = tk.Label(self.root, bd=0, bg="#555")
        self.canvas.pack(padx=12, pady=12)
        bar = tk.Frame(self.root, bg="#555")
        bar.pack(fill="x", padx=12)
        self.w, self.h, self.ch = board.size
        lcd = board.cfg.board == "lcd"
        buttons = ((("Up", BTN_UP, False), ("Up long", BTN_UP, True), ("Down", BTN_DOWN, False),
                    ("Down long", BTN_DOWN, True)) if lcd else
                   (("BOOT", BTN_BOOT, False), ("BOOT long", BTN_BOOT, True), ("PWR", BTN_PWR, False),
                    ("PWR long", BTN_PWR, True), ("PWR hold", BTN_PWR, 2)))
        for text, btn, long in buttons:
            tk.Button(bar, text=text, command=lambda b=btn, lp=long: self.board.button(b, lp == 1, lp == 2)).pack(side="left")
        for z in reversed(ZOOMS):
            tk.Button(bar, text=f"{z}x", command=lambda z=z: self.set_zoom(z)).pack(side="right")
        tk.Label(bar, text="scale", bg="#555", fg="white").pack(side="right", padx=(0, 4))
        self._post_form()
        self.status = tk.Label(self.root, anchor="w", justify="left", font=("TkFixedFont", 9), bg="#555", fg="white")
        self.status.pack(fill="x", padx=12, pady=(6, 12))
        keys: dict[str, Callable[[], None]] = {
            "b": lambda: self.board.button(BTN_BOOT, False), "B": lambda: self.board.button(BTN_BOOT, True),
            "p": lambda: self.board.button(BTN_PWR, False), "P": lambda: self.board.button(BTN_PWR, True),
            "H": lambda: self.board.button(BTN_PWR, False, True),
            "u": lambda: self.board.button(BTN_UP, False), "U": lambda: self.board.button(BTN_UP, True),
            "d": lambda: self.board.button(BTN_DOWN, False), "D": lambda: self.board.button(BTN_DOWN, True),
            "s": self.snapshot, "q": self.close,
            "plus": lambda: self.set_zoom(self.zoom + 1), "equal": lambda: self.set_zoom(self.zoom + 1),
            "KP_Add": lambda: self.set_zoom(self.zoom + 1),
            "minus": lambda: self.set_zoom(self.zoom - 1), "KP_Subtract": lambda: self.set_zoom(self.zoom - 1),
        }
        keys.update({str(z): lambda z=z: self.set_zoom(z) for z in ZOOMS})
        for key, action in keys.items():
            self.root.bind(f"<KeyPress-{key}>", lambda e, a=action: None if isinstance(e.widget, tk.Entry) else a())
        if lcd:  # the mouse is a finger on the touch panel
            self.canvas.bind("<ButtonPress-1>", self._finger)
            self.canvas.bind("<B1-Motion>", self._finger)
            self.canvas.bind("<ButtonRelease-1>", lambda _e: self.board.touch(False))
        self.root.protocol("WM_DELETE_WINDOW", self.close)
        self._last: bytes | None = None
        self._photo: tk.PhotoImage | None = None

    def _post_form(self) -> None:
        form = tk.LabelFrame(self.root, text="Post notification", bg="#555", fg="white")
        form.pack(fill="x", padx=12, pady=(8, 0))
        self.f_platform = tk.StringVar(value="Android")
        self.f_category = tk.StringVar(value="social")
        self.f_preset = tk.StringVar(value="preset")
        self.f_title, self.f_body = tk.StringVar(value="Xin chào"), tk.StringVar(value="Thử nghiệm")
        self.f_app, self.f_app_id = tk.StringVar(value="Zalo"), tk.StringVar(value="com.zing.zalo")
        row = tk.Frame(form, bg="#555")
        row.pack(fill="x", padx=4, pady=2)
        tk.OptionMenu(row, self.f_platform, "Android", "iOS (ANCS)").pack(side="left")
        tk.OptionMenu(row, self.f_category, *CATEGORIES).pack(side="left")
        presets = tk.OptionMenu(row, self.f_preset, *PRESETS, command=self._load_preset)
        presets.pack(side="left")
        for label, var in (("Title", self.f_title), ("Body", self.f_body), ("App", self.f_app), ("App ID", self.f_app_id)):
            r = tk.Frame(form, bg="#555")
            r.pack(fill="x", padx=4, pady=1)
            tk.Label(r, text=label, width=7, anchor="w", bg="#555", fg="white").pack(side="left")
            e = tk.Entry(r, textvariable=var)
            e.pack(side="left", fill="x", expand=True)
            e.bind("<Return>", lambda _e: self.post())
        row = tk.Frame(form, bg="#555")
        row.pack(fill="x", padx=4, pady=(2, 4))
        tk.Button(row, text="Post", command=self.post).pack(side="left")
        tk.Button(row, text="Remove last", command=self.remove_last).pack(side="left")
        self.f_result = tk.Label(row, anchor="w", bg="#555", fg="white")
        self.f_result.pack(side="left", padx=6)
        self._seq = 0
        self._posted: list[str] = []

    def _load_preset(self, name: str) -> None:
        app, app_id, title, body, cat = PRESETS[name]
        for var, val in ((self.f_app, app), (self.f_app_id, app_id), (self.f_title, title), (self.f_body, body),
                         (self.f_category, CATEGORIES[cat])):
            var.set(val)

    def post(self) -> None:
        self._seq += 1
        cat = CATEGORIES.index(self.f_category.get())
        fields = dict(app_id=self.f_app_id.get(), app_name=self.f_app.get(), title=self.f_title.get(), category=cat)
        if self.f_platform.get() == "Android":
            key = f"ui-{self._seq}"
            st, nid = self.board.android(key=key, body=self.f_body.get(), **fields)
            if st == 0:
                self._posted.append(key)
            self.f_result.configure(text=f"posted key={key} id={nid}" if st == 0 else f"refused: status {st}")
        else:
            self.board.ancs(event_id=0, uid=1000 + self._seq, message=self.f_body.get(), **fields)
            self.f_result.configure(text=f"ANCS uid={1000 + self._seq} sent")

    def remove_last(self) -> None:
        if not self._posted:
            self.f_result.configure(text="nothing to remove")
            return
        key = self._posted.pop()
        self.f_result.configure(text=f"removed {key}" if self.board.android_remove(key) == 0 else f"{key} not found")

    def _finger(self, e: tk.Event) -> None:
        x, y = e.x // self.zoom, e.y // self.zoom
        if 0 <= x < self.w and 0 <= y < self.h:
            self.board.touch(True, x, y)

    def set_zoom(self, zoom: int) -> None:
        zoom = max(ZOOMS[0], min(ZOOMS[-1], zoom))
        if zoom != self.zoom:
            self.zoom = zoom
            self._last = None  # re-render at the next refresh
            self.root.geometry("")  # let the window shrink as well as grow

    def snapshot(self) -> None:
        name = time.strftime("epager-%Y%m%d-%H%M%S.png")
        png.write(name, self.board.panel(), self.w, self.h, self.zoom, self.ch)
        print(f"[viewer] wrote {name}")

    def _refresh(self) -> None:
        panel = self.board.panel()
        if panel != self._last:
            self._last = panel
            self._photo = tk.PhotoImage(data=_pnm(panel, self.w, self.h, self.ch), format="PPM").zoom(self.zoom)
            self.canvas.configure(image=self._photo)
        d = self.board.display_state()
        s = self.board.state()
        if self.ch == 3:
            now = f"backlight {d.backlight} %" + (" (panel asleep)" if d.asleep else "")
        else:
            now = f"{d.kind} refresh {d.elapsed_ms} ms" if d.refreshing else "idle" + (" (panel asleep)" if d.asleep else "")
        batt = "?" if s["battery_pct"] > 100 else f"{s['battery_pct']} %"
        self.status.configure(text=f"display: {now}\n"
                                   f"refreshes: {d.partial} partial, {d.full} full; {d.partials_since_full} since full\n"
                                   f"battery {batt} | link {'connected' if s['connected'] else '-'} | "
                                   f"{'on' if s['powered'] else 'OFF'} | fw {s['version']} | ota {s['ota_state']}")
        self.root.after(40, self._refresh)

    def close(self) -> None:
        if self.on_close:
            self.on_close()
        self.root.destroy()

    def run(self) -> None:
        self._refresh()
        self.root.mainloop()
