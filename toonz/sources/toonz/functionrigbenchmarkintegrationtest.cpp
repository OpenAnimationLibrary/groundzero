#include "functionrigbenchmarkintegrationtest.h"
#include "tapp.h"
#include "toonz/toonzscene.h"
#include "toonz/tscenehandle.h"
#include "toonz/txsheet.h"
#include "toonz/txsheethandle.h"
#include "toonz/tstageobject.h"
#include "toonz/tstageobjecttree.h"
#include "toonzqt/functioncurveoperations.h"
#include "toonzqt/functionpanel.h"
#include "toonzqt/functionselection.h"
#include "toonzqt/functiontreeviewer.h"
#include "toonzqt/functionviewer.h"
#include "tunit.h"
#include "tundo.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSysInfo>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool condition, const char *reason) {
  if (!condition) throw std::runtime_error(reason);
}
double displayOracle(int channel, double frame) {
  const int left = int(std::floor(frame));
  const double fraction = frame - left;
  return channel * 4. + .04 * ((1 - fraction) * (left % 16) + fraction * ((left + 1) % 16));
}
double nativeValue(TDoubleParam *curve, double display) {
  const auto *measure = curve->getMeasure();
  return measure && measure->getCurrentUnit() ? measure->getCurrentUnit()->convertFrom(display) : display;
}
void settle() {
  QApplication::sendPostedEvents();
  QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
}
QImage render(FunctionPanel *graph) {
  QImage image(graph->size(), QImage::Format_ARGB32_Premultiplied);
  image.fill(Qt::transparent); graph->render(&image); return image;
}
}  // namespace

void runFunctionRigBenchmarkIntegrationTest(FunctionViewer *viewer) {
  std::cerr << "function-workflow-native: multi-owner rig graph benchmark" << std::endl;
  auto *app = TApp::instance();
  auto *scene = app->getCurrentScene()->getScene();
  auto *sheet = app->getCurrentXsheet()->getXsheet();
  auto *tree = viewer->findChild<FunctionTreeView *>();
  auto *graph = viewer->findChild<FunctionPanel *>();
  require(scene && sheet && tree && graph, "Rig graph benchmark has no native scene/editor");
  constexpr int owners = 24, channelsPerOwner = 4, keyCount = 256;
  std::vector<TDoubleParamP> curves, originals;
  for (int owner = 0; owner < owners; ++owner) {
    const TStageObjectId id = TStageObjectId::PegbarId(100 + owner);
    require(!sheet->getStageObjectTree()->getStageObject(id, false), "Rig benchmark owner ID already exists");
    auto *object = sheet->getStageObjectTree()->getStageObject(id);
    object->setName("Graph benchmark rig " + std::to_string(owner));
    if (owner) {
      const auto parent = TStageObjectId::PegbarId(100 + owner - 1);
      object->setParent(parent);
      require(object->getParent() == parent, "Rig benchmark did not preserve its native parent chain");
    }
    object->insertOffsetLayer(0, object->createOffsetLayer(L"Graph benchmark offset"));
    for (auto *curve : {object->getParam(TStageObject::T_X), object->getParam(TStageObject::T_Y),
                       object->getOffsetParam(0, TStageObject::T_X), object->getOffsetParam(0, TStageObject::T_Y)}) {
      require(curve, "Rig benchmark offset channel is missing");
      const int channel = int(curves.size());
      curve->setWorkspaceId("rig-benchmark-" + std::to_string(channel));
      for (int key = 0; key < keyCount; ++key) {
        TDoubleKeyframe value(key, nativeValue(curve, displayOracle(channel, key)));
        value.m_type = TDoubleKeyframe::Linear; value.m_prevType = TDoubleKeyframe::Linear;
        curve->setKeyframe(value);
      }
      curves.emplace_back(curve); originals.emplace_back(new TDoubleParam(*curve));
    }
  }
  app->getCurrentXsheet()->notifyXsheetChanged(); settle();
  std::map<TDoubleParam *, FunctionTreeModel::Channel *> channels;
  for (auto *channel : tree->channels()) {
    const bool included = std::find(curves.begin(), curves.end(), TDoubleParamP(channel->getParam())) != curves.end();
    channel->setIsActive(included);
    if (included) channels.emplace(channel->getParam(), channel);
  }
  require(channels.size() == owners * channelsPerOwner, "Native graph omitted a rig owner/offset channel");
  channels.at(curves.front().getPointer())->setIsCurrent(true);
  viewer->getSelection()->selectNone();
  viewer->getSelection()->select(curves.front().getPointer(), 101);
  viewer->getSelection()->select(curves.front().getPointer(), 102);
  struct RestoreMinimum {
    FunctionPanel *graph;
    QSize minimum;
    ~RestoreMinimum() { graph->setMinimumSize(minimum); }
  } restoreMinimum{graph, graph->minimumSize()};
  graph->setComparison({}); graph->setNormalized(false); graph->setMinimumSize(1000, 960);
  graph->resize(1000, 960); graph->show(); settle();
  graph->fitRegion(100, -2, 140, curves.size() * 4. + 2);
  app->getCurrentScene()->setDirtyFlag(false); TUndoManager::manager()->reset();
  const QImage full = render(graph); // Warm graph/font caches before measuring.
  constexpr double probeFrame = 137.25;
  QJsonArray probes;
  for (int index : {1, 32, 64, 95}) {
    auto *curve = curves[index].getPointer();
    const double expected = nativeValue(curve, displayOracle(index, probeFrame));
    require(std::abs(curve->getValue(probeFrame) - expected) < 1.e-8,
            "Rig graph source differs from independent piecewise-linear oracle");
    const QPoint point(qRound(graph->frameToX(probeFrame)), qRound(graph->valueToY(curve, expected)));
    require(full.rect().adjusted(4, 4, -4, -4).contains(point) && point.x() > full.width() * .6,
            "Rig curve probe lies outside its visible plot or inside the legend");
    channels.at(curve)->setIsActive(false);
    const QImage absent = render(graph);
    channels.at(curve)->setIsActive(true);
    int changed = 0;
    for (int y = point.y() - 3; y <= point.y() + 3; ++y)
      for (int x = point.x() - 3; x <= point.x() + 3; ++x) {
        const QColor a = full.pixelColor(x, y), b = absent.pixelColor(x, y);
        if (std::max({std::abs(a.red() - b.red()), std::abs(a.green() - b.green()),
                      std::abs(a.blue() - b.blue())}) > 10) ++changed;
      }
    require(changed > 0, "Removing a rig channel did not remove artwork at its independent curve coordinate");
    probes.append(QJsonObject{{"channel", index}, {"changed_curve_pixels", changed}});
  }
  QJsonArray samples;
  QElapsedTimer timer;
  for (int paint = 0; paint < 6; ++paint) {
    timer.start(); const QImage image = render(graph);
    samples.append(timer.nsecsElapsed() / 1000000.);
    require(image == full, "Repeated large-rig graph paints changed visible curve output");
  }
  require(viewer->getSelection()->getSelectedKeyframeCount() == 2 &&
              viewer->getSelection()->isSelected(curves.front().getPointer(), 101) &&
              viewer->getSelection()->isSelected(curves.front().getPointer(), 102),
          "Large-rig rendering changed the selected keys");
  for (size_t index = 0; index < curves.size(); ++index)
    require(FunctionCurveOperations::matchesSnapshot(curves[index].getPointer(), originals[index].getPointer()) &&
                curves[index]->getWorkspaceId() == "rig-benchmark-" + std::to_string(index),
            "Large-rig rendering changed native keys or owner identities");
  require(!app->getCurrentScene()->getDirtyFlag() && TUndoManager::manager()->getHistoryCount() == 0,
          "Large-rig rendering dirtied the scene or created undo history");
  const QJsonObject report{{"fixture", "native-multi-owner-rig-graph-v1"}, {"owners", owners},
      {"channels", int(curves.size())}, {"keys", int(curves.size()) * keyCount},
      {"fit_request_first", 100}, {"fit_request_last", 140}, {"width", graph->width()}, {"height", graph->height()},
      {"measurement", "native QWidget graph paint into QImage; compositor and input latency excluded"},
      {"device_pixel_ratio", graph->devicePixelRatioF()}, {"qt", QString::fromLatin1(qVersion())},
      {"platform", QGuiApplication::platformName()}, {"os", QSysInfo::productType()},
      {"architecture", QSysInfo::buildCpuArchitecture()}, {"paint_ms", samples}, {"curve_probes", probes}};
  std::cout << "function-rig-graph-metrics: " << QJsonDocument(report).toJson(QJsonDocument::Compact).constData() << std::endl;
}
