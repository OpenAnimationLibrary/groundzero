#include "stdfx.h"
#include "t3dsource.h"
#include "tfxparam.h"
#include "tnotanimatableparam.h"
#include "tseexprattributes.h"

#include <QCryptographicHash>
#include <QString>
#include <sstream>

// Render-local user attributes work for both ordinary raster expressions and
// a compatible unprojected 3D source. Values are frozen before input traversal.
class SeExprAttributeFx final : public TStandardRasterFx, public T3DRenderSource {
  FX_PLUGIN_DECLARATION(SeExprAttributeFx)
  TRasterFxPort m_source;
  TStringParamP m_namespace, m_name;
  TIntEnumParamP m_scope, m_type;
  TDoubleParamP m_scalar, m_x, m_y, m_z;

  TRenderSettings settings(const TRenderSettings &input, double frame) const {
    try {
    TSeExprAttributes::Snapshot snapshot;
    const std::string key = TSeExprAttributes::name(
        QString::fromStdWString(m_namespace->getValue()).toStdString() + "::" +
        QString::fromStdWString(m_name->getValue()).toStdString());
    const auto value = m_type->getValue() == 0
        ? TSeExpr::Value::scalar(m_scalar->getValue(frame))
        : TSeExpr::Value::vector(m_x->getValue(frame), m_y->getValue(frame),
                                 m_z->getValue(frame));
    (m_scope->getValue() == 1 ? snapshot.options : snapshot.attributes)[key] = value;
    TRenderSettings result(input);
    result.m_data.push_back(new TSeExprAttributes::RenderData(std::move(snapshot)));
    result.m_userCachable = false;
    return result;
    } catch (const std::exception &error) {
      throw TException("Expression Attribute: " + std::string(error.what()));
    }
  }
  T3DRenderSource *source3D() const {
    TFx *fx = m_source.getFx();
    if (auto *column = dynamic_cast<TZeraryColumnFx *>(fx)) fx = column->getZeraryFx();
    auto *source = dynamic_cast<T3DRenderSource *>(fx);
    if (!source)
      throw std::runtime_error("Expression Attribute needs a compatible 3D Source for this connection");
    return source;
  }

public:
  bool hasSurface3D() const override {
    if (!m_source.isConnected()) return false;
    TFx *fx = m_source.getFx();
    if (auto *column = dynamic_cast<TZeraryColumnFx *>(fx)) fx = column->getZeraryFx();
    const auto *source = dynamic_cast<const T3DRenderSource *>(fx);
    return source && source->hasSurface3D();
  }
  void dryCompute3D(double frame, const TRenderSettings &input) const override {
    if (m_source.isConnected()) source3D()->dryCompute3D(frame, settings(input, frame));
  }

  SeExprAttributeFx()
      : m_namespace(L"user"), m_name(L"value"),
        m_scope(0, "Attribute"), m_type(0, "Scalar"),
        m_scalar(0.0), m_x(0.0), m_y(0.0), m_z(0.0) {
    addInputPort("Source", m_source);
    bindParam(this, "namespace", m_namespace);
    bindParam(this, "name", m_name);
    bindParam(this, "scope", m_scope);
    bindParam(this, "valueType", m_type);
    bindParam(this, "value", m_scalar);
    bindParam(this, "x", m_x); bindParam(this, "y", m_y); bindParam(this, "z", m_z);
    m_scope->addItem(1, "Option"); m_type->addItem(1, "Vector");
    for (const auto &parameter : {m_scalar, m_x, m_y, m_z})
      parameter->setValueRange(-1000000000.0, 1000000000.0);
    enableComputeInFloat(true);
  }

  bool canHandle(const TRenderSettings &, double) override { return true; }
  bool allowUserCacheOnPort(int) override { return false; }
  bool doGetBBox(double frame, TRectD &box, const TRenderSettings &input) override {
    box = TRectD();
    if (!m_source.isConnected()) return false;
    return m_source->doGetBBox(frame, box, settings(input, frame));
  }
  void doDryCompute(TRectD &rect, double frame, const TRenderSettings &input) override {
    if (m_source.isConnected()) m_source->dryCompute(rect, frame, settings(input, frame));
  }
  void doCompute(TTile &tile, double frame, const TRenderSettings &input) override {
    tile.getRaster()->clear();
    if (input.m_isCanceled && *input.m_isCanceled) return;
    if (m_source.isConnected()) m_source->compute(tile, frame, settings(input, frame));
  }
  std::string getAlias(double frame, const TRenderSettings &input) const override {
    const auto snapshot = settings(input, frame);
    std::ostringstream alias;
    alias << getFxType() << '[' << TSeExprAttributes::snapshot(snapshot).identity();
    if (m_source.isConnected())
      alias << ',' << TRasterFxP(m_source.getFx())->getAlias(frame, snapshot);
    return alias.str() + ']';
  }
  std::shared_ptr<const otglb::RenderScene> get3DRenderScene(
      double frame, const int *canceled,
      const otglb::LightingRig *lighting = nullptr,
      const std::vector<otglb::ModelTransform> *transforms = nullptr,
      const TRenderSettings *input = nullptr,
      const std::shared_ptr<const otglb::SurfaceRequest> &surface = {}) const override {
    if (!m_source.isConnected() || (canceled && *canceled)) return {};
    const auto snapshot = settings(input ? *input : TRenderSettings(), frame);
    return source3D()->get3DRenderScene(frame, canceled, lighting, transforms,
                                       &snapshot, surface);
  }
  std::shared_ptr<const otglb::RenderScene> get3DRenderGeometry(
      double frame, const int *canceled,
      const std::vector<otglb::ModelTransform> *transforms = nullptr,
      const TRenderSettings *input = nullptr,
      const std::shared_ptr<const otglb::SurfaceRequest> &surface = {}) const override {
    if (!m_source.isConnected() || (canceled && *canceled)) return {};
    const auto snapshot = settings(input ? *input : TRenderSettings(), frame);
    return source3D()->get3DRenderGeometry(frame, canceled, transforms, &snapshot, surface);
  }
};

FX_PLUGIN_IDENTIFIER(SeExprAttributeFx, "seexprAttributeFx")
