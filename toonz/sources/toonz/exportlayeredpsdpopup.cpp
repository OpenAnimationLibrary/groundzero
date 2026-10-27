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
#include "toonz/fxdag.h"
#include "toonz/scenefx.h"
#include "toonz/tcolumnfx.h"
#include "toonz/tcolumnfxset.h"
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
#include "tzeraryfx.h"

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
#include <QMainWindow>
#include <QLineEdit>
#include <QMutex>
#include <QMutexLocker>
#include <QPushButton>
#include <QThread>
#include <QVBoxLayout>

#include <algorithm>
#include <atomic>
#include <map>
#include <set>
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
  TFxP m_root;     // the top of the column's own fx chain, see getChainRoot()
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

// Switches the 'preview visible' eye flag of the hidden columns which are
// exported, so that their fx trees are built, and restores the flags when
// going out of scope.
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

  void setVisible(int index, bool visible) {
    if (TXshColumn *column = m_xsh->getColumn(index))
      column->setPreviewVisible(visible);
  }
};

//-----------------------------------------------------------------------------

// The top of the fx chain which belongs to a column alone: from the column fx
// up through the fxs which have no other input, to the fx connected to the
// xsheet node, or to the last fx before a branch or before an fx which also
// takes other inputs (as Matte In does). Null when the chain is not rendered
// (connected neither to the xsheet nor to the output nor to another fx).
TFx *getChainRoot(TXsheet *xsh, TFx *fx) {
  FxDag *dag = xsh->getFxDag();
  std::set<TFx *> dagFxs;
  dag->getInternalFxs()->getFxs(dagFxs);
  while (fx) {
    if (dag->getTerminalFxs()->containsFx(fx)) return fx;

    // The fxs which take fx as an input. The fxs of the dag only: the render
    // trees built for previews have clones connected to it too.
    int links = 0;
    TFx *next = 0;
    for (TFx *dagFx : dagFxs)
      for (int p = 0; p < dagFx->getInputPortCount(); ++p)
        if (dagFx->getInputPort(p)->getFx() == fx) {
          ++links;
          next = dagFx;
        }
    for (int o = 0; o < dag->getOutputFxCount(); ++o) {
      TFx *outputFx = dag->getOutputFx(o);
      if (outputFx && outputFx->getInputPortCount() > 0 &&
          outputFx->getInputPort(0)->getFx() == fx) {
        ++links;
        next = 0;
      }
    }
    // the inputs of a particles column are ports of its zerary fx
    for (int c = 0; c < xsh->getColumnCount(); ++c) {
      TXshColumn *column = xsh->getColumn(c);
      TZeraryColumnFx *zcfx =
          column ? dynamic_cast<TZeraryColumnFx *>(column->getFx()) : 0;
      TFx *zeraryFx = zcfx ? zcfx->getZeraryFx() : 0;
      if (!zeraryFx) continue;
      for (int p = 0; p < zeraryFx->getInputPortCount(); ++p)
        if (zeraryFx->getInputPort(p)->getFx() == fx) {
          ++links;
          next = 0;
        }
    }
    if (links == 0) return 0;
    if (links > 1 || !next) return fx;

    for (int p = 0; p < next->getInputPortCount(); ++p) {
      TFx *input = next->getInputPort(p)->getFx();
      if (input && input != fx) return fx;
    }
    fx = next;
  }
  return 0;
}

//-----------------------------------------------------------------------------

// Connects fx in place of the xsheet node in tree. Returns false when tree has
// no xsheet node.
bool replaceXsheetNode(TFx *tree, TFx *fx) {
  bool replaced = false;
  for (int i = 0; i < tree->getInputPortCount(); ++i) {
    TFxPort *port = tree->getInputPort(i);
    TFx *input    = port->getFx();
    if (!input) continue;
    if (dynamic_cast<TXsheetFx *>(input)) {
      port->setFx(fx);
      replaced = true;
    } else if (replaceXsheetNode(input, fx))
      replaced = true;
  }
  return replaced;
}

//-----------------------------------------------------------------------------

// The render tree of a layer: the fx chain of a column (root), with the column
// placement, under the fxs between the xsheet node and the output (postFx,
// built by buildPostSceneFx()), as if the chain were alone connected to the
// xsheet node. Unlike buildSceneFx(), it has no background color card.
TFxP buildLayerFx(ToonzScene *scene, TXsheet *xsh, int row, const TFxP &root,
                  const TFxP &postFx) {
  if (!root || !postFx) return TFxP();  // nothing is rendered

  // The chain alone, with the camera transforms: when there are no fxs after
  // the xsheet node, or when the chain is connected to the output itself
  FxDag *dag     = xsh->getFxDag();
  TFx *outputFx  = dag->getOutputFxCount() > 0 ? dag->getOutputFx(0) : 0;
  bool chainOnly = dynamic_cast<TXsheetFx *>(postFx.getPointer()) ||
                   (outputFx && outputFx->getInputPortCount() > 0 &&
                    outputFx->getInputPort(0)->getFx() == root.getPointer());
  if (chainOnly)
    return ::buildSceneFx(scene, (double)row, xsh, root, BSFX_DEFAULT_TR, false,
                          TOutputProperties::AllLevels, 1);

  TFxP fx = ::buildSceneFx(scene, (double)row, xsh, root, BSFX_COLUMN_TR, false,
                           TOutputProperties::AllLevels, 1);
  if (!fx) return fx;
  // The post-xsheet tree is cloned, as the multimedia render does
  TFxP tree(postFx->clone(true));
  if (replaceXsheetNode(tree.getPointer(), fx.getPointer())) return tree;
  return TFxP();  // the output does not take the xsheet node
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
    jobs.push_back({col, getColumnName(xsh, col), visible,
                    getChainRoot(xsh, column->getFx())});
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
      std::vector<TFxP> postFxs(n);
      for (int i = 0; i < n; ++i)
        postFxs[i] = ::buildPostSceneFx(scene, xsh, c0 + i, 1, false);
      for (int j = 0; j < layerCount; ++j) {
        if (!jobs[j].m_visible) guard.setVisible(jobs[j].m_column, true);
        for (int i = 0; i < n; ++i)
          fxs[j + 1][i] =
              buildLayerFx(scene, xsh, c0 + i, jobs[j].m_root, postFxs[i]);
        if (!jobs[j].m_visible) guard.setVisible(jobs[j].m_column, false);
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
