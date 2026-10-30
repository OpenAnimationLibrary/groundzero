#include "fillworkflowintegrationtest.h"
#include "vectormodaltest.h"
#include "tapp.h"
#include "filmstrip.h"
#include "filmstripcommand.h"
#include "filmstripselection.h"
#include "iocommand.h"
#include "nativeuiintegrationtest.h"
#include "tools/tool.h"
#include "tools/toolhandle.h"
#include "tools/strokeselection.h"
#include "toonzqt/selectioncommandids.h"
#include "toonzqt/menubarcommand.h"
#include "toonzqt/planeviewer.h"
#include "toonzqt/tselectionhandle.h"
#include "toonz/toonzscene.h"
#include "toonz/tscenehandle.h"
#include "toonz/txshsimplelevel.h"
#include "toonz/txshleveltypes.h"
#include "toonz/txshlevelhandle.h"
#include "toonz/levelproperties.h"
#include "toonz/morphhints.h"
#include "toonz/txsheet.h"
#include "toonz/txshcell.h"
#include "toonz/txsheethandle.h"
#include "toonz/tcolumnhandle.h"
#include "toonz/tframehandle.h"
#include "tvectorimage.h"
#include "tregion.h"
#include "tstroke.h"
#include "tpalette.h"
#include "tcolorstyles.h"
#include "tproperty.h"
#include "tundo.h"
#include "tlevel_io.h"
#include "tlevel.h"
#include <QApplication>
#include <QDialog>
#include <QImage>
#include <QComboBox>
#include <QKeyEvent>
#include <QPushButton>
#include <QTimer>
#include <QTableWidget>
#include <QLabel>
#include <QTableView>
#include <QSlider>
#include <QTemporaryDir>
#include <QAction>
#include <QAbstractButton>
#include <QEvent>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPointer>
#include <QScrollArea>
#include <QResizeEvent>
#include <algorithm>
#include <stdexcept>
#include <exception>
#include <cmath>
#include <memory>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

namespace {
void require(bool ok, const char *message) {
  if (!ok) throw std::runtime_error(message);
}

void setBool(TTool *tool, const char *name, bool value) {
  auto *property = dynamic_cast<TBoolProperty *>(tool->getProperties(0)->getProperty(std::string(name)));
  require(property, "fill workflow tool property missing");
  property->setValue(value);
  tool->onPropertyChanged(name);
}
std::string geometryDifference(const TVectorImageP &a, const TVectorImageP &b) {
  std::ostringstream difference;
  difference << std::setprecision(17);
  if (a->getStrokeCount() != b->getStrokeCount()) {
    difference << "stroke count: actual " << a->getStrokeCount()
               << ", expected " << b->getStrokeCount();
    return difference.str();
  }
  for (int i = 0; i < int(a->getStrokeCount()); ++i) {
    auto *x = a->getStroke(i), *y = b->getStroke(i);
    if (x->getControlPointCount() != y->getControlPointCount()) {
      difference << "stroke " << i << " control-point count: actual "
                 << x->getControlPointCount() << ", expected "
                 << y->getControlPointCount();
      return difference.str();
    }
    for (int j = 0; j < x->getControlPointCount(); ++j) {
      const auto actual = x->getControlPoint(j), expected = y->getControlPoint(j);
      if (actual != expected) {
        difference << "stroke " << i << " control point " << j << ": actual ("
                   << actual.x << ", " << actual.y << ", " << actual.thick
                   << "), expected (" << expected.x << ", " << expected.y
                   << ", " << expected.thick << ')';
        return difference.str();
      }
    }
  }
  return {};
}
bool sameGeometry(const TVectorImageP &a, const TVectorImageP &b) {
  return geometryDifference(a, b).empty();
}
void requireGeometry(const TVectorImageP &actual, const TVectorImageP &expected,
                     const char *phase) {
  const auto difference = geometryDifference(actual, expected);
  if (!difference.empty()) throw std::runtime_error(std::string(phase) + ": " + difference);
}
void requireFilledCenter(const TVectorImageP &image, const char *phase) {
  const auto *region = image->getRegion(TPointD(0,0));
  if (!region || region->getStyle() != 1) {
    std::ostringstream difference;
    difference << phase << ": center fill style: actual ";
    if (region) difference << region->getStyle();
    else difference << "no region";
    difference << ", expected 1 (region count " << image->getRegionCount() << ')';
    throw std::runtime_error(difference.str());
  }
}
void fillCheckpoint(const char *phase) {
  std::cout << "vector-fill-native: " << phase << std::endl;
}

// The caller's live Apply/Undo oracle stays exact. A native PLI reopen also
// performs legacy region normalization: the writer stores XY at scale 16384
// (absolute first point, rounded deltas thereafter), then the reader's
// findRegions truncates new, non-point strokes to the 1/128 region grid.
// Widths use rounded bytes relative to ceil(maximum width). Predict those
// documented representations without invoking the production reader/region
// code on the expected image. Opacity/profile/style fields remain untouched.
TVectorImageP expectedMorphPliDrawing(const TVectorImageP &source) {
  TVectorImageP expected = source->clone();
  constexpr double storedScale = 16384., regionScale = 128.;
  for (UINT index = 0; index < source->getStrokeCount(); ++index) {
    const auto *stroke = source->getStroke(index);
    auto *decoded = expected->getStroke(index);
    const int count = stroke->getControlPointCount();
    require(count >= 3 && count % 2 == 1, "morph PLI oracle requires quadratic controls");
    double peak = 0.;
    for (int i = 0; i < count; ++i) {
      const auto p = stroke->getControlPoint(i);
      require(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.thick) &&
                  std::abs(p.x * storedScale) < 2147483647. &&
                  std::abs(p.y * storedScale) < 2147483647. && p.thick >= 0.,
              "morph PLI oracle received an unrepresentable fixture control");
      peak = std::max(peak, p.thick);
    }
    require(peak > 0. && peak <= 255., "morph PLI oracle width range changed");
    const double widthUnit = std::ceil(peak) / 255.;
    const auto bounds = stroke->getBBox();
    const double pointThreshold = 2.1 * stroke->getThickPoint(0).thick;
    require(bounds.getLx() > pointThreshold || bounds.getLy() > pointThreshold,
            "morph PLI fixture unexpectedly uses the point-stroke region exception");
    TPointD previousStored;
    for (int i = 0; i < count; ++i) {
      const auto p = stroke->getControlPoint(i);
      TPointD stored;
      if (!i) stored = TPointD(std::trunc(p.x * storedScale) / storedScale,
                               std::trunc(p.y * storedScale) / storedScale);
      else {
        const auto prior = stroke->getControlPoint(i - 1);
        const TPointD delta(std::round((p.x - prior.x) * storedScale) / storedScale,
                            std::round((p.y - prior.y) * storedScale) / storedScale);
        // The native reader repairs coincident quadratic handles. This
        // deliberately nondegenerate fixture must not exercise that policy.
        require(delta != TPointD(), "morph PLI fixture acquired a degenerate stored handle");
        stored = previousStored + delta;
      }
      previousStored = stored;
      const TThickPoint point(std::trunc(stored.x * regionScale) / regionScale,
                               std::trunc(stored.y * regionScale) / regionScale,
                               std::min(255., std::round(p.thick / widthUnit)) * widthUnit);
      decoded->setControlPoint(i, point);
      if (i && (i % 2 == 0)) {
        const auto a = decoded->getControlPoint(i - 2), b = decoded->getControlPoint(i - 1);
        require(a != b || b != point, "morph PLI fixture acquired a degenerate region quadratic");
      }
    }
  }
  return expected;
}

void morphCallerChecks() {
  auto *app = TApp::instance();
  QString resetError;
  if (!resetNativeUiIntegrationScene(&resetError))
    throw std::runtime_error(resetError.toStdString());
  TXshSimpleLevelP level = app->getCurrentScene()->getScene()->createNewLevel(
      PLI_XSHLEVEL, L"MorphCallerFixture")->getSimpleLevel();
  TXshSimpleLevelP other = app->getCurrentScene()->getScene()->createNewLevel(
      PLI_XSHLEVEL, L"MorphCallerOther")->getSimpleLevel();
  TPaletteP palette = new TPalette;
  level->setPalette(palette.getPointer()); other->setPalette(palette.getPointer());
  auto make = [&](double x) {
    TVectorImageP image = new TVectorImage;
    image->setPalette(palette.getPointer());
    auto *stroke = new TStroke(std::vector<TThickPoint>{
        {x, 0., 2.}, {x + 10., 10., 2.}, {x + 20., 0., 2.}});
    stroke->setStyle(1);
    stroke->setOpacityProfile({{0., .3}, {1., .9}});
    image->addStroke(stroke);
    return image;
  };
  other->setFrame(TFrameId(1), make(100.));
  other->getProperties()->setDirtyFlag(false);
  auto *undo = TUndoManager::manager();
  QScrollArea area;
  area.resize(260, 480);
  auto *strip = new FilmstripFrames(&area);
  area.setWidget(strip);
  strip->setLevel(level.getPointer()); strip->setActive(true);
  area.show();
  QCoreApplication::processEvents();

  auto selectRange = [&]() {
    strip->select(0, FilmstripFrames::ONLY_SELECT);
    strip->select(2, FilmstripFrames::SHIFT_SELECT);
  };
  auto resetDrawing = [&]() {
    undo->reset();
    level->clearFrames();
    level->setMorphHints({});
    level->setFrame(TFrameId(1), make(0.));
    level->setFrame(TFrameId(2), make(50.));
    level->setFrame(TFrameId(3), make(20.));
    palette->getStyle(1)->setMainColor(TPixel32::Black);
    app->getCurrentLevel()->setLevel(level.getPointer());
    app->getCurrentFrame()->setFid(TFrameId(1));
    app->getCurrentLevel()->notifyLevelChange();
    selectRange();
    QCoreApplication::processEvents();
    auto *selection = dynamic_cast<TFilmstripSelection *>(TSelection::getCurrent());
    require(selection && selection->getInbetweenRange() ==
                TFilmstripSelection::InbetweenRange(TFrameId(1), TFrameId(3)),
            "actual Filmstrip fixture did not select its three-frame range");
    level->getProperties()->setDirtyFlag(false);
    palette->setDirtyFlag(false);
    app->getCurrentScene()->setDirtyFlag(false);
  };

  struct Drawing {
    TFrameId fid;
    TVectorImageP live, expected;
  };
  auto drawings = [&]() {
    std::vector<Drawing> result;
    std::vector<TFrameId> fids; level->getFids(fids);
    for (const auto &fid : fids) {
      TVectorImageP image = level->getFrame(fid, false);
      require(image, "morph caller fixture lost a drawing before review");
      result.push_back({fid, image, image->clone()});
    }
    return result;
  };
  auto sameAuthoredDrawing = [&](const TVectorImageP &live, const TVectorImageP &expected,
                                 bool checkIds = true) {
    if (!live || !expected || !sameGeometry(live, expected)) return false;
    for (UINT i = 0; i < live->getStrokeCount(); ++i) {
      const auto *a = live->getStroke(i), *b = expected->getStroke(i);
      if ((checkIds && a->getId() != b->getId()) || a->getStyle() != b->getStyle() ||
          a->isSelfLoop() != b->isSelfLoop() ||
          a->getOpacity() != b->getOpacity() || a->getOpacityProfile() != b->getOpacityProfile())
        return false;
    }
    return true;
  };
  // The caller is entered through the painted inbetween hit target, never by
  // wiring a standalone dialog's accepted signal to the command under test.
  auto invoke = [&](int scenario) {
    bool prepared = false, delivered = false, accepted = false, timedOut = false;
    bool queuedApplyRequested = false, queuedApplyDelivered = false, applyClicked = false;
    bool warning = false;
    std::exception_ptr failure;
    std::vector<Drawing> expected;
    std::vector<TFrameId> expectedFids;
    std::string expectedHints;
    std::set<TFrameId> expectedSelection;
    TSelection *expectedActiveSelection = nullptr;
    ToonzScene *expectedScene = nullptr;
    TXshLevel *expectedCurrentLevel = nullptr;
    TPixel32 expectedColor;
    bool expectedDirty = false, expectedLevelDirty = false;
    int expectedHistory = 0;
    QObject callbackContext;
    QTimer driver, deadline;
    driver.setInterval(20); deadline.setSingleShot(true);
    QPointer<InbetweenDialog> review;
    QObject::connect(&deadline, &QTimer::timeout, &callbackContext, [&]() {
      timedOut = true;
      if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) dialog->reject();
    });
    QObject::connect(&driver, &QTimer::timeout, &callbackContext, [&]() {
      auto *modal = qobject_cast<QDialog *>(QApplication::activeModalWidget());
      auto *dialog = dynamic_cast<InbetweenDialog *>(modal);
      if (modal && !dialog) {
        QString text;
        if (auto *message = qobject_cast<QMessageBox *>(modal)) text = message->text();
        else for (auto *label : modal->findChildren<QLabel *>()) text += label->text() + '\n';
        QPushButton *ok = nullptr;
        for (auto *button : modal->findChildren<QPushButton *>()) {
          QString caption = button->text(); caption.remove('&');
          if (caption == "OK" && button->isVisible() && button->isEnabled()) ok = button;
        }
        // DVGui::warning uses a DVGui::Dialog popup, not QMessageBox. Dismiss
        // only this caller's exact stale-source explanation through its button.
        if (text.contains("The drawings, range or morph hints changed while the preview was open.") &&
            text.contains("No drawing was overwritten.") && ok) {
          warning = true; ok->click();
        } else {
          failure = std::make_exception_ptr(std::runtime_error(
              "unexpected modal in morph caller: " + text.toStdString()));
          modal->reject();
        }
        return;
      }
      if (!dialog || delivered) return;
      review = dialog;
      if (!prepared) {
        dialog->setValue("Linear"); dialog->setAutoMatch(false);
        dialog->setUnmatchedPolicy(TInbetween::HoldUnmatched);
        prepared = true;
      }
      auto *table = dialog->findChild<QTableWidget *>("morphCorrespondence");
      auto *apply = dialog->findChild<QPushButton *>("morphApply");
      if (!table || !apply || !table->property("previewReady").toBool() || !apply->isEnabled()) return;
      delivered = true;
      QObject::connect(apply, &QPushButton::clicked, &callbackContext,
                       [&]() { applyClicked = true; });
      // A separate queued callback leaves this repeating timer available for
      // the real caller's stale-source warning after the preview loop exits.
      QTimer::singleShot(0, &callbackContext, [&, apply]() {
        try {
          require(review, "morph caller review disappeared before queued action");
          QPointer<QPushButton> queuedApply(apply);
          if (scenario >= 0)
            queuedApplyRequested = QMetaObject::invokeMethod(apply, [&, apply] {
              queuedApplyDelivered = true; apply->click();
            }, Qt::QueuedConnection);
          TVectorImageP first = level->getFrame(TFrameId(1), false);
          TVectorImageP middle = level->getFrame(TFrameId(2), false);
          TVectorImageP last = level->getFrame(TFrameId(3), false);
          if (scenario == 0) first->getStroke(0)->transform(TTranslation(4., 3.));
          else if (scenario == 1) level->setFrame(TFrameId(2), make(75.));
          else if (scenario == 2) {
            middle->getStroke(0)->transform(TTranslation(-7., 6.));
            middle->getStroke(0)->setOpacityProfile({{0., .8}, {1., .1}});
          } else if (scenario == 3) level->setFrame(TFrameId(2, "A"), make(90.));
          else if (scenario == 4) {
            QJsonObject hint{{"kind", "pair"}, {"firstFrame", MorphHints::frame(TFrameId(1))},
                {"lastFrame", MorphHints::frame(TFrameId(3))},
                {"first", MorphHints::anchor(first, 0)}, {"last", MorphHints::anchor(last, 0)}};
            level->setMorphHints(MorphHints::encode({hint}));
          } else if (scenario == 5) palette->getStyle(1)->setMainColor(TPixel32(70, 140, 210));
          else if (scenario == 6) strip->select(1, FilmstripFrames::ONLY_SELECT);
          else if (scenario == 7) {
            app->getCurrentLevel()->setLevel(other.getPointer());
            app->getCurrentLevel()->setLevel(level.getPointer());
            selectRange();
          } else if (scenario == 8) {
            strip->select(1, FilmstripFrames::ONLY_SELECT); selectRange();
          }
          if (scenario >= 0 && scenario < 6) {
            level->setDirtyFlag(true); app->getCurrentScene()->setDirtyFlag(true);
          }
          expected = drawings();
          if (scenario == 9) {
            // New Scene intentionally clears old level frames and schedules
            // the old scene for deletion. Never reactivate that old pointer.
            // Hold the actual drawings to verify the canceled caller does not
            // change their pixels or write into the newly active scene.
            IoCmd::newScene();
          } else if (scenario == 10) {
            TSelection *closingSelection = TSelection::getCurrent();
            auto *closing = area.takeWidget();
            strip = nullptr;
            delete closing;
            require(TSelection::getCurrent() != closingSelection,
                    "closing a Filmstrip during preview retained its dead selection");
          } else if (scenario == 11) {
            strip->hide(); strip->show();
          } else if (scenario == 12) {
            area.hide(); area.show();
          }
          level->getFids(expectedFids);
          expectedHints = level->getMorphHints();
          if (auto *selection = dynamic_cast<TFilmstripSelection *>(TSelection::getCurrent()))
            expectedSelection = selection->getSelectedFids();
          expectedActiveSelection = TSelection::getCurrent();
          expectedScene = app->getCurrentScene()->getScene();
          expectedCurrentLevel = app->getCurrentLevel()->getLevel();
          expectedColor = palette->getStyle(1)->getMainColor();
          expectedDirty = app->getCurrentScene()->getDirtyFlag();
          expectedLevelDirty = level->getProperties()->getDirtyFlag();
          expectedHistory = undo->getHistoryCount();
          if (scenario >= 0) {
            if (queuedApply) QCoreApplication::sendPostedEvents(queuedApply, QEvent::MetaCall);
            accepted = review && review->result() == QDialog::Accepted;
          } else {
            auto *button = review->findChild<QPushButton *>(
                scenario == -1 ? "morphCancel" : "morphApply");
            require(button, "morph caller Apply/Cancel control missing");
            button->click();
          }
        } catch (...) {
          failure = std::current_exception();
          if (review) review->reject();
        }
      });
    });
    const QPoint point(strip->width() - 30,
        strip->index2y(1) + strip->getOneFrameHeight() / 2);
    require(strip->rect().contains(point), "morph inbetween hit target is outside its Filmstrip");
    QMouseEvent press(QEvent::MouseButtonPress, point, Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QPointer<FilmstripFrames> target(strip);
    driver.start(); deadline.start(10000);
    QApplication::sendEvent(target, &press);
    driver.stop(); deadline.stop();
    QMouseEvent release(QEvent::MouseButtonRelease, point, Qt::LeftButton,
                        Qt::NoButton, Qt::NoModifier);
    if (target) QApplication::sendEvent(target, &release);
    if (failure) std::rethrow_exception(failure);
    require(prepared && delivered && !timedOut, "actual Filmstrip morph caller did not finish its review");
    if (scenario == -2) return;
    if (scenario >= 0)
      require(queuedApplyRequested && (!review || (queuedApplyDelivered &&
                  (scenario >= 10 ? !applyClicked : applyClicked))),
              "morph fixture did not deliver its queued native Apply");
    if (scenario >= 0 && scenario < 6) {
      require(accepted, "silent-source morph change did not reach the caller's Accepted guard");
      require(warning, "changed-source morph caller did not explain its rejected Apply");
    } else if (scenario >= 6) {
      // A signaled rejection clears the dialog's detached sources. Its real
      // accept() rechecks the preview and must refuse the queued Apply.
      require(!review || review->result() == QDialog::Rejected,
              "queued morph Apply reaccepted a closed context review");
    }
    std::vector<TFrameId> actualFids; level->getFids(actualFids);
    require(actualFids == expectedFids && level->getMorphHints() == expectedHints &&
            app->getCurrentScene()->getScene() == expectedScene &&
            app->getCurrentLevel()->getLevel() == expectedCurrentLevel &&
            TSelection::getCurrent() == expectedActiveSelection &&
            palette->getStyle(1)->getMainColor() == expectedColor &&
            undo->getHistoryCount() == expectedHistory &&
            app->getCurrentScene()->getDirtyFlag() == expectedDirty &&
            level->getProperties()->getDirtyFlag() == expectedLevelDirty,
            "rejected actual morph caller changed intervening context/metadata/history/dirty state");
    if (auto *selection = dynamic_cast<TFilmstripSelection *>(TSelection::getCurrent()))
      require(selection->getSelectedFids() == expectedSelection,
              "rejected morph caller restored an obsolete selection");
    for (const auto &drawing : expected) {
      require(sameAuthoredDrawing(drawing.live, drawing.expected),
              "rejected morph caller changed intervening drawing content");
      if (scenario != 9)
        require(TVectorImageP(level->getFrame(drawing.fid, false)) == drawing.live,
                "rejected morph caller replaced an intervening drawing identity");
    }
  };

  for (int scenario = 0; scenario < 9; ++scenario) {
    resetDrawing();
    std::cout << "vector-morph-caller: stale scenario " << scenario << std::endl;
    invoke(scenario);
  }
  for (int scenario : {11,12}) {
    resetDrawing();
    std::cout << "vector-morph-caller: owner hide/show scenario " << scenario << std::endl;
    invoke(scenario);
  }
  resetDrawing(); invoke(-1);
  auto before = drawings();
  TInbetween::MatchOptions options; options.m_unmatchedPolicy = TInbetween::HoldUnmatched;
  TVectorImageP expectedMiddle = TInbetween(before.front().live, before.back().live, {}, options).tween(.5);
  auto verifyApplied = [&]() {
    require(sameAuthoredDrawing(level->getFrame(TFrameId(2), false), expectedMiddle, false),
            "actual Filmstrip Apply/redo differed from the reviewed interpolation");
    for (int index : {0, 2}) {
      const auto &endpoint = before[index];
      require(TVectorImageP(level->getFrame(endpoint.fid, false)) == endpoint.live &&
              sameAuthoredDrawing(endpoint.live, endpoint.expected),
              "actual Filmstrip Apply/redo altered a key drawing");
    }
  };
  invoke(-2);
  require(undo->getHistoryCount() == 1, "actual Filmstrip Apply did not create one transaction");
  verifyApplied();
  for (int repeat = 0; repeat < 3; ++repeat) {
    undo->undo();
    for (const auto &drawing : before)
      require(TVectorImageP(level->getFrame(drawing.fid, false)) == drawing.live &&
              sameAuthoredDrawing(drawing.live, drawing.expected),
              "actual Filmstrip undo lost original drawing identity or content");
    undo->redo();
    verifyApplied();
  }
  std::cout << "vector-morph-caller: actual Apply and three Undo/Redo cycles verified" << std::endl;
  QTemporaryDir directory;
  require(directory.isValid(), "morph caller reload directory unavailable");
  const TFilePath path(directory.filePath("caller.pli"));
  const TPixel32 beforeWriteColor = palette->getStyle(1)->getMainColor();
  { TLevelWriterP writer(path);
    for (int frame = 1; frame <= 3; ++frame)
      writer->getFrameWriter(TFrameId(frame))->save(level->getFrame(TFrameId(frame), false));
  }
  verifyApplied();
  require(level->getPalette() == palette.getPointer() &&
          TVectorImageP(level->getFrame(TFrameId(2), false))->getPalette() == palette.getPointer() &&
          palette->getStyle(1)->getMainColor() == beforeWriteColor,
          "saving actual Filmstrip result changed its live palette binding or color");
  std::cout << "vector-morph-caller: native save preserved live authored result" << std::endl;
  TLevelReaderP reader(path);
  TLevelP loaded = reader->loadInfo();
  require(loaded && loaded->getPalette(), "morph caller reopen lost its palette");
  TVectorImageP reopened = reader->getFrameReader(TFrameId(2))->load();
  require(reopened, "morph caller reopen lost the generated middle drawing");
  reopened->setPalette(loaded->getPalette());
  const TVectorImageP persistedExpected = expectedMorphPliDrawing(expectedMiddle);
  const bool drawingMatches = sameAuthoredDrawing(reopened, persistedExpected, false);
  const auto actualColor = reopened->getPalette()->getStyle(1)->getMainColor();
  const auto expectedColor = palette->getStyle(1)->getMainColor();
  if (!drawingMatches || actualColor != expectedColor) {
    // Preserve the strict oracle while identifying which serialized field
    // differs. Native PLI's coordinate/width encoding is separate from the
    // exact profile payload; a generic Apply failure cannot distinguish them.
    std::ostringstream diagnostic;
    diagnostic << std::setprecision(17)
               << "vector-morph-caller: PLI reload geometry="
               << geometryDifference(reopened, persistedExpected)
               << " palette actual=" << int(actualColor.r) << ',' << int(actualColor.g)
               << ',' << int(actualColor.b) << ',' << int(actualColor.m)
               << " expected=" << int(expectedColor.r) << ',' << int(expectedColor.g)
               << ',' << int(expectedColor.b) << ',' << int(expectedColor.m);
    const UINT count = std::min<UINT>(4, std::min(reopened->getStrokeCount(), expectedMiddle->getStrokeCount()));
    for (UINT i = 0; i < count; ++i) {
      const auto *actual = reopened->getStroke(i), *expected = expectedMiddle->getStroke(i);
      const auto &a = actual->getOpacityProfile(), &b = expected->getOpacityProfile();
      diagnostic << " stroke=" << i << " style=" << actual->getStyle() << '/' << expected->getStyle()
                 << " opacity=" << actual->getOpacity() << '/' << expected->getOpacity()
                 << " profile_knots=" << a.size() << '/' << b.size();
      for (size_t knot = 0; knot < std::min(a.size(), b.size()); ++knot) {
        if (a[knot] == b[knot]) continue;
        diagnostic << " first_profile_difference=" << knot << ":(" << a[knot].x << ','
                   << a[knot].y << ")/(" << b[knot].x << ',' << b[knot].y << ')';
        break;
      }
    }
    std::cerr << diagnostic.str() << std::endl;
  }
  require(drawingMatches && actualColor == expectedColor,
          "actual Filmstrip Apply did not persist authored geometry/opacity/palette");
  std::cout << "vector-morph-caller: PLI reload matches exact stored/region geometry, width and authored opacity" << std::endl;

  // The scroll-area viewport survives frame replacement. Its old resize
  // filter must retire before selection-detachment signals can reenter layout,
  // rather than retaining a raw pointer to the deleted frame widget.
  for (int cycle = 0; cycle < 3; ++cycle) {
    std::cout << "vector-morph-caller: retire strip and resize surviving viewport " << cycle << std::endl;
    TSelection *closingSelection = TSelection::getCurrent();
    const auto filters = strip->findChildren<QObject *>(
        "filmstripViewportResizeFilter", Qt::FindDirectChildrenOnly);
    require(filters.size() == 1, "Filmstrip does not own exactly one viewport resize filter");
    QPointer<QObject> oldFilter(filters.front());
    QPointer<FilmstripFrames> oldStrip(strip);
    bool detached = false, retiredBeforeDetach = false;
    QObject detachObserver;
    QObject::connect(app->getCurrentSelection(), &TSelectionHandle::selectionSwitched,
                     &detachObserver, [&](TSelection *oldSelection, TSelection *selection) {
      if (oldSelection != closingSelection || selection) return;
      detached = true;
      retiredBeforeDetach = oldFilter.isNull();
      QResizeEvent reentrantResize(area.viewport()->size(), area.viewport()->size());
      QApplication::sendEvent(area.viewport(), &reentrantResize);
    });
    auto *closing = area.takeWidget(); strip = nullptr;
    require(closing && closing == oldStrip && !closing->parentWidget() && oldFilter,
            "Filmstrip takeWidget did not detach the still-live strip");
    // takeWidget removes the frame's QWidget parent before its destructor
    // runs. Layout cannot consult that missing parent during the live gap.
    QResizeEvent detachedResize(area.viewport()->size(), area.viewport()->size());
    QApplication::sendEvent(area.viewport(), &detachedResize);
    delete closing;
    require(!oldStrip && !oldFilter && detached && retiredBeforeDetach &&
                TSelection::getCurrent() != closingSelection,
            "Filmstrip teardown retained a resize callback or dead selection during layout");
    QResizeEvent lateResize(area.viewport()->size(), area.viewport()->size());
    QApplication::sendEvent(area.viewport(), &lateResize);
    QCoreApplication::processEvents();
    std::cout << "vector-morph-caller: construct and resize replacement strip " << cycle << std::endl;
    strip = new FilmstripFrames(&area); area.setWidget(strip);
    strip->setLevel(level.getPointer()); strip->setActive(true); strip->show();
    resetDrawing();
    require(dynamic_cast<TFilmstripSelection *>(TSelection::getCurrent()) != nullptr,
            "replacement Filmstrip did not acquire a live selection");
    strip->updateIconLayout();
    const int viewportWidth = area.viewport()->width(), iconWidth = strip->getIconSize().width();
    const auto beforeResize = drawings();
    const int resizeHistory = undo->getHistoryCount();
    const bool resizeDirty = app->getCurrentScene()->getDirtyFlag();
    area.resize(area.width() + 64, area.height() + 16);
    QCoreApplication::processEvents();
    require(area.viewport()->width() != viewportWidth &&
                strip->getIconSize().width() - iconWidth == area.viewport()->width() - viewportWidth &&
                undo->getHistoryCount() == resizeHistory &&
                app->getCurrentScene()->getDirtyFlag() == resizeDirty,
            "replacement Filmstrip viewport resize lost responsive layout or changed history");
    for (const auto &drawing : beforeResize)
      require(TVectorImageP(level->getFrame(drawing.fid, false)) == drawing.live &&
                  sameAuthoredDrawing(drawing.live, drawing.expected),
              "replacement Filmstrip resize changed authored drawings");
    invoke(-1);
  }
  std::cout << "vector-morph-caller: delete strip inside actual preview" << std::endl;
  resetDrawing(); invoke(10);
  require(!strip, "queued Filmstrip destruction did not run inside the actual preview");
  strip = new FilmstripFrames(&area); area.setWidget(strip);
  strip->setLevel(level.getPointer()); strip->setActive(true); strip->show();
  resetDrawing();
  auto *oldScene = app->getCurrentScene()->getScene();
  std::cout << "vector-morph-caller: replace scene inside actual preview" << std::endl;
  invoke(9);
  require(app->getCurrentScene()->getScene() != oldScene &&
          app->getCurrentLevel()->getLevel() == nullptr && undo->getHistoryCount() == 0,
          "morph New Scene cancellation restored the discarded scene");
  std::cout << "vector-morph-caller: PASS" << std::endl;
}
}

// Runs in the real application's native UI fixture, with its tool, level,
// scene, viewer and undo stack. This exercises the transaction, not a mock.
void runVectorFillNativeChecks() {
  auto *app = TApp::instance();
  auto *level = app->getCurrentScene()->getScene()->createNewLevel(
      PLI_XSHLEVEL, L"FillReviewFixture")->getSimpleLevel();
  TPaletteP palette = new TPalette;
  level->setPalette(palette.getPointer());
  TVectorImageP image = new TVectorImage;
  image->setPalette(palette.getPointer());
  auto *stroke = new TStroke(std::vector<TThickPoint>{
      {-20,-20,1}, {0,-20,1}, {20,-20,1}, {20,0,1}, {20,20,1},
      {0,20,1}, {-20,20,1}, {-20,0,1}, {-20,-20,1}});
  stroke->setStyle(1); stroke->setSelfLoop(true); image->addStroke(stroke);
  image->findRegions();
  require(image->getRegion(TPointD(0,0)), "fill fixture has no region");
  image->getRegion(TPointD(0,0))->setStyle(1);
  level->setFrame(TFrameId(1), image);
  app->getCurrentXsheet()->getXsheet()->setCell(0, 0, TXshCell(level, TFrameId(1)));
  app->getCurrentXsheet()->getXsheet()->setCell(1, 0, TXshCell(level, TFrameId(1)));
  app->getCurrentColumn()->setColumnIndex(0);
  app->getCurrentFrame()->setFrame(0);
  app->getCurrentLevel()->setLevel(level);
  app->getCurrentTool()->setTool("T_ControlPointEditor");
  QCoreApplication::processEvents();
  auto *tool = app->getCurrentTool()->getTool();
  setBool(tool, "Auto Select Drawing", false);
  setBool(tool, "Review Fills", true);
  auto *undo = TUndoManager::manager();
  undo->reset();
  TMouseEvent mouse;
  mouse.m_button = Qt::LeftButton; mouse.m_buttons = Qt::LeftButton;
  // Binding the existing Control Point Editor subdivides odd quadratic spans
  // between cusp points before a drag transaction starts. Verify that binding
  // independently, then retain exact control-point equality for drag rollback.
  // Each straight side of this square is split exactly in half (9 -> 17 CPs).
  fillCheckpoint("control-point selection normalization");
  TVectorImageP unbound = image->clone();
  app->getCurrentScene()->setDirtyFlag(false);
  tool->mouseMove(TPointD(20,20), mouse);
  tool->leftButtonDown(TPointD(20,20), mouse);
  tool->leftButtonUp(TPointD(20,20), mouse);
  TVectorImageP selectedShape = new TVectorImage;
  selectedShape->addStroke(new TStroke(std::vector<TThickPoint>{
      {-20,-20,1}, {-10,-20,1}, {0,-20,1}, {10,-20,1}, {20,-20,1},
      {20,-10,1}, {20,0,1}, {20,10,1}, {20,20,1}, {10,20,1},
      {0,20,1}, {-10,20,1}, {-20,20,1}, {-20,10,1}, {-20,0,1},
      {-20,-10,1}, {-20,-20,1}}));
  requireGeometry(image, selectedShape, "control-point selection changed square shape");
  require(image->getStroke(0)->getId() == unbound->getStroke(0)->getId() &&
              image->getStroke(0)->getStyle() == unbound->getStroke(0)->getStyle() &&
              image->getStroke(0)->isSelfLoop() == unbound->getStroke(0)->isSelfLoop(),
          "control-point selection changed stroke identity, style or closure");
  requireFilledCenter(image, "control-point selection");
  require(undo->getHistoryCount() == 0 && !app->getCurrentScene()->getDirtyFlag(),
          "control-point selection changed history or dirty state");
  TVectorImageP original = image->clone();
  auto pick = [&]() {
    tool->mouseMove(TPointD(20,20), mouse);
    tool->leftButtonDown(TPointD(20,20), mouse);
    tool->leftButtonUp(TPointD(20,20), mouse);
    tool->leftButtonDown(TPointD(20,20), mouse);
  };
  auto edit = [&](bool accept, bool switchTool = false, bool switchFrame = false,
                  int ownerMode = -1) {
    bool sawDialog = false, cleanDuringReview = false, queuedAccepted = false, ownerCancelled = false;
    app->getCurrentScene()->setDirtyFlag(false);
    pick();
    tool->leftButtonDrag(TPointD(25,25), mouse);
    QPointer<QWidget> previous = QApplication::activeWindow();
    std::unique_ptr<QWidget> owner;
    if (ownerMode >= 0) {
      owner.reset(new QWidget);
      VectorModalTest::activateOwner(*owner, "fill");
    }
    QTimer response;
    response.setSingleShot(true);
    QObject::connect(&response, &QTimer::timeout, [&]() {
      auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
      sawDialog = dialog && dialog->objectName() == "vectorFillReview";
      cleanDuringReview = !app->getCurrentScene()->getDirtyFlag() &&
                          undo->getHistoryCount() == 0;
      if (sawDialog) {
        if (switchTool) {
          app->getCurrentTool()->setTool("T_Selection");
          if (dialog->isVisible()) dialog->reject();
          return;
        }
        if (switchFrame) {
          // Both rows expose this same drawing. Returning before the queued
          // Apply preserves all pointer/ID checks but must not revive review.
          app->getCurrentFrame()->setFrame(1);
          app->getCurrentFrame()->setFrame(0);
        }
        auto *button = dialog->findChild<QPushButton *>(accept ? "vectorFillApply" : "vectorFillRestore");
        if (button && ownerMode >= 0) {
          const bool exactOwner = VectorModalTest::hasReviewOwner(dialog, owner.get());
          if (ownerMode == 1) owner.reset(); else { owner->hide(); owner->show(); }
          bool delivered = false;
          const bool queued = QMetaObject::invokeMethod(button,[&] { delivered = true; button->click(); },
                                                       Qt::QueuedConnection);
          QCoreApplication::sendPostedEvents(button,QEvent::MetaCall);
          ownerCancelled = exactOwner && queued && delivered && !button->isEnabled() &&
                           dialog->result() == QDialog::Rejected;
        } else if (button && switchFrame) {
          const bool queued = QMetaObject::invokeMethod(button,"click",Qt::QueuedConnection);
          QCoreApplication::sendPostedEvents(button,QEvent::MetaCall);
          queuedAccepted = queued && dialog->result() == QDialog::Accepted;
        } else if (button) button->click(); else dialog->reject();
      } else if (dialog) dialog->reject();
    });
    response.start(0);
    tool->leftButtonUp(TPointD(25,25), mouse);
    require(sawDialog, "native control-point drag did not open fill review");
    require(cleanDuringReview, "review dirtied scene or recorded undo before Apply");
    require(!switchFrame || queuedAccepted,
            "fill review did not receive the queued Apply after context rejection");
    require(ownerMode < 0 || ownerCancelled,
            "queued Apply revived a fill review after its original owner closed");
    owner.reset(); if (previous) QApplication::setActiveWindow(previous);
  };
  fillCheckpoint("control-point Restore Original");
  edit(false);
  requireGeometry(image, original, "Restore Original failed to recover geometry");
  requireFilledCenter(image, "Restore Original");
  require(undo->getHistoryCount() == 0 && !app->getCurrentScene()->getDirtyFlag(),
          "cancel changed history or dirty state");
  fillCheckpoint("control-point frame round-trip rejects queued Apply");
  for (int repeat = 0; repeat < 2; ++repeat) {
    edit(true, false, true);
    TVectorImageP restored = level->getFrame(TFrameId(1),false);
    requireGeometry(restored, original, "frame-switch review did not restore original geometry");
    requireFilledCenter(restored, "frame-switch review");
    require(restored->getStroke(0)->getId() == original->getStroke(0)->getId() &&
            restored->getStroke(0)->getStyle() == original->getStroke(0)->getStyle() &&
            restored->getStroke(0)->isSelfLoop() == original->getStroke(0)->isSelfLoop() &&
            undo->getHistoryCount() == 0 && !app->getCurrentScene()->getDirtyFlag(),
            "queued Apply revived a dismissed fill review or changed history");
  }
  fillCheckpoint("control-point original owner hide/delete restores fill review");
  for (int destroy = 0; destroy < 2; ++destroy) {
    edit(true,false,false,destroy);
    TVectorImageP restored = level->getFrame(TFrameId(1),false);
    requireGeometry(restored, original, "owner-cancelled fill review lost original geometry");
    requireFilledCenter(restored, "owner-cancelled fill review");
    require(restored->getStroke(0)->getId() == original->getStroke(0)->getId() &&
                restored->getStroke(0)->getStyle() == original->getStroke(0)->getStyle() &&
                restored->getStroke(0)->isSelfLoop() == original->getStroke(0)->isSelfLoop() &&
                restored->getStroke(0)->getOpacity() == original->getStroke(0)->getOpacity() &&
                restored->getStroke(0)->getOpacityProfile() == original->getStroke(0)->getOpacityProfile() &&
                undo->getHistoryCount() == 0 && !app->getCurrentScene()->getDirtyFlag(),
            "owner-cancelled fill review changed authored properties/history/dirty state");
  }
  fillCheckpoint("control-point Apply and undo/redo");
  edit(true);
  require(undo->getHistoryCount() == 1 && !sameGeometry(image, original),
          "Apply did not record one changed drawing");
  TVectorImageP proposed = image->clone();
  undo->undo();
  requireGeometry(image, original, "control-point undo lost geometry");
  requireFilledCenter(image, "control-point undo");
  undo->redo();
  requireGeometry(image, proposed, "control-point redo lost geometry");
  requireFilledCenter(image, "control-point redo");
  undo->undo(); undo->reset();
  app->getCurrentScene()->setDirtyFlag(false);
  fillCheckpoint("control-point Escape and tool-switch cancellation");
  pick(); tool->leftButtonDrag(TPointD(25,25), mouse);
  QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
  require(tool->keyDown(&escape), "Escape did not cancel pending control-point drag");
  requireGeometry(image, original, "interrupted control-point drag geometry");
  requireFilledCenter(image, "interrupted control-point drag");
  require(undo->getHistoryCount() == 0 &&
              !app->getCurrentScene()->getDirtyFlag(), "interrupted drag was not restored");
  edit(false, true);
  requireGeometry(image, original, "tool-switch control-point cancellation geometry");
  requireFilledCenter(image, "tool-switch control-point cancellation");
  require(undo->getHistoryCount() == 0 &&
              !app->getCurrentScene()->getDirtyFlag(), "tool switch during control-point review was not restored");

  // Free Deform and Envelope use the same staged multi-drawing transaction.
  // Exercise both through the actual Selection tool's corner handle.
  TVectorImageP second = original->clone();
  level->setFrame(TFrameId(2), second);
  app->getCurrentTool()->setTool("T_Selection");
  QCoreApplication::processEvents();
  tool = app->getCurrentTool()->getTool();
  auto *mode = dynamic_cast<TEnumProperty *>(tool->getProperties(0)->getProperty(std::string("Mode:")));
  require(mode, "native selection mode missing");
  mode->setValue(L"Whole Level"); tool->onPropertyChanged("Mode:");
  setBool(tool, "Review Fills", true);
  auto deform = [&](bool envelope, bool accept, bool switchTool = false) {
    setBool(tool, "Envelope", envelope);
    bool sawDialog = false, clean = false, stagedBoth = false;
    bool expired = false;
    std::exception_ptr responseFailure;
    app->getCurrentScene()->setDirtyFlag(false);
    TMouseEvent cornerEvent = mouse;
    cornerEvent.m_modifiersMask = TMouseEvent::CTRL_KEY;
    const TPointD corner = image->getBBox().getP11();
    const TPointD moved = corner + TPointD(8, 5);
    fillCheckpoint("deformation gesture begins");
    tool->mouseMove(corner, cornerEvent);
    tool->leftButtonDown(corner, cornerEvent);
    tool->leftButtonDrag(moved, cornerEvent);
    fillCheckpoint("deformation drag completed");
    QTimer response, deadline;
    response.setInterval(10);
    deadline.setSingleShot(true);
    auto reportModal = [&]() {
      QWidget *modal = QApplication::activeModalWidget();
      std::cout << "vector-fill-native: active modal class="
                << (modal ? modal->metaObject()->className() : "none")
                << " name=" << (modal ? modal->objectName().left(120).toStdString() : "")
                << " title=" << (modal ? modal->windowTitle().left(160).toStdString() : "")
                << std::endl;
    };
    QObject::connect(&response, &QTimer::timeout, [&]() {
      QPointer<QDialog> dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
      if (!dialog || dialog->objectName() != "vectorFillReview") return;
      try {
        auto *button = dialog->findChild<QPushButton *>(accept ? "vectorFillApply" : "vectorFillRestore");
        if (!button || !button->isVisible() || !button->isEnabled()) return;
        response.stop();
        sawDialog = true;
        fillCheckpoint("deformation review received");
        auto *frames = dialog->findChild<QComboBox *>("vectorFillFrame");
        stagedBoth = frames && frames->count() == 2 &&
                     !sameGeometry(image, original) && !sameGeometry(second, original);
        if (frames) frames->setCurrentIndex(1);
        clean = !app->getCurrentScene()->getDirtyFlag() && undo->getHistoryCount() == 0;
        if (switchTool) {
          fillCheckpoint("deformation review switches tool");
          app->getCurrentTool()->setTool("T_ControlPointEditor");
          if (dialog && dialog->isVisible()) dialog->reject();
          return;
        }
        fillCheckpoint("deformation review button click begins");
        button->click();
        fillCheckpoint("deformation review button click returned");
      } catch (...) {
        responseFailure = std::current_exception();
        response.stop();
        reportModal();
        if (dialog) dialog->reject();
      }
    });
    QObject::connect(&deadline, &QTimer::timeout, [&]() {
      expired = true;
      response.stop();
      fillCheckpoint("deformation review deadline expired");
      reportModal();
      if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()))
        dialog->reject();
    });
    response.start(); deadline.start(10000);
    fillCheckpoint("deformation leftButtonUp begins");
    tool->leftButtonUp(moved, cornerEvent);
    fillCheckpoint("deformation leftButtonUp returned");
    response.stop(); deadline.stop();
    if (responseFailure) std::rethrow_exception(responseFailure);
    require(!expired, "deformation review did not finish within its 10-second deadline");
    require(sawDialog && stagedBoth, "deformation did not review both staged drawings");
    require(clean, "multi-drawing review dirtied history before Apply");
  };
  for (bool envelope : {false, true}) {
    fillCheckpoint(envelope ? "Envelope Restore Original" : "Free Deform Restore Original");
    deform(envelope, false);
    fillCheckpoint("deformation cancellation geometry checks");
    requireGeometry(image, original, "deformation cancellation drawing 1");
    requireGeometry(second, original, "deformation cancellation drawing 2");
    require(undo->getHistoryCount() == 0 && !app->getCurrentScene()->getDirtyFlag(),
            "multi-drawing deformation cancel did not restore all drawings");
    fillCheckpoint("deformation cancellation pixel checks");
    requireFilledCenter(image, "deformation cancellation drawing 1");
    requireFilledCenter(second, "deformation cancellation drawing 2");
    fillCheckpoint(envelope ? "Envelope Apply and undo/redo" : "Free Deform Apply and undo/redo");
    deform(envelope, true);
    require(undo->getHistoryCount() == 1, "deformation did not create one undo transaction");
    TVectorImageP changedFirst = image->clone(), changedSecond = second->clone();
    fillCheckpoint("deformation undo begins");
    undo->undo();
    fillCheckpoint("deformation undo returned; geometry checks");
    requireGeometry(image, original, "deformation undo drawing 1");
    requireGeometry(second, original, "deformation undo drawing 2");
    fillCheckpoint("deformation undo pixel checks");
    requireFilledCenter(image, "deformation undo drawing 1");
    requireFilledCenter(second, "deformation undo drawing 2");
    fillCheckpoint("deformation redo begins");
    undo->redo();
    fillCheckpoint("deformation redo returned; geometry checks");
    requireGeometry(image, changedFirst, "deformation redo drawing 1");
    requireGeometry(second, changedSecond, "deformation redo drawing 2");
    fillCheckpoint("deformation redo pixel checks");
    requireFilledCenter(image, "deformation redo drawing 1");
    requireFilledCenter(second, "deformation redo drawing 2");
    fillCheckpoint("deformation final undo begins");
    undo->undo(); undo->reset();
    fillCheckpoint("deformation final undo completed");
  }
  fillCheckpoint("Free Deform tool-switch cancellation");
  deform(false, false, true);
  requireGeometry(image, original, "tool-switch deformation cancellation drawing 1");
  requireGeometry(second, original, "tool-switch deformation cancellation drawing 2");
  requireFilledCenter(image, "tool-switch deformation cancellation drawing 1");
  requireFilledCenter(second, "tool-switch deformation cancellation drawing 2");
  require(undo->getHistoryCount() == 0 && !app->getCurrentScene()->getDirtyFlag(),
          "tool switch during multi-drawing review was not restored");
  fillCheckpoint("PASS");
}

void runVectorMorphNativeChecks() {
  auto *app = TApp::instance();
  auto *level = app->getCurrentScene()->getScene()->createNewLevel(
      PLI_XSHLEVEL, L"MorphReviewFixture")->getSimpleLevel();
  TPaletteP palette = new TPalette;
  level->setPalette(palette.getPointer());
  auto make = [&](const std::vector<double> &offsets) {
    TVectorImageP image = new TVectorImage;
    image->setPalette(palette.getPointer());
    for (double x : offsets) {
      auto *stroke = new TStroke(std::vector<TThickPoint>{
          {x,0,2}, {x+5,10,2}, {x+10,0,2}});
      stroke->setStyle(1); image->addStroke(stroke);
    }
    return image;
  };
  TVectorImageP first = make({0,30,60}), last = make({10,50}), blank = make({});
  level->setFrame(TFrameId(1), first);
  level->setFrame(TFrameId(2), blank);
  level->setFrame(TFrameId(3), last);
  level->setFrame(TFrameId(4), first->clone());
  QJsonObject pairHint{{"kind", "pair"}, {"firstFrame", MorphHints::frame(TFrameId(1))},
      {"lastFrame", MorphHints::frame(TFrameId(3))},
      {"first", MorphHints::anchor(first,2)}, {"last", MorphHints::anchor(last,0)}};
  QJsonObject zoneHint{{"kind", "zone"}, {"firstFrame", MorphHints::frame(TFrameId(1))},
      {"lastFrame", MorphHints::frame(TFrameId(3))},
      {"first", MorphHints::anchor(first,0)}, {"last", MorphHints::anchor(last,0)},
      {"firstMembers", QJsonArray{MorphHints::anchor(first,0),MorphHints::anchor(first,2)}},
      {"lastMembers", QJsonArray{MorphHints::anchor(last,0)}},
      {"firstRect", MorphHints::rect(first->getBBox())}, {"lastRect", MorphHints::rect(last->getBBox())}};
  level->setMorphHints(MorphHints::encode({pairHint,zoneHint}));
  require(StrokeSelection::getInbetweenStrokePairs(level,TFrameId(1),TFrameId(3)) ==
              TInbetween::StrokeCorrespondence{{2,0}}, "saved native stroke pairing did not resolve");
  require(StrokeSelection::getInbetweenStrokePairs(level,TFrameId(4),TFrameId(3)).empty(),
          "copied drawing inherited another drawing's saved pairing");
  require(StrokeSelection::getInbetweenZoneHints(level,TFrameId(1),TFrameId(3)).size() == 1,
          "saved native zone did not resolve");
  std::unique_ptr<TStroke> removed(new TStroke(*first->getStroke(2)));
  first->removeStroke(2,false);
  require(StrokeSelection::getInbetweenStrokePairs(level,TFrameId(1),TFrameId(3)).empty() &&
              StrokeSelection::getInbetweenZoneHints(level,TFrameId(1),TFrameId(3)).empty(),
          "deleted member left a stale pair or zone active");
  first->addStroke(removed.release());
  require(StrokeSelection::getInbetweenStrokePairs(level,TFrameId(3),TFrameId(1)) ==
              TInbetween::StrokeCorrespondence{{0,2}}, "reverse saved pairing did not follow drawings");
  app->getCurrentXsheet()->getXsheet()->setCell(0, 0, TXshCell(level, TFrameId(1)));
  app->getCurrentLevel()->setLevel(level);
  app->getCurrentFrame()->setFrame(0);
  auto *undo = TUndoManager::manager(); undo->reset();
  app->getCurrentScene()->setDirtyFlag(false);
  const TInbetween::StrokeCorrespondence pairs{{2,0}};
  InbetweenDialog dialog(nullptr);
  dialog.setStrokePairCount(1); dialog.setZoneHintCount(0);
  dialog.setAutoMatch(false); dialog.setUnmatchedPolicy(TInbetween::HoldUnmatched);
  dialog.setPreviewSource(first, last, pairs, {});
  bool reviewed = false, immutable = false, provenance = false;
  QTimer check, timeout;
  check.setInterval(30); timeout.setSingleShot(true);
  QObject::connect(&check, &QTimer::timeout, [&]() {
    auto *table = dialog.findChild<QTableWidget *>("morphCorrespondence");
    auto *status = dialog.findChild<QLabel *>("morphReviewStatus");
    if (!table || table->rowCount() != 3 || !status || !table->property("previewReady").toBool()) return;
    reviewed = true;
    bool manual = false, disappearing = false;
    for (int i = 0; i < table->rowCount(); ++i) {
      const int source = table->item(i,0)->data(Qt::UserRole).toInt();
      const int target = table->item(i,1)->data(Qt::UserRole).toInt();
      if (source == 2 && target == 0)
        manual = table->item(i,2)->data(Qt::UserRole).toInt() == TInbetween::ManualMatch;
      if (target < 0) disappearing = true;
    }
    provenance = manual && disappearing;
    table->selectRow(0); // Native source-highlight path.
    immutable = !app->getCurrentScene()->getDirtyFlag() && undo->getHistoryCount() == 0 &&
                TVectorImageP(level->getFrame(TFrameId(2), false))->getStrokeCount() == 0;
    dialog.reject();
  });
  QObject::connect(&timeout, &QTimer::timeout, &dialog, &QDialog::reject);
  check.start(); timeout.start(5000); dialog.exec(); check.stop(); timeout.stop();
  require(reviewed && provenance, "native morph correspondence review did not resolve manual/unmatched rows");
  require(immutable && !app->getCurrentScene()->getDirtyFlag(), "morph preview edited the document");
  FilmstripCmd::InbetweenOptions options;
  options.m_unmatchedPolicy = dialog.unmatchedPolicy();
  TInbetween::MatchOptions matchOptions;
  matchOptions.m_unmatchedPolicy = TInbetween::HoldUnmatched;
  TInbetween expected(first, last, pairs, matchOptions);
  TVectorImageP expectedMiddle = expected.tween(.5);
  FilmstripCmd::inbetween(level, TFrameId(1), TFrameId(3), FilmstripCmd::II_Linear, pairs, options);
  require(undo->getHistoryCount() == 1 &&
              sameGeometry(level->getFrame(TFrameId(2), false), expectedMiddle),
          "native morph command differs from reviewed resolver or undo transaction");
  require(TVectorImageP(level->getFrame(TFrameId(1), false)).getPointer() == first.getPointer() &&
              TVectorImageP(level->getFrame(TFrameId(3), false)).getPointer() == last.getPointer(),
          "native morph command replaced key drawings");
  undo->undo();
  require(TVectorImageP(level->getFrame(TFrameId(2), false))->getStrokeCount() == 0,
          "native morph undo did not restore intermediate drawing");
  undo->redo();
  require(sameGeometry(level->getFrame(TFrameId(2), false), expectedMiddle),
          "native morph redo does not retain unmatched policy");
  undo->reset();

  // Lettered drawing IDs must bound both generation and redo exactly.
  const TFrameId a(10,"A"), b(10,"B"), c(10,"C"), d(10,"D"), e(10,"E");
  TVectorImageP suffixFirst = first->clone(), suffixLast = last->clone(), outside = first->clone();
  level->setFrame(a,suffixFirst); level->setFrame(b,make({})); level->setFrame(c,make({}));
  level->setFrame(d,suffixLast); level->setFrame(e,outside);
  FilmstripCmd::inbetween(level,a,d,FilmstripCmd::II_Linear,pairs,options);
  require(undo->getHistoryCount() == 1, "lettered morph did not create one transaction");
  undo->undo(); undo->redo();
  require(TVectorImageP(level->getFrame(d,false)) == suffixLast &&
              TVectorImageP(level->getFrame(e,false)) == outside,
          "lettered morph redo changed an endpoint or outside drawing");
  undo->reset();

  // Each endpoint is legal, but their union exceeds the native knot limit.
  // Failure must leave the level and undo stack untouched.
  StrokeOpacity::Profile denseFirst{{0.,1.}}, denseLast{{0.,1.}};
  const int half = int(StrokeOpacity::MaxKnots / 2);
  for (int i = 1; i < half; ++i) {
    denseFirst.emplace_back(double(i) / half, .5);
    denseLast.emplace_back((i - .5) / half, .75);
  }
  denseFirst.emplace_back(1.,1.); denseLast.emplace_back(1.,1.);
  // Add one distinct interior knot on each side so the union is too large.
  denseFirst.insert(denseFirst.begin()+1,TPointD(.125 / half,.5));
  denseLast.insert(denseLast.begin()+1,TPointD(.25 / half,.75));
  require(first->getStroke(2)->setOpacityProfile(denseFirst) &&
              last->getStroke(0)->setOpacityProfile(denseLast), "dense morph fixture invalid");
  const TVectorImageP previousMiddle = level->getFrame(TFrameId(2),false);
  app->getCurrentScene()->setDirtyFlag(false);
  bool rejected = false;
  try { FilmstripCmd::inbetween(level,TFrameId(1),TFrameId(3),FilmstripCmd::II_Linear,pairs,options); }
  catch (...) { rejected = true; }
  require(rejected && TVectorImageP(level->getFrame(TFrameId(2),false)) == previousMiddle &&
              undo->getHistoryCount() == 0 && !app->getCurrentScene()->getDirtyFlag(),
          "failed morph changed artwork, history or dirty state");
  first->getStroke(2)->setOpacityProfile({}); last->getStroke(0)->setOpacityProfile({});

  // Drive the native preview, Apply and Cancel controls for the Fade policy.
  // The accepted dialog dispatches the same production Filmstrip command as
  // FilmstripFrames; the generated images and history are checked below.
  auto fadeFirst = make({0,30}), fadeLast = make({0}), fadeBlank = make({});
  const StrokeOpacity::Profile fadeProfile{{0.,.2},{.5,.8},{1.,1.}};
  fadeFirst->getStroke(1)->setOpacity(.6);
  require(fadeFirst->getStroke(1)->setOpacityProfile(fadeProfile), "native Fade profile fixture invalid");
  fadeFirst->group(1,1);
  level->setFrame(TFrameId(1),fadeFirst); level->setFrame(TFrameId(2),fadeBlank);
  level->setFrame(TFrameId(3),fadeLast);
  options.m_unmatchedPolicy = TInbetween::FadeUnmatched;
  app->getCurrentScene()->setDirtyFlag(false); undo->reset();
  auto fadeDialog = [&](bool apply, bool blocked) {
    InbetweenDialog review(nullptr);
    review.setValue("Linear"); review.setAutoMatch(false);
    review.setUnmatchedPolicy(TInbetween::FadeUnmatched);
    review.setPreviewSource(level->getFrame(TFrameId(1),false),
                            level->getFrame(TFrameId(3),false), {}, {});
    auto *policy = review.findChild<QComboBox *>("morphUnmatchedPolicy");
    auto *button = review.findChild<QPushButton *>("morphApply");
    auto *cancel = review.findChild<QPushButton *>("morphCancel");
    auto *position = review.findChild<QSlider *>("morphPreviewPosition");
    auto *swatch = review.findChild<PlaneViewer *>("morphResultPreview");
    auto *table = review.findChild<QTableWidget *>("morphCorrespondence");
    auto *status = review.findChild<QLabel *>("morphReviewStatus");
    require(policy && button && cancel && position && swatch && table && status &&
                policy->currentData().toInt() == TInbetween::FadeUnmatched,
            "native Fade controls or stable policy value missing");
    std::exception_ptr failure;
    QObject::connect(&review, &QDialog::accepted, [&]() {
      try { FilmstripCmd::inbetween(level,TFrameId(1),TFrameId(3),FilmstripCmd::II_Linear,{},options); }
      catch (...) { failure = std::current_exception(); }
    });
    const int history = undo->getCurrentHistoryIndex();
    const bool dirty = app->getCurrentScene()->getDirtyFlag();
    const bool levelDirty = level->getProperties()->getDirtyFlag();
    TVectorImageP before = level->getFrame(TFrameId(2),false);
    QTimer drive, deadline; drive.setInterval(30); deadline.setSingleShot(true);
    int stage = 0; bool finished = false; QImage midpoint, early;
    QObject::connect(&drive, &QTimer::timeout, [&]() {
      try {
        require(undo->getCurrentHistoryIndex() == history &&
                    app->getCurrentScene()->getDirtyFlag() == dirty &&
                    level->getProperties()->getDirtyFlag() == levelDirty &&
                    TVectorImageP(level->getFrame(TFrameId(2),false)) == before,
                "native Fade preview changed artwork/history/dirty state");
        if (blocked) {
          if (!status->text().contains("painted regions")) return;
          require(!button->isEnabled() && !table->property("previewReady").toBool(),
                  "painted-boundary Fade still enabled Apply");
          review.accept(); // Programmatic/keyboard acceptance must revalidate too.
          require(review.isVisible(), "unsupported Fade bypassed preflight through accept");
          finished = true; cancel->click(); return;
        }
        if (!table->property("previewReady").toBool()) return;
        require(button->isEnabled(), "supported Fade preview did not enable Apply");
        const QImage image = swatch->grabFramebuffer();
        require(!image.isNull(), "native Fade preview framebuffer is empty");
        if (stage == 0) { midpoint = image; ++stage; position->setValue(25); return; }
        if (stage == 1) {
          early = image; require(early != midpoint, "Fade preview did not change visible opacity");
          ++stage; position->setValue(75); return;
        }
        if (stage == 2) {
          require(image != early && image != midpoint, "Fade preview reused stale opacity");
          ++stage; position->setValue(50); return;
        }
        require(image == midpoint, "repeated Fade preview accumulated opacity changes");
        finished = true;
        if (apply) button->click(); else cancel->click();
      } catch (...) { failure = std::current_exception(); review.reject(); }
    });
    QObject::connect(&deadline, &QTimer::timeout, &review, &QDialog::reject);
    drive.start(); deadline.start(5000); review.exec(); drive.stop(); deadline.stop();
    if (failure) std::rethrow_exception(failure);
    require(finished, "native Fade dialog did not finish its preview/preflight workflow");
  };
  auto verifyFade = [&]() {
    TVectorImageP image = level->getFrame(TFrameId(2),false);
    require(image->getStrokeCount() == 2 && image->getGroupDepth(1) == 1 &&
                std::abs(image->getStroke(1)->getOpacity() - .3) < 1.e-12 &&
                image->getStroke(1)->getOpacityProfile() == fadeProfile &&
                TVectorImageP(level->getFrame(TFrameId(1),false)) == fadeFirst &&
                TVectorImageP(level->getFrame(TFrameId(3),false)) == fadeLast &&
                fadeFirst->getStroke(1)->getOpacity() == .6,
            "native Fade Apply/redo lost authored appearance, groups or endpoints");
    requireGeometry(image,fadeFirst,"native Fade geometry");
  };
  fadeDialog(false,false);
  require(undo->getHistoryCount() == 0 && !app->getCurrentScene()->getDirtyFlag(),
          "canceling native Fade changed document/history");
  fadeDialog(true,false); verifyFade();
  require(undo->getHistoryCount() == 1, "Fade Apply did not create one undo transaction");
  undo->undo();
  require(TVectorImageP(level->getFrame(TFrameId(2),false)) == fadeBlank, "Fade undo lost original drawing identity");
  undo->redo(); verifyFade();
  fadeDialog(true,false); verifyFade();
  require(undo->getHistoryCount() == 2, "repeated Fade Apply changed transaction count");
  undo->undo(); verifyFade(); undo->undo();
  require(TVectorImageP(level->getFrame(TFrameId(2),false)) == fadeBlank, "repeated Fade undo lost original drawing");
  undo->reset();

  TVectorImageP painted = make({});
  auto *boundary = new TStroke(std::vector<TThickPoint>{{0,0,1},{5,0,1},{10,0,1},
      {10,5,1},{10,10,1},{5,10,1},{0,10,1},{0,5,1},{0,0,1}});
  boundary->setStyle(1); boundary->setSelfLoop(); painted->addStroke(boundary);
  painted->findRegions(); require(painted->getRegionCount() > 0,"native Fade painted fixture lacks region");
  painted->getRegion(0)->setStyle(1);
  for (bool appearing : {false,true}) {
    level->setFrame(TFrameId(1),appearing ? make({}) : painted);
    level->setFrame(TFrameId(3),appearing ? painted : make({}));
    app->getCurrentScene()->setDirtyFlag(false);
    level->getProperties()->setDirtyFlag(false);
    FilmstripCmd::inbetween(level,TFrameId(1),TFrameId(2),FilmstripCmd::II_Linear,{},options);
    require(undo->getHistoryCount() == 0 && !app->getCurrentScene()->getDirtyFlag(),
            "Fade without intermediate drawings created a phantom transaction");
    fadeDialog(false,true);
    bool refused = false;
    try { FilmstripCmd::inbetween(level,TFrameId(1),TFrameId(3),FilmstripCmd::II_Linear,{},options); }
    catch (...) { refused = true; }
    require(refused && undo->getHistoryCount() == 0 &&
                TVectorImageP(level->getFrame(TFrameId(2),false)) == fadeBlank &&
                !app->getCurrentScene()->getDirtyFlag() && !level->getProperties()->getDirtyFlag() &&
                painted->getRegion(0)->getStyle() == 1 &&
                painted->getStroke(0)->getOpacity() == 1.,
            "painted Fade failure changed artwork/history/dirty state");
  }
  morphCallerChecks();
  std::cout << "vector-morph-native: PASS" << std::endl;
}

void runVectorOpacityProfileNativeChecks() {
  auto *app = TApp::instance();
  auto *level = app->getCurrentScene()->getScene()->createNewLevel(
      PLI_XSHLEVEL, L"OpacityEnvelopeFixture")->getSimpleLevel();
  TPaletteP palette = new TPalette;
  level->setPalette(palette.getPointer());
  TVectorImageP image = new TVectorImage; image->setPalette(palette.getPointer());
  const StrokeOpacity::Profile firstProfile{{0.,.2},{1.,.8}};
  const StrokeOpacity::Profile secondProfile{{0.,1.},{1.,.4}};
  for (int i = 0; i < 2; ++i) {
    auto *stroke = new TStroke(std::vector<TThickPoint>{
        {0.,i*20.,2.},{25.,i*20.+10.,2.},{50.,i*20.,2.}});
    stroke->setStyle(1); stroke->setOpacity(i ? .8 : .3);
    stroke->setOpacityProfile(i ? secondProfile : firstProfile); image->addStroke(stroke);
  }
  level->setFrame(TFrameId(1), image);
  app->getCurrentXsheet()->getXsheet()->setCell(0,0,TXshCell(level,TFrameId(1)));
  app->getCurrentFrame()->setFrame(0); app->getCurrentColumn()->setColumnIndex(0);
  app->getCurrentLevel()->setLevel(level); app->getCurrentTool()->setTool("T_Selection");
  QCoreApplication::processEvents();
  auto *tool = app->getCurrentTool()->getTool();
  auto *mode = dynamic_cast<TEnumProperty *>(tool->getProperties(0)->getProperty(std::string("Mode:")));
  require(mode, "opacity editor selection mode unavailable");
  mode->setValue(L"Standard"); tool->onPropertyChanged("Mode:");
  auto *selection = dynamic_cast<StrokeSelection *>(tool->getSelection());
  require(selection, "opacity editor native selection unavailable");
  selection->selectNone(); selection->select(0,true); selection->select(1,true); selection->makeCurrent();
  auto *action = CommandManager::instance()->getAction(MI_EditStrokeOpacityProfile);
  require(action && action->isEnabled(), "opacity envelope command not registered and enabled");
  auto *undo = TUndoManager::manager(); undo->reset();
  auto interact = [&](bool apply) {
    bool saw = false, mixedGuard = false, clean = false, validStep = false, recoveredEndpoint = false;
    app->getCurrentScene()->setDirtyFlag(false);
    QTimer response; response.setSingleShot(true);
    QObject::connect(&response,&QTimer::timeout,[&]() {
      auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
      saw = dialog && dialog->objectName() == "strokeOpacityEditor";
      if (!saw) { if (dialog) dialog->reject(); return; }
      auto *applyButton = dialog->findChild<QPushButton *>("strokeOpacityApply");
      auto *fade = dialog->findChild<QPushButton *>("strokeOpacityFadeOut");
      auto *add = dialog->findChild<QPushButton *>("strokeOpacityAdd");
      auto *step = dialog->findChild<QPushButton *>("strokeOpacityStep");
      auto *remove = dialog->findChild<QPushButton *>("strokeOpacityRemove");
      auto *table = dialog->findChild<QTableView *>("strokeOpacityKnots");
      if (!applyButton || !fade || !add || !step || !remove || !table) { dialog->reject(); return; }
      mixedGuard = !applyButton->isEnabled();
      fade->click(); table->selectRow(1); remove->click();
      recoveredEndpoint = table->model()->rowCount() == 1 && !applyButton->isEnabled();
      add->click();
      recoveredEndpoint &= table->model()->rowCount() == 2 && applyButton->isEnabled();
      fade->click(); table->selectRow(0); add->click();
      table->selectRow(1); step->click();
      auto *model = table->model();
      validStep = model->rowCount() == 4 &&
          model->setData(model->index(2,1), .25, Qt::EditRole) && applyButton->isEnabled();
      clean = !app->getCurrentScene()->getDirtyFlag() && undo->getHistoryCount() == 0 &&
          image->getStroke(0)->getOpacityProfile() == firstProfile &&
          image->getStroke(1)->getOpacityProfile() == secondProfile;
      if (apply && validStep) applyButton->click(); else dialog->reject();
    });
    response.start(0); action->trigger();
    require(saw && mixedGuard && validStep && recoveredEndpoint,
            "native opacity editor failed mixed-profile, endpoint recovery or exact-step workflow");
    require(clean, "opacity editor preview changed document/history");
  };
  interact(false);
  require(undo->getHistoryCount() == 0 && !app->getCurrentScene()->getDirtyFlag() &&
      image->getStroke(0)->getOpacityProfile() == firstProfile &&
      image->getStroke(1)->getOpacityProfile() == secondProfile, "opacity editor Cancel changed artwork");
  interact(true);
  const StrokeOpacity::Profile expected{{0.,1.},{.5,.5},{.5,.25},{1.,0.}};
  require(undo->getHistoryCount() == 1 && image->getStroke(0)->getOpacityProfile() == expected &&
              image->getStroke(1)->getOpacityProfile() == expected,
          "opacity editor did not apply one profile transaction to both selected strokes");
  require(image->getStroke(0)->getOpacity() == .3 && image->getStroke(1)->getOpacity() == .8,
          "opacity envelope edit replaced per-stroke overall opacity");
  undo->undo();
  require(image->getStroke(0)->getOpacityProfile() == firstProfile &&
              image->getStroke(1)->getOpacityProfile() == secondProfile, "opacity profile undo lost mixed originals");
  undo->redo();
  require(image->getStroke(0)->getOpacityProfile() == expected &&
              image->getStroke(1)->getOpacityProfile() == expected, "opacity profile redo lost step knots");
  QTemporaryDir dir; require(dir.isValid(), "opacity workflow reload directory");
  const TFilePath path(dir.filePath("edited.pli"));
  { TLevelWriterP writer(path); writer->getFrameWriter(TFrameId(1))->save(image); }
  TLevelReaderP reader(path); reader->loadInfo();
  TVectorImageP reopened = reader->getFrameReader(TFrameId(1))->load();
  require(reopened && reopened->getStroke(0)->getOpacityProfile() == expected &&
              reopened->getStroke(1)->getOpacityProfile() == expected,
          "native opacity editor result lost exact step knots on PLI reopen");
  undo->reset();
  std::cout << "vector-opacity-profile-native: PASS" << std::endl;
}
