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

animation_help = next(item['documentation'] for item in expr.native_functions()
                      if item['name'] == 'animCurve')
require(editor.help.findText('animCurve') >= 0 and
        'animCurve' in editor.edit._completer.model().stringList(),
        'native animation functions were omitted from editor help or source completion')
editor.help.setCurrentText('animCurve')
editor._show_help()
require(editor.status.text() == animation_help,
        'selecting a native function did not display its actual engine documentation')
editor.setExpr('ani')
cursor = editor.edit.textCursor()
cursor.setPosition(len(editor.getExpr()))
editor.edit.setTextCursor(cursor)
editor.edit._insert('animCurve')
require(editor.getExpr() == 'animCurve', 'native function completion did not replace the typed prefix')
editor.setExpr('animCurve($u,"constant","constant",0,"",'
               '0,2,1,1,0,0,"linear","linear",0,2,6,1,1,0,0,"linear","linear",0)')
require(editor.validateExpression() and editor.previewExpression() == 2.5,
        'the native function offered by editor completion did not evaluate with its host context')

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


def animation_key(time, value, identifier):
    return {'id': identifier, 'time': time, 'value': value, 'inWeight': 0.125,
            'outWeight': 0.25, 'inAngle': -12.123456789012345,
            'outAngle': 22.123456789012345, 'inTangent': 'fixed',
            'outTangent': 'spline', 'weightsLocked': False,
            'hostKey': {'retain': [1, 'fixture']}}


animation = ui.ExprAnimCurve()
animation_metadata = {'type': 'animcurve', 'name': 'fixtureAnimation',
                      'preInfinity': 'linear', 'postInfinity': 'cycleRelative',
                      'weighted': True, 'link': 'fixture_refresh',
                      'hostControl': {'version': 7, 'keep': True},
                      'keys': [animation_key(0, 2.1234567890123457, 'start'),
                               animation_key(2, -0.12345678901234568, 'finish')]}
animation.setParams(animation_metadata)
animation_changes = []
animation.curveChanged.connect(lambda: animation_changes.append(animation.parameters()))
require(animation.parameters() == animation_metadata and animation._samples and not animation.error.text(),
        'animation metadata, full numeric precision or native preview was lost on opening')
require(animation.apply() and not animation_changes and animation.parameters() == animation_metadata,
        'unchanged animation Apply rewrote metadata or notified the host')
animation.table.item(0, 1).setText('9.5')
animation.cancelButton.click()
require(animation.parameters() == animation_metadata and not animation_changes and
        float(animation.table.item(0, 1).text()) == animation_metadata['keys'][0]['value'],
        'animation Cancel did not restore the complete metadata draft')
animation.table.item(1, 1).setText('0.5')
animation.applyButton.click()
expected_animation = json.loads(json.dumps(animation_metadata))
expected_animation['keys'][1]['value'] = 0.5
require(animation.parameters() == expected_animation and len(animation_changes) == 1,
        'one animation edit rounded another field, discarded unknown metadata or lost its apply signal')

# Locking synchronizes the other handle only when a weight is edited.
locked = animation.table.item(0, 8)
checked = QtCore.Qt.CheckState.Checked if hasattr(QtCore.Qt, 'CheckState') else QtCore.Qt.Checked
locked.setCheckState(checked)
animation.table.item(0, 2).setText('0.625')
require(animation.table.item(0, 3).text() == '0.625',
        'animation weight locking did not synchronize the other handle')
require(animation.apply() and animation.parameters()['keys'][0]['inWeight'] == 0.625 and
        animation.parameters()['keys'][0]['outWeight'] == 0.625 and
        animation.parameters()['keys'][0]['inAngle'] == animation_metadata['keys'][0]['inAngle'],
        'locked animation weights changed unrelated tangent precision')

for tangent in ('fixed', 'auto', 'linear', 'flat', 'step', 'stepnext', 'spline'):
    animation.table.cellWidget(0, 6).setCurrentText(tangent)
    animation.table.cellWidget(0, 7).setCurrentText(tangent)
    require(animation.apply() and animation.parameters()['keys'][0]['inTangent'] == tangent and
            animation.parameters()['keys'][0]['outTangent'] == tangent and animation._samples,
            'an animation tangent mode could not be authored and evaluated: ' + tangent)
for before, after in zip(('constant', 'linear', 'cycle', 'cycleRelative', 'oscillate'),
                         ('oscillate', 'cycleRelative', 'cycle', 'linear', 'constant')):
    animation.preInfinity.setCurrentText(before)
    animation.postInfinity.setCurrentText(after)
    require(animation.apply() and animation.parameters()['preInfinity'] == before and
            animation.parameters()['postInfinity'] == after,
            'animation pre/post infinity modes could not be set independently')
for time, value in animation._samples[::16]:
    require(value == expr.evaluate(animation.expression('$u'), {'u': time}, 'scalar'),
            'animation graph samples differ from the actual native evaluator')

before_invalid = animation.parameters()
before_invalid_changes = len(animation_changes)
animation.table.item(1, 2).setText('-0.25')
require('nonnegative' in animation.error.text() and not animation.apply() and
        animation.table.item(1, 2).text() == '-0.25' and
        animation.parameters() == before_invalid and len(animation_changes) == before_invalid_changes,
        'invalid animation weights were silently clamped, discarded or applied')
animation.table.item(1, 2).setText('0.25')
animation.table.item(1, 0).setText('0')
require('distinct' in animation.error.text() and not animation.apply(),
        'duplicate animation key times were accepted')
animation.table.item(1, 0).setText('2')
animation.table.item(1, 1).setText('nan')
require('finite' in animation.error.text() and not animation.apply(),
        'nonfinite animation key values were accepted')
animation.table.item(1, 1).setText('0.75')
require(animation.apply() and not animation.error.text() and
        animation.parameters()['keys'][1]['value'] == 0.75,
        'a corrected animation draft did not recover without reopening the control')

# Row operations retain both the draft text and each key's host identity.
animation.table.setCurrentCell(1, 0)
animation._move(-1)
require('increasing' in animation.error.text() and animation.table.item(0, 0).text() == '2',
        'animation key reordering silently sorted or discarded the draft')
animation.sortButton.click()
require(not animation.error.text() and animation.apply() and
        [key['id'] for key in animation.keys()] == ['start', 'finish'],
        'sorting animation keys lost stable identifiers or metadata')
animation.table.setCurrentCell(0, 0)
animation.removeButton.click()
require(animation.apply() and animation.keys()[0]['id'] == 'finish' and
        expr.evaluate(animation.expression('100'), result_type='scalar') == 0.75,
        'removing an animation key lost the remaining identity or one-key constant behavior')
animation.table.setCurrentCell(0, 0)
animation.removeButton.click()
require(animation.apply() and not animation.keys() and
        expr.evaluate(animation.expression('100'), result_type='scalar') == 0,
        'an empty animation curve could not be authored and evaluated')
animation.addButton.click()
require(animation.apply() and len(animation.keys()) == 1,
        'adding a key to an empty animation curve failed')
animation.setParams({'type': 'animcurve', 'keys': [animation_key(i, i, 'key_' + str(i)) for i in range(64)]})
require(animation.table.rowCount() == 64 and not animation.addButton.isEnabled() and animation._samples,
        'the animation authoring limit did not include exactly 64 keys')
animation._add()
require(animation.table.rowCount() == 64 and '64' in animation.error.text(),
        'adding beyond the animation authoring limit had no explicit error')
oversized_animation = {'type': 'animcurve',
                       'keys': [animation_key(i, i, 'key_' + str(i)) for i in range(65)]}
animation.setParams(oversized_animation)
require('64' in animation.error.text() and not animation.apply() and
        animation.parameters() == oversized_animation,
        'oversized animation metadata was truncated or silently applied')
animation.table.setCurrentCell(64, 0)
animation.removeButton.click()
require(animation.apply() and len(animation.keys()) == 64,
        'an oversized animation draft could not be corrected by removing a key')

# Linked snapshots are supplied explicitly by the host and remain cancellable.
animation.setParams(animation_metadata)
require(not animation.refreshButton.isEnabled() and not animation.refreshLinkedCurve() and
        'has not provided' in animation.error.text() and animation.parameters() == animation_metadata,
        'a missing animation link resolver was not diagnosed without changing metadata')
refresh_calls = []


def refresh_animation(link, metadata):
    refresh_calls.append((link, metadata))
    return {'postInfinity': 'oscillate',
            'keys': [{'id': 'start', 'time': 0, 'value': 12.5}]}


animation.setLinkResolver(refresh_animation)
before_refresh_changes = len(animation_changes)
animation.refreshButton.click()
require(refresh_calls[-1][0] == 'fixture_refresh' and
        refresh_calls[-1][1] == animation_metadata and animation.parameters() == animation_metadata and
        animation.table.rowCount() == 1 and animation.table.item(0, 1).text() == '12.5' and
        animation.draftParameters()['keys'][0]['hostKey'] == animation_metadata['keys'][0]['hostKey'] and
        animation.draftParameters()['hostControl'] == animation_metadata['hostControl'] and
        len(animation_changes) == before_refresh_changes,
        'linked Refresh lost metadata, key identity or the unapplied draft boundary')
animation.cancelButton.click()
require(animation.parameters() == animation_metadata and animation.table.rowCount() == 2 and
        len(animation_changes) == before_refresh_changes,
        'canceling a successful linked Refresh changed applied animation metadata')
require(animation.refreshLinkedCurve() and animation.apply() and
        animation.keys()[0]['id'] == 'start' and animation.keys()[0]['value'] == 12.5 and
        animation.parameters()['hostControl'] == animation_metadata['hostControl'] and
        len(animation_changes) == before_refresh_changes + 1,
        'a linked animation snapshot did not apply once with retained metadata')


def reject_animation_refresh(link, metadata):
    metadata['keys'] = []
    raise ValueError('Fixture unavailable channel')


animation.setLinkResolver(reject_animation_refresh)
animation.table.item(0, 1).setText('9.25')
applied_before_refresh_failure = animation.parameters()
changes_before_refresh_failure = len(animation_changes)
require(not animation.refreshLinkedCurve() and animation.error.text() == 'Fixture unavailable channel' and
        animation.table.item(0, 1).text() == '9.25' and
        animation.parameters() == applied_before_refresh_failure and
        len(animation_changes) == changes_before_refresh_failure,
        'a failed or mutating animation resolver discarded visible text or escaped its metadata copy')
animation.setLinkResolver(lambda link, metadata: {'keys': [{'time': 0, 'value': 1, 'inWeight': -1}]})
require(not animation.refreshLinkedCurve() and 'nonnegative' in animation.error.text() and
        animation.table.item(0, 1).text() == '9.25' and animation.parameters() == applied_before_refresh_failure,
        'invalid refreshed animation metadata replaced the current visible draft')
animation.cancelButton.click()
animation.setLinkResolver(None)

animation_source = ('# é😀\n$gain=0.75;\n$wave=animCurve(\n'
                    ' ($u + $gain), # lookup context\n'
                    ' "cycle", # pre infinity comment\n'
                    ' "oscillate",2,"fixture_refresh",\n'
                    ' 0.0,2.1234567890123457,1e-12,0.75,-22.5,31.25,"fixed","spline",0,\n'
                    ' 2.0,4.0,0.5,0.75,45.0,0.0,"linear","stepnext",-3\n'
                    '); # surrounding comment\n$wave+0.25')
editor.setContext({'u': 0.25}, result_type='scalar')
editor.setExpr(animation_source)
animation_control = next(control for control in editor.collection.controls() if isinstance(control, ui.ExprAnimCurve))
applied_count = len(applied)
require(animation_control.apply() and editor.getExpr() == animation_source and len(applied) == applied_count,
        'unchanged inline animation Apply changed comments, numeric tokens or nonzero flag literals')
animation_control.table.item(1, 1).setText('99')
animation_control.cancelButton.click()
require(editor.getExpr() == animation_source and len(applied) == applied_count,
        'inline animation Cancel changed the host source')
animation_control.table.item(1, 1).setText('4.125')
animation_control.applyButton.click()
edited_animation_source = animation_source.replace('2.0,4.0,', '2.0,4.125,')
require(editor.getExpr() == edited_animation_source and len(applied) == applied_count + 1 and
        expr.evaluate(editor.getExpr(), {'u': 0.25}, 'scalar') ==
        expr.evaluate(edited_animation_source, {'u': 0.25}, 'scalar'),
        'editing one inline animation literal changed its lookup, comments, neighboring source or untouched precision')
animation_control = next(control for control in editor.collection.controls() if isinstance(control, ui.ExprAnimCurve))
animation_control.table.setCurrentCell(1, 0)
animation_control.removeButton.click()
animation_control.applyButton.click()
require('# lookup context' in editor.getExpr() and '# pre infinity comment' in editor.getExpr() and
        '# surrounding comment' in editor.getExpr() and '($u + $gain)' in editor.getExpr() and
        editor.validateExpression(), 'structural animation edits lost comments or the original lookup')

invalid_animation_source = 'animCurve($u,"constant","constant",0,"",0,1,1,1,0,0,"unknown","auto",0)'
editor.setExpr(invalid_animation_source)
animation_control = next(control for control in editor.collection.controls() if isinstance(control, ui.ExprAnimCurve))
require('inTangent' in animation_control.error.text() and not animation_control.apply() and
        editor.getExpr() == invalid_animation_source,
        'an unsupported inline tangent was hidden or its source was overwritten')
animation_control.table.cellWidget(0, 6).setCurrentText('fixed')
require(animation_control.apply() and editor.validateExpression() and '"unknown"' not in editor.getExpr(),
        'correcting an unsupported inline tangent did not retain an editable draft')
computed_animation = '1+animCurve($u,"constant","constant",0,"",0,1,1,1,0,0,"auto","auto",0)'
editor.setExpr(computed_animation)
require(any(isinstance(control, ui.ExprAnimCurve) for control in editor.collection.controls()),
        'an animation curve nested in a computed expression had no authoring control')
nonliteral_animation = 'animCurve($u,"constant","constant",0,"",$u,1,1,1,0,0,"auto","auto",0)'
editor.setExpr(nonliteral_animation)
require(any(isinstance(control, QtWidgets.QLabel) and 'must be literals' in control.text()
            for control in editor.collection.controls()) and editor.getExpr() == nonliteral_animation,
        'an unsupported computed key literal was silently skipped or replaced')


def refresh_inline_animation(link, metadata):
    require(link == 'fixture_refresh', 'inline animation refresh changed the link identifier')
    metadata['keys'][1]['value'] = 4.5
    return metadata


editor.setAnimationCurveLinkResolver(refresh_inline_animation)
editor.setExpr(animation_source)
animation_control = next(control for control in editor.collection.controls() if isinstance(control, ui.ExprAnimCurve))
before_inline_refresh = len(applied)
require(animation_control.refreshButton.isEnabled() and animation_control.refreshLinkedCurve() and
        editor.getExpr() == animation_source and len(applied) == before_inline_refresh,
        'inline linked Refresh changed source before Apply')
animation_control.cancelButton.click()
require(editor.getExpr() == animation_source and animation_control.refreshLinkedCurve() and animation_control.apply() and
        editor.getExpr() == animation_source.replace('2.0,4.0,', '2.0,4.5,') and
        len(applied) == before_inline_refresh + 1,
        'applying an inline linked snapshot changed its lookup, comments or untouched literal precision')
animation_control = next(control for control in editor.collection.controls() if isinstance(control, ui.ExprAnimCurve))
require(animation_control.refreshButton.isEnabled(), 'rebuilding inline controls discarded the host link resolver')
editor.setAnimationCurveLinkResolver(None)
require(not animation_control.refreshButton.isEnabled(), 'removing the host link resolver did not update existing controls')

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
    packaged_fixture = {'format': 'fixture', 'document': {'rgb': '$u', 'controls': [animation_metadata],
                                                       'hostDocument': {'retain': True}}}
    packaged.write_text(json.dumps(packaged_fixture), encoding='utf-8')
    browser.selectPath(packaged)
    editor.setExpr('$u+1')
    browser.saveExpression()
    packaged_fixture['document']['rgb'] = '$u+1'
    require(json.loads(packaged.read_text(encoding='utf-8')) == packaged_fixture,
            'JSON preset save corrupted the envelope, animation metadata or unknown document fields')

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
for widget in (short, water, swatches, colors, curve, animation, editor): widget.close()
app.processEvents()
print('SeExpr Python Qt SDK: PASS')
