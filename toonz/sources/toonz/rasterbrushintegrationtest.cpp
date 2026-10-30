#include "rasterbrushintegrationtest.h"
#include "../toonzlib/tests/rasterbrushfixture.h"
#include "tapp.h"
#include "sceneviewer.h"
#include "tools/tool.h"
#include "tools/toolhandle.h"
#include "toonz/tscenehandle.h"
#include "toonz/txshlevelhandle.h"
#include "toonz/tframehandle.h"
#include "toonz/tcolumnhandle.h"
#include "toonz/txsheethandle.h"
#include "toonz/tpalettehandle.h"
#include "toonz/toonzscene.h"
#include "toonz/txshsimplelevel.h"
#include "toonz/txshleveltypes.h"
#include "toonz/levelproperties.h"
#include "toonz/levelset.h"
#include "toonz/txsheet.h"
#include "toonz/txshcell.h"
#include "toonz/preferences.h"
#include "toonz/rasterbrushimport.h"
#include "toonz/toonzfolders.h"
#include "tpalette.h"
#include "tcolorstyles.h"
#include "tproperty.h"
#include "trasterimage.h"
#include "tundo.h"
#include "timage_io.h"
#include "tmsgcore.h"
#include <QApplication>
#include <QDialog>
#include <QTimer>
#include <QMainWindow>
#include <QCoreApplication>
#include <QEventLoop>
#include <QDir>
#include <QFile>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QTabletEvent>
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QPointingDevice>
#endif
#include <QPushButton>
#include <QMessageBox>
#include <QFileDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QInputDialog>
#include <QDialogButtonBox>
#include <QTemporaryDir>
#include <QLabel>
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}
void settle() {
  QCoreApplication::sendPostedEvents();
  QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 20);
}
bool same(const TRaster32P &a, const TRaster32P &b) {
  if (!a || !b || a->getSize() != b->getSize()) return false;
  for (int y = 0; y < a->getLy(); ++y)
    for (int x = 0; x < a->getLx(); ++x)
      if (a->pixels(y)[x] != b->pixels(y)[x]) return false;
  return true;
}
void requireSame(const TRaster32P &expected, const TRaster32P &actual,
                 const QString &context) {
  if (same(expected, actual)) return;
  std::cerr << "raster-brush-integration: pixel mismatch "
            << context.toStdString() << " expected "
            << (expected ? expected->getLx() : -1) << 'x'
            << (expected ? expected->getLy() : -1) << " actual "
            << (actual ? actual->getLx() : -1) << 'x'
            << (actual ? actual->getLy() : -1) << std::endl;
  if (expected && actual && expected->getSize() == actual->getSize()) {
    int differences = 0, maxChannel = 0;
    for (int y = 0; y < expected->getLy(); ++y)
      for (int x = 0; x < expected->getLx(); ++x) {
        const auto a = expected->pixels(y)[x], b = actual->pixels(y)[x];
        if (a == b) continue;
        maxChannel = std::max(
            {maxChannel, std::abs(int(a.r) - b.r), std::abs(int(a.g) - b.g),
             std::abs(int(a.b) - b.b), std::abs(int(a.m) - b.m)});
        if (++differences <= 8)
          std::cerr << "pixel " << x << ',' << y << " RGBA " << int(a.r) << ','
                    << int(a.g) << ',' << int(a.b) << ',' << int(a.m) << " -> "
                    << int(b.r) << ',' << int(b.g) << ',' << int(b.b) << ','
                    << int(b.m) << std::endl;
      }
    std::cerr << "different pixels " << differences << " max channel "
              << maxChannel << std::endl;
  }
  throw std::runtime_error(context.toStdString());
}
TRaster32P raster(TXshSimpleLevel *level, int frame) {
  TRasterImageP image(level->getFrame(TFrameId(frame), true));
  return image ? TRaster32P(image->getRaster()) : TRaster32P();
}
void checkpoint(const char *stage) {
  std::cerr << "raster-brush-integration: " << stage << std::endl;
}
void tap(TTool *tool) {
  require(tool->preLeftButtonDown(), "brush refused tap");
  TMouseEvent event;
  event.m_button   = Qt::LeftButton;
  event.m_buttons  = Qt::LeftButton;
  event.m_isTablet = true;
  event.m_pressure = 1;
  tool->leftButtonDown(TPointD(0, 0), event);
  event.m_buttons = Qt::NoButton;
  tool->leftButtonUp(TPointD(0, 0), event);
  settle();
}
void draw(TTool *tool, bool cancel, double pressure = .8, double tiltX = .2,
          double rotation = .4, bool uprightReturn = false) {
  require(tool->preLeftButtonDown(), "brush refused prepared raster");
  TMouseEvent event;
  event.m_button   = Qt::LeftButton;
  event.m_buttons  = Qt::LeftButton;
  event.m_isTablet = true;
  event.m_pressure = pressure;
  event.m_tilt     = TPointD(tiltX, -.3);
  event.m_hasTilt = event.m_hasRotation = true;
  event.m_rotation                      = rotation;
  if (uprightReturn) {
    event.m_tilt    = TPointD();
    event.m_hasTilt = false;
  }
  tool->leftButtonDown(TPointD(-30, 0), event);
  for (int i = 1; i <= 20; ++i) {
    if (uprightReturn) {
      event.m_tilt    = i < 10 ? TPointD(.8, 0) : TPointD();
      event.m_hasTilt = i < 10;
    }
    tool->leftButtonDrag(TPointD(-30 + 3 * i, 0), event);
  }
  if (cancel) {
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    require(tool->keyDown(&escape), "native stroke did not handle Escape");
  }
  event.m_buttons = Qt::NoButton;
  tool->leftButtonUp(TPointD(30, 0), event);
  settle();
}
}  // namespace

int runRasterBrushIntegrationTest(QMainWindow *window) {
  try {
    checkpoint("importing through the packaged application worker");
    RasterBrush::Record original;
    QString importError;
    QString package = QDir::current().filePath("original.gzbrush");
    require(RasterBrush::save(package, original, importError),
            "cannot write original native brush fixture");
    auto imported = RasterBrush::importBrushProcess(
        QCoreApplication::applicationFilePath(), package, importError);
    require(importError.isEmpty() && imported.complete &&
                imported.records.size() == 1,
            "packaged application brush import worker failed");
    checkpoint("creating disposable full-color level");
    auto app   = TApp::instance();
    auto scene = app->getCurrentScene()->getScene();
    require(scene && window, "native application scene is missing");
    Preferences::instance()->setValue(EnableAutocreation, true);
    auto level = scene->createNewLevel(OVL_XSHLEVEL, L"RasterBrushFixture")
                     ->getSimpleLevel();
    require(level, "cannot create raster level");
    level->setPath(TFilePath(QDir::current().filePath("brush..png")));
    level->getProperties()->setImageRes(TDimension(128, 128));
    level->getProperties()->setDpi(TPointD(72, 72));
    level->getProperties()->setImageDpi(TPointD(72, 72));
    auto palette = new TPalette;
    level->setPalette(palette);
    TRaster32P initial(128, 128);
    initial->fill(TPixel32(255, 255, 255, 255));
    TRasterImageP image(new TRasterImage(initial));
    image->setDpi(72, 72);
    level->setFrame(TFrameId(1), image);
    app->getCurrentLevel()->setLevel(level);
    app->getCurrentFrame()->setFid(TFrameId(1));
    app->getCurrentPalette()->setPalette(palette, 1);
    app->getCurrentTool()->setTool("T_Brush");
    settle();
    TTool *tool = TTool::getTool("T_Brush", TTool::RasterImage);
    require(tool, "full-color brush tool unavailable");
    SceneViewer *viewer = nullptr;
    for (auto candidate : window->findChildren<SceneViewer *>())
      if (candidate->isVisible()) {
        viewer = candidate;
        break;
      }
    require(viewer, "native scene viewer unavailable");
    tool->setViewer(viewer);
    tool->onActivate();
    auto property = dynamic_cast<TEnumProperty *>(
        tool->getProperties(0)->getProperty("Native Brush:"));
    require(property && property->isValue(L"<round>"),
            "native brush selector missing");
    property->setValue(L"<round>");
    require(tool->onPropertyChanged("Native Brush:"),
            "native brush selection failed");
    auto undo = TUndoManager::manager();
    undo->reset();
    auto verifyScenePersistence = [&](const TRaster32P &expected,
                                      const QString &label) {
      require(scene->getXsheet()->setCell(0, 0, TXshCell(level, TFrameId(1))),
              "cannot expose lifecycle fixture for scene save");
      level->save();
      const TFilePath scenePath(
          QDir::current().filePath("lifecycle-" + label + ".tnz"));
      scene->saveCopy(scenePath);
      ToonzScene restored;
      restored.loadTnzFile(scenePath);
      auto restoredLevel =
          restored.getLevelSet()->getLevel(L"RasterBrushFixture");
      require(restoredLevel && restoredLevel->getSimpleLevel(),
              "lifecycle scene lost its raster level");
      auto restoredRaster = restoredLevel->getSimpleLevel();
      restoredRaster->load();
      requireSame(expected, raster(restoredRaster, 1),
                  "scene/level lifecycle pixels: " + label);
      require(restored.getXsheet()->getCell(0, 0).m_frameId == TFrameId(1),
              "lifecycle scene lost painted frame exposure");
    };
    auto verifyMidStrokeFrameSwitch = [&]() {
      const auto baseline = raster(level, 1)->clone();
      TRasterImageP secondImage(new TRasterImage(baseline->clone()));
      secondImage->setDpi(72, 72);
      level->setFrame(TFrameId(2), secondImage);
      const auto selected = property->getValue();
      require(tool->preLeftButtonDown(),
              "brush refused midstroke lifecycle fixture");
      TMouseEvent event;
      event.m_button   = Qt::LeftButton;
      event.m_buttons  = Qt::LeftButton;
      event.m_isTablet = true;
      event.m_pressure = .8;
      event.m_tilt     = TPointD(.5, -.2);
      event.m_hasTilt  = true;
      tool->leftButtonDown(TPointD(-30, 0), event);
      for (int i = 1; i <= 10; ++i)
        tool->leftButtonDrag(TPointD(-30 + 3 * i, 0), event);
      property->setValue(L"<round>");
      require(tool->onPropertyChanged("Native Brush:") &&
                  property->getValue() == selected,
              "midstroke preset change replaced an active stroke's settings");
      app->getCurrentFrame()->setFid(TFrameId(2));
      settle();
      tool->onImageChanged();
      const auto committed = raster(level, 1)->clone();
      tool->leftButtonDrag(TPointD(30, 0), event);
      event.m_buttons = Qt::NoButton;
      tool->leftButtonUp(TPointD(30, 0), event);
      settle();
      require(!same(baseline, committed) && same(baseline, raster(level, 2)) &&
                  undo->getHistoryCount() == 1,
              "frame switch lost the original stroke or painted the new frame");
      require(undo->undo() && same(baseline, raster(level, 1)) &&
                  same(baseline, raster(level, 2)),
              "frame-switch undo changed the wrong frame");
      require(undo->redo() && same(committed, raster(level, 1)) &&
                  same(baseline, raster(level, 2)),
              "frame-switch redo did not restore the original target only");
      require(undo->undo(),
              "cannot restore lifecycle fixture after frame switch");
      app->getCurrentFrame()->setFid(TFrameId(1));
      settle();
      tool->onImageChanged();
      level->eraseFrame(TFrameId(2));
      undo->reset();
    };
    checkpoint("qualifying native editor scratchpad isolation and cancel");
    {
      const auto baseline = raster(level, 1)->clone();
      bool editorSeen = false, scratchQualified = false;
      QTimer::singleShot(0, [&]() {
        auto dialog =
            qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!dialog) return;
        editorSeen   = dialog->objectName() == "nativeBrushEditor";
        auto preview = dialog->findChild<QWidget *>("nativeBrushScratchpad");
        auto clear =
            dialog->findChild<QPushButton *>("nativeBrushClearScratchpad");
        auto demo = dialog->findChild<QPushButton *>("nativeBrushSampleStroke");
        if (preview && clear && demo) {
          clear->click();
          const auto blank = preview->grab().toImage();
          QMouseEvent press(QEvent::MouseButtonPress, QPointF(40, 70),
                            Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
          QApplication::sendEvent(preview, &press);
          QMouseEvent move(QEvent::MouseMove, QPointF(240, 70), Qt::NoButton,
                           Qt::LeftButton, Qt::NoModifier);
          QApplication::sendEvent(preview, &move);
          QMouseEvent release(QEvent::MouseButtonRelease, QPointF(240, 70),
                              Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
          QApplication::sendEvent(preview, &release);
          const auto painted = preview->grab().toImage();
          clear->click();
          const bool erased = preview->grab().toImage() == blank;
          demo->click();
          scratchQualified =
              painted != blank && erased && preview->grab().toImage() != blank;
        }
        dialog->reject();
      });
      property->setValue(L"<edit>");
      require(tool->onPropertyChanged("Native Brush:") && editorSeen &&
                  scratchQualified,
              "native editor scratchpad did not paint, clear and regenerate");
      require(same(baseline, raster(level, 1)) && undo->getHistoryCount() == 0,
              "cancelled native editor scratchpad changed document or undo");
    }
    checkpoint(
        "qualifying scratchpad tablet pressure and elapsed-time dynamics");
    {
      RasterBrush::Record fixture;
      fixture.preset.name = "Original scratchpad time and pressure fixture";
      fixture.preset.opacity.mappings.push_back(
          {RasterBrush::Sensor::Time, {{0, 0}, {.05, 0}, {.06, 1}, {1, 1}}, 1});
      fixture.preset.size.mappings.push_back(
          {RasterBrush::Sensor::Pressure, {{0, 0}, {1, 1}}, 1});
      const QString folder =
          ToonzFolder::getLibraryFolder().getQString() + "/rasterbrushes";
      const QString path = RasterBrush::install(folder, fixture, importError);
      require(importError.isEmpty() && !path.isEmpty(),
              "cannot install tablet scratchpad fixture");
      tool->onActivate();
      property->setValue(QDir(folder).relativeFilePath(path).toStdWString());
      require(tool->onPropertyChanged("Native Brush:"),
              "cannot select scratchpad time fixture");
      const auto baseline = raster(level, 1)->clone();
      bool qualified      = false;
      QTimer::singleShot(0, [&]() {
        auto dialog =
            qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!dialog) return;
        auto preview = dialog->findChild<QWidget *>("nativeBrushScratchpad");
        auto clear =
            dialog->findChild<QPushButton *>("nativeBrushClearScratchpad");
        if (preview && clear) {
          clear->click();
          const auto blank = preview->grab().toImage();
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
          QPointingDevice device("Original tablet fixture", 1,
                                 QInputDevice::DeviceType::Stylus,
                                 QPointingDevice::PointerType::Pen,
                                 QInputDevice::Capability::Position |
                                     QInputDevice::Capability::Pressure |
                                     QInputDevice::Capability::XTilt |
                                     QInputDevice::Capability::YTilt |
                                     QInputDevice::Capability::Rotation,
                                 1, 1);
#endif
          auto sendTablet = [&](QEvent::Type type, double x, double pressure,
                                int tilt) {
            const auto button =
                type == QEvent::TabletMove ? Qt::NoButton : Qt::LeftButton;
            const Qt::MouseButtons buttons =
                type == QEvent::TabletRelease ? Qt::NoButton : Qt::LeftButton;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
            QTabletEvent event(type, &device, QPointF(x, 70), QPointF(x, 70),
                               pressure, tilt, 0, 0, 0, 0, Qt::NoModifier,
                               button, buttons);
#else
            QTabletEvent event(type, QPointF(x, 70), QPointF(x, 70), QTabletEvent::RotationStylus,
                               QTabletEvent::Pen, pressure, tilt, 0, 0, 0, 0,
                               Qt::NoModifier, 1, button, buttons);
#endif
            QApplication::sendEvent(preview, &event);
          };
          sendTablet(QEvent::TabletPress, 40, 0, 0);
          QEventLoop elapsed;
          QTimer::singleShot(100, &elapsed, &QEventLoop::quit);
          elapsed.exec();
          sendTablet(QEvent::TabletMove, 140, 1, 20);
          sendTablet(QEvent::TabletMove, 240, 1, 0);
          sendTablet(QEvent::TabletRelease, 240, 0, 0);
          qualified = preview->grab().toImage() != blank;
        }
        dialog->reject();
      });
      property->setValue(L"<edit>");
      require(
          tool->onPropertyChanged("Native Brush:") && qualified,
          "tablet scratchpad replaced elapsed time with an event-count clock");
      require(same(baseline, raster(level, 1)) && undo->getHistoryCount() == 0,
              "tablet scratchpad changed document or undo");
    }
    checkpoint("qualifying invalid installed preset selection isolation");
    {
      const QString folder =
          ToonzFolder::getLibraryFolder().getQString() + "/rasterbrushes";
      const QString brokenPath =
          QDir(folder).filePath("original-invalid-installed.gzbrush");
      QFile broken(brokenPath);
      require(broken.open(QIODevice::WriteOnly) &&
                  broken.write("invalid JSON") == 12,
              "cannot stage invalid installed preset");
      broken.close();
      tool->onActivate();
      const auto previousSelection = property->getValue();
      const auto baseline          = raster(level, 1)->clone();
      bool warningSeen             = false;
      QTimer rejectWarning;
      QObject::connect(&rejectWarning, &QTimer::timeout, [&]() {
        auto dialog =
            qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (dialog && dialog->windowTitle() != "Import Raster Brush") {
          warningSeen = true;
          dialog->accept();
        }
      });
      rejectWarning.start(25);
      property->setValue(
          QDir(folder).relativeFilePath(brokenPath).toStdWString());
      const bool handled = tool->onPropertyChanged("Native Brush:");
      rejectWarning.stop();
      require(
          handled && warningSeen && property->getValue() == previousSelection &&
              same(baseline, raster(level, 1)) && undo->getHistoryCount() == 0,
          "invalid installed brush replaced active selection or changed "
          "document");
      require(QFile::remove(brokenPath),
              "cannot remove invalid installed fixture");
    }
    checkpoint("qualifying explicit source-to-native copy conversion");
    for (bool wetConversion : {false, true}) {
      RasterBrush::Record fixture;
      fixture.preset.name    = wetConversion ? "Original wet conversion fixture"
                                             : "Original pixel conversion fixture";
      fixture.preset.profile = wetConversion
                                   ? "krita-5.3.4.1-legacy-dulling-candidate-1"
                                   : "krita-5.3.4.1-hard-pixel-candidate-1";
      fixture.preset.wet =
          wetConversion ? RasterBrush::Wet::Dulling : RasterBrush::Wet::Off;
      fixture.preset.smudgeRadius = 0;
      // A forged classification must not hide the actual source runtime in
      // the editor; the explicit conversion action is still required.
      fixture.provenance["conversionKind"] = "native-reinterpretation";
      fixture.provenance["sourceProfileBeforeNativeConversion"] =
          QString::fromStdString(fixture.preset.profile);
      fixture.provenance["sourceSha256"] =
          QString(64, wetConversion ? 'b' : 'c');
      auto texture   = std::make_shared<RasterBrush::Image>();
      texture->width = texture->height = 1;
      texture->pixels                  = {{0, 0, 0, 1}};
      fixture.preset.texture.image     = texture;
      const auto assets = RasterBrush::resources(fixture, importError);
      require(importError.isEmpty() && assets.size() == 1,
              "source conversion resource fixture invalid");
      fixture.resourceRights[assets.front().hash] = QJsonObject{
          {"redistribution", "restricted"}, {"license", "Original fixture"}};
      const QString folder =
          ToonzFolder::getLibraryFolder().getQString() + "/rasterbrushes";
      const QString originalPath =
          RasterBrush::install(folder, fixture, importError);
      require(importError.isEmpty() && !originalPath.isEmpty(),
              "cannot install source conversion fixture");
      const auto originalBytes = RasterBrush::readBoundedRegularFile(
          originalPath, 64 * 1024 * 1024, importError);
      require(importError.isEmpty(),
              "cannot capture original conversion preset");
      tool->onActivate();
      property->setValue(
          QDir(folder).relativeFilePath(originalPath).toStdWString());
      require(tool->onPropertyChanged("Native Brush:"),
              "cannot select source conversion fixture");
      const auto originalSelection = property->getValue();
      const auto baseline          = raster(level, 1)->clone();
      for (int outcome = 0; outcome < 3; ++outcome) {
        bool requested = false, confirmed = false, statusCorrect = false,
             originalStatus = false;
        QTimer conversionDriver;
        QObject::connect(&conversionDriver, &QTimer::timeout, [&]() {
          auto modal = QApplication::activeModalWidget();
          if (modal && modal->objectName() == "nativeBrushEditor") {
            auto editor = qobject_cast<QDialog *>(modal);
            auto status =
                editor->findChild<QLabel *>("nativeBrushCompatibilityStatus");
            auto convert =
                editor->findChild<QPushButton *>("nativeBrushConvertCopy");
            if (!requested && convert) {
              requested      = true;
              originalStatus = status && status->text().contains(
                                             "Experimental source profile");
              QTimer::singleShot(0, [&]() {
                auto confirmation = qobject_cast<QMessageBox *>(
                    QApplication::activeModalWidget());
                if (!confirmation || confirmation->objectName() !=
                                         "nativeBrushConfirmNativeConversion")
                  return;
                confirmed = confirmation->defaultButton() ==
                                confirmation->button(QMessageBox::Cancel) &&
                            confirmation->text().contains(
                                "original remains unchanged") &&
                            (!wetConversion || confirmation->text().contains(
                                                   "native wet mixing"));
                if (outcome == 0)
                  confirmation->reject();
                else {
                  auto accept = confirmation->findChild<QPushButton *>(
                      "nativeBrushAcceptNativeConversion");
                  if (accept)
                    accept->click();
                  else
                    confirmation->reject();
                }
              });
              convert->click();
            } else if (requested) {
              statusCorrect =
                  status && status->text().contains(
                                outcome == 0 ? "Experimental source profile"
                                             : "Native reinterpretation");
              if (wetConversion && outcome != 0) {
                auto pickup =
                    editor->findChild<QDoubleSpinBox *>("nativeBrushPickup");
                auto colorRate =
                    editor->findChild<QDoubleSpinBox *>("nativeBrushColorRate");
                auto radius = editor->findChild<QDoubleSpinBox *>(
                    "nativeBrushSmudgeRadius");
                statusCorrect = statusCorrect && pickup && colorRate &&
                                radius && pickup->value() == .5 &&
                                colorRate->value() == .25 &&
                                radius->value() == 1;
              }
              if (outcome < 2)
                editor->reject();
              else {
                auto size = editor->findChild<QDoubleSpinBox *>(
                    "nativeBrushPresetSize");
                auto buttons = editor->findChild<QDialogButtonBox *>();
                if (size && buttons && buttons->button(QDialogButtonBox::Ok)) {
                  size->setValue(32.5);  // Deliberately outside strict integer
                                         // source-size eligibility.
                  buttons->button(QDialogButtonBox::Ok)->click();
                } else
                  editor->reject();
              }
            }
          } else if (auto name = qobject_cast<QInputDialog *>(modal))
            name->accept();
        });
        conversionDriver.start(25);
        property->setValue(L"<edit>");
        const bool handled = tool->onPropertyChanged("Native Brush:");
        conversionDriver.stop();
        require(handled && requested && confirmed && originalStatus &&
                    statusCorrect,
                "explicit conversion acknowledgment/status workflow failed");
        const auto unchanged = RasterBrush::readBoundedRegularFile(
            originalPath, 64 * 1024 * 1024, importError);
        require(importError.isEmpty() && unchanged == originalBytes &&
                    same(baseline, raster(level, 1)) &&
                    undo->getHistoryCount() == 0,
                "source conversion modified original preset or document");
        if (outcome < 2)
          require(property->getValue() == originalSelection,
                  "cancelled native conversion selected a copy");
        else {
          require(property->getValue() != originalSelection,
                  "accepted native conversion replaced original selection "
                  "identity");
          const QString copyPath = QDir(folder).filePath(
              QString::fromStdWString(property->getValue()));
          const auto copy = RasterBrush::importBrushProcess(
              QCoreApplication::applicationFilePath(), copyPath, importError);
          require(importError.isEmpty() && copy.complete &&
                      copy.records.size() == 1,
                  "native conversion copy failed reopening");
          const auto &converted = copy.records.front();
          require(
              converted.preset.profile == "groundzero-native-1" &&
                  converted.preset.size.value == 32.5 &&
                  converted.preset.texture.image &&
                  converted.resourceRights == fixture.resourceRights &&
                  converted.provenance["sourceProfileBeforeNativeConversion"]
                          .toString() ==
                      QString::fromStdString(fixture.preset.profile) &&
                  !converted.provenance["sourceReferenceVerified"].toBool() &&
                  (!wetConversion || (converted.preset.smudgeRadius == 1 &&
                                      converted.preset.pickup == .5 &&
                                      converted.preset.colorRate == .25)),
              "native conversion lost assets, declarations, edited values or "
              "reset semantics");
          const QString portable = QDir::current().filePath(
              wetConversion ? "converted-wet.gzbrush"
                            : "converted-pixel.gzbrush");
          require(RasterBrush::savePortable(
                      portable, converted,
                      RasterBrush::ResourcePackaging::PrivateCopy, importError),
                  "cannot export native conversion copy");
          const auto reopened = RasterBrush::importBrushProcess(
              QCoreApplication::applicationFilePath(), portable, importError);
          require(
              importError.isEmpty() && reopened.complete &&
                  reopened.records.size() == 1 &&
                  reopened.records.front().provenance["conversionKind"] ==
                      "native-reinterpretation" &&
                  reopened.records.front()
                          .provenance["sourceProfileBeforeNativeConversion"] ==
                      converted
                          .provenance["sourceProfileBeforeNativeConversion"] &&
                  reopened.records.front().resourceRights ==
                      fixture.resourceRights,
              "portable conversion reimport lost original-profile identity or "
              "resource rights");
        }
      }
    }
    checkpoint("qualifying explicit approximation acceptance and rejection");
    for (const QString &kind :
         {QString(), QString("tip-only"), QString("static-native"),
          QString("partial-native")}) {
      const QString sourcePath =
          QDir::current().filePath("original-ui-acceptance.gzbrush");
      QFile source(sourcePath);
      RasterBrush::Record importedFixture;
      importedFixture.preset.name =
          "Original diagnostic-free conversion fixture " + kind.toStdString();
      if (!kind.isEmpty()) importedFixture.provenance["conversionKind"] = kind;
      importedFixture.provenance["sourceSha256"] = QString(64, 'a');
      const auto bytes                           = RasterBrush::encodePortable(
          importedFixture, RasterBrush::ResourcePackaging::ManifestOnly,
          importError);
      require(importError.isEmpty() && importedFixture.diagnostics.empty(),
              "cannot encode diagnostic-free conversion fixture");
      require(source.open(QIODevice::WriteOnly) &&
                  source.write(bytes) == bytes.size(),
              "cannot stage approximation acceptance fixture");
      source.close();
      const auto baseline = raster(level, 1)->clone();
      const QDir library(ToonzFolder::getLibraryFolder().getQString() +
                         "/rasterbrushes");
      bool alreadyInstalled = false;
      for (bool acceptImport : {false, true, true}) {
        const auto beforeFiles =
            library.entryList(QDir::AllEntries | QDir::NoDotAndDotDot);
        const bool nativeDialogsDisabled =
            QApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
        bool acceptanceSeen = false, defaultCancel = false,
             labelsCorrect = false, pickerSubmitted = false;
        int attempts = 0;
        QTimer modalDriver;
        QObject::connect(&modalDriver, &QTimer::timeout, [&]() {
          auto modal = QApplication::activeModalWidget();
          if (auto picker = qobject_cast<QFileDialog *>(modal)) {
            if (!pickerSubmitted) {
              pickerSubmitted = true;
              picker->selectFile(sourcePath);
              QMetaObject::invokeMethod(picker, "accept", Qt::QueuedConnection);
            }
          } else if (modal && modal->objectName() ==
                                  "nativeBrushApproximationAcceptance") {
            auto report   = qobject_cast<QMessageBox *>(modal);
            defaultCancel = report && report->defaultButton() ==
                                          report->button(QMessageBox::Cancel);
            acceptanceSeen = true;
            if (report) {
              auto acceptLabel = report->findChild<QPushButton *>(
                  "nativeBrushAcceptApproximation");
              labelsCorrect =
                  kind.isEmpty() ||
                  (kind == "tip-only" &&
                   report->text().contains("Tip extraction only") &&
                   acceptLabel &&
                   acceptLabel->text() == "Accept extracted tips only") ||
                  (kind == "static-native" &&
                   report->text().contains("Static native conversion") &&
                   acceptLabel &&
                   acceptLabel->text() == "Accept static native conversions") ||
                  (kind == "partial-native" &&
                   report->text().contains("Partial native conversion") &&
                   acceptLabel &&
                   acceptLabel->text() == "Accept partial native conversions");
              if (acceptImport) {
                auto accept = report->findChild<QPushButton *>(
                    "nativeBrushAcceptApproximation");
                if (accept)
                  accept->click();
                else
                  report->reject();
              } else
                report->reject();
            }
          } else if (++attempts > 2400) {
            if (auto dialog = qobject_cast<QDialog *>(modal)) dialog->reject();
          }
        });
        modalDriver.start(25);
        property->setValue(L"<import>");
        const bool handled = tool->onPropertyChanged("Native Brush:");
        modalDriver.stop();
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs,
                                   nativeDialogsDisabled);
        require(handled && acceptanceSeen && defaultCancel && labelsCorrect,
                "foreign import lacked explicit approximation acceptance with "
                "Cancel default");
        const auto afterFiles =
            library.entryList(QDir::AllEntries | QDir::NoDotAndDotDot);
        require(
            (acceptImport && !alreadyInstalled ? afterFiles != beforeFiles
                                               : afterFiles == beforeFiles) &&
                same(baseline, raster(level, 1)) &&
                undo->getHistoryCount() == 0,
            "approximation acceptance did not control installation "
            "independently of document state");
        if (acceptImport) alreadyInstalled = true;
        if (acceptImport && !kind.isEmpty()) {
          bool statusCorrect = false;
          QTimer::singleShot(0, [&]() {
            auto editor =
                qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!editor) return;
            auto status =
                editor->findChild<QLabel *>("nativeBrushCompatibilityStatus");
            statusCorrect =
                status &&
                status->text().contains(
                    kind == "tip-only"         ? "Tip extraction only"
                    : kind == "partial-native" ? "Partial native conversion"
                                               : "Static native conversion");
            editor->reject();
          });
          property->setValue(L"<edit>");
          require(
              tool->onPropertyChanged("Native Brush:") && statusCorrect,
              "portable conversion classification was lost in editor status");
        }
      }
    }
    checkpoint("qualifying source initialization without paint or undo");
    {
      RasterBrush::Record fixture;
      fixture.preset.name         = "Original source initializer";
      fixture.preset.profile      = "krita-5.3.4.1-legacy-dulling-candidate-1";
      fixture.preset.wet          = RasterBrush::Wet::Dulling;
      fixture.preset.smudgeRadius = 0;
      const QString folder =
          ToonzFolder::getLibraryFolder().getQString() + "/rasterbrushes";
      QString path = RasterBrush::install(folder, fixture, importError);
      require(!path.isEmpty() && importError.isEmpty(),
              "cannot install source initializer");
      tool->onActivate();
      property->setValue(QDir(folder).relativeFilePath(path).toStdWString());
      require(tool->onPropertyChanged("Native Brush:"),
              "cannot select source initializer");
      TRaster32P baseline = raster(level, 1)->clone();
      tap(tool);
      require(same(baseline, raster(level, 1)) && undo->getHistoryCount() == 0,
              "source initialization painted or created an undo entry");
      app->getCurrentFrame()->setFid(TFrameId(4));
      settle();
      tap(tool);
      require(
          !level->getFrame(TFrameId(4), false) && undo->getHistoryCount() == 0,
          "unpainted source initialization retained a created frame");
      app->getCurrentFrame()->setFid(TFrameId(1));
      settle();
    }
    checkpoint("qualifying exact premultiplied PNG persistence");
    {
      // Exercise every valid 8-bit alpha/channel pair through real file I/O.
      // Two cycles also expose cumulative rounding drift.
      TRaster32P values(256, 256);
      for (int alpha = 0; alpha < 256; ++alpha)
        for (int channel = 0; channel < 256; ++channel) {
          const int c                    = std::min(channel, alpha);
          values->pixels(alpha)[channel] = TPixel32(c, alpha - c, c / 2, alpha);
        }
      const TRaster32P originalValues = values->clone();
      const TFilePath path(
          QDir::current().filePath("premultiplied-roundtrip.png"));
      for (int cycle = 0; cycle < 2; ++cycle) {
        TImageWriter::save(path, TRasterP(values));
        TRasterP decoded;
        require(TImageReader::load(path, decoded),
                "cannot read PNG channel fixture");
        values = TRaster32P(decoded);
        requireSame(originalValues, values,
                    "PNG premultiplied channel round trip");
      }
    }
    checkpoint("qualifying source wet stroke transactions and persistence");
    for (const QString &model :
         {"legacy-smear", "legacy-dulling", "new-smear", "new-dulling"})
      for (const QString &variant :
           {QString(), QString("wetPressure"), QString("radiusPressure"),
            QString("halfOpacity")}) {
        const QString label =
            model + "-" + (variant.isEmpty() ? QString("fixed") : variant);
        const QString sourcePath =
            QDir::current().filePath("original-source-" + label + ".kpp");
        QFile sourceFile(sourcePath);
        const QByteArray sourceBytes =
            RasterBrushTest::hardPixelKpp(variant, model);
        require(sourceFile.open(QIODevice::WriteOnly) &&
                    sourceFile.write(sourceBytes) == sourceBytes.size(),
                "cannot write original KPP transaction fixture");
        sourceFile.close();
        const auto sourceImport = RasterBrush::importBrushProcess(
            QCoreApplication::applicationFilePath(), sourcePath, importError);
        require(importError.isEmpty() && sourceImport.complete &&
                    sourceImport.records.size() == 1,
                "packaged application KPP import failed");
        auto fixture = sourceImport.records.front();
        require(
            fixture.preset.profile ==
                    "krita-5.3.4.1-" + model.toStdString() + "-candidate-1" &&
                !fixture.provenance["sourceReferenceVerified"].toBool(),
            "KPP worker lost strict candidate identity or qualification");
        if (variant == "halfOpacity")
          require(fixture.preset.opacity.value == .5,
                  "source static half opacity lost during worker import");
        if (variant == "radiusPressure")
          require(fixture.preset.smudgeRadius ==
                          (model.endsWith("dulling") ? 1 : 0) &&
                      fixture.preset.pickupControl.mappings.size() == 1 &&
                      fixture.preset.colorRateControl.mappings.size() == 1,
                  "combined source radius and pressure mappings were lost");
        fixture.preset.name =
            "Original source transaction " + label.toStdString();
        const QString folder =
            ToonzFolder::getLibraryFolder().getQString() + "/rasterbrushes";
        const QString path = RasterBrush::install(folder, fixture, importError);
        require(!path.isEmpty() && importError.isEmpty(),
                "cannot install source transaction fixture");
        tool->onActivate();
        property->setValue(QDir(folder).relativeFilePath(path).toStdWString());
        require(tool->onPropertyChanged("Native Brush:"),
                "cannot select source transaction fixture");
        const TRaster32P baseline = raster(level, 1)->clone();
        draw(tool, false);
        const TRaster32P painted = raster(level, 1)->clone();
        require(!same(baseline, painted) && undo->getHistoryCount() == 1,
                "source wet stroke did not create one painted transaction");
        require(undo->undo() && same(baseline, raster(level, 1)),
                "source wet undo did not restore original pixels");
        require(undo->redo() && same(painted, raster(level, 1)),
                "source wet redo did not restore exact pixels");
        const TFilePath output(
            QDir::current().filePath("source-" + label + ".png"));
        TImageWriter::save(output, TRasterP(painted));
        TRasterP reopened;
        require(TImageReader::load(output, reopened),
                "source wet PNG could not reopen");
        requireSame(
            painted, TRaster32P(reopened),
            "source wet save/reopen changed committed pixels: " + model);
        if (variant == "halfOpacity") verifyScenePersistence(painted, label);
        require(undo->undo() && same(baseline, raster(level, 1)),
                "source wet second undo failed");
        undo->reset();
        draw(tool, true);
        require(
            same(baseline, raster(level, 1)) && undo->getHistoryCount() == 0,
            "cancelled source wet stroke left pixels or undo history");
        draw(tool, false);
        require(
            same(painted, raster(level, 1)),
            "source wet state leaked across completed or cancelled strokes");
        require(undo->undo() && same(baseline, raster(level, 1)),
                "repeated source wet undo failed");
        undo->reset();
        if (variant == "halfOpacity") verifyMidStrokeFrameSwitch();
      }
    checkpoint("qualifying combined portable resources and native dynamics");
    for (bool wet : {false, true}) {
      RasterBrush::Record fixture;
      auto mask   = std::make_shared<RasterBrush::Image>();
      mask->width = mask->height = 4;
      for (int i = 0; i < 16; ++i)
        mask->pixels.push_back({0, 0, 0, (i + i / 4) % 2 ? .25 : 1});
      auto &p = fixture.preset;
      p.name =
          wet ? "Original combined native wet" : "Original combined native dry";
      p.tip.shape = RasterBrush::Shape::Bitmap;
      p.tip.images.push_back(mask);
      p.texture.image = mask;
      p.texture.scale = 2;
      p.texture.depth = .7;
      p.size.value    = 24;
      p.size.mappings.push_back(
          {RasterBrush::Sensor::Pressure, {{0, .2}, {1, 1}}, 1});
      p.aspect.value = .6;
      p.angle.value  = 90;
      p.angle.mappings.push_back(
          {RasterBrush::Sensor::Rotation, {{0, 0}, {1, 1}}, 1});
      p.opacity.value = .8;
      p.opacity.mappings.push_back(
          {RasterBrush::Sensor::TiltX, {{0, .3}, {1, 1}}, 1});
      p.flow.value          = .3;
      p.backgroundMix.value = .2;
      p.wash                = !wet;
      if (wet) {
        p.wet = RasterBrush::Wet::Dulling;
        p.pickupControl.mappings.push_back(
            {RasterBrush::Sensor::Pressure, {{0, .2}, {1, 1}}, 1});
      } else {
        p.secondary.enabled            = true;
        p.secondary.independentSpacing = true;
        p.secondary.spacing            = .6;
        p.secondary.tip.shape          = RasterBrush::Shape::Square;
        p.secondary.size.value         = .75;
        p.secondary.opacity.value      = .7;
        p.blend                        = RasterBrush::Blend::Multiply;
      }
      // Export an unknown-rights manifest, fail closed on missing resources,
      // then relink recipient-owned copies through the isolated decoder.
      const QString exported = QDir::current().filePath(
          wet ? "combined-wet.gzbrush" : "combined-dry.gzbrush");
      const QString ownCopy = QDir::current().filePath(
          wet ? "original-wet-copy.gzbrush" : "original-dry-copy.gzbrush");
      require(RasterBrush::save(ownCopy, fixture, importError),
              "cannot save original recipient resource copy");
      require(RasterBrush::savePortable(
                  exported, fixture,
                  RasterBrush::ResourcePackaging::ManifestOnly, importError),
              "combined manifest export failed");
      const auto manifest = RasterBrush::readBoundedRegularFile(
          exported, 64 * 1024 * 1024, importError);
      const auto info = RasterBrush::inspectPortable(manifest, importError);
      require(importError.isEmpty() && info.portable &&
                  info.resources.size() == 1 &&
                  !info.resources.front().embedded &&
                  info.resources.front().redistribution == "unknown",
              "unknown-rights manifest embedded or lost a shared resource");
      const TRaster32P beforeRepair = raster(level, 1)->clone();
      const auto incomplete         = RasterBrush::importBrushProcess(
          QCoreApplication::applicationFilePath(), exported, importError);
      require(!importError.isEmpty() && incomplete.records.empty() &&
                  !incomplete.complete && undo->getHistoryCount() == 0 &&
                  same(beforeRepair, raster(level, 1)),
              "missing-resource import changed the document or became usable");
      const auto copies = RasterBrush::importBrushProcess(
          QCoreApplication::applicationFilePath(), ownCopy, importError);
      require(importError.isEmpty() && copies.complete,
              "recipient copy import failed");
      QStringList matched;
      const auto repaired = RasterBrush::relinkPortable(
          manifest, copies.records, matched, importError);
      require(importError.isEmpty() && matched.size() == 1,
              "shared tip/texture hash did not relink exactly once");
      QFile repairedFile(exported);
      require(repairedFile.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
                  repairedFile.write(repaired) == repaired.size(),
              "cannot stage repaired integration fixture");
      repairedFile.close();
      const auto restored = RasterBrush::importBrushProcess(
          QCoreApplication::applicationFilePath(), exported, importError);
      require(importError.isEmpty() && restored.complete &&
                  restored.records.size() == 1,
              "complete portable resource repair failed worker validation");
      fixture = restored.records.front();
      require(fixture.preset.tip.images.size() == 1 &&
                  fixture.preset.texture.image &&
                  fixture.preset.tip.images.front() ==
                      fixture.preset.texture.image &&
                  fixture.preset.secondary.enabled == !wet &&
                  fixture.preset.secondary.independentSpacing == !wet &&
                  (wet || fixture.preset.secondary.spacing == .6),
              "combined preset lost relinked resources or settings");
      const QString folder =
          ToonzFolder::getLibraryFolder().getQString() + "/rasterbrushes";
      const QString installed =
          RasterBrush::install(folder, fixture, importError);
      require(!installed.isEmpty() && importError.isEmpty(),
              "combined preset install failed");
      tool->onActivate();
      property->setValue(
          QDir(folder).relativeFilePath(installed).toStdWString());
      require(tool->onPropertyChanged("Native Brush:"),
              "combined preset selection failed");
      if (!wet) {
        checkpoint("qualifying export defaults and resource repair through UI");
        QTemporaryDir uiFiles;
        require(uiFiles.isValid(), "cannot stage UI export fixture");
        const QString uiExport = uiFiles.filePath("manifest.gzbrush");
        const auto beforeUi    = raster(level, 1)->clone();
        const bool nativeDialogsDisabled =
            QApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
        bool exportDefault = false;
        int exportAttempts = 0;
        QTimer exportDriver;
        QObject::connect(&exportDriver, &QTimer::timeout, [&]() {
          auto modal = QApplication::activeModalWidget();
          if (auto picker = qobject_cast<QFileDialog *>(modal)) {
            if (!picker->property("fixtureSubmitted").toBool()) {
              picker->setProperty("fixtureSubmitted", true);
              picker->selectFile(uiExport);
              QMetaObject::invokeMethod(picker, "accept", Qt::QueuedConnection);
            }
          } else if (modal &&
                     modal->objectName() == "nativeBrushExportResources") {
            auto choice =
                modal->findChild<QComboBox *>("nativeBrushResourcePackaging");
            exportDefault =
                choice && choice->currentData().toInt() ==
                              int(RasterBrush::ResourcePackaging::ManifestOnly);
            qobject_cast<QDialog *>(modal)->accept();
          } else if (++exportAttempts > 2400) {
            if (auto dialog = qobject_cast<QDialog *>(modal)) dialog->reject();
          }
        });
        exportDriver.start(25);
        property->setValue(L"<export>");
        const bool exportedUi = tool->onPropertyChanged("Native Brush:");
        exportDriver.stop();
        const auto uiBytes = RasterBrush::readBoundedRegularFile(
            uiExport, 64 * 1024 * 1024, importError);
        const auto uiInfo = RasterBrush::inspectPortable(uiBytes, importError);
        require(
            exportedUi && exportDefault && importError.isEmpty() &&
                uiInfo.portable && !uiInfo.resources.empty() &&
                std::none_of(uiInfo.resources.begin(), uiInfo.resources.end(),
                             [](const RasterBrush::ResourceInfo &r) {
                               return r.embedded;
                             }),
            "unknown-rights export UI did not default to manifest-only");
        for (bool repairResources : {false, true}) {
          const QDir library(folder);
          const auto beforeFiles =
              library.entryList(QDir::AllEntries | QDir::NoDotAndDotDot);
          const auto previousSelection = property->getValue();
          bool relinkSeen = false, unexpectedAcceptance = false;
          int pickers = 0, attempts = 0;
          QTimer repairDriver;
          QObject::connect(&repairDriver, &QTimer::timeout, [&]() {
            auto modal = QApplication::activeModalWidget();
            if (auto picker = qobject_cast<QFileDialog *>(modal)) {
              if (!picker->property("fixtureSubmitted").toBool()) {
                picker->setProperty("fixtureSubmitted", true);
                picker->selectFile(pickers++ == 0 ? uiExport : ownCopy);
                QMetaObject::invokeMethod(picker, "accept",
                                          Qt::QueuedConnection);
              }
            } else if (modal &&
                       modal->objectName() == "nativeBrushRelinkResources") {
              relinkSeen  = true;
              auto repair = qobject_cast<QMessageBox *>(modal);
              auto choose = modal->findChild<QPushButton *>(
                  "nativeBrushChooseResourceCopies");
              if (repairResources && choose)
                choose->click();
              else if (repair)
                repair->reject();
            } else if (modal && modal->objectName() ==
                                    "nativeBrushApproximationAcceptance") {
              unexpectedAcceptance = true;
              qobject_cast<QDialog *>(modal)->reject();
            } else if (++attempts > 2400) {
              if (auto dialog = qobject_cast<QDialog *>(modal))
                dialog->reject();
            }
          });
          repairDriver.start(25);
          property->setValue(L"<import>");
          const bool importedUi = tool->onPropertyChanged("Native Brush:");
          repairDriver.stop();
          require(importedUi && relinkSeen && !unexpectedAcceptance &&
                      same(beforeUi, raster(level, 1)) &&
                      undo->getHistoryCount() == 0,
                  "native resource repair UI changed document or claimed "
                  "foreign conversion");
          if (!repairResources)
            require(property->getValue() == previousSelection &&
                        library.entryList(QDir::AllEntries |
                                          QDir::NoDotAndDotDot) == beforeFiles,
                    "cancelled unresolved-resource repair installed or "
                    "selected a preset");
          else {
            const auto selectedPath = QDir(folder).filePath(
                QString::fromStdWString(property->getValue()));
            const auto repairedPreset = RasterBrush::importBrushProcess(
                QCoreApplication::applicationFilePath(), selectedPath,
                importError);
            require(
                importError.isEmpty() && repairedPreset.complete &&
                    repairedPreset.records.size() == 1 &&
                    repairedPreset.records.front().preset.tip.images.size() ==
                        1 &&
                    repairedPreset.records.front().preset.texture.image,
                "resource repair UI did not install and select a complete "
                "portable brush");
          }
        }
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs,
                                   nativeDialogsDisabled);
      }
      const TRaster32P baseline = raster(level, 1)->clone();
      draw(tool, false, .8, .8, 1.5707963267948966);
      const TRaster32P high = raster(level, 1)->clone();
      require(!same(baseline, high) && undo->getHistoryCount() == 1 &&
                  undo->undo() && same(baseline, raster(level, 1)) &&
                  undo->redo() && same(high, raster(level, 1)),
              "combined preset paint/undo/redo transaction failed");
      const TFilePath output(QDir::current().filePath(
          wet ? "combined-wet.png" : "combined-dry.png"));
      TImageWriter::save(output, TRasterP(high));
      TRasterP reopened;
      require(TImageReader::load(output, reopened),
              "combined preset painted image did not reopen");
      requireSame(high, TRaster32P(reopened), "combined preset PNG round trip");
      verifyScenePersistence(high,
                             wet ? "combined-wet" : "combined-dual-texture");
      require(undo->undo() && same(baseline, raster(level, 1)),
              "combined preset restore failed");
      undo->reset();
      draw(tool, true, .8, .8, 1.5707963267948966);
      require(same(baseline, raster(level, 1)) && undo->getHistoryCount() == 0,
              "combined preset cancellation left pixels or history");
      draw(tool, false, .2, -.8, .4);
      require(!same(high, raster(level, 1)),
              "combined preset ignored changed tablet input");
      require(undo->undo() && same(baseline, raster(level, 1)),
              "combined preset low-input undo failed");
      undo->reset();
      verifyMidStrokeFrameSwitch();
      checkpoint("qualifying same-name preset copies and duplicate saves");
      const auto originalSelection = property->getValue();
      const QString originalPath =
          QDir(folder).filePath(QString::fromStdWString(originalSelection));
      const auto originalBytes = RasterBrush::readBoundedRegularFile(
          originalPath, 64 * 1024 * 1024, importError);
      require(importError.isEmpty(),
              "cannot capture original preset for copy workflow");
      std::wstring editedSelection;
      for (int saveIndex = 0; saveIndex < 2; ++saveIndex) {
        bool edited = false, named = false;
        QTimer editorDriver;
        QObject::connect(&editorDriver, &QTimer::timeout, [&]() {
          auto modal = QApplication::activeModalWidget();
          if (modal && modal->objectName() == "nativeBrushEditor") {
            auto opacity =
                modal->findChild<QDoubleSpinBox *>("nativeBrushPresetOpacity");
            auto buttons = modal->findChild<QDialogButtonBox *>();
            if (opacity && buttons && buttons->button(QDialogButtonBox::Ok)) {
              opacity->setValue(.333);
              edited = true;
              buttons->button(QDialogButtonBox::Ok)->click();
            } else
              qobject_cast<QDialog *>(modal)->reject();
          } else if (auto name = qobject_cast<QInputDialog *>(modal)) {
            named = name->textValue() ==
                    QString::fromStdString(fixture.preset.name);
            name->accept();
          }
        });
        editorDriver.start(25);
        property->setValue(L"<edit>");
        const bool handled = tool->onPropertyChanged("Native Brush:");
        editorDriver.stop();
        require(
            handled && edited && named &&
                property->getValue() != originalSelection,
            "same-name edit did not create and select a distinct preset copy");
        if (saveIndex == 0)
          editedSelection = property->getValue();
        else
          require(property->getValue() == editedSelection,
                  "identical edited preset created a duplicate copy");
        const auto newPath = QDir(folder).filePath(
            QString::fromStdWString(property->getValue()));
        const auto saved = RasterBrush::importBrushProcess(
            QCoreApplication::applicationFilePath(), newPath, importError);
        require(
            importError.isEmpty() && saved.complete &&
                saved.records.size() == 1 &&
                saved.records.front().preset.opacity.value == .333 &&
                saved.records.front().preset.name == fixture.preset.name,
            "same-name preset copy did not preserve edited settings on reopen");
        const auto unchanged = RasterBrush::readBoundedRegularFile(
            originalPath, 64 * 1024 * 1024, importError);
        require(importError.isEmpty() && unchanged == originalBytes &&
                    same(baseline, raster(level, 1)) &&
                    undo->getHistoryCount() == 0,
                "preset editing overwrote original resource package or changed "
                "document");
      }
      property->setValue(originalSelection);
      require(tool->onPropertyChanged("Native Brush:"),
              "cannot restore original preset after copy workflow");
    }
    checkpoint(
        "qualifying preset and toolbar alpha preservation through actual tool");
    {
      auto alphaModifier = dynamic_cast<TBoolProperty *>(
          tool->getProperties(0)->getProperty("Lock Alpha"));
      auto eraserModifier = dynamic_cast<TBoolProperty *>(
          tool->getProperties(0)->getProperty("ModifierEraser"));
      require(alphaModifier && eraserModifier,
              "native tool alpha/eraser modifiers missing");
      const bool previousEraser = eraserModifier->getValue();
      const auto previousColor  = palette->getStyle(1)->getMainColor();
      TRaster32P opaqueEraserResult;
      const bool previousModifier  = alphaModifier->getValue();
      const auto previousSelection = property->getValue();
      const auto originalPixels    = raster(level, 1)->clone();
      TRaster32P alphaRamp(originalPixels->getSize());
      for (int y = 0; y < alphaRamp->getLy(); ++y)
        for (int x = 0; x < alphaRamp->getLx(); ++x) {
          const int alpha         = x < 64 ? 0 : std::min(255, (x - 63) * 4);
          alphaRamp->pixels(y)[x] = TPixel32(alpha, alpha, alpha, alpha);
        }
      for (int mode = 0; mode < 5; ++mode) {
        const bool erase        = mode >= 3;
        const bool presetLock   = mode == 1 || erase,
                   modifierLock = mode == 2 || erase;
        palette->getStyle(1)->setMainColor(mode == 4 ? TPixel32(0, 0, 0, 0)
                                                     : previousColor);
        RasterBrush::Record fixture;
        fixture.preset.name =
            "Original alpha-preservation fixture " + std::to_string(mode);
        fixture.preset.lockAlpha  = presetLock;
        fixture.preset.size.value = 24;
        if (erase) fixture.preset.wet = RasterBrush::Wet::Dulling;
        const QString folder =
            ToonzFolder::getLibraryFolder().getQString() + "/rasterbrushes";
        const QString path = RasterBrush::install(folder, fixture, importError);
        require(importError.isEmpty() && !path.isEmpty(),
                "cannot install alpha-preservation fixture");
        tool->onActivate();
        property->setValue(QDir(folder).relativeFilePath(path).toStdWString());
        require(tool->onPropertyChanged("Native Brush:"),
                "cannot select alpha-preservation fixture");
        eraserModifier->setValue(erase);
        require(tool->onPropertyChanged("ModifierEraser"),
                "cannot set toolbar eraser modifier");
        alphaModifier->setValue(modifierLock);
        require(tool->onPropertyChanged("Lock Alpha"),
                "cannot set toolbar alpha-lock modifier");
        raster(level, 1)->copy(alphaRamp);
        tool->onImageChanged();
        undo->reset();
        draw(tool, false);
        const auto painted = raster(level, 1)->clone();
        bool alphaChanged = false, transparentPainted = false,
             alphaReduced = false, alphaIncreased = false;
        for (int y = 0; y < alphaRamp->getLy(); ++y)
          for (int x = 0; x < alphaRamp->getLx(); ++x) {
            const auto before = alphaRamp->pixels(y)[x];
            const auto after  = painted->pixels(y)[x];
            alphaChanged      = alphaChanged || before.m != after.m;
            alphaReduced      = alphaReduced || after.m < before.m;
            alphaIncreased    = alphaIncreased || after.m > before.m;
            transparentPainted =
                transparentPainted || (before.m == 0 && after.m != 0);
          }
        require(!same(alphaRamp, painted) && undo->getHistoryCount() == 1,
                "alpha-lock fixture did not paint existing color in one "
                "transaction");
        if (erase) {
          require(alphaReduced && !alphaIncreased && !transparentPainted,
                  "global eraser failed to override native wet mode and both "
                  "alpha locks");
          if (mode == 3)
            opaqueEraserResult = painted;
          else
            require(same(opaqueEraserResult, painted),
                    "native eraser coverage depended on foreground alpha");
        } else if (presetLock || modifierLock)
          require(!alphaChanged && !transparentPainted,
                  "saved or toolbar alpha lock failed to preserve the alpha "
                  "ramp and transparent region");
        else
          require(alphaChanged && transparentPainted,
                  "unlocked control did not paint the transparent region");
        require(undo->undo() && same(alphaRamp, raster(level, 1)) &&
                    undo->redo() && same(painted, raster(level, 1)),
                "alpha-preserving stroke undo/redo changed channels");
        verifyScenePersistence(painted, "alpha-lock-" + QString::number(mode));
        require(undo->undo() && same(alphaRamp, raster(level, 1)),
                "alpha-preserving stroke restore failed");
        undo->reset();
        draw(tool, true);
        require(
            same(alphaRamp, raster(level, 1)) && undo->getHistoryCount() == 0,
            "cancelled alpha-preserving stroke changed the frame");
      }
      raster(level, 1)->copy(originalPixels);
      tool->onImageChanged();
      palette->getStyle(1)->setMainColor(previousColor);
      eraserModifier->setValue(previousEraser);
      require(tool->onPropertyChanged("ModifierEraser"),
              "cannot restore toolbar eraser modifier");
      alphaModifier->setValue(previousModifier);
      require(tool->onPropertyChanged("Lock Alpha"),
              "cannot restore toolbar alpha modifier");
      property->setValue(previousSelection);
      require(tool->onPropertyChanged("Native Brush:"),
              "cannot restore original brush after alpha qualification");
      undo->reset();
    }
    checkpoint("rolling back actual-tool work-limit failure");
    {
      RasterBrush::Record fixture;
      auto &p                        = fixture.preset;
      p.name                         = "Original bounded-work rollback fixture";
      p.size.value                   = 4096;
      p.tip.antialiasEdges           = true;
      p.count                        = 64;
      p.secondary.enabled            = true;
      p.secondary.count              = 64;
      p.secondary.tip.antialiasEdges = true;
      // On this 128-square canvas, early synchronized stamps fit the work
      // budget and paint; later stamps in the same add exceed it. This checks
      // actual transaction rollback after partial deposition, not just an
      // engine's initial preflight rejection.
      const QString folder =
          ToonzFolder::getLibraryFolder().getQString() + "/rasterbrushes";
      const QString installed =
          RasterBrush::install(folder, fixture, importError);
      require(!installed.isEmpty() && importError.isEmpty(),
              "cannot install work-limit rollback fixture");
      tool->onActivate();
      property->setValue(
          QDir(folder).relativeFilePath(installed).toStdWString());
      require(tool->onPropertyChanged("Native Brush:"),
              "cannot select work-limit rollback fixture");
      const TRaster32P baseline = raster(level, 1)->clone();
      const int history         = undo->getHistoryCount();
      QStringList warnings;
      QObject capture;
      QObject::connect(TMsgCore::instance(), &TMsgCore::sendMessage, &capture,
                       [&](int type, const QString &message) {
                         if (type == DVGui::WARNING)
                           warnings.push_back(message);
                       });
      // The app may show warnings in its log panel or in a modal popup. Keep
      // the production error path intact and acknowledge a modal as a user
      // would, then assert the emitted reason rather than suppressing errors.
      QTimer dismiss;
      QObject::connect(&dismiss, &QTimer::timeout, &capture, [] {
        if (auto dialog =
                qobject_cast<QDialog *>(QApplication::activeModalWidget()))
          dialog->done(0);
      });
      dismiss.start(10);
      tap(tool);
      dismiss.stop();
      require(warnings.size() == 1 &&
                  warnings.front() == "Stroke input pixel work limit exceeded",
              "actual tool did not report the expected work-limit failure");
      requireSame(baseline, raster(level, 1),
                  "work-limit failure raster rollback");
      require(undo->getHistoryCount() == history,
              "work-limit failure published partial stroke undo history");
      // A subsequent valid stroke must not inherit a failed track/mask state.
      property->setValue(L"<round>");
      require(tool->onPropertyChanged("Native Brush:"),
              "cannot restore valid preset after work-limit failure");
      draw(tool, false);
      require(!same(baseline, raster(level, 1)) && undo->undo() &&
                  same(baseline, raster(level, 1)),
              "work-limit failure poisoned the next stroke transaction");
      undo->reset();
    }
    checkpoint("qualifying pressure, tilt and rotation through tool input");
    for (auto sensor :
         {RasterBrush::Sensor::Pressure, RasterBrush::Sensor::TiltX,
          RasterBrush::Sensor::Rotation}) {
      RasterBrush::Record fixture;
      fixture.preset.name          = "Original input fixture";
      fixture.preset.opacity.value = .7;
      fixture.preset.opacity.mappings.push_back({sensor, {{0, 0}, {1, 1}}, 1});
      const QString folder =
          ToonzFolder::getLibraryFolder().getQString() + "/rasterbrushes";
      QString path = RasterBrush::install(folder, fixture, importError);
      require(!path.isEmpty() && importError.isEmpty(),
              "cannot install original sensor fixture");
      tool->onActivate();
      property->setValue(QDir(folder).relativeFilePath(path).toStdWString());
      require(tool->onPropertyChanged("Native Brush:"),
              "cannot select installed sensor fixture");
      TRaster32P baseline = raster(level, 1)->clone();
      draw(tool, false, .2, -.8, 1.5707963267948966);
      TRaster32P low = raster(level, 1)->clone();
      require(undo->undo() && same(baseline, raster(level, 1)),
              "sensor stroke undo did not restore baseline");
      draw(tool, false, .8, .8, 3.141592653589793);
      TRaster32P high = raster(level, 1)->clone();
      require(!same(low, high), "tablet sensor did not affect actual pixels");
      require(undo->undo() && same(baseline, raster(level, 1)),
              "second sensor stroke undo did not restore baseline");
      if (sensor == RasterBrush::Sensor::TiltX) {
        draw(tool, false, .8, 0, 0, true);
        const auto upright = raster(level, 1)->clone();
        require(!same(baseline, upright) && undo->undo() &&
                    same(baseline, raster(level, 1)),
                "zero/nonzero/zero tablet tilt stroke or undo failed");
        draw(tool, true, .8, 0, 0, true);
        require(same(baseline, raster(level, 1)),
                "cancelled upright-return stroke left pixels");
        draw(tool, false, .8, 0, 0, true);
        require(same(upright, raster(level, 1)) && undo->undo(),
                "tablet tilt capability state leaked across stroke boundaries");
      }
      undo->reset();
    }
    property->setValue(L"<round>");
    require(tool->onPropertyChanged("Native Brush:"),
            "cannot restore native round brush");
    TRaster32P before = raster(level, 1)->clone();
    checkpoint("drawing through actual brush tool and undo manager");
    draw(tool, false);
    TRaster32P after = raster(level, 1)->clone();
    require(!same(before, after), "native tool did not paint");
    require(undo->getHistoryCount() == 1,
            "stroke was not one undo transaction");
    require(undo->undo(), "native undo unavailable");
    require(same(before, raster(level, 1)), "actual undo changed prior pixels");
    require(undo->redo(), "native redo unavailable");
    require(same(after, raster(level, 1)),
            "actual redo changed painted pixels");
    checkpoint("cancelling existing-frame stroke");
    int history = undo->getHistoryCount();
    draw(tool, true);
    require(same(after, raster(level, 1)), "cancel failed to restore raster");
    require(undo->getHistoryCount() == history, "cancel created an undo entry");
    checkpoint("creating and undoing a new frame");
    app->getCurrentFrame()->setFid(TFrameId(2));
    settle();
    draw(tool, false);
    require(bool(raster(level, 2)), "new frame was not created");
    TRaster32P second = raster(level, 2)->clone();
    require(undo->undo(), "new-frame undo unavailable");
    require(!level->getFrame(TFrameId(2), false),
            "undo did not remove created frame");
    require(undo->redo(), "new-frame redo unavailable");
    require(same(second, raster(level, 2)),
            "redo did not restore created frame");
    checkpoint("cancelling newly created frame");
    app->getCurrentFrame()->setFid(TFrameId(3));
    settle();
    draw(tool, true);
    require(!level->getFrame(TFrameId(3), false),
            "cancel left a newly created frame");
    checkpoint("saving and reopening native-painted image");
    TFilePath output(QDir::current().filePath("native-painted.png"));
    TImageWriter::save(output, TRasterP(second));
    TRasterP reopened;
    require(TImageReader::load(output, reopened),
            "cannot reopen saved native-painted image");
    require(same(second, TRaster32P(reopened)),
            "saved native-painted pixels changed");
    checkpoint("saving and reopening scene and raster level");
    require(scene->getXsheet()->setCell(0, 0, TXshCell(level, TFrameId(1))) &&
                scene->getXsheet()->setCell(1, 0, TXshCell(level, TFrameId(2))),
            "cannot expose painted level for scene persistence");
    level->save();
    TFilePath scenePath(QDir::current().filePath("native-painted.tnz"));
    scene->saveCopy(scenePath);
    {
      ToonzScene restored;
      restored.loadTnzFile(scenePath);
      auto restoredLevel =
          restored.getLevelSet()->getLevel(L"RasterBrushFixture");
      require(restoredLevel && restoredLevel->getSimpleLevel(),
              "scene lost painted raster level");
      auto restoredRaster = restoredLevel->getSimpleLevel();
      restoredRaster->load();
      require(same(after, raster(restoredRaster, 1)) &&
                  same(second, raster(restoredRaster, 2)),
              "scene/level reopening changed native-painted pixels");
      require(restored.getXsheet()->getCell(1, 0).m_frameId == TFrameId(2),
              "scene reopening lost painted frame exposure");
      require(!restoredRaster->getFrame(TFrameId(3), false),
              "scene reopening resurrected cancelled frame");
    }
    checkpoint("creating, undoing and redoing a new raster level");
    Preferences::instance()->setValue(DefLevelType, OVL_XSHLEVEL);
    app->getCurrentColumn()->setColumnIndex(1);
    app->getCurrentFrame()->setFrame(0);
    app->getCurrentLevel()->setLevel(nullptr);
    app->getCurrentXsheet()->notifyXsheetChanged();
    settle();
    const int levelCount = scene->getLevelSet()->getLevelCount();
    draw(tool, false);
    auto createdCell = scene->getXsheet()->getCell(0, 1);
    require(createdCell.getSimpleLevel(), "stroke did not create raster level");
    TRasterImageP createdImage(createdCell.getImage(true));
    require(bool(createdImage), "new level has no painted raster");
    TRaster32P createdPixels = createdImage->getRaster()->clone();
    require(scene->getLevelSet()->getLevelCount() == levelCount + 1,
            "new level was not added to the scene cast");
    require(undo->undo(), "new-level undo unavailable");
    require(scene->getXsheet()->getCell(0, 1).isEmpty() &&
                scene->getLevelSet()->getLevelCount() == levelCount,
            "new-level undo left scene cells or cast entries");
    require(undo->redo(), "new-level redo unavailable");
    auto restoredCell = scene->getXsheet()->getCell(0, 1);
    TRasterImageP restoredImage(restoredCell.getImage(true));
    require(restoredImage &&
                same(createdPixels, TRaster32P(restoredImage->getRaster())) &&
                scene->getLevelSet()->getLevelCount() == levelCount + 1,
            "new-level redo lost painted pixels or scene cast entry");
    checkpoint("cancelling a newly created raster level");
    app->getCurrentColumn()->setColumnIndex(2);
    app->getCurrentFrame()->setFrame(0);
    app->getCurrentLevel()->setLevel(nullptr);
    app->getCurrentXsheet()->notifyXsheetChanged();
    settle();
    history = undo->getHistoryCount();
    draw(tool, true);
    require(scene->getXsheet()->getCell(0, 2).isEmpty() &&
                scene->getLevelSet()->getLevelCount() == levelCount + 1 &&
                undo->getHistoryCount() == history,
            "cancelled new level left scene content or undo history");
    tool->onDeactivate();
    undo->reset();
    app->getCurrentScene()->setDirtyFlag(false);
    std::cout << "raster-brush-integration: PASS" << std::endl;
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "raster-brush-integration: FAIL: " << error.what()
              << std::endl;
  } catch (...) {
    std::cerr << "raster-brush-integration: FAIL: unexpected native exception"
              << std::endl;
  }
  return 1;
}
