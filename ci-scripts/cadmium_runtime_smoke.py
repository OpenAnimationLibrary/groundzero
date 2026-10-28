"""Exercise the packaged local worker with real models and a synthetic drawing."""

import argparse
import base64
import hashlib
import http.client
import json
from pathlib import Path
import struct
import subprocess
import time
import zlib

MODELS = {
    "ant_v2_fp32.onnx": (1388610539, "6c8d95cbfa62408f3cbfbbc29204fac7df5a587466dbb751e7e1b0bb530ff12b"),
    "gap_closer_fp32.onnx": (497500380, "3a9a792f72fd3451c36145a0657328e333d6e5e982f0760b5e5a6e7e9934d327"),
}


def png_uri(width, height, channels, pixels):
    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xffffffff)
    rows = b"".join(b"\0" + pixels[y * width * channels:(y + 1) * width * channels] for y in range(height))
    data = b"\x89PNG\r\n\x1a\n"
    data += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6 if channels == 4 else 0, 0, 0, 0))
    data += chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b"")
    return "data:image/png;base64," + base64.b64encode(data).decode("ascii")


def request(port, operation, body=None):
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=1200)
    try:
        payload = None if body is None else json.dumps(body).encode("utf-8")
        connection.request("GET" if body is None else "POST", "/" + operation,
                           body=payload, headers={"Content-Type": "application/json"})
        response = connection.getresponse()
        result = json.loads(response.read())
        if response.status != 200:
            raise RuntimeError("The packaged worker rejected the inference fixture")
        return result
    finally:
        connection.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--models", type=Path, required=True)
    parser.add_argument("--port", type=int, default=18772)
    args = parser.parse_args()
    for name, (size, digest) in MODELS.items():
        path = args.models / name
        if path.stat().st_size != size:
            raise RuntimeError("Model size mismatch")
        hasher = hashlib.sha256()
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(4 * 1024 * 1024), b""):
                hasher.update(chunk)
        if hasher.hexdigest() != digest:
            raise RuntimeError("Model digest mismatch")
    command = [str(args.runtime.resolve()), "--host", "127.0.0.1", "--port", str(args.port), "--ep", "cpu",
               "--ant-model", str((args.models / "ant_v2_fp32.onnx").resolve()),
               "--gap-model", str((args.models / "gap_closer_fp32.onnx").resolve())]
    # Keep host reports out of public job logs.
    process = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(60):
            if process.poll() is not None:
                raise RuntimeError("Packaged worker exited at startup")
            try:
                if request(args.port, "health").get("status") == "ok":
                    break
            except (OSError, ValueError):
                time.sleep(1)
        else:
            raise RuntimeError("Packaged worker startup timed out")
        line = bytearray()
        labels = bytearray()
        for y in range(128):
            for x in range(128):
                boundary = (23 <= x <= 105 and (23 <= y <= 25 or 103 <= y <= 105)) or (
                    23 <= y <= 105 and (23 <= x <= 25 or 103 <= x <= 105))
                line.extend((0, 0, 0, 255) if boundary else (255, 255, 255, 0))
                labels.append(1 if 26 <= x <= 102 and 26 <= y <= 102 else 0)
        line_uri = png_uri(128, 128, 4, line)
        label_uri = png_uri(128, 128, 1, labels)
        colors = [[0, 0, 0, 0], [180, 80, 40, 255]]
        started = time.monotonic()
        result = request(args.port, "colorize", {
            "target_line_image_uri": line_uri,
            "target_seg_map_uri": label_uri,
            "references": [{"line_image_uri": line_uri, "seg_map_uri": label_uri, "colors_rgba": colors}],
            "return_colorized": False,
            "verbose": False,
        })
        predicted = result.get("target_colors_rgba", [])
        if len(predicted) != 2 or predicted[1] != colors[1]:
            raise RuntimeError("Self-reference colorization failed to preserve the enclosed region color")
        print("Packaged CPU colorization passed; cold request seconds:", round(time.monotonic() - started, 2))
        started = time.monotonic()
        segmented = request(args.port, "segment", {"line_image_uri": line_uri, "gap_closer_strength": 1.0})
        if not isinstance(segmented.get("seg_map_uri"), str) or segmented.get("num_segments", 0) < 1:
            raise RuntimeError("Packaged gap-closing fixture produced no segmentation")
        print("Packaged CPU gap closing passed; cold request seconds:", round(time.monotonic() - started, 2))
    finally:
        process.terminate()
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


if __name__ == "__main__":
    main()
