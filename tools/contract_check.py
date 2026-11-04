#!/usr/bin/env python3
"""Check the app/firmware contract is consistent. Exit 1 on any problem.

1. registry.json is valid against registry.schema.json and passes cross-reference checks
2. generated code/docs (tools/codegen/gen.py) and vectors (tools/codegen/vectors.py) are up to date
3. generated JSON Schemas are valid, and every logical value in the vectors validates against them
4. gatt.json UUIDs and char ids are unique, TCP ops are distinct
5. CONTRACT_VERSION is semver and has a CHANGELOG entry
6. release-manifest.schema.json is a valid schema and agrees with the releases.json vectors
7. portable components include no ESP-IDF / FreeRTOS headers
8. clock-face-gallery.schema.json is a valid schema and agrees with the gallery cases of clock_faces.json
"""
from __future__ import annotations

import json
import re
import subprocess
import sys
from pathlib import Path

import jsonschema
from referencing import Registry as RefRegistry
from referencing import Resource

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))
from epager_proto import registry as R  # noqa: E402

PORTABLE = ["protocol", "notification_pipeline", "config_store", "transfer", "core", "clock_face"]
FORBIDDEN_INCLUDE = re.compile(r'#\s*include\s*[<"](esp_|freertos/|driver/|sdkconfig|nvs|soc/|hal/)')

problems: list[str] = []


def check(cond: bool, msg: str) -> None:
    if not cond:
        problems.append(msg)


def check_registry() -> R.Registry | None:
    raw = json.loads((R.CONTRACTS / "registry.json").read_text())
    schema = json.loads((R.CONTRACTS / "registry.schema.json").read_text())
    for e in jsonschema.Draft202012Validator(schema).iter_errors(raw):
        problems.append(f"registry.json: {e.json_path}: {e.message}")
    try:
        return R.load()
    except ValueError as e:
        problems.append(str(e))
        return None


def check_generated() -> None:
    for script in ("codegen/gen.py", "codegen/vectors.py"):
        r = subprocess.run([sys.executable, str(TOOLS / script), "--check"], capture_output=True, text=True)
        check(r.returncode == 0, f"{script} --check failed:\n{r.stdout}{r.stderr}")


def check_schemas_and_vectors(reg: R.Registry) -> None:
    resources = []
    for p in sorted((R.CONTRACTS / "schemas").glob("*.schema.json")):
        doc = json.loads(p.read_text())
        jsonschema.Draft202012Validator.check_schema(doc)
        resources.append((doc["$id"], Resource.from_contents(doc)))
    refs = RefRegistry().with_resources(resources)
    base = "https://epager.local/contracts/schemas/"

    def validator(uri: str) -> jsonschema.Draft202012Validator:
        return jsonschema.Draft202012Validator({"$ref": uri}, registry=refs)

    msg_v = validator(base + "message.schema.json")
    vec = json.loads((R.CONTRACTS / "test-vectors/messages.json").read_text())
    n = 0
    for c in vec["cases"]:
        if "logical" in c:
            n += 1
            for e in msg_v.iter_errors(c["logical"]):
                problems.append(f"messages.json {c['name']}: logical does not match schema: {e.message}")
    check(n > 0, "no logical message vectors found")

    xfer = json.loads((R.CONTRACTS / "test-vectors/transfer.json").read_text())
    n = 0
    for sc in xfer["scenarios"]:
        for i, st in enumerate(sc["steps"]):
            for logical in [st.get("logical")] + [e["logical"] for e in st.get("expect", [])]:
                if logical is None:
                    continue
                n += 1
                for e in msg_v.iter_errors(logical):
                    problems.append(f"transfer.json {sc['name']} step {i}: logical does not match schema: {e.message}")
    check(n > 0, "no transfer scenario messages found")

    cfg_v = validator(base + "config.schema.json#/$defs/config_values")
    cfg = json.loads((R.CONTRACTS / "test-vectors/config_values.json").read_text())
    names = {k["id"]: k["name"] for k in reg.config_keys}
    for c in cfg["cases"]:
        if c["expect"] == "OK" and "value" in c and c["key"] in names:
            for e in cfg_v.iter_errors({names[c["key"]]: c["value"]}):
                problems.append(f"config_values.json {c['name']}: {e.message}")
    for p in sorted((R.CONTRACTS / "test-vectors").glob("*.json")):
        d = json.loads(p.read_text())
        check(d.get("contract_version") == reg.contract_version, f"{p.name}: contract_version mismatch")


def check_gatt() -> None:
    g = json.loads((R.CONTRACTS / "gatt.json").read_text())
    chars = g["service"]["characteristics"]
    uuids = [g["service"]["uuid"]] + [c["uuid"] for c in chars]
    check(len(set(u.lower() for u in uuids)) == len(uuids), "gatt.json: duplicate UUIDs")
    ids = [c["char_id"] for c in chars] + [c["char_id"] for s in g["standard_services"]
                                           for c in s["characteristics"] if "char_id" in c]
    check(len(set(ids)) == len(ids), "gatt.json: duplicate char_id")
    ops = list(g["tcp"]["ops"].values())
    check(len(set(ops)) == len(ops), "gatt.json: duplicate tcp op")
    check(g["advertising"]["name_prefix"] != "", "gatt.json: empty advertising name_prefix")


def check_releases() -> None:
    schema = json.loads((R.CONTRACTS / "release-manifest.schema.json").read_text())
    jsonschema.Draft202012Validator.check_schema(schema)
    v = jsonschema.Draft202012Validator(schema)
    vec = json.loads((R.CONTRACTS / "test-vectors/releases.json").read_text())
    for c in vec["manifests"]:
        check(v.is_valid(c["manifest"]) == (c["problem"] != "schema"),
              f"releases.json {c['name']}: schema result disagrees with problem {c['problem']!r}")


def check_clock_faces() -> None:
    schema = json.loads((R.CONTRACTS / "clock-face-gallery.schema.json").read_text())
    jsonschema.Draft202012Validator.check_schema(schema)
    v = jsonschema.Draft202012Validator(schema)
    vec = json.loads((R.CONTRACTS / "test-vectors/clock_faces.json").read_text())
    for c in vec["gallery"]:
        check(v.is_valid(c["manifest"]) == c["valid"], f"clock_faces.json gallery {c['name']}: schema result disagrees")


def check_version(reg: R.Registry) -> None:
    check(re.fullmatch(r"\d+\.\d+\.\d+", reg.contract_version) is not None, "CONTRACT_VERSION is not semver")
    log = (R.CONTRACTS / "CHANGELOG.md").read_text() if (R.CONTRACTS / "CHANGELOG.md").exists() else ""
    check(f"## {reg.contract_version}" in log, f"CHANGELOG.md has no '## {reg.contract_version}' entry")


def check_portable() -> None:
    for comp in PORTABLE:
        root = R.REPO / "components" / comp
        if not root.exists():
            continue
        for p in list(root.rglob("*.c")) + list(root.rglob("*.h")):
            if "/esp/" in str(p.relative_to(root)) or p.name.endswith("_esp.c"):
                continue  # explicitly platform-specific glue lives in esp/ or *_esp.c
            for i, line in enumerate(p.read_text().splitlines(), 1):
                if FORBIDDEN_INCLUDE.search(line):
                    problems.append(f"{p.relative_to(R.REPO)}:{i}: platform include in portable code: {line.strip()}")


def main() -> int:
    reg = check_registry()
    check_generated()
    if reg:
        check_schemas_and_vectors(reg)
        check_version(reg)
    check_gatt()
    check_releases()
    check_clock_faces()
    check_portable()
    if problems:
        print("contract check FAILED:")
        for p in problems:
            print("  -", p)
        return 1
    print(f"contract check OK (contract {reg.contract_version})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
