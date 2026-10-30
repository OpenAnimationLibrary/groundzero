"""Exercise official model operators on synthetic arrays, without user artwork."""

import importlib.metadata
import json
from pathlib import Path
import platform
import os
import struct
import subprocess
import queue
import threading
import re
import sys
import time

import numpy as np


def make_interpreter(path):
    from ai_edge_litert.interpreter import Interpreter
    interpreter = Interpreter(model_path=str(path), num_threads=2)
    interpreter.allocate_tensors()
    return interpreter


def invoke(interpreter, arrays):
    details = interpreter.get_input_details()
    if len(details) != len(arrays):
        raise AssertionError("Unexpected model input count")
    for detail in details:
        shape = tuple(detail["shape"])
        matches = [array for array in arrays if array.shape == shape]
        if len(matches) != 1 or detail["dtype"] != np.float32:
            raise AssertionError("Unverified input shape or numeric contract")
        interpreter.set_tensor(detail["index"], matches[0])
    interpreter.invoke()
    outputs = interpreter.get_output_details()
    if len(outputs) != 1:
        raise AssertionError("Unexpected model output count")
    output = interpreter.get_tensor(outputs[0]["index"])
    if not np.isfinite(output).all():
        raise AssertionError("Model produced nonfinite values")
    return output


def main():
    models, report, native = map(Path, sys.argv[1:])
    prediction = make_interpreter(models / "prediction.tflite")
    transfer = make_interpreter(models / "transfer.tflite")
    style_shape = tuple(prediction.get_input_details()[0]["shape"])
    content_shapes = [tuple(d["shape"]) for d in transfer.get_input_details() if d["shape"][-1] == 3]
    if len(content_shapes) != 1 or style_shape != (1, 256, 256, 3):
        raise AssertionError("Unexpected official model contract")
    content_shape = content_shapes[0]
    if len(content_shape) != 4 or content_shape[0] != 1 or max(content_shape[1:3]) > 512:
        raise AssertionError("Content shape exceeds evaluation bound")
    rng = np.random.default_rng(2026)
    style = rng.random(style_shape, dtype=np.float32)
    other_style = np.zeros(style_shape, dtype=np.float32)
    other_style[..., 0] = 1
    content = rng.random(content_shape, dtype=np.float32)
    started = time.monotonic()
    embedding = invoke(prediction, [style])
    other_embedding = invoke(prediction, [other_style])
    output = invoke(transfer, [content, embedding])
    repeated = invoke(transfer, [content, embedding])
    other = invoke(transfer, [content, other_embedding])
    other_content = np.zeros(content_shape, dtype=np.float32)
    other_content[:, :content_shape[1] // 2, :, :] = 1
    changed_content = invoke(transfer, [other_content, embedding])
    if output.shape != content_shape:
        raise AssertionError("Output dimensions changed")
    np.testing.assert_allclose(output, repeated, rtol=0, atol=1e-5)
    effect = float(np.mean(np.abs(output - content)))
    reference_effect = float(np.mean(np.abs(output - other)))
    content_effect = float(np.mean(np.abs(output - changed_content)))
    if effect <= 1e-4 or reference_effect <= 1e-4 or content_effect <= 1e-4:
        raise AssertionError("Model failed content/style influence checks")
    inference_seconds = time.monotonic() - started
    cases = [(1, 1, style, embedding), (2, 2, content, output),
             (2, 3, content, output), (1, 4, other_style, other_embedding),
             (2, 5, content, other), (1, 6, style, embedding),
             (2, 7, other_content, changed_content)]
    native_result = native_check(native, cases)
    # Quantized fixtures exercise the application's QImage conversion against
    # an independent Python oracle using exactly the same input values.
    fixtures = report / "host-fixtures"
    fixtures.mkdir()
    inputs = {}
    for name, value in (("reference", style), ("alternate", other_style), ("content", content)):
        quantized = (np.clip(value, 0, 1) * 255 + .5).astype(np.uint8)
        (fixtures / (name + ".rgb")).write_bytes(quantized.tobytes())
        inputs[name] = quantized.astype(np.float32) / np.float32(255)
    host_arrays = {name: (inputs[name] * 255 + .5).astype(np.uint8) for name in inputs}
    for name, key in (("expected", "reference"), ("alternate-expected", "alternate")):
        predicted = invoke(prediction, [inputs[key]])
        styled = invoke(transfer, [inputs["content"], predicted])
        host_arrays[name] = (np.clip(styled, 0, 1) * 255 + .5).astype(np.uint8)
        (fixtures / (name + ".rgb")).write_bytes(host_arrays[name].tobytes())
    np.savez_compressed(report / "golden.npz", style=style, content=content,
                        other_style=other_style, other_content=other_content,
                        embedding=embedding, other_embedding=other_embedding,
                        output=output, other=other, changed_content=changed_content,
                        **{"host_" + key: value for key, value in host_arrays.items()})
    result = {"runtime": importlib.metadata.version("ai-edge-litert"),
              "platform": platform.system(), "machine": platform.machine(),
              "content_shape": [int(value) for value in content_shape],
              "embedding_shape": [int(value) for value in embedding.shape],
              "inference_seconds": inference_seconds,
              "timed_operations": "two style predictions and four content transfers; excludes initialization",
              "mean_content_difference": effect, "mean_reference_difference": reference_effect,
              "mean_changed_content_difference": content_effect,
              "repeat_max_difference": float(np.max(np.abs(output - repeated))),
              "native_worker": native_result,
              "quality_claim": "Operator/numerical smoke only, not artist or temporal validation"}
    (report / "inference.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print("::notice title=Neural numerical evaluation::" + json.dumps(result), flush=True)


def native_check(directory, cases):
    """Exercise the native C API with Python/developer loader paths removed."""
    library = {"win32": "libLiteRt.dll", "darwin": "libLiteRt.dylib"}.get(sys.platform, "libLiteRt.so")
    if sys.platform == "darwin" and (directory / "libCLiteRT_mac.dylib").is_file():
        library = "libCLiteRT_mac.dylib"
    executable = directory / ("groundzero-style-worker" + (".exe" if os.name == "nt" else ""))
    args = [str(executable), str(directory / library), str(directory / "prediction.tflite"),
            str(directory / "transfer.tflite")]
    magic = 0x31535A47
    environment = {key: value for key, value in os.environ.items()
                   if not key.startswith(("PYTHON", "DYLD_", "LD_"))}
    if os.name == "nt":
        system = Path(os.environ["SystemRoot"])
        environment["PATH"] = os.pathsep.join(map(str, [directory, system / "System32", system]))
    else:
        environment["PATH"] = "/usr/bin:/bin:/usr/sbin:/sbin"

    def request(command, sequence, array):
        return struct.pack("<IIII", magic, command, sequence, array.size) + array.astype("<f4").tobytes()

    def run(data, success=True):
        result = subprocess.run(args, input=data, capture_output=True, timeout=60, env=environment)
        if success and result.returncode:
            raise AssertionError("Native worker failed: " + result.stderr.decode(errors="replace")[-3000:])
        if not success and result.returncode == 0:
            raise AssertionError("Malformed worker request was accepted")
        return result

    style = cases[0][2]
    data = b"".join(request(command, sequence, pixels) for command, sequence, pixels, _ in cases)
    started = time.monotonic()
    result = run(data)
    memory = re.search(rb"peak_memory_bytes=(\d+)", result.stderr)
    if not memory or int(memory[1]) > 512 * 1024 * 1024:
        raise AssertionError("Native worker peak memory absent or exceeds 512 MiB")
    position = 0
    frames = []
    expected_frames = [(0, 0, np.empty(0, dtype=np.float32))] + [
        (command, sequence, expected) for command, sequence, _, expected in cases]
    max_difference = 0.0
    for command, sequence, expected in expected_frames:
        count = expected.size
        if len(result.stdout) - position < 16:
            raise AssertionError("Truncated native reply")
        header = struct.unpack_from("<IIII", result.stdout, position)
        if header != (magic, command, sequence, count):
            raise AssertionError("Native reply identity mismatch")
        position += 16
        frame = np.frombuffer(result.stdout[position:position + count * 4], dtype="<f4")
        if frame.size != count:
            raise AssertionError("Truncated native pixel payload")
        np.testing.assert_allclose(frame, expected.ravel(), rtol=0, atol=1e-5)
        if count:
            max_difference = max(max_difference, float(np.max(np.abs(frame - expected.ravel()))))
        frames.append(frame)
        position += count * 4
    if position != len(result.stdout):
        raise AssertionError("Unexpected native reply bytes")
    np.testing.assert_array_equal(frames[2], frames[3])
    malformed = run(struct.pack("<IIII", magic, 2, 9, 0xFFFFFFFF), False)
    if b"oversized" not in malformed.stderr:
        raise AssertionError("Oversized request did not fail before allocation")
    truncated = run(struct.pack("<IIII", magic, 1, 9, style.size), False)
    if b"Truncated" not in truncated.stderr:
        raise AssertionError("Truncated request did not fail explicitly")
    bad_pixels = style.copy();bad_pixels.flat[0] = np.nan
    nonfinite = run(request(1, 9, bad_pixels), False)
    if b"finite" not in nonfinite.stderr:
        raise AssertionError("Nonfinite request did not fail explicitly")
    # Process termination is the cancellation boundary for synchronous inference.
    process = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, env=environment)
    ready = queue.Queue()
    reader = threading.Thread(target=lambda: ready.put(process.stdout.read(16)), daemon=True)
    reader.start()
    try:
        if ready.get(timeout=10) != struct.pack("<IIII", magic, 0, 0, 0):
            raise AssertionError("Worker did not initialize before cancellation")
        process.stdin.write(request(1, 10, style));process.stdin.flush()
        process.terminate();process.wait(timeout=5)
    finally:
        if process.poll() is None:
            process.kill();process.wait(timeout=5)
        process.stdin.close();process.stdout.close();process.stderr.close()
        reader.join(timeout=1)
    restarted = run(data)
    if restarted.stdout != result.stdout:
        raise AssertionError("Worker restart changed deterministic results")
    return {"protocol": 1, "python_api_required": False, "developer_loader_paths_removed": True,
            "peak_memory_bytes": int(memory[1]), "elapsed_seconds": time.monotonic() - started,
            "malformed_requests_rejected": True, "termination_and_restart": True,
            "reference_replacement_verified": True, "python_reference_max_difference": max_difference}


if __name__ == "__main__":
    main()
