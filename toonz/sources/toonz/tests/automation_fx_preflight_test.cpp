#include "automation_jobs.h"
#include "automation.h"
#include "automation_dispatch.h"
#include "automation_fx.h"
#include "automation_fx_profile.h"
#include "tdoublekeyframe.h"
#include "tdoubleparam.h"
#include "tfx.h"
#include "tfxattributes.h"
#include "tparamcontainer.h"
#include "tparamset.h"
#include "traster.h"
#include "trasterfx.h"
#include "trenderer.h"
#include "toonz/fxdag.h"
#include "toonz/scenefx.h"
#include "toonz/tcamera.h"
#include "toonz/tcolumnfx.h"
#include "toonz/tcolumnfxset.h"
#include "toonz/toonzscene.h"
#include "toonz/tstageobject.h"
#include "toonz/tstageobjecttree.h"
#include "toonz/txsheet.h"

#include <QEventLoop>
#include <QTemporaryDir>
#include <QTimer>

#include <cmath>
#include <functional>
#include <set>
#include <stdexcept>
#include <vector>

using namespace GroundzeroAutomation;

namespace {
void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

QJsonObject call(Context& context, Document& document, const QString& method,
                 QJsonObject params) {
  params["document_id"] = document.id;
  return executeFx(context, method, params).toObject();
}

TFx* create(Context& context, Document& document, const QString& type) {
  return document.fx(call(context, document, "fx.create",
                          {{"type", type}, {"frame_count", 13}})
                         .value("fx_id")
                         .toString());
}

TFx* native(TFx* fx) {
  auto* wrapper = dynamic_cast<TZeraryColumnFx*>(fx);
  return wrapper ? wrapper->getZeraryFx() : fx;
}

void output(Document& document, TFx* fx) {
  document.scene->getXsheet()
      ->getFxDag()
      ->getCurrentOutputFx()
      ->getInputPort(0)
      ->setFx(fx);
}

void rejects(Context& context, const std::function<void()>& action,
             const QString& code, QJsonObject* details = nullptr) {
  context.resetMutationStarted();
  try {
    action();
  } catch (const Error& error) {
    require(error.code == code, "Finishing rejection code changed");
    require(!context.mutationStarted(),
            "A finishing rejection began a mutation");
    if (details) *details = error.details;
    return;
  }
  throw std::runtime_error("Finishing invalid state was accepted");
}

Document& scene(Context& context, int width, int height) {
  auto& document = context.addDocument(std::make_unique<ToonzScene>());
  TCamera* camera =
      document.scene->getXsheet()->getStageObjectTree()->getCurrentCamera();
  camera->setRes(TDimension(width, height));
  camera->setSize(TDimensionD(1.2, 0.9));
  return document;
}

TDoubleParam* number(TFx* fx, const char* name) {
  auto* param =
      dynamic_cast<TDoubleParam*>(native(fx)->getParams()->getParam(name));
  require(param, "Finishing parameter is missing");
  return param;
}

void key(TDoubleParam* param, double frame, double value) {
  TDoubleKeyframe keyframe(frame, value);
  keyframe.m_isKeyframe = true;
  keyframe.m_prevType   = TDoubleKeyframe::Linear;
  param->setKeyframe(keyframe);
}

// Records the affine that the native host hands an effect while rendering.
class AffineProbeFx final : public TRasterFx {
  TRasterFxPort m_input;

public:
  enum class Mode { Scale, Full, Isotropic };
  static Mode mode;
  static std::vector<TAffine> received;

  AffineProbeFx() { addInputPort("Source", m_input); }
  const TPersistDeclaration* getDeclaration() const override;
  std::string getPluginId() const override { return std::string(); }
  bool canHandle(const TRenderSettings& info, double) override {
    if (mode == Mode::Isotropic) return info.m_affine.isIsotropic(0.001);
    return mode == Mode::Full;
  }
  bool doGetBBox(double frame, TRectD& box,
                 const TRenderSettings& info) override {
    return m_input.isConnected() && m_input->doGetBBox(frame, box, info);
  }
  void doCompute(TTile& tile, double frame,
                 const TRenderSettings& info) override {
    received.push_back(info.m_affine);
    if (m_input.isConnected()) m_input->compute(tile, frame, info);
  }
};
AffineProbeFx::Mode AffineProbeFx::mode = AffineProbeFx::Mode::Scale;
std::vector<TAffine> AffineProbeFx::received;

// Registered on first use only, so ordinary workers never see this type.
const TPersistDeclaration* AffineProbeFx::getDeclaration() const {
  static const TFxDeclarationT<AffineProbeFx> declaration(
      TFxInfo("gzAutomationAffineProbeFx", true));
  return &declaration;
}

TAffine received(ToonzScene& scene, int frame) {
  AffineProbeFx::received.clear();
  TRenderer renderer(1);
  const auto id = TRenderer::buildRenderId();
  renderer.install(id);
  renderer.declareRenderStart(id);
  renderer.declareFrameStart(frame);
  // The worker's render path: camera DPI applied unless exactly identity.
  TRasterFxP fx = buildSceneFx(&scene, double(frame), 1, false);
  require(bool(fx), "Probe scene produced no effect");
  const TDimension res = scene.getCurrentCamera()->getRes();
  TTile tile;
  TRenderSettings settings;
  settings.m_bpp = 32;
  fx->allocateAndCompute(tile, TPointD(-res.lx / 2.0, -res.ly / 2.0), res,
                         TRaster32P(1, 1), frame, settings);
  renderer.declareFrameEnd(frame);
  renderer.declareRenderEnd(id);
  renderer.uninstall();
  require(!AffineProbeFx::received.empty(), "Probe effect was not computed");
  return AffineProbeFx::received.front();
}

double spectralNorm(const TAffine& a) {
  const double squares =
      a.a11 * a.a11 + a.a12 * a.a12 + a.a21 * a.a21 + a.a22 * a.a22;
  const double det = a.det();
  return std::sqrt(
      (squares + std::sqrt(std::max(0.0, squares * squares - 4 * det * det))) /
      2);
}

TStageObject* object(Document& document, TStageObjectId id) {
  return document.scene->getXsheet()->getStageObject(id);
}

void setChannel(TStageObject* target, TStageObject::Channel channel,
                double value) {
  target->getParam(channel)->setDefaultValue(value);
  target->invalidate();
}

// The bound must hold for the affine the effect actually receives, and be
// exact when only one placement can reach the probe. The tolerance covers
// the different rounding of the native spectral norm.
void requireBound(Document& document, TFx* probe, int frame, bool exact) {
  const TDimension res = document.scene->getCurrentCamera()->getRes();
  const double bound =
      automationFxRenderScale(*document.scene, probe, res.lx, res.ly, frame);
  const double actual = spectralNorm(received(*document.scene, frame));
  require(actual <= bound * (1 + 1e-7),
          "Preflight scale is below the affine the native host hands an FX");
  require(!exact || std::fabs(actual - bound) <= bound * 1e-7,
          "Preflight scale is not the native render scale");
}

TFx* insertProbe(Document& document, TFx* source) {
  TFxP probe   = new AffineProbeFx;
  FxDag* graph = document.scene->getXsheet()->getFxDag();
  graph->assignUniqueId(probe.getPointer());
  graph->getInternalFxs()->addFx(probe.getPointer());
  probe->getInputPort(0)->setFx(source);
  output(document, probe.getPointer());
  return probe.getPointer();
}

void columnPlacements(Context& context) {
  auto& document = scene(context, 64, 48);
  TFx* card      = create(context, document, "colorCardFx");
  TFx* probe     = insertProbe(document, card);
  for (auto mode : {AffineProbeFx::Mode::Scale, AffineProbeFx::Mode::Full}) {
    AffineProbeFx::mode = mode;
    requireBound(document, probe, 0, true);
  }
  TStageObject* column = object(document, TStageObjectId::ColumnId(0));
  setChannel(column, TStageObject::T_ScaleX, 3);
  setChannel(column, TStageObject::T_ScaleY, 0.5);
  setChannel(column, TStageObject::T_Angle, 30);
  setChannel(column, TStageObject::T_Z, -300);
  key(column->getParam(TStageObject::T_Scale), 0, 1);
  key(column->getParam(TStageObject::T_Scale), 12, 4);
  column->invalidate();
  TStageObject* camera = object(
      document,
      document.scene->getXsheet()->getStageObjectTree()->getCurrentCameraId());
  setChannel(camera, TStageObject::T_Scale, 0.5);
  setChannel(camera, TStageObject::T_Z, 200);
  document.scene->getCurrentCamera()->setRes(TDimension(100, 30));
  for (auto mode : {AffineProbeFx::Mode::Scale, AffineProbeFx::Mode::Full}) {
    AffineProbeFx::mode = mode;
    for (int frame : {0, 6, 12}) requireBound(document, probe, frame, true);
  }
  output(document, nullptr);
  context.removeDocument(document.id);
}

void sharedPlacements(Context& context) {
  auto& document = scene(context, 64, 48);
  TFx* near      = create(context, document, "colorCardFx");
  TFx* far       = create(context, document, "colorCardFx");
  setChannel(object(document, TStageObjectId::ColumnId(0)),
             TStageObject::T_Scale, 4);
  TFx* over = create(context, document, "overFx");
  over->getInputPort(0)->setFx(near);
  over->getInputPort(1)->setFx(far);
  AffineProbeFx::mode = AffineProbeFx::Mode::Scale;
  TFx* probe          = insertProbe(document, over);
  requireBound(document, probe, 0, true);
  FxDag* graph = document.scene->getXsheet()->getFxDag();
  graph->addToXsheet(near);
  probe->getInputPort(0)->setFx(graph->getXsheetFx());
  requireBound(document, probe, 0, true);
  probe->getInputPort(0)->setFx(nullptr);
  output(document, nullptr);
  context.removeDocument(document.id);
}

// A tiny mirrored placement is still "isotropic" within the native 0.001
// tolerance, so an effect that handles it receives a negative determinant.
void mirroredPlacement(Context& context) {
  auto& document       = scene(context, 64, 48);
  TFx* card            = create(context, document, "colorCardFx");
  TStageObject* column = object(document, TStageObjectId::ColumnId(0));
  setChannel(column, TStageObject::T_ScaleX, -0.0001);
  setChannel(column, TStageObject::T_ScaleY, 0.0001);
  AffineProbeFx::mode = AffineProbeFx::Mode::Isotropic;
  TFx* probe          = insertProbe(document, card);
  require(received(*document.scene, 0).det() < 0,
          "The native host no longer hands a mirrored affine to the FX");
  output(document, nullptr);
  probe->getInputPort(0)->setFx(nullptr);
  TFx* erode = create(context, document, "STD_erodeDilateFx");
  erode->getInputPort(0)->setFx(card);
  output(document, erode);
  QJsonObject details;
  rejects(
      context, [&] { preflightAutomationRender(document, 64, 48, 0, 0, 1); },
      "fx_preflight_failed", &details);
  require(details.value("reason") == "positive_determinant" &&
              details.value("fx_id") == document.idForFx(erode),
          "Mirrored erode/dilate preflight details changed");
  setChannel(column, TStageObject::T_ScaleX, 0.0001);
  preflightAutomationRender(document, 64, 48, 0, 0, 1);
  context.removeDocument(document.id);
}

void extentPreflight(Context& context) {
  auto& document = scene(context, 64, 48);
  TFx* card      = create(context, document, "colorCardFx");
  TFx* blur      = create(context, document, "STD_blurFx");
  // Disconnected, so it neither pads nor appears in the provenance.
  create(context, document, "STD_glowFx");
  blur->getInputPort(0)->setFx(card);
  output(document, blur);
  number(blur, "value")->setDefaultValue(150);
  const QJsonArray provenance =
      preflightAutomationRender(document, 64, 48, 0, 12, 1);
  require(
      provenance.size() == 2 &&
          provenance[0].toObject().value("fx_id") == document.idForFx(blur) &&
          provenance[0].toObject().value("replay_class") == "stateless" &&
          provenance[1].toObject().value("type") == "colorCardFx",
      "Render provenance must list exactly the rendered effects");
  TStageObject* column = object(document, TStageObjectId::ColumnId(0));
  key(column->getParam(TStageObject::T_Scale), 0, 1);
  key(column->getParam(TStageObject::T_Scale), 12, 10);
  column->invalidate();
  QJsonObject details;
  rejects(
      context, [&] { preflightAutomationRender(document, 64, 48, 0, 12, 1); },
      "fx_preflight_failed", &details);
  require(details.value("reason") == "extent" &&
              details.value("parameter") == "value" &&
              details.value("frame") == 8 && details.value("max_px") == 1024 &&
              details.value("extent_px").toDouble() > 1024,
          "Extent preflight must report the first frame beyond the limit");
  rejects(
      context, [&] { preflightAutomationRender(document, 64, 48, 0, 12, 5); },
      "fx_preflight_failed", &details);
  require(details.value("frame") == 10, "Extent preflight ignored the step");
  preflightAutomationRender(document, 64, 48, 0, 7, 1);
  blur->getAttributes()->enable(false);
  require(preflightAutomationRender(document, 64, 48, 0, 12, 1).size() == 1,
          "A disabled effect must neither pad nor appear in provenance");
  blur->getAttributes()->enable(true);
  output(document, nullptr);
  context.removeDocument(document.id);
}

void macroMemberExtent(Context& context) {
  auto& document = scene(context, 64, 48);
  TFx* card      = create(context, document, "colorCardFx");
  TFx* blur      = create(context, document, "STD_blurFx");
  TFx* glow      = create(context, document, "STD_glowFx");
  blur->getInputPort(0)->setFx(card);
  glow->getInputPort("Light")->setFx(blur);
  number(glow, "value")->setDefaultValue(200);
  const QJsonObject macro = call(
      context, document, "fx.macro.create",
      {{"name", "Extent macro"},
       {"fx_ids", QJsonArray{document.idForFx(blur), document.idForFx(glow)}}});
  TFx* container =
      document.fx(macro.value("node").toObject().value("fx_id").toString());
  output(document, container);
  QJsonObject details;
  rejects(
      context, [&] { preflightAutomationRender(document, 1024, 768, 0, 0, 1); },
      "fx_preflight_failed", &details);
  require(details.value("fx_id") == document.idForFx(container) &&
              details.value("member_key").isString(),
          "Macro member extents must name the macro and its member");
  output(document, nullptr);
  context.removeDocument(document.id);
}
QJsonObject waitForJob(Context& context, const QString& job) {
  QJsonObject status;
  QEventLoop loop;
  QTimer poll, limit;
  QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
    status = executeJobs(context, "job.status", {{"job_id", job}}).toObject();
    const QString state = status.value("status").toString();
    if (state == "completed" || state == "failed" || state == "canceled" ||
        state == "deadline_exceeded")
      loop.quit();
  });
  QObject::connect(&limit, &QTimer::timeout, &loop, &QEventLoop::quit);
  poll.start(5);
  limit.start(30000);
  loop.exec(QEventLoop::ExcludeUserInputEvents);
  return status;
}

QString submitSequence(Context& context, Document& document) {
  return executeRender(context, "render.submit",
                       {{"document_id", document.id},
                        {"start_frame", 0},
                        {"end_frame", 119}})
      .toObject()
      .value("job_id")
      .toString();
}

void renderDeadline(Context& context) {
  auto& document = scene(context, 64, 48);
  output(document, create(context, document, "colorCardFx"));
  const int budget         = context.renderDeadlineMs;
  context.renderDeadlineMs = 1;
  const QString late       = submitSequence(context, document);
  const QJsonObject ended  = waitForJob(context, late);
  require(
      ended.value("status") == "deadline_exceeded" &&
          ended.value("writers_closed").toBool() &&
          ended.value("failure").isNull() &&
          ended.value("termination").toObject().value("mode") == "cooperative",
      "A render past its deadline must end as deadline_exceeded");
  rejects(
      context, [&] { executeJobs(context, "job.result", {{"job_id", late}}); },
      "artifacts_unavailable");
  context.renderDeadlineMs = 200;
  const QString canceled   = submitSequence(context, document);
  executeJobs(context, "job.cancel", {{"job_id", canceled}});
  require(waitForJob(context, canceled).value("status") == "canceled",
          "A caller's cancellation must not be reported as a deadline");
  context.renderDeadlineMs = budget;
  const QString preview =
      executeRender(context, "render.preview",
                    {{"document_id", document.id}, {"frame", 0}})
          .toObject()
          .value("job_id")
          .toString();
  require(waitForJob(context, preview).value("status") == "completed",
          "The worker must keep rendering after a deadline");
  context.removeDocument(document.id);
}
}  // namespace

namespace GroundzeroAutomation {
void runAutomationFxPreflightTests(Context& context) {
  renderDeadline(context);
  columnPlacements(context);
  sharedPlacements(context);
  mirroredPlacement(context);
  extentPreflight(context);
  macroMemberExtent(context);
}
}  // namespace GroundzeroAutomation
