"""Use the optional SDK widgets with their actual native evaluator in CI."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import SeExprPy as expr

os.environ.setdefault('QT_QPA_PLATFORM', 'offscreen')
import expreditor2 as ui
from SeExprPy.editor import QtCore, QtWidgets

# Other application jobs may select the shipping Qt's platform plugins.
# This SDK uses the Qt belonging to the selected Python binding.
info = QtCore.QLibraryInfo
plugins = info.path(info.LibraryPath.PluginsPath) if hasattr(info, 'path') else info.location(info.PluginsPath)
os.environ['QT_QPA_PLATFORM_PLUGIN_PATH'] = str(Path(plugins) / 'platforms')
os.environ['QT_PLUGIN_PATH'] = plugins
if os.name == 'nt':
    os.environ['QT_QPA_FONTDIR'] = str(Path(os.environ['WINDIR']) / 'Fonts')
app = QtWidgets.QApplication([])


def require(condition, message):
    if not condition:
        raise AssertionError(message)


modal_errors = []


def respond_to_color(accept, values=None):
    def respond():
        dialog = app.activeModalWidget()
        try:
            require(dialog is not None and dialog.objectName() == 'expressionColorDialog',
                    'the real numeric color dialog did not open')
            if values is not None:
                for name, value in zip(('R', 'G', 'B'), values):
                    field = dialog.findChild(QtWidgets.QLineEdit, 'component' + name)
                    require(field is not None, 'a numeric color component was missing')
                    field.setText(str(value))
            dialog.accept() if accept else dialog.reject()
        except Exception as error:
            modal_errors.append(str(error))
            if dialog is not None:
                dialog.reject()
    QtCore.QTimer.singleShot(0, respond)


editor = ui.ExprEditor()
applied = []
editor.applyExpression.connect(lambda: applied.append(editor.getExpr()))
editor.setContext({'u': 0.25}, result_type='scalar')
editor.setExpr('$amount=2; #0,10\n$amount+$u')
require(editor.validateExpression(), 'valid editor source did not validate')
require(editor.previewExpression() == 2.25, 'editor preview did not use native context')
controls = editor.collection.controls()
require(len(controls) == 1 and isinstance(controls[0], QtWidgets.QDoubleSpinBox),
        'annotated numeric control was not created')
controls[0].setValue(4)
require('$amount=4.0' in editor.getExpr() and len(applied) == 1,
        'native control did not update source and signal the host')
editor.setExpr('$amount=-2; #-4,4\n$amount+$u')
require(len(editor.collection.controls()) == 1 and editor.collection.controls()[0].value() == -2,
        'negative literal control was not retained')
editor.setExpr('1+')
editor._apply()
require(len(applied) == 1 and editor.status.text(), 'invalid editor draft was applied')
editor.registerExtraVariable('fixture_variable', 'Fixture variable documentation')
editor.updateCompleter()
require(editor.help.findText('fixture_variable') >= 0, 'extra completion was lost')

editor.setExpr('$local_gain=2*$u; $text="$hidden=3;"; # $comment=4\n$local_gain')
require(editor.help.findText('local_gain') >= 0 and editor.help.findText('text') >= 0 and
        editor.help.findText('hidden') == -1 and editor.help.findText('comment') == -1,
        'local completion missed computed assignments or parsed strings/comments as variables')
require('local_gain' in editor.edit._completer.model().stringList(),
        'parsed locals did not reach the source completion model')
editor.setExpr('$local_gain=2; $lo')
cursor = editor.edit.textCursor()
cursor.setPosition(len(editor.getExpr()))
editor.edit.setTextCursor(cursor)
editor.edit._insert('local_gain')
require(editor.getExpr() == '$local_gain=2; $local_gain',
        'completing a prefixed local duplicated its dollar sign')
editor.setExpr('$replacement=3; $replacement')
editor.clearExtraCompleters()
require(editor.help.findText('local_gain') == -1 and editor.help.findText('replacement') >= 0 and
        editor.help.findText('fixture_variable') == -1,
        'rebuilding completions retained old locals or erased current locals')
editor.setExpr('1+')
require(editor.help.findText('replacement') == -1,
        'an invalid draft retained stale local completions')

curve = ui.ExprCurve()
curve.setPoints([[0.0, 0.0, 0], [0.5, 0.8, 4], [1.0, 1.0, 1]])
require(curve.points()[1][2] == 4 and curve._samples, 'curve interpolation or preview was lost')
colors = ui.ExprColorCurve()
colors.setPoints([[0.0, [1, 0, 0], 1], [1.0, [0, 0, 1], 1]])
require(len(colors._samples) == 33 and len(colors._samples[0][1]) == 3,
        'color curve did not use vector evaluation')
swatches = ui.ExprColorSwatchWidget(True)
swatches.setColors([[1, 0, 0], [0, 1, 0], [0, 0, 1]])
swatches.list.setCurrentRow(2)
swatches._move(-1)
require(swatches.colors()[1] == [0, 0, 1], 'swatch reordering did not retain colors')
hdr = [2.1234567890123457, -0.25, 1e-12]
swatches.setColors([hdr])
color_changes = []
swatches.swatchChanged.connect(lambda: color_changes.append(swatches.colors()))
respond_to_color(True)
swatches._edit(swatches.list.item(0))
require(swatches.colors() == [hdr] and not color_changes,
        'opening and accepting a swatch clamped or quantized the original HDR values')
respond_to_color(False, [3.25, 0.5, 0.75])
swatches._edit(swatches.list.item(0))
require(swatches.colors() == [hdr] and not color_changes,
        'canceling a swatch editor changed source colors')
respond_to_color(True, [3.25, hdr[1], hdr[2]])
swatches._edit(swatches.list.item(0))
require(swatches.colors() == [[3.25, hdr[1], hdr[2]]] and len(color_changes) == 1,
        'a numeric color edit changed untouched components or lost its notification')

editor.setContext({}, result_type='vector')
hdr_source = '# é😀\n$tint=[2.1234567890123457,-0.25,1e-12];\n$tint'
editor.setExpr(hdr_source)
color_control = editor.collection.controls()[0]
color_button = color_control.findChild(QtWidgets.QPushButton)
require(color_button is not None, 'the vector color editor was not available')
respond_to_color(True)
color_button.click()
require(editor.getExpr() == hdr_source, 'unchanged color OK changed Unicode source or HDR precision')
respond_to_color(False, [0, 0, 0])
color_button.click()
require(editor.getExpr() == hdr_source, 'color Cancel changed the vector expression')
fields = color_control.findChildren(QtWidgets.QDoubleSpinBox)
fields[0].setValue(1.5)
require(expr.evaluate(editor.getExpr(), result_type='vector') == (1.5, hdr[1], hdr[2]),
        'changing one vector component rounded untouched components')
require(not modal_errors, '; '.join(modal_errors))

editor.setContext({'u': 0}, result_type='scalar')
curve_prefix = '# é😀\n$bias=0.25; $wave='
editor.setExpr(curve_prefix + 'curve($u,0,0,1,1,1,1); $wave+$bias')
curve_control = next(control for control in editor.collection.controls() if isinstance(control, ui.ExprCurve))
curve_control.table.item(0, 1).setText('0.5')
require(editor.getExpr().startswith(curve_prefix) and
        expr.evaluate(editor.getExpr(), {'u': 0}, 'scalar') == 0.75,
        'editing a later curve erased preceding source or changed its input expression')

try:
    curve.setPoints([[0, 1, float('inf')]])
except ValueError:
    pass
else:
    raise AssertionError('nonfinite curve interpolation was accepted')
water = ui.ExprDeepWater()
parameters = water.parameters()
parameters['amplitude'] = 0
water.setParams(parameters)
require(water.spectrumValue(water.parameters(), 0.5) == 0,
        'zero-amplitude spectrum did not remain finite')

with tempfile.TemporaryDirectory(prefix='seexpr-sdk-') as directory:
    preset = Path(directory) / 'fixture.se'
    preset.write_text('$u*2', encoding='utf-8')
    browser = ui.ExprBrowser(editor=editor)
    browser.addPath('Fixtures', directory)
    require(browser.list.count() == 1, 'preset browser did not discover source')
    browser.selectPath(preset)
    require(editor.getExpr() == '$u*2', 'preset source did not load')
    editor.setExpr('$u*3')
    browser.saveExpression()
    require(preset.read_text(encoding='utf-8') == '$u*3', 'preset source did not save')
    packaged = Path(directory) / 'fixture.json'
    packaged.write_text(json.dumps({'document': {'rgb': '$u', 'controls': []}}), encoding='utf-8')
    browser.selectPath(packaged)
    editor.setExpr('$u+1')
    browser.saveExpression()
    require(json.loads(packaged.read_text(encoding='utf-8'))['document']['rgb'] == '$u+1',
            'JSON preset save corrupted the preset envelope')

    nested = Path(directory) / 'nested' / 'levels' / 'fixture.se'
    nested.parent.mkdir(parents=True)
    nested.write_text('$u*5', encoding='utf-8')
    if os.name == 'nt':
        # Directory junctions do not need elevation and were not recognized by
        # Path.is_symlink on Python versions supported before isjunction.
        subprocess.run(['cmd.exe', '/d', '/c', 'mklink', '/J', 'cycle', directory],
                       cwd=nested.parent, check=True, capture_output=True, text=True)
    else:
        (nested.parent / 'cycle').symlink_to(directory, target_is_directory=True)
    browser.update()
    require(browser.list.count() == 3 and not browser.error.text(),
            'nested presets were omitted, duplicated through links or failed to scan')
    browser.search.setText('levels/fixture')
    require(browser.list.count() == 1 and 'nested/levels/fixture' in browser.list.item(0).text(),
            'nested preset labels or folder-aware filtering were lost')
    browser.selectPath(nested)
    require(editor.getExpr() == '$u*5', 'nested preset did not load its source')
    editor.setExpr('$u*6')
    browser.saveExpression()
    require(nested.read_text(encoding='utf-8') == '$u*6' and
            preset.read_text(encoding='utf-8') == '$u*3',
            'saving a nested preset overwrote another preset with the same name')
    scan_limit = browser._MAX_SCAN_ENTRIES
    browser._MAX_SCAN_ENTRIES = 2
    browser.update()
    require('limit' in browser.error.text(), 'oversized preset traversal was not diagnosed')
    browser._MAX_SCAN_ENTRIES = scan_limit
    browser.search.clear()
    browser.update()
    require(browser.list.count() == 3 and not browser.error.text(),
            'preset scanning did not recover after the traversal limit')

short = ui.ExprShortEdit()
short.setExpressionString('2+3')
short.expandPressed()
require(short._dialog.getExpressionString() == '2+3', 'expanded source was not retained')
short._dialog.editor.setContext({}, result_type='scalar')
short._dialog.setExpressionString('4+5')
short._dialog.applyExpression()
require(short.getExpressionString() == '4+5', 'dialog apply did not update short editor')
short._dialog.close()
for widget in (short, water, swatches, colors, curve, editor): widget.close()
app.processEvents()
print('SeExpr Python Qt SDK: PASS')
