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
//  - Symbols: each sub-xsheet (TXshChildLevel) shown is one symbol of the
//    symbol dictionary, exported once however many columns show it. Its
//    timeline is built by the same code as the main one, from the rows of
//    the child xsheet seen through its current camera: the symbol
//    coordinates are the frame of the child camera, in its pixels, y down
//    from its top left corner. Its drawings are sprites of the same pages.
//    A sub-xsheet column is a layer whose frames are symbol instances: a run
//    of rows whose child frame (the cell frame number - 1, as in
//    Stage::visit()) advances by one per row is one instance which plays
//    once from its first child frame, or loops if it wraps from the last row
//    of the child xsheet to the first; a held child frame is a single frame
//    instance (AtlasJson::instanceRuns()). The instance matrix is the column
//    placement relative to the camera, as for drawings, with the center of
//    the child camera on the column origin; the child camera placement,
//    scaled by its Z as Stage::visit() does, is inverted in the matrices of
//    the symbol's layers, row by row. Sub-xsheets are followed up to
//    c_maxSymbolDepth levels.
//
// Not exported: fxs, column transparency and color filters, Z depth and
// stacking order changes, Plastic deformations.

#include "textureatlasexport.h"

// Tnz6 includes
#include "tapp.h"
#include "menubarcommandids.h"

// TnzQt includes
#include "toonzqt/menubarcommand.h"
#include "toonzqt/dvdialog.h"
#include "toonzqt/gutil.h"
#include "toonzqt/uitranslations.h"

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
#include "toonz/txshchildlevel.h"
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
#include <set>
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

// Levels of nested sub-xsheets exported as symbols
const int c_maxSymbolDepth = 16;

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

//! What a layer shows at a frame: a sprite, a frame of a symbol, or nothing
struct AtlasCell {
  int m_sprite   = -1;  //!< Index in the sprites, or -1
  int m_symbol   = -1;  //!< Index in the symbols, or -1
  int m_childRow = 0;   //!< Frame of the symbol: row of its xsheet
};

struct AtlasColumn {
  int m_index;
  QString m_name;
  std::vector<AtlasCell> m_frames;  //!< One per frame of the timeline
};

//! The camera a timeline is seen through: the current camera of its xsheet
struct AtlasView {
  TDimension m_res;
  TAffine m_toPixels, m_toStage;   //!< Stage units <-> camera pixels
  std::vector<TAffine> m_inverse;  //!< Inverse camera placement, per frame
  std::vector<char> m_valid;       //!< m_inverse exists, per frame
};

//! The rows [m_r0, m_r0 + m_frameCount) of an xsheet as a timeline
struct AtlasTimeline {
  TXsheet *m_xsheet = nullptr;
  int m_r0 = 0, m_frameCount = 0;
  AtlasView m_view;
  std::vector<AtlasColumn> m_columns;  //!< Left to right
};

//! A sub-xsheet: its whole xsheet, from row 0
struct AtlasSymbol {
  QString m_name;
  AtlasTimeline m_timeline;
};

struct AtlasOptions {
  int m_pageSize = 2048;
  int m_padding  = 2;
  int m_scale    = 100;  //!< Percent
};

//-----------------------------------------------------------------------------

// Shows the options; returns false if canceled
bool askOptions(AtlasOptions &options, int layerCount, int spriteCount,
                int symbolCount, int fxCount) {
  DVGui::Dialog dialog(TApp::instance()->getMainWindow(), true);
  dialog.setWindowTitle(atlasTr("Export Texture Atlas"));

  const auto description = [layerCount, spriteCount, symbolCount, fxCount]() {
    QString text =
        atlasTr(
            "%1 layer(s), %2 drawing(s) and %3 symbol(s) will be exported: "
            "the drawings are packed into atlas pages (spritemap1.png, ...) "
            "described by spritemap1.json, ..., and the timelines of the "
            "layers and of the symbols are written to Animation.json, in the "
            "folder chosen next.")
            .arg(layerCount)
            .arg(spriteCount)
            .arg(symbolCount);
    text += "\n\n" + atlasTr(
                         "Sub-xsheets are exported as symbols, up to %1 "
                         "levels of nesting. FX are not exported.")
                         .arg(c_maxSymbolDepth);
    if (fxCount > 0)
      text +=
          " " + atlasTr("This scene has %1 FX, which are left out.").arg(fxCount);
    return text;
  };
  QLabel *info = new QLabel(description(), &dialog);
  info->setWordWrap(true);
  dialog.addWidget(info);
  // Counts are an owned snapshot; retranslation neither renders again nor
  // resets the export options currently being edited.
  UiTranslations::onLanguageChanged(&dialog,
      [info, description]() { info->setText(description()); });
  UiTranslations::bind(&dialog, "windowTitle", "TextureAtlasExport",
                       "Export Texture Atlas");

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

// The camera reference (camera pixels, y up, the camera centered on its
// origin) to the camera frame of size res (pixels, y down from its top left
// corner), and back
TAffine cameraToFrame(const TDimension &res) {
  return TAffine(1.0, 0.0, 0.5 * res.lx, 0.0, -1.0, 0.5 * res.ly);
}

TAffine frameToCamera(const TDimension &res) {
  return TAffine(1.0, 0.0, -0.5 * res.lx, 0.0, -1.0, 0.5 * res.ly);
}

//-----------------------------------------------------------------------------

// The written matrix of a transform, rounded
AtlasJson::Matrix3D toMatrix(const TAffine &aff) {
  return AtlasJson::matrixFromAffine(
      roundTo(aff.a11, c_linearUnit), roundTo(aff.a21, c_linearUnit),
      roundTo(aff.a12, c_linearUnit), roundTo(aff.a22, c_linearUnit),
      roundTo(aff.a13, c_positionUnit), roundTo(aff.a23, c_positionUnit));
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
  return toMatrix(cameraToFrame(res) * columnToCamera * spriteToColumn);
}

//-----------------------------------------------------------------------------

/*!
Sets the view of \e timeline, whose xsheet and frames are set: its current
camera, and the inverse of the camera placement at each frame. If \e
withCameraZ, the camera Z scales the view, as Stage::visit() does for the
camera of a sub-xsheet.
*/
void initView(AtlasTimeline &timeline, bool withCameraZ) {
  AtlasView &view = timeline.m_view;
  view.m_inverse.assign(timeline.m_frameCount, TAffine());
  view.m_valid.assign(timeline.m_frameCount, 0);

  TXsheet *xsh            = timeline.m_xsheet;
  TStageObjectTree *tree  = xsh->getStageObjectTree();
  TStageObjectId cameraId = tree->getCurrentCameraId();
  TCamera *camera         = tree->getCamera(cameraId);
  if (!camera) return;
  view.m_res      = camera->getRes();
  view.m_toStage  = getDpiAffine(camera);
  view.m_toPixels = view.m_toStage.inv();

  for (int f = 0; f < timeline.m_frameCount; ++f) {
    int row           = timeline.m_r0 + f;
    TAffine cameraAff = xsh->getPlacement(cameraId, row);
    if (withCameraZ) {
      double k = (1000.0 + xsh->getZ(cameraId, row)) / 1000.0;
      if (!std::isfinite(k) || k <= 1e-6) k = 1.0;
      cameraAff = cameraAff * TScale(k);
    }
    if (std::abs(cameraAff.det()) < 1e-12) continue;
    view.m_inverse[f] = cameraAff.inv();
    view.m_valid[f]   = 1;
  }
}

//=============================================================================

/*!
Collects the layers of a timeline, the drawings they show (the sprites) and
the sub-xsheets they show (the symbols, collected the same way), each one
once over the whole export.
*/
class AtlasCollector {
public:
  std::vector<AtlasSprite> m_sprites;
  std::vector<AtlasSymbol> m_symbols;  //!< A symbol after those it shows
  QStringList m_leftOut;  //!< Sub-xsheets left out somewhere: nested too
                          //!< deeply, or inside themselves

  //! \e mainSymbolName: the symbol name of the main timeline, which no
  //! symbol takes
  explicit AtlasCollector(const QString &mainSymbolName) {
    m_symbolNames.insert(mainSymbolName);
  }

  //! Fills the columns of \e timeline, whose xsheet, frames and view are set.
  //! \e depth is its nesting level, 0 for the main timeline.
  void collect(AtlasTimeline &timeline, int depth);

private:
  std::map<std::pair<TXshSimpleLevel *, TFrameId>, int> m_spriteIndices;
  std::map<TXshChildLevel *, int> m_symbolIndices;  //!< -1: nothing shown
  std::set<TXshChildLevel *> m_building, m_leftOutLevels;
  QSet<QString> m_spriteNames, m_symbolNames;

  int spriteOf(TXshSimpleLevel *sl, const TFrameId &fid);
  int symbolOf(TXshChildLevel *cl, int depth);
};

//-----------------------------------------------------------------------------

// The index of the sprite of a drawing, added on the first call
int AtlasCollector::spriteOf(TXshSimpleLevel *sl, const TFrameId &fid) {
  auto key = std::make_pair(sl, fid);
  auto it  = m_spriteIndices.find(key);
  if (it != m_spriteIndices.end()) return it->second;

  AtlasSprite sprite;
  sprite.m_level = sl;
  sprite.m_fid   = fid;
  QString name   = QString::fromStdWString(sl->getName()) + "_" +
                 QString::fromStdString(fid.expand());
  sprite.m_name = uniqueName(fileSafeName(name), m_spriteNames, true);
  m_sprites.push_back(sprite);
  int index = (int)m_sprites.size() - 1;
  m_spriteIndices.insert({key, index});
  return index;
}

//-----------------------------------------------------------------------------

// The index of the symbol of a sub-xsheet, built on the first call at the
// nesting level depth; -1 if it shows nothing, is nested too deeply or is
// being built (it is inside itself)
int AtlasCollector::symbolOf(TXshChildLevel *cl, int depth) {
  auto it = m_symbolIndices.find(cl);
  if (it != m_symbolIndices.end()) return it->second;

  TXsheet *child = cl->getXsheet();
  if (!child) {
    m_symbolIndices[cl] = -1;
    return -1;
  }
  if (depth > c_maxSymbolDepth || m_building.count(cl) > 0) {
    if (m_leftOutLevels.insert(cl).second)
      m_leftOut << QString::fromStdWString(cl->getName());
    return -1;
  }

  m_building.insert(cl);
  AtlasSymbol symbol;
  symbol.m_timeline.m_xsheet     = child;
  symbol.m_timeline.m_r0         = 0;
  symbol.m_timeline.m_frameCount = std::max(0, child->getFrameCount());
  initView(symbol.m_timeline, true);
  collect(symbol.m_timeline, depth);
  m_building.erase(cl);

  int index = -1;
  if (!symbol.m_timeline.m_columns.empty()) {
    QString name = QString::fromStdWString(cl->getName()).trimmed();
    if (name.isEmpty()) name = "Symbol";
    symbol.m_name = uniqueName(name, m_symbolNames);
    m_symbols.push_back(std::move(symbol));
    index = (int)m_symbols.size() - 1;
  }
  m_symbolIndices[cl] = index;
  return index;
}

//-----------------------------------------------------------------------------

void AtlasCollector::collect(AtlasTimeline &timeline, int depth) {
  TXsheet *xsh = timeline.m_xsheet;
  QSet<QString> layerNames;

  for (int col = 0; col < xsh->getColumnCount(); ++col) {
    if (xsh->isColumnEmpty(col)) continue;
    TXshColumn *column = xsh->getColumn(col);
    if (!column || !column->isPreviewVisible()) continue;
    TXshLevelColumn *levelColumn = column->getLevelColumn();
    if (!levelColumn) continue;

    AtlasColumn atlasColumn;
    atlasColumn.m_index = col;
    bool used           = false;
    for (int f = 0; f < timeline.m_frameCount; ++f) {
      TXshCell cell       = levelColumn->getRemappedCell(timeline.m_r0 + f);
      TXshSimpleLevel *sl = cell.getSimpleLevel();
      AtlasCell shown;
      if (sl && isExportedLevelType(sl->getType()))
        shown.m_sprite = spriteOf(sl, cell.getFrameId());
      else if (TXshChildLevel *cl = cell.getChildLevel()) {
        // The row of the sub-xsheet, as in Stage::visit(); the rows after its
        // end show nothing
        int childRow = cell.getFrameId().getNumber() - 1;
        int symbol   = symbolOf(cl, depth + 1);
        if (symbol >= 0 && childRow >= 0 &&
            childRow < m_symbols[symbol].m_timeline.m_frameCount) {
          shown.m_symbol   = symbol;
          shown.m_childRow = childRow;
        }
      }
      if (shown.m_sprite >= 0 || shown.m_symbol >= 0) used = true;
      atlasColumn.m_frames.push_back(shown);
    }
    if (!used) continue;

    TStageObject *columnObj =
        xsh->getStageObject(TStageObjectId::ColumnId(col));
    QString name = QString::fromStdString(columnObj->getName());
    if (name.isEmpty()) name = QString("Col%1").arg(col + 1);
    atlasColumn.m_name = uniqueName(name, layerNames);
    timeline.m_columns.push_back(atlasColumn);
  }
}

//=============================================================================

/*!
The layers of \e timeline, the top one (the last column) first. A run of
frames of a column which shows the same sprite with the same matrix is one
frame entry; the frames which show a symbol are symbol instances made by
AtlasJson::instanceRuns(). \e spriteToStage converts the column reference
in the camera pixels of the main camera (in which the sprites are rendered
and their centers given) into stage units. The matrices place the sprites
and the symbols in the frame of the timeline's camera, in its pixels.
*/
std::vector<AtlasJson::Layer> timelineLayers(
    const AtlasTimeline &timeline, const std::vector<AtlasSprite> &sprites,
    const std::vector<AtlasSymbol> &symbols, const TAffine &spriteToStage) {
  const AtlasView &view = timeline.m_view;
  const int frameCount  = timeline.m_frameCount;
  const TAffine toFrame = cameraToFrame(view.m_res);

  std::vector<AtlasJson::Layer> layers;
  for (int c = (int)timeline.m_columns.size() - 1; c >= 0; --c) {
    const AtlasColumn &column = timeline.m_columns[c];
    TStageObjectId columnId   = TStageObjectId::ColumnId(column.m_index);

    // The column reference (stage units) to the camera reference (camera
    // pixels), at the frames where the column shows something and is not
    // squashed to nothing
    std::vector<TAffine> columnToCamera(frameCount);
    std::vector<char> visible(frameCount, 0);
    for (int f = 0; f < frameCount; ++f) {
      const AtlasCell &cell = column.m_frames[f];
      if ((cell.m_sprite < 0 && cell.m_symbol < 0) || view.m_valid[f] == 0)
        continue;
      TAffine placement =
          timeline.m_xsheet->getPlacement(columnId, timeline.m_r0 + f);
      if (std::abs(placement.det()) < 1e-12) continue;
      columnToCamera[f] = view.m_toPixels * view.m_inverse[f] * placement;
      visible[f]        = 1;
    }

    AtlasJson::Layer layer;
    layer.m_name = column.m_name;

    // Sprites
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
      int index = column.m_frames[f].m_sprite;
      if (index < 0 || !sprites[index].m_valid || visible[f] == 0) {
        closeRun(f);
        continue;
      }
      AtlasJson::Matrix3D matrix = spriteMatrix(
          sprites[index], columnToCamera[f] * spriteToStage, view.m_res);
      if (index == runSprite && matrix == runMatrix) continue;
      closeRun(f);
      runStart  = f;
      runSprite = index;
      runMatrix = matrix;
    }
    closeRun(frameCount);

    // Symbol instances, of each symbol the column shows (usually one)
    std::vector<int> shownSymbols;
    for (const AtlasCell &cell : column.m_frames)
      if (cell.m_symbol >= 0 &&
          std::find(shownSymbols.begin(), shownSymbols.end(), cell.m_symbol) ==
              shownSymbols.end())
        shownSymbols.push_back(cell.m_symbol);

    for (int s : shownSymbols) {
      const AtlasSymbol &symbol   = symbols[s];
      const AtlasView &symbolView = symbol.m_timeline.m_view;
      // The symbol coordinates (the frame of the child camera, in its pixels)
      // to the column reference (stage units)
      const TAffine symbolToColumn =
          symbolView.m_toStage * frameToCamera(symbolView.m_res);

      std::vector<int> childFrames(frameCount, -1), segments(frameCount, 0);
      std::vector<AtlasJson::Matrix3D> matrices(frameCount,
                                                AtlasJson::identityMatrix());
      int segment        = 0;
      bool previousShown = false;
      for (int f = 0; f < frameCount; ++f) {
        const AtlasCell &cell = column.m_frames[f];
        if (cell.m_symbol != s || visible[f] == 0) {
          previousShown = false;
          continue;
        }
        matrices[f] = toMatrix(toFrame * columnToCamera[f] * symbolToColumn);
        // A new instance when the matrix changes
        if (!previousShown || matrices[f] != matrices[f - 1]) ++segment;
        segments[f]    = segment;
        childFrames[f] = cell.m_childRow;
        previousShown  = true;
      }

      for (const AtlasJson::InstanceRun &run : AtlasJson::instanceRuns(
               childFrames, symbol.m_timeline.m_frameCount, segments)) {
        AtlasJson::Frame frame;
        frame.m_start      = run.m_start;
        frame.m_duration   = run.m_duration;
        frame.m_symbol     = symbol.m_name;
        frame.m_firstFrame = run.m_firstFrame;
        frame.m_loop       = run.m_loop;
        frame.m_matrix     = matrices[run.m_start];
        layer.m_frames.push_back(frame);
      }
    }

    layers.push_back(layer);
  }
  return layers;
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

  QString sceneName = QString::fromStdWString(scene->getSceneName());
  if (sceneName.isEmpty()) sceneName = "untitled";

  // Layers, used drawings and sub-xsheets

  AtlasTimeline mainTimeline;
  mainTimeline.m_xsheet     = xsh;
  mainTimeline.m_r0         = r0;
  mainTimeline.m_frameCount = frameCount;
  initView(mainTimeline, false);

  AtlasCollector collector(sceneName);
  collector.collect(mainTimeline, 0);
  std::vector<AtlasSprite> &sprites = collector.m_sprites;

  if (mainTimeline.m_columns.empty()) {
    DVGui::error(atlasTr("No columns can be exported."));
    return;
  }

  // Options and folder

  auto fxCountOf = [](TXsheet *x) {
    int count = 0;
    if (FxDag *fxDag = x->getFxDag())
      if (TFxSet *internalFxs = fxDag->getInternalFxs())
        count = internalFxs->getFxCount();
    return count;
  };
  int fxCount = fxCountOf(xsh);
  for (const AtlasSymbol &symbol : collector.m_symbols)
    fxCount += fxCountOf(symbol.m_timeline.m_xsheet);

  AtlasOptions options;
  options.m_pageSize = TextureAtlasExportPageSize;
  options.m_padding  = TextureAtlasExportPadding;
  options.m_scale    = TextureAtlasExportScale;
  if (!askOptions(options, (int)mainTimeline.m_columns.size(),
                  (int)sprites.size(), (int)collector.m_symbols.size(),
                  fxCount))
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

  // Timelines: the layers of the main timeline, the top one first, and of
  // every symbol

  AtlasJson::Animation animation;
  animation.m_name        = sceneName;
  animation.m_symbolName  = sceneName;
  animation.m_frameRate   = fps;
  animation.m_stageWidth  = res.lx;
  animation.m_stageHeight = res.ly;
  animation.m_frameCount  = frameCount;
  animation.m_layers =
      timelineLayers(mainTimeline, sprites, collector.m_symbols, pixelsToStage);
  for (const AtlasSymbol &symbol : collector.m_symbols) {
    AtlasJson::Symbol json;
    json.m_name       = symbol.m_name;
    json.m_frameCount = symbol.m_timeline.m_frameCount;
    json.m_layers     = timelineLayers(symbol.m_timeline, sprites,
                                       collector.m_symbols, pixelsToStage);
    animation.m_symbols.push_back(json);
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
                    "%2 page(s), %3 layer(s), %4 symbol(s).")
                    .arg((int)packed.size())
                    .arg((int)pages.size())
                    .arg((int)animation.m_layers.size())
                    .arg((int)animation.m_symbols.size());
  if (!collector.m_leftOut.isEmpty()) {
    QStringList names = collector.m_leftOut.mid(0, 5);
    if (collector.m_leftOut.size() > 5) names << "...";
    str += "\n\n" + atlasTr(
                        "These sub-xsheets are nested more than %1 levels "
                        "deep, or inside themselves, and are left out "
                        "there (blank frames): %2.")
                        .arg(c_maxSymbolDepth)
                        .arg(names.join(", "));
  }
  std::vector<QString> buttons = {atlasTr("OK"),
                                  atlasTr("Open containing folder")};
  if (DVGui::MsgBox(
          collector.m_leftOut.isEmpty() ? DVGui::INFORMATION : DVGui::WARNING,
          str, buttons) == 2) {
    TFilePath folderPath(folder.toStdWString());
    if (TSystem::isUNC(folderPath))
      QDesktopServices::openUrl(QUrl(folderPath.getQString()));
    else
      QDesktopServices::openUrl(QUrl::fromLocalFile(folderPath.getQString()));
  }
}
