"""Verify and exercise the delivered native component without Python inference."""
from array import array
import hashlib
import json
import math
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import threading

PINS = {"prediction": "af6ad4b2e7aeba0675f32636082ab915ced5375229a3f8aff7e714c6213f5ed2",
        "transfer": "7a1550643cf034a4d813c0aa276976cd15da4141b4f1ec3631db1d0d9c8e2cd1"}


def bounded_run(arguments, data, environment):
    process = subprocess.Popen(arguments, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, env=environment)
    output, diagnostic, errors = bytearray(), bytearray(), []
    def reader(stream, target, limit):
        while block := stream.read(65536):
            if len(target) + len(block) > limit:
                errors.append("Installed worker exceeded the bounded pipe output")
                process.kill();return
            target.extend(block)
    def writer():
        try:
            process.stdin.write(data)
            process.stdin.close()
        except (BrokenPipeError, OSError):
            pass
    threads = [threading.Thread(target=reader,args=(process.stdout,output,8*1024*1024)),
               threading.Thread(target=reader,args=(process.stderr,diagnostic,65536)),
               threading.Thread(target=writer)]
    for thread in threads: thread.start()
    try:
        process.wait(timeout=120)
    finally:
        if process.poll() is None: process.kill();process.wait(timeout=5)
        for thread in threads: thread.join(timeout=5)
        process.stdout.close();process.stderr.close()
        try: process.stdin.close()
        except (BrokenPipeError, OSError): pass
    if errors: raise RuntimeError(errors[0])
    return subprocess.CompletedProcess(arguments,process.returncode,bytes(output),bytes(diagnostic))


def verify(directory):
    manifest_path = directory / "package.json"
    if not manifest_path.is_file() or manifest_path.stat().st_size > 16384:
        raise RuntimeError("Installed neural component manifest missing or oversized")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if (manifest.get("version"), manifest.get("protocol"), manifest.get("runtimeVersion")) != (1, 1, "2.3.0"):
        raise RuntimeError("Installed neural component version mismatch")
    files = {}
    for role in ("worker", "runtime", "prediction", "transfer"):
        entry = manifest[role]
        name = entry["file"]
        if not isinstance(name, str) or not name or name in (".", "..") or any(c in name for c in "/\\:"):
            raise RuntimeError("Invalid installed component filename")
        path = directory / name
        if path.is_symlink() or not path.is_file() or not 0 < path.stat().st_size <= 64 * 1024 * 1024:
            raise RuntimeError("Installed component file missing or oversized")
        with path.open("rb") as stream:
            digest = hashlib.file_digest(stream, "sha256").hexdigest()
        if path.stat().st_size != entry["bytes"] or digest != entry["sha256"] or (role in PINS and digest != PINS[role]):
            raise RuntimeError("Installed neural component checksum mismatch: " + role)
        files[role] = path
    if not (directory / "Apache-2.0.txt").is_file() or not (directory / "licenses").is_dir() or not (directory / "provenance.json").is_file():
        raise RuntimeError("Installed neural component notices/provenance missing")
    return files


def check(directory):
    files = verify(directory)
    environment = {key: value for key, value in os.environ.items()
                   if not key.startswith(("PYTHON", "DYLD_", "LD_"))}
    if os.name == "nt":
        root = Path(os.environ["SystemRoot"])
        environment["PATH"] = os.pathsep.join(map(str, (root / "System32", root)))
    else:
        environment["PATH"] = "/usr/bin:/bin:/usr/sbin:/sbin"
    def request(command, sequence, pixels):
        if sys.byteorder != "little": pixels.byteswap()
        return struct.pack("<IIII", 0x31535a47, command, sequence, len(pixels)) + pixels.tobytes()
    style = array("f", ((i % 251) / 250 for i in range(256*256*3)))
    alternate = array("f", (1 if i % 3 == 0 else 0 for i in range(256*256*3)))
    content = array("f", ((i % 113) / 112 for i in range(384*384*3)))
    other_content = array("f", (0 for _ in range(384*384*3)))
    data = request(1,1,style)+request(2,2,content)+request(2,3,content)+request(1,4,alternate)+request(2,5,content)+request(2,6,other_content)
    result = bounded_run([str(files[role]) for role in ("worker", "runtime", "prediction", "transfer")], data, environment)
    if result.returncode:
        raise RuntimeError("Installed neural worker failed: " + result.stderr.decode(errors="replace")[-2000:])
    position = 0
    replies = []
    for command, sequence, count in ((0,0,0),(1,1,100),(2,2,384*384*3),(2,3,384*384*3),(1,4,100),(2,5,384*384*3),(2,6,384*384*3)):
        if len(result.stdout)-position < 16 or struct.unpack_from("<IIII",result.stdout,position) != (0x31535a47,command,sequence,count):
            raise RuntimeError("Installed neural reply contract mismatch")
        position += 16
        payload = result.stdout[position:position+count*4]
        if len(payload) != count*4: raise RuntimeError("Truncated installed neural result")
        values = array("f");values.frombytes(payload)
        if sys.byteorder != "little": values.byteswap()
        if not all(map(math.isfinite, values)): raise RuntimeError("Nonfinite installed neural output")
        replies.append(values);position += count*4
    if position != len(result.stdout) or replies[2] != replies[3]:
        raise RuntimeError("Installed neural inference is not repeatable")
    effect = sum(abs(a-b) for a,b in zip(replies[2],replies[5]))/len(replies[2])
    content_effect = sum(abs(a-b) for a,b in zip(replies[5],replies[6]))/len(replies[5])
    memory = re.search(rb"peak_memory_bytes=(\d+)", result.stderr)
    if effect <= 1e-4 or content_effect <= 1e-4 or not memory or int(memory[1]) > 512*1024*1024:
        raise RuntimeError("Installed neural reference influence or memory bound failed")
    print("neural-installed-component: PASS", flush=True)


if __name__ == "__main__":
    check(Path(sys.argv[1]).resolve())
