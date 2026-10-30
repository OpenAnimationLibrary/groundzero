"""Original reference workload using only Krita's public scripting API."""
import faulthandler
import base64
import hashlib
import json
import math
import os
import random
import time
import zlib
from pathlib import Path
from krita import Krita, Extension, ManagedColor, Preset
from . import resource_probe, sensor_probe, mirror_probe, masked_probe
from xml.etree.ElementTree import fromstring
from PyQt5.QtCore import QByteArray, QPoint, QPointF, QTimer, QEvent, Qt
from PyQt5.QtGui import QColor, QImage, QTabletEvent
from PyQt5.QtWidgets import QApplication, QMessageBox, QWidget, QComboBox


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

    def paint_case(self, app, view, node, case):
        if case.get("delivery") != "Qt tablet events":
            # Node.paintLine accepts integer QPoint coordinates. Never silently
            # round a fractional fixture and compare different stroke geometry.
            points = [case["start"], case["end"]]
            if any(float(value) != int(value) for point in points for value in point):
                raise RuntimeError("Node.paintLine fixture requires integral coordinates")
            start, end = [QPoint(*(int(value) for value in point)) for point in points]
            node.paintLine(start, end, *case["pressure"], "ForegroundColor")
            return {"mode": "Node.paintLine", "events": [
                {"type": "line-start", "position": [start.x(), start.y()],
                 "pressure": case["pressure"][0]},
                {"type": "line-end", "position": [end.x(), end.y()],
                 "pressure": case["pressure"][1]}],
                "timing": "Public API line endpoints delivered synchronously; internal source dab times are not exposed"}
        # Public action identifier and View transforms; no engine-private API.
        action = app.action("KritaShape/KisToolBrush")
        if action is None:
            raise RuntimeError("Freehand brush action unavailable")
        action.trigger()
        window = app.activeWindow().qwindow()
        window.showMaximized()
        yield
        smoothing = []
        for combo in window.findChildren(QComboBox):
            labels = [combo.itemText(i).replace("&", "").strip().lower()
                      for i in range(combo.count())]
            if (any("stabilizer" in label for label in labels) and
                    any("weighted" in label for label in labels) and
                    any("basic" in label for label in labels)):
                smoothing.append((combo, labels))
        if len(smoothing) != 1:
            raise RuntimeError("Cannot identify the unique source brush smoothing control")
        combo, labels = smoothing[0]
        disabled = [i for i, label in enumerate(labels)
                    if label in ("none", "no smoothing", "no smoothing.")]
        if len(disabled) != 1:
            raise RuntimeError("Source smoothing control has no explicit disabled mode: " + str(labels))
        combo.setCurrentIndex(disabled[0])
        yield
        if combo.currentIndex() != disabled[0]:
            raise RuntimeError("Source smoothing did not remain disabled")
        canvases = [widget for widget in window.findChildren(QWidget)
                    if widget.isVisible() and widget.metaObject().className() in
                    ("KisQPainterCanvas", "KisOpenGLCanvas2")]
        if len(canvases) != 1:
            raise RuntimeError("Expected exactly one visible source canvas, got " + str(len(canvases)))
        canvas = canvases[0]
        if "canvasCenter" in case:
            view.canvas().setWrapAroundMode(False)
            view.canvas().setLevelOfDetailMode(False)
            view.canvas().setPreferredCenter(QPointF(*case["canvasCenter"]))
            yield
            if abs(view.canvas().zoomLevel() - 1) > 1e-9 or view.canvas().wrapAroundMode():
                raise RuntimeError("Boundary source fixture changed zoom or wraparound")
        image_to_flake, invertible = view.flakeToImageTransform().inverted()
        if not invertible:
            raise RuntimeError("Source image transform is singular")
        flake_to_canvas = view.flakeToCanvasTransform()
        canvas_to_flake, invertible = flake_to_canvas.inverted()
        if not invertible:
            raise RuntimeError("Source canvas transform is singular")
        delivered = []
        # Qt timestamps belong to the input device clock, not the individual
        # stroke. Replaying 0..267 ms twice sends time backwards through the
        # source input manager even though the authored relative trace matches.
        timestamp_base = max(time.monotonic_ns() // 1000000,
                             getattr(self, "lastTabletTimeMs", -1) + 1)

        def send(row, kind, target, button, buttons, pressure):
            local = flake_to_canvas.map(image_to_flake.map(QPointF(row["x"], row["y"])))
            if not canvas.rect().contains(local.toPoint()):
                raise RuntimeError("Tablet fixture is outside the visible source canvas")
            global_pos = QPointF(canvas.mapToGlobal(local.toPoint())) + local - QPointF(local.toPoint())
            event = QTabletEvent(kind, local, global_pos, QTabletEvent.Stylus,
                                 QTabletEvent.Pen, pressure, row["tiltX"], row["tiltY"],
                                 0.0, row["rotation"], 0, Qt.NoModifier, 1, button, buttons)
            image_pos = view.flakeToImageTransform().map(canvas_to_flake.map(event.posF()))
            if max(abs(image_pos.x() - row["x"]), abs(image_pos.y() - row["y"])) > 1e-7:
                raise RuntimeError("Tablet event lost authored fractional image coordinates")
            event.setTimestamp(timestamp_base + round(row["timeUs"] / 1000))
            self.lastTabletTimeMs = event.timestamp()
            event.setAccepted(False)
            QApplication.sendEvent(target, event)
            delivered.append({"type": int(kind), "local": [event.posF().x(), event.posF().y()],
                              "image": [image_pos.x(), image_pos.y()],
                              "authoredImage": [row["x"], row["y"]],
                              "timeMs": event.timestamp(), "accepted": event.isAccepted(),
                              "pressure": event.pressure(), "device": int(event.device()),
                              "pointerType": int(event.pointerType()), "uniqueId": event.uniqueId(), "tilt": [event.xTilt(), event.yTilt()],
                              "rotation": event.rotation()})

        rows = case["events"]
        def tool_state():
            resource = view.currentBrushPreset()
            xml = Preset(resource).toXML()
            fields = {field.attrib.get("name"): field.text for field in fromstring(xml).findall("param")}
            return {"preset": resource.name(), "size": view.brushSize(),
                    "opacity": view.paintingOpacity(), "flow": view.paintingFlow(),
                    "presetSha256": hashlib.sha256(xml.encode()).hexdigest(),
                    "brushDefinition": fields.get("brush_definition"),
                    "paintOpAction": fields.get("PaintOpAction"),
                    "disablePressure": view.disablePressure(),
                    "pressureControls": {key: fields.get(key) for prefix in ("Size", "Opacity", "Flow")
                        for key in ("Pressure" + prefix, prefix + "UseCurve", prefix + "UseSameCurve",
                                    prefix + "commonCurve", prefix + "Sensor") if key in fields},
                    "precision": fields.get("KisPrecisionOption/precisionLevel"),
                    "smoothing": combo.currentText()}
        before_proximity = tool_state()
        send(rows[0], QEvent.TabletEnterProximity, QApplication.instance(), Qt.NoButton, Qt.NoButton, 0)
        yield
        after_proximity = tool_state()
        for phase, state in (("beforeProximity", before_proximity), ("afterProximity", after_proximity)):
            print("::notice title=Tablet source state::" + json.dumps(
                {"phase": phase, "state": state}, separators=(",", ":")).replace(
                    "%", "%25").replace("\n", "%0A").replace("\r", "%0D"), flush=True)
        # Establish the hover position before contact, as a real tablet does.
        # This also initializes the source input manager's pointer state before
        # its first pressure sample rather than inheriting the mouse position.
        send(rows[0], QEvent.TabletMove, canvas, Qt.NoButton, Qt.NoButton, 0)
        yield
        before_contact = tool_state()
        print("::notice title=Tablet source state::" + json.dumps(
            {"phase": "beforeContact", "state": before_contact}, separators=(",", ":")).replace(
                "%", "%25").replace("\n", "%0A").replace("\r", "%0D"), flush=True)
        for index, row in enumerate(rows):
            send(row, QEvent.TabletPress if index == 0 else QEvent.TabletMove,
                 canvas, Qt.LeftButton if index == 0 else Qt.NoButton,
                 Qt.LeftButton, row["pressure"])
            yield
        send(rows[-1], QEvent.TabletRelease, canvas, Qt.LeftButton, Qt.NoButton, 0)
        yield
        send(rows[-1], QEvent.TabletLeaveProximity, QApplication.instance(), Qt.NoButton, Qt.NoButton, 0)
        yield
        return {"mode": "Qt tablet events", "canvasClass": canvas.metaObject().className(),
                "smoothing": combo.currentText(),
                "stateBeforeProximity": before_proximity,
                "stateAfterProximity": after_proximity, "stateBeforeContact": before_contact,
                "canvasSize": [canvas.width(), canvas.height()], "events": delivered,
                "zoom": view.canvas().zoomLevel(),
                "preferredCenter": [view.canvas().preferredCenter().x(), view.canvas().preferredCenter().y()],
                "timing": "240 Hz event timestamps; Qt event-loop delivery, not physical device timing"}

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
            expected_names = set(fixtures["presets"])
            by_name, duplicate_originals = {}, []
            for resource in resources.values():
                resource_name = resource.name()
                if resource_name in expected_names and resource_name in by_name:
                    duplicate_originals.append(resource_name)
                by_name[resource_name] = resource
            missing = sorted(expected_names - set(by_name))
            loaded_originals = sorted(name for name in by_name if name.startswith("GZ Original"))
            if missing or duplicate_originals:
                # Bounded original-only metadata makes filename/version and
                # resource discovery failures diagnosable without substituting
                # a nearby preset or relaxing exact-name identity.
                inventory = {"expectedCount": len(expected_names),
                    "loadedOriginalCount": len(loaded_originals),
                    "missing": missing[:512], "duplicates": duplicate_originals[:512],
                    "loadedOriginals": loaded_originals[:512]}
                (output / "resource-index-failure.json").write_text(json.dumps(inventory, indent=2))
                report["resourceIndexFailure"] = inventory
                raise RuntimeError("Original reference preset identity mismatch; missing=" +
                                   repr(missing[:4]) + "; duplicates=" +
                                   repr(duplicate_originals[:4]) + "; loadedOriginalCount=" +
                                   str(len(loaded_originals)))
            if fixtures.get("group") == "pixelcontrols":
                # Inspect only public serialized setting metadata from bundled
                # presets. No brush images or engine implementation are copied.
                metadata = []
                for resource_name in sorted(by_name):
                    if "basic" not in resource_name.lower() or resource_name.startswith("GZ Original"):
                        continue
                    root = fromstring(Preset(by_name[resource_name]).toXML())
                    if root.attrib.get("paintopid") != "paintbrush":
                        continue
                    fields = {p.attrib["name"]: (p.text or "")[:160]
                              for p in root.findall("param") if "name" in p.attrib and
                              any(key in p.attrib["name"].lower() for key in ("flow", "opacity", "painting", "paintopaction"))}
                    metadata.append({"preset": resource_name, "settings": fields})
                    if len(metadata) == 3:
                        break
                report["bundledSettingMetadata"] = metadata
                (output / "bundled-settings.json").write_text(json.dumps(metadata, indent=2))
            w, h = fixtures["width"], fixtures["height"]
            checkpoint("create reference document")
            document = app.createDocument(w, h, "Original reference canvas", "RGBA", "U8", "sRGB-elle-V2-srgbtrc.icc", 72.0)
            view = app.activeWindow().addView(document)
            app.setActiveDocument(document)
            node = document.createNode("Original strips", "paintlayer")
            document.rootNode().addChildNode(node, None)
            document.setActiveNode(node)
            def clear_boundary_margin():
                # Source paint layers may retain pixels outside document bounds.
                # Clear a bounded halo before each independent boundary stroke.
                for x, y, width, height in ((-128, -128, w + 256, 128), (-128, h, w + 256, 128),
                                             (-128, 0, 128, h), (w, 0, 128, h)):
                    if not node.setPixelData(QByteArray(bytes(width * height * 4)), x, y, width, height):
                        raise RuntimeError("Cannot clear source off-document fixture margin")
            report["documentIsolation"] = "full-raster reset before each independent stroke; input mode recorded per case"
            execution_cases = list(fixtures["cases"])
            if fixtures.get("group") == "gihstatistics":
                random.Random(0x47494831).shuffle(execution_cases)
            for execution_ordinal, case in enumerate(execution_cases):
                name = case["preset"]
                if name not in by_name:
                    raise RuntimeError("Original reference preset was not loaded: " + name)
                # Krita integer RGBA API uses BGRA channel order, top row first.
                colors = [(255, 0, 0, 255), (0, 255, 0, 128), (0, 0, 255, 255), (0, 0, 0, 0)]
                row = (bytes(w * 4) if case.get("background") == "transparent" else
                       b"".join(bytes(colors[(x // 128) % 4]) for x in range(w)))
                if case.get("background") == "blend-stripes":
                    # Original encoded-sRGB swatches: non-primary colors,
                    # both sides of Overlay's midpoint, opaque/partial/zero alpha.
                    rgba = [(32, 160, 224, 255), (192, 64, 96, 128),
                            (128, 128, 128, 255), (224, 192, 32, 64),
                            (64, 224, 160, 255), (160, 32, 224, 192),
                            (0, 0, 0, 0), (255, 255, 255, 255)]
                    row = b"".join(bytes((c[2], c[1], c[0], c[3]))
                                   for x in range(w) for c in [rgba[(x // 8) % 8]])
                if case.get("background") == "wet-halves":
                    boundary = math.ceil(case.get("wetBoundary", 336))
                    row = bytes((255, 0, 0, 255)) * boundary + bytes((0, 0, 255, 128)) * (w - boundary)
                if case.get("wetPattern") == "two-pixel-red-yellow":
                    boundary = math.ceil(case.get("wetBoundary", 336))
                    row = row[:boundary * 4] + b"".join(bytes((0, 0, 255, 128) if (x // 2) % 2 == 0 else (0, 255, 255, 255)) for x in range(boundary, w))
                if case.get("wetPattern") == "opaque-black-white-column":
                    boundary = math.ceil(case.get("wetBoundary", 416))
                    column = 256 + case["wetKernelOffset"]
                    row = b"".join(bytes((255, 255, 255, 255) if x == column else (0, 0, 0, 255))
                                   for x in range(boundary)) + bytes((0, 0, 0, 0)) * (w - boundary)
                if "wetThirdBoundary" in case:
                    third = case["wetThirdRGBA8"]
                    boundary = case["wetThirdBoundary"]
                    row = row[:boundary * 4] + bytes((third[2], third[1], third[0], third[3])) * (w - boundary)
                initial = row * h
                if case.get("wetPattern") == "opaque-black-white-row":
                    boundary = math.ceil(case["wetBoundary"])
                    dark = bytes((0, 0, 0, 255)) * boundary + bytes((0, 0, 0, 0)) * (w - boundary)
                    light = bytes((255, 255, 255, 255)) * boundary + bytes((0, 0, 0, 0)) * (w - boundary)
                    white_y = 1024 + case["wetKernelOffset"]
                    initial = dark * white_y + light + dark * (h - white_y - 1)
                checkpoint(name + ": initialize pixels")
                if fixtures.get("group") == "wetboundary":
                    clear_boundary_margin()
                if not node.setPixelData(QByteArray(initial), 0, 0, w, h):
                    raise RuntimeError("Cannot initialize original reference raster")
                document.refreshProjection()
                document.waitForDone()
                checkpoint(name + ": select preset")
                view.setCurrentBrushPreset(by_name[name])
                if view.currentBrushPreset().name() != name:
                    raise RuntimeError("Source view did not select the exact original preset: " + name)
                view.setBrushSize(case["diameter"])
                view.setPaintingOpacity(case["opacity"])
                view.setPaintingFlow(case["flow"])
                view.setBrushRotation(case.get("brushRotationDegrees", 0))
                view.setEraserMode(False)
                view.setGlobalAlphaLock(False)
                view.setDisablePressure(False)
                view.setForeGroundColor(ManagedColor.fromQColor(QColor(*case.get("foregroundRGBA8", [220, 40, 30, 255])), view.canvas()))
                if case.get("sensorProbe", {}).get("verifyForeground"):
                    observed_color = view.foregroundColor().colorForCanvas(view.canvas())
                    observed_rgba = [observed_color.red(), observed_color.green(), observed_color.blue(), observed_color.alpha()]
                    if any(abs(a - b) > 1 for a, b in zip(observed_rgba, case["foregroundRGBA8"])):
                        raise RuntimeError("Source foreground differs from authored RGBA8")
                view.canvas().setZoomLevel(1.0)
                effective_view = {"diameter": view.brushSize(), "opacity": view.paintingOpacity(),
                                  "flow": view.paintingFlow(), "rotation": view.brushRotation(),
                                  "blend": view.currentBlendingMode(), "eraser": view.eraserMode(),
                                  "alphaLock": view.globalAlphaLock(), "disablePressure": view.disablePressure()}
                def control_failure(reason):
                    expected = {key: case[key] for key in ("diameter", "opacity", "flow")}
                    expected.update({"rotation": case.get("brushRotationDegrees", 0),
                                     "blend": case.get("overrides", {}).get("CompositeOp", "normal"),
                                     "eraser": False, "alphaLock": False, "disablePressure": False})
                    evidence = {"preset": name, "error": reason, "expected": expected,
                                "actual": effective_view,
                                "selectedPreset": view.currentBrushPreset().name(),
                                "resourceProbe": case.get("resourceProbe")}
                    # Preserve the settings actually present at failure before
                    # another fixture can change the selected resource.
                    failed_xml = Preset(view.currentBrushPreset()).toXML()
                    if len(failed_xml) <= 1024 * 1024:
                        (output / "controls-failure.effective.xml").write_text(failed_xml)
                    (output / "controls-failure.json").write_text(json.dumps(evidence, indent=2))
                    raise RuntimeError(reason + ": " + json.dumps(
                        {key: evidence[key] for key in ("preset", "expected", "actual")},
                        separators=(",", ":")))
                # The UI may quantize opacity/flow to one byte. Record that
                # value rather than pretending requested and effective values
                # are necessarily identical. Do not tune candidate inputs to
                # this observation: source quantization belongs in its profile.
                if abs(effective_view["diameter"] - case["diameter"]) > 1e-5 or any(
                        abs(effective_view[key] - case[key]) > 1 / 255 + 1e-9
                        for key in ("opacity", "flow")):
                    control_failure("Source view did not apply requested brush controls")
                if (abs(effective_view["rotation"] - case.get("brushRotationDegrees", 0)) > (.001 if "resourceProbe" in case else 1e-5) or effective_view["blend"] != case.get("overrides", {}).get("CompositeOp", "normal") or
                        any(effective_view[key] for key in ("eraser", "alphaLock", "disablePressure"))):
                    control_failure("Source view retained an unexpected painting modifier")
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
                if effective_fields.get("CompositeOp") != case.get("overrides", {}).get("CompositeOp", "normal"):
                    raise RuntimeError("Selected source preset lost requested blend mode")
                controls = {key: effective_fields.get(key) for key in ("CompositeOp", "PaintOpAction", "PressureFlow", "FlowUseCurve", "PressureOpacity", "OpacityUseCurve", "PressureColorRate", "ColorRateUseCurve", "SmudgeRateValue", "ColorRateValue", "SmudgeRadiusValue", "PressureSmudgeRate", "SmudgeRateUseNewEngine", "SmudgeRateMode", "SmudgeRateSmearAlpha")}
                kernel_geometry = None
                if fixtures.get("group") == "wetkernelcount":
                    definition = effective_fields.get("brush_definition", "")
                    brush = fromstring(definition)
                    mask = brush.find("MaskGenerator")
                    if mask is None or "diameter" not in mask.attrib:
                        raise RuntimeError("Effective source kernel geometry is missing")
                    kernel_geometry = {
                        "serializedDiameter": mask.attrib["diameter"],
                        "serializedRadius": effective_fields.get("SmudgeRadiusValue"),
                        "serializedRatio": mask.attrib.get("ratio"),
                        "serializedAngle": brush.attrib.get("angle"),
                        "serializedSpacing": brush.attrib.get("spacing"),
                        "effectiveViewDiameter": effective_view["diameter"],
                        "brushDefinition": definition}
                    if (abs(float(kernel_geometry["serializedDiameter"]) - case["diameter"]) > 1e-5 or
                            abs(float(kernel_geometry["serializedRadius"]) - float(case["overrides"]["SmudgeRadiusValue"])) > 1e-9):
                        raise RuntimeError("Source serialized kernel controls differ from requested values")
                size_curve_controls = None
                if fixtures.get("group") in ("pixelcurveknots", "pixelprecision") and case.get("overrides", {}).get("PressureSize") == "true":
                    keys = ("PressureSize", "SizeUseCurve", "SizeUseSameCurve",
                            "SizecommonCurve", "SizeSensor", "SizecurveMode")
                    size_curve_controls = {key: effective_fields.get(key) for key in keys}
                    for key in keys:
                        if key in case["overrides"] and size_curve_controls[key] != case["overrides"][key]:
                            raise RuntimeError("Source size curve setting did not round trip: " + key)
                transfer_controls = None
                if case.get("variant", "").startswith("transfer "):
                    transfer_controls = {key: effective_fields.get(key)
                        for prefix in ("Opacity", "Flow")
                        for key in ("Pressure" + prefix, prefix + "Value", prefix + "UseCurve",
                                    prefix + "UseSameCurve", prefix + "commonCurve", prefix + "Sensor")}
                    for key, expected in case["overrides"].items():
                        if effective_fields.get(key) != expected:
                            raise RuntimeError("Source transfer setting did not round trip: " + key)
                texture_controls = None
                if fixtures.get("group") in ("texturemasks", "texturestate"):
                    texture_controls = {key: value for key, value in effective_fields.items()
                                        if (key.startswith("Texture/") or key.startswith("PressureTexture/"))
                                        and key != "Texture/Pattern/Pattern"}
                    for key, wanted in case["overrides"].items():
                        if not (key.startswith("Texture/") or key.startswith("PressureTexture/")) or key == "Texture/Pattern/Pattern":
                            continue
                        actual = effective_fields.get(key)
                        try:
                            matches = abs(float(actual) - float(wanted)) <= 1e-9
                        except (TypeError, ValueError):
                            matches = actual == wanted
                        if not matches:
                            raise RuntimeError("Source texture setting differs from fixture: " + key)
                    patterns = [resource for resource in app.resources("pattern").values()
                                if Path(resource.filename()).name == "gz-original-steps.png"]
                    if len(patterns) != 1:
                        raise RuntimeError("Original source texture pattern did not resolve uniquely")
                    pattern = patterns[0].image()
                    if pattern.width() != 16 or pattern.height() != 16 or any(
                            pattern.pixelColor(x, 0).getRgb() != ((x // 4) * 85,) * 3 + (255,)
                            for x in range(16)):
                        raise RuntimeError("Source texture resource differs from original grayscale steps")
                resource_resolution = (resource_probe.resolve(app, output.parent, case, effective_fields)
                                       if "resourceProbe" in case else None)
                checkpoint(name + ": paint line")
                # The pinned 5.3.4.1 Python binding accepts QPoint, despite the
                # online C++ API describing QPointF. This fixture is integral.
                delivery = yield from self.paint_case(app, view, node, case)
                document.waitForDone()
                checkpoint(name + ": read painted pixels")
                raw = bytes(node.pixelData(0, 0, w, h))
                if len(raw) != w * h * 4:
                    raise RuntimeError("Reference API returned an incomplete raster")
                mask_evidence = (masked_probe.capture(case, effective_fields, delivery, raw, w)
                                 if "maskProbe" in case else None)
                if mask_evidence is not None:
                    masked_probe.emit_notices(name, mask_evidence)
                    (output / (name.replace(" ", "_") + ".mask.json")).write_text(
                        json.dumps(mask_evidence, indent=2))
                sensor_evidence = (sensor_probe.capture(case, effective_fields, delivery, raw, w)
                                   if "sensorProbe" in case else None)
                if sensor_evidence is not None:
                    if "mirrorProbe" in case:
                        mirror_probe.qualify(case, effective_fields, sensor_evidence)
                    if case["sensorProbe"].get("verifyForeground"):
                        sensor_evidence["effectiveForegroundDisplayRGBA8"] = observed_rgba
                    sensor_probe.emit_notices(name, sensor_evidence)
                    # Persist bounded diagnostics before a no-op or repeat gate
                    # can fail. Empty output is not silently excused as acquisition.
                    (output / (name.replace(" ", "_") + ".sensor.json")).write_text(
                        json.dumps(sensor_evidence, indent=2))
                if raw == initial and not case.get("expectUnchanged", False) and "changeExpectation" not in case:
                    raise RuntimeError("Reference API stroke produced no changed pixels")
                if raw != initial and case.get("expectUnchanged", False):
                    raise RuntimeError("Source no-op boundary case unexpectedly changed pixels")
                repeat_hashes = [hashlib.sha256(raw).hexdigest()]
                repetitions = 3 if fixtures.get("group") == "geometry" else (2 if case.get("variant") == "baseline" or fixtures.get("group") == "wetboundary" else 1)
                for repetition in range(1, repetitions):
                    # These fixtures disable random dynamics. Independent
                    # strokes must start from the same raster and must not
                    # inherit reservoirs or deferred state from the last one.
                    yield
                    if fixtures.get("group") == "wetboundary":
                        clear_boundary_margin()
                    if not node.setPixelData(QByteArray(initial), 0, 0, w, h):
                        raise RuntimeError("Cannot reset repeated reference raster")
                    document.refreshProjection()
                    document.waitForDone()
                    repeated_delivery = yield from self.paint_case(app, view, node, case)
                    document.waitForDone()
                    repeated = bytes(node.pixelData(0, 0, w, h))
                    repeat_hashes.append(hashlib.sha256(repeated).hexdigest())
                    if repeated != raw:
                        first_differences = []
                        changed_pixels = 0
                        for offset in range(0, len(raw), 4):
                            if raw[offset:offset+4] == repeated[offset:offset+4]:
                                continue
                            changed_pixels += 1
                            if len(first_differences) < 8:
                                first_differences.append([offset // 4 % w, offset // 4 // w,
                                                          list(raw[offset:offset+4]), list(repeated[offset:offset+4])])
                        def endpoints(value):
                            return [event for event in value.get("events", [])
                                    if event["type"] in (int(QEvent.TabletPress), int(QEvent.TabletRelease))]
                        detail = {"preset": name, "repetition": repetition, "sha256Pixels": repeat_hashes,
                                  "changedPixels": changed_pixels, "firstDifferencesBGRA8": first_differences,
                                  "firstToolState": delivery.get("stateAfterProximity"),
                                  "repeatedToolState": repeated_delivery.get("stateAfterProximity"),
                                  "firstEndpoints": endpoints(delivery), "repeatedEndpoints": endpoints(repeated_delivery)}
                        (output / "repeat-failure.json").write_text(json.dumps(detail, indent=2))
                        for suffix, pixels in (("first", raw), ("repeated", repeated)):
                            if not QImage(pixels, w, h, w * 4, QImage.Format_ARGB32).copy().save(str(output / ("repeat-failure-" + suffix + ".png"))):
                                raise RuntimeError("Cannot retain repeated source failure image")
                        raise RuntimeError("Deterministic source fixture changed across independent strokes: " + json.dumps(detail, separators=(",", ":")))
                    del repeated
                image = QImage(raw, w, h, w * 4, QImage.Format_ARGB32).copy()
                filename = name.replace(" ", "_") + ".png"
                if not image.save(str(output / filename), "PNG"):
                    raise RuntimeError("Cannot save reference fixture")
                # Bounded numerical probes expose pickup/drop-off relationships
                # without requiring a screenshot or access to hidden state.
                samples = []
                for x in sorted(set(range(240, 1809, 16)) | set(case.get("probePositions", []))):
                    offset = (1024 * w + x) * 4
                    blue, green, red, alpha = raw[offset:offset+4]
                    samples.append([x, red, green, blue, alpha])
                geometry = None
                if case.get("background") == "transparent" and fixtures.get("group") != "tablet":
                    # Exact alpha coverage around all seven short strokes.
                    # This small window is diagnostic; comparison still uses
                    # both full-canvas changed sets and their union.
                    if fixtures.get("group") in ("geometry", "pixelsoft", "pixelsoftsize", "pixelrectsize", "pixelaarange", "pixelshape", "pixelprecision", "pixelcurves", "pixelcurveknots"):
                        side = math.ceil(case["diameter"]) + 10
                        left = math.floor(case["start"][0] - case["diameter"] / 2) - 4
                        top = math.floor(case["start"][1] - case["diameter"] / 2) - 4
                        rect = [left, top, side, side]
                    else:
                        rect = [math.floor(case["start"][0]) - 24, math.floor(case["start"][1]) - 24, 80, 48]
                    if fixtures.get("group") in ("pixelsoftsize", "pixelrectsize") and case["diameter"] >= 64:
                        # Bounded edge diagnostic; full-canvas metric gates and
                        # retained images still qualify the entire larger mask.
                        rect = [math.floor(case["start"][0] - case["diameter"] / 2) - 2,
                                math.floor(case["start"][1]) - 8, 40, 16]
                    left, top, width, height = rect
                    alpha = bytes(raw[(y * w + x) * 4 + 3]
                                  for y in range(top, top + height)
                                  for x in range(left, left + width))
                    geometry = {"rect": rect, "encoding": "zlib-base64-alpha8-row-major",
                                "data": base64.b64encode(zlib.compress(alpha)).decode("ascii")}
                if fixtures.get("group") == "wethold" and case["diameter"] == 7 and case["variant"].endswith("dabs 2"):
                    # Exact second-dab RGBA resolves odd-size Smear placement;
                    # bounding boxes alone cannot identify its mask lattice.
                    left, top, width, height = 284, 1016, 16, 16
                    pixels = b"".join(raw[(y * w + left) * 4:(y * w + left + width) * 4]
                                      for y in range(top, top + height))
                    geometry = {"rect": [left, top, width, height],
                                "encoding": "zlib-base64-bgra8-row-major",
                                "data": base64.b64encode(zlib.compress(pixels)).decode("ascii")}
                if fixtures.get("group") in ("pixelpressure", "pixelsmallpressure") and case["spacing"] == 1:
                    # Sparse pressure ramps expose individual footprint spacing.
                    # Preserve exact visible-change bits, including transparent
                    # strips, rather than infer dab centers from sparse RGB probes.
                    left, top, width, height = 232, 1000, 1584, 48
                    packed = bytearray((width * height + 7) // 8)
                    for y in range(height):
                        for x in range(width):
                            offset = ((top + y) * w + left + x) * 4
                            changed = (raw[offset:offset+4] != initial[offset:offset+4] and
                                       (raw[offset+3] != 0 or initial[offset+3] != 0))
                            bit = y * width + x
                            if changed:
                                packed[bit // 8] |= 1 << (7 - bit % 8)
                    geometry = {"rect": [left, top, width, height],
                                "encoding": "zlib-base64-visible-change1-msb-row-major",
                                "data": base64.b64encode(zlib.compress(packed)).decode("ascii")}
                if fixtures.get("group") in ("texturemasks", "texturestate"):
                    left, top, width, height = case.get("textureProbeRect", [(240 if expected_engine == "paintbrush" else 560), 1020, 32, 8])
                    pixels = b"".join(raw[(y * w + left) * 4:(y * w + left + width) * 4]
                                      for y in range(top, top + height))
                    geometry = {"rect": [left, top, width, height],
                                "encoding": "zlib-base64-bgra8-row-major",
                                "data": base64.b64encode(zlib.compress(pixels)).decode("ascii")}
                boundary_outside = None
                if fixtures.get("group") == "wetboundary":
                    left, top, width, height = case["boundaryProbeRect"]
                    pixels = b"".join(raw[(y * w + left) * 4:(y * w + left + width) * 4]
                                      for y in range(top, top + height))
                    geometry = {"rect": [left, top, width, height],
                                "encoding": "zlib-base64-bgra8-row-major",
                                "data": base64.b64encode(zlib.compress(pixels)).decode("ascii")}
                    outside_rect = case["outsideProbeRect"]
                    outside = bytes(node.pixelData(*outside_rect))
                    if len(outside) != outside_rect[2] * outside_rect[3] * 4:
                        raise RuntimeError("Incomplete source outside-layer probe")
                    boundary_outside = {"rect": outside_rect, "encoding": "zlib-base64-bgra8-row-major",
                                        "data": base64.b64encode(zlib.compress(outside)).decode("ascii"),
                                        "nonzeroAlpha": sum(value != 0 for value in outside[3::4])}
                resource_readback = resource_probe.readback(raw, w, case) if "resourceProbe" in case else None
                report["records"].append({**case, "executionOrdinal": execution_ordinal, "image": filename, "sha256Pixels": hashlib.sha256(raw).hexdigest(), "repeatSha256Pixels": repeat_hashes, "profile": document.colorProfile(), "input": delivery["mode"], "deliveryReport": delivery, "changed": raw != initial, "centerlineRGBA8": samples, "geometryProbe": geometry, "effectiveView": effective_view, "effectivePresetXml": effective_name, "effectivePresetSha256": hashlib.sha256(effective_xml.encode()).hexdigest(), "effectiveWetControls": controls, "effectiveTextureControls": texture_controls, "effectivePixelTransferControls": transfer_controls, "effectiveSizeCurveControls": size_curve_controls, "effectiveKernelGeometry": kernel_geometry, "boundaryOutsideProbe": boundary_outside, "resourceResolution": resource_resolution, "resourceReadback": resource_readback, "sensorEvidence": sensor_evidence, "maskEvidence": mask_evidence})
                checkpoint(name + ": saved output")
                # Return to Qt between independent strokes. Calling processEvents
                # inside paint/document operations reentered native consumers;
                # one monolithic loop instead starved deferred view updates.
                yield
            canonical_order = {name: index for index, name in enumerate(fixtures["presets"])}
            report["records"].sort(key=lambda record: canonical_order[record["preset"]])
            report["resourceStatistics"] = resource_probe.summarize(report["records"])
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
