"""Original reference workload using only Krita's public scripting API."""
import faulthandler
import base64
import hashlib
import json
import math
import os
import zlib
from pathlib import Path
from krita import Krita, Extension, ManagedColor, Preset
from xml.etree.ElementTree import fromstring
from PyQt5.QtCore import QByteArray, QPoint, QTimer
from PyQt5.QtGui import QColor, QImage
from PyQt5.QtWidgets import QApplication, QMessageBox


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
        self.faults = (output / "python-stacks.log").open("w")
        faulthandler.enable(file=self.faults)
        faulthandler.dump_traceback_later(60, repeat=True, file=self.faults)
        self.work = self.render(app, output)
        self.advance()

    def advance(self):
        try:
            next(self.work)
        except StopIteration:
            return
        QTimer.singleShot(0, self.advance)

    def render(self, app, output):
        build = app.version()
        version = build.split()[0] if build.split() else ""
        report = {"status": "running", "version": version, "build": build, "records": []}
        def checkpoint(stage):
            with (output / "progress.log").open("a") as progress:
                progress.write(stage + "\n")
            (output / "oracle-status.json").write_text(json.dumps(report, indent=2))
        document = None
        try:
            checkpoint("reference plugin started: " + build)
            if version != "5.3.4.1":
                raise RuntimeError("Reference application version mismatch")
            fixtures = json.loads((output / "fixtures.json").read_text())
            resources = app.resources("preset")
            by_name = {resource.name(): resource for resource in resources.values()}
            w, h = fixtures["width"], fixtures["height"]
            checkpoint("create reference document")
            document = app.createDocument(w, h, "Original reference canvas", "RGBA", "U8", "sRGB-elle-V2-srgbtrc.icc", 72.0)
            view = app.activeWindow().addView(document)
            app.setActiveDocument(document)
            node = document.createNode("Original strips", "paintlayer")
            document.rootNode().addChildNode(node, None)
            document.setActiveNode(node)
            report["documentIsolation"] = "full-raster reset before each separate Node.paintLine stroke"
            for case in fixtures["cases"]:
                name = case["preset"]
                if name not in by_name:
                    raise RuntimeError("Original reference preset was not loaded: " + name)
                # Krita integer RGBA API uses BGRA channel order, top row first.
                colors = [(255, 0, 0, 255), (0, 255, 0, 128), (0, 0, 255, 255), (0, 0, 0, 0)]
                row = (bytes(w * 4) if case.get("background") == "transparent" else
                       b"".join(bytes(colors[(x // 128) % 4]) for x in range(w)))
                initial = row * h
                checkpoint(name + ": initialize pixels")
                if not node.setPixelData(QByteArray(initial), 0, 0, w, h):
                    raise RuntimeError("Cannot initialize original reference raster")
                document.refreshProjection()
                document.waitForDone()
                checkpoint(name + ": select preset")
                view.setCurrentBrushPreset(by_name[name])
                view.setBrushSize(case["diameter"])
                view.setPaintingOpacity(case["opacity"])
                view.setPaintingFlow(case["flow"])
                view.setBrushRotation(0)
                view.setEraserMode(False)
                view.setGlobalAlphaLock(False)
                view.setDisablePressure(False)
                view.setForeGroundColor(ManagedColor.fromQColor(QColor(220, 40, 30, 255), view.canvas()))
                view.canvas().setZoomLevel(1.0)
                effective_view = {"diameter": view.brushSize(), "opacity": view.paintingOpacity(),
                                  "flow": view.paintingFlow(), "rotation": view.brushRotation(),
                                  "blend": view.currentBlendingMode(), "eraser": view.eraserMode(),
                                  "alphaLock": view.globalAlphaLock(), "disablePressure": view.disablePressure()}
                # The UI may quantize opacity/flow to one byte. Record that
                # value rather than pretending requested and effective values
                # are necessarily identical. Do not tune candidate inputs to
                # this observation: source quantization belongs in its profile.
                if abs(effective_view["diameter"] - case["diameter"]) > 1e-5 or any(
                        abs(effective_view[key] - case[key]) > 1 / 255 + 1e-9
                        for key in ("opacity", "flow")):
                    raise RuntimeError("Source view did not apply requested brush controls")
                if (effective_view["rotation"] != 0 or effective_view["blend"] != "normal" or
                        any(effective_view[key] for key in ("eraser", "alphaLock", "disablePressure"))):
                    raise RuntimeError("Source view retained an unexpected painting modifier")
                # Read back the selected source settings through the public API;
                # handwritten input XML alone does not establish effective state.
                effective_xml = Preset(view.currentBrushPreset()).toXML()
                if len(effective_xml) > 1024 * 1024:
                    raise RuntimeError("Unexpectedly large effective source preset")
                effective_root = fromstring(effective_xml)
                expected_engine = "paintbrush" if " dry " in name else "colorsmudge"
                if effective_root.attrib.get("paintopid") != expected_engine:
                    raise RuntimeError("Selected source engine differs from fixture")
                effective_name = name.replace(" ", "_") + ".effective.xml"
                (output / effective_name).write_text(effective_xml)
                effective_fields = {field.attrib["name"]: field.text or "" for field in effective_root.findall("param") if "name" in field.attrib}
                if (effective_fields.get("KisPrecisionOption/precisionLevel") != "5" or
                        effective_fields.get("KisPrecisionOption/AutoPrecisionEnabled") != "false"):
                    raise RuntimeError("Selected source preset lost fixed precision 5")
                controls = {key: effective_fields.get(key) for key in ("SmudgeRateValue", "ColorRateValue", "SmudgeRadiusValue", "PressureSmudgeRate", "SmudgeRateUseNewEngine", "SmudgeRateMode", "SmudgeRateSmearAlpha")}
                checkpoint(name + ": paint line")
                # The pinned 5.3.4.1 Python binding accepts QPoint, despite the
                # online C++ API describing QPointF. This fixture is integral.
                node.paintLine(QPoint(*case["start"]), QPoint(*case["end"]), *case["pressure"], "ForegroundColor")
                document.waitForDone()
                checkpoint(name + ": read painted pixels")
                raw = bytes(node.pixelData(0, 0, w, h))
                if len(raw) != w * h * 4:
                    raise RuntimeError("Reference API returned an incomplete raster")
                if raw == initial and not case.get("expectUnchanged", False):
                    raise RuntimeError("Reference API stroke produced no changed pixels")
                if raw != initial and case.get("expectUnchanged", False):
                    raise RuntimeError("Source no-op boundary case unexpectedly changed pixels")
                image = QImage(raw, w, h, w * 4, QImage.Format_ARGB32).copy()
                filename = name.replace(" ", "_") + ".png"
                if not image.save(str(output / filename), "PNG"):
                    raise RuntimeError("Cannot save reference fixture")
                # Bounded numerical probes expose pickup/drop-off relationships
                # without requiring a screenshot or access to hidden state.
                samples = []
                for x in range(240, 1809, 16):
                    offset = (1024 * w + x) * 4
                    blue, green, red, alpha = raw[offset:offset+4]
                    samples.append([x, red, green, blue, alpha])
                geometry = None
                if case.get("background") == "transparent":
                    # Exact alpha coverage around all seven short strokes.
                    # This small window is diagnostic; comparison still uses
                    # both full-canvas changed sets and their union.
                    if fixtures.get("group") == "geometry":
                        side = math.ceil(case["diameter"]) + 10
                        left = math.floor(case["start"][0] - case["diameter"] / 2) - 4
                        top = math.floor(case["start"][1] - case["diameter"] / 2) - 4
                        rect = [left, top, side, side]
                    else:
                        rect = [232, 1000, 80, 48]
                    left, top, width, height = rect
                    alpha = bytes(raw[(y * w + x) * 4 + 3]
                                  for y in range(top, top + height)
                                  for x in range(left, left + width))
                    geometry = {"rect": rect, "encoding": "zlib-base64-alpha8-row-major",
                                "data": base64.b64encode(zlib.compress(alpha)).decode("ascii")}
                report["records"].append({**case, "image": filename, "sha256Pixels": hashlib.sha256(raw).hexdigest(), "profile": document.colorProfile(), "input": "Node.paintLine", "changed": raw != initial, "centerlineRGBA8": samples, "geometryProbe": geometry, "effectiveView": effective_view, "effectivePresetXml": effective_name, "effectivePresetSha256": hashlib.sha256(effective_xml.encode()).hexdigest(), "effectiveWetControls": controls})
                checkpoint(name + ": saved output")
                # Return to Qt between independent strokes. Calling processEvents
                # inside paint/document operations reentered native consumers;
                # one monolithic loop instead starved deferred view updates.
                yield
            report["status"] = "complete"
            (output / "oracle-status.json").write_text(json.dumps(report, indent=2))
        except Exception as error:
            report["status"] = "failed"
            report["error"] = str(error)
            (output / "oracle-status.json").write_text(json.dumps(report, indent=2))
            print("::error::Krita reference workload failed: " + str(error), flush=True)
            # A failed case may not have reached its normal yield. Let queued
            # document/view notifications run before clearing the dirty flag.
            yield
        finally:
            if document is not None:
                document.setModified(False)
            # Keep the native view/document alive until the normal window close.
            # Manually closing Document and pumping events raced queued thumbnail
            # and canvas consumers in the pinned application. Each case instead
            # resets every pixel in this one canvas and starts a new API stroke.
            # Closing the main window runs Krita's normal document/view cleanup;
            # exit() skips it and left display consumers alive during teardown.
            # CI independently requires a complete status AND a zero exit code.
            def verify_closed():
                for widget in QApplication.topLevelWidgets():
                    if (isinstance(widget, QMessageBox) and widget.isVisible() and
                            "Original reference canvas" in widget.text() and
                            widget.standardButtons() & QMessageBox.Discard and
                            not report.get("fixtureDiscarded", False)):
                        # This isolated process owns only the generated fixture
                        # document. Its output pixels are already retained (or
                        # its primary failure remains recorded). Discarding this
                        # disposable document cannot turn a failed case green.
                        report["fixtureDiscarded"] = True
                        widget.button(QMessageBox.Discard).click()
                        QTimer.singleShot(100, verify_closed)
                        return
                visible = [widget.metaObject().className() for widget in QApplication.topLevelWidgets() if widget.isVisible()]
                remaining = len(app.documents())
                report["shutdown"] = {"visibleWidgets": visible, "remainingDocuments": remaining}
                report["shutdown"]["messages"] = [widget.text()[:1000] for widget in QApplication.topLevelWidgets()
                                                    if isinstance(widget, QMessageBox) and widget.isVisible()]
                checkpoint("normal window close finished: " + json.dumps(report["shutdown"]))
                if visible or remaining:
                    # A dialog or document still owns native state. Keep it
                    # observable and let CI fail instead of forcing teardown.
                    report["status"] = "failed"
                    report["shutdown"]["error"] = "Reference window close left live UI or documents"
                    report.setdefault("error", report["shutdown"]["error"])
                    checkpoint(report["shutdown"]["error"])
                    return
                # Krita can keep its application event loop alive with no
                # windows. Quit only after normal view/document destruction.
                QApplication.quit()

            def close_windows():
                checkpoint("request normal window close")
                QTimer.singleShot(1000, verify_closed)
                QApplication.closeAllWindows()

            QTimer.singleShot(0, close_windows)
