#include "exportlayeredpsdpopup.h"

// Tnz6 includes
#include "tapp.h"
#include "menubarcommandids.h"

// TnzQt includes
#include "toonzqt/filefield.h"
#include "toonzqt/intfield.h"
#include "toonzqt/menubarcommand.h"

// TnzLib includes
#include "toonz/tcamera.h"
#include "toonz/scenefx.h"
#include "toonz/sceneproperties.h"
#include "toonz/toonzscene.h"
#include "toonz/tscenehandle.h"
#include "toonz/tstageobject.h"
#include "toonz/txsheet.h"
#include "toonz/txsheethandle.h"
#include "toonz/txshcolumn.h"

// TnzBase includes
#include "trasterfx.h"
#include "trenderer.h"
#include "toutputproperties.h"

// TnzCore includes
#include "tconvert.h"
#include "tenv.h"
#include "texception.h"
#include "timage_io.h"
#include "tpsdlayerwriter.h"
#include "tsystem.h"

// Qt includes
#include <QCheckBox>
#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMutex>
#include <QMutexLocker>
#include <QPushButton>
#include <QThread>
#include <QVBoxLayout>

#include <algorithm>
#include <atomic>
#include <map>
#include <vector>

namespace {

TEnv::StringVar LayeredPsdExportFolder("LayeredPsdExportFolder", "");
TEnv::IntVar LayeredPsdExportHidden("LayeredPsdExportHidden", 0);

//-----------------------------------------------------------------------------

// A top-level column exported as a layer
struct LayerJob {
  int m_column;
  QString m_name;
  bool m_visible;  // false for a column which the render does not show
};

//-----------------------------------------------------------------------------

QString getColumnName(TXsheet *xsh, int col) {
  QString name;
  if (TStageObject *obj = xsh->getStageObject(TStageObjectId::ColumnId(col)))
    name = QString::fromStdWString(::to_wstring(obj->getName()));
  if (name.isEmpty()) name = QString("Col%1").arg(col + 1);
  return name;
}

//-----------------------------------------------------------------------------

// Isolates the columns for the render by switching the 'preview visible' eye
// flag (as the script Renderer does), and restores it when going out of scope.
class ColumnVisibilityGuard {
  TXsheet *m_xsh;
  std::vector<bool> m_states;

public:
  explicit ColumnVisibilityGuard(TXsheet *xsh) : m_xsh(xsh) {
    for (int i = 0; i < m_xsh->getColumnCount(); ++i) {
      TXshColumn *column = m_xsh->getColumn(i);
      m_states.push_back(column ? column->isPreviewVisible() : true);
    }
  }
  ~ColumnVisibilityGuard() {
    for (int i = 0; i < (int)m_states.size(); ++i)
      if (TXshColumn *column = m_xsh->getColumn(i))
        column->setPreviewVisible(m_states[i]);
  }

  void showOnly(int index) {
    for (int i = 0; i < (int)m_states.size(); ++i)
      if (TXshColumn *column = m_xsh->getColumn(i))
        column->setPreviewVisible(i == index);
  }
};

//-----------------------------------------------------------------------------

// Collects the rendered rasters of a set of frames
class FrameRenderPort final : public TRenderPort {
  QMutex m_mutex;

public:
  std::atomic<bool> m_finished;
  std::atomic<bool> m_failed;
  std::map<int, TRasterP> m_rasters;  // by frame

  FrameRenderPort(const TDimension &resolution)
      : m_finished(false), m_failed(false) {
    double rx = resolution.lx * 0.5;
    double ry = resolution.ly * 0.5;
    setRenderArea(TRectD(-rx, -ry, rx, ry));
  }

  void onRenderRasterCompleted(const RenderData &renderData) override {
    if (!renderData.m_rasA) {
      m_failed = true;
      return;
    }
    TRasterP ras = renderData.m_rasA->clone();
    QMutexLocker locker(&m_mutex);
    for (double frame : renderData.m_frames) m_rasters[qRound(frame)] = ras;
  }

  void onRenderFailure(const RenderData &, TException &) override {
    m_failed = true;
  }

  void onRenderFinished(bool isCanceled = false) override {
    if (isCanceled) m_failed = true;
    m_finished = true;
  }
};

//-----------------------------------------------------------------------------

// A transparent raster for a column without a drawing in the frame
TRasterP makeTransparentRaster(const TDimension &resolution, int bpp) {
  if (bpp >= 64) {
    TRaster64P ras(resolution);
    ras->fill(TPixel64(0, 0, 0, 0));
    return ras;
  }
  TRaster32P ras(resolution);
  ras->fill(TPixel32(0, 0, 0, 0));
  return ras;
}

//-----------------------------------------------------------------------------

QString safeFileName(QString name) {
  const QString invalid = QStringLiteral("\\/:*?\"<>|");
  for (QChar &ch : name)
    if (invalid.contains(ch) || ch.unicode() < 0x20) ch = QLatin1Char('_');
  return name.trimmed();
}

}  // namespace

//*********************************************************************************
//    Export Layered PSD Popup  implementation
//*********************************************************************************

ExportLayeredPsdPopup::ExportLayeredPsdPopup()
    : DVGui::Dialog(TApp::instance()->getMainWindow(), false, false,
                    "ExportLayeredPsd") {
  setWindowTitle(tr("Export Layered PSD"));

  m_folderField = new DVGui::FileField(this, QString());
  m_folderField->setFileMode(QFileDialog::Directory);
  m_nameField = new QLineEdit(this);
  m_fromField = new DVGui::IntLineEdit(this, 1, 1);
  m_toField   = new DVGui::IntLineEdit(this, 1, 1);
  m_hiddenCB  = new QCheckBox(tr("Include Hidden Columns"), this);

  m_nameField->setToolTip(
      tr("The files are named <name>_0001.psd, <name>_0002.psd and so on"));
  m_hiddenCB->setToolTip(
      tr("Also export the columns whose render (eye) toggle is off, as hidden "
         "layers"));

  QPushButton *exportButton = new QPushButton(tr("Export"), this);
  QPushButton *cancelButton = new QPushButton(tr("Cancel"), this);
  exportButton->setFocusPolicy(Qt::NoFocus);
  cancelButton->setFocusPolicy(Qt::NoFocus);

  QGridLayout *gridLay = new QGridLayout();
  gridLay->setContentsMargins(0, 0, 0, 0);
  gridLay->setHorizontalSpacing(5);
  gridLay->setVerticalSpacing(5);
  {
    gridLay->addWidget(new QLabel(tr("Save In:"), this), 0, 0,
                       Qt::AlignRight | Qt::AlignVCenter);
    gridLay->addWidget(m_folderField, 0, 1, 1, 3);

    gridLay->addWidget(new QLabel(tr("Name:"), this), 1, 0,
                       Qt::AlignRight | Qt::AlignVCenter);
    gridLay->addWidget(m_nameField, 1, 1, 1, 3);

    gridLay->addWidget(new QLabel(tr("Frames:"), this), 2, 0,
                       Qt::AlignRight | Qt::AlignVCenter);
    gridLay->addWidget(m_fromField, 2, 1);
    gridLay->addWidget(new QLabel(tr("to"), this), 2, 2, Qt::AlignCenter);
    gridLay->addWidget(m_toField, 2, 3);

    gridLay->addWidget(m_hiddenCB, 3, 1, 1, 3);
  }
  gridLay->setColumnStretch(1, 1);
  gridLay->setColumnStretch(3, 1);
  m_topLayout->addLayout(gridLay, 0);

  QHBoxLayout *buttonsLay = new QHBoxLayout();
  buttonsLay->setContentsMargins(5, 10, 5, 5);
  buttonsLay->setSpacing(20);
  {
    buttonsLay->addStretch(1);
    buttonsLay->addWidget(exportButton, 0);
    buttonsLay->addWidget(cancelButton, 0);
    buttonsLay->addStretch(1);
  }
  m_topLayout->addLayout(buttonsLay, 0);

  connect(exportButton, &QPushButton::clicked, this,
          &ExportLayeredPsdPopup::onExport);
  connect(cancelButton, &QPushButton::clicked, this, &QWidget::close);
}

//-----------------------------------------------------------------------------

void ExportLayeredPsdPopup::showEvent(QShowEvent *event) {
  initialize();
  DVGui::Dialog::showEvent(event);
}

//-----------------------------------------------------------------------------

void ExportLayeredPsdPopup::initialize() {
  ToonzScene *scene = TApp::instance()->getCurrentScene()->getScene();
  TXsheet *xsh      = TApp::instance()->getCurrentXsheet()->getXsheet();

  QString folder = QString::fromStdString(LayeredPsdExportFolder);
  if (folder.isEmpty() && !scene->isUntitled())
    folder = scene->getScenePath().getParentDir().getQString();
  m_folderField->setPath(folder);

  m_nameField->setText(safeFileName(
      QString::fromStdWString(scene->isUntitled() ? std::wstring(L"untitled")
                                                  : scene->getSceneName())));

  int r0 = 0, r1 = std::max(0, xsh->getFrameCount() - 1);
  m_fromField->setValue(r0 + 1);
  m_toField->setValue(r1 + 1);

  m_hiddenCB->setChecked(LayeredPsdExportHidden != 0);
}

//-----------------------------------------------------------------------------

void ExportLayeredPsdPopup::saveSettings() {
  LayeredPsdExportFolder = m_folderField->getPath().toStdString();
  LayeredPsdExportHidden = m_hiddenCB->isChecked() ? 1 : 0;
}

//-----------------------------------------------------------------------------

void ExportLayeredPsdPopup::onExport() {
  ToonzScene *scene = TApp::instance()->getCurrentScene()->getScene();
  TXsheet *xsh      = TApp::instance()->getCurrentXsheet()->getXsheet();

  // Output files
  if (m_folderField->getPath().trimmed().isEmpty()) {
    DVGui::error(tr("Please specify the folder to save the files in."));
    return;
  }
  const QString baseName = safeFileName(m_nameField->text());
  if (baseName.isEmpty()) {
    DVGui::error(tr("Please specify a file name."));
    return;
  }
  const TFilePath folder = scene->decodeFilePath(
      TFilePath(m_folderField->getPath().trimmed().toStdWString()));
  if (!QDir().mkpath(folder.getQString())) {
    DVGui::error(
        tr("Unable to create the folder %1.").arg(folder.getQString()));
    return;
  }

  // Frame range (rows)
  const int r0 = m_fromField->getValue() - 1;
  const int r1 = m_toField->getValue() - 1;
  if (r0 < 0 || r1 < r0 || r1 >= xsh->getFrameCount()) {
    DVGui::error(tr("The frame range must be inside the xsheet, which has %1 "
                    "frames.")
                     .arg(xsh->getFrameCount()));
    return;
  }

  // Columns, bottom one first
  const bool includeHidden = m_hiddenCB->isChecked();
  std::vector<LayerJob> jobs;
  for (int col = 0; col < xsh->getColumnCount(); ++col) {
    TXshColumn *column = xsh->getColumn(col);
    if (!column || xsh->isColumnEmpty(col)) continue;
    const int type = column->getColumnType();
    if (type != TXshColumn::eLevelType && type != TXshColumn::eZeraryFxType)
      continue;
    const bool visible = column->isPreviewVisible();
    if (!visible && !includeHidden) continue;
    jobs.push_back({col, getColumnName(xsh, col), visible});
  }
  if (jobs.empty()) {
    DVGui::error(tr("No columns can be exported."));
    return;
  }

  auto framePath = [&](int row) {
    return folder + TFilePath(QString("%1_%2.psd")
                                  .arg(baseName)
                                  .arg(row + 1, 4, 10, QChar('0')));
  };

  int existing = 0;
  for (int row = r0; row <= r1; ++row)
    if (QFileInfo::exists(framePath(row).getQString())) ++existing;
  if (existing > 0) {
    std::vector<QString> buttons = {tr("Cancel"), tr("Overwrite")};
    if (DVGui::MsgBox(
            DVGui::WARNING,
            tr("%1 of the files already exist. Overwrite them?").arg(existing),
            buttons) != 2)
      return;
  }

  saveSettings();

  // Render settings: the camera and options of the output settings, at full
  // size and without fields
  TOutputProperties *oprop = scene->getProperties()->getOutputProperties();
  TRenderSettings settings = oprop->getRenderSettings();
  settings.m_shrinkX = settings.m_shrinkY = 1;
  settings.m_bpp                          = settings.m_bpp >= 64 ? 64 : 32;
  settings.m_stereoscopic                 = false;
  settings.m_fieldPrevalence              = TRenderSettings::NoField;

  TCamera *camera         = scene->getCurrentCamera();
  const TDimension res    = camera->getRes();
  const TPointD cameraDpi = camera->getDpi();

  const int procCount       = TSystem::getProcessorCount();
  const int threadCounts[3] = {1, std::max(1, procCount / 2),
                               std::max(1, procCount)};
  const int threadIndex     = std::max(0, std::min(2, oprop->getThreadIndex()));
  const int threadCount     = threadCounts[threadIndex];

  TRenderer renderer(threadCount);
  renderer.enablePrecomputing(false);

  const int layerCount = (int)jobs.size();
  const int frameCount = r1 - r0 + 1;
  DVGui::ProgressDialog progress(tr("Exporting..."), tr("Cancel"), 0,
                                 frameCount, this);
  progress.setWindowTitle(tr("Export Layered PSD"));
  progress.setWindowModality(Qt::WindowModal);
  progress.show();
  setEnabled(false);

  QString errorMessage;
  bool canceled = false;

  // The frames are rendered a few at a time, for each layer, and then written,
  // to keep a limited number of rasters in memory
  for (int c0 = r0; c0 <= r1 && errorMessage.isEmpty() && !canceled;
       c0 += threadCount) {
    const int c1 = std::min(r1, c0 + threadCount - 1);
    const int n  = c1 - c0 + 1;

    // The fx trees: [0] is the whole scene (the merged image), then the layers
    std::vector<std::vector<TFxP>> fxs(layerCount + 1, std::vector<TFxP>(n));
    {
      ColumnVisibilityGuard guard(xsh);
      for (int i = 0; i < n; ++i)
        fxs[0][i] = ::buildSceneFx(scene, xsh, c0 + i,
                                   TOutputProperties::AllLevels, 1, false);
      for (int j = 0; j < layerCount; ++j) {
        guard.showOnly(jobs[j].m_column);
        for (int i = 0; i < n; ++i)
          fxs[j + 1][i] = ::buildSceneFx(
              scene, xsh, c0 + i, TOutputProperties::AllLevels, 1, false);
      }
    }

    std::vector<std::map<int, TRasterP>> rasters(layerCount + 1);
    for (int j = 0; j <= layerCount && errorMessage.isEmpty(); ++j) {
      if (progress.wasCanceled()) {
        canceled = true;
        break;
      }
      progress.setLabelText(
          j == 0 ? tr("Rendering the scene...")
                 : tr("Rendering layer: %1").arg(jobs[j - 1].m_name));
      QCoreApplication::processEvents();

      auto *renderData = new std::vector<TRenderer::RenderData>;
      for (int i = 0; i < n; ++i) {
        if (!fxs[j][i]) continue;  // nothing in this frame
        TFxPair fxPair;
        fxPair.m_frameA = fxs[j][i];
        renderData->push_back(TRenderer::RenderData(c0 + i, settings, fxPair));
      }
      if (renderData->empty()) {
        delete renderData;
        continue;
      }

      FrameRenderPort port(res);
      renderer.addPort(&port);
      renderer.startRendering(renderData);
      while (!port.m_finished) {
        QCoreApplication::processEvents();
        QThread::msleep(5);
      }
      renderer.removePort(&port);
      if (port.m_failed)
        errorMessage = tr("Unable to render the scene.");
      else
        rasters[j] = port.m_rasters;
    }
    if (!errorMessage.isEmpty() || canceled) break;

    // One layered file for each frame
    try {
      for (int i = 0; i < n; ++i) {
        const int row  = c0 + i;
        auto getRaster = [&](int j) {
          auto it = rasters[j].find(row);
          if (it != rasters[j].end() && it->second) return it->second;
          return makeTransparentRaster(res, settings.m_bpp);
        };

        std::vector<TLayeredPsdLayer> layers;
        for (int j = 0; j < layerCount; ++j) {
          TLayeredPsdLayer layer;
          layer.m_name    = jobs[j].m_name.toStdWString();
          layer.m_raster  = getRaster(j + 1);
          layer.m_visible = jobs[j].m_visible;
          layers.push_back(layer);
        }
        writeLayeredPsd(framePath(row), getRaster(0), layers, cameraDpi.x,
                        cameraDpi.y);

        progress.setValue(row - r0 + 1);
        QCoreApplication::processEvents();
      }
    } catch (const TException &e) {
      errorMessage = tr("Unable to save the PSD file: %1")
                         .arg(QString::fromStdWString(e.getMessage()));
    }
  }

  setEnabled(true);
  progress.close();

  if (!errorMessage.isEmpty())
    DVGui::error(errorMessage);
  else if (!canceled)
    DVGui::info(tr("%1 layered PSD file(s) exported to %2.")
                    .arg(frameCount)
                    .arg(folder.getQString()));
}

//*********************************************************************************
//    Export Layered PSD Command  instantiation
//*********************************************************************************

OpenPopupCommandHandler<ExportLayeredPsdPopup> ExportLayeredPsdPopupCommand(
    MI_ExportLayeredPSD);
