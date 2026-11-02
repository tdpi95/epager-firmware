"""Load contracts/registry.json and check the rules JSON Schema cannot express."""
from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
CONTRACTS = REPO / "contracts"

INT_WIDTH = {"u8": 1, "u16": 2, "u32": 4, "i16": 2, "i32": 4}
SIGNED = {"i16", "i32"}
ID_MAX_LEN = 32


@dataclass
class Field:
    tag: int
    name: str
    type: str
    required: bool = False
    repeated: bool = False
    min_len: int | None = None
    max_len: int | None = None
    min: int | None = None
    max: int | None = None
    struct: str | None = None
    enum: str | None = None
    bitmask: str | None = None
    description: str = ""

    @property
    def eff_min_len(self) -> int | None:
        return 1 if self.type == "id" and self.min_len is None else self.min_len

    @property
    def eff_max_len(self) -> int | None:
        return ID_MAX_LEN if self.type == "id" and self.max_len is None else self.max_len


@dataclass
class Message:
    id: int
    name: str
    kind: str
    channel: str
    fields: list[Field]
    description: str = ""


@dataclass
class Registry:
    raw: dict
    contract_version: str
    proto_major: int
    limits: dict[str, int]
    status_by_name: dict[str, int]
    status_by_id: dict[int, str]
    enums: dict
    bitmasks: dict
    structs: dict[str, list[Field]]
    messages: list[Message]
    msg_by_id: dict[int, Message] = field(default_factory=dict)
    msg_by_name: dict[str, Message] = field(default_factory=dict)
    config_keys: list[dict] = field(default_factory=list)
    text: dict = field(default_factory=dict)

    def status(self, name: str) -> int:
        return self.status_by_name[name]


def _fields(raw_fields: list[dict]) -> list[Field]:
    return [Field(**f) for f in raw_fields]


def load(contracts: Path = CONTRACTS) -> Registry:
    raw = json.loads((contracts / "registry.json").read_text())
    version = (contracts / "CONTRACT_VERSION").read_text().strip()
    structs = {k: _fields(v["fields"]) for k, v in raw["structs"].items()}
    messages = [
        Message(m["id"], m["name"], m["kind"], m["channel"], _fields(m["fields"]), m.get("description", ""))
        for m in raw["messages"]
    ]
    reg = Registry(
        raw=raw,
        contract_version=version,
        proto_major=raw["proto_major"],
        limits=raw["limits"],
        status_by_name={s["name"]: s["id"] for s in raw["status_codes"]},
        status_by_id={s["id"]: s["name"] for s in raw["status_codes"]},
        enums=raw["enums"],
        bitmasks=raw["bitmasks"],
        structs=structs,
        messages=messages,
        msg_by_id={m.id: m for m in messages},
        msg_by_name={m.name: m for m in messages},
        config_keys=raw["config_keys"],
        text=raw["text"],
    )
    check(reg)
    return reg


def check(reg: Registry) -> None:
    """Cross-reference checks. Raises ValueError with all problems found."""
    errs: list[str] = []
    if int(reg.contract_version.split(".")[0]) != reg.proto_major:
        errs.append(f"proto_major {reg.proto_major} != CONTRACT_VERSION major {reg.contract_version}")

    def check_fields(owner: str, fields: list[Field]) -> None:
        tags, names = set(), set()
        for f in fields:
            if f.tag in tags:
                errs.append(f"{owner}: duplicate tag {f.tag}")
            if f.name in names:
                errs.append(f"{owner}: duplicate field name {f.name}")
            tags.add(f.tag)
            names.add(f.name)
            if f.type == "struct" and f.struct not in reg.structs:
                errs.append(f"{owner}.{f.name}: unknown struct {f.struct}")
            if f.type != "struct" and f.struct:
                errs.append(f"{owner}.{f.name}: struct set on non-struct field")
            if f.enum and f.enum != "status" and f.enum not in reg.enums:
                errs.append(f"{owner}.{f.name}: unknown enum {f.enum}")
            if f.bitmask and f.bitmask not in reg.bitmasks:
                errs.append(f"{owner}.{f.name}: unknown bitmask {f.bitmask}")
            if (f.min is not None or f.max is not None) and f.type not in INT_WIDTH:
                errs.append(f"{owner}.{f.name}: min/max only allowed on integers")
            if (f.min_len is not None or f.max_len is not None) and f.type not in ("str", "id", "text", "bytes"):
                errs.append(f"{owner}.{f.name}: min_len/max_len only allowed on str/id/text/bytes")
            if f.type in ("str", "text", "bytes") and f.max_len is None:
                errs.append(f"{owner}.{f.name}: variable-length field needs max_len")

    for name, fields in reg.structs.items():
        check_fields(f"struct {name}", fields)
    ids = set()
    for m in reg.messages:
        if m.id in ids:
            errs.append(f"duplicate message id {m.id}")
        ids.add(m.id)
        check_fields(m.name, m.fields)
        if m.kind == "request":
            if not 0x01 <= m.id <= 0x5F:
                errs.append(f"{m.name}: request id must be 0x01-0x5F")
            if m.name != "XFER_CHUNK":
                rsp = reg.msg_by_id.get(m.id | 0x80)
                if not rsp or rsp.name != m.name + "_RSP" or rsp.kind != "response":
                    errs.append(f"{m.name}: missing response {m.name}_RSP with id {m.id | 0x80:#x}")
        if m.kind == "response":
            if not m.fields or m.fields[0].tag != 1 or m.fields[0].name != "status" or not m.fields[0].required:
                errs.append(f"{m.name}: responses must start with required tag 1 'status'")
        if m.kind == "event" and not 0x60 <= m.id <= 0x7D:
            errs.append(f"{m.name}: event id must be 0x60-0x7D")
    seen_keys = set()
    for k in reg.config_keys:
        if k["id"] in seen_keys:
            errs.append(f"duplicate config key {k['id']}")
        seen_keys.add(k["id"])
        if k["type"] == "str" and "max_len" not in k:
            errs.append(f"config {k['name']}: str needs max_len")
        if k.get("enum") and k["enum"] not in reg.enums:
            errs.append(f"config {k['name']}: unknown enum")
    if errs:
        raise ValueError("registry errors:\n  " + "\n  ".join(errs))


def hexcp(s: str) -> int:
    return int(s, 16)
