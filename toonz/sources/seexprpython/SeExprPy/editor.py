"""Optional Python Qt authoring API backed by the native SeExpr evaluator.

This independently implemented SDK frontend follows the upstream expreditor2
widget interfaces. A host owns its document, animation and undo transaction;
apply signals carry the same boundary as the C++ authoring API.
"""
import json
import math
import os
from pathlib import Path
import re

from . import AST, ASTType, Edits, evaluate, functions, getComment, validate

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

    def controls(self):
        return list(self._controls)

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
        assignments, strings = [], []
        def walk(node, assigned_literal=False):
            children = node.children()
            if node.type == ASTType.Assign and children: assignments.append(node)
            if node.type == ASTType.String and not assigned_literal:
                annotation = getComment(node, source).split()
                if len(annotation) == 2: strings.append((annotation[1], node))
            for index, child in enumerate(children):
                walk(child, node.type == ASTType.Assign and index == 0)
        if root: walk(root)
        records = [(assignment.value, assignment.children()[0]) for assignment in assignments] + strings
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
                if node.value in ("curve", "ccurve") and len(children) >= 4 and (len(children) - 1) % 3 == 0:
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
        self._completer.setModel(QtCore.QStringListModel(sorted(names), self._completer))

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

    def exprChanged(self):
        self.clearErrors()
        self._timer.start()

    def rebuildControls(self):
        source = self.getExpr()
        self.collection.rebuild(source, lambda interval, replacement:
                                self._control_change(source, interval, replacement))

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
        self._documentation.update({name: self.tr("Host variable") for name in self._variables})
        self._documentation.update({name: definition.get("documentation", self.tr("Host function"))
                                    for name, definition in self._functions.items()})
        self._documentation.update(self._extra_functions)
        self._documentation.update(self._extra_variables)
        self.help.clear()
        self.help.addItems(sorted(self._documentation))
        completer = QtWidgets.QCompleter(sorted(self._documentation), self)
        completer.setCaseSensitivity(_enum(QtCore.Qt, "CaseSensitivity", "CaseInsensitive"))
        self.help.setCompleter(completer)
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

    def update(self, *_args):
        if not hasattr(self, "list"): return super().update()
        self.list.clear()
        query = self.search.text().casefold()
        for name, directory in self._paths.items():
            if not directory.is_dir(): continue
            for path in sorted(directory.iterdir()):
                if path.is_file() and path.suffix.lower() in (".se", ".json") and query in path.stem.casefold():
                    item = QtWidgets.QListWidgetItem(name + ": " + path.stem)
                    item.setData(_enum(QtCore.Qt, "ItemDataRole", "UserRole"), str(path))
                    self.list.addItem(item)

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
