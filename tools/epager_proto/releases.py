"""Reference rules for firmware release manifests (contracts/releases.md)."""
from __future__ import annotations

import json

import jsonschema

from . import registry as R
from .transfer import compare_versions, parse_version

SCHEMA = json.loads((R.CONTRACTS / "release-manifest.schema.json").read_text())
_VALIDATOR = jsonschema.Draft202012Validator(SCHEMA)


def manifest_problem(m: object) -> str | None:
    """None if the manifest is valid, else `schema`, `duplicate_board_id` or `file_name`."""
    if not _VALIDATOR.is_valid(m):
        return "schema"
    assert isinstance(m, dict)
    ids = [i["board_id"] for i in m["images"]]
    if len(set(ids)) != len(ids):
        return "duplicate_board_id"
    if any(i["file"] != f"epager-{i['board_id']}-{m['version']}.bin" for i in m["images"]):
        return "file_name"
    return None


def select(m: object, board: dict, app_contract: str, developer_mode: bool) -> dict:
    """The image the app may offer to a board, checked in releases.md order.

    board: Capabilities `board_id`, `variant` (enum name) and `firmware_version`.
    Returns {"image": file name} or {"refuse": reason}."""
    if manifest_problem(m) is not None:
        return {"refuse": "invalid_manifest"}
    assert isinstance(m, dict)
    version = parse_version(m["version"])
    if version[3] and not developer_mode:
        return {"refuse": "prerelease"}
    img = next((i for i in m["images"] if i["board_id"] == board["board_id"]), None)
    if img is None:
        return {"refuse": "no_image"}
    if img["variant"] != board["variant"]:
        return {"refuse": "variant_mismatch"}
    app = parse_version(app_contract)
    if parse_version(m["contract_version"])[0] != app[0]:
        return {"refuse": "contract_major"}
    if "min_app_contract" in img and compare_versions(app, parse_version(img["min_app_contract"])) < 0:
        return {"refuse": "app_too_old"}
    running = parse_version(board["firmware_version"])
    if running is not None and compare_versions(version, running) <= 0:
        return {"refuse": "not_newer"}
    return {"image": img["file"]}
