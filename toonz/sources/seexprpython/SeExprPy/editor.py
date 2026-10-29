"""Optional Python Qt authoring API backed by the native SeExpr evaluator.

This independently implemented SDK frontend follows the upstream expreditor2
widget interfaces. A host owns its document, animation and undo transaction;
apply signals carry the same boundary as the C++ authoring API.
"""
import copy
import json
import math
import os
from pathlib import Path
import re
import stat

from . import AST, ASTType, Edits, evaluate, functions, getComment, native_functions, validate

_api = os.environ.get("SEEXPR_QT_API", "").lower()
if _api not in ("", "pyqt5", "pyqt6"):
    raise ImportError("SEEXPR_QT_API must be pyqt5 or pyqt6")
if _api == "pyqt5":
    from PyQt5 import QtCore, QtGui, QtWidgets
else:
    try:
        from PyQt6 import QtCore, QtGui, QtWidgets
    except ImportError:
        if _api == "pyqt6":
            raise
        from PyQt5 import QtCore, QtGui, QtWidgets

Signal = QtCore.pyqtSignal


def _enum(owner, group, name):
    return getattr(getattr(owner, group, owner), name)


def _exec(dialog):
    return getattr(dialog, "exec", None)() if hasattr(dialog, "exec") else dialog.exec_()


def _qposition(text, index):
    return len(text[:index].encode("utf-16-le")) // 2


def _sourcepart(source, node):
    start, end = node.range
    return source.encode("utf-8")[start:end].decode("utf-8")


def _literal(node):
    if node.type == ASTType.String:
        return node.value
    if node.type == ASTType.Num:
        return node.value if math.isfinite(node.value) else None
    if node.type == ASTType.UnaryOp and node.value in ("+", "-"):
        children = node.children()
        value = _literal(children[0]) if len(children) == 1 else None
        if isinstance(value, (int, float)):
            return value if node.value == "+" else -value
    if node.type == ASTType.Vec:
        values = [_literal(child) for child in node.children()]
        if all(isinstance(value, (int, float)) for value in values):
            return values
    return None


def _format(value):
    if isinstance(value, (list, tuple)):
        return "[" + ",".join(_format(item) for item in value) + "]"
    if isinstance(value, str):
        return json.dumps(value, ensure_ascii=False)
    if not math.isfinite(float(value)):
        raise ValueError("Control values must be finite")
    return repr(float(value))


class _ColorDialog(QtWidgets.QDialog):
    """Keep numeric RGB values separate from the display color's precision."""
    def __init__(self, values, parent=None):
        super().__init__(parent)
        self.setObjectName("expressionColorDialog")
        self.setWindowTitle(self.tr("Color"))
        self._values = list(values)
        self.fields = []
        layout = QtWidgets.QVBoxLayout(self)
        form = QtWidgets.QFormLayout()
        layout.addLayout(form)
        for name, value in zip(("R", "G", "B"), values):
            field = QtWidgets.QLineEdit(_format(value), self)
            field.setObjectName("component" + name)
            self.fields.append(field)
            form.addRow(name, field)
        choose = QtWidgets.QPushButton(self.tr("Choose Display Color"), self)
        choose.setToolTip(self.tr("The display picker uses colors from 0 to 1. RGB fields retain the full numeric range."))
        choose.clicked.connect(self._choose)
        layout.addWidget(choose)
        self.error = QtWidgets.QLabel(self)
        self.error.setTextFormat(_enum(QtCore.Qt, "TextFormat", "PlainText"))
        layout.addWidget(self.error)
        buttons = QtWidgets.QDialogButtonBox(
            _enum(QtWidgets.QDialogButtonBox, "StandardButton", "Ok") |
            _enum(QtWidgets.QDialogButtonBox, "StandardButton", "Cancel"), self)
        buttons.accepted.connect(self.accept)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)

    def _read(self):
        try:
            values = [float(field.text()) for field in self.fields]
            if not all(math.isfinite(value) for value in values):
                raise ValueError
        except ValueError:
            self.error.setText(self.tr("Enter finite RGB values."))
            return None
        self.error.clear()
        return values

    def _choose(self):
        values = self._read()
        if values is None:
            return
        initial = QtGui.QColor.fromRgbF(*[min(1.0, max(0.0, value)) for value in values])
        selected = QtWidgets.QColorDialog.getColor(initial, self)
        # QColor can quantize even an unchanged display color. Only an actual
        # selection replaces numeric HDR values with the display-picker values.
        if selected.isValid() and selected != initial:
            for field, value in zip(self.fields, (selected.redF(), selected.greenF(), selected.blueF())):
                field.setText(_format(value))

    def accept(self):
        values = self._read()
        if values is not None:
            self._values = values
            super().accept()


def _edit_color(parent, values):
    dialog = _ColorDialog(values, parent)
    try:
        if _exec(dialog) == _enum(QtWidgets.QDialog, "DialogCode", "Accepted"):
            return list(dialog._values)
        return None
    finally:
        dialog.deleteLater()


class _Highlighter(QtGui.QSyntaxHighlighter):
    def highlightBlock(self, text):
        rules = [(r"\b(?:if|else|def|extern|FLOAT|STRING|CONSTANT|UNIFORM|VARYING)\b", "#8457b0"),
                 (r"\$?[A-Za-z_][A-Za-z0-9_.]*(?=\s*\()", "#227c92"),
                 (r"\b(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?\b", "#39773e"),
                 (r'"(?:\\.|[^"\\])*"', "#996126"), (r"#.*$", "#777777")]
        for pattern, color in rules:
            style = QtGui.QTextCharFormat()
            foreground = QtGui.QColor(color)
            if QtWidgets.QApplication.palette().base().color().lightnessF() < 0.4:
                foreground = foreground.lighter(170)
            style.setForeground(foreground)
            for match in re.finditer(pattern, text):
                begin = _qposition(text, match.start())
                end = _qposition(text, match.end())
                self.setFormat(begin, end - begin, style)


class ExprCurve(QtWidgets.QWidget):
    curveChanged = Signal()
    _color = False

    def __init__(self, parent=None, pLabel="", vLabel="", iLabel=""):
        super().__init__(parent)
        self._updating = False
        self._points = [[0.0, [0.0] * 3, 1], [1.0, [1.0] * 3, 1]] if self._color else [[0.0, 0.0, 1], [1.0, 1.0, 1]]
        self._samples = []
        layout = QtWidgets.QVBoxLayout(self)
        self.table = QtWidgets.QTableWidget(0, 3, self)
        self.table.setHorizontalHeaderLabels([pLabel or self.tr("Position"),
                                              vLabel or self.tr("Value"),
                                              iLabel or self.tr("Interpolation")])
        layout.addWidget(self.table)
        buttons = QtWidgets.QHBoxLayout()
        add = QtWidgets.QPushButton(self.tr("Add Point"), self)
        remove = QtWidgets.QPushButton(self.tr("Remove Selected"), self)
        buttons.addWidget(add)
        buttons.addWidget(remove)
        layout.addLayout(buttons)
        self.graph = _CurveGraph(self)
        layout.addWidget(self.graph)
        self.table.itemChanged.connect(self._read)
        add.clicked.connect(self._add)
        remove.clicked.connect(self._remove)
        self.setPoints(self._points)

    def points(self):
        return json.loads(json.dumps(self._points))

    def setPoints(self, points):
        converted = []
        for point in points:
            if not isinstance(point, (list, tuple)) or len(point) not in (2, 3):
                raise ValueError("A curve point requires position, value and optional interpolation")
            position = float(point[0])
            value = point[1]
            raw_mode = float(point[2]) if len(point) > 2 else 1.0
            if not math.isfinite(position) or not math.isfinite(raw_mode) or raw_mode not in range(5):
                raise ValueError("Invalid curve position or interpolation")
            mode = int(raw_mode)
            if self._color:
                if not isinstance(value, (list, tuple)) or len(value) != 3:
                    raise ValueError("A color curve point requires RGB")
                value = [float(component) for component in value]
                if not all(math.isfinite(component) for component in value):
                    raise ValueError("Color curve values must be finite")
            else:
                value = float(value)
                if not math.isfinite(value):
                    raise ValueError("Curve values must be finite")
            converted.append([position, value, mode])
        if not 1 <= len(converted) <= 64:
            raise ValueError("A curve requires one to 64 points")
        self._points = converted
        self._updating = True
        self.table.setRowCount(len(converted))
        for row, (position, value, mode) in enumerate(converted):
            for column, content in enumerate((str(position), _format(value), str(mode))):
                self.table.setItem(row, column, QtWidgets.QTableWidgetItem(content))
        self._updating = False
        self._sample()

    def _read(self, _item=None):
        if self._updating:
            return
        try:
            points = []
            for row in range(self.table.rowCount()):
                raw = self.table.item(row, 1).text()
                value = json.loads(raw) if self._color else float(raw)
                points.append([float(self.table.item(row, 0).text()), value,
                               int(self.table.item(row, 2).text())])
            self.setPoints(points)
            self.setToolTip("")
        except (ValueError, TypeError, AttributeError):
            self.setToolTip(self.tr("Use finite point values and interpolation modes 0 through 4."))
            return
        self.curveChanged.emit()

    def _add(self):
        if len(self._points) < 64:
            self.setPoints(self._points + [[1.0, [1.0] * 3 if self._color else 1.0, 1]])
            self.curveChanged.emit()

    def _remove(self):
        row = self.table.currentRow()
        if row >= 0 and len(self._points) > 1:
            self.setPoints(self._points[:row] + self._points[row + 1:])
            self.curveChanged.emit()

    def expression(self, input_expression="$u"):
        values = [input_expression]
        for position, value, mode in self._points:
            values.extend((_format(position), _format(value), str(mode)))
        return ("ccurve" if self._color else "curve") + "(" + ",".join(values) + ")"

    def _sample(self):
        self._samples = []
        try:
            source = self.expression()
            for index in range(33):
                position = index / 32
                value = evaluate(source, {"u": position}, "vector" if self._color else "scalar")
                self._samples.append((position, value))
        except RuntimeError as error:
            self.setToolTip(str(error))
        self.graph.update()


class ExprColorCurve(ExprCurve):
    _color = True

    def __init__(self, parent=None, pLabel="", vLabel="", iLabel=""):
        super().__init__(parent, pLabel, vLabel, iLabel)
        self.setPoints([[0.0, [0.0] * 3, 1], [1.0, [1.0] * 3, 1]])


class ExprAnimCurve(QtWidgets.QWidget):
    """Edit an owned animation-curve draft and apply it as one host change.

    ``parameters`` uses the native ``type: animcurve`` JSON contract. Unknown
    properties and optional key identifiers survive every edit. Numeric text
    fields retain double precision; the graph always samples the native engine.
    ``link`` is host refresh metadata and never opens a file or runs code.
    An optional ``setLinkResolver`` callback supplies explicit linked snapshots.
    """
    curveChanged = Signal()
    tangentNames = ("fixed", "auto", "linear", "flat", "step", "stepnext", "spline")
    infinityNames = ("constant", "linear", "cycle", "cycleRelative", "oscillate")
    _numbers = ("time", "value", "inWeight", "outWeight", "inAngle", "outAngle")
    _key_defaults = dict(zip(_numbers, (0.0, 0.0, 1.0, 1.0, 0.0, 0.0)),
                         inTangent="auto", outTangent="auto", weightsLocked=False)
    _defaults = {"type": "animcurve", "preInfinity": "constant", "postInfinity": "constant",
                 "weighted": False, "link": "", "keys": []}

    def __init__(self, parent=None):
        super().__init__(parent)
        self._updating = False
        self._inline = None
        self._link_resolver = None
        self._key_slots = []
        self._samples = []
        self._preview_keys = []
        self._params = copy.deepcopy(self._defaults)
        layout = QtWidgets.QVBoxLayout(self)
        settings = QtWidgets.QFormLayout()
        layout.addLayout(settings)
        self.preInfinity = self._combo(self.infinityNames, "constant")
        self.preInfinity.setObjectName("preInfinity")
        self.postInfinity = self._combo(self.infinityNames, "constant")
        self.postInfinity.setObjectName("postInfinity")
        self.weighted = QtWidgets.QCheckBox(self.tr("Use weighted tangents"), self)
        self.weighted.setObjectName("weighted")
        self.link = QtWidgets.QLineEdit(self)
        self.link.setObjectName("link")
        self.link.setMaxLength(65536)
        self.link.setToolTip(self.tr("Refresh identifier supplied to the host application."))
        settings.addRow(self.tr("Pre Infinity"), self.preInfinity)
        settings.addRow(self.tr("Post Infinity"), self.postInfinity)
        settings.addRow(self.weighted)
        settings.addRow(self.tr("Link"), self.link)
        self.refreshButton = QtWidgets.QPushButton(self.tr("Refresh Linked Curve"), self)
        self.refreshButton.setObjectName("animationCurveRefresh")
        settings.addRow(self.refreshButton)
        self.table = QtWidgets.QTableWidget(0, 9, self)
        self.table.setObjectName("animationKeys")
        self.table.setHorizontalHeaderLabels([self.tr("Time"), self.tr("Value"),
            self.tr("In Weight"), self.tr("Out Weight"), self.tr("In Angle (degrees)"),
            self.tr("Out Angle (degrees)"), self.tr("In Tangent"), self.tr("Out Tangent"),
            self.tr("Lock Weights")])
        layout.addWidget(self.table)
        actions = QtWidgets.QHBoxLayout()
        self.addButton = QtWidgets.QPushButton(self.tr("Add Key"), self)
        self.removeButton = QtWidgets.QPushButton(self.tr("Remove Selected"), self)
        self.upButton = QtWidgets.QPushButton(self.tr("Move Up"), self)
        self.downButton = QtWidgets.QPushButton(self.tr("Move Down"), self)
        self.sortButton = QtWidgets.QPushButton(self.tr("Sort by Time"), self)
        for button, operation in ((self.addButton, self._add), (self.removeButton, self._remove),
                (self.upButton, lambda: self._move(-1)), (self.downButton, lambda: self._move(1)),
                (self.sortButton, self._sort)):
            actions.addWidget(button)
            button.clicked.connect(operation)
        layout.addLayout(actions)
        self.graph = _AnimCurveGraph(self)
        layout.addWidget(self.graph)
        self.error = QtWidgets.QLabel(self)
        self.error.setObjectName("animationCurveError")
        self.error.setTextFormat(_enum(QtCore.Qt, "TextFormat", "PlainText"))
        self.error.setWordWrap(True)
        layout.addWidget(self.error)
        decisions = QtWidgets.QHBoxLayout()
        self.cancelButton = QtWidgets.QPushButton(self.tr("Cancel"), self)
        self.applyButton = QtWidgets.QPushButton(self.tr("Apply"), self)
        self.cancelButton.setObjectName("animationCurveCancel")
        self.applyButton.setObjectName("animationCurveApply")
        decisions.addWidget(self.cancelButton)
        decisions.addWidget(self.applyButton)
        layout.addLayout(decisions)
        self.cancelButton.clicked.connect(self.cancel)
        self.applyButton.clicked.connect(self.apply)
        self.table.itemChanged.connect(self._item_changed)
        self.preInfinity.currentTextChanged.connect(self._changed)
        self.postInfinity.currentTextChanged.connect(self._changed)
        self.weighted.toggled.connect(self._changed)
        self.link.textChanged.connect(self._changed)
        self.refreshButton.clicked.connect(self.refreshLinkedCurve)
        self.setLinkResolver(None)
        self.setParams(self._params)

    def _combo(self, choices, value):
        combo = QtWidgets.QComboBox(self)
        combo.addItems(choices)
        self._select(combo, value)
        return combo

    @staticmethod
    def _select(combo, value):
        text = str(value)
        index = combo.findText(text)
        if index < 0:
            combo.addItem(text)
            index = combo.count() - 1
        combo.setCurrentIndex(index)

    @staticmethod
    def _number_text(value):
        return repr(value) if isinstance(value, (int, float)) else str(value)

    @classmethod
    def _validated(cls, parameters):
        if not isinstance(parameters, dict) or parameters.get("type", "animcurve") != "animcurve":
            raise ValueError("Animation-curve metadata must have type 'animcurve'.")
        result = copy.deepcopy(cls._defaults)
        result.update(copy.deepcopy(parameters))
        for name in ("preInfinity", "postInfinity"):
            if result[name] not in cls.infinityNames:
                raise ValueError("Unsupported " + name + ": choose " + ", ".join(cls.infinityNames) + ".")
        if not isinstance(result["weighted"], bool):
            raise ValueError("Animation-curve weighted must be a boolean.")
        if not isinstance(result["link"], str) or "\0" in result["link"] or len(result["link"].encode("utf-8")) > 65536:
            raise ValueError("Animation-curve link must be a string without null characters, at most 65536 UTF-8 bytes.")
        if not isinstance(result["keys"], list) or len(result["keys"]) > 64:
            raise ValueError("An animation curve requires zero to 64 keys.")
        ids = set()
        previous = None
        for index, original in enumerate(result["keys"]):
            prefix = "Key " + str(index + 1) + ": "
            if not isinstance(original, dict):
                raise ValueError(prefix + "metadata must be an object.")
            key = dict(cls._key_defaults)
            key.update(original)
            if "id" in key:
                identifier = key["id"]
                if not isinstance(identifier, str) or re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]{0,47}", identifier) is None or identifier in ids:
                    raise ValueError(prefix + "identifier must be valid and unique.")
                ids.add(identifier)
            for name in cls._numbers:
                value = key[name]
                if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
                    raise ValueError(prefix + name + " must be a finite number.")
                if name in ("inWeight", "outWeight") and value < 0:
                    raise ValueError(prefix + name + " must be nonnegative.")
            for name in ("inTangent", "outTangent"):
                if key[name] not in cls.tangentNames:
                    raise ValueError(prefix + name + " must be one of " + ", ".join(cls.tangentNames) + ".")
            if not isinstance(key["weightsLocked"], bool):
                raise ValueError(prefix + "weightsLocked must be a boolean.")
            if previous is not None and (key["time"] <= previous or not math.isfinite(key["time"] - previous)):
                raise ValueError(prefix + "times must be increasing and distinct with finite intervals; use Sort by Time to reorder keys.")
            previous = key["time"]
            result["keys"][index] = key
        if len(result["keys"]) > 1 and not math.isfinite(result["keys"][-1]["time"] - result["keys"][0]["time"]):
            raise ValueError("Animation-curve time range exceeds the finite numerical range.")
        return result

    def parameters(self):
        """Return the last applied metadata, including properties unknown to Qt."""
        return copy.deepcopy(self._params)

    def draftParameters(self):
        """Return the visible draft without applying it to the host."""
        return self._draft()

    def setLinkResolver(self, resolver):
        """Set ``resolver(link, metadata_copy) -> snapshot_metadata`` or None.

        The callback belongs to the host and runs only on explicit Refresh. It
        may raise an exception to display an error. Unknown control properties
        and properties of matching key identifiers (or times) are retained.
        """
        if resolver is not None and not callable(resolver):
            raise TypeError("The animation-curve link resolver must be callable or None.")
        self._link_resolver = resolver
        self.refreshButton.setEnabled(resolver is not None)
        self.refreshButton.setToolTip(self.tr("Refresh the draft from the host's linked curve.") if resolver is not None else
                                      self.tr("The host has not provided an animation-curve link resolver."))

    @staticmethod
    def _matching_key(key, keys):
        identifier = key.get("id")
        if identifier is not None:
            return next((index for index, old in enumerate(keys) if old.get("id") == identifier), None)
        return next((index for index, old in enumerate(keys) if old.get("time", 0.0) == key.get("time", 0.0)), None)

    def refreshLinkedCurve(self):
        """Refresh only the draft; failures preserve all current field text."""
        if self._link_resolver is None:
            self.error.setText(self.tr("The host has not provided an animation-curve link resolver."))
            return False
        try:
            draft = self._draft()
            self._validated(draft)
            snapshot = self._link_resolver(self.link.text(), copy.deepcopy(draft))
            if not isinstance(snapshot, dict):
                raise ValueError("The animation-curve link resolver must return metadata as an object.")
            refreshed = copy.deepcopy(draft)
            refreshed.update(copy.deepcopy(snapshot))
            slots = []
            old_keys = draft.get("keys", [])
            if isinstance(refreshed.get("keys", []), list):
                keys = []
                for key in refreshed.get("keys", []):
                    if not isinstance(key, dict):
                        raise ValueError("Each refreshed animation key must be an object.")
                    match = self._matching_key(key, old_keys)
                    merged = copy.deepcopy(old_keys[match]) if match is not None else {}
                    merged.update(key)
                    keys.append(merged)
                    slots.append(self._row_slots[match] if match is not None else None)
                refreshed["keys"] = keys
            self._sample(refreshed)
            self._load_draft(refreshed, slots)
            return True
        except Exception as error:
            self.error.setText(str(error) or self.tr("The linked animation curve could not be refreshed."))
            return False

    def setParams(self, parameters):
        if not isinstance(parameters, dict) or not isinstance(parameters.get("keys", []), list):
            raise ValueError("Animation-curve metadata must be an object with a keys array.")
        if any(not isinstance(key, dict) for key in parameters.get("keys", [])):
            raise ValueError("Each animation key must be an object.")
        self._inline = None
        self._params = copy.deepcopy(parameters)
        self._key_slots = [None] * len(parameters.get("keys", []))
        self.cancel()

    def keys(self):
        return copy.deepcopy(self._params.get("keys", []))

    def setKeys(self, keys):
        parameters = self.parameters()
        parameters["keys"] = copy.deepcopy(keys)
        self.setParams(parameters)

    def _rows(self):
        checked = _enum(QtCore.Qt, "CheckState", "Checked")
        return [{"base": copy.deepcopy(base), "slot": self._row_slots[row],
                 "texts": [self.table.item(row, column).text() for column in range(6)],
                 "inTangent": self.table.cellWidget(row, 6).currentText(),
                 "outTangent": self.table.cellWidget(row, 7).currentText(),
                 "weightsLocked": self.table.item(row, 8).checkState() == checked}
                for row, base in enumerate(self._row_bases)]

    def _render_rows(self, records):
        self._updating = True
        self.table.setRowCount(0)
        self.table.setRowCount(len(records))
        self._row_bases = [copy.deepcopy(record["base"]) for record in records]
        self._row_slots = [record["slot"] for record in records]
        for row, record in enumerate(records):
            for column, text in enumerate(record["texts"]):
                self.table.setItem(row, column, QtWidgets.QTableWidgetItem(text))
            for column, name in ((6, "inTangent"), (7, "outTangent")):
                combo = self._combo(self.tangentNames, record[name])
                self.table.setCellWidget(row, column, combo)
                combo.currentTextChanged.connect(self._changed)
            locked = QtWidgets.QTableWidgetItem()
            locked.setFlags((locked.flags() | _enum(QtCore.Qt, "ItemFlag", "ItemIsUserCheckable")) &
                            ~_enum(QtCore.Qt, "ItemFlag", "ItemIsEditable"))
            locked.setCheckState(_enum(QtCore.Qt, "CheckState", "Checked" if record["weightsLocked"] else "Unchecked"))
            self.table.setItem(row, 8, locked)
        self._updating = False
        self.addButton.setEnabled(len(records) < 64)

    @classmethod
    def _record(cls, key, slot=None):
        values = dict(cls._key_defaults)
        values.update(key)
        return {"base": copy.deepcopy(key), "slot": slot,
                "texts": [cls._number_text(values[name]) for name in cls._numbers],
                "inTangent": values["inTangent"], "outTangent": values["outTangent"],
                "weightsLocked": values["weightsLocked"]}

    def _draft(self):
        parameters = copy.deepcopy(self._working)
        for name, value in (("preInfinity", self.preInfinity.currentText()),
                ("postInfinity", self.postInfinity.currentText()), ("weighted", self.weighted.isChecked()),
                ("link", self.link.text())):
            # Invalid metadata remains visible until the corresponding field is
            # corrected; untouched fields never acquire a different JSON type.
            if value != self._global_views[name]:
                parameters[name] = value
        keys = []
        for row, record in enumerate(self._rows()):
            key = record["base"]
            for name, text in zip(self._numbers, record["texts"]):
                original = key.get(name, self._key_defaults[name])
                if text != self._number_text(original):
                    try:
                        key[name] = float(text)
                    except ValueError:
                        raise ValueError("Key " + str(row + 1) + ": " + name + " must be a finite number.") from None
            for name in ("inTangent", "outTangent", "weightsLocked"):
                original = key.get(name, self._key_defaults[name])
                displayed = bool(original) if name == "weightsLocked" else str(original)
                if record[name] != displayed:
                    key[name] = record[name]
            keys.append(key)
        if keys != parameters.get("keys", []):
            parameters["keys"] = keys
        return parameters

    @classmethod
    def _arguments(cls, parameters):
        model = cls._validated(parameters)
        return cls._raw_arguments(model)

    @classmethod
    def _raw_arguments(cls, parameters):
        model = dict(cls._defaults)
        model.update(parameters)
        weighted = int(model["weighted"]) if isinstance(model["weighted"], bool) else model["weighted"]
        values = [model["preInfinity"], model["postInfinity"], weighted, model["link"]]
        for original in model["keys"]:
            key = dict(cls._key_defaults)
            key.update(original)
            values.extend(key[name] for name in cls._numbers)
            locked = int(key["weightsLocked"]) if isinstance(key["weightsLocked"], bool) else key["weightsLocked"]
            values.extend((key["inTangent"], key["outTangent"], locked))
        return values

    def expression(self, input_expression=None):
        """Return applied source; an imported call retains untouched literals."""
        return self._expression(self._params, self._key_slots, input_expression)

    def _expression(self, parameters, slots, input_expression=None):
        values = self._arguments(parameters)
        if self._inline is None or input_expression is not None:
            lookup = input_expression if input_expression is not None else "$u"
            return "animCurve(" + lookup + "," + ",".join(_format(value) for value in values) + ")"
        original = self._inline
        edits = []
        same_keys = slots == list(range(len(original["parameters"]["keys"])))
        original_values = self._raw_arguments(original["parameters"])
        count = len(values) if same_keys else 4
        for index in range(count):
            if values[index] != original_values[index]:
                edits.append((original["args"][index + 1].range, _format(values[index])))
        if not same_keys:
            # Keep the complete lookup and global-setting prefix, including
            # comments. Structural edits replace only the key argument suffix.
            replacement = "," + ",".join(_format(value) for value in values[4:]) if values[4:] else ""
            suffix = original["source"].encode("utf-8")[original["args"][4].range[1]:original["range"][1] - 1].decode("utf-8")
            tokens = re.findall(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|#[^\n]*', suffix)
            comments = [token for token in tokens if token.startswith("#")]
            if comments:
                replacement = "\n" + "\n".join(comments) + "\n" + replacement
            edits.append(((original["args"][4].range[1], original["range"][1] - 1), replacement))
        data = original["source"].encode("utf-8")
        for (start, end), replacement in sorted(edits, reverse=True):
            data = data[:start] + replacement.encode("utf-8") + data[end:]
        return data.decode("utf-8")

    @classmethod
    def _import(cls, source):
        try:
            root = AST(source).root()
        except RuntimeError as error:
            raise ValueError(str(error)) from None
        pending = [root] if root else []
        call = None
        while pending:
            node = pending.pop()
            if node.type == ASTType.Call and node.value == "animCurve":
                call = node
                break
            pending.extend(reversed(node.children()))
        if call is None:
            raise ValueError("No animation-curve call was found.")
        args = call.children()
        if len(args) < 5 or (len(args) - 5) % 9:
            raise ValueError("An animation curve needs lookup, pre/post infinity, weighted, link and nine fields per key.")
        literals = [_literal(arg) for arg in args[1:]]
        if any(value is None for value in literals):
            raise ValueError("Animation-curve settings and keys must be literals; the lookup may be any expression.")
        parameters = {"type": "animcurve", "preInfinity": literals[0], "postInfinity": literals[1],
                      "weighted": literals[2], "link": literals[3], "keys": []}
        if isinstance(literals[2], (int, float)):
            parameters["weighted"] = bool(literals[2])
        for index in range(4, len(literals), 9):
            key = dict(zip(cls._numbers + ("inTangent", "outTangent", "weightsLocked"), literals[index:index + 9]))
            if isinstance(key["weightsLocked"], (int, float)):
                key["weightsLocked"] = bool(key["weightsLocked"])
            parameters["keys"].append(key)
        return {"source": source, "args": args, "range": call.range, "parameters": parameters}

    def setExpression(self, source):
        imported = self._import(str(source))
        self.setParams(imported["parameters"])
        self._inline = imported
        self._key_slots = list(range(len(self.keys())))
        self.cancel()

    def _item_changed(self, item):
        if self._updating:
            return
        if item.column() in (2, 3) and self.table.item(item.row(), 8).checkState() == _enum(QtCore.Qt, "CheckState", "Checked"):
            self._updating = True
            self.table.item(item.row(), 5 - item.column()).setText(item.text())
            self._updating = False
        self._changed()

    def _changed(self, *_args):
        if self._updating:
            return
        try:
            draft = self._draft()
            self._sample(draft)
            self.error.clear()
            self.applyButton.setEnabled(True)
        except (ValueError, RuntimeError, OverflowError) as error:
            self._samples = []
            self._preview_keys = []
            self.graph.update()
            self.error.setText(str(error))
            self.applyButton.setEnabled(False)

    def _sample(self, parameters):
        model = self._validated(parameters)
        keys = model["keys"]
        first, last = (keys[0]["time"], keys[-1]["time"]) if keys else (-1.0, 1.0)
        span = last - first
        margin = span * 0.1 if span else max(1.0, abs(first) * 0.1)
        low = first - margin if math.isfinite(first - margin) else first
        high = last + margin if math.isfinite(last + margin) else last
        if low == high:
            low, high = first - 1.0, last + 1.0
        times = {low * (1.0 - index / 64) + high * (index / 64) for index in range(65)}
        times.update(key["time"] for key in keys)
        source = "animCurve($u," + ",".join(_format(value) for value in self._arguments(model)) + ")"
        samples = []
        for time in sorted(times):
            value = evaluate(source, {"u": time}, "scalar")
            if not math.isfinite(value):
                raise ValueError("Animation-curve preview returned a nonfinite value at time " + repr(time) + ".")
            samples.append((time, value))
        self._samples = samples
        self._preview_keys = [(key["time"], key["value"]) for key in keys]
        self.graph.update()

    def apply(self):
        try:
            draft = self._draft()
            self._sample(draft)
            slots = list(self._row_slots)
            source = self._expression(draft, slots) if self._inline else None
            imported = self._import(source) if source is not None else None
        except (ValueError, RuntimeError, OverflowError) as error:
            self.error.setText(str(error))
            return False
        changed = draft != self._params
        self._params = draft
        if imported is not None:
            self._inline = imported
            self._key_slots = list(range(len(self.keys())))
        else:
            self._key_slots = slots
        self.cancel()
        if changed:
            self.curveChanged.emit()
        return True

    def cancel(self):
        self._load_draft(self._params, self._key_slots)

    def _load_draft(self, parameters, slots):
        self._working = copy.deepcopy(parameters)
        self._updating = True
        for name, combo in (("preInfinity", self.preInfinity), ("postInfinity", self.postInfinity)):
            self._select(combo, self._working.get(name, self._defaults[name]))
        self.weighted.setChecked(bool(self._working.get("weighted", False)))
        self.link.setText(str(self._working.get("link", "")))
        self._global_views = {"preInfinity": self.preInfinity.currentText(),
            "postInfinity": self.postInfinity.currentText(), "weighted": self.weighted.isChecked(),
            "link": self.link.text()}
        records = [self._record(key, slot) for key, slot in zip(self._working.get("keys", []), slots)]
        self._render_rows(records)
        self._updating = False
        self._changed()

    def _add(self):
        records = self._rows()
        if len(records) >= 64:
            self.error.setText(self.tr("An animation curve supports at most 64 keys."))
            return
        time = 0.0
        try:
            times = [float(record["texts"][0]) for record in records]
            if times and all(math.isfinite(value) for value in times):
                time = max(times) + 1.0
                if not math.isfinite(time) or time <= max(times):
                    time = math.nextafter(max(times), math.inf)
        except ValueError:
            pass
        key = dict(self._key_defaults)
        key["time"] = time
        ids = {record["base"]["id"] for record in records if isinstance(record["base"].get("id"), str)}
        identifier = 0
        while "key_" + str(identifier) in ids:
            identifier += 1
        key["id"] = "key_" + str(identifier)
        records.append(self._record(key))
        self._render_rows(records)
        self.table.setCurrentCell(len(records) - 1, 0)
        self._changed()

    def _remove(self):
        row = self.table.currentRow()
        records = self._rows()
        if 0 <= row < len(records):
            records.pop(row)
            self._render_rows(records)
            if records:
                self.table.setCurrentCell(min(row, len(records) - 1), 0)
            self._changed()

    def _move(self, step):
        row = self.table.currentRow()
        records = self._rows()
        target = row + step
        if 0 <= row < len(records) and 0 <= target < len(records):
            records[row], records[target] = records[target], records[row]
            self._render_rows(records)
            self.table.setCurrentCell(target, 0)
            self._changed()

    def _sort(self):
        records = self._rows()
        try:
            times = [float(record["texts"][0]) for record in records]
            if not all(math.isfinite(value) for value in times):
                raise ValueError
        except ValueError:
            self.error.setText(self.tr("Enter finite key times before sorting."))
            return
        records = [record for _time, _index, record in sorted(zip(times, range(len(records)), records))]
        self._render_rows(records)
        self._changed()


class _AnimCurveGraph(QtWidgets.QWidget):
    def __init__(self, curve):
        super().__init__(curve)
        self.curve = curve
        self.setMinimumHeight(160)
        self.setAccessibleName(self.tr("Animation Curve Preview"))

    def paintEvent(self, _event):
        painter = QtGui.QPainter(self)
        painter.fillRect(self.rect(), self.palette().base())
        plot = self.rect().adjusted(12, 18, -12, -20)
        painter.setPen(self.palette().mid().color())
        painter.drawRect(plot)
        samples = self.curve._samples
        if not samples:
            return
        first, last = samples[0][0], samples[-1][0]
        values = [value for _time, value in samples] + [value for _time, value in self.curve._preview_keys]
        low, high = min(values), max(values)
        # Normalize before subtraction so finite HDR values cannot overflow
        # merely while mapping the native samples to display coordinates.
        xscale, yscale = max(1.0, abs(first), abs(last)), max(1.0, abs(low), abs(high))
        left, right = first / xscale, last / xscale
        bottom, top = low / yscale, high / yscale
        if left == right:
            left, right = left - 0.5, right + 0.5
        if bottom == top:
            bottom, top = bottom - 0.5, top + 0.5
        def point(time, value):
            return QtCore.QPointF(plot.left() + (time / xscale - left) / (right - left) * plot.width(),
                                 plot.bottom() - (value / yscale - bottom) / (top - bottom) * plot.height())
        painter.setPen(self.palette().highlight().color())
        path = QtGui.QPainterPath()
        for index, (time, value) in enumerate(samples):
            path.moveTo(point(time, value)) if index == 0 else path.lineTo(point(time, value))
        painter.drawPath(path)
        for time, value in self.curve._preview_keys:
            painter.drawEllipse(point(time, value), 3.0, 3.0)
        painter.setPen(self.palette().text().color())
        painter.drawText(12, 13, self.tr("Value ") + repr(low) + " … " + repr(high))
        painter.drawText(12, self.height() - 4, self.tr("Time ") + repr(first) + " … " + repr(last))


class _CurveGraph(QtWidgets.QWidget):
    def __init__(self, curve):
        super().__init__(curve)
        self.curve = curve
        self.setMinimumHeight(100)
        self.setAccessibleName(self.tr("Curve Preview"))

    def paintEvent(self, _event):
        painter = QtGui.QPainter(self)
        painter.fillRect(self.rect(), self.palette().base())
        plot = self.rect().adjusted(6, 6, -6, -6)
        painter.setPen(self.palette().mid().color())
        painter.drawRect(plot)
        channels = 3 if self.curve._color else 1
        colors = [QtGui.QColor("#c24f4f"), QtGui.QColor("#4c9c52"), QtGui.QColor("#4e74be")]
        for channel in range(channels):
            path = QtGui.QPainterPath()
            for index, (position, value) in enumerate(self.curve._samples):
                component = value[channel] if channels == 3 else value
                point = QtCore.QPointF(plot.left() + position * plot.width(),
                    plot.bottom() - min(1.0, max(0.0, component)) * plot.height())
                path.moveTo(point) if index == 0 else path.lineTo(point)
            painter.setPen(colors[channel] if channels == 3 else self.palette().highlight().color())
            painter.drawPath(path)


class ExprColorSwatchWidget(QtWidgets.QWidget):
    swatchChanged = Signal()

    def __init__(self, indexLabel=False, parent=None):
        super().__init__(parent)
        self._colors = [[0.0, 0.0, 0.0], [1.0, 1.0, 1.0]]
        self._index_label = indexLabel
        layout = QtWidgets.QVBoxLayout(self)
        self.list = QtWidgets.QListWidget(self)
        layout.addWidget(self.list)
        actions = QtWidgets.QHBoxLayout()
        for label, callback in ((self.tr("Add"), self._add), (self.tr("Remove"), self._remove),
                                (self.tr("Move Up"), lambda: self._move(-1)),
                                (self.tr("Move Down"), lambda: self._move(1))):
            button = QtWidgets.QPushButton(label, self)
            button.clicked.connect(callback)
            actions.addWidget(button)
        layout.addLayout(actions)
        self.list.itemDoubleClicked.connect(self._edit)
        self.setColors(self._colors)

    def colors(self):
        return [list(color) for color in self._colors]

    def setColors(self, colors):
        values = [[float(component) for component in color] for color in colors]
        if not 1 <= len(values) <= 64 or any(len(color) != 3 or
                not all(math.isfinite(c) for c in color) for color in values):
            raise ValueError("Swatches require one to 64 finite RGB colors")
        self._colors = values
        self.list.clear()
        for index, color in enumerate(values):
            item = QtWidgets.QListWidgetItem((str(index) + ": " if self._index_label else "") + _format(color))
            item.setBackground(QtGui.QColor.fromRgbF(*[min(1.0, max(0.0, c)) for c in color]))
            self.list.addItem(item)

    def _add(self):
        if len(self._colors) < 64:
            self.setColors(self._colors + [[1.0] * 3])
            self.swatchChanged.emit()

    def _remove(self):
        row = self.list.currentRow()
        if row >= 0 and len(self._colors) > 1:
            self.setColors(self._colors[:row] + self._colors[row + 1:])
            self.swatchChanged.emit()

    def _move(self, step):
        row = self.list.currentRow()
        target = row + step
        if 0 <= row < len(self._colors) and 0 <= target < len(self._colors):
            self._colors[row], self._colors[target] = self._colors[target], self._colors[row]
            self.setColors(self._colors)
            self.list.setCurrentRow(target)
            self.swatchChanged.emit()

    def _edit(self, item):
        row = self.list.row(item)
        if row < 0:
            return
        color = _edit_color(self, self._colors[row])
        if color is not None and color != self._colors[row]:
            self._colors[row] = color
            self.setColors(self._colors)
            self.swatchChanged.emit()


class ExprDeepWater(QtWidgets.QWidget):
    deepWaterChanged = Signal()
    _defaults = {"resolution": 10, "tileSize": 100.0, "lengthCutoff": 0.01,
                 "amplitude": 1.0, "windAngle": 0.0, "windSpeed": 10.0,
                 "directionalFactorExponent": 2.0, "directionalReflectionDamping": 0.0,
                 "flowDirection": [0.0, 0.0, 0.0], "sharpen": 0.0, "time": 0.0,
                 "filterWidth": 1.0}

    def __init__(self, parent=None):
        super().__init__(parent)
        self._updating = False
        self._fields = {}
        layout = QtWidgets.QVBoxLayout(self)
        form = QtWidgets.QFormLayout()
        layout.addLayout(form)
        for name, default in self._defaults.items():
            if name == "flowDirection":
                for index, component in enumerate(default):
                    self._field(form, "flow" + str(index), component)
            else:
                self._field(form, name, default)
        self.graph = ExprCurve(self)
        self.graph.table.hide()
        for button in self.graph.findChildren(QtWidgets.QPushButton): button.hide()
        layout.addWidget(self.graph)
        self.setParams(self._defaults)

    def _field(self, form, name, value):
        field = QtWidgets.QDoubleSpinBox(self)
        field.setDecimals(0 if name == "resolution" else 6)
        field.setRange(-1000000.0, 1000000.0)
        if name == "resolution": field.setRange(1, 20)
        if name in ("tileSize", "windSpeed"): field.setMinimum(0.000001)
        if name in ("lengthCutoff", "amplitude", "directionalFactorExponent"):
            field.setMinimum(0)
        if name == "directionalFactorExponent": field.setMaximum(32)
        field.setValue(value)
        field.valueChanged.connect(self._changed)
        self._fields[name] = field
        form.addRow(name, field)

    def parameters(self):
        result = {name: field.value() for name, field in self._fields.items() if not name.startswith("flow")}
        result["resolution"] = int(result["resolution"])
        result["flowDirection"] = [self._fields["flow" + str(i)].value() for i in range(3)]
        return result

    def setParams(self, parameters):
        merged = dict(self._defaults)
        merged.update(parameters)
        self._updating = True
        for name, field in self._fields.items():
            value = merged["flowDirection"][int(name[-1])] if name.startswith("flow") else merged[name]
            field.setValue(float(value))
        self._updating = False
        self._changed()

    @staticmethod
    def spectrumValue(parameters, position):
        p = dict(ExprDeepWater._defaults)
        p.update(parameters)
        wind, amplitude, exponent = (float(p[key]) for key in
                                    ("windSpeed", "amplitude", "directionalFactorExponent"))
        cutoff = float(p["lengthCutoff"])
        if not all(math.isfinite(value) for value in (wind, amplitude, cutoff, exponent, position)) or wind <= 0 or amplitude < 0 or cutoff < 0 or not 0 <= exponent <= 32:
            raise ValueError("Invalid spectrum parameters")
        if amplitude == 0:
            return 0.0
        count = math.ceil((math.log(3000000.0) + 5.0) / 0.01) + 1
        index = min(count - 1, max(0, int(min(1.0, max(0.0, position)) * count - 1)))
        k = math.exp(index * 0.01 - 5.0)
        length = wind * wind / 9.8
        power = lambda x: amplitude * math.exp(-1.0 / (x * x)) / math.pow(x, 4.0 + exponent)
        peak = math.sqrt(2.0 / (4.0 + exponent))
        return 0.95 / math.pow(power(peak), 0.1) * math.pow(
            max(0.0, power(k) * math.exp(-math.pow(k / length * cutoff, 2.0))), 0.1)

    def _changed(self, _value=None):
        if self._updating:
            return
        parameters = self.parameters()
        try:
            points = [[index / 32, self.spectrumValue(parameters, index / 32), 1] for index in range(33)]
            self.graph.setPoints(points)
            self.setToolTip("")
        except (ValueError, OverflowError) as error:
            self.setToolTip(str(error))
            return
        self.deepWaterChanged.emit()


class ExprControlCollection(QtWidgets.QWidget):
    controlChanged = Signal(int)

    def __init__(self, parent=None):
        super().__init__(parent)
        self.form = QtWidgets.QFormLayout(self)
        self._controls = []
        self._animation_link_resolver = None

    def controls(self):
        return list(self._controls)

    def setAnimationCurveLinkResolver(self, resolver):
        if resolver is not None and not callable(resolver):
            raise TypeError("The animation-curve link resolver must be callable or None.")
        self._animation_link_resolver = resolver
        for control in self._controls:
            if isinstance(control, ExprAnimCurve):
                control.setLinkResolver(resolver)

    def clear(self):
        while self.form.count():
            item = self.form.takeAt(0)
            if item.widget(): item.widget().deleteLater()
        self._controls = []

    def rebuild(self, source, changed):
        self.clear()
        try:
            root = AST(source).root()
        except RuntimeError:
            return
        assignments, strings, animations = [], [], []
        def walk(node, assigned_literal=False, assignment_name=""):
            children = node.children()
            if node.type == ASTType.Assign and children: assignments.append(node)
            if node.type == ASTType.Call and node.value == "animCurve":
                animations.append((assignment_name or self.tr("Animation Curve"), node))
            if node.type == ASTType.String and not assigned_literal:
                annotation = getComment(node, source).split()
                if len(annotation) == 2: strings.append((annotation[1], node))
            for index, child in enumerate(children):
                walk(child, node.type == ASTType.Assign and index == 0,
                     node.value if node.type == ASTType.Assign else assignment_name)
        if root: walk(root)
        records = [(assignment.value, assignment.children()[0]) for assignment in assignments] + strings
        recorded = {node.range for _label, node in records}
        records.extend((label, node) for label, node in animations if node.range not in recorded)
        for label, node in records:
            def notify(interval, replacement, identifier=len(self._controls)):
                changed(interval, replacement)
                self.controlChanged.emit(identifier)
            literal = _literal(node)
            widget = None
            callback = None
            comment = getComment(node, source)
            real = r"([+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?)"
            bounds = re.fullmatch(r"\s*" + real + r"\s*,\s*" + real + r"\s*", comment)
            minimum, maximum = (float(bounds[i]) for i in (1, 2)) if bounds else (0.0, 1.0)
            if not all(math.isfinite(value) for value in (minimum, maximum)) or minimum > maximum:
                bounds = None
                minimum, maximum = 0.0, 1.0
            if isinstance(literal, (int, float)):
                widget = QtWidgets.QDoubleSpinBox(self)
                widget.setDecimals(0 if bounds and not re.search(r"[.eE]", comment) else 8)
                widget.setRange(min(minimum, literal), max(maximum, literal))
                widget.setValue(literal)
                callback = lambda value, n=node, action=notify: action(n.range, _format(value))
                widget.valueChanged.connect(callback)
            elif isinstance(literal, str):
                annotation = comment.split()
                if len(annotation) == 2: label = annotation[1]
                if annotation and annotation[0] in ("file", "directory"):
                    widget = QtWidgets.QWidget(self)
                    row = QtWidgets.QHBoxLayout(widget)
                    field = QtWidgets.QLineEdit(literal, widget)
                    row.addWidget(field)
                    browse = QtWidgets.QPushButton(self.tr("Browse"), widget)
                    row.addWidget(browse)
                    def choose(_checked=False, line=field, kind=annotation[0], action=notify, n=node):
                        if kind == "directory":
                            selected = QtWidgets.QFileDialog.getExistingDirectory(self, self.tr("Choose Directory"), line.text())
                        else:
                            selected, _filter = QtWidgets.QFileDialog.getOpenFileName(self, self.tr("Choose File"), line.text())
                        if selected:
                            line.setText(selected)
                            action(n.range, _format(selected))
                    browse.clicked.connect(choose)
                else:
                    field = widget = QtWidgets.QLineEdit(literal, self)
                field.editingFinished.connect(lambda n=node, w=field, action=notify: action(n.range, _format(w.text())))
            elif isinstance(literal, list) and len(literal) == 3:
                widget = QtWidgets.QWidget(self)
                row = QtWidgets.QHBoxLayout(widget)
                spins = []
                components = list(literal)
                for value in literal:
                    spin = QtWidgets.QDoubleSpinBox(widget)
                    spin.setDecimals(8)
                    spin.setRange(min(minimum, value), max(maximum, value))
                    spin.setValue(value)
                    spins.append(spin)
                    row.addWidget(spin)
                def update(value=None, index=None, n=node, values=components, action=notify):
                    if index is not None:
                        values[index] = value
                    action(n.range, _format(values))
                for index, spin in enumerate(spins):
                    spin.valueChanged.connect(lambda value, i=index, action=update: action(value, i))
                if bounds is None:
                    choose = QtWidgets.QPushButton(self.tr("Color"), widget)
                    row.addWidget(choose)
                    def pick(_checked=False, fields=spins, values=components, changed_color=update):
                        color = _edit_color(self, values)
                        if color is not None and color != values:
                            for field, value in zip(fields, color):
                                blocked = field.blockSignals(True)
                                try:
                                    field.setRange(min(field.minimum(), value), max(field.maximum(), value))
                                    field.setValue(value)
                                finally:
                                    field.blockSignals(blocked)
                            values[:] = color
                            changed_color()
                    choose.clicked.connect(pick)
            elif node.type == ASTType.Call:
                children = node.children()
                if node.value == "animCurve":
                    animation = ExprAnimCurve(self)
                    animation.setLinkResolver(self._animation_link_resolver)
                    try:
                        animation.setExpression(_sourcepart(source, node))
                        widget = animation
                        animation.curveChanged.connect(lambda n=node, w=animation, action=notify:
                                                       action(n.range, w.expression()))
                    except ValueError as error:
                        animation.deleteLater()
                        widget = QtWidgets.QLabel(str(error), self)
                        widget.setTextFormat(_enum(QtCore.Qt, "TextFormat", "PlainText"))
                        widget.setWordWrap(True)
                elif node.value in ("curve", "ccurve") and len(children) >= 4 and (len(children) - 1) % 3 == 0:
                    try:
                        points = [[_literal(children[i]), _literal(children[i + 1]), _literal(children[i + 2])]
                                  for i in range(1, len(children), 3)]
                        widget = ExprColorCurve(self) if node.value == "ccurve" else ExprCurve(self)
                        widget.setPoints(points)
                        input_expression = _sourcepart(source, children[0])
                        widget.curveChanged.connect(lambda n=node, w=widget, arg=input_expression, action=notify:
                                                    action(n.range, w.expression(arg)))
                    except (TypeError, ValueError):
                        if widget: widget.deleteLater()
                        widget = None
                elif node.value == "swatch" and len(children) >= 2:
                    colors = [_literal(child) for child in children[1:]]
                    if all(isinstance(color, list) and len(color) == 3 for color in colors):
                        widget = ExprColorSwatchWidget(True, self)
                        widget.setColors(colors)
                        input_expression = _sourcepart(source, children[0])
                        widget.swatchChanged.connect(lambda n=node, w=widget, arg=input_expression, action=notify:
                            action(n.range, "swatch(" + arg + "," + ",".join(_format(c) for c in w.colors()) + ")"))
                elif node.value == "deepWater" and len(children) == 13:
                    values = [_literal(child) for child in children[1:]]
                    if all(value is not None for value in values):
                        widget = ExprDeepWater(self)
                        widget.setParams(dict(zip(ExprDeepWater._defaults, values)))
                        input_expression = _sourcepart(source, children[0])
                        widget.deepWaterChanged.connect(lambda n=node, w=widget, arg=input_expression, action=notify:
                            action(n.range, "deepWater(" + arg + "," + ",".join(
                                _format(w.parameters()[key]) for key in ExprDeepWater._defaults) + ")"))
            if widget is not None:
                index = len(self._controls)
                widget.setObjectName("control_" + label)
                caption = QtWidgets.QLabel(label, self)
                caption.setTextFormat(_enum(QtCore.Qt, "TextFormat", "PlainText"))
                self.form.addRow(caption, widget)
                self._controls.append(widget)


class _SourceEdit(QtWidgets.QPlainTextEdit):
    def __init__(self, parent):
        super().__init__(parent)
        self._completer = QtWidgets.QCompleter(self)
        self._completer.setWidget(self)
        self._completer.setCompletionMode(_enum(QtWidgets.QCompleter, "CompletionMode", "PopupCompletion"))
        self._completer.setCaseSensitivity(_enum(QtCore.Qt, "CaseSensitivity", "CaseInsensitive"))
        self._completer.activated[str].connect(self._insert)

    def setCompletions(self, names):
        model = self._completer.model()
        if not isinstance(model, QtCore.QStringListModel):
            model = QtCore.QStringListModel(self._completer)
            self._completer.setModel(model)
        model.setStringList(sorted(names))

    def _insert(self, name):
        cursor = self.textCursor()
        cursor.select(_enum(QtGui.QTextCursor, "SelectionType", "WordUnderCursor"))
        cursor.insertText(name)
        self.setTextCursor(cursor)

    def keyPressEvent(self, event):
        key = event.key()
        if self._completer.popup().isVisible() and key in (
                _enum(QtCore.Qt, "Key", "Key_Return"), _enum(QtCore.Qt, "Key", "Key_Enter"),
                _enum(QtCore.Qt, "Key", "Key_Escape"), _enum(QtCore.Qt, "Key", "Key_Tab")):
            event.ignore()
            return
        requested = key == _enum(QtCore.Qt, "Key", "Key_Space") and bool(
            event.modifiers() & _enum(QtCore.Qt, "KeyboardModifier", "ControlModifier"))
        if not requested: super().keyPressEvent(event)
        cursor = self.textCursor()
        cursor.select(_enum(QtGui.QTextCursor, "SelectionType", "WordUnderCursor"))
        prefix = cursor.selectedText()
        if not requested and (len(prefix) < 2 or not event.text() or not re.match(r"\w+$", prefix)):
            self._completer.popup().hide()
            return
        self._completer.setCompletionPrefix(prefix)
        self._completer.complete(self.cursorRect())


class ExprEditor(QtWidgets.QWidget):
    applyExpression = Signal()
    clearExpression = Signal()
    previewRequested = Signal()

    def __init__(self, parent=None, collection=None):
        super().__init__(parent)
        self.collection = collection or ExprControlCollection(self)
        self._extra_functions = {}
        self._extra_variables = {}
        self._variables = {"u": 0.5, "v": 0.5, "x": 0.0, "y": 0.0,
                           "frame": 1.0, "time": 0.0, "seed": 0.0}
        self._functions = {}
        self._result_type = "vector"
        layout = QtWidgets.QVBoxLayout(self)
        self.edit = _SourceEdit(self)
        self.edit.setObjectName("expressionSource")
        self._highlighter = _Highlighter(self.edit.document())
        layout.addWidget(self.edit)
        self.status = QtWidgets.QLabel(self)
        self.status.setTextFormat(_enum(QtCore.Qt, "TextFormat", "PlainText"))
        self.status.setWordWrap(True)
        layout.addWidget(self.status)
        self.help = QtWidgets.QComboBox(self)
        self.help.setEditable(True)
        self.help.setInsertPolicy(_enum(QtWidgets.QComboBox, "InsertPolicy", "NoInsert"))
        layout.addWidget(self.help)
        self.help.activated.connect(self._show_help)
        if collection is None: layout.addWidget(self.collection)
        actions = QtWidgets.QHBoxLayout()
        check = QtWidgets.QPushButton(self.tr("Validate"), self)
        preview = QtWidgets.QPushButton(self.tr("Preview"), self)
        apply = QtWidgets.QPushButton(self.tr("Apply"), self)
        for button in (check, preview, apply): actions.addWidget(button)
        layout.addLayout(actions)
        check.clicked.connect(self.validateExpression)
        preview.clicked.connect(self.previewExpression)
        apply.clicked.connect(self._apply)
        self._timer = QtCore.QTimer(self)
        self._timer.setSingleShot(True)
        self._timer.setInterval(200)
        self._timer.timeout.connect(self.rebuildControls)
        self.edit.textChanged.connect(self.exprChanged)
        self.updateCompleter()

    def getExpr(self):
        return self.edit.toPlainText()

    def setExpr(self, expression, apply=False):
        self.edit.setPlainText(str(expression))
        self.rebuildControls()
        if apply: self._apply()

    def setContext(self, variables=None, functions=None, result_type="vector"):
        if variables is not None: self._variables = dict(variables)
        if functions is not None: self._functions = dict(functions)
        self._result_type = result_type
        self.updateCompleter()

    def setAnimationCurveLinkResolver(self, resolver):
        """Provide the host snapshot callback to current and rebuilt controls."""
        self.collection.setAnimationCurveLinkResolver(resolver)

    def exprChanged(self):
        self.clearErrors()
        self._timer.start()

    def rebuildControls(self):
        # setExpr rebuilds immediately. Do not let its pending debounce timer
        # replace a newly opened animation control and discard its draft.
        self._timer.stop()
        source = self.getExpr()
        self.collection.rebuild(source, lambda interval, replacement:
                                self._control_change(source, interval, replacement))
        self.updateCompleter()

    def controlChanged(self, _identifier):
        self._apply()

    def _control_change(self, previous, interval, replacement):
        if self.getExpr() != previous:
            self.rebuildControls()
            return
        start, end = interval
        source = previous.encode("utf-8")
        self.setExpr((source[:start] + replacement.encode("utf-8") + source[end:]).decode("utf-8"))
        self.applyExpression.emit()

    def addError(self, startPos, endPos, error):
        source = self.getExpr().encode("utf-8")
        start = len(source[:max(0, startPos)].decode("utf-8", errors="ignore").encode("utf-16-le")) // 2
        end = len(source[:max(0, endPos)].decode("utf-8", errors="ignore").encode("utf-16-le")) // 2
        selection = QtWidgets.QTextEdit.ExtraSelection()
        selection.cursor = self.edit.textCursor()
        selection.cursor.setPosition(start)
        selection.cursor.setPosition(max(start, end), _enum(QtGui.QTextCursor, "MoveMode", "KeepAnchor"))
        selection.format.setUnderlineColor(QtGui.QColor("#cc4444"))
        selection.format.setUnderlineStyle(_enum(QtGui.QTextCharFormat, "UnderlineStyle", "WaveUnderline"))
        self.edit.setExtraSelections(self.edit.extraSelections() + [selection])
        self.status.setText(str(error))

    def clearErrors(self):
        self.edit.setExtraSelections([])
        self.status.clear()

    def clearExtraCompleters(self):
        self._extra_functions.clear()
        self._extra_variables.clear()
        self.updateCompleter()

    def registerExtraFunction(self, name, docString):
        self._extra_functions[str(name)] = str(docString)

    def registerExtraVariable(self, name, docString):
        self._extra_variables[str(name)] = str(docString)

    def updateCompleter(self):
        self._documentation = {entry["name"]: entry["documentation"] for entry in functions()}
        self._documentation.update({entry["name"]: entry["documentation"]
                                    for entry in native_functions()})
        self._documentation.update({name: self.tr("Host variable") for name in self._variables})
        self._documentation.update({name: definition.get("documentation", self.tr("Host function"))
                                    for name, definition in self._functions.items()})
        self._documentation.update(self._extra_functions)
        self._documentation.update(self._extra_variables)
        # Inspect syntax rather than matching source text: comments and strings
        # may contain assignment-like text, and computed locals need completion
        # even when they do not produce a literal control.
        try:
            root = AST(self.getExpr()).root()
        except RuntimeError:
            root = None
        pending = [root] if root else []
        while pending:
            node = pending.pop()
            if node.type == ASTType.Assign:
                name = str(node.value).lstrip("$")
                self._documentation.setdefault(name, self.tr("Local variable"))
            pending.extend(node.children())
        self.help.clear()
        self.help.addItems(sorted(self._documentation))
        completer = self.help.completer()
        completer.setCaseSensitivity(_enum(QtCore.Qt, "CaseSensitivity", "CaseInsensitive"))
        self.edit.setCompletions(self._documentation)

    def _show_help(self, _index=None):
        self.status.setText(self._documentation.get(self.help.currentText(), ""))

    def validateExpression(self):
        self.clearErrors()
        try:
            result = validate(self.getExpr(), self._variables, self._result_type,
                              functions=self._functions)
        except RuntimeError as error:
            self.status.setText(str(error))
            return False
        for error in result["errors"]:
            self.addError(error["begin"], error["end"], error["message"])
        if result["valid"]: self.status.setText(self.tr("Expression is valid."))
        return result["valid"]

    def previewExpression(self):
        if not self.validateExpression(): return None
        try:
            answer = evaluate(self.getExpr(), self._variables, self._result_type,
                              functions=self._functions)
        except RuntimeError as error:
            self.status.setText(str(error))
            return None
        self.status.setText(self.tr("Preview: ") + str(answer))
        self.previewRequested.emit()
        return answer

    def _apply(self):
        if self.validateExpression(): self.applyExpression.emit()


class ExprDialog(QtWidgets.QDialog):
    preview = Signal()
    expressionApplied = Signal()

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setWindowTitle(self.tr("Expression Editor"))
        layout = QtWidgets.QVBoxLayout(self)
        self.editor = ExprEditor(self)
        layout.addWidget(self.editor)
        self.editor.applyExpression.connect(self.applyExpression)
        self.editor.previewRequested.connect(self.preview.emit)
        self.resize(650, 650)

    def getExpressionString(self):
        return self.editor.getExpr()

    def setExpressionString(self, expression):
        self.editor.setExpr(expression)

    def applyExpression(self):
        if self.editor.validateExpression(): self.expressionApplied.emit()

    def clearExpression(self):
        self.editor.setExpr("")
        self.editor.clearExpression.emit()


class ExprBrowser(QtWidgets.QWidget):
    _MAX_SCAN_ENTRIES = 4096
    _MAX_SCAN_DEPTH = 32

    def __init__(self, parent=None, editor=None):
        super().__init__(parent)
        self.editor = editor
        self._paths = {}
        self._selected = ""
        layout = QtWidgets.QVBoxLayout(self)
        self.search = QtWidgets.QLineEdit(self)
        self.search.setPlaceholderText(self.tr("Search presets"))
        self.list = QtWidgets.QListWidget(self)
        self.error = QtWidgets.QLabel(self)
        self.error.setTextFormat(_enum(QtCore.Qt, "TextFormat", "PlainText"))
        self.error.setWordWrap(True)
        layout.addWidget(self.search)
        layout.addWidget(self.list)
        layout.addWidget(self.error)
        actions = QtWidgets.QHBoxLayout()
        for caption, operation in ((self.tr("Save"), self.saveExpression),
                                   (self.tr("Save As"), self.saveExpressionAs),
                                   (self.tr("Save Local As"), self.saveLocalExpressionAs)):
            button = QtWidgets.QPushButton(caption, self)
            button.clicked.connect(lambda _checked=False, action=operation: self._save(action))
            actions.addWidget(button)
        layout.addLayout(actions)
        self.search.textChanged.connect(self.update)
        self.list.itemActivated.connect(self._activate)

    def _activate(self, item):
        try:
            self.selectPath(item.data(_enum(QtCore.Qt, "ItemDataRole", "UserRole")))
            self.error.clear()
        except (OSError, ValueError) as error:
            self.error.setText(str(error))

    def _save(self, operation):
        try:
            operation()
            self.error.clear()
        except (OSError, ValueError) as error:
            self.error.setText(str(error))

    def addPath(self, name, path):
        self._paths[str(name)] = Path(path)
        self.update()

    def getSelectedPath(self):
        return self._selected

    def selectPath(self, path):
        path = Path(path)
        if path.stat().st_size > 1048576:
            raise ValueError("Preset exceeds the SDK size limit")
        source = path.read_text(encoding="utf-8")
        if path.suffix.lower() == ".json":
            value = json.loads(source)
            if not isinstance(value, dict): raise ValueError("Preset envelope must be an object")
            value = value.get("document", value)
            if not isinstance(value, dict): raise ValueError("Preset document must be an object")
            source = value.get("rgb", value.get("source", ""))
            if not isinstance(source, str): raise ValueError("Preset source must be a string")
        if self.editor is not None: self.editor.setExpr(source)
        self._selected = str(path)

    def setSearchPath(self, context, path):
        self.addPath(context, path)

    def _presetPaths(self, directory):
        # Explicit roots may contain nested collections. Do not follow links
        # back into the collection or outside it, and bound traversal so opening
        # a preset browser cannot scan an unbounded directory tree.
        pending = [(directory, 0)]
        visited = 0
        while pending:
            folder, depth = pending.pop()
            entries = []
            with os.scandir(folder) as iterator:
                for entry in iterator:
                    visited += 1
                    if visited > self._MAX_SCAN_ENTRIES:
                        raise ValueError("Preset browser entry limit exceeded")
                    entries.append(entry)
            subdirectories = []
            for entry in sorted(entries, key=lambda item: item.name.casefold()):
                attributes = getattr(entry.stat(follow_symlinks=False), "st_file_attributes", 0)
                if entry.is_symlink() or attributes & getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400):
                    continue
                path = Path(entry.path)
                if entry.is_dir(follow_symlinks=False):
                    if depth >= self._MAX_SCAN_DEPTH:
                        raise ValueError("Preset browser directory depth exceeded")
                    subdirectories.append((path, depth + 1))
                elif entry.is_file(follow_symlinks=False) and path.suffix.lower() in (".se", ".json"):
                    yield path
            pending.extend(reversed(subdirectories))

    def update(self, *_args):
        if not hasattr(self, "list"): return super().update()
        self.list.clear()
        query = self.search.text().casefold()
        errors = []
        for name, directory in self._paths.items():
            if not directory.is_dir(): continue
            try:
                for path in self._presetPaths(directory):
                    relative = path.relative_to(directory).with_suffix("").as_posix()
                    if query not in relative.casefold(): continue
                    item = QtWidgets.QListWidgetItem(name + ": " + relative)
                    item.setData(_enum(QtCore.Qt, "ItemDataRole", "UserRole"), str(path))
                    self.list.addItem(item)
            except (OSError, ValueError) as error:
                errors.append(str(error))
        self.error.setText("\n".join(errors))

    def clear(self):
        self._paths.clear(); self._selected = ""; self.list.clear()

    def saveExpression(self):
        if self._selected and self.editor is not None:
            path = Path(self._selected)
            source = self.editor.getExpr()
            if path.suffix.lower() == ".json":
                value = json.loads(path.read_text(encoding="utf-8"))
                if not isinstance(value, dict): raise ValueError("Preset envelope must be an object")
                document = value.get("document", value)
                if not isinstance(document, dict): raise ValueError("Preset document must be an object")
                document["rgb" if "rgb" in document else "source"] = source
                source = json.dumps(value, ensure_ascii=False, indent=2)
            path.write_text(source, encoding="utf-8")
        else: self.saveExpressionAs()

    def saveExpressionAs(self):
        path, _filter = QtWidgets.QFileDialog.getSaveFileName(self, self.tr("Save Expression"), "", "SeExpr (*.se)")
        if path and self.editor is not None:
            Path(path).write_text(self.editor.getExpr(), encoding="utf-8")
            self._selected = path
            self.update()

    def saveLocalExpressionAs(self):
        self.saveExpressionAs()


class ExprShortTextEdit(QtWidgets.QTextEdit):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setAcceptRichText(False)


class ExprShortEdit(QtWidgets.QWidget):
    exprChanged = Signal()

    def __init__(self, parent=None):
        super().__init__(parent)
        layout = QtWidgets.QHBoxLayout(self)
        self.edit = ExprShortTextEdit(self)
        self.edit.setMaximumHeight(80)
        self.expandButton = QtWidgets.QToolButton(self)
        self.expandButton.setText(self.tr("Expand"))
        layout.addWidget(self.edit)
        layout.addWidget(self.expandButton)
        self._dialog = ExprDialog(self)
        self._details = None
        self.expandButton.clicked.connect(self.expandPressed)
        self.edit.textChanged.connect(self.handleTextEdited)
        self._dialog.expressionApplied.connect(self.expressionApplied)

    def getExpressionString(self): return self.edit.toPlainText()
    def setExpressionString(self, expression): self.edit.setPlainText(str(expression))
    def clearExtraCompleters(self): self._dialog.editor.clearExtraCompleters()
    def registerExtraFunction(self, name, docString): self._dialog.editor.registerExtraFunction(name, docString)
    def registerExtraVariable(self, name, docString): self._dialog.editor.registerExtraVariable(name, docString)
    def updateCompleter(self): self._dialog.editor.updateCompleter()
    def setSimple(self, enabled): self.expandButton.setVisible(not enabled)
    def setDetailsMenu(self, menu):
        self._details = menu
        self.expandButton.setMenu(menu)
        if menu:
            self.expandButton.setPopupMode(_enum(QtWidgets.QToolButton, "ToolButtonPopupMode", "MenuButtonPopup"))
    def setSearchPath(self, context, path):
        if not hasattr(self, "_browser"):
            self._browser = ExprBrowser(self._dialog, self._dialog.editor)
            self._dialog.layout().addWidget(self._browser)
        self._browser.setSearchPath(context, path)
    def setVerticalScrollBarPolicy(self, policy): self.edit.setVerticalScrollBarPolicy(policy)
    def hideErrors(self, hidden, err): self.edit.setToolTip("" if hidden else str(err))
    def showDetails(self, _index=0): self.expandPressed()
    def detailPressed(self):
        if self._details: self._details.popup(self.expandButton.mapToGlobal(QtCore.QPoint(0, self.expandButton.height())))
        else: self.expandPressed()
    def expandPressed(self):
        if not self._dialog.isVisible():
            self._dialog.setExpressionString(self.getExpressionString())
        self._dialog.show()
        self._dialog.raise_()
    def textFinished(self): self.exprChanged.emit()
    def handleTextEdited(self): self.exprChanged.emit()
    def controlChanged(self, _identifier): self.exprChanged.emit()
    def rebuildControls(self): self._dialog.editor.rebuildControls()
    def expressionApplied(self): self.setExpressionString(self._dialog.getExpressionString())
    def dialogClosed(self): self._dialog.hide()


__all__ = ["ExprEditor", "ExprDialog", "ExprControlCollection", "ExprBrowser",
           "ExprShortEdit", "ExprShortTextEdit", "ExprCurve", "ExprColorCurve",
           "ExprColorSwatchWidget", "ExprDeepWater"]
