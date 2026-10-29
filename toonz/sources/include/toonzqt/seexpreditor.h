#pragma once

#include "tcommon.h"
#include "tseexpr.h"

#include <QJsonObject>
#include <QWidget>
#include <functional>
#include <memory>

#undef DVAPI
#ifdef TOONZQT_EXPORTS
#define DVAPI DV_EXPORT_API
#else
#define DVAPI DV_IMPORT_API
#endif

// A native draft editor. Applying emits one complete document/source; the host
// owns its undo transaction, animation and scene persistence.
class DVAPI SeExprEditor final : public QWidget {
  Q_OBJECT
public:
  using RangeEvaluator = std::function<bool(double, std::vector<double> &, QString &)>;
  using AnimationCurveLinkResolver =
      std::function<bool(const QString &, QJsonObject &, QString &)>;
  explicit SeExprEditor(QWidget *parent = nullptr);
  ~SeExprEditor() override;

  void setDocument(const QString &document, bool discardDraft = false);
  QString document() const;
  void setSource(const QString &source, bool discardDraft = false);
  QString source() const;
  void setResultType(TSeExpr::Type type);
  void setSurfaceDocumentSupported(bool supported);
  void setContext(const std::vector<TSeExpr::Variable> &variables,
                  const std::vector<TSeExpr::Function> &functions = {});
  void setFrame(double frame);
  void setPresetDirectory(const QString &directory);
  void setBuiltinPresetDirectory(const QString &directory);

  // Overrides let animation/FX hosts validate against their real references
  // and native schema. Callbacks execute on the GUI thread.
  void setValidator(std::function<QString(const QString &)> validator);
  void setPreviewEvaluator(std::function<QString(const QString &)> evaluator);
  void setCheckedPreviewEvaluator(
      std::function<bool(const QString &, QString &, QString &)> evaluator);
  void setRangeEvaluator(std::function<bool(const QString &, double,
                                           std::vector<double> &, QString &)> evaluator);
  // Called once when the range dialog opens, so hosts can clone their native
  // parameter state rather than sharing mutable evaluation caches.
  void setRangeEvaluatorFactory(
      std::function<RangeEvaluator(const QString &)> factory);
  // Explicit GUI action: replace a draft curve with a snapshot of its linked
  // native channel. Hosts guard their own lifetime and reference context.
  void setAnimationCurveLinkResolver(AnimationCurveLinkResolver resolver);
  bool validateDraft();
  bool applyDraft();
  void revertDraft();
  bool isModified() const;

  // Bounded, versioned JSON import/export also used by native workflow tests.
  bool importPreset(const QByteArray &data, QString *error = nullptr);
  QByteArray exportPreset() const;
  static QString renameVariable(const QString &source, const QString &oldName,
                                const QString &newName);

signals:
  void documentApplied(const QString &document);
  void sourceApplied(const QString &source);
  void modifiedChanged(bool modified);

protected:
  void changeEvent(QEvent *event) override;

private:
  class Imp;
  std::unique_ptr<Imp> m_imp;
};
