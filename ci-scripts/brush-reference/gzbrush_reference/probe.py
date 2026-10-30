"""Original reference workload using only Krita's public scripting API."""
import hashlib
import json
import os
from pathlib import Path
from krita import Krita, Extension, ManagedColor
from PyQt5.QtCore import QByteArray, QPointF, QTimer
from PyQt5.QtGui import QColor, QImage
from PyQt5.QtWidgets import QApplication


class Probe(Extension):
    def __init__(self, parent):
        super().__init__(parent)
        self.started = False
        self.attempts = 0

    def setup(self):
        QTimer.singleShot(2000, self.run)

    def createActions(self, window):
        pass

    def run(self):
        if self.started:
            return
        app = Krita.instance()
        if app.activeWindow() is None and self.attempts < 30:
            self.attempts += 1
            QTimer.singleShot(1000, self.run)
            return
        self.started = True
        output = Path(os.environ["BRUSH_REFERENCE_ROOT"]) / "output"
        report = {"status": "running", "version": app.version(), "records": []}
        try:
            if app.version() != "5.3.4.1":
                raise RuntimeError("Reference application version mismatch")
            fixtures = json.loads((output / "fixtures.json").read_text())
            resources = app.resources("preset")
            by_name = {resource.name(): resource for resource in resources.values()}
            w, h = fixtures["width"], fixtures["height"]
            for name in fixtures["presets"]:
                if name not in by_name:
                    raise RuntimeError("Original reference preset was not loaded: " + name)
                document = app.createDocument(w, h, name, "RGBA", "U8", "sRGB-elle-V2-srgbtrc.icc", 72.0)
                view = app.activeWindow().addView(document)
                app.setActiveDocument(document)
                node = document.createNode("Original strips", "paintlayer")
                document.rootNode().addChildNode(node, None)
                document.setActiveNode(node)
                # Krita integer RGBA API uses BGRA channel order, top row first.
                colors = [(255, 0, 0, 255), (0, 255, 0, 128), (0, 0, 255, 255), (0, 0, 0, 0)]
                row = b"".join(bytes(colors[(x // 128) % 4]) for x in range(w))
                initial = row * h
                if not node.setPixelData(QByteArray(initial), 0, 0, w, h):
                    raise RuntimeError("Cannot initialize original reference raster")
                document.refreshProjection()
                document.waitForDone()
                view.setCurrentBrushPreset(by_name[name])
                view.setBrushSize(32)
                view.setPaintingOpacity(1.0)
                view.setPaintingFlow(1.0)
                view.setForeGroundColor(ManagedColor.fromQColor(QColor(220, 40, 30, 255), view.canvas()))
                view.canvas().setZoomLevel(1.0)
                node.paintLine(QPointF(256, 1024), QPointF(1792, 1024), 1.0, 1.0, "ForegroundColor")
                document.waitForDone()
                raw = bytes(node.pixelData(0, 0, w, h))
                if len(raw) != w * h * 4 or raw == initial:
                    raise RuntimeError("Reference API stroke produced no changed pixels")
                image = QImage(raw, w, h, w * 4, QImage.Format_ARGB32).copy()
                filename = name.replace(" ", "_") + ".png"
                if not image.save(str(output / filename), "PNG"):
                    raise RuntimeError("Cannot save reference fixture")
                report["records"].append({"preset": name, "image": filename, "sha256Pixels": hashlib.sha256(raw).hexdigest(), "profile": document.colorProfile(), "input": "Node.paintLine", "pressure": [1, 1]})
                document.setModified(False)
                document.close()
            report["status"] = "complete"
            (output / "oracle-status.json").write_text(json.dumps(report, indent=2))
            QApplication.exit(0)
        except Exception as error:
            report["status"] = "failed"
            report["error"] = str(error)
            (output / "oracle-status.json").write_text(json.dumps(report, indent=2))
            print("::error::Krita reference workload failed: " + str(error), flush=True)
            QApplication.exit(1)
