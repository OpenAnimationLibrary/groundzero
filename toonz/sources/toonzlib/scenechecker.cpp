#include "toonz/scenechecker.h"

// TnzLib includes
#include "toonz/levelproperties.h"
#include "toonz/levelset.h"
#include "toonz/sceneproperties.h"
#include "toonz/sceneresources.h"
#include "toonz/tcamera.h"
#include "toonz/toonzscene.h"
#include "toonz/tstageobjecttree.h"
#include "toonz/txsheet.h"
#include "toonz/txshcell.h"
#include "toonz/txshchildlevel.h"
#include "toonz/txshcolumn.h"
#include "toonz/txshleveltypes.h"
#include "toonz/txshpalettelevel.h"
#include "toonz/txshsimplelevel.h"
#include "toonz/txshsoundlevel.h"

// TnzCore includes
#include "tcolorstyles.h"
#include "timageinfo.h"
#include "tlevel_io.h"
#include "toutputproperties.h"
#include "tpalette.h"
#include "trasterfx.h"
#include "tregion.h"
#include "tstroke.h"
#include "tsystem.h"
#include "ttoonzimage.h"
#include "tvectorimage.h"

#include <QFileInfo>
#include <QObject>

#include <algorithm>
#include <cmath>
#include <map>

namespace SceneChecker {

namespace {

QString name(const std::wstring &s) { return QString::fromStdWString(s); }

QString frameName(const TFrameId &fid) {
  return QString::fromStdString(fid.expand());
}

QString rowsText(int row, int count) {
  return count > 1 ? QObject::tr("rows %1 to %2").arg(row + 1).arg(row + count)
                   : QObject::tr("row %1").arg(row + 1);
}

QString number(double v) { return QString::number(v, 'g', 6); }

bool differs(double a, double b, double relative) {
  return std::abs(a - b) > relative * std::max(std::abs(a), std::abs(b));
}

double readMovieFrameRate(const TFilePath &path) {
  try {
    TLevelReaderP reader(path);
    if (!reader) return 0;
    const TImageInfo *info = reader->getImageInfo();
    return info ? info->m_frameRate : 0;
  } catch (...) {
    return 0;
  }
}

//-----------------------------------------------------------------------------

// The levels of the cast, from the scene's resources
class Collector final : public ResourceProcessor {
public:
  std::vector<TXshSimpleLevel *> m_simple;
  std::vector<TXshPaletteLevel *> m_palettes;
  std::vector<TXshSoundLevel *> m_sounds;

  void process(TXshSimpleLevel *sl) override { m_simple.push_back(sl); }
  void process(TXshPaletteLevel *pl) override { m_palettes.push_back(pl); }
  void process(TXshSoundLevel *sl) override { m_sounds.push_back(sl); }
};

bool fileExists(ToonzScene *scene, const TFilePath &path) {
  TFilePath fp = scene->decodeFilePath(path);
  if (fp.getType() == "psd") ResourceImporter::extractPsdSuffix(fp);
  return TSystem::doesExistFileOrLevel(fp);
}

//-----------------------------------------------------------------------------

class Checker {
  ToonzScene *m_scene;
  const Options &m_options;
  std::vector<Issue> &m_issues;

  std::set<TXshLevel *> m_used;               // levels used in the xsheets
  std::set<TXshSimpleLevel *> m_missingFile;  // saved, but not on disk

public:
  Checker(ToonzScene *scene, const Options &options, std::vector<Issue> &issues)
      : m_scene(scene), m_options(options), m_issues(issues) {}

  Issue &add(Severity severity, Kind kind, const QString &text) {
    Issue issue;
    issue.severity = severity;
    issue.kind     = kind;
    issue.text     = text;
    m_issues.push_back(issue);
    return m_issues.back();
  }

  void run();

private:
  void checkFiles(const Collector &cast);
  void checkXsheet(TXsheet *xsh, const std::wstring &subName,
                   std::vector<std::pair<TXsheet *, std::wstring>> &todo);
  void checkCells(TXsheet *xsh, const std::wstring &subName, int c,
                  TXshCellColumn *column);
  void checkUnusedLevels(const Collector &cast);
  void checkPalettes(const Collector &cast);
  void checkDpi(const Collector &cast);
  void checkFrameRates(const Collector &cast);
  void checkResolution();
};

//-----------------------------------------------------------------------------

void Checker::run() {
  Collector cast;
  {
    SceneResources resources(m_scene, nullptr);
    resources.accept(&cast);
  }
  m_scene->getTopXsheet()->getUsedLevels(m_used);

  checkFiles(cast);

  // The top xsheet, then each sub-xsheet once
  std::set<TXsheet *> visited;
  std::vector<std::pair<TXsheet *, std::wstring>> todo;
  todo.push_back({m_scene->getTopXsheet(), std::wstring()});
  while (!todo.empty()) {
    auto next = todo.back();
    todo.pop_back();
    if (!next.first || !visited.insert(next.first).second) continue;
    checkXsheet(next.first, next.second, todo);
  }

  checkUnusedLevels(cast);
  checkPalettes(cast);
  checkDpi(cast);
  checkFrameRates(cast);
  checkResolution();
}

//-----------------------------------------------------------------------------

void Checker::checkFiles(const Collector &cast) {
  for (TXshSimpleLevel *sl : cast.m_simple) {
    if (sl->getPath().isEmpty() || fileExists(m_scene, sl->getPath())) continue;
    // A level never saved has its drawings in memory only
    bool unsaved = sl->getProperties()->getDirtyFlag();
    if (!unsaved) m_missingFile.insert(sl);
    Issue &issue =
        unsaved ? add(Info, UnsavedLevel,
                      QObject::tr("The level %1 is not saved yet.")
                          .arg(name(sl->getName())))
                : add(Error, MissingFile,
                      QObject::tr("The file of the level %1 is missing: %2")
                          .arg(name(sl->getName()))
                          .arg(name(sl->getPath().getWideString())));
    issue.levelName = sl->getName();
  }
  for (TXshPaletteLevel *pl : cast.m_palettes) {
    if (pl->getPath().isEmpty() || fileExists(m_scene, pl->getPath())) continue;
    add(Error, MissingFile,
        QObject::tr("The file of the palette level %1 is missing: %2")
            .arg(name(pl->getName()))
            .arg(name(pl->getPath().getWideString())))
        .levelName = pl->getName();
  }
  for (TXshSoundLevel *sl : cast.m_sounds) {
    if (sl->getPath().isEmpty() || fileExists(m_scene, sl->getPath())) continue;
    add(Error, MissingFile,
        QObject::tr("The file of the sound %1 is missing: %2")
            .arg(name(sl->getName()))
            .arg(name(sl->getPath().getWideString())))
        .levelName = sl->getName();
  }
}

//-----------------------------------------------------------------------------

void Checker::checkXsheet(
    TXsheet *xsh, const std::wstring &subName,
    std::vector<std::pair<TXsheet *, std::wstring>> &todo) {
  for (int c = 0; c < xsh->getColumnCount(); ++c) {
    TXshColumn *column = xsh->getColumn(c);
    if (!column || column->isEmpty()) {
      Issue &issue =
          add(Info, EmptyColumn,
              subName.empty()
                  ? QObject::tr("Column %1 is empty.").arg(c + 1)
                  : QObject::tr("Column %1 of the sub-xsheet %2 is empty.")
                        .arg(c + 1)
                        .arg(name(subName)));
      issue.column    = c;
      issue.xsheet    = xsh;
      issue.subXsheet = subName;
      continue;
    }
    // Sound columns make their cells on the fly: they hold no drawings
    if (column->getSoundColumn()) continue;
    TXshCellColumn *cellColumn = column->getCellColumn();
    if (!cellColumn) continue;
    checkCells(xsh, subName, c, cellColumn);

    int r0, r1;
    if (!cellColumn->getRange(r0, r1)) continue;
    for (int r = r0; r <= r1; ++r) {
      TXshChildLevel *cl = cellColumn->getCell(r).getChildLevel();
      if (cl) todo.push_back({cl->getXsheet(), cl->getName()});
    }
  }
}

//-----------------------------------------------------------------------------

void Checker::checkCells(TXsheet *xsh, const std::wstring &subName, int c,
                         TXshCellColumn *column) {
  int r0, r1;
  if (!column->getRange(r0, r1)) return;

  // Consecutive cells showing the same missing drawing make one issue
  TXshLevel *runLevel = nullptr;
  TFrameId runFid;
  int runStart = -1, runCount = 0;
  auto flush = [&]() {
    if (!runLevel) return;
    QString where = subName.empty()
                        ? QObject::tr("column %1, %2")
                              .arg(c + 1)
                              .arg(rowsText(runStart, runCount))
                        : QObject::tr("column %1, %2 of the sub-xsheet %3")
                              .arg(c + 1)
                              .arg(rowsText(runStart, runCount))
                              .arg(name(subName));
    Issue &issue =
        runLevel->getChildLevel()
            ? add(Error, MissingDrawing,
                  QObject::tr("Frame %1 of the sub-xsheet %2 is past its "
                              "end (%3).")
                      .arg(runFid.getNumber())
                      .arg(name(runLevel->getName()))
                      .arg(where))
            : add(Error, MissingDrawing,
                  QObject::tr("The level %1 has no drawing %2 (%3).")
                      .arg(name(runLevel->getName()))
                      .arg(frameName(runFid))
                      .arg(where));
    issue.column    = c;
    issue.row       = runStart;
    issue.rowCount  = runCount;
    issue.levelName = runLevel->getName();
    issue.xsheet    = xsh;
    issue.subXsheet = subName;
    runLevel        = nullptr;
  };

  for (int r = r0; r <= r1; ++r) {
    const TXshCell &cell = column->getCell(r);
    bool missing         = false;
    if (TXshSimpleLevel *sl = cell.getSimpleLevel())
      // A level without its file has no drawings at all: its file is the
      // issue, not each cell
      missing = !m_missingFile.count(sl) && !sl->isFid(cell.getFrameId());
    else if (TXshChildLevel *cl = cell.getChildLevel())
      missing = cell.getFrameId().getNumber() - 1 >= cl->getFrameCount();

    if (missing && runLevel == cell.m_level.getPointer() &&
        runFid == cell.getFrameId() && runStart + runCount == r) {
      ++runCount;
      continue;
    }
    flush();
    if (missing) {
      runLevel = cell.m_level.getPointer();
      runFid   = cell.getFrameId();
      runStart = r;
      runCount = 1;
    }
  }
  flush();
}

//-----------------------------------------------------------------------------

void Checker::checkUnusedLevels(const Collector &cast) {
  TLevelSet *levels = m_scene->getLevelSet();
  for (int i = 0; i < levels->getLevelCount(); ++i) {
    TXshLevel *level = levels->getLevel(i);
    if (!level || m_used.count(level)) continue;
    add(Info, UnusedLevel,
        QObject::tr("The level %1 is in the cast but in no xsheet.")
            .arg(name(level->getName())))
        .levelName = level->getName();
  }
}

//-----------------------------------------------------------------------------

// The look of a style: everything but its name and flags
bool sameLook(const TColorStyle *a, const TColorStyle *b) {
  if (a->getTagId() != b->getTagId() || a->getMainColor() != b->getMainColor())
    return false;
  int colorCount = a->getColorParamCount(), paramCount = a->getParamCount();
  if (colorCount != b->getColorParamCount() || paramCount != b->getParamCount())
    return false;
  for (int p = 0; p < colorCount; ++p)
    if (a->getColorParamValue(p) != b->getColorParamValue(p)) return false;
  for (int p = 0; p < paramCount; ++p) {
    if (a->getParamType(p) != b->getParamType(p)) return false;
    switch (a->getParamType(p)) {
    case TColorStyle::BOOL:
      if (a->getParamValue(TColorStyle::bool_tag(), p) !=
          b->getParamValue(TColorStyle::bool_tag(), p))
        return false;
      break;
    case TColorStyle::INT:
    case TColorStyle::ENUM:
      if (a->getParamValue(TColorStyle::int_tag(), p) !=
          b->getParamValue(TColorStyle::int_tag(), p))
        return false;
      break;
    case TColorStyle::DOUBLE:
      if (a->getParamValue(TColorStyle::double_tag(), p) !=
          b->getParamValue(TColorStyle::double_tag(), p))
        return false;
      break;
    case TColorStyle::FILEPATH:
      if (a->getParamValue(TColorStyle::TFilePath_tag(), p) !=
          b->getParamValue(TColorStyle::TFilePath_tag(), p))
        return false;
      break;
    default:
      break;
    }
  }
  return true;
}

QString styleText(TPalette *palette, int styleId) {
  TColorStyle *style = palette->getStyle(styleId);
  return style && !style->getName().empty()
             ? QObject::tr("%1 (%2)").arg(styleId).arg(name(style->getName()))
             : QString::number(styleId);
}

void Checker::checkPalettes(const Collector &cast) {
  // Each palette once, with the levels drawn with it
  std::vector<TPalette *> palettes;
  std::map<TPalette *, std::vector<TXshSimpleLevel *>> levelsOf;
  std::map<TPalette *, std::wstring> owner;
  for (TXshSimpleLevel *sl : cast.m_simple) {
    TPalette *palette = sl->getPalette();
    if (!palette) continue;
    if (!levelsOf.count(palette)) {
      palettes.push_back(palette);
      owner[palette] = sl->getName();
    }
    levelsOf[palette].push_back(sl);
  }
  for (TXshPaletteLevel *pl : cast.m_palettes) {
    TPalette *palette = pl->getPalette();
    if (!palette || levelsOf.count(palette)) continue;
    palettes.push_back(palette);
    owner[palette] = pl->getName();
    levelsOf[palette];
  }

  for (TPalette *palette : palettes) {
    const std::wstring &levelName = owner[palette];

    // Duplicates: each style against the ones before it
    std::vector<int> seen;
    for (int p = 0; p < palette->getPageCount(); ++p) {
      TPalette::Page *page = palette->getPage(p);
      for (int i = 0; i < page->getStyleCount(); ++i) {
        int id = page->getStyleId(i);
        if (id <= 0) continue;  // style 0 is "none"
        TColorStyle *style = palette->getStyle(id);
        auto same          = std::find_if(seen.begin(), seen.end(), [&](int o) {
          return sameLook(palette->getStyle(o), style);
        });
        if (same != seen.end()) {
          Issue &issue =
              add(Info, DuplicateStyle,
                  QObject::tr("In the palette of %1, style %2 looks the same "
                              "as style %3.")
                      .arg(name(levelName))
                      .arg(styleText(palette, id))
                      .arg(styleText(palette, *same)));
          issue.levelName = levelName;
          issue.styleId   = id;
        } else
          seen.push_back(id);
      }
    }

    // Unused styles, over every drawing of the levels in the xsheets
    if (!m_options.unusedStyles) continue;
    std::vector<TXshSimpleLevel *> drawn;
    for (TXshSimpleLevel *sl : levelsOf[palette])
      if (m_used.count(sl) && !m_missingFile.count(sl) &&
          (sl->getType() == PLI_XSHLEVEL || sl->getType() == TZP_XSHLEVEL))
        drawn.push_back(sl);
    if (drawn.empty()) continue;
    std::set<int> used;
    for (TXshSimpleLevel *sl : drawn) {
      std::set<int> ids = usedStyleIds(sl);
      used.insert(ids.begin(), ids.end());
    }
    for (int p = 0; p < palette->getPageCount(); ++p) {
      TPalette::Page *page = palette->getPage(p);
      for (int i = 0; i < page->getStyleCount(); ++i) {
        // As Erase Unused Styles, the first two styles of the first page
        // are always kept
        if (p == 0 && i < 2) continue;
        int id = page->getStyleId(i);
        if (used.count(id)) continue;
        Issue &issue    = add(Info, UnusedStyle,
                              QObject::tr("In the palette of %1, no drawing "
                                             "uses style %2.")
                                  .arg(name(levelName))
                                  .arg(styleText(palette, id)));
        issue.levelName = levelName;
        issue.styleId   = id;
      }
    }
  }
}

//-----------------------------------------------------------------------------

void Checker::checkDpi(const Collector &cast) {
  TCamera *camera = m_scene->getCurrentCamera();
  if (!camera) return;
  TPointD cameraDpi = camera->getDpi();
  if (!(cameraDpi.x > 0) || !(cameraDpi.y > 0)) return;
  for (TXshSimpleLevel *sl : cast.m_simple) {
    if (!m_used.count(sl)) continue;
    if (sl->getType() != OVL_XSHLEVEL && sl->getType() != TZP_XSHLEVEL)
      continue;
    LevelProperties *lp = sl->getProperties();
    bool imageDpi       = lp->getDpiPolicy() == LevelProperties::DP_ImageDpi;
    TPointD dpi         = imageDpi ? lp->getImageDpi() : lp->getDpi();
    if (!(dpi.x > 0) || !(dpi.y > 0)) {
      add(Warning, LevelDpi,
          QObject::tr("The level %1 has no DPI: it is placed at the camera "
                      "DPI.")
              .arg(name(sl->getName())))
          .levelName = sl->getName();
      continue;
    }
    if (!differs(dpi.x, cameraDpi.x, 0.01) &&
        !differs(dpi.y, cameraDpi.y, 0.01))
      continue;
    add(Warning, LevelDpi,
        QObject::tr("The level %1 is at %2 DPI (%3), the camera at %4 DPI: "
                    "it shows at %5% of its size in pixels.")
            .arg(name(sl->getName()))
            .arg(number(dpi.x))
            .arg(imageDpi ? QObject::tr("the image DPI")
                          : QObject::tr("a custom DPI"))
            .arg(number(cameraDpi.x))
            .arg(number(std::round(cameraDpi.x / dpi.x * 1000) / 10)))
        .levelName = sl->getName();
  }
}

//-----------------------------------------------------------------------------

void Checker::checkFrameRates(const Collector &cast) {
  double fps = m_scene->getProperties()->getOutputProperties()->getFrameRate();
  if (!(fps > 0)) return;
  for (TXshSoundLevel *sl : cast.m_sounds) {
    if (!m_used.count(sl) || !differs(sl->getFrameRate(), fps, 1e-3)) continue;
    add(Warning, FrameRate,
        QObject::tr("The sound %1 is laid out at %2 fps, the output runs at "
                    "%3 fps: it will drift out of sync.")
            .arg(name(sl->getName()))
            .arg(number(sl->getFrameRate()))
            .arg(number(fps)))
        .levelName = sl->getName();
  }
  for (TXshSimpleLevel *sl : cast.m_simple) {
    if (!m_used.count(sl) || m_missingFile.count(sl) ||
        !isMovieType(sl->getPath()))
      continue;
    TFilePath path = m_scene->decodeFilePath(sl->getPath());
    double movie   = m_options.movieFrameRate ? m_options.movieFrameRate(path)
                                              : readMovieFrameRate(path);
    if (!(movie > 0) || !differs(movie, fps, 1e-3)) continue;
    add(Warning, FrameRate,
        QObject::tr("The movie %1 runs at %2 fps, the output at %3 fps: one "
                    "frame per cell, it will play at another speed.")
            .arg(name(sl->getName()))
            .arg(number(movie))
            .arg(number(fps)))
        .levelName = sl->getName();
  }
}

//-----------------------------------------------------------------------------

void Checker::checkResolution() {
  TStageObjectTree *tree = m_scene->getTopXsheet()->getStageObjectTree();
  TCamera *camera        = tree->getCurrentCamera();
  if (!camera) return;
  TDimension res = camera->getRes();
  if (res.lx <= 0 || res.ly <= 0) {
    add(Error, Resolution,
        QObject::tr("The camera has no resolution (%1x%2).")
            .arg(res.lx)
            .arg(res.ly));
    return;
  }
  if (!camera->isPixelSquared())
    add(Warning, Resolution,
        QObject::tr("The camera's pixels are not square: its resolution "
                    "(%1x%2) and its size (%3x%4) have different shapes.")
            .arg(res.lx)
            .arg(res.ly)
            .arg(number(camera->getSize().lx))
            .arg(number(camera->getSize().ly)));

  TOutputProperties *output = m_scene->getProperties()->getOutputProperties();
  int shrink = std::max(1, output->getRenderSettings().m_shrinkX);
  TDimension rendered(res.lx / shrink, res.ly / shrink);
  if (shrink > 1)
    add(Info, Resolution,
        QObject::tr("The output is shrunk by %1: %2x%3 instead of the "
                    "camera's %4x%5.")
            .arg(shrink)
            .arg(rendered.lx)
            .arg(rendered.ly)
            .arg(res.lx)
            .arg(res.ly));
  TDimension written = output->getOutputSize(rendered);
  if (written != rendered)
    add(Info, Resolution,
        QObject::tr("The output aspect ratio makes the frames %1x%2 from "
                    "the rendered %3x%4.")
            .arg(written.lx)
            .arg(written.ly)
            .arg(rendered.lx)
            .arg(rendered.ly));
  std::string type = output->getPath().getType();
  if ((type == "mp4" || type == "webm" || type == "mov") &&
      (written.lx % 2 || written.ly % 2))
    add(Warning, Resolution,
        QObject::tr("The output is %1x%2: a %3 movie needs an even width and "
                    "height.")
            .arg(written.lx)
            .arg(written.ly)
            .arg(QString::fromStdString(type)));

  TCamera *preview = tree->getCurrentPreviewCamera();
  if (preview && preview != camera && preview->getRes() != res)
    add(Info, Resolution,
        QObject::tr("The preview camera is %1x%2, the output camera %3x%4.")
            .arg(preview->getRes().lx)
            .arg(preview->getRes().ly)
            .arg(res.lx)
            .arg(res.ly));
}

}  // namespace

//=============================================================================

std::vector<Issue> check(ToonzScene *scene, const Options &options) {
  std::vector<Issue> issues;
  if (!scene) return issues;
  Checker(scene, options, issues).run();
  if (options.compatibility) {
    const auto compatibility = compatibilityReport(scene);
    issues.insert(issues.end(), compatibility.begin(), compatibility.end());
  }
  std::stable_sort(issues.begin(), issues.end(),
                   [](const Issue &a, const Issue &b) {
                     if (a.severity != b.severity)
                       return a.severity > b.severity;
                     return a.kind < b.kind;
                   });
  return issues;
}

//-----------------------------------------------------------------------------

QString severityName(Severity severity) {
  switch (severity) {
  case Error:
    return "error";
  case Warning:
    return "warning";
  default:
    return "info";
  }
}

QString kindName(Kind kind) {
  static const char *names[KindCount] = {
      "missingDrawing", "missingFile", "unsavedLevel",   "emptyColumn",
      "unusedLevel",    "unusedStyle", "duplicateStyle", "levelDpi",
      "frameRate",      "resolution", "compatibility"};
  return kind >= 0 && kind < KindCount ? names[kind] : "";
}

QString kindTitle(Kind kind) {
  switch (kind) {
  case MissingDrawing:
    return QObject::tr("Missing Drawings");
  case MissingFile:
    return QObject::tr("Missing Files");
  case UnsavedLevel:
    return QObject::tr("Unsaved Levels");
  case EmptyColumn:
    return QObject::tr("Empty Columns");
  case UnusedLevel:
    return QObject::tr("Unused Levels");
  case UnusedStyle:
    return QObject::tr("Unused Styles");
  case DuplicateStyle:
    return QObject::tr("Duplicate Styles");
  case LevelDpi:
    return QObject::tr("Level DPI");
  case FrameRate:
    return QObject::tr("Frame Rates");
  case Resolution:
    return QObject::tr("Resolution");
  case Compatibility:
    return QObject::tr("Older-build Compatibility");
  default:
    return QString();
  }
}

//-----------------------------------------------------------------------------

std::set<int> usedStyleIds(TXshSimpleLevel *level) {
  std::set<int> ids;
  if (!level) return ids;
  for (const TFrameId &fid : level->getFids()) {
    TImageP image = level->getFrame(fid, false);
    if (TVectorImageP vi = image) {
      for (UINT i = 0; i < vi->getStrokeCount(); ++i)
        if (TStroke *stroke = vi->getStroke(i)) ids.insert(stroke->getStyle());
      for (UINT i = 0; i < vi->getRegionCount(); ++i)
        if (TRegion *region = vi->getRegion(i)) ids.insert(region->getStyle());
    } else if (TToonzImageP ti = image) {
      TRasterCM32P ras = ti->getRaster();
      if (!ras) continue;
      ras->lock();
      for (int y = 0; y < ras->getLy(); ++y) {
        const TPixelCM32 *pix = ras->pixels(y), *end = pix + ras->getLx();
        for (; pix < end; ++pix) {
          ids.insert(pix->getInk());
          ids.insert(pix->getPaint());
        }
      }
      ras->unlock();
    }
  }
  return ids;
}

//-----------------------------------------------------------------------------

Statistics statistics(ToonzScene *scene) {
  Statistics stats;
  if (!scene) return stats;
  TXsheet *top = scene->getTopXsheet();
  std::set<TXshLevel *> used;
  top->getUsedLevels(used);
  std::set<TPalette *> palettes;
  for (TXshLevel *level : used)
    if (TXshSimpleLevel *sl = level->getSimpleLevel())
      stats.drawings += sl->getFrameCount();

  TLevelSet *levels = scene->getLevelSet();
  stats.levels      = levels->getLevelCount();
  for (int i = 0; i < stats.levels; ++i) {
    TXshLevel *level  = levels->getLevel(i);
    TPalette *palette = nullptr;
    if (TXshSimpleLevel *sl = level->getSimpleLevel())
      palette = sl->getPalette();
    else if (TXshPaletteLevel *pl = level->getPaletteLevel())
      palette = pl->getPalette();
    if (palette && palettes.insert(palette).second)
      stats.styles += palette->getStyleInPagesCount();
  }

  for (int c = 0; c < top->getColumnCount(); ++c)
    if (!top->isColumnEmpty(c)) ++stats.columns;
  stats.frames = scene->getFrameCount();

  if (!scene->isUntitled()) {
    QFileInfo file(QString::fromStdWString(
        scene->decodeFilePath(scene->getScenePath()).getWideString()));
    if (file.exists()) stats.sceneFileSize = file.size();
  }
  return stats;
}

}  // namespace SceneChecker
