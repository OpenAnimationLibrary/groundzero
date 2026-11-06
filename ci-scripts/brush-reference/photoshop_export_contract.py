"""Bounded original P2711 reference-input ingestion, never source qualification.

Reads supplied files only. It neither runs Photoshop nor decodes ABR setting
enums. UI/ownership claims are operator declarations; hashes, headers, one-value
changes and opaque observation linkage are checked independently.
"""
import argparse
import contextlib
import hashlib
import json
import math
import os
from pathlib import Path
import re
import stat
import struct
import zlib

SCHEMA = "groundzero-photoshop-export-input-1"
OBSERVATIONS = "groundzero-abr-observations-1"
PROFILE = "P2711"
MAX_MANIFEST = 16 * 1024 * 1024
MAX_TOTAL = 512 * 1024 * 1024
MAX_FILES, MAX_CASES, MAX_SNAPSHOTS, MAX_GRAPH_NODES = 1024, 256, 512, 4096
FILE_LIMITS = {"abr-input": 64 * 1024 * 1024, "abr-export": 64 * 1024 * 1024,
               "ui-png": 8 * 1024 * 1024, "asset-png": 16 * 1024 * 1024,
               "observation-json": 1024 * 1024}
REQUIRED_VARIANTS = ["1", "2"] + [str(v) + "/" + str(s)
    for v in range(6, 11) for s in (1, 2)]
ID = re.compile(r"[a-z][a-z0-9-]{0,63}\Z")
FILENAME = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,119}\Z")
HEX = re.compile(r"[0-9a-f]{64}\Z")

# Acquisition labels, NOT ABR property names, control enum IDs or equations.
CONTROL_SPECS = {}
def _add(prefix, kind, unit, names):
    for name in names.split(): CONTROL_SPECS[prefix + "." + name] = (kind, unit)

_add("tip", "asset", "asset-id", "resource")
_add("tip", "number", "pixels", "diameter")
_add("tip", "number", "percent", "roundness hardness spacing")
_add("tip", "number", "degrees", "angle")
_add("tip", "bool", "boolean", "spacingEnabled flipX flipY")
_add("shape", "bool", "boolean", "enabled")
_add("shape", "number", "percent", "sizeJitter minimumDiameter tiltScale angleJitter roundnessJitter minimumRoundness")
_add("shape", "choice", "ui-label", "sizeControl angleControl roundnessControl")
_add("shape", "integer", "ui-steps", "sizeFadeSteps angleFadeSteps roundnessFadeSteps")
_add("scatter", "bool", "boolean", "enabled bothAxes")
_add("scatter", "number", "percent", "amount countJitter")
_add("scatter", "integer", "count", "count")
_add("scatter", "choice", "ui-label", "control countControl")
_add("scatter", "integer", "ui-steps", "fadeSteps countFadeSteps")
_add("transfer", "bool", "boolean", "enabled")
_add("transfer", "number", "percent", "opacityJitter flowJitter")
_add("transfer", "choice", "ui-label", "opacityControl flowControl")
_add("transfer", "integer", "ui-steps", "opacityFadeSteps flowFadeSteps")
_add("color", "bool", "boolean", "enabled applyPerTip")
_add("color", "number", "percent", "foregroundBackgroundJitter hueJitter saturationJitter brightnessJitter purity")
_add("color", "choice", "ui-label", "foregroundBackgroundControl")
_add("color", "integer", "ui-steps", "foregroundBackgroundFadeSteps")
_add("texture", "bool", "boolean", "enabled invert eachTip")
_add("texture", "asset", "asset-id", "pattern")
_add("texture", "number", "percent", "scale depth contrast brightness")
_add("texture", "number", "source-ui", "originX originY")
_add("texture", "choice", "ui-label", "blendMode")
_add("dual", "bool", "boolean", "enabled bothAxes flipX flipY")
_add("dual", "asset", "asset-id", "resource")
_add("dual", "number", "pixels", "diameter")
_add("dual", "number", "percent", "spacing scatter")
_add("dual", "integer", "count", "count")
_add("dual", "choice", "ui-label", "blendMode")
_add("options", "number", "percent", "opacity flow smoothing")
_add("options", "choice", "ui-label", "blendMode")
_add("options", "bool", "boolean", "airbrush forceSizePressure forceOpacityPressure")
_add("options", "number", "source-ui", "airbrushRate")
_add("options", "rgba", "rgba8-srgb", "foreground background")
_add("outside", "bool", "boolean", "wetEdges noise protectTexture brushPoseOverride extraBuildUp")


def require(condition, message):
    if not condition: raise ValueError(message)


def keys(value, names, label):
    require(type(value) is dict and set(value) == set(names.split()),
            label + ": missing or unknown fields")


def identifier(value):
    require(type(value) is str and ID.fullmatch(value), "invalid local fixture ID")
    return value


def plain(value, maximum=256):
    require(type(value) is str and 0 < len(value.encode("utf-8")) <= maximum,
            "missing or oversized UI/identity text")
    require(not any(ord(c) < 32 or ord(c) == 127 for c in value) and
            "@" not in value and "://" not in value and "\\" not in value and
            not value.startswith("/") and not re.search(r"[A-Za-z]:[/\\]", value) and
            not re.search(r"session_[A-Za-z0-9_-]{16,}", value),
            "private/path/control text is not permitted in reference metadata")
    return value


def sha(value):
    require(type(value) is str and HEX.fullmatch(value) and value != "0" * 64,
            "invalid or placeholder SHA-256")
    return value


def integer(value, low, high):
    require(type(value) is int and low <= value <= high, "integer outside input budget")
    return value


def number(value):
    require(type(value) in (int, float) and abs(value) <= 1e9 and math.isfinite(value),
            "nonfinite or oversized UI number")
    return value


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"),
                      ensure_ascii=True, allow_nan=False).encode()


def same(a, b):
    return canonical(a) == canonical(b)


def parse(data):
    def unique(pairs):
        out = {}
        for key, value in pairs:
            require(key not in out, "duplicate JSON key")
            out[key] = value
        return out
    try:
        result = json.loads(data, object_pairs_hook=unique,
                            parse_constant=lambda _: require(False, "nonfinite JSON"))
    except (UnicodeError, RecursionError) as error:
        raise ValueError("invalid or excessive JSON encoding/depth") from error
    nodes = 0
    def visit(value, depth=0):
        nonlocal nodes
        nodes += 1
        require(depth <= 32 and nodes <= 1000000, "JSON tree budget exceeded")
        if type(value) is dict:
            for key, child in value.items():
                require(type(key) is str and len(key) <= 256, "JSON key budget")
                visit(child, depth + 1)
        elif type(value) is list:
            for child in value: visit(child, depth + 1)
        elif type(value) in (int, float):
            require(abs(value) <= 2**53 and math.isfinite(value), "JSON number budget")
        else: require(type(value) in (str, bool, type(None)), "unknown JSON primitive")
    visit(result)
    return result


class InputDirectory:
    """Flat regular-file inventory with anchored, no-reparse reads."""
    def __init__(self, path):
        supplied = Path(path)
        require(not str(supplied).startswith(("\\\\", "//")), "local input directory required")
        self.root = supplied.resolve(strict=True)
        require(self.root.is_dir(), "input root is not a directory")
        self.directory_fd, self.handles = None, []
        if os.name == "nt":
            import ctypes
            from ctypes import wintypes
            self.ctypes = ctypes
            self.kernel = ctypes.WinDLL("kernel32", use_last_error=True)
            self.kernel.CreateFileW.argtypes = (wintypes.LPCWSTR, wintypes.DWORD,
                wintypes.DWORD, wintypes.LPVOID, wintypes.DWORD, wintypes.DWORD,
                wintypes.HANDLE)
            self.kernel.CreateFileW.restype = wintypes.HANDLE
            self.kernel.CloseHandle.argtypes = (wintypes.HANDLE,)
            self.kernel.GetFileInformationByHandleEx.argtypes = (wintypes.HANDLE,
                ctypes.c_int, wintypes.LPVOID, wintypes.DWORD)
            self.kernel.GetFileType.argtypes = (wintypes.HANDLE,)
            self.kernel.GetFileType.restype = wintypes.DWORD
            self.attribute_type = type("AttributeTag", (ctypes.Structure,), {
                "_fields_": [("attributes", wintypes.DWORD), ("tag", wintypes.DWORD)]})
            require(re.match(r"[A-Za-z]:\\", str(self.root)), "local disk path required")
            try:
                for directory in list(reversed(self.root.parents)) + [self.root]:
                    self.handles.append(self._windows_open(directory, True))
            except BaseException:
                self.close()
                raise
        else:
            self.directory_fd = os.open(self.root, os.O_RDONLY | os.O_DIRECTORY |
                os.O_CLOEXEC | os.O_NOFOLLOW | os.O_NONBLOCK)

    def _windows_open(self, path, directory):
        flags = 0x00200000 | (0x02000000 if directory else 0x08000000)
        handle = self.kernel.CreateFileW("\\\\?\\" + str(path),
            0x81 if directory else 0x80000000, 3, None, 3, flags, None)
        require(handle not in (None, self.ctypes.c_void_p(-1).value), "cannot open regular input object")
        attributes = self.attribute_type()
        valid = self.kernel.GetFileInformationByHandleEx(
            handle, 9, self.ctypes.byref(attributes), self.ctypes.sizeof(attributes))
        if (not valid or attributes.attributes & 0x400 or
                bool(attributes.attributes & 0x10) != directory or
                self.kernel.GetFileType(handle) != 1):
            self.kernel.CloseHandle(handle)
            raise ValueError("input object is a reparse point or wrong file kind")
        return handle

    def close(self):
        if self.directory_fd is not None:
            os.close(self.directory_fd)
            self.directory_fd = None
        for handle in reversed(self.handles): self.kernel.CloseHandle(handle)
        self.handles.clear()

    @contextlib.contextmanager
    def opened(self, name, maximum, inventory=True):
        require(type(name) is str and name not in ("", ".", "..") and
                not any(c in name for c in ("/", "\\", "\x00")), "unsafe flat input filename")
        if inventory:
            require(FILENAME.fullmatch(name) and ".." not in name and
                    not name.endswith("."), "unsafe inventory filename")
        if os.name == "nt":
            import msvcrt
            handle = self._windows_open(self.root / name, False)
            try: fd = msvcrt.open_osfhandle(int(handle), os.O_RDONLY | os.O_BINARY)
            except BaseException:
                self.kernel.CloseHandle(handle)
                raise
        else:
            fd = os.open(name, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW |
                         os.O_NONBLOCK, dir_fd=self.directory_fd)
        with os.fdopen(fd, "rb") as stream:
            before = os.fstat(stream.fileno())
            require(stat.S_ISREG(before.st_mode) and before.st_nlink == 1 and
                    0 < before.st_size <= maximum, "nonregular, linked or oversized input")
            yield stream, before
            after = os.fstat(stream.fileno())
            require((before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns) ==
                    (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns),
                    "input changed while being read")

    def read(self, name, maximum):
        with self.opened(name, maximum) as (stream, before):
            data = stream.read(maximum + 1)
            require(len(data) == before.st_size, "input length changed while reading")
            return data


def abr_header(data):
    require(len(data) >= 4, "truncated ABR header")
    major, second = struct.unpack(">HH", data[:4])
    if major in (1, 2):
        require(1 <= second <= 256, "legacy ABR record count outside reference budget")
        return {"major": major, "subversion": None, "legacyRecordCount": second}
    require(6 <= major <= 10 and second in (1, 2), "unsupported ABR version/subversion")
    return {"major": major, "subversion": second, "legacyRecordCount": None}


def variant(header):
    return str(header["major"]) + ("/" + str(header["subversion"]) if header["subversion"] else "")


def png_structure(data, asset=False):
    require(data[:8] == b"\x89PNG\r\n\x1a\n", "PNG signature mismatch")
    offset, chunks, image_data, geometry = 8, 0, False, None
    # Known image structure and fixed binary color/geometry declarations only.
    # ICC profiles and arbitrary ancillary payloads can contain private text.
    safe_chunks = {b"IHDR", b"PLTE", b"IDAT", b"IEND", b"tRNS",
                   b"cHRM", b"gAMA", b"sRGB", b"sBIT", b"bKGD", b"pHYs"}
    palette_entries, seen = None, set()
    while offset < len(data):
        require(offset + 12 <= len(data), "truncated PNG chunk")
        length, kind = struct.unpack(">I4s", data[offset:offset + 8])
        require(length <= len(data) - offset - 12, "PNG chunk exceeds file")
        payload = data[offset + 8:offset + 8 + length]
        crc = struct.unpack(">I", data[offset + 8 + length:offset + 12 + length])[0]
        require(zlib.crc32(kind + payload) & 0xffffffff == crc, "PNG chunk checksum mismatch")
        chunks += 1
        require(chunks <= 4096 and kind in safe_chunks,
                "unknown, profile, animated or private PNG metadata is not permitted")
        if kind != b"IDAT":
            require(kind not in seen, "duplicate PNG structure/color chunk")
            seen.add(kind)
        if chunks == 1:
            require(kind == b"IHDR" and length == 13, "PNG must start with one IHDR")
            width, height, depth, color, compression, filtering, interlace = struct.unpack(">IIBBBBB", payload)
            require(0 < width <= 8192 and 0 < height <= 8192 and depth in (8, 16) and
                    color in (0, 2, 3, 4, 6) and (color != 3 or depth == 8) and
                    compression == filtering == 0 and
                    interlace in (0, 1), "PNG geometry/format outside input envelope")
            if asset: require(width <= 4096 and height <= 4096, "original asset dimensions exceed D envelope")
            geometry = {"width": width, "height": height, "bitDepth": depth, "colorType": color}
        else: require(kind != b"IHDR", "duplicate PNG IHDR")
        if kind == b"PLTE":
            require(not image_data and color not in (0, 4) and
                    0 < length <= 768 and length % 3 == 0, "invalid PNG palette")
            palette_entries = length // 3
        elif kind == b"tRNS":
            require(not image_data and ((color == 0 and length == 2) or
                (color == 2 and length == 6) or (color == 3 and
                palette_entries is not None and 0 < length <= palette_entries)),
                "invalid PNG transparency declaration")
        elif kind in (b"cHRM", b"gAMA", b"sRGB", b"sBIT", b"bKGD", b"pHYs"):
            expected = {b"cHRM": 32, b"gAMA": 4, b"sRGB": 1,
                b"sBIT": {0: 1, 2: 3, 3: 3, 4: 2, 6: 4}[color],
                b"bKGD": 1 if color == 3 else 2 if color in (0, 4) else 6, b"pHYs": 9}[kind]
            require(not image_data and length == expected, "invalid PNG binary declaration")
        if kind == b"IDAT":
            require(color != 3 or palette_entries is not None, "indexed PNG palette missing")
            image_data = True
        offset += length + 12
        if kind == b"IEND":
            require(length == 0 and image_data and offset == len(data), "invalid PNG termination")
            return geometry
    raise ValueError("PNG IEND missing")


def opaque(value):
    require(type(value) is str and 0 < len(value) <= 2048 and len(value) % 2 == 0 and
            re.fullmatch(r"[0-9a-f]+", value), "identity must be bounded exact hexadecimal bytes")


def identity_bytes(value, kind):
    # Presets/inline generated definitions need not serialize an ID field.
    # Their file-bound structural locator can identify the record itself; no
    # fabricated GUID is required. Sample/pattern reference IDs stay exact.
    if value is None and kind in ("preset", "generated"): return
    opaque(value)


def observation_file(value):
    keys(value, "schema abrSha256 observer observations", "structural observation report")
    require(value["schema"] == OBSERVATIONS, "unknown structural observation schema")
    sha(value["abrSha256"])
    keys(value["observer"], "kind revision", "structural observer")
    require(value["observer"]["kind"] == "bounded-structural-reader" and
            type(value["observer"]["revision"]) is str and
            re.fullmatch(r"[0-9a-f]{40}", value["observer"]["revision"]), "unidentified structural reader")
    rows = value["observations"]
    require(type(rows) is list and len(rows) <= MAX_GRAPH_NODES, "observation row budget")
    result = {}
    for row in rows:
        keys(row, "localId kind opaqueHex locator", "opaque observation")
        identifier(row["localId"])
        require(row["localId"] not in result, "duplicate opaque observation identity")
        require(row["kind"] in ("preset", "sample", "pattern", "generated"), "unknown identity kind")
        identity_bytes(row["opaqueHex"], row["kind"])
        plain(row["locator"])
        result[row["localId"]] = row
    return value["abrSha256"], result


def _value(value, kind, assets):
    if kind == "bool": require(type(value) is bool, "UI checkbox must be Boolean")
    elif kind == "number": number(value)
    elif kind == "integer": integer(value, 0, 1000000000)
    elif kind == "choice": plain(value, 128)
    elif kind == "asset": require(identifier(value) in assets, "UI resource references unknown original asset")
    elif kind == "rgba":
        require(type(value) is list and len(value) == 4, "RGBA8 source color shape")
        for component in value: integer(component, 0, 255)
    else: raise ValueError("unknown UI control kind")


def evidence(ids, files, kind="ui-png"):
    require(type(ids) is list and 1 <= len(ids) <= 16, "missing readback evidence")
    require(all(type(i) is str for i in ids) and len(set(ids)) == len(ids), "repeated/invalid evidence identity")
    for item in ids:
        require(identifier(item) in files and files[item]["kind"] == kind, "wrong evidence file kind")


def control(value, kind, unit, assets, files):
    keys(value, "available enabled value unit label choices evidence", "UI control readback")
    require(type(value["available"]) is bool and type(value["enabled"]) is bool and
            value["unit"] == unit, "UI availability/unit mismatch")
    plain(value["label"], 128)
    evidence(value["evidence"], files)
    choices = value["choices"]
    require(type(choices) is list and len(choices) <= 32 and
            all(type(c) is str for c in choices) and len(set(choices)) == len(choices), "invalid UI choices inventory")
    for choice in choices: plain(choice, 128)
    if not value["available"]:
        require(value["enabled"] is False and value["value"] is None and not choices,
                "unavailable UI control must remain explicitly unobserved")
    else:
        _value(value["value"], kind, assets)
        if kind == "choice": require(value["value"] in choices, "selected UI choice absent from observed choices")
        else: require(not choices, "non-choice UI control has enum choices")


def snapshots(values, assets, files):
    require(type(values) is dict and 0 <= len(values) <= MAX_SNAPSHOTS, "UI snapshot budget")
    for name, value in values.items():
        identifier(name)
        keys(value, "controls unrepresented inventoryComplete", "UI snapshot")
        require(value["inventoryComplete"] is True and type(value["controls"]) is dict and
                set(value["controls"]) == set(CONTROL_SPECS), "incomplete D UI control inventory")
        for label, (kind, unit) in CONTROL_SPECS.items():
            control(value["controls"][label], kind, unit, assets, files)
        extra = value["unrepresented"]
        require(type(extra) is dict and len(extra) <= 128, "unrepresented UI inventory budget")
        for label, item in extra.items():
            identifier(label)
            keys(item, "active readback", "unrepresented UI control")
            require(type(item["active"]) is bool, "unknown UI active state must be explicit")
            readback = item["readback"]
            keys(readback, "available enabled value unit label choices evidence", "unrepresented readback")
            require(readback["unit"] in ("boolean", "source-ui", "ui-label"), "unknown UI unit")
            kind = {"boolean": "bool", "source-ui": "number", "ui-label": "choice"}[readback["unit"]]
            control(readback, kind, readback["unit"], assets, files)
            if not readback["available"]: require(item["active"] is False, "unobserved control cannot claim active")


def _snapshot_flat(value):
    result = dict(value["controls"])
    for name, extra in value["unrepresented"].items():
        result["extra." + name] = dict(extra["readback"], active=extra["active"])
    return result


def case_delta(parent, case, states):
    require(parent["abr"] is not None and case["abr"] is not None,
            "single-setting export pairs need both baseline and changed exports")
    before = {node["id"]: node for node in parent["presets"]}
    after = {node["id"]: node for node in case["presets"]}
    require(set(before) == set(after), "controlled pair changed membership; use a separate baseline")
    differences, derived = [], []
    for preset_id in sorted(before):
        require(before[preset_id]["name"] == after[preset_id]["name"], "controlled pair renamed a preset")
        old = _snapshot_flat(states[before[preset_id]["uiSnapshot"]])
        new = _snapshot_flat(states[after[preset_id]["uiSnapshot"]])
        require(set(old) == set(new), "controlled pair changed unknown-control inventory")
        for name in sorted(old):
            if not same(old[name]["value"], new[name]["value"]):
                row = {"preset": preset_id, "control": name,
                       "before": old[name]["value"], "after": new[name]["value"]}
                if old[name]["available"] != new[name]["available"]:
                    # A newly visible Fade-step entry is an observation of
                    # source UI state, not a second authored knob change.
                    derived.append(dict(row, field="value", cause="availability-change"))
                else:
                    differences.append(row)
            for field in ("available", "enabled", "choices", "active"):
                if field in old[name] and not same(old[name][field], new[name][field]):
                    derived.append({"preset": preset_id, "control": name, "field": field,
                                    "before": old[name][field], "after": new[name][field]})
    if not same(parent["exportOrder"], case["exportOrder"]):
        differences.append({"preset": None, "control": "export.selectionOrder",
                            "before": parent["exportOrder"], "after": case["exportOrder"]})
    require(len(differences) == 1, "controlled export must change exactly one declared UI setting")
    expected = dict(differences[0], derivedStateChanges=derived)
    require(same(case["change"], expected), "authored/derived changes do not match the snapshots")
    return expected


def source_identity(value, kind, local_id, abr_sha, files, observations):
    keys(value, "status kind opaqueHex locator evidence", "source identity")
    require(value["kind"] == kind and value["status"] in ("observed", "unresolved"), "unknown source identity state")
    if value["status"] == "unresolved":
        require(value["opaqueHex"] is None and value["evidence"] is None,
                "unresolved identity must not invent source bytes")
        plain(value["locator"])
        return False
    identity_bytes(value["opaqueHex"], kind)
    plain(value["locator"])
    report_id = identifier(value["evidence"])
    require(report_id in files and files[report_id]["kind"] == "observation-json", "identity report missing")
    report_sha, rows = observations[report_id]
    require(report_sha == abr_sha and local_id in rows and rows[local_id] == {
        "localId": local_id, "kind": kind, "opaqueHex": value["opaqueHex"],
        "locator": value["locator"]}, "identity report is not bound to this ABR/local node")
    return True


def identity_roles(value, kind, local_id, abr_roles, files, observations):
    keys(value, "input export", "input/export identities")
    unresolved = 0
    for role in ("input", "export"):
        file_id = abr_roles[role]
        if file_id is None: require(value[role] is None, "identity supplied without its ABR role")
        else: unresolved += not source_identity(value[role], kind, local_id,
            files[file_id]["sha256"], files, observations)
    return unresolved


def validate(root):
    reader = InputDirectory(root)
    try:
        manifest_bytes = reader.read("manifest.json", MAX_MANIFEST)
        data = parse(manifest_bytes)
        keys(data, "schema profile collection producer attestation files assets snapshots cases", "reference manifest")
        require(data["schema"] == SCHEMA and data["profile"] == PROFILE, "unknown Photoshop input baseline")
        identifier(data["collection"])
        keys(data["attestation"], "authorizedExecution originalAssets privacyReviewed", "operator attestation")
        require(all(v is True for v in data["attestation"].values()), "operator attestations incomplete")
        producer = data["producer"]
        keys(producer, "product version reportedVersion build platform architecture uiLanguage executableSha256 executableBytes executableMatchedRunningApp identityEvidenceSha256 presetSaveOptions", "producer")
        require(producer["product"] == "Adobe Photoshop" and producer["version"] == "27.11" and
                type(producer["reportedVersion"]) is str and
                re.fullmatch(r"27\.11(?:\.0){0,2}", producer["reportedVersion"]), "exact P2711 version required")
        plain(producer["build"], 128)
        require(producer["platform"] in ("windows", "macos") and
                producer["architecture"] in ("x86_64", "arm64") and
                producer["uiLanguage"] == "en-US" and
                producer["executableMatchedRunningApp"] is True,
                "producer platform/UI/running-image identity not declared")
        sha(producer["executableSha256"])
        integer(producer["executableBytes"], 1, 4 * 1024 * 1024 * 1024)
        keys(producer["presetSaveOptions"], "includeSize includeColor includeToolSettings", "preset-save UI options")
        require(all(type(v) is bool for v in producer["presetSaveOptions"].values()), "preset-save options must be explicit Booleans")
        rows = data["files"]
        require(type(rows) is list and 1 <= len(rows) <= MAX_FILES, "file inventory budget")
        files, observations, paths, total = {}, {}, set(), len(manifest_bytes)
        for row in rows:
            keys(row, "id path kind bytes sha256", "reference input file")
            file_id = identifier(row["id"])
            require(file_id not in files and type(row["path"]) is str and
                    row["path"] not in paths and row["path"] != "manifest.json", "duplicate input identity/path")
            require(row["kind"] in FILE_LIMITS, "unknown input file kind")
            maximum = FILE_LIMITS[row["kind"]]
            integer(row["bytes"], 1, maximum)
            sha(row["sha256"])
            total += row["bytes"]
            require(total <= MAX_TOTAL, "aggregate input byte budget")
            payload = reader.read(row["path"], maximum)
            require(len(payload) == row["bytes"] and hashlib.sha256(payload).hexdigest() == row["sha256"], "input byte identity mismatch")
            suffix = ".abr" if row["kind"].startswith("abr-") else ".png" if row["kind"].endswith("png") else ".json"
            require(row["path"].endswith(suffix), "input suffix/kind mismatch")
            info = dict(row)
            if row["kind"].startswith("abr-"): info["header"] = abr_header(payload)
            elif row["kind"].endswith("png"): info["png"] = png_structure(payload, row["kind"] == "asset-png")
            else: observations[file_id] = observation_file(parse(payload))
            files[file_id] = info
            paths.add(row["path"])
        # Private About/System Info evidence is never ingested or serialized.
        # Retain only its operator-declared digest, not a report or image path.
        sha(producer["identityEvidenceSha256"])
        used_files = set()
        used_assets, used_states = set(), set()
        assets = {}
        require(type(data["assets"]) is list and 1 <= len(data["assets"]) <= 256, "original asset inventory budget")
        public_assets = True
        for asset in data["assets"]:
            keys(asset, "id kind creation rights file definition", "original asset")
            name = identifier(asset["id"])
            require(name not in assets and asset["kind"] in ("sampled-mask", "pattern", "generated-ellipse"), "duplicate/unknown original asset")
            plain(asset["creation"], 256)
            keys(asset["rights"], "operatorOriginal license redistribution", "original-asset rights")
            require(asset["rights"]["operatorOriginal"] is True and
                    (asset["rights"]["license"], asset["rights"]["redistribution"]) in (
                        ("CC0-1.0", "permitted"),
                        ("LicenseRef-Original-Private", "private-evidence-only")), "asset ownership/license declaration missing")
            public_assets &= asset["rights"]["redistribution"] == "permitted"
            if asset["kind"] == "generated-ellipse":
                require(asset["file"] is None, "generated definition must not borrow a tip image")
                keys(asset["definition"], "diameter roundness hardness angle", "original generated ellipse")
                for value in asset["definition"].values(): number(value)
            else:
                require(asset["definition"] is None and asset["file"] in files and
                        files[asset["file"]]["kind"] == "asset-png", "original asset file missing")
                used_files.add(asset["file"])
                if asset["kind"] == "sampled-mask":
                    require(files[asset["file"]]["png"]["colorType"] in (0, 4), "sampled original must be grayscale")
            assets[name] = asset
        snapshots(data["snapshots"], assets, files)
        cases = data["cases"]
        require(type(cases) is list and 1 <= len(cases) <= MAX_CASES, "reference case budget")
        declared, deltas, unresolved, nodes = {}, [], 0, 0
        ledger = {name: {"inputFiles": [], "exportFiles": [],
                         "p2711Interpretations": [], "p2711Rejections": [], "renderAssociations": []}
                  for name in REQUIRED_VARIANTS}
        for case in cases:
            keys(case, "id parent purpose inputAbr inputProvenance abr expectedInputHeader expectedExportHeader interpretation exportOrder resources presets change", "reference case")
            name = identifier(case["id"])
            require(name not in declared and case["purpose"] in (
                "d-control-mapping", "resource-identity", "outside-envelope"), "duplicate/unknown capture purpose")
            roles = {"input": case["inputAbr"], "export": case["abr"]}
            require(any(v is not None for v in roles.values()), "reference case has no ABR bytes")
            for role, file_id in roles.items():
                expected_key = "expectedInputHeader" if role == "input" else "expectedExportHeader"
                if file_id is None:
                    require(case[expected_key] is None, "header supplied without its ABR role")
                else:
                    require(identifier(file_id) in files and files[file_id]["kind"] == "abr-" + role, "ABR role/file kind mismatch")
                    require(same(case[expected_key], files[file_id]["header"]), "declared ABR header differs from bytes")
                    used_files.add(file_id)
                    target = ledger[variant(files[file_id]["header"])]["inputFiles" if role == "input" else "exportFiles"]
                    if file_id not in target: target.append(file_id)
            provenance = case["inputProvenance"]
            if roles["input"] is None:
                require(provenance is None, "input provenance supplied without input bytes")
            else:
                keys(provenance, "kind generatorRevision producerLabel", "original input provenance")
                require(provenance["kind"] in ("independently-authored-structural", "original-producer-export"), "input provenance kind")
                if provenance["kind"] == "independently-authored-structural":
                    require(type(provenance["generatorRevision"]) is str and
                        re.fullmatch(r"[0-9a-f]{40}", provenance["generatorRevision"]), "structural input generator revision missing")
                    require(provenance["producerLabel"] is None, "structural input cannot invent a producer")
                else:
                    plain(provenance["producerLabel"])
                    require(provenance["generatorRevision"] is None, "producer export cannot claim synthetic generator")
            interpreted = case["interpretation"]
            keys(interpreted, "status uiEvidence renderAssociation rejectionReason", "P2711 interpretation")
            require(interpreted["status"] in ("pending", "loaded", "rejected"), "interpretation state")
            if interpreted["status"] == "pending":
                require(roles["export"] is None and interpreted["uiEvidence"] == [] and
                        interpreted["renderAssociation"] is None and interpreted["rejectionReason"] is None,
                        "pending input cannot claim executed output")
            else:
                evidence(interpreted["uiEvidence"], files)
                used_files.update(interpreted["uiEvidence"])
                if interpreted["status"] == "rejected":
                    require(roles["input"] is not None and roles["export"] is None and
                            interpreted["renderAssociation"] is None, "rejected input cannot claim export/render")
                    plain(interpreted["rejectionReason"], 512)
                else:
                    require(interpreted["rejectionReason"] is None, "loaded input has a rejection reason")
            association = interpreted["renderAssociation"]
            if association is not None:
                keys(association, "referenceSet manifestSha256 inputAbrSha256 producerExecutableSha256", "render association")
                require(roles["input"] is not None and interpreted["status"] == "loaded", "render association needs interpreted input")
                identifier(association["referenceSet"])
                sha(association["manifestSha256"])
                require(association["inputAbrSha256"] == files[roles["input"]]["sha256"] and
                        association["producerExecutableSha256"] == producer["executableSha256"], "render association baseline mismatch")
            if roles["input"] is not None and interpreted["status"] == "loaded":
                target = ledger[variant(files[roles["input"]]["header"])]
                target["p2711Interpretations"].append(name)
                if association is not None: target["renderAssociations"].append(association)
            if roles["input"] is not None and interpreted["status"] == "rejected":
                ledger[variant(files[roles["input"]]["header"])]["p2711Rejections"].append(name)
            resources, presets = {}, {}
            require(type(case["resources"]) is list and 1 <= len(case["resources"]) <= 256 and
                    type(case["presets"]) is list and 1 <= len(case["presets"]) <= 256, "source graph budget")
            for resource in case["resources"]:
                keys(resource, "id asset ordinals identities", "source resource")
                local_id = identifier(resource["id"])
                require(local_id not in resources and resource["asset"] in assets, "duplicate/unowned resource node")
                source_ordinals(resource["ordinals"], roles, files)
                kind = {"sampled-mask": "sample", "pattern": "pattern", "generated-ellipse": "generated"}[assets[resource["asset"]]["kind"]]
                unresolved += identity_roles(resource["identities"], kind, local_id, roles, files, observations)
                used_assets.add(resource["asset"])
                for identity in resource["identities"].values():
                    if identity is not None and identity["evidence"] is not None:
                        used_files.add(identity["evidence"])
                resources[local_id] = resource
            for preset in case["presets"]:
                keys(preset, "id name ordinals primary texture secondary uiSnapshot identities", "source preset")
                local_id = identifier(preset["id"])
                require(local_id not in presets and local_id not in resources, "duplicate preset ID")
                if interpreted["status"] != "loaded":
                    require(preset["uiSnapshot"] is None, "unloaded interpretation cannot claim preset UI readback")
                else:
                    require(preset["uiSnapshot"] in data["snapshots"], "executed interpretation lacks UI readback")
                    used_states.add(preset["uiSnapshot"])
                plain(preset["name"], 16384)
                require(len(preset["name"].encode("utf-16-le")) // 2 <= 4096,
                        "source preset name exceeds original input budget")
                source_ordinals(preset["ordinals"], roles, files)
                for role in ("primary", "texture", "secondary"):
                    ref = preset[role]
                    require((role != "primary" and ref is None) or ref in resources, "unresolved local graph edge")
                    if ref is not None:
                        expected = ("pattern",) if role == "texture" else ("sampled-mask", "generated-ellipse")
                        require(assets[resources[ref]["asset"]]["kind"] in expected, "wrong asset kind for preset role")
                if preset["uiSnapshot"] is not None:
                    controls = data["snapshots"][preset["uiSnapshot"]]["controls"]
                    for key, role in (("tip.resource", "primary"), ("texture.pattern", "texture"), ("dual.resource", "secondary")):
                        ref = preset[role]
                        if controls[key]["available"]:
                            require(ref is not None and controls[key]["value"] == resources[ref]["asset"],
                                    "UI-selected resource is missing or disagrees with the graph")
                unresolved += identity_roles(preset["identities"], "preset", local_id, roles, files, observations)
                for identity in preset["identities"].values():
                    if identity is not None and identity["evidence"] is not None:
                        used_files.add(identity["evidence"])
                presets[local_id] = preset
            nodes += len(resources) + len(presets)
            require(nodes <= MAX_GRAPH_NODES and type(case["exportOrder"]) is list and
                    all(type(v) is str for v in case["exportOrder"]) and
                    len(case["exportOrder"]) == len(presets) and
                    set(case["exportOrder"]) == set(presets), "graph/order budget or identity mismatch")
            if case["parent"] is None:
                require(case["change"] is None, "baseline cannot declare a parent change")
            else:
                require(identifier(case["parent"]) in declared, "parent must precede child")
                delta = dict(case=name, parent=case["parent"],
                    **case_delta(declared[case["parent"]], case, data["snapshots"]))
                previous = files[declared[case["parent"]]["abr"]]
                current = files[case["abr"]]
                delta["serializedBytesChanged"] = previous["sha256"] != current["sha256"]
                delta["sameExportHeader"] = same(previous["header"], current["header"])
                deltas.append(delta)
            declared[name] = case
        require(used_states == set(data["snapshots"]), "unreferenced UI snapshot")
        require(used_assets == set(assets), "unreferenced original asset")
        for state in data["snapshots"].values():
            for item in _snapshot_flat(state).values(): used_files.update(item["evidence"])
        require(used_files == set(files), "unreferenced reference-input file")
        unavailable = [{"snapshot": name, "control": control_id}
            for name, state in data["snapshots"].items()
            for control_id, item in _snapshot_flat(state).items() if not item["available"]]
        return {"schema": "groundzero-photoshop-input-receipt-1", "status": "inputs-validated-not-qualified",
            "profile": PROFILE, "collection": data["collection"],
            "manifestSha256": hashlib.sha256(manifest_bytes).hexdigest(),
            "producer": producer, "files": files, "caseCount": len(cases),
            "singleSettingPairs": deltas, "containerEvidence": ledger,
            "unresolvedSourceIdentities": unresolved, "unavailableUiControls": unavailable,
            "originalAssetRedistributionDeclaredPermitted": public_assets,
            "sourceIdentityObservationsAreDeclared": True,
            "executableFingerprintIsOperatorDeclared": True,
            "renderAssociationsAreExternalAndUnvalidated": True,
            "abrHeaderOnly": True, "metadataDeclarationsChecked": True,
            "abrBodyDecoded": False, "abrDescriptorEnumsInterpreted": False,
            "pngPixelsDecoded": False, "sourceSemanticallySupported": False,
            "sourceReferenceVerified": False, "physicalTabletValidated": False,
            "remaining": ["Independent bounded ABR structural decoding and UI/byte correspondence review",
                          "Complete D controls, declared version variants and Cartesian combinations",
                          "Matching P2711 delivered-event stroke references and physical-device evidence"]}
    except (TypeError, KeyError, OverflowError, UnicodeError, RecursionError) as error:
        raise ValueError("malformed or excessive Photoshop reference metadata") from error
    finally:
        reader.close()


def source_ordinals(value, roles, files):
    keys(value, "input export", "input/export ordinals")
    for role in ("input", "export"):
        if roles[role] is None: require(value[role] is None, "ordinal supplied without its ABR role")
        elif value[role] is not None:
            integer(value[role], 0, 255)
            count = files[roles[role]]["header"]["legacyRecordCount"]
            if count is not None:
                require(value[role] < count, "source ordinal exceeds legacy header record count")


def fingerprint_executable(path):
    target = Path(path).resolve(strict=True)
    reader = InputDirectory(target.parent)
    try:
        digest, size = hashlib.sha256(), 0
        with reader.opened(target.name, 4 * 1024 * 1024 * 1024, inventory=False) as (stream, before):
            while True:
                chunk = stream.read(1024 * 1024)
                if not chunk: break
                size += len(chunk)
                require(size <= before.st_size, "executable grew during fingerprinting")
                digest.update(chunk)
            require(size == before.st_size, "executable length changed")
        return {"sha256": digest.hexdigest(), "bytes": size,
                "executed": False, "pathRetained": False}
    finally:
        reader.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    ingest = sub.add_parser("ingest", help="Read supplied originals and print an unqualified receipt")
    ingest.add_argument("directory", type=Path)
    fingerprint = sub.add_parser("fingerprint-executable", help="Hash the selected image without executing/copying it")
    fingerprint.add_argument("path", type=Path)
    sub.add_parser("describe", help="Print mandatory acquisition labels/bounds, not source settings")
    args = parser.parse_args()
    if args.command == "describe":
        value = {"schema": SCHEMA, "profile": PROFILE, "version": "27.11",
                 "controls": {key: {"kind": kind, "unit": unit} for key, (kind, unit) in CONTROL_SPECS.items()},
                 "requiredAbrVariants": REQUIRED_VARIANTS,
                 "limits": {"files": MAX_FILES, "cases": MAX_CASES, "snapshots": MAX_SNAPSHOTS,
                            "graphNodes": MAX_GRAPH_NODES, "manifestBytes": MAX_MANIFEST,
                            "aggregateBytes": MAX_TOTAL}, "qualification": False}
    elif args.command == "fingerprint-executable": value = fingerprint_executable(args.path)
    else: value = validate(args.directory)
    print(json.dumps(value, indent=2, sort_keys=True, ensure_ascii=True, allow_nan=False))


if __name__ == "__main__":
    try:
        main()
    except OSError:
        raise SystemExit("Photoshop reference input rejected: cannot read a bounded regular local input") from None
    except (ValueError, OverflowError, TypeError, KeyError, RecursionError) as error:
        raise SystemExit("Photoshop reference input rejected: " + str(error)) from None
