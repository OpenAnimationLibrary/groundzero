#include "mcpliveintegrationtest.h"

#include <QAction>
#include <QApplication>
#include <QCryptographicHash>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMainWindow>
#include <QPointer>
#include <QProcess>
#include <QSaveFile>
#include <QThread>
#include <QTimer>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <utility>

#include "automation_live_startup.h"
#include "menubarcommandids.h"
#include "nativeuiintegrationtest.h"
#include "tapp.h"
#include "tcolorstyles.h"
#include "tdoublekeyframe.h"
#include "tdoubleparam.h"
#include "tests/mcp_live_generation_test.h"
#include "tests/mcp_live_security_fixture.h"
#include "texception.h"
#include "tfx.h"
#include "toonz/doubleparamcmd.h"
#include "toonz/fxdag.h"
#include "toonz/levelset.h"
#include "toonz/preferences.h"
#include "toonz/tcamera.h"
#include "toonz/tcolumnfxset.h"
#include "toonz/tfxhandle.h"
#include "toonz/toonzscene.h"
#include "toonz/tscenehandle.h"
#include "toonz/tstageobject.h"
#include "toonz/tstageobjecttree.h"
#include "toonz/txsheet.h"
#include "toonz/txshleveltypes.h"
#include "toonz/txshsimplelevel.h"
#include "toonzqt/menubarcommand.h"
#include "tpalette.h"
#include "tparamcontainer.h"
#include "tparamset.h"
#include "trasterimage.h"
#include "tstream.h"
#include "tundo.h"

namespace {

using GroundzeroAutomation::Live::LiveUi;
using GroundzeroAutomation::Live::Options;

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

QString fixturePath(const QString& relative) {
  return QDir::current().filePath("mcp-live/" + relative);
}

QByteArray readFile(const QString& path) {
  QFile file(path);
  require(file.open(QIODevice::ReadOnly), "Live fixture evidence is missing");
  require(file.size() <= 1024 * 1024, "Live fixture evidence exceeds 1 MiB");
  return file.readAll();
}

QJsonObject readObject(const QString& path) {
  QJsonParseError error;
  const auto value = QJsonDocument::fromJson(readFile(path), &error);
  require(error.error == QJsonParseError::NoError && value.isObject(),
          "Live fixture configuration must be one JSON object");
  return value.object();
}

void writeObject(const QString& path, const QJsonObject& value) {
  QSaveFile file(path);
  const QByteArray bytes = QJsonDocument(value).toJson(QJsonDocument::Indented);
  require(file.open(QIODevice::WriteOnly), "Cannot retain live fixture evidence");
  require(file.write(bytes) == bytes.size() && file.commit(),
          "Cannot commit live fixture evidence");
}

QString configuredFile(const QJsonObject& settings, const QString& key) {
  const QString path = settings.value(key).toString();
  const QFileInfo info(path);
  require(info.isAbsolute() && info.isFile() && !info.isSymLink(),
          "Live fixture requires explicit regular executable/script paths");
  return info.canonicalFilePath();
}

void setKeys(TDoubleParam* parameter, double first, double last) {
  parameter->clearKeyframes();
  for (const auto& entry : {std::pair<int, double>{0, first}, {10, last}}) {
    TDoubleKeyframe key(entry.first, entry.second);
    key.m_type = TDoubleKeyframe::Linear;
    key.m_prevType = TDoubleKeyframe::Linear;
    key.m_isKeyframe = true;
    key.m_step = 1;
    parameter->setKeyframe(key);
  }
}

TFxP makeBlur(ToonzScene* scene, const std::wstring& name) {
  std::unique_ptr<TPersist> created(TPersist::create("STD_blurFx"));
  auto* effect = dynamic_cast<TFx*>(created.get());
  require(effect && effect->getFxType() == "STD_blurFx", "Native Blur factory is unavailable");
  TFxP result(effect);
  created.release();
  effect->setName(name);
  auto* graph = scene->getXsheet()->getFxDag();
  graph->assignUniqueId(effect);
  effect->setNewIdentifier();
  graph->getInternalFxs()->addFx(effect);
  return result;
}

TDoubleParam* blurParameter(TFx* effect) {
  auto* parameter = dynamic_cast<TDoubleParam*>(effect->getParams()->getParam("value"));
  require(parameter, "Native Blur value parameter is not a scalar");
  return parameter;
}

QJsonObject parameterState(TDoubleParam* parameter, const QString& label) {
  const QString path = fixturePath(label + ".curve");
  {
    TOStream stream(TFilePath(path.toStdWString()));
    require(bool(stream), "Cannot serialize the native curve oracle");
    parameter->saveData(stream);
  }
  QJsonArray keys;
  for (int index = 0; index < parameter->getKeyframeCount(); ++index) {
    const auto& key = parameter->getKeyframe(index);
    keys.append(QJsonArray{key.m_frame, key.m_value});
  }
  return {{"default", parameter->getDefaultValue()},
          {"editor_locked", parameter->isEditorLocked()},
          {"cycle", parameter->isCycleEnabled()},
          {"keys", keys},
          {"midpoint", parameter->getValue(5)},
          {"serialized_sha256",
           QString::fromLatin1(
               QCryptographicHash::hash(readFile(path), QCryptographicHash::Sha256).toHex())}};
}

void nativeCommand(const char* command) {
  QAction* action = CommandManager::instance()->getAction(command, false);
  require(action && action->isEnabled(), "Native undo/redo action is unavailable");
  action->trigger();
}

QJsonArray rgba(const TPixel32& pixel) {
  return {int(pixel.r), int(pixel.g), int(pixel.b), int(pixel.m)};
}

class LiveFixture final : public QObject {
  QMainWindow* m_window;
  std::function<void(int)> m_completed;
  std::unique_ptr<LiveUi> m_live;
  std::unique_ptr<QDialog> m_modal;
  QProcess m_client;
  QTimer m_deadline;
  QByteArray m_pending;
  QString m_session;
  QString m_error;
  QJsonArray m_phases;
  QMetaObject::Connection m_historyStop;
  bool m_finished = false;
  bool m_historyStopObserved = false;
  bool m_shutdownRequested = false;
  bool m_afterExit = false;
  int m_result = 1;
  ToonzScene* m_scene = nullptr;
  std::shared_ptr<void> m_lease;
  TStageObjectP m_camera, m_peg;
  TFxP m_blur, m_keyedBlur;
  TDoubleParamP m_previousBlurParameter;
  TXshSimpleLevelP m_level;
  TRasterImageP m_image;

public:
  LiveFixture(QMainWindow* window, std::function<void(int)> completed)
      : QObject(window), m_window(window), m_completed(std::move(completed)) {}

  void start() {
    guard([this]() {
      require(!qEnvironmentVariableIsSet("GROUNDZERO_ARTIST_BENCHMARK"),
              "Live diagnostic suite cannot select the artist benchmark");
      QString error;
      require(resetNativeUiIntegrationScene(&error), error.toUtf8().constData());
      runNativeGenerationTests();
      require(resetNativeUiIntegrationScene(&error), error.toUtf8().constData());
      prepareScene();
      const QJsonObject settings = readObject(fixturePath("config.json"));
      m_session = settings.value("session").toString();
      require(QFileInfo(m_session).isAbsolute(), "Live fixture grant path is not absolute");
      prepareMcpLivePrivateDirectory(QFileInfo(m_session).absolutePath());
      restart("edit");
      launch(settings);
    });
  }

  int afterApplicationExit() {
    m_afterExit = true;
    if (m_finished) return m_result;
    if (!m_shutdownRequested) m_error = "Application exited before the fixed shutdown phase";
    if (m_client.state() != QProcess::NotRunning && !m_client.waitForFinished(10000)) {
      m_error = "SDK client did not observe active GUI shutdown within 10 seconds";
      m_client.kill();
      if (!m_client.waitForFinished(3000)) return 1;
    }
    if (!m_finished) finish(m_client.exitCode(), m_client.exitStatus());
    return m_result;
  }

private:
  void guard(const std::function<void()>& action) {
    try {
      action();
    } catch (const TException& error) {
      fail(QString::fromStdWString(error.getMessage()));
    } catch (const std::exception& error) {
      fail(QString::fromUtf8(error.what()));
    } catch (...) {
      fail("Unexpected native exception in live fixture");
    }
  }

  void prepareScene() {
    Preferences::instance()->setValue(pixelsOnly, false);
    auto* handle = TApp::instance()->getCurrentScene();
    m_scene = handle->getScene();
    m_lease = handle->retainScene();
    auto* tree = m_scene->getXsheet()->getStageObjectTree();
    m_camera = tree->getStageObject(TStageObjectId::CameraId(0), false);
    m_peg = tree->getStageObject(TStageObjectId::PegbarId(0));
    require(bool(m_camera) && bool(m_peg), "Native camera/peg fixture is missing");
    m_peg->setName("Live fixture peg");
    setKeys(m_camera->getParam(TStageObject::T_X), 0, 1);
    setKeys(m_peg->getParam(TStageObject::T_X), 2, 4);
    m_blur = makeBlur(m_scene, L"Live default blur");
    m_keyedBlur = makeBlur(m_scene, L"Live keyed blur");
    blurParameter(m_blur.getPointer())->setDefaultValue(2);
    auto* keyed = blurParameter(m_keyedBlur.getPointer());
    require(keyed->setValue(0, 2) && keyed->setValue(10, 6),
            "Native Blur fixture did not create its existing scalar keys");
    prepareArtwork();
    TApp::instance()->getCurrentFx()->setFx(nullptr);
    TUndoManager::manager()->reset();
    handle->setDirtyFlag(false);
  }

  void prepareArtwork() {
    m_level = new TXshSimpleLevel(L"Excluded artwork content");
    m_level->setScene(m_scene);
    m_level->setType(OVL_XSHLEVEL);
    m_level->setPath(TFilePath(fixturePath("excluded..png").toStdWString()));
    m_level->setPalette(new TPalette());
    TRaster32P raster(2, 2);
    raster->clear();
    m_image = new TRasterImage(raster);
    m_image->setDpi(120, 120);
    m_level->setFrame(TFrameId(1), m_image);
    m_scene->getLevelSet()->insertLevel(m_level.getPointer());
  }

  QJsonObject nativeState() {
    auto* current = TApp::instance()->getCurrentScene();
    auto* history = TUndoManager::manager();
    return {{"scene_present", current->getScene() != nullptr},
            {"original_scene_current", current->getScene() == m_scene},
            {"history_count", history->getHistoryCount()},
            {"history_index", history->getCurrentHistoryIndex()},
            {"dirty", current->getDirtyFlag()},
            {"camera_locked", m_camera->isLocked()},
            {"camera_width_in", m_camera->getCamera()->getSize().lx},
            {"art_pixel", rgba(TRaster32P(m_image->getRaster())->pixels(0)[0])},
            {"art_palette", rgba(m_level->getPalette()->getStyle(1)->getMainColor())},
            {"camera_x", parameterState(m_camera->getParam(TStageObject::T_X), "camera")},
            {"peg_x", parameterState(m_peg->getParam(TStageObject::T_X), "peg")},
            {"blur", parameterState(blurParameter(m_blur.getPointer()), "blur")},
            {"keyed_blur", parameterState(blurParameter(m_keyedBlur.getPointer()), "keyed-blur")}};
  }

  void restart(const QString& mode) {
    if (m_live) m_live->stop();
    m_live.reset();
    m_live = std::make_unique<LiveUi>(*qApp, *m_window, Options{mode, m_session});
    m_live->start();
    require(m_live->running(), "Production LiveUi did not start its service");
  }

  void launch(const QJsonObject& settings) {
    m_client.setProgram(configuredFile(settings, "node"));
    m_client.setArguments({configuredFile(settings, "client"), "--executable",
                           QCoreApplication::applicationFilePath(), "--adapter",
                           configuredFile(settings, "adapter"), "--session", m_session,
                           "--evidence", fixturePath("")});
    m_client.setStandardErrorFile(fixturePath("sdk.stderr.log"));
    connect(&m_client, &QProcess::readyReadStandardOutput, this,
            [this]() { guard([this]() { readPhases(); }); });
    connect(&m_client, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus status) { finish(code, status); });
    connect(&m_client, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
      if (error == QProcess::FailedToStart) fail("Official SDK fixture did not start");
    });
    connect(&m_deadline, &QTimer::timeout, this,
            [this]() { fail("Live fixture exceeded its 120-second deadline"); });
    m_deadline.setSingleShot(true);
    m_deadline.start(120000);
    m_client.start();
  }

  void readPhases() {
    m_pending += m_client.readAllStandardOutput();
    if (m_shutdownRequested) {
      require(m_pending.isEmpty(), "SDK fixture requested a native phase after GUI shutdown");
      return;
    }
    require(m_pending.size() <= 65536, "SDK fixture phase exceeds 64 KiB");
    int newline = -1;
    while ((newline = m_pending.indexOf('\n')) >= 0) {
      const QByteArray line = m_pending.left(newline);
      m_pending.remove(0, newline + 1);
      QJsonParseError error;
      const auto request = QJsonDocument::fromJson(line, &error);
      require(error.error == QJsonParseError::NoError && request.isObject(),
              "SDK fixture phase is not valid JSON");
      phase(request.object());
    }
  }

  void humanStageEdit() {
    auto* parameter = m_peg->getParam(TStageObject::T_X);
    KeyframeSetter edit(parameter, parameter->getClosestKeyframe(10));
    edit.setValue(parameter->getValue(10) + 0.25);
  }

  void reinsertPeg() {
    auto* tree = m_scene->getXsheet()->getStageObjectTree();
    tree->removeStageObject(m_peg->getId());
    tree->insertStageObject(m_peg.getPointer());
  }

  void reinsertFx() {
    auto* effects = m_scene->getXsheet()->getFxDag()->getInternalFxs();
    effects->removeFx(m_blur.getPointer());
    effects->addFx(m_blur.getPointer());
  }

  void suppressedFxEdit() {
    auto* container = m_blur->getParams();
    auto* observer = container->getParamObserver();
    container->setParamObserver(nullptr);
    auto* parameter = blurParameter(m_blur.getPointer());
    try {
      parameter->setDefaultValue(parameter->getDefaultValue() + 0.5);
    } catch (...) {
      container->setParamObserver(observer);
      throw;
    }
    container->setParamObserver(observer);
  }

  void resizeCamera() {
    auto* camera = m_camera->getCamera();
    const auto size = camera->getSize();
    camera->setSize(TDimensionD(size.lx + 0.125, size.ly));
  }

  void replaceFxParameter() {
    m_previousBlurParameter = blurParameter(m_blur.getPointer());
    m_blur->getParams()->unlink();
    require(blurParameter(m_blur.getPointer()) != m_previousBlurParameter.getPointer(),
            "Native parameter unlink did not replace the equal-valued parameter");
  }

  void artworkEdit() {
    TRaster32P raster = m_image->getRaster();
    raster->pixels(0)[0] = TPixel32(211, 31, 77, 255);
    m_level->touchFrame(TFrameId(1));
    m_level->getPalette()->getStyle(1)->setMainColor(TPixel32(17, 149, 233, 255));
  }

  void replaceScene() {
    QString error;
    require(resetNativeUiIntegrationScene(&error), error.toUtf8().constData());
    require(TApp::instance()->getCurrentScene()->getScene() != m_scene,
            "Native New Scene retained the original scene pointer");
  }

  void stopAction() {
    auto* action = m_window->findChild<QAction*>("automationLiveStop");
    require(action && action->isEnabled(), "Production Stop Live Automation action is absent");
    action->trigger();
    require(!m_live->running(), "Production Stop action left the service running");
  }

  void armHistoryStop() {
    require(!m_historyStopObserved, "The fixed history Stop case already ran");
    require(m_live && m_live->running(), "History Stop requires an active live grant");
    QObject::disconnect(m_historyStop);
    m_historyStop = connect(
        TUndoManager::manager(), &TUndoManager::historyChanged, this,
        [this]() {
          QObject::disconnect(m_historyStop);
          guard([this]() {
            require(TUndoManager::manager()->isBusy(), "Stop did not run inside native history");
            stopAction();
            m_historyStopObserved = true;
          });
        },
        Qt::DirectConnection);
  }

  std::map<QString, std::function<void()>> actions() {
    return {
        {"snapshot", []() {}},
        {"human-undo", []() { nativeCommand(MI_Undo); }},
        {"human-redo", []() { nativeCommand(MI_Redo); }},
        {"human-stage-edit", [this]() { humanStageEdit(); }},
        {"history-reset", []() { TUndoManager::manager()->reset(); }},
        {"peg-reinsert", [this]() { reinsertPeg(); }},
        {"fx-reinsert", [this]() { reinsertFx(); }},
        {"fx-suppressed-edit", [this]() { suppressedFxEdit(); }},
        {"fx-parameter-replace", [this]() { replaceFxParameter(); }},
        {"camera-size-edit", [this]() { resizeCamera(); }},
        {"camera-lock", [this]() { m_camera->lock(true); }},
        {"camera-unlock", [this]() { m_camera->lock(false); }},
        {"curve-lock", [this]() { m_camera->getParam(TStageObject::T_X)->setEditorLocked(true); }},
        {"curve-unlock",
         [this]() { m_camera->getParam(TStageObject::T_X)->setEditorLocked(false); }},
        {"cycle-enable", [this]() { m_camera->getParam(TStageObject::T_X)->enableCycle(true); }},
        {"cycle-disable", [this]() { m_camera->getParam(TStageObject::T_X)->enableCycle(false); }},
        {"excluded-artwork-edit", [this]() { artworkEdit(); }},
        {"scene-null", []() { TApp::instance()->getCurrentScene()->setScene(nullptr); }},
        {"scene-replace", [this]() { replaceScene(); }},
        {"scene-reinstate", [this]() { TApp::instance()->getCurrentScene()->setScene(m_scene); }},
        {"inspect-mode", [this]() { restart("inspect"); }},
        {"edit-mode", [this]() { restart("edit"); }},
        {"restart", [this]() { restart("edit"); }},
        {"stop", [this]() { stopAction(); }},
        {"arm-stop-on-history", [this]() { armHistoryStop(); }},
        {"modal-open",
         [this]() {
           m_modal = std::make_unique<QDialog>(m_window);
           m_modal->setModal(true);
           m_modal->show();
         }},
        {"modal-close", [this]() { m_modal.reset(); }},
        {"security-files", []() {}},
        {"security-files-cleanup", []() { removeMcpLiveInsecureFiles(); }},
        {"admission-timeout", []() {}},
        {"shutdown", []() {}}};
  }

  void phase(const QJsonObject& request) {
    require(
        request.size() == 2 && request.value("id").isString() && request.value("phase").isString(),
        "Unknown SDK fixture phase schema");
    require(m_phases.size() < 128, "Live fixture exceeded 128 native phases");
    const QString name = request.value("phase").toString();
    const auto handlers = actions();
    const auto found = handlers.find(name);
    require(found != handlers.end(), "Unknown fixed native live phase");
    found->second();
    QJsonObject result{{"session", m_session},
                       {"native", nativeState()},
                       {"stop_during_history", m_historyStopObserved}};
    if (name == "security-files") result["files"] = createMcpLiveInsecureFiles(m_session);
    m_phases.append(QJsonObject{{"phase", name}, {"native", result.value("native")}});
    const QByteArray response =
        QJsonDocument(QJsonObject{{"id", request.value("id")}, {"result", result}})
            .toJson(QJsonDocument::Compact) +
        '\n';
    require(m_client.write(response) == response.size(), "Cannot reply to SDK fixture phase");
    afterPhase(name);
  }

  void afterPhase(const QString& name) {
    if (name != "admission-timeout" && name != "shutdown") return;
    require(m_client.bytesToWrite() == 0 || m_client.waitForBytesWritten(1000),
            "Cannot flush fixed phase acknowledgment");
    if (name == "admission-timeout") {
      QThread::msleep(3000);
      return;
    }
    require(m_live && m_live->running(), "GUI shutdown requires an active native grant");
    m_shutdownRequested = true;
    qApp->quit();
  }

  bool onGuiLoop() const { return !m_shutdownRequested && !m_afterExit; }

  void fail(const QString& message) {
    if (m_finished) return;
    if (m_error.isEmpty()) m_error = message.left(2048);
    if (onGuiLoop() && m_live) m_live->stop();
    if (m_client.state() == QProcess::NotRunning)
      finish(-1, QProcess::CrashExit);
    else
      m_client.kill();
  }

  bool succeeded(int code, QProcess::ExitStatus status) const {
    return m_error.isEmpty() && code == 0 && status == QProcess::NormalExit;
  }

  void retainResult(int code, QProcess::ExitStatus status) {
    try {
      removeMcpLiveInsecureFiles();
      writeObject(fixturePath("native.json"),
                  {{"status", succeeded(code, status) ? "passed" : "failed"},
                   {"error", m_error},
                   {"sdk_exit_code", code},
                   {"sdk_exit_status", status == QProcess::NormalExit ? "normal" : "crashed"},
                   {"service_running", m_live && m_live->running()},
                   {"active_shutdown", m_shutdownRequested},
                   {"stop_during_history", m_historyStopObserved},
                   {"phases", m_phases}});
    } catch (const std::exception& error) {
      std::cerr << "mcp-live-native: evidence failure: " << error.what() << std::endl;
      m_error = "Cannot retain independent native evidence";
    }
  }

  void finish(int code, QProcess::ExitStatus status) {
    if (m_finished) return;
    m_finished = true;
    m_deadline.stop();
    if (!m_pending.isEmpty()) m_error = "SDK fixture ended during a phase frame";
    if (onGuiLoop()) closeGuiOwners();
    retainResult(code, status);
    m_result = succeeded(code, status) ? 0 : 1;
    std::cout << "mcp-live-native: " << (m_result == 0 ? "PASS" : "FAIL") << std::endl;
    if (onGuiLoop()) m_completed(m_result);
  }

  void closeGuiOwners() {
    QObject::disconnect(m_historyStop);
    if (m_live) m_live->stop();
    m_live.reset();
    m_modal.reset();
  }
};

QPointer<LiveFixture> activeFixture;

}  // namespace

void startMcpLiveIntegrationTest(QMainWindow* window, std::function<void(int)> completed) {
  auto* fixture = new LiveFixture(window, std::move(completed));
  activeFixture = fixture;
  QTimer::singleShot(0, fixture, [fixture]() { fixture->start(); });
}

int finishMcpLiveIntegrationTest() {
  if (!activeFixture) {
    std::cerr << "mcp-live-native: fixed fixture is missing after application exit" << std::endl;
    return 1;
  }
  return activeFixture->afterApplicationExit();
}
