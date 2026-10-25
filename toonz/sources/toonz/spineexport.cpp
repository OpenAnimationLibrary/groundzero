// Export to Spine JSON: writes the cut-out rig and animation of the current
// xsheet as a Spine skeleton (3.8 or 4.x JSON), with one PNG per used drawing
// and a simple atlas (one page per image), so that game engine runtimes can
// load it directly.
//
// Mapping:
//  - Bones: a "root" bone carrying the inverse camera motion, then one bone
//    per stage object (table, pegbars, columns, cameras) in the parent chain
//    of the exported columns, parented as in the Stage Schematic.
//  - Slots: one per preview visible level column, in column order, with a
//    region attachment per used drawing (Toonz Vector, Toonz Raster, Raster).
//  - Animation: every frame of the output range is baked into translate,
//    rotate, scale and shear keys (keys inside runs of equal values are
//    dropped), relative to the setup pose (the first frame), and into slot
//    attachment keys for drawing changes.
//
// Bone local transforms are computed from the stage objects' world
// placements (child = parent * local), so hooks, handles, pinned skeletons
// and columns linked to a Plastic skeleton vertex ("H0:" handles, see
// HandleManager::getHandleAffine()) are all followed as rendered. Coordinates
// are converted from stage inches to camera pixels (Spine is y-up as the
// stage).
//
// Not exported: Plastic / mesh deformations, sub-xsheets, Z depth and
// stacking order changes, fxs, column transparency and color filters.

#include "spineexport.h"

// Tnz6 includes
#include "tapp.h"
#include "menubarcommandids.h"
#include "filebrowserpopup.h"
#include "exportlevelcommand.h"

// TnzQt includes
#include "toonzqt/menubarcommand.h"
#include "toonzqt/dvdialog.h"
#include "toonzqt/gutil.h"

// TnzLib includes
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
#include "toonz/levelproperties.h"
#include "toonz/tstageobject.h"
#include "toonz/tstageobjecttree.h"
#include "toonz/tcamera.h"
#include "toonz/dpiscale.h"
#include "toutputproperties.h"

// TnzCore includes
#include "tsystem.h"
#include "tutil.h"
#include "trop.h"
#include "trasterimage.h"
#include "tvectorimage.h"

// Qt includes
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QFile>
#include <QDir>
#include <QSet>
#include <QImage>
#include <QLabel>
#include <QComboBox>
#include <QHBoxLayout>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QUrl>

// STD includes
#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <vector>

namespace {

// Largest side of an exported image, in pixels
const int c_maxImageSize = 8192;

// Precision of the written values
const double c_positionUnit = 1e-3;  // pixels
const double c_angleUnit    = 1e-3;  // degrees
const double c_scaleUnit    = 1e-5;
const double c_timeUnit     = 1e-6;  // seconds

//-----------------------------------------------------------------------------

struct SpineBone {
  TStageObjectId m_id;  //!< NoneId for the root bone
  int m_parent;         //!< Index of the parent bone, -1 for the root bone
  QString m_name;
  std::vector<SpineExport::BoneTransform> m_frames;
};

struct SpineSlot {
  QString m_name;
  int m_bone;
  std::vector<int> m_frames;  //!< Image index per frame, -1 when blank
};

struct SpineImage {
  TXshSimpleLevel *m_level;
  TFrameId m_fid;
  QString m_name;  //!< Attachment name and image file name (without .png)
  bool m_valid = false;
  int m_width = 0, m_height = 0;
  TPointD m_center;  //!< Image center in the column reference, pixels
};

//-----------------------------------------------------------------------------

double roundTo(double value, double unit) {
  value = std::round(value / unit) * unit;
  return (value == 0.0) ? 0.0 : value;  // no "-0"
}

//-----------------------------------------------------------------------------

QString fileSafeName(const QString &name) {
  QString result;
  for (const QChar &c : name)
    result += (c.isLetterOrNumber() || c == '_' || c == '-') ? c : QChar('_');
  return result.isEmpty() ? QString("_") : result;
}

//-----------------------------------------------------------------------------

// Returns name, or name followed by a number if already used. Names used as
// file names are compared ignoring case.
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

// Returns the box of the pixels which are not fully transparent, empty if
// none.
TRect visibleBox(const TRaster32P &ras) {
  int x0 = ras->getLx(), y0 = ras->getLy(), x1 = -1, y1 = -1;
  ras->lock();
  for (int y = 0; y < ras->getLy(); ++y) {
    TPixel32 *pix = ras->pixels(y);
    for (int x = 0; x < ras->getLx(); ++x, ++pix) {
      if (!pix->m) continue;
      x0 = std::min(x0, x), x1 = std::max(x1, x);
      y0 = std::min(y0, y), y1 = std::max(y1, y);
    }
  }
  ras->unlock();
  return TRect(x0, y0, x1, y1);
}

//-----------------------------------------------------------------------------

/*!
Renders a drawing at the camera resolution, cropped to its visible pixels.
\e stageToPixels converts stage coordinates into camera pixels. Returns the
center of the returned raster in the column reference, in pixels, in \e center.
Returns a null raster for a blank or unreadable drawing.
*/
TRaster32P renderDrawing(TXshSimpleLevel *sl, const TFrameId &fid,
                         const TCamera *camera, const TAffine &stageToPixels,
                         TPointD &center) {
  TRaster32P ras;
  TPointD origin;  // position of the column origin in ras
  TPointD offset;  // image offset in the column reference, pixels

  if (sl->getType() == PLI_XSHLEVEL) {
    TVectorImageP vi = sl->getFrame(fid, false);
    if (!vi || vi->getStrokeCount() == 0) return TRaster32P();

    // Render on a camera centered on the column origin, as large as needed
    TRectD bbox = stageToPixels * vi->getBBox();
    int lx = 2 * (tceil(std::max(std::abs(bbox.x0), std::abs(bbox.x1))) + 4);
    int ly = 2 * (tceil(std::max(std::abs(bbox.y0), std::abs(bbox.y1))) + 4);
    lx     = std::min(lx, c_maxImageSize);
    ly     = std::min(ly, c_maxImageSize);

    TPointD dpi = camera->getDpi();
    IoCmd::ExportLevelOptions opts;
    opts.m_camera.setRes(TDimension(lx, ly));
    opts.m_camera.setSize(TDimensionD(lx / dpi.x, ly / dpi.y));

    TRasterImageP ri = IoCmd::exportedImage("png", *sl, fid, opts);
    if (!ri || !ri->getRaster()) return TRaster32P();
    ras    = ri->getRaster();
    origin = ras->getCenterD();
  } else {
    TRasterImageP ri = IoCmd::exportedImage("png", *sl, fid);
    if (!ri || !ri->getRaster()) return TRaster32P();

    // Work on a full color copy: the image may be the cached one
    TRasterP src = ri->getRaster();
    TRaster32P src32(src->getSize());
    try {
      TRaster32P srcFullColor = src;
      if (srcFullColor)
        src32->copy(srcFullColor);
      else
        TRop::convert(src32, src);
    } catch (...) {
      return TRaster32P();
    }
    if (sl->getType() == OVL_XSHLEVEL) {
      LevelProperties *prop = sl->getProperties();
      if (prop->whiteTransp()) TRop::whiteTransp(src32);
      if (prop->doPremultiply()) TRop::premultiply(src32);
    }

    // Level dpi to camera dpi
    TAffine aff = stageToPixels * getDpiAffine(sl, fid, true);
    offset      = aff * convert(ri->getOffset());
    if (areAlmostEqual(aff.a11, 1.0) && areAlmostEqual(aff.a22, 1.0) &&
        aff.a12 == 0.0 && aff.a21 == 0.0) {
      ras    = src32;
      origin = ras->getCenterD();
    } else {
      TRectD bbox = aff * TRectD(0, 0, src32->getLx(), src32->getLy());
      int lx      = std::min(tceil(bbox.getLx()) + 2, c_maxImageSize);
      int ly      = std::min(tceil(bbox.getLy()) + 2, c_maxImageSize);
      ras         = TRaster32P(lx, ly);
      ras->clear();
      origin = ras->getCenterD();
      TRop::resample(
          ras, src32,
          TTranslation(origin) * aff * TTranslation(-src32->getCenterD()));
    }
  }

  TRect box = visibleBox(ras);
  if (box.isEmpty()) return TRaster32P();

  TRaster32P cropped(box.getLx(), box.getLy());
  TRect extractBox = box;
  cropped->copy(ras->extract(extractBox));

  center = TPointD(0.5 * (box.x0 + box.x1 + 1), 0.5 * (box.y0 + box.y1 + 1)) -
           origin + offset;
  return cropped;
}

//-----------------------------------------------------------------------------

/*!
Returns the keys of a channel baked at every frame, with one or two values
per key written as \e xName and \e yName (an empty name is not written).
Keys inside runs of equal values are dropped, as linear interpolation
restores them. Returns no keys if the channel always has its default value,
unless \e holdToEnd: then a last key, at the end of the last frame, also
gives the animation the duration of the frame range.
*/
QJsonArray bakeKeys(std::vector<TPointD> values, const TPointD &defaultValue,
                    const QString &xName, const QString &yName, double fps,
                    bool holdToEnd = false) {
  QJsonArray keys;
  if (values.empty()) return keys;

  auto equal = [](const TPointD &a, const TPointD &b) {
    return a.x == b.x && a.y == b.y;
  };
  if (holdToEnd)
    values.push_back(values.back());
  else if (std::all_of(values.begin(), values.end(), [&](const TPointD &v) {
             return equal(v, defaultValue);
           }))
    return keys;

  int count = (int)values.size();
  for (int i = 0; i < count; ++i) {
    if (i > 0 && i < count - 1 && equal(values[i], values[i - 1]) &&
        equal(values[i], values[i + 1]))
      continue;
    QJsonObject key;
    key["time"] = roundTo(i / fps, c_timeUnit);
    if (!xName.isEmpty()) key[xName] = values[i].x;
    if (!yName.isEmpty()) key[yName] = values[i].y;
    keys.append(key);
  }
  return keys;
}

//-----------------------------------------------------------------------------

void writeBoneTimelines(QJsonObject &timelines, const SpineBone &bone,
                        const SpineExport::BoneTransform &setup, bool spine4,
                        double fps) {
  std::vector<TPointD> translate, rotate, scale, shear;
  for (const SpineExport::BoneTransform &t : bone.m_frames) {
    translate.push_back(TPointD(roundTo(t.m_x - setup.m_x, c_positionUnit),
                                roundTo(t.m_y - setup.m_y, c_positionUnit)));
    rotate.push_back(
        TPointD(roundTo(t.m_rotation - setup.m_rotation, c_angleUnit), 0.0));
    scale.push_back(TPointD(roundTo(t.m_scaleX / setup.m_scaleX, c_scaleUnit),
                            roundTo(t.m_scaleY / setup.m_scaleY, c_scaleUnit)));
    shear.push_back(
        TPointD(0.0, roundTo(t.m_shearY - setup.m_shearY, c_angleUnit)));
  }

  // Timeline values are relative to the setup pose
  QJsonObject json;
  QJsonArray keys =
      bakeKeys(rotate, TPointD(), spine4 ? "value" : "angle", QString(), fps);
  if (!keys.isEmpty()) json["rotate"] = keys;
  keys = bakeKeys(translate, TPointD(), "x", "y", fps, bone.m_parent < 0);
  if (!keys.isEmpty()) json["translate"] = keys;
  keys = bakeKeys(scale, TPointD(1.0, 1.0), "x", "y", fps);
  if (!keys.isEmpty()) json["scale"] = keys;
  keys = bakeKeys(shear, TPointD(), QString(), "y", fps);
  if (!keys.isEmpty()) json["shear"] = keys;

  if (!json.isEmpty()) timelines[bone.m_name] = json;
}

//-----------------------------------------------------------------------------

QByteArray atlasPage(const QString &fileName, const SpineImage &image) {
  // Spine 3.8 atlas syntax, which Spine 4.x runtimes also read
  QString size = QString("%1, %2").arg(image.m_width).arg(image.m_height);
  QString page = QString(
                     "\n%1\nsize: %2\nformat: RGBA8888\nfilter: "
                     "Linear,Linear\nrepeat: none\n%3\n  rotate: false\n  xy: "
                     "0, 0\n  size: %2\n  orig: %2\n  offset: 0, 0\n  index: "
                     "-1\n")
                     .arg(fileName, size, image.m_name);
  return page.toUtf8();
}

}  // namespace

//=============================================================================
// ExportSpineCommand
//-----------------------------------------------------------------------------

class ExportSpineCommand final : public MenuItemHandler {
public:
  ExportSpineCommand() : MenuItemHandler(MI_ExportSpine) {}
  void execute() override;
} exportSpineCommand;

void ExportSpineCommand::execute() {
  ToonzScene *scene = TApp::instance()->getCurrentScene()->getScene();
  TXsheet *xsh      = TApp::instance()->getCurrentXsheet()->getXsheet();
  TFilePath fp      = scene->getScenePath().withType("json");

  // Frame range
  TOutputProperties *oprop = scene->getProperties()->getOutputProperties();
  int r0, r1, step;
  if (scene->getTopXsheet() != xsh || !oprop->getRange(r0, r1, step))
    r0 = 0, r1 = xsh->getFrameCount() - 1;
  if (r1 < r0) {
    DVGui::error(QObject::tr("No columns can be exported."));
    return;
  }
  double fps = oprop->getFrameRate();
  if (fps <= 0.0) fps = 24.0;

  TStageObjectTree *tree  = xsh->getStageObjectTree();
  TStageObjectId cameraId = tree->getCurrentCameraId();
  TCamera *camera         = tree->getCamera(cameraId);
  TAffine stageToPixels   = getDpiAffine(camera).inv();
  TAffine pixelsToStage   = getDpiAffine(camera);

  // Bones, slots and used drawings

  std::vector<SpineBone> bones;
  std::map<TStageObjectId, int> boneIndices;
  QSet<QString> boneNames, slotNames, imageNames;

  bones.push_back({TStageObjectId::NoneId, -1, "root", {}});
  boneNames.insert("root");

  std::function<int(const TStageObjectId &)> addBone =
      [&](const TStageObjectId &id) -> int {
    auto it = boneIndices.find(id);
    if (it != boneIndices.end()) return it->second;
    boneIndices[id] = 0;  // guard against parenting loops

    TStageObject *obj       = xsh->getStageObject(id);
    TStageObjectId parentId = obj->getParent();
    int parent = (parentId == TStageObjectId::NoneId) ? 0 : addBone(parentId);

    QString name = QString::fromStdString(obj->getName());
    bones.push_back({id, parent, uniqueName(name, boneNames), {}});
    return boneIndices[id] = (int)bones.size() - 1;
  };

  std::vector<SpineSlot> spineSlots;
  std::vector<SpineImage> images;
  std::map<std::pair<TXshSimpleLevel *, TFrameId>, int> imageIndices;

  for (int col = 0; col < xsh->getColumnCount(); ++col) {
    if (xsh->isColumnEmpty(col)) continue;
    TXshColumn *column = xsh->getColumn(col);
    if (!column || !column->isPreviewVisible()) continue;
    TXshLevelColumn *levelColumn = column->getLevelColumn();
    if (!levelColumn) continue;

    SpineSlot slot;
    bool used = false;
    for (int r = r0; r <= r1; ++r) {
      const TXshCell &cell = levelColumn->getCell(r);
      TXshSimpleLevel *sl  = cell.getSimpleLevel();
      int imageIndex       = -1;
      if (sl && isExportedLevelType(sl->getType())) {
        auto key = std::make_pair(sl, cell.getFrameId());
        auto it  = imageIndices.find(key);
        if (it == imageIndices.end()) {
          SpineImage image;
          image.m_level = sl;
          image.m_fid   = cell.getFrameId();
          QString name  = QString::fromStdWString(sl->getName()) + "_" +
                         QString::fromStdString(image.m_fid.expand());
          image.m_name = uniqueName(fileSafeName(name), imageNames, true);
          images.push_back(image);
          it = imageIndices.insert({key, (int)images.size() - 1}).first;
        }
        imageIndex = it->second;
        used       = true;
      }
      slot.m_frames.push_back(imageIndex);
    }
    if (!used) continue;

    TStageObjectId columnId = TStageObjectId::ColumnId(col);
    QString name =
        QString::fromStdString(xsh->getStageObject(columnId)->getName());
    slot.m_bone = addBone(columnId);
    slot.m_name = uniqueName(name, slotNames);
    spineSlots.push_back(slot);
  }

  if (spineSlots.empty()) {
    DVGui::error(QObject::tr("No columns can be exported."));
    return;
  }

  // Ask for the file

  static GenericSaveFilePopup *savePopup = nullptr;
  static QComboBox *versionCombo         = nullptr;

  if (!savePopup) {
    versionCombo = new QComboBox();
    versionCombo->addItem("Spine 4.2", "4.2.00");
    versionCombo->addItem("Spine 4.1", "4.1.00");
    versionCombo->addItem("Spine 4.0", "4.0.00");
    versionCombo->addItem("Spine 3.8", "3.8.99");

    QWidget *customWidget  = new QWidget();
    QHBoxLayout *customLay = new QHBoxLayout();
    customLay->setContentsMargins(5, 5, 5, 5);
    customLay->setSpacing(5);
    customLay->addWidget(new QLabel(QObject::tr("Spine Version:")));
    customLay->addWidget(versionCombo);
    customLay->addStretch(1);
    customWidget->setLayout(customLay);

    savePopup = new GenericSaveFilePopup(QObject::tr("Export to Spine JSON"),
                                         customWidget);
    savePopup->addFilterType("json");
  }
  if (!scene->isUntitled())
    savePopup->setFolder(fp.getParentDir());
  else
    savePopup->setFolder(
        TProjectManager::instance()->getCurrentProject()->getScenesPath());

  savePopup->setFilename(fp.withoutParentDir());
  fp = savePopup->getPath();
  if (fp.isEmpty()) return;
  fp = fp.withType("json");

  QString version = versionCombo->currentData().toString();
  bool spine4     = !version.startsWith("3.");

  QString imagesFolder = QString::fromStdString(fp.getName()) + "_images";
  TFilePath imagesDir  = fp.getParentDir() + TFilePath(imagesFolder);
  if (!QDir(imagesDir.getQString()).mkpath(".")) {
    DVGui::error(QObject::tr("Unable to create folder for saving images."));
    return;
  }

  // Images

  DVGui::ProgressDialog progressDialog("", QObject::tr("Cancel"), 0,
                                       (int)images.size());
  progressDialog.setWindowTitle(QObject::tr("Exporting..."));
  progressDialog.setWindowModality(Qt::WindowModal);
  progressDialog.show();

  QByteArray atlas;
  for (int i = 0; i < (int)images.size(); ++i) {
    SpineImage &image = images[i];
    progressDialog.setLabelText(image.m_name);
    progressDialog.setValue(i);
    QCoreApplication::processEvents();
    if (progressDialog.wasCanceled()) return;

    TRaster32P ras = renderDrawing(image.m_level, image.m_fid, camera,
                                   stageToPixels, image.m_center);
    if (!ras) continue;

    QString fileName = image.m_name + ".png";
    QImage img       = rasterToQImage(ras);
    if (!img.save(imagesDir.getQString() + "/" + fileName, "PNG")) {
      progressDialog.close();
      DVGui::error(QObject::tr("Unable to save %1.").arg(fileName));
      return;
    }
    image.m_valid  = true;
    image.m_width  = ras->getLx();
    image.m_height = ras->getLy();
    atlas += atlasPage(imagesFolder + "/" + fileName, image);
  }
  progressDialog.close();

  // Bake the bone transforms of every frame

  for (int r = r0; r <= r1; ++r) {
    TAffine rootAff =
        stageToPixels * xsh->getPlacement(cameraId, r).inv() * pixelsToStage;
    for (SpineBone &bone : bones) {
      TAffine aff = rootAff;
      if (bone.m_parent >= 0) {
        TAffine parent = (bone.m_parent > 0)
                             ? xsh->getPlacement(bones[bone.m_parent].m_id, r)
                             : TAffine();
        if (std::abs(parent.det()) < 1e-12)
          aff = TAffine();
        else
          aff = stageToPixels * parent.inv() * xsh->getPlacement(bone.m_id, r) *
                pixelsToStage;
      }
      const SpineExport::BoneTransform *previous =
          bone.m_frames.empty() ? nullptr : &bone.m_frames.back();
      bone.m_frames.push_back(SpineExport::decomposeAffine(
          aff.a11, aff.a12, aff.a13, aff.a21, aff.a22, aff.a23, previous));
    }
  }

  // Skeleton JSON

  QJsonObject bonesTimelines, slotsTimelines;

  QJsonArray bonesJson;
  for (const SpineBone &bone : bones) {
    // The setup pose is the first frame
    SpineExport::BoneTransform setup = bone.m_frames.front();
    if (std::abs(setup.m_scaleX) < c_scaleUnit) setup.m_scaleX = 1.0;
    if (std::abs(setup.m_scaleY) < c_scaleUnit) setup.m_scaleY = 1.0;

    QJsonObject json;
    json["name"] = bone.m_name;
    if (bone.m_parent >= 0) json["parent"] = bones[bone.m_parent].m_name;
    json["x"]        = roundTo(setup.m_x, c_positionUnit);
    json["y"]        = roundTo(setup.m_y, c_positionUnit);
    json["rotation"] = roundTo(setup.m_rotation, c_angleUnit);
    json["scaleX"]   = roundTo(setup.m_scaleX, c_scaleUnit);
    json["scaleY"]   = roundTo(setup.m_scaleY, c_scaleUnit);
    json["shearY"]   = roundTo(setup.m_shearY, c_angleUnit);
    bonesJson.append(json);

    // Keys are relative to the rounded setup pose
    setup.m_x        = json["x"].toDouble();
    setup.m_y        = json["y"].toDouble();
    setup.m_rotation = json["rotation"].toDouble();
    setup.m_scaleX   = json["scaleX"].toDouble();
    setup.m_scaleY   = json["scaleY"].toDouble();
    setup.m_shearY   = json["shearY"].toDouble();
    writeBoneTimelines(bonesTimelines, bone, setup, spine4, fps);
  }

  QJsonArray slotsJson;
  QJsonObject skinAttachments;
  for (const SpineSlot &slot : spineSlots) {
    std::vector<int> frames;
    for (int index : slot.m_frames)
      frames.push_back(index >= 0 && images[index].m_valid ? index : -1);

    QJsonObject json;
    json["name"] = slot.m_name;
    json["bone"] = bones[slot.m_bone].m_name;
    if (frames.front() >= 0) json["attachment"] = images[frames.front()].m_name;
    slotsJson.append(json);

    // Region attachments
    QJsonObject attachments;
    for (int index : frames) {
      if (index < 0 || attachments.contains(images[index].m_name)) continue;
      const SpineImage &image = images[index];
      QJsonObject region;
      region["x"]               = roundTo(image.m_center.x, c_positionUnit);
      region["y"]               = roundTo(image.m_center.y, c_positionUnit);
      region["width"]           = image.m_width;
      region["height"]          = image.m_height;
      attachments[image.m_name] = region;
    }
    if (!attachments.isEmpty()) skinAttachments[slot.m_name] = attachments;

    // Drawing changes
    QJsonArray keys;
    for (int f = 0; f < (int)frames.size(); ++f) {
      if (f > 0 && frames[f] == frames[f - 1]) continue;
      QJsonObject key;
      key["time"] = roundTo(f / fps, c_timeUnit);
      key["name"] = (frames[f] >= 0) ? QJsonValue(images[frames[f]].m_name)
                                     : QJsonValue(QJsonValue::Null);
      keys.append(key);
    }
    if (keys.size() > 1) {
      QJsonObject timelines;
      timelines["attachment"]     = keys;
      slotsTimelines[slot.m_name] = timelines;
    }
  }

  QJsonObject skeleton;
  TDimension res     = camera->getRes();
  skeleton["spine"]  = version;
  skeleton["x"]      = -0.5 * res.lx;
  skeleton["y"]      = -0.5 * res.ly;
  skeleton["width"]  = res.lx;
  skeleton["height"] = res.ly;
  skeleton["fps"]    = fps;
  skeleton["images"] = "./" + imagesFolder + "/";
  skeleton["audio"]  = "";

  QJsonObject skin;
  skin["name"]        = "default";
  skin["attachments"] = skinAttachments;
  QJsonArray skins;
  skins.append(skin);

  QJsonObject animation;
  if (!bonesTimelines.isEmpty()) animation["bones"] = bonesTimelines;
  if (!slotsTimelines.isEmpty()) animation["slots"] = slotsTimelines;
  QString animationName = QString::fromStdWString(scene->getSceneName());
  QJsonObject animations;
  animations[animationName.isEmpty() ? QString("animation") : animationName] =
      animation;

  QJsonObject root;
  root["skeleton"]   = skeleton;
  root["bones"]      = bonesJson;
  root["slots"]      = slotsJson;
  root["skins"]      = skins;
  root["animations"] = animations;

  // Write

  QFile jsonFile(fp.getQString());
  if (!jsonFile.open(QIODevice::WriteOnly) ||
      jsonFile.write(QJsonDocument(root).toJson()) < 0) {
    DVGui::error(QObject::tr("Unable to save %1.").arg(fp.getQString()));
    return;
  }
  jsonFile.close();

  TFilePath atlasPath = fp.withType("atlas");
  QFile atlasFile(atlasPath.getQString());
  if (!atlasFile.open(QIODevice::WriteOnly) || atlasFile.write(atlas) < 0) {
    DVGui::error(QObject::tr("Unable to save %1.").arg(atlasPath.getQString()));
    return;
  }
  atlasFile.close();

  QString str = QObject::tr("%1 has been exported successfully.")
                    .arg(QString::fromStdString(fp.getLevelName()));
  std::vector<QString> buttons = {QObject::tr("OK"),
                                  QObject::tr("Open containing folder")};
  if (DVGui::MsgBox(DVGui::INFORMATION, str, buttons) == 2) {
    TFilePath folderPath = fp.getParentDir();
    if (TSystem::isUNC(folderPath))
      QDesktopServices::openUrl(QUrl(folderPath.getQString()));
    else
      QDesktopServices::openUrl(QUrl::fromLocalFile(folderPath.getQString()));
  }
}
