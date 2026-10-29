// SPDX-License-Identifier: BSD-3-Clause

// Runtime compatibility check between two builds (see
// doc/compat_regression.md). It runs a deterministic workload and writes
// everything it reads, saves and renders to an output folder, so that the
// outputs of the edition build and of a newer build can be compared file by
// file (the Compat job of .github/workflows/tests_linux.yml):
//   - every .pli file of the repository (thirdparty excluded) is loaded with
//     the level reader, saved again with the level writer into pli/<key>.pli,
//     dumped as text into pli/<key>.txt (frames, strokes with their control
//     points, styles and outline options, regions, palette styles) and, when
//     an OpenGL context can be created, each frame is rendered with the vector
//     renderer into pli/<key>.<frame>.png;
//   - a small scene is built in memory (three columns of some of those levels,
//     a keyframed pegbar and columns, a blur FX with keys), saved into
//     scene.tnz (its levels are copied into levels/ and referenced through
//     $scenefolder, so that the file holds no absolute path), loaded back and
//     dumped into scene_dump.txt; three of its frames are rendered through the
//     FX tree of buildSceneFx() into scene.<row>.png when OpenGL is available;
//   - with --load, a scene file written by the other build is loaded and
//     dumped into scene_dump.txt, as the scene above.
// manifest.txt lists what was done and what was skipped.
//
// The file must also compile, unchanged, against the edition tree: it uses
// only the API that exists there.
//
// Usage: compat_regression [--require-rendering] <repository root> <output folder>
//        compat_regression --load <scene.tnz> <output folder>
// Graphics CI uses --require-rendering: missing OpenGL or a failed scene
// render is an error, not a serialization-only comparison.

#include "tcommon.h"
#include "tenv.h"
#include "tfilepath.h"
#include "texception.h"
#include "tthread.h"
#include "timagecache.h"
#include "tnzimage.h"
#include "tlevel.h"
#include "tlevel_io.h"
#include "timage_io.h"
#include "tvectorimage.h"
#include "tstroke.h"
#include "tregion.h"
#include "tpalette.h"
#include "tcolorstyles.h"
#include "tsimplecolorstyles.h"
#include "tvectorbrushstyle.h"
#include "tofflinegl.h"
#include "tvectorrenderdata.h"
#include "tgeometry.h"
#include "traster.h"
#include "tfx.h"
#include "trasterfx.h"
#include "trenderer.h"
#include "tparam.h"
#include "tparamcontainer.h"
#include "tdoubleparam.h"
#include "tdoublekeyframe.h"
#include "tstream.h"

#include "toonz/toonzscene.h"
#include "toonz/tproject.h"
#include "toonz/preferences.h"
#include "toonz/levelset.h"
#include "toonz/txsheet.h"
#include "toonz/txshcell.h"
#include "toonz/txshcolumn.h"
#include "toonz/txshlevel.h"
#include "toonz/txshsimplelevel.h"
#include "toonz/txshleveltypes.h"
#include "toonz/tstageobject.h"
#include "toonz/tstageobjectid.h"
#include "toonz/tstageobjecttree.h"
#include "toonz/tcamera.h"
#include "toonz/fxdag.h"
#include "toonz/tcolumnfxset.h"
#include "toonz/tcolumnfx.h"
#include "toonz/scenefx.h"
#include "toonz/sceneproperties.h"

#include <QByteArray>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QVariant>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

DV_IMPORT_API void initStdFx();
DV_IMPORT_API void initColorFx();

namespace {

// Size of the PNG renders of the PLI frames
const TDimension cFrameSize(128, 128);
// Frames at which the animated values of the scene are dumped
const double cDumpFrames[] = {0, 3, 6, 11};
// Rows of the scene rendered to PNG
const int cRenderRows[] = {0, 5, 11};
// Levels used by the scene, if the repository has them
const char *const cSceneLevels[] = {"stuff/library/vector brushes/circle.pli",
                                    "stuff/library/vector brushes/arrow.pli",
                                    "stuff/library/custom styles/star.pli"};

std::string textf(const char *fmt, ...) {
  char buffer[512];
  va_list args;
  va_start(args, fmt);
  int n = std::vsnprintf(buffer, sizeof(buffer), fmt, args);
  va_end(args);
  if (n < 0) return std::string();
  if (n < (int)sizeof(buffer)) return std::string(buffer, n);
  std::string text(n + 1, '\0');
  va_start(args, fmt);
  std::vsnprintf(&text[0], n + 1, fmt, args);
  va_end(args);
  text.resize(n);
  return text;
}

std::string str(const QString &s) { return s.toStdString(); }
std::string str(const std::wstring &s) {
  return QString::fromStdWString(s).toStdString();
}
std::string str(const TFilePath &fp) { return str(fp.getWideString()); }

TFilePath toPath(const QString &path) { return TFilePath(path.toStdWString()); }

void writeText(const QString &path, const std::string &text) {
  QFile f(path);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
    throw std::runtime_error("cannot write " + str(path));
  QByteArray data(text.data(), (int)text.size());
  if (f.write(data) != data.size())
    throw std::runtime_error("short write of " + str(path));
}

// Runs a body and turns its exceptions into a message (empty on success)
template <typename Body>
std::string guarded(Body body) {
  try {
    body();
  } catch (const TException &e) {
    std::string msg = str(e.getMessage());
    return msg.empty() ? std::string("TException") : msg;
  } catch (const std::exception &e) {
    return e.what();
  } catch (...) {
    return "unknown exception";
  }
  return std::string();
}

// Rasters are stored bottom row first; images top row first. The pixels are
// stored as they are (premultiplied), as in stdfx/tests/fxrender_test.cpp.
QImage toImage(const TRaster32P &ras) {
  QImage image(ras->getLx(), ras->getLy(), QImage::Format_ARGB32);
  ras->lock();
  for (int y = 0; y < ras->getLy(); ++y) {
    const TPixel32 *pix = ras->pixels(ras->getLy() - 1 - y);
    QRgb *line          = reinterpret_cast<QRgb *>(image.scanLine(y));
    for (int x = 0; x < ras->getLx(); ++x, ++pix)
      line[x] = qRgba(pix->r, pix->g, pix->b, pix->m);
  }
  ras->unlock();
  return image;
}

void savePng(const TRaster32P &ras, const QString &path) {
  if (!ras) throw std::runtime_error("no 32-bit raster");
  if (!toImage(ras).save(path, "PNG"))
    throw std::runtime_error("cannot write " + str(path));
}

// The vector renderer draws with OpenGL (TOfflineGL). Without a context (no
// display for the GLX of the platform plugin, for example) the renders are
// skipped and the rest still runs.
bool openGLAvailable() {
  QOffscreenSurface surface;
  surface.create();
  QOpenGLContext context;
  if (!surface.isValid() || !context.create()) return false;
  bool ok = context.makeCurrent(&surface);
  if (ok) context.doneCurrent();
  return ok;
}

//=============================================================================
// PLI levels
//-----------------------------------------------------------------------------

// The relative paths of the .pli files under the root, sorted. Hidden
// folders, thirdparty and the output folder are skipped.
QStringList findPliFiles(const QString &root, const QString &outPath) {
  QStringList files;
  const QDir rootDir(root);
  QDirIterator it(root, QStringList() << "*.pli", QDir::Files,
                  QDirIterator::Subdirectories);
  while (it.hasNext()) {
    const QString path = QFileInfo(it.next()).absoluteFilePath();
    if (path.startsWith(outPath + "/")) continue;
    const QString rel = rootDir.relativeFilePath(path);
    bool skip         = false;
    for (const QString &part : rel.split('/'))
      if (part == "thirdparty" || part.startsWith('.')) skip = true;
    if (!skip) files << rel;
  }
  files.sort();
  return files;
}

QString keyFor(QString rel) {
  if (rel.endsWith(".pli")) rel.chop(4);
  rel.replace(QChar('/'), QString("__"));
  rel.replace(QChar(' '), QChar('_'));
  return rel;
}

std::string dumpPalette(TPalette *palette) {
  if (!palette) return "palette none\n";
  std::string out = textf("palette \"%s\" styles %d pages %d\n",
                          str(palette->getPaletteName()).c_str(),
                          palette->getStyleCount(), palette->getPageCount());
  for (int p = 0; p < palette->getPageCount(); p++) {
    TPalette::Page *page = palette->getPage(p);
    if (!page) continue;
    out += textf("page %d \"%s\":", p, str(page->getName()).c_str());
    for (int i = 0; i < page->getStyleCount(); i++)
      out += textf(" %d", page->getStyleId(i));
    out += "\n";
  }
  for (int id = 0; id < palette->getStyleCount(); id++) {
    TColorStyle *style = palette->getStyle(id);
    if (!style) {
      out += textf("style %d none\n", id);
      continue;
    }
    TPixel32 color = style->getMainColor();
    out += textf("style %d tag %d color %d %d %d %d name \"%s\"\n", id,
                 style->getTagId(), (int)color.r, (int)color.g, (int)color.b,
                 (int)color.m, str(style->getName()).c_str());
  }
  return out;
}

std::string dumpVectorImage(TVectorImage *vi) {
  std::string out = textf("strokes %u\n", vi->getStrokeCount());
  for (UINT s = 0; s < vi->getStrokeCount(); s++) {
    TStroke *stroke                        = vi->getStroke(s);
    const TStroke::OutlineOptions &options = stroke->outlineOptions();
    out += textf(
        "stroke %u style %d group %d loop %d cap %d join %d miter %.6f %.6f "
        "points %d\n",
        s, stroke->getStyle(), vi->getGroupDepth(s), (int)stroke->isSelfLoop(),
        (int)options.m_capStyle, (int)options.m_joinStyle, options.m_miterLower,
        options.m_miterUpper, stroke->getControlPointCount());
    for (int i = 0; i < stroke->getControlPointCount(); i++) {
      TThickPoint p = stroke->getControlPoint(i);
      out += textf("  %.6f %.6f %.6f\n", p.x, p.y, p.thick);
    }
  }
  // After the save: the writer gets the image as the reader made it
  vi->findRegions();
  out += textf("regions %u\n", vi->getRegionCount());
  for (UINT r = 0; r < vi->getRegionCount(); r++) {
    TRegion *region = vi->getRegion(r);
    if (region)
      out += textf("region %u style %d edges %u subregions %u\n", r,
                   region->getStyle(), region->getEdgeCount(),
                   region->getSubregionCount());
  }
  return out;
}

// Fits the box in the frame, with a margin
TAffine fitAffine(const TRectD &box, const TDimension &size) {
  const TTranslation toCenter(0.5 * size.lx, 0.5 * size.ly);
  if (box.isEmpty()) return toCenter;
  const double margin = 4;
  double sx = box.getLx() > 1e-9 ? (size.lx - 2 * margin) / box.getLx() : 1;
  double sy = box.getLy() > 1e-9 ? (size.ly - 2 * margin) / box.getLy() : 1;
  const double cx = 0.5 * (box.x0 + box.x1), cy = 0.5 * (box.y0 + box.y1);
  return toCenter * TScale(std::min(sx, sy)) * TTranslation(-cx, -cy);
}

void renderVectorImage(TVectorImage *vi, TPalette *palette,
                       const QString &path) {
  static TOfflineGL *offline = 0;  // one context for all the frames
  if (!offline) offline = new TOfflineGL(cFrameSize);

  TVectorRenderData rd(TVectorRenderData::ProductionSettings(),
                       fitAffine(vi->getBBox(), cFrameSize), TRect(cFrameSize),
                       palette);
  offline->makeCurrent();
  offline->clear(TPixel32(0, 0, 0, 0));
  offline->draw(TVectorImageP(vi), rd, true);
  TRaster32P ras = offline->getRaster();
  offline->doneCurrent();
  savePng(ras, path);
}

// Loads, saves again, dumps and renders one level. Returns a line of the
// manifest.
std::string processPli(const QString &root, const QString &rel,
                       const QDir &outDir, bool gl) {
  const QString key = keyFor(rel);
  std::string dump  = "file " + str(rel) + "\n";
  int frames = 0, renders = 0;
  std::string renderError;

  std::string error = guarded([&]() {
    TLevelReaderP reader(toPath(QDir(root).filePath(rel)));
    if (!reader) throw std::runtime_error("no level reader");
    TLevelP level = reader->loadInfo();
    if (!level) throw std::runtime_error("no level");
    TPalette *palette = level->getPalette();

    std::vector<std::pair<TFrameId, TVectorImageP>> images;
    for (TLevel::Iterator it = level->begin(); it != level->end(); ++it) {
      TImageReaderP frameReader = reader->getFrameReader(it->first);
      TVectorImageP vi          = frameReader ? frameReader->load() : TImageP();
      if (vi && palette) vi->setPalette(palette);
      images.push_back(std::make_pair(it->first, vi));
    }
    frames = (int)images.size();

    // Saved before the dump, which computes the regions
    std::string saveError = guarded([&]() {
      if (!palette) throw std::runtime_error("the level has no palette");
      TLevelWriterP writer(toPath(outDir.filePath("pli/" + key + ".pli")));
      for (const auto &frame : images)
        if (frame.second)
          writer->getFrameWriter(frame.first)
              ->save(TImageP(frame.second.getPointer()));
    });  // the writer writes the file when it is released
    if (!saveError.empty()) dump += "save error: " + saveError + "\n";

    dump += textf("frames %d\n", frames);
    for (const auto &frame : images) {
      dump += "frame " + frame.first.expand() + "\n";
      if (!frame.second) {
        dump += "not a vector image\n";
        continue;
      }
      dump += dumpVectorImage(frame.second.getPointer());
    }
    dump += dumpPalette(palette);

    if (!gl || !palette) return;
    for (const auto &frame : images) {
      if (!frame.second) continue;
      std::string e = guarded([&]() {
        renderVectorImage(
            frame.second.getPointer(), palette,
            outDir.filePath("pli/" + key + "." +
                            QString::fromStdString(frame.first.expand()) +
                            ".png"));
      });
      if (e.empty())
        renders++;
      else if (renderError.empty())
        renderError = e;
    }
  });
  if (!error.empty()) dump += "error: " + error + "\n";
  writeText(outDir.filePath("pli/" + key + ".txt"), dump);

  std::string line =
      textf("pli %s: %d frames, %d renders", str(rel).c_str(), frames, renders);
  if (!error.empty()) line += ", error: " + error;
  if (!renderError.empty()) line += ", render error: " + renderError;
  return line + "\n";
}

//=============================================================================
// Scene
//-----------------------------------------------------------------------------

const std::pair<TStageObject::Channel, const char *> cChannels[] = {
    {TStageObject::T_X, "x"},           {TStageObject::T_Y, "y"},
    {TStageObject::T_Z, "z"},           {TStageObject::T_SO, "so"},
    {TStageObject::T_Angle, "angle"},   {TStageObject::T_ScaleX, "scalex"},
    {TStageObject::T_ScaleY, "scaley"}, {TStageObject::T_Scale, "scale"},
    {TStageObject::T_ShearX, "shearx"}, {TStageObject::T_ShearY, "sheary"}};

std::string dumpCurve(const char *name, TDoubleParam *param) {
  if (!param) return textf("  %s none\n", name);
  std::string out = textf("  %s default %.6f keys %d", name,
                          param->getDefaultValue(), param->getKeyframeCount());
  for (int k = 0; k < param->getKeyframeCount(); k++) {
    const TDoubleKeyframe &key = param->getKeyframe(k);
    out += textf(" (%.6f %.6f %d)", key.m_frame, key.m_value, (int)key.m_type);
  }
  if (param->getKeyframeCount() > 0) {
    out += " values";
    for (double frame : cDumpFrames)
      out += textf(" %.6f", param->getValue(frame));
  }
  return out + "\n";
}

// Column fxs are named by their column: the parameters they hold differ
// between the builds (the time remap of tier12 is saved only when it is used)
std::string fxName(TFx *fx) {
  if (!fx) return "none";
  if (TColumnFx *columnFx = dynamic_cast<TColumnFx *>(fx))
    return textf("%s(column %d)", fx->getFxType().c_str(),
                 columnFx->getColumnIndex());
  return fx->getFxType() + ":" + str(fx->getFxId());
}

std::string dumpFx(TFx *fx) {
  std::string out =
      "fx " + fxName(fx) + " name \"" + str(fx->getName()) + "\"\n";
  for (int p = 0; p < fx->getInputPortCount(); p++) {
    TFxPort *port = fx->getInputPort(p);
    out += "  port " + fx->getInputPortName(p) + " <- " +
           fxName(port ? port->getFx() : 0) + "\n";
  }
  TParamContainer *params = fx->getParams();
  for (int i = 0; params && i < params->getParamCount(); i++) {
    TParam *param = params->getParam(i);
    if (!param) continue;
    std::string name = params->getParamName(i);
    if (TDoubleParam *curve = dynamic_cast<TDoubleParam *>(param))
      out += dumpCurve(name.c_str(), curve);
    out += "  param " + name + ":";
    for (double frame : cDumpFrames)
      out += " " + param->getValueAlias(frame, 6);
    out += "\n";
  }
  return out;
}

std::string dumpScene(ToonzScene &scene) {
  TXsheet *xsh          = scene.getXsheet();
  VersionNumber version = scene.getVersionNumber();
  std::string out = textf("version %d.%d\n", version.first, version.second);
  for (const std::string &tag : scene.getUnrecognizedSceneTags())
    out += "unrecognized " + tag + "\n";
  out += textf("framecount %d\n", xsh->getFrameCount());
  if (TCamera *camera = scene.getCurrentCamera())
    out +=
        textf("camera res %d %d size %.6f %.6f\n", camera->getRes().lx,
              camera->getRes().ly, camera->getSize().lx, camera->getSize().ly);

  TLevelSet *levels = scene.getLevelSet();
  out += textf("levels %d\n", levels->getLevelCount());
  for (int i = 0; i < levels->getLevelCount(); i++) {
    TXshLevel *level = levels->getLevel(i);
    if (level)
      out += textf("level \"%s\" type %d path \"%s\"\n",
                   str(level->getName()).c_str(), level->getType(),
                   str(level->getPath()).c_str());
  }

  out += textf("columns %d\n", xsh->getColumnCount());
  for (int c = 0; c < xsh->getColumnCount(); c++) {
    TXshColumn *column = xsh->getColumn(c);
    if (!column) {
      out += textf("column %d empty\n", c);
      continue;
    }
    out += textf("column %d type %d camstand %d preview %d opacity %d\n", c,
                 (int)column->getColumnType(), (int)column->isCamstandVisible(),
                 (int)column->isPreviewVisible(), (int)column->getOpacity());
    TXshCellColumn *cells = column->getCellColumn();
    int r0 = 0, r1 = -1;
    if (!cells || !cells->getRange(r0, r1)) continue;
    for (int r = r0; r <= r1; r++) {
      const TXshCell &cell = cells->getCell(r);
      if (cell.isEmpty())
        out += textf("  cell %d -\n", r);
      else
        out += textf("  cell %d \"%s\" %s\n", r,
                     str(cell.m_level->getName()).c_str(),
                     cell.m_frameId.expand().c_str());
    }
  }

  // Sorted by id, whatever the order of the tree
  std::map<std::string, std::string> objects;
  TStageObjectTree *tree = xsh->getStageObjectTree();
  for (int i = 0; i < tree->getStageObjectCount(); i++) {
    TStageObject *object = tree->getStageObject(i);
    if (!object) continue;
    std::string id   = object->getId().toString();
    std::string text = "object " + id + " name \"" + object->getName() +
                       "\" parent " + object->getParent().toString() + "\n";
    for (const auto &channel : cChannels)
      text += dumpCurve(channel.second, object->getParam(channel.first));
    objects[id] = text;
  }
  out += textf("stage objects %d\n", (int)objects.size());
  for (const auto &object : objects) out += object.second;

  // FX sets are ordered by address: sorted by name here
  FxDag *dag = xsh->getFxDag();
  std::vector<std::string> fxs, terminal;
  TFxSet *internalFxs = dag->getInternalFxs();
  for (int i = 0; i < internalFxs->getFxCount(); i++)
    if (TFx *fx = internalFxs->getFx(i)) fxs.push_back(dumpFx(fx));
  TFxSet *terminalFxs = dag->getTerminalFxs();
  for (int i = 0; i < terminalFxs->getFxCount(); i++)
    terminal.push_back(fxName(terminalFxs->getFx(i)));
  std::sort(fxs.begin(), fxs.end());
  std::sort(terminal.begin(), terminal.end());
  out += textf("internal fxs %d\n", (int)fxs.size());
  for (const std::string &fx : fxs) out += fx;
  out += textf("terminal fxs %d\n", (int)terminal.size());
  for (const std::string &fx : terminal) out += "  " + fx + "\n";
  for (int i = 0; i < dag->getOutputFxCount(); i++) {
    TOutputFx *output = dag->getOutputFx(i);
    TFxPort *port     = output ? output->getInputPort(0) : 0;
    out += textf("output %d <- ", i) + fxName(port ? port->getFx() : 0) + "\n";
  }
  return out;
}

// The levels of the scene: the preferred ones, completed from the list
QStringList sceneLevels(const QString &root, const QStringList &plis) {
  QStringList chosen;
  for (const char *rel : cSceneLevels)
    if (QFileInfo(QDir(root).filePath(rel)).isFile()) chosen << rel;
  for (int i = 0; i < plis.size() && chosen.size() < 3; i++)
    if (!chosen.contains(plis[i])) chosen << plis[i];
  return chosen;
}

// Three columns: one level per column, the first on ones, the second on twos
// and the third on rows 2 to 9 only. The first column is parented to a
// keyframed pegbar, the others have keys of their own, and the second goes
// through a blur FX with keys.
void buildScene(ToonzScene &scene, const QString &root, const QStringList &plis,
                const QDir &outDir) {
  TProjectManager::instance()->initializeScene(&scene);
  // The level paths are coded when the levels are loaded: with a titled scene
  // and this priority they are written relative to the scene folder
  scene.setScenePath(toPath(outDir.filePath("scene.tnz")));
  Preferences::instance()->setValue(pathAliasPriority,
                                    (int)Preferences::SceneFolderAlias, false);

  TXsheet *xsh = scene.getXsheet();
  if (TCamera *camera = scene.getCurrentCamera()) {
    camera->setSize(TDimensionD(16, 9));
    camera->setRes(TDimension(192, 108));
  }
  scene.getProperties()->cloneCamerasFrom(xsh->getStageObjectTree());

  outDir.mkpath("levels");
  const QStringList chosen = sceneLevels(root, plis);
  if (chosen.isEmpty()) throw std::runtime_error("no pli file for the scene");
  for (int c = 0; c < chosen.size(); c++) {
    const QString src = QDir(root).filePath(chosen[c]);
    const QString dst = outDir.filePath(QString("levels/level%1_").arg(c + 1) +
                                        QFileInfo(src).fileName());
    QFile::remove(dst);
    if (!QFile::copy(src, dst))
      throw std::runtime_error("cannot copy " + str(src));

    TXshLevel *level = scene.loadLevel(toPath(dst));
    if (!level) throw std::runtime_error("cannot load " + str(dst));
    std::vector<TFrameId> fids;
    if (TXshSimpleLevel *sl = level->getSimpleLevel()) fids = sl->getFids();
    if (fids.empty()) fids.push_back(TFrameId(1));

    for (int r = 0; r < 12; r++) {
      if (c == 2 && (r < 2 || r > 9)) continue;
      int index = c == 1 ? r / 2 : r;
      xsh->setCell(r, c, TXshCell(level, fids[index % fids.size()]));
    }
  }

  TStageObject *pegbar = xsh->getStageObject(TStageObjectId::PegbarId(0));
  pegbar->getParam(TStageObject::T_X)->setValue(0, 0.0);
  pegbar->getParam(TStageObject::T_X)->setValue(11, 1.5);
  pegbar->getParam(TStageObject::T_Angle)->setValue(0, 0.0);
  pegbar->getParam(TStageObject::T_Angle)->setValue(6, 30.0);
  pegbar->getParam(TStageObject::T_Angle)->setValue(11, 90.0);
  xsh->setStageObjectParent(TStageObjectId::ColumnId(0),
                            TStageObjectId::PegbarId(0));
  if (xsh->getColumnCount() > 1) {
    TStageObject *object = xsh->getStageObject(TStageObjectId::ColumnId(1));
    object->getParam(TStageObject::T_Y)->setValue(0, -0.5);
    object->getParam(TStageObject::T_Y)->setValue(11, 0.5);
  }
  if (xsh->getColumnCount() > 2) {
    TStageObject *object = xsh->getStageObject(TStageObjectId::ColumnId(2));
    object->getParam(TStageObject::T_SO)->setValue(0, 0.0);
    object->getParam(TStageObject::T_SO)->setValue(6, 1.0);
  }

  // One internal FX only: the FX sets are ordered by address
  TXshColumn *column = xsh->getColumnCount() > 1 ? xsh->getColumn(1) : 0;
  TFx *columnFx      = column ? column->getFx() : 0;
  TFx *blur          = columnFx ? TFx::create("STD_blurFx") : 0;
  if (blur && blur->getInputPortCount() > 0) {
    FxDag *dag = xsh->getFxDag();
    dag->assignUniqueId(blur);
    dag->getInternalFxs()->addFx(blur);
    blur->getInputPort(0)->setFx(columnFx);
    dag->removeFromXsheet(columnFx);
    dag->addToXsheet(blur);
    if (TDoubleParam *value = dynamic_cast<TDoubleParam *>(
            blur->getParams()->getParam("value"))) {
      value->setValue(0, 2.0);
      value->setValue(11, 8.0);
    }
  }
  xsh->updateFrameCount();
}

// Renders rows through the FX tree of the scene on this thread, as
// stdfx/tests/fxrender_test.cpp renders its FX graphs. Returns a manifest line
// per row.
std::string renderScene(ToonzScene &scene, const QDir &outDir, bool &failed) {
  std::string out;
  TCamera *camera = scene.getCurrentCamera();
  if (!camera) {
    failed = true;
    return "scene render: no camera\n";
  }
  const TDimension res = camera->getRes();
  TRenderer renderer(1);
  for (int row : cRenderRows) {
    std::string error = guarded([&]() {
      TFxP fx             = buildSceneFx(&scene, double(row), 1, false);
      TRasterFxP rasterFx = fx;
      if (!rasterFx) throw std::runtime_error("no raster fx");

      unsigned long renderId = TRenderer::buildRenderId();
      renderer.install(renderId);
      renderer.declareRenderStart(renderId);
      renderer.declareFrameStart(row);

      TRenderSettings info;
      info.m_bpp = 32;
      TTile tile;
      std::string computeError = guarded([&]() {
        rasterFx->allocateAndCompute(tile,
                                     TPointD(-0.5 * res.lx, -0.5 * res.ly), res,
                                     TRaster32P(1, 1), row, info);
      });

      renderer.declareFrameEnd(row);
      renderer.declareRenderEnd(renderId);
      renderer.uninstall();
      if (!computeError.empty()) throw std::runtime_error(computeError);

      TRaster32P ras = tile.getRaster();
      savePng(ras, outDir.filePath(QString("scene.%1.png").arg(row + 1)));
    });
    out += textf("scene render row %d: ", row + 1) +
           (error.empty() ? std::string("ok") : "error: " + error) + "\n";
    if (!error.empty()) failed = true;
  }
  return out;
}

//=============================================================================

void setLibraryRoot(const TFilePath &library) {
  TRasterImagePatternStrokeStyle::setRootDir(library);
  TVectorImagePatternStrokeStyle::setRootDir(library);
  TVectorBrushStyle::setRootDir(library);
  TPalette::setRootDir(library);
}

int runGenerate(const QString &root, const QString &outPath,
                bool requireRendering) {
  const QDir outDir(outPath);
  if (!outDir.mkpath("pli"))
    throw std::runtime_error("cannot create compatibility output directory");
  setLibraryRoot(toPath(QDir(root).filePath("stuff/library")));

  const bool gl = openGLAvailable();
  std::string manifest =
      textf("opengl %s\n", gl ? "yes" : "no (renders skipped)");
  if (requireRendering && !gl) {
    writeText(outDir.filePath("manifest.txt"), manifest);
    std::cerr << "compat_regression: rendering requires an OpenGL context"
              << std::endl;
    return 1;
  }

  const QStringList plis = findPliFiles(root, outDir.absolutePath());
  manifest += textf("pli files %d\n", (int)plis.size());
  for (const QString &rel : plis) {
    std::string line = processPli(root, rel, outDir, gl);
    std::cout << line;
    manifest += line;
  }

  ToonzScene scene;
  std::string error = guarded([&]() {
    buildScene(scene, root, plis, outDir);
    const TFilePath scenePath = toPath(outDir.filePath("scene.tnz"));
    scene.saveCopy(scenePath);

    ToonzScene loaded;
    loaded.loadTnzFile(scenePath);
    writeText(outDir.filePath("scene_dump.txt"), dumpScene(loaded));
  });
  manifest += "scene: " + (error.empty() ? std::string("ok") : error) + "\n";
  bool renderFailed = false;
  if (error.empty())
    manifest += gl ? renderScene(scene, outDir, renderFailed)
                   : std::string("scene render skipped: no OpenGL context\n");

  writeText(outDir.filePath("manifest.txt"), manifest);
  std::cout << manifest;
  if (!error.empty()) {
    std::cerr << "compat_regression: scene: " << error << std::endl;
    return 1;
  }
  if (requireRendering && renderFailed) {
    std::cerr << "compat_regression: a required scene render failed"
              << std::endl;
    return 1;
  }
  return 0;
}

int runLoad(const QString &scenePath, const QString &outPath) {
  const QDir outDir(outPath);
  outDir.mkpath(".");
  std::string dump;
  std::string error = guarded([&]() {
    ToonzScene loaded;
    loaded.loadTnzFile(toPath(QFileInfo(scenePath).absoluteFilePath()));
    dump = dumpScene(loaded);
  });
  if (!error.empty()) {
    std::cerr << "compat_regression: " << error << std::endl;
    dump += "error: " + error + "\n";
  }
  writeText(outDir.filePath("scene_dump.txt"), dump);
  return error.empty() ? 0 : 1;
}

}  // namespace

int main(int argc, char *argv[]) {
  // A QGuiApplication: the vector renderer draws through an offscreen OpenGL
  // surface (QT_QPA_PLATFORM=offscreen when headless)
  QGuiApplication app(argc, argv);
  QStringList args = app.arguments();
  const bool requireRendering =
      args.size() > 1 && args[1] == "--require-rendering";
  if (requireRendering) args.removeAt(1);
  const bool load        = args.size() == 4 && args[1] == "--load";
  if ((load && requireRendering) ||
      (!load && (args.size() != 3 || args[1].startsWith("--")))) {
    std::cerr << "Usage: compat_regression [--require-rendering] "
                 "<repository root> <output folder>\n"
                 "       compat_regression --load <scene.tnz> <output folder>"
              << std::endl;
    return 2;
  }

  int status        = 1;
  std::string error = guarded([&]() {
    QTemporaryDir dir;
    if (!dir.isValid()) throw std::runtime_error("no temporary directory");

    // The scenes need a stuff folder (for the sandbox project and the
    // preferences): use an empty one, so that the defaults are used
    QString stuffPath = QDir(dir.path()).filePath("stuff");
    QDir().mkpath(QDir(stuffPath).filePath("profiles/layouts/rooms/Default"));
    QDir().mkpath(QDir(stuffPath).filePath("library"));
    TEnv::setStuffDir(TFilePath(stuffPath));
    // created on the main thread, as in the application
    Preferences::instance();
    // written into the scene files: the same for both builds
    TEnv::setApplicationFullName("compat_regression");

    QDir().mkpath(QDir(dir.path()).filePath("cache"));
    TImageCache::instance()->setRootDir(
        toPath(QDir(dir.path()).filePath("cache")));
    TThread::init();
    initImageIo();
    initStdFx();
    initColorFx();
    setLibraryRoot(toPath(QDir(stuffPath).filePath("library")));

    status = load ? runLoad(args[2], args[3])
                  : runGenerate(QFileInfo(args[1]).absoluteFilePath(),
                                QFileInfo(args[2]).absoluteFilePath(),
                                requireRendering);
  });
  if (!error.empty()) {
    std::cerr << "compat_regression: " << error << std::endl;
    return 1;
  }
  return status;
}
