import os
from pathlib import Path
import traceback

boot = Path(os.environ["BRUSH_REFERENCE_ROOT"]) / "output" / "plugin-startup.log"
boot.write_text("Python plugin import started\n")
try:
    from .probe import Probe
    from krita import Krita
    Krita.instance().addExtension(Probe(Krita.instance()))
    with boot.open("a") as stream:
        stream.write("Extension registered\n")
except Exception:
    with boot.open("a") as stream:
        stream.write(traceback.format_exc())
    raise
