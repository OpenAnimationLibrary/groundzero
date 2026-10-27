# Qt 6 port: status

This records the state of master-plan item D1. The code builds with Qt 5.15
(the default) and is being made to build with Qt 6 as well, selected by
`-DOT_QT_MAJOR_VERSION=6` (CMake sets `QT_VERSION_MAJOR` from it, and every
`CMakeLists.txt` names `Qt${QT_VERSION_MAJOR}::Foo`). The manual workflow
"Linux Qt 6 (experimental)" (`.github/workflows/workflow_linux_qt6.yml`)
configures with that option and runs `ninja -k 0`. It is expected to fail
until the port is done; its job summary and uploaded `build.log` give the
error counts.

**Status: the whole tree builds and links with Qt 6 on Linux** (Ubuntu 24.04,
Qt 6.4, the Qt 6 job of `.github/workflows/tests_linux.yml` on the `tier12`
branch). The Qt 5 build and its 25 tests are unchanged. The fixes came from a
static audit first, then from about ten CI rounds; the rounds are listed under
"Fixed from the CI build" below.

## How the code stays valid for both versions

- `toonzqt/qtcompat.h` holds the spellings that differ: `TEnterEvent`
  (the `enterEvent()` argument), `TNativeEventResult` (the result argument of
  `nativeEventFilter()`), `TTouchDeviceType`, `TTabletPointerType`,
  `isKnownTabletPointer()` and, new in this round, `menuOfAction()`.
- Anything else is a local `#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)`.
- Qt 6 modules: OpenGLWidgets (for `QOpenGLWidget`) and Core5Compat (for
  `QTextCodec`) are found and linked only when `QT_VERSION_MAJOR` is 6.

## Fixed in this round

| What | Where | Qt 5 behaviour |
|---|---|---|
| `QAction::menu()` returns a `QObject *` in Qt 6 (it is a template with that default). The four uses that need a `QMenu *` call `menuOfAction()`; the `dynamic_cast<DVMenuAction *>(act->menu())` uses are valid in both and left alone. | `qtcompat.h` (now includes `QAction` and `QMenu`), `toonz/menubar.cpp` | Same call as before. |
| `QVariant::canConvert(QMetaType::QString)` takes a `QMetaType` in Qt 6: `canConvert<QString>()`, as the earlier port did for the `QVariant::Type` uses. | `toonz/dvdirtreeview.cpp` (3) | Same. |
| `QtPlatformHeaders` and `QWindowsWindowFunctions` do not exist in Qt 6 (Windows full-screen border for OpenGL windows). The include and the three `setHasBorderInFullScreen()` calls are Qt 5 only. | `toonz/main.cpp`, `toonz/mainwindow.cpp`, `toonzqt/imageutils.cpp` | Same. |
| `QWindowsWindowFunctions::setWinTabEnabled()` is a feature of the customized Qt 5.15.2. `WITH_WINTAB` now defines `WITH_WINTAB` only for Qt 5, so a Qt 6 build never shows or uses the "Use Qt native Windows Ink" option. | `toonz/CMakeLists.txt`, `CMakeLists.txt` (option text) | Same. |
| The Windows Ink event filter builds Qt 5 `QTabletEvent`s with the old constructor and `long *` result, and Qt 6 handles the pointer messages itself. With Qt 6 `KisTabletSupportWin8` keeps its interface (the callers compile) but `isAvailable()`, `isPenDeviceAvailable()` and `init()` return false and the filter does nothing, so the Preferences option is hidden and the filter is never installed. The signature uses `TNativeEventResult *`. | `toonz/kis_tablet_support_win8.h/.cpp` | The Qt 5 code is untouched, only wrapped in `#else`. |
| `QTime::start()` / `elapsed()` are gone in Qt 6: `QElapsedTimer` in the code that is compiled out unless `TIPC_DEBUG` or `FPS_HISTOGRAM` is defined. | `common/tipc/tipc.cpp`, `t32bitsrv/t32movmsg.cpp`, `toonz/sceneviewer.cpp` | Same. |
| `QWheelEvent::delta()` is gone in Qt 6: `angleDelta().y()` (the same 120 per notch), in the code compiled only with `_SSDEBUG`. | `toonzlib/tcenterlineskeletonizer.cpp` | Same. |
| `QFlags == 0` comparison written with the enumerator. | `toonz/shortcutpopup.cpp` | Same. |

## Categories checked and clean

Each line was searched over all of `toonz/sources` (outside the 32-bit-only
pencil test, see below). "Clean" means every use is either already valid for
both versions, guarded by `QT_VERSION`, or only deprecated (still compiles in
Qt 6, listed further down). Repeat the search to confirm after new code is
merged.

| Category | Search | Result |
|---|---|---|
| Removed classes | `QRegExp`, `QRegExpValidator`, `QDesktopWidget`, `QGLWidget`, `QGLFormat`, `QGLContext`, `QGLPixelBuffer`, `QMatrix`, `QStringRef`, `QLinkedList`, `QDirModel`, `QSound`, `QCameraInfo` | Only in `QT_VERSION < 6` branches, comments, and the 32-bit pencil test. |
| `QTextCodec` | `QTextCodec` | Kept: Core5Compat is linked into every target that uses it (`image`, `tnztools`, `toonzlib`, `toonz`, and the type tool test). |
| Split behaviour | `SkipEmptyParts`, `KeepEmptyParts`, `QString::SplitBehavior` | 28 files, all `Qt::SkipEmptyParts` (valid from 5.14). |
| Layout margins | `setMargin(` | Only in the 32-bit pencil test. |
| `QFontMetrics::width` | `fm.width(`, `.width("` | Clean: `horizontalAdvance` (86 uses). |
| Variant types | `QVariant::Type`, `.type() ==`, `QVariant::Int` | Clean: `QMetaType::Type` is used for the preference types. |
| Removed algorithms | `qSort`, `qStableSort`, `qCopy`, `qrand`, `qsrand`, `qVariantValue`, `qFill`, `qBinaryFind` | None. |
| Containers | `insertMulti`, `uniqueKeys`, `unite`, `toSet`, `toStdVector`, `fromStdList`, `QList::swap` | None. `QHash` / `QSet` keys are all hashable. |
| Mouse events | `Qt::MidButton`, `QMouseEvent` constructors, zero flags | Clean: the constructors take `Qt::NoButton` / `Qt::MouseButtons()`. |
| Wheel events | `delta()`, `orientation()` | Clean: all handlers use `angleDelta()` / `position()`. |
| `enterEvent` | `void enterEvent(` | All overrides use `TEnterEvent *`. |
| Combo and button-group signals | `activated(QString)`, `currentIndexChanged(QString)`, `buttonClicked(int)`, `buttonPressed(int)`, `QSignalMapper` | Clean: `textActivated`, `idClicked`, `idPressed` or `QOverload<int>`. |
| `QProcess` | `start(string)`, `error()` signal, `finished` overloads | Clean: program and arguments, `errorOccurred`, `QOverload`. |
| Text streams | `setCodec`, `endl`, `flush` on `QTextStream` | Clean: `Qt::endl`. |
| Null strings | `QString::null` | None. |
| Style options | `QStyleOption::init`, `QStyleOption*V2/V3/V4` | Clean: `initFrom`. |
| Painter and palette | `HighQualityAntialiasing`, `QPalette::Background`, `setMatrix` | None. |
| High DPI attributes | `AA_EnableHighDpiScaling`, `AA_UseHighDpiPixmaps` | None. |
| Item views | `setFilterRegExp`, `reset()` of a model, `setResizeMode` of a header | None (the `setResizeMode` uses are on `QListView`, which still has it). |
| XML | `QXmlStreamReader::name() ==`, `attributes().value().toInt()` | Clean: `.toString()` before comparing; no `QStringView::toInt`. |
| Recursive mutexes, `QMutexLocker` | `QMutex::Recursive`, `QMutexLocker` as a type | Clean: `TThread::Mutex`; the locals use class template deduction. |
| Mixed `qsizetype` / `int` | `std::min`, `std::max`, `qMin`, `qMax`, `qBound` with `size()`, `count()`, `length()`, `indexOf()` | Clean: cast to `int`, or both sides are `std::` containers. |
| Key combinations | `Qt::CTRL + key`, `key() + modifiers()` | Valid in Qt 6 (an `int` sum). |
| Qt Multimedia | `QCamera*`, `QSound`, `QAbstractVideoSurface`, `QMediaPlayer` | Guarded; the 64-bit pencil test and the stop motion code have Qt 6 branches. |
| Screens | `QApplication::desktop()` | Guarded in `gutil.cpp` and `pickrgbutils.cpp`. |
| OpenGL context | `QGLContext` in `tgl.cpp`, `sceneviewer.cpp`, `main.cpp` | Guarded; Qt 6 uses `QOpenGLContext`. |
| Header moves | `QAction`, `QActionGroup`, `QShortcut`, `QOpenGLWidget`, `QOpenGLFunctions` includes | Valid for both (the module is linked); `QUndo*` classes are not used. |
| Native event filter | `long *` result | Only `KisTabletSupportWin8` was left, now `TNativeEventResult`. |
| Meta-type registration | `qRegisterMetaTypeStreamOperators` | None. |
| Windows-only | `QtWinExtras`, `QWinTaskbarButton`, `QtPlatformHeaders`, `QPlatformNativeInterface`, private headers | Only the `QtPlatformHeaders` uses fixed above. |

## Deprecated in Qt 6 but still compiling (warnings only)

These are not errors. They are listed so that a clean warning log is not
expected before they are changed.

- `QMouseEvent` / `QContextMenuEvent` `pos()`, `globalPos()`, `x()`, `y()`,
  `localPos()`, `windowPos()`, `screenPos()` (49 files). The replacement is
  `position()`, `globalPosition()`, `scenePosition()`, but the two sets
  return `QPointF`, so each use needs a look at the rounding.
- `QTabletEvent::posF()`, `QTouchEvent::touchPoints()` and
  `TouchPoint::pos()` (5 files; `touchPoints()` becomes `points()`).
- `QFontDatabase` used through an instance (`paramfield.cpp`,
  `dvtextedit.cpp`, `typetooltext.cpp`); the functions are static in Qt 6.
- `QMenu::addAction(text, receiver, SLOT(...))` in `symbolspane.cpp`.
- `QCheckBox::stateChanged(int)` (deprecated from Qt 6.9).
- `QDomDocument::setContent(QIODevice *)` (deprecated from Qt 6.5).
- `QSet<TXshLevelP>` in `levelsettingspopup.h` hashes the smart pointer
  through its `operator bool`, so every key lands in one of two buckets. It
  compiles in both versions and is correct, only slow for many levels; a
  `qHash` for the pointer would fix it.

## Not ported

- `toonz/penciltestpopup_qt.cpp/.h` and `cameracapturelevelcontrol_qt.cpp/.h`
  are built only when `PLATFORM` is not 64 (32-bit builds, see
  `toonz/CMakeLists.txt`) and still use `QRegExp`, `QRegExpValidator`,
  `QLayout::setMargin`, `QFontMetrics::width` and `QAbstractVideoSurface`.
  Qt 6 has no 32-bit Windows build to test them with, so they are left as they are.
- Windows: `QWindowsWindowFunctions::setHasBorderInFullScreen()` has no
  replacement here. The Qt 6 one is private API
  (`QNativeInterface::Private::QWindowsWindow`). Qt 6 may not need it (the
  flicker it works around concerns OpenGL windows in full screen); check a
  full-screen viewer and flipbook on Windows before adding it.
- Windows Ink: Qt 6 handles the pointer messages. Whether the pen behaves as
  well as with `KisTabletSupportWin8` (pressure, tilt, the eraser end,
  cursor proximity) has to be tested on a Windows machine with a tablet. The
  Qt 6 WinTab switch (`QWindowsWindowFunctions::setWinTabEnabled()` of the
  customized Qt 5) is not ported either.
- The pencil test in `penciltestpopup.cpp` has a Qt
  6 branch (`QMediaDevices`, `QCameraDevice`) that was written from the
  documentation only.

## Fixed from the CI build

Each round fixed every distinct error the Qt 6 job reported:

- tnzcore: `QStringList` forward declaration (a typedef in Qt 6), the
  `QMultiMap` iterators in `tthread.cpp`.
- toonzlib and stdfx: the audio scrub player (`QMediaDevices`,
  `QAudioDevice`, `QAudioSink`), the movie renderer's `QMutexLocker` (a
  template), `QJSEngine::throwError` with a string literal (now ambiguous),
  `QList` comparison needing a `const operator==`, `QAssociativeIterable`,
  `<climits>` for the `*_MAX` constants, `QChar` against `QLatin1Char`.
- toonzqt: `QActionGroup` includes (a QtGui class), `contentsMargins()`
  instead of `margin()`, `keySequenceOf()` in `qtcompat.h` (no `operator+`
  between a key and its modifiers), explicit `QFileInfo` and `QChar`
  conversions.
- toonz: float audio formats (`isFloatAudio`), `TThread::MutexLocker`, the
  Ctrl+Alt key sequence, `AdjustToMinimumContentsLengthWithIcon`,
  `QStandardPaths`, the tracker error string; and the unused
  `fileinfopopup.h`, whose missing constructor only Qt 6's moc refers to.

## What remains

- **The Qt 6 build has not been run.** Runtime behaviour is untested:
  string-based `connect()` calls whose signal changed (they compile, and fail
  with a console warning), high DPI scaling, tablet input, the stop motion
  camera, `QOpenGLWidget` context sharing and the offscreen render context.
- The unit tests run with Qt 5 only. Running them with Qt 6 is the next step.
- Windows and macOS have not been built with Qt 6; the Windows tablet and
  full-screen code is Qt 5 only (see "Not ported").
- Qt 5 stays the default. Make Qt 6 the default only after the tests pass
  with it and the application has been tried on each platform.
