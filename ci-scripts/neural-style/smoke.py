"""Exercise official model operators on synthetic arrays, without user artwork."""

import importlib.metadata
import json
from pathlib import Path
import platform
import sys
import time

import numpy as np
from ai_edge_litert.interpreter import Interpreter


def make_interpreter(path):
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
    models, report = map(Path, sys.argv[1:])
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
    result = {"runtime": importlib.metadata.version("ai-edge-litert"),
              "platform": platform.system(), "machine": platform.machine(),
              "content_shape": list(content_shape), "embedding_shape": list(embedding.shape),
              "inference_seconds": time.monotonic() - started,
              "timed_operations": "two style predictions and four content transfers; excludes initialization",
              "mean_content_difference": effect, "mean_reference_difference": reference_effect,
              "mean_changed_content_difference": content_effect,
              "repeat_max_difference": float(np.max(np.abs(output - repeated))),
              "quality_claim": "Operator/numerical smoke only, not artist or temporal validation"}
    (report / "inference.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps(result), flush=True)


if __name__ == "__main__":
    main()
