"""Install the two pinned universal models atomically for CI or offline packaging."""

import argparse
import hashlib
import os
from pathlib import Path
import urllib.parse
import urllib.request

from cadmium_runtime_smoke import MODELS


def verified(path, size, digest):
    if not path.is_file() or path.stat().st_size != size:
        return False
    hasher = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(4 * 1024 * 1024), b""):
            hasher.update(chunk)
    return hasher.hexdigest() == digest


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    args.directory.mkdir(parents=True, exist_ok=True)
    for name, (size, digest) in MODELS.items():
        target = args.directory / name
        if verified(target, size, digest):
            print("Verified cached model:", name)
            continue
        temporary = args.directory / (name + ".part")
        address = urllib.parse.urlunsplit(("https", "github.com",
            "/latentspacelabs/cadmium-oss/releases/download/models-v2/" + name, "", ""))
        hasher = hashlib.sha256()
        received = 0
        try:
            with urllib.request.urlopen(address, timeout=120) as response, temporary.open("wb") as stream:
                if response.status != 200 or urllib.parse.urlsplit(response.geturl()).scheme != "https":
                    raise RuntimeError("Model server rejected the download")
                for chunk in iter(lambda: response.read(4 * 1024 * 1024), b""):
                    received += len(chunk)
                    if received > size:
                        raise RuntimeError("Model download exceeded expected size")
                    hasher.update(chunk)
                    stream.write(chunk)
            if received != size or hasher.hexdigest() != digest:
                raise RuntimeError("Model integrity verification failed")
            os.replace(temporary, target)
            print("Installed verified model:", name)
        except Exception:
            temporary.unlink(missing_ok=True)
            raise RuntimeError("Model download failed; retry or restore verified offline files") from None


if __name__ == "__main__":
    main()
