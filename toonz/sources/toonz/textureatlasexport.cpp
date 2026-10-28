// Export Texture Atlas: packs the drawings of the current xsheet into atlas
// pages and writes, next to the page images, JSON files describing the
// sprites and the timelines of the layers, in the style of the Texture Atlas
// of Adobe Animate (spritemap1.png, spritemap1.json, Animation.json) that
// game engine plugins read. See doc/animate_texture_atlas.md.
//
// Mapping:
//  - Layers: the preview visible level columns of the current xsheet, top
//    one (the last column) first, named after the columns.
//  - Sprites: each drawing (level and frame) shown in the frame range, of
//    the Toonz Vector, Toonz Raster and Raster levels, rendered at the camera
//    resolution and trimmed to its visible pixels as the Spine export does
//    (renderTrimmedDrawing(), see spineexport.cpp), optionally scaled down,
//    and packed into pages with AtlasPacker. The pixels are copied into the
//    pages, premultiplied, and saved as PNG (which un-premultiplies them).
//  - Frames: a run of frames of a column which shows the same drawing with the
//    same transform is one frame entry with its duration. Empty cells are
//    blank frames. The transform of each frame is the column placement
//    relative to the camera, baked frame by frame, with the trim offset of the
//    sprite and the compensation of the scale, so that the sprite lands where
//    the render shows the drawing. The stage is the camera frame, in camera
//    pixels, y down from its top left corner. Cells are read through the time
//    remap of the column, as the render does.
//
// Not exported: sub-xsheets, fxs, column transparency and color filters,
// Z depth and stacking order changes, Plastic deformations.

#include "textureatlasexport.h"

// Tnz6 includes
#include "tapp.h"
#include "menubarcommandids.h"

// TnzQt includes
#include "toonzqt/menubarcommand.h"
#include "toonzqt/dvdialog.h"
#include "toonzqt/gutil.h"

// TnzLib includes
#include "toonz/atlasjson.h"
#include "toonz/atlaspacker.h"
#include "toonz/toonzscene.h"
#include "toonz/tproject.h"
#include "toonz/sceneproperties.h"
#include "toonz/tscenehandle.h"
#include "toonz/txsheethandle.h"
#include "toonz/txsheet.h"
#include "toonz/txshcolumn.h"
#include "toonz/txshlevelcolumn.h"
#include "toonz/txshcell.h"
#include "toonz/txshsimplelevel.h"
#include "toonz/txshleveltypes.h"
#include "toonz/tstageobject.h"
#include "toonz/tstageobjecttree.h"
#include "toonz/tcamera.h"
#include "toonz/dpiscale.h"
#include "toonz/fxdag.h"
#include "toonz/tcolumnfxset.h"
#include "toutputproperties.h"

// TnzCore includes
#include "tenv.h"
#include "tsystem.h"

// Qt includes
#include <QComboBox>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QGridLayout>
#include <QImage>
#include <QLabel>
#include <QMainWindow>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QSpinBox>
#include <QStringList>
#include <QTransform>
#include <QUrl>

// STD includes
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <utility>
#include <vector>

namespace {

TEnv::StringVar TextureAtlasExportFolder("TextureAtlasExportFolder", "");
TEnv::IntVar TextureAtlasExportPageSize("TextureAtlasExportPageSize", 2048);
TEnv::IntVar TextureAtlasExportPadding("TextureAtlasExportPadding", 2);
TEnv::IntVar TextureAtlasExportScale("TextureAtlasExportScale", 100);

const char *c_animationFileName = "Animation.json";

// Precision of the written matrices
const double c_positionUnit = 1e-3;  // pixels
const double c_linearUnit   = 1e-6;

//-----------------------------------------------------------------------------

QString atlasTr(const char *text) {
  return QCoreApplication::translate("TextureAtlasExport", text);
}

//-----------------------------------------------------------------------------

double roundTo(double value, double unit) {
  value = std::round(value / unit) * unit;
  return (value == 0.0) ? 0.0 : value;  // no "-0"
}

//-----------------------------------------------------------------------------

// As in the Spine export
QString fileSafeName(const QString &name) {
  QString result;
  for (const QChar &c : name)
    result += (c.isLetterOrNumber() || c == '_' || c == '-') ? c : QChar('_');
  return result.isEmpty() ? QString("_") : result;
}

//-----------------------------------------------------------------------------

// Returns name, or name followed by a number if already used. Names are
// compared ignoring case if ignoreCase. As in the Spine export.
QString uniqueName(const QString &name, QSet<QString> &usedNames,
                   bool ignoreCase = false) {
  QString result = name;
  for (int i = 2;; ++i) {
    QString key = ignoreCase ? result.toLower() : result;
    if (!usedNames.contains(key)) {
      usedNames.insert(key);
      return result;
    }
    result = QString("%1_%2").arg(name).arg(i);
  }
}

//-----------------------------------------------------------------------------

bool isExportedLevelType(int type) {
  return type == PLI_XSHLEVEL || type == TZP_XSHLEVEL || type == OVL_XSHLEVEL;
}

//-----------------------------------------------------------------------------

QString spritemapBaseName(int page) {
  return QString("spritemap%1").arg(page + 1);
}

//=============================================================================

struct AtlasSprite {
  TXshSimpleLevel *m_level;
  TFrameId m_fid;
  QString m_name;

  bool m_valid = false;  //!< Rendered, and not blank
  QImage m_image;        //!< As stored in the page: premultiplied, top row
                         //!< first, scaled
  int m_width = 0, m_height = 0;  //!< Rendered size, before the scale
  TPointD m_center;  //!< Rendered image center in the column reference,
                     //!< camera pixels, y up

  // Place in the atlas
  int m_page = -1;
  int m_x = 0, m_y = 0;
  bool m_rotated = false;
};

struct AtlasColumn {
  int m_index;
  QString m_name;
  std::vector<int> m_frames;  //!< Sprite index per frame, -1 when blank
};

struct AtlasOptions {
  int m_pageSize = 2048;
  int m_padding  = 2;
  int m_scale    = 100;  //!< Percent
};

//-----------------------------------------------------------------------------

// Shows the options; returns false if canceled
bool askOptions(AtlasOptions &options, int layerCount, int spriteCount,
                int subXsheetColumns, int fxCount) {
  DVGui::Dialog dialog(TApp::instance()->getMainWindow(), true);
  dialog.setWindowTitle(atlasTr("Export Texture Atlas"));

  QString text =
      atlasTr(
          "%1 layer(s) and %2 drawing(s) will be exported: the drawings "
          "are packed into atlas pages (spritemap1.png, ...) described by "
          "spritemap1.json, ..., and the timeline of the layers is "
          "written to Animation.json, in the folder chosen next.")
          .arg(layerCount)
          .arg(spriteCount);
  text += "\n\n" + atlasTr("Sub-xsheets and FX are not exported.");
  if (subXsheetColumns > 0 || fxCount > 0)
    text += " " + atlasTr(
                      "This scene has %1 sub-xsheet column(s) and %2 FX, "
                      "which are left out.")
                      .arg(subXsheetColumns)
                      .arg(fxCount);
  QLabel *info = new QLabel(text, &dialog);
  info->setWordWrap(true);
  dialog.addWidget(info);

  QComboBox *sizeCombo  = new QComboBox(&dialog);
  const int pageSizes[] = {1024, 2048, 4096};
  for (int size : pageSizes)
    sizeCombo->addItem(QString("%1 x %1").arg(size), size);
  int sizeIndex = sizeCombo->findData(options.m_pageSize);
  sizeCombo->setCurrentIndex(sizeIndex >= 0 ? sizeIndex : 1);

  QSpinBox *paddingField = new QSpinBox(&dialog);
  paddingField->setRange(0, 32);
  paddingField->setValue(std::max(0, std::min(32, options.m_padding)));
  paddingField->setToolTip(
      atlasTr("Empty pixels between the sprites of a page"));

  QComboBox *scaleCombo = new QComboBox(&dialog);
  scaleCombo->addItem("100%", 100);
  scaleCombo->addItem("50%", 50);
  int scaleIndex = scaleCombo->findData(options.m_scale);
  scaleCombo->setCurrentIndex(scaleIndex >= 0 ? scaleIndex : 0);
  scaleCombo->setToolTip(
      atlasTr("Size of the sprite pixels; the transforms make up for it, so "
              "that the sprites keep their size on the stage"));

  QGridLayout *grid = new QGridLayout();
  grid->setContentsMargins(0, 5, 0, 0);
  grid->setHorizontalSpacing(5);
  grid->setVerticalSpacing(5);
  grid->addWidget(new QLabel(atlasTr("Maximum Page Size:"), &dialog), 0, 0,
                  Qt::AlignRight | Qt::AlignVCenter);
  grid->addWidget(sizeCombo, 0, 1);
  grid->addWidget(new QLabel(atlasTr("Padding (pixels):"), &dialog), 1, 0,
                  Qt::AlignRight | Qt::AlignVCenter);
  grid->addWidget(paddingField, 1, 1);
  grid->addWidget(new QLabel(atlasTr("Scale:"), &dialog), 2, 0,
                  Qt::AlignRight | Qt::AlignVCenter);
  grid->addWidget(scaleCombo, 2, 1);
  grid->setColumnStretch(1, 1);
  dialog.m_topLayout->addLayout(grid);

  QPushButton *exportButton = new QPushButton(atlasTr("Export..."), &dialog);
  QPushButton *cancelButton = new QPushButton(atlasTr("Cancel"), &dialog);
  exportButton->setDefault(true);
  dialog.addButtonBarWidget(exportButton, cancelButton);
  QObject::connect(exportButton, SIGNAL(clicked()), &dialog, SLOT(accept()));
  QObject::connect(cancelButton, SIGNAL(clicked()), &dialog, SLOT(reject()));

  if (dialog.exec() != QDialog::Accepted) return false;

  options.m_pageSize = sizeCombo->currentData().toInt();
  options.m_padding  = paddingField->value();
  options.m_scale    = scaleCombo->currentData().toInt();
  return true;
}

//-----------------------------------------------------------------------------

// Asks for the output folder; returns an empty string if canceled
QString askFolder(ToonzScene *scene) {
  QString start = QString::fromStdString(std::string(TextureAtlasExportFolder));
  if (start.isEmpty() || !QDir(start).exists()) {
    TFilePath folder = scene->isUntitled()
                           ? scene->decodeFilePath(TProjectManager::instance()
                                                       ->getCurrentProject()
                                                       ->getScenesPath())
                           : scene->getScenePath().getParentDir();
    start            = folder.getQString();
  }
  return QFileDialog::getExistingDirectory(
      TApp::instance()->getMainWindow(),
      atlasTr("Export Texture Atlas: Choose the Folder"), start);
}

//-----------------------------------------------------------------------------

// Renders the drawing of the sprite into m_image; returns false for a blank
// or unreadable drawing
bool renderSprite(AtlasSprite &sprite, const TCamera *camera,
                  const TAffine &stageToPixels, int scalePercent) {
  TPointD center;
  TRaster32P ras = renderTrimmedDrawing(sprite.m_level, sprite.m_fid, camera,
                                        stageToPixels, center);
  if (!ras || ras->getLx() <= 0 || ras->getLy() <= 0) return false;

  // A new image, top row first, with the premultiplied pixels of the raster
  QImage image = rasterToQImage(ras, true, true);
  if (image.isNull()) return false;
  image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);

  sprite.m_width  = image.width();
  sprite.m_height = image.height();
  sprite.m_center = center;

  if (scalePercent != 100) {
    // Smooth scaling of premultiplied pixels does not darken the edges
    int w = std::max(1, (int)std::lround(image.width() * scalePercent / 100.0));
    int h =
        std::max(1, (int)std::lround(image.height() * scalePercent / 100.0));
    image = image.scaled(w, h, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                .convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (image.isNull()) return false;
  }

  sprite.m_image = image;
  sprite.m_valid = true;
  return true;
}

//-----------------------------------------------------------------------------

// The image of an atlas page: the sprites' pixels copied (not blended) into a
// transparent image
QImage composePage(int width, int height,
                   const std::vector<AtlasSprite> &sprites, int page) {
  QImage image(width, height, QImage::Format_ARGB32_Premultiplied);
  if (image.isNull()) return image;
  image.fill(Qt::transparent);

  for (const AtlasSprite &sprite : sprites) {
    if (!sprite.m_valid || sprite.m_page != page) continue;

    // Rotated sprites are stored turned 90 degrees clockwise
    QImage src = sprite.m_rotated
                     ? sprite.m_image.transformed(QTransform().rotate(90))
                     : sprite.m_image;
    src        = src.convertToFormat(QImage::Format_ARGB32_Premultiplied);

    int x0    = sprite.m_x;
    int count = std::min(src.width(), width - x0);
    if (x0 < 0 || count <= 0) continue;
    for (int y = 0; y < src.height(); ++y) {
      int py = sprite.m_y + y;
      if (py < 0 || py >= height) continue;
      std::memcpy(image.scanLine(py) + 4 * x0, src.constScanLine(y),
                  4 * (size_t)count);
    }
  }
  return image;
}

//-----------------------------------------------------------------------------

/*!
The matrix placing the sprite on the stage: \e columnToCamera maps the column
reference to the camera reference (both in camera pixels, y up, the camera
centered on its origin), and the stage is the camera frame of size \e res, y
down from its top left corner. The sprite pixels are y down from the top left
corner of the stored (scaled) image.
*/
AtlasJson::Matrix3D spriteMatrix(const AtlasSprite &sprite,
                                 const TAffine &columnToCamera,
                                 const TDimension &res) {
  double sx = (double)sprite.m_width / sprite.m_image.width();
  double sy = (double)sprite.m_height / sprite.m_image.height();
  TAffine spriteToColumn(sx, 0.0, sprite.m_center.x - 0.5 * sprite.m_width, 0.0,
                         -sy, sprite.m_center.y + 0.5 * sprite.m_height);
  TAffine cameraToStage(1.0, 0.0, 0.5 * res.lx, 0.0, -1.0, 0.5 * res.ly);
  TAffine aff = cameraToStage * columnToCamera * spriteToColumn;

  return AtlasJson::matrixFromAffine(
      roundTo(aff.a11, c_linearUnit), roundTo(aff.a21, c_linearUnit),
      roundTo(aff.a12, c_linearUnit), roundTo(aff.a22, c_linearUnit),
      roundTo(aff.a13, c_positionUnit), roundTo(aff.a23, c_positionUnit));
}

//-----------------------------------------------------------------------------

bool writeFile(const QString &path, const QByteArray &data) {
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
  bool ok = file.write(data) == (qint64)data.size();
  file.close();
  return ok;
}

}  // namespace

//=============================================================================
// ExportTextureAtlasCommand
//-----------------------------------------------------------------------------

class ExportTextureAtlasCommand final : public MenuItemHandler {
public:
  ExportTextureAtlasCommand() : MenuItemHandler(MI_ExportTextureAtlas) {}
  void execute() override;
} exportTextureAtlasCommand;

void ExportTextureAtlasCommand::execute() {
  ToonzScene *scene = TApp::instance()->getCurrentScene()->getScene();
  TXsheet *xsh      = TApp::instance()->getCurrentXsheet()->getXsheet();

  // Frame range, as in the Spine export
  TOutputProperties *oprop = scene->getProperties()->getOutputProperties();
  int r0, r1, step;
  if (scene->getTopXsheet() != xsh || !oprop->getRange(r0, r1, step))
    r0 = 0, r1 = xsh->getFrameCount() - 1;
  if (r1 < r0) {
    DVGui::error(atlasTr("No columns can be exported."));
    return;
  }
  const int frameCount = r1 - r0 + 1;
  double fps           = oprop->getFrameRate();
  if (!(fps > 0.0)) fps = 24.0;

  TStageObjectTree *tree  = xsh->getStageObjectTree();
  TStageObjectId cameraId = tree->getCurrentCameraId();
  TCamera *camera         = tree->getCamera(cameraId);
  const TDimension res    = camera->getRes();
  TAffine stageToPixels   = getDpiAffine(camera).inv();
  TAffine pixelsToStage   = getDpiAffine(camera);

  // Layers and used drawings

  std::vector<AtlasColumn> columns;
  std::vector<AtlasSprite> sprites;
  std::map<std::pair<TXshSimpleLevel *, TFrameId>, int> spriteIndices;
  QSet<QString> spriteNames, layerNames;
  int subXsheetColumns = 0;

  for (int col = 0; col < xsh->getColumnCount(); ++col) {
    if (xsh->isColumnEmpty(col)) continue;
    TXshColumn *column = xsh->getColumn(col);
    if (!column || !column->isPreviewVisible()) continue;
    TXshLevelColumn *levelColumn = column->getLevelColumn();
    if (!levelColumn) continue;

    AtlasColumn atlasColumn;
    atlasColumn.m_index = col;
    bool used = false, subXsheet = false;
    for (int r = r0; r <= r1; ++r) {
      TXshCell cell       = levelColumn->getRemappedCell(r);
      TXshSimpleLevel *sl = cell.getSimpleLevel();
      int index           = -1;
      if (sl && isExportedLevelType(sl->getType())) {
        auto key = std::make_pair(sl, cell.getFrameId());
        auto it  = spriteIndices.find(key);
        if (it == spriteIndices.end()) {
          AtlasSprite sprite;
          sprite.m_level = sl;
          sprite.m_fid   = cell.getFrameId();
          QString name   = QString::fromStdWString(sl->getName()) + "_" +
                         QString::fromStdString(sprite.m_fid.expand());
          sprite.m_name = uniqueName(fileSafeName(name), spriteNames, true);
          sprites.push_back(sprite);
          it = spriteIndices.insert({key, (int)sprites.size() - 1}).first;
        }
        index = it->second;
        used  = true;
      } else if (cell.getChildLevel())
        subXsheet = true;
      atlasColumn.m_frames.push_back(index);
    }
    if (subXsheet) ++subXsheetColumns;
    if (!used) continue;

    TStageObject *columnObj =
        xsh->getStageObject(TStageObjectId::ColumnId(col));
    QString name = QString::fromStdString(columnObj->getName());
    if (name.isEmpty()) name = QString("Col%1").arg(col + 1);
    atlasColumn.m_name = uniqueName(name, layerNames);
    columns.push_back(atlasColumn);
  }

  if (columns.empty()) {
    DVGui::error(atlasTr("No columns can be exported."));
    return;
  }

  // Options and folder

  int fxCount = 0;
  if (FxDag *fxDag = xsh->getFxDag())
    if (TFxSet *internalFxs = fxDag->getInternalFxs())
      fxCount = internalFxs->getFxCount();

  AtlasOptions options;
  options.m_pageSize = TextureAtlasExportPageSize;
  options.m_padding  = TextureAtlasExportPadding;
  options.m_scale    = TextureAtlasExportScale;
  if (!askOptions(options, (int)columns.size(), (int)sprites.size(),
                  subXsheetColumns, fxCount))
    return;
  TextureAtlasExportPageSize = options.m_pageSize;
  TextureAtlasExportPadding  = options.m_padding;
  TextureAtlasExportScale    = options.m_scale;

  QString folder = askFolder(scene);
  if (folder.isEmpty()) return;
  TextureAtlasExportFolder = folder.toStdString();
  QDir dir(folder);

  // Sprites

  {
    DVGui::ProgressDialog progressDialog("", atlasTr("Cancel"), 0,
                                         (int)sprites.size());
    progressDialog.setWindowTitle(atlasTr("Exporting..."));
    progressDialog.setWindowModality(Qt::WindowModal);
    progressDialog.show();

    for (int i = 0; i < (int)sprites.size(); ++i) {
      progressDialog.setLabelText(sprites[i].m_name);
      progressDialog.setValue(i);
      QCoreApplication::processEvents();
      if (progressDialog.wasCanceled()) return;
      renderSprite(sprites[i], camera, stageToPixels, options.m_scale);
    }
    progressDialog.close();
  }

  std::vector<int> packed;  // index in sprites of each packed rectangle
  std::vector<AtlasPacker::Size> sizes;
  for (int i = 0; i < (int)sprites.size(); ++i) {
    if (!sprites[i].m_valid) continue;
    packed.push_back(i);
    sizes.push_back(AtlasPacker::Size(sprites[i].m_image.width(),
                                      sprites[i].m_image.height()));
  }
  if (packed.empty()) {
    DVGui::error(atlasTr("The exported columns show no visible drawing."));
    return;
  }

  AtlasPacker::Options packOptions;
  packOptions.m_maxPageSize   = options.m_pageSize;
  packOptions.m_padding       = options.m_padding;
  packOptions.m_allowRotation = false;
  packOptions.m_powerOfTwo    = false;
  AtlasPacker::Result packing = AtlasPacker::pack(sizes, packOptions);

  if (!packing.ok()) {
    QStringList names;
    for (const AtlasPacker::Rejected &rejected : packing.m_rejected) {
      if (names.size() == 5) {
        names << "...";
        break;
      }
      const AtlasSprite &sprite = sprites[packed[rejected.m_index]];
      names << QString("%1 (%2 x %3)")
                   .arg(sprite.m_name)
                   .arg(sprite.m_image.width())
                   .arg(sprite.m_image.height());
    }
    DVGui::error(
        atlasTr("These drawings are larger than the maximum page size (%1 "
                "pixels): %2.\nChoose a larger page size or the 50% scale.")
            .arg(options.m_pageSize)
            .arg(names.join(", ")));
    return;
  }

  for (size_t k = 0; k < packed.size(); ++k) {
    const AtlasPacker::Placement &p = packing.m_placements[k];
    AtlasSprite &sprite             = sprites[packed[k]];
    sprite.m_page                   = p.m_page;
    sprite.m_x                      = p.m_x;
    sprite.m_y                      = p.m_y;
    sprite.m_rotated                = p.m_rotated;
  }

  std::vector<AtlasJson::Page> pages(packing.m_pages.size());
  for (size_t i = 0; i < pages.size(); ++i) {
    pages[i].m_image      = spritemapBaseName((int)i) + ".png";
    pages[i].m_width      = packing.m_pages[i].m_w;
    pages[i].m_height     = packing.m_pages[i].m_h;
    pages[i].m_resolution = options.m_scale / 100.0;
  }
  for (size_t k = 0; k < packed.size(); ++k) {
    const AtlasPacker::Placement &p = packing.m_placements[k];
    AtlasJson::Sprite sprite;
    sprite.m_name    = sprites[packed[k]].m_name;
    sprite.m_x       = p.m_x;
    sprite.m_y       = p.m_y;
    sprite.m_w       = p.m_w;
    sprite.m_h       = p.m_h;
    sprite.m_rotated = p.m_rotated;
    pages[p.m_page].m_sprites.push_back(sprite);
  }

  // Timelines: the camera of every frame, then the frames of every layer, the
  // top one first

  std::vector<TAffine> cameraInverse(frameCount);
  std::vector<char> cameraValid(frameCount, 0);
  for (int f = 0; f < frameCount; ++f) {
    TAffine cameraAff = xsh->getPlacement(cameraId, r0 + f);
    if (std::abs(cameraAff.det()) < 1e-12) continue;
    cameraInverse[f] = cameraAff.inv();
    cameraValid[f]   = 1;
  }

  AtlasJson::Animation animation;
  QString sceneName = QString::fromStdWString(scene->getSceneName());
  if (sceneName.isEmpty()) sceneName = "untitled";
  animation.m_name        = sceneName;
  animation.m_symbolName  = sceneName;
  animation.m_frameRate   = fps;
  animation.m_stageWidth  = res.lx;
  animation.m_stageHeight = res.ly;
  animation.m_frameCount  = frameCount;

  for (int c = (int)columns.size() - 1; c >= 0; --c) {
    const AtlasColumn &column = columns[c];
    TStageObjectId columnId   = TStageObjectId::ColumnId(column.m_index);

    AtlasJson::Layer layer;
    layer.m_name = column.m_name;

    int runStart = 0, runSprite = -1;
    AtlasJson::Matrix3D runMatrix = AtlasJson::identityMatrix();
    auto closeRun                 = [&](int end) {
      if (runSprite >= 0 && end > runStart) {
        AtlasJson::Frame frame;
        frame.m_start    = runStart;
        frame.m_duration = end - runStart;
        frame.m_sprite   = sprites[runSprite].m_name;
        frame.m_matrix   = runMatrix;
        layer.m_frames.push_back(frame);
      }
      runSprite = -1;
    };

    for (int f = 0; f < frameCount; ++f) {
      int index  = column.m_frames[f];
      bool shown = index >= 0 && sprites[index].m_valid && cameraValid[f] != 0;
      AtlasJson::Matrix3D matrix = AtlasJson::identityMatrix();
      if (shown) {
        TAffine placement = xsh->getPlacement(columnId, r0 + f);
        if (std::abs(placement.det()) < 1e-12)
          shown = false;  // squashed to nothing: not visible
        else
          matrix = spriteMatrix(
              sprites[index],
              stageToPixels * cameraInverse[f] * placement * pixelsToStage,
              res);
      }
      if (!shown) {
        closeRun(f);
        continue;
      }
      if (index == runSprite && matrix == runMatrix) continue;
      closeRun(f);
      runStart  = f;
      runSprite = index;
      runMatrix = matrix;
    }
    closeRun(frameCount);

    animation.m_layers.push_back(layer);
  }

  QString problem = AtlasJson::validate(pages, animation);
  if (!problem.isEmpty()) {
    DVGui::error(
        atlasTr("Unable to export the texture atlas: %1").arg(problem));
    return;
  }

  // Ask before overwriting; the pages of an earlier export which this one
  // does not write are removed, so that the folder holds one atlas

  QRegularExpression pageFile("^spritemap(\\d+)\\.(json|png)$",
                              QRegularExpression::CaseInsensitiveOption);
  QStringList existing, stale;
  for (const QString &entry : dir.entryList(QDir::Files)) {
    QRegularExpressionMatch match = pageFile.match(entry);
    if (match.hasMatch()) {
      existing << entry;
      if (match.captured(1).toInt() > (int)pages.size()) stale << entry;
    } else if (entry.compare(QString::fromLatin1(c_animationFileName),
                             Qt::CaseInsensitive) == 0)
      existing << entry;
  }
  if (!existing.isEmpty()) {
    QString question =
        atlasTr(
            "The folder already holds Texture Atlas files (%1). Overwrite "
            "them?")
            .arg(existing.join(", "));
    if (!stale.isEmpty())
      question += "\n" + atlasTr(
                             "The pages which this export does not write "
                             "again (%1) are removed.")
                             .arg(stale.join(", "));
    std::vector<QString> buttons = {atlasTr("Overwrite"), atlasTr("Cancel")};
    if (DVGui::MsgBox(DVGui::WARNING, question, buttons, 1) != 1) return;
    for (const QString &entry : stale) dir.remove(entry);
  }

  // Write

  const QString app     = QString::fromStdString(TEnv::getApplicationName());
  const QString version = QString::fromStdString(TEnv::getApplicationVersion());

  for (size_t i = 0; i < pages.size(); ++i) {
    QImage image =
        composePage(pages[i].m_width, pages[i].m_height, sprites, (int)i);
    QString imagePath = dir.filePath(pages[i].m_image);
    if (image.isNull() || !image.save(imagePath, "PNG")) {
      DVGui::error(atlasTr("Unable to save %1.").arg(imagePath));
      return;
    }
    QString jsonPath = dir.filePath(spritemapBaseName((int)i) + ".json");
    if (!writeFile(jsonPath, AtlasJson::toBytes(AtlasJson::spritemapJson(
                                 pages[i], app, version)))) {
      DVGui::error(atlasTr("Unable to save %1.").arg(jsonPath));
      return;
    }
  }

  QString animationPath =
      dir.filePath(QString::fromLatin1(c_animationFileName));
  if (!writeFile(animationPath,
                 AtlasJson::toBytes(AtlasJson::animationJson(animation)))) {
    DVGui::error(atlasTr("Unable to save %1.").arg(animationPath));
    return;
  }

  QString str = atlasTr(
                    "The texture atlas has been exported: %1 sprite(s) on "
                    "%2 page(s), %3 layer(s).")
                    .arg((int)packed.size())
                    .arg((int)pages.size())
                    .arg((int)animation.m_layers.size());
  std::vector<QString> buttons = {atlasTr("OK"),
                                  atlasTr("Open containing folder")};
  if (DVGui::MsgBox(DVGui::INFORMATION, str, buttons) == 2) {
    TFilePath folderPath(folder.toStdWString());
    if (TSystem::isUNC(folderPath))
      QDesktopServices::openUrl(QUrl(folderPath.getQString()));
    else
      QDesktopServices::openUrl(QUrl::fromLocalFile(folderPath.getQString()));
  }
}
