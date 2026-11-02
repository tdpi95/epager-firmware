"""A BlueZ pairing agent for talking to a real board from this laptop (M5, hw-checklist 4 and 38).

The board is DisplayOnly (PLAN D6): it shows a six-digit passkey and the other side types it. BlueZ asks its
default agent for that number (`RequestPasskey`); this agent is the laptop's keyboard. bleak (3.0.2) has no
agent support of its own, so the agent is registered here with dbus-fast, which bleak already depends on.

    agent = PasskeyAgent(passkey=None)   # None: ask on the terminal when BlueZ wants it
    await agent.register()
    ...                                  # BleakClient.pair() now works against the board
    await agent.unregister()

TODO(hw-verify): not run against the board yet (needs the go-ahead to flash it).
"""
import asyncio
from typing import Callable, Optional

from dbus_fast import BusType, DBusError
from dbus_fast.aio import MessageBus
from dbus_fast.service import ServiceInterface, method

AGENT_PATH = "/org/epager/agent"
CAPABILITY = "KeyboardOnly"  # we have a keyboard and no display: BlueZ picks passkey entry against a DisplayOnly board


class _Agent(ServiceInterface):
    def __init__(self, ask):
        super().__init__("org.bluez.Agent1")
        self._ask = ask

    @method()
    def Release(self) -> "":  # type: ignore[name-defined]  # noqa: F722, N802 (BlueZ's names)
        pass

    @method()
    async def RequestPasskey(self, device: "o") -> "u":  # type: ignore[name-defined]  # noqa: F821, N802
        return await self._ask(device)

    @method()
    def RequestAuthorization(self, device: "o") -> "":  # type: ignore[name-defined]  # noqa: F722, F821, N802
        pass  # "just works" requests: accept (the board never asks for them: it only offers a passkey)

    @method()
    def AuthorizeService(self, device: "o", uuid: "s") -> "":  # type: ignore[name-defined]  # noqa: F722, F821, N802
        pass

    @method()
    def Cancel(self) -> "":  # noqa: F722, N802
        pass

    @method()
    def RequestConfirmation(self, device: "o", passkey: "u") -> "":  # type: ignore[name-defined]  # noqa: F722, F821, N802
        raise DBusError("org.bluez.Error.Rejected", "numeric comparison is not used with this board")


class PasskeyAgent:
    def __init__(self, passkey: Optional[int] = None, log: Callable[[str], None] = print):
        self.passkey = passkey
        self.log = log
        self._bus: Optional[MessageBus] = None

    async def _ask(self, _device: str) -> int:
        if self.passkey is not None:
            return self.passkey
        loop = asyncio.get_running_loop()
        text = await loop.run_in_executor(None, lambda: input("Passkey shown on the board: "))
        try:
            return int(text.strip())
        except ValueError:
            raise DBusError("org.bluez.Error.Rejected", "not a number") from None

    async def register(self) -> None:
        self._bus = await MessageBus(bus_type=BusType.SYSTEM).connect()
        self._bus.export(AGENT_PATH, _Agent(self._ask))
        intro = await self._bus.introspect("org.bluez", "/org/bluez")
        manager = self._bus.get_proxy_object("org.bluez", "/org/bluez", intro).get_interface("org.bluez.AgentManager1")
        await manager.call_register_agent(AGENT_PATH, CAPABILITY)
        await manager.call_request_default_agent(AGENT_PATH)
        self.log("pairing agent registered")

    async def unregister(self) -> None:
        if self._bus is None:
            return
        try:
            intro = await self._bus.introspect("org.bluez", "/org/bluez")
            manager = self._bus.get_proxy_object("org.bluez", "/org/bluez", intro).get_interface("org.bluez.AgentManager1")
            await manager.call_unregister_agent(AGENT_PATH)
        finally:
            self._bus.disconnect()
            self._bus = None
