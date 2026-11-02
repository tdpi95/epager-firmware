"""BLE peripheral transport for the fake board (BlueZ through bless): the ePager service from
contracts/gatt.json plus the Battery Service, so a phone (the Flutter app, nRF Connect) can talk
to the fake board over real BLE.

Limits of a laptop peripheral (documented in tools/README.md):
- bless advertises the ePager service UUID and the local name only (no ANCS solicitation, and
  BlueZ decides what goes into the scan response); iOS ANCS cannot be simulated this way.
- Device Information is BlueZ's own service (its deviceinfo plugin), not the board's values.
- The encrypted characteristics use BlueZ's "encrypt-*" flags; pairing needs a BlueZ agent
  (e.g. `bluetoothctl` with `agent on`, `default-agent`, `pairable on`).
- bless hides the ATT MTU and the peer: we read them from BlueZ's ReadValue/WriteValue options
  (`mtu`, `device`); until the first access the MTU is taken as 23. TODO(hw-verify): BlueZ 5.72
  passes `mtu` in these options.
"""
# No `from __future__ import annotations`: dbus-next reads the D-Bus signatures from annotations.
import asyncio
import json
from pathlib import Path
from typing import Any, Callable, Optional

from bless import BlessServer, GATTAttributePermissions, GATTCharacteristicProperties
from bless.backends.bluezdbus.dbus import characteristic as _bzc
from bless.backends.bluezdbus.dbus import service as _bzs
from dbus_next import BusType, Variant
from dbus_next.aio import MessageBus
from dbus_next.service import method

from . import native as N
from .board import FakeBoard

GATT = json.loads((Path(__file__).resolve().parents[2] / "contracts" / "gatt.json").read_text())
SERVICE = GATT["service"]["uuid"]
BATTERY_SERVICE = "0000180f-0000-1000-8000-00805f9b34fb"
BATTERY_LEVEL = "00002a19-0000-1000-8000-00805f9b34fb"
P, A = GATTCharacteristicProperties, GATTAttributePermissions

_access_hook: Optional[Callable[[dict], None]] = None


class _Characteristic(_bzc.BlueZGattCharacteristic):
    """bless's characteristic, but the options BlueZ passes (peer device, ATT MTU) are kept."""

    @method()
    def ReadValue(self, options: "a{sv}") -> "ay":  # type: ignore # noqa: F722 F821 N802
        if _access_hook:
            _access_hook(options)
        return self._service.app.Read(self)

    @method()
    def WriteValue(self, value: "ay", options: "a{sv}"):  # type: ignore # noqa: F722 F821 N802
        if _access_hook:
            _access_hook(options)
        self._service.app.Write(self, value)


_bzs.BlueZGattCharacteristic = _Characteristic


def _props(names: list[str]) -> GATTCharacteristicProperties:
    m = {"read": P.read, "write": P.write, "write_without_response": P.write_without_response, "notify": P.notify}
    out = P(0)
    for n in names:
        out |= m[n]
    return out


class _BleLink:
    def __init__(self, server: "BleServer"):
        self.server = server

    def notify(self, char: int, value: bytes) -> None:
        self.server.loop.call_soon_threadsafe(self.server.send, char, value)

    def drop(self) -> None:
        self.server.loop.call_soon_threadsafe(lambda: asyncio.ensure_future(self.server.disconnect_peer()))


class BleServer:
    def __init__(self, board: FakeBoard, name: Optional[str] = None, mtu: Optional[int] = None,
                 secure: bool = True, log: Callable[[str], None] = print):
        self.board = board
        self.name = name or board.cfg.device_name
        self.forced_mtu = mtu
        self.secure = secure
        self.log = log
        self.uuid_of = {c["char_id"]: c["uuid"] for c in GATT["service"]["characteristics"]}
        self.char_of = {u: i for i, u in self.uuid_of.items()} | {BATTERY_LEVEL: N.BATTERY}
        self.link = _BleLink(self)
        self.peer: Optional[str] = None
        self.mtu = 23
        self.loop: asyncio.AbstractEventLoop
        self._server: Any = None
        self._bus: Optional[MessageBus] = None
        self._watch: Optional[asyncio.Task] = None

    async def start(self) -> None:
        global _access_hook
        self.loop = asyncio.get_running_loop()
        _access_hook = self._on_access
        self._bus = await MessageBus(bus_type=BusType.SYSTEM).connect()
        s = BlessServer(name=self.name, loop=self.loop)
        s.read_request_func = self._read
        s.write_request_func = self._write
        await s.add_new_service(SERVICE)
        for c in GATT["service"]["characteristics"]:
            perms = A(0)
            if "read" in c["properties"]:
                perms |= A.readable
            if any(p.startswith("write") for p in c["properties"]):
                perms |= A.writeable
            if self.secure and c["security"] != "none":
                perms |= (A.read_encryption_required if "read" in c["properties"] else A(0))
                perms |= (A.write_encryption_required if A.writeable in perms else A(0))
            await s.add_new_characteristic(SERVICE, c["uuid"], _props(c["properties"]), None, perms)
        await s.add_new_service(BATTERY_SERVICE)
        bperm = A.readable | (A.read_encryption_required if self.secure else A(0))
        await s.add_new_characteristic(BATTERY_SERVICE, BATTERY_LEVEL, P.read | P.notify, None, bperm)
        await s.start()
        self._server = s
        self.log(f"[ble] advertising as {self.name} (service {SERVICE})")

    async def stop(self) -> None:
        global _access_hook
        if self._watch:
            self._watch.cancel()
        if self._server:
            await self._server.stop()
        if self._bus:
            self._bus.disconnect()
        _access_hook = None

    # ------------------------------------------------------------------ requests from BlueZ

    def _on_access(self, options: dict) -> None:
        dev = options.get("device")
        mtu = options.get("mtu")
        path = dev.value if isinstance(dev, Variant) else None
        if isinstance(mtu, Variant):
            self.mtu = int(mtu.value)
        if path and path != self.peer:
            if self.peer is not None:
                return  # one connection at a time
            if self.board.connect(self.link, self.forced_mtu or self.mtu):
                self.peer = path
                self.log(f"[ble] connected {path} (MTU {self.forced_mtu or self.mtu})")
                self._watch = asyncio.ensure_future(self._watch_peer(path))

    def _read(self, characteristic: Any, **_kw: Any) -> bytearray:
        char = self.char_of.get(str(characteristic.uuid).lower())
        if char is None:
            return bytearray()
        if self.peer is None:  # a read without a device option (older BlueZ): treat as connected
            self.board.connect(self.link, self.forced_mtu or self.mtu)
        return bytearray(self.board.read(self.link, char))

    def _write(self, characteristic: Any, value: Any, **_kw: Any) -> None:
        char = self.char_of.get(str(characteristic.uuid).lower())
        if char in (N.CONFIG, N.NOTIFICATION, N.TRANSFER):
            self.board.write(self.link, char, bytes(value))

    # ------------------------------------------------------------------ board -> phone

    def send(self, char: int, value: bytes) -> None:
        uuid, service = (BATTERY_LEVEL, BATTERY_SERVICE) if char == N.BATTERY else (self.uuid_of[char], SERVICE)
        c = self._server.get_characteristic(uuid)
        c.value = bytearray(value)
        self._server.update_value(service, uuid)

    async def _device_connected(self, path: str) -> bool:
        intro = await self._bus.introspect("org.bluez", path)
        props = self._bus.get_proxy_object("org.bluez", path, intro).get_interface("org.freedesktop.DBus.Properties")
        return bool((await props.call_get("org.bluez.Device1", "Connected")).value)

    async def _watch_peer(self, path: str) -> None:
        try:
            while await self._device_connected(path):
                await asyncio.sleep(1.0)
        except Exception as e:  # the device object went away
            self.log(f"[ble] watch {path}: {e}")
        if self.peer == path:
            self.peer = None
            self.board.disconnect(self.link)
            self.log(f"[ble] disconnected {path}")

    async def disconnect_peer(self) -> None:
        """The board dropped the link (reboot, power off): disconnect the phone."""
        path, self.peer = self.peer, None
        if not path:
            return
        try:
            intro = await self._bus.introspect("org.bluez", path)
            dev = self._bus.get_proxy_object("org.bluez", path, intro).get_interface("org.bluez.Device1")
            await dev.call_disconnect()
        except Exception as e:
            self.log(f"[ble] disconnect {path}: {e}")
