// Native particle-cache import, inspection, interchange and baking commands.
#include "tapp.h"
#include "menubarcommandids.h"
#include "toonzqt/menubarcommand.h"
#include "toonzqt/dvdialog.h"
#include "toonz/dpiscale.h"
#include "toonz/fxcommand.h"
#include "toonz/fxdag.h"
#include "toonz/particlecache.h"
#include "toonz/particlebake.h"
#include "toonz/sceneresources.h"
#include "toonz/scenefx.h"
#include "toonz/sceneproperties.h"
#include "toonz/tcamera.h"
#include "toonz/tcolumnfx.h"
#include "toonz/tcolumnfxset.h"
#include "toonz/tcolumnhandle.h"
#include "toonz/tframehandle.h"
#include "toonz/tfxhandle.h"
#include "toonz/toonzscene.h"
#include "toonz/tscenehandle.h"
#include "toonz/tstageobjecttree.h"
#include "toonz/txsheet.h"
#include "toonz/txsheethandle.h"
#include "toonz/txshcell.h"
#include "toonz/txshzeraryfxcolumn.h"
#include "tdoubleparam.h"
#include "tfxattributes.h"
#include "tnotanimatableparam.h"
#include "toutputproperties.h"
#include "tparamcontainer.h"
#include "tparamset.h"
#include "trasterfx.h"
#include "trenderer.h"
#include "tundo.h"
#include "tzeraryfx.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QProgressDialog>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <memory>
#include <set>

namespace {
QString tr(const char *s) { return QCoreApplication::translate("ParticleCache", s); }

TFx *innerFx(TFx *fx) {
  if (auto column = dynamic_cast<TZeraryColumnFx *>(fx))
    return column->getZeraryFx();
  return fx;
}

TStringParam *stringParam(TFx *fx, const char *name) {
  return fx ? dynamic_cast<TStringParam *>(fx->getParams()->getParam(name))
            : nullptr;
}

QString stringValue(TFx *fx, const char *name) {
  auto param = stringParam(fx, name);
  return param ? QString::fromStdWString(param->getValue()) : QString();
}

void setString(TFx *fx, const char *name, const QString &value) {
  if (auto param = stringParam(fx, name)) param->setValue(value.toStdWString());
}

void setNumber(TFx *fx, const char *name, double value) {
  if (auto param = dynamic_cast<TDoubleParam *>(fx->getParams()->getParam(name)))
    param->setDefaultValue(value);
}

void usePackageSizeSemantics(TFx *fx, const ParticleCache::Metadata &metadata) {
  if (metadata.extra.value("sizeSemantics").toString() == "artwork-scale")
    if (auto param = dynamic_cast<TIntEnumParam *>(fx->getParams()->getParam("sizeMode")))
      param->setValue(1);
}

bool isCacheFx(TFx *fx) { return stringParam(fx, "cacheFile") != nullptr; }

void notifyChange() {
  TApp *app = TApp::instance();
  app->getCurrentScene()->setDirtyFlag(true);
  app->getCurrentFx()->notifyFxChanged();
  app->getCurrentXsheet()->notifyXsheetChanged();
}

QString decodedPath(const QString &path) {
  if (path.trimmed().isEmpty()) return QString();
  ToonzScene *scene = TApp::instance()->getCurrentScene()->getScene();
  TFilePath result = scene->decodeFilePath(TFilePath(path.toStdWString()));
  if (!result.isAbsolute()) {
    const TFilePath parent = scene->getScenePath().getParentDir();
    if (parent.isEmpty() || !parent.isAbsolute()) return QString();
    result = parent + result;
  }
  return result.getQString();
}

QString encodedPath(const QString &path) {
  ToonzScene *scene = TApp::instance()->getCurrentScene()->getScene();
  return scene->codeFilePath(TFilePath(path.toStdWString())).getQString();
}

QString readFilter() {
  QStringList patterns;
  patterns << "*.otpc";
  for (const auto &format : ParticleCache::capabilities()) {
    if (!format.readable) continue;
    patterns << ("*." + format.extension) << ("*." + format.extension + ".gz");
  }
  return tr("Particle caches (%1)").arg(patterns.join(' '));
}

class CacheSettingsUndo final : public TUndo {
  TFxP m_fx;
  std::vector<TParamP> m_parameters, m_before, m_after;

  void apply(const std::vector<TParamP> &values) const {
    for (std::size_t i = 0; i < values.size(); ++i)
      m_parameters[i]->copy(values[i].getPointer());
    notifyChange();
  }

public:
  explicit CacheSettingsUndo(TFx *fx) : m_fx(fx) {
    for (const char *name : {"cacheFile", "mappingJson", "useManifestMapping"}) {
      TParam *param = fx->getParams()->getParam(name);
      if (!param) continue;
      m_parameters.push_back(TParamP(param));
      m_before.push_back(TParamP(param->clone()));
    }
  }
  void finish() {
    for (const auto &param : m_parameters)
      m_after.push_back(TParamP(param->clone()));
  }
  void undo() const override { apply(m_before); }
  void redo() const override { apply(m_after); }
  int getSize() const override { return sizeof(*this); }
  QString getHistoryString() override { return tr("Particle Cache Settings"); }
};

struct AttributeMapping {
  const char *caption;
  QString ParticleCache::Mapping::*member;
};
const AttributeMapping mappings[] = {
    {"Position", &ParticleCache::Mapping::position},
    {"Velocity", &ParticleCache::Mapping::velocity},
    {"Particle ID", &ParticleCache::Mapping::id},
    {"Size", &ParticleCache::Mapping::size},
    {"Rotation", &ParticleCache::Mapping::rotation},
    {"Color", &ParticleCache::Mapping::color},
    {"Opacity", &ParticleCache::Mapping::opacity},
    {"Age", &ParticleCache::Mapping::age},
    {"Lifetime", &ParticleCache::Mapping::lifetime},
    {"Artwork source", &ParticleCache::Mapping::artworkIndex},
    {"Artwork frame", &ParticleCache::Mapping::artworkFrame}};

QDoubleSpinBox *numberBox(QWidget *parent, double value,
                          double minimum = 0.000001, double maximum = 1000000) {
  auto box = new QDoubleSpinBox(parent);
  box->setDecimals(6);
  box->setRange(minimum, maximum);
  box->setValue(value);
  return box;
}

class Inspector final : public QDialog {
  QLineEdit *m_path;
  QLabel *m_summary;
  QTableWidget *m_attributes;
  std::vector<QComboBox *> m_maps;
  QDoubleSpinBox *m_units, *m_defaultSize;
  std::array<QComboBox *, 3> m_axes;
  QCheckBox *m_diameter, *m_radians;
  QPushButton *m_accept;
  ParticleCache::Mapping m_mapping;
  QString m_loadedPath;
  ParticleCache::Sequence m_sequence;
  bool m_mappingEditable;

  void showMapping(const ParticleCache::Mapping &mapping,
                   const ParticleCache::Snapshot &snapshot) {
    m_mapping = mapping;
    for (std::size_t i = 0; i < m_maps.size(); ++i) {
      auto box = m_maps[i];
      box->clear();
      box->addItem(QString());
      for (const auto &attribute : snapshot.attributes)
        if (!attribute.fixed) box->addItem(attribute.name);
      box->setCurrentText(mapping.*(mappings[i].member));
    }
    m_units->setValue(mapping.units);
    m_defaultSize->setValue(mapping.defaultSize);
    m_diameter->setChecked(mapping.sizeIsDiameter);
    m_radians->setChecked(mapping.rotationInRadians);
    for (int i = 0; i < 3; ++i)
      m_axes[i]->setCurrentIndex(mapping.axes[i] * 2 +
                                (mapping.axisSigns[i] < 0 ? 1 : 0));
  }

  void load(bool keepMapping) {
    m_accept->setEnabled(false);
    m_attributes->setRowCount(0);
    QString error;
    ParticleCache::Sequence sequence;
    if (!ParticleCache::openSequence(decodedPath(m_path->text().trimmed()),
                                     sequence, error)) {
      m_summary->setText(error);
      return;
    }
    if (sequence.frames.empty()) {
      m_summary->setText(tr("The cache has no frames."));
      return;
    }
    // Inspect original attributes even when the current mapping is invalid.
    auto mapping = (keepMapping || !m_mappingEditable) ? currentMapping() : sequence.mapping;
    auto snapshot = ParticleCache::inspectFrame(sequence.frames.begin()->second, error);
    if (!snapshot) {
      m_summary->setText(error);
      return;
    }
    m_sequence = sequence;
    m_loadedPath = m_path->text().trimmed();
    m_attributes->setRowCount(int(snapshot->attributes.size()));
    int row = 0;
    for (const auto &attribute : snapshot->attributes) {
      QString type;
      switch (attribute.type) {
      case ParticleCache::AttributeType::Float: type = tr("Float"); break;
      case ParticleCache::AttributeType::Vector: type = tr("Vector"); break;
      case ParticleCache::AttributeType::Int: type = tr("Integer"); break;
      case ParticleCache::AttributeType::IndexedString: type = tr("String"); break;
      }
      const QStringList cells{attribute.name, type,
                              QString::number(attribute.components),
                              attribute.fixed ? tr("Global") : tr("Per particle")};
      for (int col = 0; col < cells.size(); ++col)
        m_attributes->setItem(row, col, new QTableWidgetItem(cells[col]));
      ++row;
    }
    showMapping(mapping, *snapshot);
    m_summary->setText(
        tr("%1 particles in the first frame; %2 samples; %3 FPS.\n%4")
            .arg(qulonglong(snapshot->particleCount))
            .arg(qulonglong(sequence.frames.size()))
            .arg(sequence.metadata.framesPerSecond)
            .arg(snapshot->warnings.join('\n')));
    m_accept->setEnabled(true);
  }

public:
  Inspector(const QString &path, const QString &mappingJson, bool importing,
            bool mappingEditable = true)
      : QDialog(TApp::instance()->getMainWindow()), m_mappingEditable(mappingEditable) {
    setWindowTitle(importing ? tr("Import Particle Cache")
                             : tr("Particle Cache Inspector"));
    resize(650, 680);
    auto layout = new QVBoxLayout(this);
    auto pathRow = new QHBoxLayout;
    m_path = new QLineEdit(path, this);
    auto browse = new QPushButton(tr("Browse..."), this);
    auto inspect = new QPushButton(tr("Inspect"), this);
    pathRow->addWidget(m_path, 1);
    pathRow->addWidget(browse);
    pathRow->addWidget(inspect);
    layout->addLayout(pathRow);
    m_summary = new QLabel(this);
    m_summary->setWordWrap(true);
    m_summary->setTextFormat(Qt::PlainText);
    layout->addWidget(m_summary);
    auto tabs = new QTabWidget(this);
    m_attributes = new QTableWidget(0, 4, tabs);
    m_attributes->setHorizontalHeaderLabels(
        {tr("Attribute"), tr("Type"), tr("Components"), tr("Scope")});
    m_attributes->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_attributes->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    tabs->addTab(m_attributes, tr("Attributes"));
    auto mappingWidget = new QWidget(tabs);
    mappingWidget->setEnabled(mappingEditable);
    auto form = new QFormLayout(mappingWidget);
    for (const auto &entry : mappings) {
      auto combo = new QComboBox(mappingWidget);
      combo->setEditable(true);
      form->addRow(tr(entry.caption), combo);
      m_maps.push_back(combo);
    }
    m_units = numberBox(mappingWidget, 1);
    m_defaultSize = numberBox(mappingWidget, 1);
    form->addRow(tr("Scale to OT drawing units"), m_units);
    form->addRow(tr("Default particle size"), m_defaultSize);
    const char *axisNames[] = {"Horizontal axis", "Vertical axis", "Depth axis"};
    for (int i = 0; i < 3; ++i) {
      m_axes[i] = new QComboBox(mappingWidget);
      m_axes[i]->addItems({"X", "-X", "Y", "-Y", "Z", "-Z"});
      m_axes[i]->setCurrentIndex(i * 2);
      form->addRow(tr(axisNames[i]), m_axes[i]);
    }
    m_diameter = new QCheckBox(tr("Size attribute is a diameter"), mappingWidget);
    m_radians = new QCheckBox(tr("Rotation attribute uses radians"), mappingWidget);
    form->addRow(m_diameter);
    form->addRow(m_radians);
    auto scroll = new QScrollArea(tabs);
    scroll->setWidgetResizable(true);
    scroll->setWidget(mappingWidget);
    tabs->addTab(scroll, tr("Attribute Mapping"));
    layout->addWidget(tabs, 1);
    if (!mappingEditable) {
      auto notice = new QLabel(tr("Mapping is controlled in FX Settings. Enable Use "
                                 "Manifest Mapping there to edit mappings in this inspector."), this);
      notice->setWordWrap(true);
      layout->addWidget(notice);
    }
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    m_accept = buttons->addButton(importing ? tr("Import") : tr("Apply"),
                                  QDialogButtonBox::AcceptRole);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_accept, &QPushButton::clicked, this, [this] {
      if (m_loadedPath != m_path->text().trimmed()) load(true);
      if (!m_accept->isEnabled()) return;
      QString error;
      auto mapped = ParticleCache::loadFrame(m_sequence.frames.begin()->second,
                                             currentMapping(), error);
      if (!mapped) { m_summary->setText(error); return; }
      accept();
    });
    connect(inspect, &QPushButton::clicked, this, [this] { load(true); });
    connect(browse, &QPushButton::clicked, this, [this] {
      const QString path = QFileDialog::getOpenFileName(
          this, tr("Open Particle Cache"), decodedPath(m_path->text()), readFilter());
      if (path.isEmpty()) return;
      m_path->setText(path);
      load(false);
    });
    ParticleCache::Snapshot empty;
    showMapping(ParticleCache::Mapping(), empty);
    m_accept->setEnabled(false);
    if (!path.isEmpty()) load(false);
    if (!mappingJson.isEmpty()) {
      QString error;
      ParticleCache::Mapping mapping;
      if (ParticleCache::mappingFromJson(
              QJsonDocument::fromJson(mappingJson.toUtf8()).object(), mapping, error)) {
        m_mapping = mapping;
        for (std::size_t i = 0; i < m_maps.size(); ++i)
          m_maps[i]->setCurrentText(mapping.*(mappings[i].member));
        m_units->setValue(mapping.units);
        m_defaultSize->setValue(mapping.defaultSize);
        m_diameter->setChecked(mapping.sizeIsDiameter);
        m_radians->setChecked(mapping.rotationInRadians);
        for (int i = 0; i < 3; ++i)
          m_axes[i]->setCurrentIndex(mapping.axes[i] * 2 +
                                    (mapping.axisSigns[i] < 0 ? 1 : 0));
      } else {
        m_summary->setText(error);
      }
    }
  }

  QString path() const { return decodedPath(m_path->text().trimmed()); }
  ParticleCache::Mapping currentMapping() const {
    auto result = m_mapping;
    for (std::size_t i = 0; i < m_maps.size(); ++i)
      result.*(mappings[i].member) = m_maps[i]->currentText().trimmed();
    result.units = m_units->value();
    result.defaultSize = m_defaultSize->value();
    result.sizeIsDiameter = m_diameter->isChecked();
    result.rotationInRadians = m_radians->isChecked();
    for (int i = 0; i < 3; ++i) {
      result.axes[i] = m_axes[i]->currentIndex() / 2;
      result.axisSigns[i] = m_axes[i]->currentIndex() % 2 ? -1 : 1;
    }
    return result;
  }
  QString mappingJson() const {
    return QString::fromUtf8(QJsonDocument(ParticleCache::mappingToJson(currentMapping()))
                                 .toJson(QJsonDocument::Compact));
  }
  const ParticleCache::Sequence &sequence() const { return m_sequence; }
};

class CacheExposureUndo final : public TUndo {
  TXshZeraryFxColumnP m_column;
  TXshCell m_cell;
  int m_first, m_oldLast, m_newLast;
  void apply(int last) const {
    m_column->clearCells(m_first, std::max(m_oldLast, m_newLast) - m_first + 1);
    for (int row = m_first; row <= last; ++row) m_column->setCell(row, m_cell);
    notifyChange();
  }
public:
  CacheExposureUndo(TXshZeraryFxColumn *column, int first, int last)
      : m_column(column), m_cell(column->getCell(first)), m_first(first),
        m_newLast(last) {
    int oldFirst = 0;
    column->getRange(oldFirst, m_oldLast);
  }
  void undo() const override { apply(m_oldLast); }
  void redo() const override { apply(m_newLast); }
  int getSize() const override { return sizeof(*this); }
  QString getHistoryString() override { return tr("Expose Particle Cache"); }
};

void importCache() {
  TApp *app = TApp::instance();
  const QString path = QFileDialog::getOpenFileName(
      app->getMainWindow(), tr("Import Particle Cache"), QString(), readFilter());
  if (path.isEmpty()) return;
  Inspector dialog(path, QString(), true);
  if (dialog.exec() != QDialog::Accepted) return;
  TFxP fx = TFx::create("STD_particleCacheFx");
  if (!fx) { DVGui::error(tr("The Particle Cache effect is unavailable.")); return; }
  setString(fx.getPointer(), "cacheFile", encodedPath(dialog.path()));
  setString(fx.getPointer(), "mappingJson", dialog.mappingJson());
  const int firstFrame = app->getCurrentFrame()->getFrame();
  const double fps = app->getCurrentScene()->getScene()
                         ->getProperties()->getOutputProperties()->getFrameRate();
  setNumber(fx.getPointer(), "startFrame", firstFrame);
  setNumber(fx.getPointer(), "sceneFps", fps);
  usePackageSizeSemantics(fx.getPointer(), dialog.sequence().metadata);
  TUndoManager::manager()->beginBlock();
  TFxCommand::addFx(fx.getPointer(), QList<TFxP>(), app,
                   app->getCurrentColumn()->getColumnIndex() + 1,
                   firstFrame, false);
  const auto &sequence = dialog.sequence();
  if (sequence.frames.size() > 1) {
    const double duration = std::ceil((double(sequence.metadata.lastFrame) -
                                      sequence.metadata.firstFrame + 1) * fps /
                                     sequence.metadata.framesPerSecond);
    auto zerary = dynamic_cast<TZeraryFx *>(fx.getPointer());
    if (zerary && zerary->getColumnFx() && std::isfinite(duration) &&
        duration >= 1 && duration <= 1000000) {
      auto undo = std::unique_ptr<CacheExposureUndo>(new CacheExposureUndo(
          zerary->getColumnFx()->getColumn(), firstFrame,
          firstFrame + int(duration) - 1));
      undo->redo();
      TUndoManager::manager()->add(undo.release());
    }
  }
  TUndoManager::manager()->endBlock();
}

void inspectCache() {
  TFxP fx = innerFx(TApp::instance()->getCurrentFx()->getFx());
  if (!isCacheFx(fx.getPointer())) {
    DVGui::error(tr("Select a Particle Cache effect in the FX Schematic."));
    return;
  }
  const auto manifestParam = dynamic_cast<TBoolParam *>(fx->getParams()->getParam("useManifestMapping"));
  const bool manifestMapping = !manifestParam || manifestParam->getValue();
  QString mappingJson = stringValue(fx.getPointer(), "mappingJson");
  if (!manifestMapping) {
    ParticleCache::Mapping mapping;
    const char *names[] = {"positionAttribute", "velocityAttribute", "idAttribute",
                          "sizeAttribute", "rotationAttribute", "colorAttribute",
                          "opacityAttribute", "ageAttribute", "lifetimeAttribute",
                          "sourceAttribute", "drawingAttribute"};
    for (std::size_t i = 0; i < sizeof(mappings) / sizeof(mappings[0]); ++i)
      mapping.*(mappings[i].member) = stringValue(fx.getPointer(), names[i]);
    const double frame = TApp::instance()->getCurrentFrame()->getFrame();
    const auto number = [&](const char *name, double fallback) {
      auto param = dynamic_cast<TDoubleParam *>(fx->getParams()->getParam(name));
      return param ? param->getValue(frame) : fallback;
    };
    const auto flag = [&](const char *name) {
      auto param = dynamic_cast<TBoolParam *>(fx->getParams()->getParam(name));
      return param && param->getValue();
    };
    auto axes = dynamic_cast<TIntEnumParam *>(fx->getParams()->getParam("axes"));
    const int plane = axes ? axes->getValue() : 0;
    mapping.axes = plane == 1 ? std::array<int, 3>{{0, 2, 1}}
                  : plane == 2 ? std::array<int, 3>{{1, 2, 0}}
                               : std::array<int, 3>{{0, 1, 2}};
    mapping.axisSigns = {{1, flag("flipY") ? -1 : 1, 1}};
    mapping.units = number("units", 1);
    mapping.defaultSize = number("defaultSize", 1);
    mapping.sizeIsDiameter = flag("sizeIsDiameter");
    mapping.rotationInRadians = flag("rotationInRadians");
    mappingJson = QString::fromUtf8(QJsonDocument(ParticleCache::mappingToJson(mapping))
                                      .toJson(QJsonDocument::Compact));
  }
  Inspector dialog(stringValue(fx.getPointer(), "cacheFile"), mappingJson, false,
                   manifestMapping);
  if (dialog.exec() != QDialog::Accepted) return;
  auto undo = std::unique_ptr<CacheSettingsUndo>(new CacheSettingsUndo(fx.getPointer()));
  setString(fx.getPointer(), "cacheFile", encodedPath(dialog.path()));
  if (manifestMapping) {
    setString(fx.getPointer(), "mappingJson", dialog.mappingJson());
    if (manifestParam) manifestParam->setValue(true);
  }
  undo->finish();
  TUndoManager::manager()->add(undo.release());
  notifyChange();
}

void exportCache() {
  TFxP fx = innerFx(TApp::instance()->getCurrentFx()->getFx());
  if (!isCacheFx(fx.getPointer())) {
    DVGui::error(tr("Select a Particle Cache effect to export. To record a live "
                    "particle effect, use Bake Particle Effect."));
    return;
  }
  ParticleCache::Sequence sequence;
  QString error;
  if (!ParticleCache::openSequence(decodedPath(stringValue(fx.getPointer(), "cacheFile")),
                                   sequence, error)) {
    DVGui::error(error); return;
  }
  QDialog dialog(TApp::instance()->getMainWindow());
  dialog.setWindowTitle(tr("Export Particle Cache"));
  auto form = new QFormLayout(&dialog);
  auto format = new QComboBox(&dialog);
  for (const auto &capability : ParticleCache::capabilities()) {
    if (!capability.writable) continue;
    format->addItem(capability.description + " (." + capability.extension + ")",
                     capability.extension);
  }
  auto compressed = new QCheckBox(tr("Compress files (.gz)"), &dialog);
  auto allowLoss = new QCheckBox(tr("Allow attributes unsupported by the format to be omitted"), &dialog);
  allowLoss->setChecked(false);
  auto destination = new QLineEdit(&dialog);
  destination->setPlaceholderText(tr("New cache folder"));
  auto browse = new QPushButton(tr("Choose parent folder..."), &dialog);
  form->addRow(tr("Format"), format);
  form->addRow(compressed);
  form->addRow(allowLoss);
  form->addRow(tr("Destination"), destination);
  form->addRow(browse);
  auto note = new QLabel(tr("The destination must be a new folder. Existing files "
                           "will not be replaced. This exports the original particle data; "
                           "collect the OT project to share its artwork and effects."), &dialog);
  note->setWordWrap(true);
  form->addRow(note);
  auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
  form->addRow(buttons);
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  QObject::connect(browse, &QPushButton::clicked, &dialog, [&] {
    QString parent = QFileDialog::getExistingDirectory(&dialog, tr("Choose Parent Folder"));
    if (!parent.isEmpty()) destination->setText(QDir(parent).filePath("particle-cache-export"));
  });
  if (dialog.exec() != QDialog::Accepted) return;
  if (destination->text().trimmed().isEmpty()) {
    DVGui::error(tr("Choose a new destination folder.")); return;
  }
  ParticleCache::WriteOptions options;
  options.format = format->currentData().toString();
  options.compressed = compressed->isChecked();
  options.allowLossy = allowLoss->isChecked();
  QProgressDialog progress(tr("Exporting particle cache..."), tr("Cancel"), 0, 100,
                            TApp::instance()->getMainWindow());
  progress.setWindowModality(Qt::ApplicationModal);
  progress.setMinimumDuration(0);
  const auto cancel = [&] { QCoreApplication::processEvents(); return progress.wasCanceled(); };
  const auto update = [&](int done, int total) {
    progress.setMaximum(total); progress.setValue(done);
  };
  if (!ParticleCache::exportFiles(sequence, destination->text().trimmed(), options,
                                  error, cancel, update) && !progress.wasCanceled())
    DVGui::error(error);
}

// Retain the original procedural graph in the scene, rather than retaining it
// only in undo history. A zerary column keeps its cells and stage placement.
class UseParticleCacheUndo final : public TUndo {
  TXsheet *m_xsheet;
  TFxP m_source, m_cache, m_column;
  struct Output { TFxP owner; int index; };
  std::vector<Output> m_outputs;
  bool m_terminal = false;

  void apply(bool useCache) const {
    FxDag *dag = m_xsheet->getFxDag();
    TFx *active = useCache ? m_cache.getPointer() : m_source.getPointer();
    if (auto column = dynamic_cast<TZeraryColumnFx *>(m_column.getPointer())) {
      if (useCache) dag->getInternalFxs()->addFx(m_source.getPointer());
      column->setZeraryFx(active);
      if (!useCache) dag->getInternalFxs()->removeFx(m_source.getPointer());
    } else {
      if (useCache) dag->getInternalFxs()->addFx(m_cache.getPointer());
      for (const auto &output : m_outputs)
        output.owner->getInputPort(output.index)->setFx(active);
      if (m_terminal) {
        dag->removeFromXsheet(useCache ? m_source.getPointer() : m_cache.getPointer());
        dag->addToXsheet(active);
      }
      if (!useCache) dag->getInternalFxs()->removeFx(m_cache.getPointer());
    }
    TApp::instance()->getCurrentFx()->setFx(active);
    notifyChange();
  }

public:
  UseParticleCacheUndo(TXsheet *xsheet, TFx *source, TFx *cache)
      : m_xsheet(xsheet), m_source(source), m_cache(cache) {
    if (auto zerary = dynamic_cast<TZeraryFx *>(source))
      m_column = zerary->getColumnFx();
    if (!m_column) {
      for (int i = 0; i < source->getOutputConnectionCount(); ++i) {
        TFxPort *port = source->getOutputConnection(i);
        TFx *owner = port->getOwnerFx();
        for (int p = 0; p < owner->getInputPortCount(); ++p)
          if (owner->getInputPort(p) == port) m_outputs.push_back({TFxP(owner), p});
      }
      m_terminal = xsheet->getFxDag()->getTerminalFxs()->containsFx(source);
    }
  }
  void undo() const override { apply(false); }
  void redo() const override { apply(true); }
  int getSize() const override { return sizeof(*this); }
  QString getHistoryString() override { return tr("Bake and Use Particle Cache"); }
};

void connectArtwork(TFx *source, TFx *cache) {
  for (int p = 0; p < source->getInputPortCount(); ++p) {
    const std::string name = source->getInputPortName(p);
    if (name.compare(0, 7, "Texture") != 0) continue;
    const std::string destination = "Artwork" + name.substr(7);
    if (!cache->getInputPort(destination))
      cache->addInputPort(destination, new TRasterFxPort, 0);
    cache->getInputPort(destination)->setFx(source->getInputPort(p)->getFx());
  }
}

template <typename Container>
void bindSceneGrammar(TXsheet *xsheet, const Container *parameters) {
  for (int i = 0; i < parameters->getParamCount(); ++i) {
    TParam &param = *parameters->getParam(i);
    if (auto number = dynamic_cast<TDoubleParam *>(&param))
      xsheet->getStageObjectTree()->setGrammar(number);
    else if (auto group = dynamic_cast<TParamSet *>(&param))
      bindSceneGrammar(xsheet, group);
  }
}

bool validBakeAffine(const TAffine &affine) {
  return std::isfinite(affine.a11) && std::isfinite(affine.a12) &&
         std::isfinite(affine.a13) && std::isfinite(affine.a21) &&
         std::isfinite(affine.a22) && std::isfinite(affine.a23) &&
         std::isfinite(affine.det()) && affine.det() != 0;
}

// Keep the scene builder's complete column, parent, perspective, camera and DPI
// placement. Peeling its outer affine nodes gives the native source and exactly
// the affine that ordinary scene rendering passes to it, including the inherited
// placement of a retained generator that no longer has its own column wrapper.
bool prepareParticleBakeSource(ToonzScene *scene, TXsheet *xsheet,
                               const TFxP &source, int frame,
                               TRasterFxP &native, TAffine &affine,
                               QString &error) {
  try {
    TFxP root = source;
    if (auto zerary = dynamic_cast<TZeraryFx *>(source.getPointer()))
      if (zerary->getColumnFx()) root = zerary->getColumnFx();
    TFxP built = buildSceneFx(scene, frame, xsheet, root, BSFX_DEFAULT_TR, false);
    affine = TAffine();
    while (auto placement = dynamic_cast<NaAffineFx *>(built.getPointer())) {
      affine = affine * placement->getPlacement(frame);
      built = TFxP(placement->getInputPort(0)->getFx());
    }
    native = dynamic_cast<TRasterFx *>(built.getPointer());
    // Empty exposures and columns behind the camera have no source to evaluate.
    if (!built) return true;
    if (!native || !ParticleBake::isSupported(native.getPointer())) {
      error = tr("The selected particle graph could not be prepared for baking.");
      return false;
    }
    if (!validBakeAffine(affine)) {
      error = tr("The particle or camera placement is singular or invalid at frame %1.")
                  .arg(frame + 1);
      return false;
    }
    resolveSceneFxResources(native.getPointer(), scene);
    return true;
  } catch (const std::exception &e) {
    error = QString::fromUtf8(e.what());
  } catch (...) {
    error = tr("The particle effect could not be prepared at frame %1.").arg(frame + 1);
  }
  return false;
}

void bakeParticles() {
  TApp *app = TApp::instance();
  TFxP source = innerFx(app->getCurrentFx()->getFx());
  if (!source || !ParticleBake::isSupported(source.getPointer())) {
    DVGui::error(tr("Select a Particles or Tiled Particles effect in the FX Schematic."));
    return;
  }
  TXsheet *xsheet = app->getCurrentXsheet()->getXsheet();
  bool sourceInXsheet = xsheet->getFxDag()->getInternalFxs()->containsFx(source.getPointer());
  if (auto zerary = dynamic_cast<TZeraryFx *>(source.getPointer())) {
    if (auto wrapper = zerary->getColumnFx()) {
      TXshZeraryFxColumn *column = wrapper->getColumn();
      sourceInXsheet = column && column->getIndex() >= 0 &&
                      xsheet->getColumn(column->getIndex()) == column;
    }
  }
  if (!sourceInXsheet) {
    DVGui::error(tr("Select a particle generator in the current Xsheet. "
                    "If it belongs to a macro, explode the macro first."));
    return;
  }
  ToonzScene *scene = app->getCurrentScene()->getScene();
  TOutputProperties *output = scene->getProperties()->getOutputProperties();
  const double fps = output->getFrameRate();
  TCamera *camera = xsheet->getStageObjectTree()->getCurrentCamera();
  if (!camera || !std::isfinite(fps) || fps <= 0) {
    DVGui::error(tr("The scene needs a camera and a valid frame rate."));
    return;
  }
  const TRectD cameraRect = camera->getStageRect();
  if (cameraRect.isEmpty() || !std::isfinite(cameraRect.getLx()) ||
      !std::isfinite(cameraRect.getLy()) ||
      camera->getRes().lx <= 0 || camera->getRes().ly <= 0) {
    DVGui::error(tr("The camera has an invalid capture resolution."));
    return;
  }
  const TAffine cameraDpi = getDpiAffine(camera).inv();
  const TAffine outputAffine = output->getRenderSettings().m_affine;
  if (!validBakeAffine(cameraDpi) || !validBakeAffine(outputAffine)) {
    DVGui::error(tr("The camera or output transform is singular or invalid."));
    return;
  }
  // Construct the viewport from integer output dimensions just as the scene
  // renderer does. A DPI round trip can add a pixel through floating-point ceil.
  const TRectD cameraPixels = outputAffine * TRectD(
      -.5 * camera->getRes().lx, -.5 * camera->getRes().ly,
       .5 * camera->getRes().lx,  .5 * camera->getRes().ly);
  QDialog dialog(app->getMainWindow());
  dialog.setWindowTitle(tr("Bake Particle Effect"));
  auto form = new QFormLayout(&dialog);
  auto first = new QSpinBox(&dialog);
  auto last = new QSpinBox(&dialog);
  first->setRange(1, 1000000);
  last->setRange(1, 1000000);
  first->setValue(1);
  last->setValue(std::max(1, xsheet->getFrameCount()));
  auto precision = new QComboBox(&dialog);
  precision->addItem(tr("8-bit per channel"), 32);
  precision->addItem(tr("16-bit per channel"), 64);
  precision->setCurrentIndex(output->getRenderSettings().m_bpp == 32 ? 0 : 1);
  auto framing = new QComboBox(&dialog);
  framing->addItem(tr("Camera framing"));
  framing->addItem(tr("Custom FX area"));
  auto width = numberBox(&dialog, cameraRect.getLx());
  auto height = numberBox(&dialog, cameraRect.getLy());
  auto centerX = numberBox(&dialog, (cameraRect.x0 + cameraRect.x1) * .5, -1000000);
  auto centerY = numberBox(&dialog, (cameraRect.y0 + cameraRect.y1) * .5, -1000000);
  auto useCache = new QCheckBox(tr("Use the completed cache in this scene"), &dialog);
  useCache->setChecked(true);
  auto path = new QLineEdit(&dialog);
  auto browse = new QPushButton(tr("Choose parent folder..."), &dialog);
  path->setPlaceholderText(tr("New cache folder"));
  form->addRow(tr("First frame"), first);
  form->addRow(tr("Last frame"), last);
  form->addRow(tr("Artwork precision"), precision);
  form->addRow(tr("Capture area"), framing);
  form->addRow(tr("Capture width (drawing units)"), width);
  form->addRow(tr("Capture height (drawing units)"), height);
  form->addRow(tr("Capture center X"), centerX);
  form->addRow(tr("Capture center Y"), centerY);
  form->addRow(tr("Destination"), path);
  form->addRow(browse);
  form->addRow(useCache);
  auto note = new QLabel(tr("Camera framing captures the output camera area at every frame, "
                           "including animated column and camera placement. Custom FX area "
                           "keeps the entered bounds fixed in the effect's coordinates. "
                           "The cache includes prepared artwork, "
                           "trails and blur. The original particle effect remains editable "
                           "in the schematic. Existing folders will not be overwritten."), &dialog);
  note->setWordWrap(true);
  form->addRow(note);
  auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
  buttons->button(QDialogButtonBox::Ok)->setText(tr("Bake"));
  form->addRow(buttons);
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  QObject::connect(browse, &QPushButton::clicked, &dialog, [&] {
    QString folder = QFileDialog::getExistingDirectory(&dialog, tr("Choose Parent Folder"));
    if (!folder.isEmpty()) path->setText(QDir(folder).filePath("particle-cache"));
  });
  bool customAreaEdited = false, settingArea = false;
  const std::array<QDoubleSpinBox *, 4> areaFields{{width, height, centerX, centerY}};
  const auto seedCustomArea = [&] {
    if (customAreaEdited) return;
    TRasterFxP native;
    TAffine affine;
    QString failure;
    TRectD localCamera = cameraRect;
    if (prepareParticleBakeSource(scene, xsheet, source, first->value() - 1,
                                  native, affine, failure) && native)
      localCamera = (outputAffine * affine).inv() * cameraPixels;
    if (!std::isfinite(localCamera.x0) || !std::isfinite(localCamera.y0) ||
        !std::isfinite(localCamera.x1) || !std::isfinite(localCamera.y1))
      return;
    settingArea = true;
    width->setValue(localCamera.getLx());
    height->setValue(localCamera.getLy());
    centerX->setValue((localCamera.x0 + localCamera.x1) * .5);
    centerY->setValue((localCamera.y0 + localCamera.y1) * .5);
    settingArea = false;
  };
  for (auto field : areaFields) {
    field->setEnabled(false);
    QObject::connect(field, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                     &dialog, [&](double) { if (!settingArea) customAreaEdited = true; });
  }
  QObject::connect(framing, QOverload<int>::of(&QComboBox::currentIndexChanged),
                   &dialog, [&](int mode) {
    for (auto field : areaFields) field->setEnabled(mode == 1);
  });
  QObject::connect(first, QOverload<int>::of(&QSpinBox::valueChanged), &dialog,
                   [&](int) { seedCustomArea(); });
  seedCustomArea();
  if (dialog.exec() != QDialog::Accepted) return;
  const QString directory = path->text().trimmed();
  if (directory.isEmpty() || first->value() > last->value()) {
    DVGui::error(tr("Choose a new destination folder and a valid frame range."));
    return;
  }
  if (QFileInfo::exists(directory)) {
    DVGui::error(tr("The destination already exists. Choose a new folder."));
    return;
  }

  const int startFrame = first->value() - 1;
  {
    TRasterFxP initialSource;
    TAffine initialAffine;
    QString preparationError;
    if (!prepareParticleBakeSource(scene, xsheet, source, startFrame, initialSource,
                                   initialAffine, preparationError)) {
      DVGui::error(preparationError);
      return;
    }
  }
  ParticleCache::Metadata metadata;
  metadata.firstFrame = 0;
  metadata.lastFrame = last->value() - first->value();
  metadata.framesPerSecond = fps;
  metadata.extra["sceneStartFrame"] = startFrame;
  metadata.extra["sceneFramesPerSecond"] = fps;
  metadata.extra["sourceEffect"] = QString::fromStdString(source->getFxType());
  metadata.extra["sizeSemantics"] = "artwork-scale";
  const bool cameraFraming = framing->currentIndex() == 0;
  metadata.extra["captureFraming"] = cameraFraming ? "camera" : "fx-area";
  ParticleBake::Options options;
  options.framesPerSecond = fps;
  options.proceduralArtworkLastFrame =
      std::max({0, last->value() - 1, xsheet->getFrameCount() - 1});
  metadata.extra["artworkLastFrame"] = options.proceduralArtworkLastFrame;
  options.domain = TRectD(centerX->value() - width->value() * .5,
                          centerY->value() - height->value() * .5,
                          centerX->value() + width->value() * .5,
                          centerY->value() + height->value() * .5);
  TRenderSettings settings = output->getRenderSettings();
  settings.m_shrinkX = settings.m_shrinkY = 1;
  settings.m_bpp = precision->currentData().toInt();
  // Native particle color math is nonlinear. Prepared sprites retain that
  // appearance; the cache effect converts it when rendering a linear scene.
  settings.m_linearColorSpace = false;
  settings.m_cameraBox = cameraPixels;

  QProgressDialog progress(tr("Baking particle effect..."), tr("Cancel"), 0,
                            metadata.lastFrame + 1, app->getMainWindow());
  progress.setWindowModality(Qt::ApplicationModal);
  progress.setMinimumDuration(0);
  const auto cancel = [&] { QCoreApplication::processEvents(); return progress.wasCanceled(); };
  const auto update = [&](int done, int total) {
    progress.setMaximum(total); progress.setValue(done);
  };
  QString error;
  const auto provider = [&](int index, ParticleBake::Frame &frame, QString &failure) {
    const int time = startFrame + index;
    try {
      TRasterFxP raster;
      TAffine affine;
      if (!prepareParticleBakeSource(scene, xsheet, source, time, raster, affine, failure))
        return false;
      TRenderSettings frameSettings(settings);
      ParticleBake::Options frameOptions(options);
      frameSettings.m_affine = settings.m_affine * (raster ? affine : cameraDpi);
      if (cameraFraming) {
        frameOptions.pixelDomain = cameraPixels;
        frameOptions.domain = frameSettings.m_affine.inv() * cameraPixels;
      }
      if (!raster) {
        frame = ParticleBake::Frame();
        frame.frame = time;
        frame.domain = frameOptions.domain;
        const TRectD pixels = cameraFraming ? cameraPixels
            : frameSettings.m_affine * frameOptions.domain;
        frame.pixelDomain = cameraFraming
            ? TRectD(pixels.x0, pixels.y0, pixels.x0 + std::ceil(pixels.getLx()),
                      pixels.y0 + std::ceil(pixels.getLy()))
            : TRectD(std::floor(pixels.x0), std::floor(pixels.y0),
                      std::ceil(pixels.x1), std::ceil(pixels.y1));
        frame.renderAffine = frameSettings.m_affine;
        frame.bitsPerPixel = frameSettings.m_bpp;
        frame.linearColorSpace = frameSettings.m_linearColorSpace;
        frame.colorSpaceGamma = frameSettings.m_colorSpaceGamma;
        return true;
      }
      struct RenderContext {
        TRenderer renderer;
        unsigned long id;
        double frame;
        explicit RenderContext(double time)
            : renderer(1), id(TRenderer::buildRenderId()), frame(time) {
          renderer.install(id);
          renderer.declareRenderStart(id);
          renderer.declareFrameStart(frame);
        }
        ~RenderContext() {
          renderer.declareFrameEnd(frame);
          renderer.declareRenderEnd(id);
          renderer.uninstall();
        }
      } context(time);
      return ParticleBake::captureFrame(raster.getPointer(), time, frameSettings,
                                        frameOptions, frame, failure, cancel);
    } catch (const std::exception &e) {
      failure = QString::fromUtf8(e.what());
      return false;
    } catch (...) {
      failure = tr("The particle effect could not be evaluated at frame %1.").arg(time + 1);
      return false;
    }
  };
  if (!ParticleBake::writePackage(directory, metadata, provider, error, cancel, update)) {
    if (!progress.wasCanceled()) DVGui::error(error);
    return;
  }
  if (!useCache->isChecked()) return;
  TFxP cache = TFx::create("STD_particleCacheFx");
  if (!cache) { DVGui::error(tr("The cache was saved, but its effect is unavailable.")); return; }
  setString(cache.getPointer(), "cacheFile", encodedPath(QDir(directory).filePath("cache.otpc")));
  setNumber(cache.getPointer(), "startFrame", startFrame);
  setNumber(cache.getPointer(), "sceneFps", fps);
  usePackageSizeSemantics(cache.getPointer(), metadata);
  connectArtwork(source.getPointer(), cache.getPointer());
  xsheet->getFxDag()->assignUniqueId(cache.getPointer());
  bindSceneGrammar(xsheet, cache->getParams());
  cache->getAttributes()->setIsOpened(xsheet->getFxDag()->getDagGridDimension() == 0);
  cache->getAttributes()->setDagNodePos(source->getAttributes()->getDagNodePos());
  auto undo = std::unique_ptr<UseParticleCacheUndo>(
      new UseParticleCacheUndo(xsheet, source.getPointer(), cache.getPointer()));
  undo->redo();
  TUndoManager::manager()->add(undo.release());
}

class ImportParticleCacheCommand final : public MenuItemHandler {
public:
  ImportParticleCacheCommand() : MenuItemHandler(MI_ImportParticleCache) {}
  void execute() override { importCache(); }
} importParticleCacheCommand;

class InspectParticleCacheCommand final : public MenuItemHandler {
public:
  InspectParticleCacheCommand() : MenuItemHandler(MI_InspectParticleCache) {}
  void execute() override { inspectCache(); }
} inspectParticleCacheCommand;

class ExportParticleCacheCommand final : public MenuItemHandler {
public:
  ExportParticleCacheCommand() : MenuItemHandler(MI_ExportParticleCache) {}
  void execute() override { exportCache(); }
} exportParticleCacheCommand;

class BakeParticleEffectCommand final : public MenuItemHandler {
public:
  BakeParticleEffectCommand() : MenuItemHandler(MI_BakeParticleEffect) {}
  void execute() override { bakeParticles(); }
} bakeParticleEffectCommand;

}  // namespace
