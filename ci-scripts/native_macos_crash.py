"""Read a bounded OS report for the original native process, without replay.

Identity/time fields follow Apple's crash-report formats:
https://developer.apple.com/documentation/xcode/interpreting-the-json-format-of-a-crash-report
https://developer.apple.com/documentation/xcode/examining-the-fields-in-a-crash-report
No OS setting is changed, subprocess launched, or unrelated report retained.
"""

from datetime import datetime
import json
import math
import os
from pathlib import Path
import re
import stat
import struct
import time
import uuid


MAX_REPORT_BYTES = 4 * 1024 * 1024
MAX_TOTAL_BYTES = 32 * 1024 * 1024
MAX_DIRECTORY_ENTRIES = 512
MAX_CANDIDATES = 64
MAX_THREADS = 16
MAX_FRAMES = 64
MAX_OUTPUT = 64 * 1024
MAX_MACHO_COMMAND_BYTES = 1024 * 1024


def _macho_uuid(executable):
    """Read the exact thin executable's LC_UUID, never invoke dwarfdump.

    Layout/constants: Apple's EXTERNAL_HEADERS/mach-o/loader.h. Universal/fat
    binaries deliberately have no fallback until their slice identity is read.
    """
    try:
        descriptor = os.open(executable, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0) |
                             getattr(os, "O_NONBLOCK", 0))
        with os.fdopen(descriptor, "rb") as source:
            info = os.fstat(source.fileno())
            if not stat.S_ISREG(info.st_mode):
                return None
            magic = source.read(4)
            formats = {b"\xcf\xfa\xed\xfe": ("<", 32, 8), b"\xfe\xed\xfa\xcf": (">", 32, 8),
                       b"\xce\xfa\xed\xfe": ("<", 28, 4), b"\xfe\xed\xfa\xce": (">", 28, 4)}
            if magic not in formats:
                return None
            endian, header_size, alignment = formats[magic]
            header = magic + source.read(header_size - 4)
            if len(header) != header_size:
                return None
            _, _, _, filetype, count, size, _ = struct.unpack(endian + "7I", header[:28])
            if (filetype != 2 or not 1 <= count <= 8192 or
                    not count * 8 <= size <= MAX_MACHO_COMMAND_BYTES or
                    header_size + size > info.st_size):
                return None
            commands = source.read(size)
        if len(commands) != size:
            return None
        offset, identities = 0, []
        for _ in range(count):
            if offset + 8 > size:
                return None
            command, length = struct.unpack_from(endian + "2I", commands, offset)
            if length < 8 or length % alignment or offset + length > size:
                return None
            if command == 0x1b:  # LC_UUID / uuid_command
                if length != 24:
                    return None
                identities.append(uuid.UUID(bytes=commands[offset + 8:offset + 24]))
            offset += length
        return identities[0] if offset == size and len(identities) == 1 else None
    except (OSError, ValueError, struct.error):
        return None


def _redacted_path(value, executable):
    return (isinstance(value, str) and value.startswith("/") and
            value.rsplit("/", 1)[-1] == executable.name and
            ("*" in value or value.startswith("/Users/USER/")))


def _main_image_uuid(images, expected_uuid, executable):
    if expected_uuid is None or not isinstance(images, list):
        return False
    matches = [image for image in images if isinstance(image, dict) and
               image.get("name") == executable.name]
    if len(matches) != 1:
        return False
    image = matches[0]
    path = image.get("path")
    if path is not None and not (_path_matches(path, executable) or _redacted_path(path, executable)):
        return False
    try:
        return uuid.UUID(image["uuid"]) == expected_uuid
    except (KeyError, ValueError, TypeError, AttributeError):
        return False


def _safe(value, limit=300):
    """Keep diagnostic symbols, never personal paths or terminal controls."""
    value = str(value)
    value = re.sub(r"\x1b\[[0-?]*[ -/]*[@-~]", "", value)
    # Paths can contain spaces and quotation marks. Drop the remainder of a
    # diagnostic field from its first absolute-path token rather than leaking
    # an unquoted/quoted suffix such as a person's name or project directory.
    value = re.sub(r"(?<![A-Za-z0-9_])(?:[A-Za-z]:[\\/]|/).*", "<path>", value,
                   flags=re.DOTALL)
    return "".join(c if c.isprintable() else " " for c in value)[:limit]


def _date(value):
    if not isinstance(value, str):
        return None
    try:
        # Apple inserts a space before the UTC offset, including after a
        # fractional second. CPython's ISO parser requires that offset to
        # immediately follow the time component.
        normalized = re.sub(r"\s+([+-]\d{2}:?\d{2})$", r"\1", value.strip())
        parsed = datetime.fromisoformat(normalized.replace("Z", "+00:00"))
        return parsed.timestamp() if parsed.tzinfo is not None else None
    except (ValueError, OverflowError):
        return None


def _path_matches(value, executable):
    if not isinstance(value, str) or not value.startswith("/"):
        return False
    # Apple's known username placeholder can be resolved without guessing any
    # other redacted component. Broad '*' paths cannot prove path identity;
    # the separate LC_UUID/main-image check is mandatory for that fallback.
    if value.startswith("/Users/USER/"):
        home = Path.home()
        if home.parent == Path("/Users"):
            value = str(home) + value[len("/Users/USER"):]
    if "*" in value or "<" in value or "???" in value:
        return False
    return os.path.realpath(value) == os.path.realpath(executable)


def _identity(pid, name, path, capture, launch, executable, expected_pid, start, finish,
              uuid_verified=False):
    if type(pid) is not int or pid != expected_pid or name != executable.name:
        return False
    if not (_path_matches(path, executable) or
            (uuid_verified and _redacted_path(path, executable))):
        return False
    captured = _date(capture)
    if captured is None or not start <= captured <= finish + 1:
        return False
    if launch is not None:
        launched = _date(launch)
        # Legacy headers may represent launch time only to the nearest second.
        if launched is None or not math.floor(start) <= launched <= captured:
            return False
    return True


def _ips(text, executable, pid, start, finish, executable_uuid=None):
    decoder = json.JSONDecoder()
    text = text.lstrip("\ufeff \r\n\t")
    metadata, end = decoder.raw_decode(text)
    payload, end_payload = decoder.raw_decode(text[end:].lstrip())
    if (not isinstance(metadata, dict) or str(metadata.get("bug_type")) != "309" or
            not isinstance(payload, dict) or
            text[end:].lstrip()[end_payload:].strip()):
        return None
    if any(payload.get(key) not in (None, False, 0, "0", "false")
           for key in ("isNonFatal", "isSimulated")):
        return None
    if not _identity(payload.get("pid"), payload.get("procName"),
                     payload.get("procPath"), payload.get("captureTime"),
                     payload.get("procLaunch"), executable, pid, start, finish,
                     _main_image_uuid(payload.get("usedImages"), executable_uuid, executable)):
        return None
    lines = ["Format: IPS", "Capture: " + _safe(payload["captureTime"])]
    if not _path_matches(payload.get("procPath"), executable):
        lines.append("Executable identity: exact Mach-O LC_UUID and unique main image (redacted path).")
    for section, fields in (("exception", ("type", "signal", "codes", "subtype")),
                            ("termination", ("namespace", "code", "indicator"))):
        values = payload.get(section, {})
        if isinstance(values, dict):
            for key in fields:
                if key in values:
                    lines.append(section + "." + key + ": " + _safe(values[key]))
    threads, images = payload.get("threads", []), payload.get("usedImages", [])
    if not isinstance(threads, list) or not isinstance(images, list):
        return None
    fault = payload.get("faultingThread")
    def crashed(index):
        return index == fault or (isinstance(threads[index], dict) and
                                  threads[index].get("triggered") is True)
    order = sorted(range(len(threads)), key=lambda index: not crashed(index))
    referenced = set()
    for index in order[:MAX_THREADS]:
        thread = threads[index]
        if not isinstance(thread, dict):
            continue
        lines.append(f"Thread {index}" + (" CRASHED" if crashed(index) else ""))
        frames = thread.get("frames", [])
        if not isinstance(frames, list):
            continue
        for number, frame in enumerate(frames[:MAX_FRAMES]):
            if not isinstance(frame, dict):
                continue
            image_index = frame.get("imageIndex")
            binary = "unknown-image"
            if type(image_index) is int and 0 <= image_index < len(images):
                referenced.add(image_index)
                image = images[image_index]
                if isinstance(image, dict):
                    binary = Path(str(image.get("name", "unknown-image"))).name
            lines.append(f"  {number} {_safe(binary, 100)} " +
                         _safe(frame.get("symbol", "<unsymbolicated>"), 500) +
                         " + " + _safe(frame.get("symbolLocation", 0), 30) +
                         " [imageOffset=" + _safe(frame.get("imageOffset", "?"), 30) + "]")
        if len(frames) > MAX_FRAMES:
            lines.append("  Frames truncated.")
    if len(threads) > MAX_THREADS:
        lines.append("Threads truncated.")
    for index in sorted(referenced):
        image = images[index]
        if isinstance(image, dict):
            name = Path(str(image.get("name", "unknown-image"))).name
            lines.append(f"Image {index}: {_safe(name, 100)} UUID=" +
                         _safe(image.get("uuid", "unknown"), 40) +
                         " base=" + _safe(image.get("base", "unknown"), 30) +
                         " arch=" + _safe(image.get("arch", "unknown"), 30))
    return lines


def _legacy(text, executable, pid, start, finish, executable_uuid=None):
    def field(name):
        found = re.search(r"^" + re.escape(name) + r":\s*(.+)$", text, re.MULTILINE)
        return found.group(1).strip() if found else None
    process = re.fullmatch(r"(.+?)\s+\[(\d+)\]", field("Process") or "")
    images = []
    if "\nBinary Images:" in text:
        binary_section = text.split("\nBinary Images:", 1)[1]
        for line in binary_section.splitlines():
            image = re.match(r"^\s*0x[0-9a-fA-F]+\s*-\s*0x[0-9a-fA-F]+\s+\+?(\S+)\s+.*?"
                             r"<([0-9a-fA-F-]+)>\s+(/.*)$", line)
            if image:
                # The first legacy token may be a bundle identifier. The
                # executable identity is the path's basename plus exact UUID;
                # path conflicts and duplicate candidates are still rejected.
                image_path = image.group(3)
                images.append(dict(name=image_path.rsplit("/", 1)[-1],
                                   uuid=image.group(2), path=image_path))
    if not process or not _identity(int(process.group(2)), process.group(1), field("Path"),
                                    field("Date/Time"), field("Launch Time"),
                                    executable, pid, start, finish,
                                    _main_image_uuid(images, executable_uuid, executable)):
        return None
    lines = ["Format: crash", "Capture: " + _safe(field("Date/Time"))]
    if not _path_matches(field("Path"), executable):
        lines.append("Executable identity: exact Mach-O LC_UUID and unique main image (redacted path).")
    for key in ("Exception Type", "Exception Codes", "Exception Subtype", "Termination Reason"):
        value = field(key)
        if value is not None:
            lines.append(key + ": " + _safe(value))
    blocks = []
    current = None
    for line in text.splitlines():
        header = re.match(r"^Thread (\d+)( Crashed)?\s*:", line)
        if header:
            current = [int(header.group(1)), bool(header.group(2)), []]
            blocks.append(current)
        elif current is not None:
            if re.match(r"^\s*\d+\s+", line):
                if len(current[2]) < MAX_FRAMES:
                    current[2].append(_safe(line, 700))
            elif line.strip():
                current = None
    # The OS can create a text report before its stack is written. Do not stop
    # polling on identity-only headers and lose the original crashing frame.
    if field("Exception Type") is None or not any(block[1] and block[2] for block in blocks):
        return None
    blocks.sort(key=lambda block: not block[1])
    for index, crashed, frames in blocks[:MAX_THREADS]:
        lines.append(f"Thread {index}" + (" CRASHED" if crashed else ""))
        lines.extend(frames)
    if len(blocks) > MAX_THREADS:
        lines.append("Threads truncated.")
    return lines


def _read(path, budget):
    # A regular directory entry can be replaced before open(). Never block on
    # a raced FIFO/device; fstat below still rejects everything but regular data.
    flags = os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_NONBLOCK", 0)
    descriptor = os.open(path, flags)
    with os.fdopen(descriptor, "rb") as source:
        info = os.fstat(source.fileno())
        if not stat.S_ISREG(info.st_mode) or info.st_size > min(MAX_REPORT_BYTES, budget):
            return None, 0
        content = source.read(min(MAX_REPORT_BYTES, budget) + 1)
    if len(content) > min(MAX_REPORT_BYTES, budget):
        return None, len(content)
    return content, len(content)


def collect_original_macos_crash(executable, pid, started_at, finished_at, *,
                               report_directories=None, timeout=5.0):
    """Return original-process evidence or explicit absence; never change status.

    Caller must have reaped the exact process and confirmed tree cleanup. UTC
    epochs are recorded immediately around launch/wait, before any replay.
    Only filename-filtered regular reports in the two standard directories are
    considered. Exact identity fields are mandatory; no newest-report fallback.
    """
    prefix = "original-macos-crash: "
    executable = Path(executable)
    if (type(pid) is not int or pid <= 0 or
            not isinstance(started_at, (int, float)) or
            not isinstance(finished_at, (int, float)) or
            not math.isfinite(started_at) or not math.isfinite(finished_at) or
            finished_at < started_at):
        return prefix + "UNAVAILABLE (original PID/time identity missing).\n"
    timeout = min(5.0, max(0.0, float(timeout)))
    directories = tuple(report_directories) if report_directories is not None else (
        Path.home() / "Library/Logs/DiagnosticReports", Path("/Library/Logs/DiagnosticReports"))
    deadline = time.monotonic() + timeout
    executable_uuid = _macho_uuid(executable)
    total_bytes, examined, malformed, inaccessible = 0, 0, 0, 0
    versions = set()
    while True:
        candidates = []
        for directory in directories:
            try:
                with os.scandir(directory) as entries:
                    for count, entry in enumerate(entries):
                        if count >= MAX_DIRECTORY_ENTRIES:
                            break
                        if (entry.name.endswith((".ips", ".crash")) and
                                entry.name.startswith((executable.name + "-", executable.name + "_")) and
                                entry.is_file(follow_symlinks=False)):
                            info = entry.stat(follow_symlinks=False)
                            if started_at - 1 <= info.st_mtime:
                                candidates.append((info.st_mtime_ns, info.st_size, Path(entry.path)))
            except OSError:
                inaccessible += 1
        candidates.sort(key=lambda item: item[0], reverse=True)
        for stamp, size, path in candidates[:MAX_CANDIDATES]:
            version = (path, stamp, size)
            if version in versions or total_bytes >= MAX_TOTAL_BYTES:
                continue
            versions.add(version)
            try:
                content, used = _read(path, MAX_TOTAL_BYTES - total_bytes)
                total_bytes += used
                examined += 1
                if content is None:
                    continue
                parser = _ips if path.suffix == ".ips" else _legacy
                result = parser(content.decode("utf-8"), executable, pid, started_at, finished_at,
                                executable_uuid)
            except (OSError, ValueError, TypeError, OverflowError, RecursionError):
                malformed += 1
                continue
            if result is not None:
                output = (prefix + f"MATCHED original PID {pid}; no replay.\n" +
                          "\n".join(result) + "\n")
                if len(output) > MAX_OUTPUT:
                    output = output[:MAX_OUTPUT] + "\nOriginal report summary truncated.\n"
                return output
        remaining = deadline - time.monotonic()
        if remaining <= 0 or total_bytes >= MAX_TOTAL_BYTES:
            break
        time.sleep(min(0.1, remaining))
    return (prefix + "UNAVAILABLE (no verifiable matching OS report within the bounded window; "
            f"examined={examined}, unreadable_or_malformed={malformed}, "
            f"directory_errors={inaccessible}, bytes={total_bytes}).\n")
