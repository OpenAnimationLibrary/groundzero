// Regression test of the scene checker (doc/scene_checker.md): each check on a
// scene built in memory, once with the issue (it is found) and once without
// (it is not), and the statistics.

#include "toonz/scenechecker.h"

#include "toonz/levelproperties.h"
#include "toonz/levelset.h"
#include "toonz/sceneproperties.h"
#include "toonz/tcamera.h"
#include "toonz/toonzscene.h"
#include "toonz/tproject.h"
#include "toonz/tstageobject.h"
#include "toonz/tstageobjecttree.h"
#include "toonz/txsheet.h"
#include "toonz/txshcell.h"
#include "toonz/txshchildlevel.h"
#include "toonz/txshcolumn.h"
#include "toonz/txshleveltypes.h"
#include "toonz/txshsimplelevel.h"
#include "toonz/txshsoundlevel.h"
#include "tcolorstyles.h"
#include "tconvert.h"
#include "tenv.h"
#include "toutputproperties.h"
#include "tpalette.h"
#include "trasterfx.h"
#include "tstroke.h"
#include "ttoonzimage.h"
#include "tvectorimage.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using namespace SceneChecker;

void require(bool condition, const std::string &message) {
  if (!condition) throw std::runtime_error(message);
}

TFilePath toPath(const QString &path) { return TFilePath(path.toStdWString()); }

QString g_dir;  // a temporary folder for files that exist

// The issues of one kind, optionally of one level
std::vector<Issue> of(const std::vector<Issue> &issues, Kind kind,
                      const std::wstring &level = L"*") {
  std::vector<Issue> found;
  for (const Issue &issue : issues)
    if (issue.kind == kind && (level == L"*" || issue.levelName == level))
      found.push_back(issue);
  return found;
}

struct Scene {
  ToonzScene scene;
  Scene() { TProjectManager::instance()->initializeScene(&scene); }
  TXsheet *xsh() { return scene.getXsheet(); }

  // A vector level with the given drawings
  TXshSimpleLevel *vectorLevel(const std::wstring &name, int drawings) {
    TXshLevel *level = scene.createNewLevel(PLI_XSHLEVEL, name);
    require(level && level->getSimpleLevel(), "setup: no vector level");
    TXshSimpleLevel *sl = level->getSimpleLevel();
    for (int f = 1; f <= drawings; ++f)
      sl->setFrame(TFrameId(f), new TVectorImage());
    return sl;
  }
  TXshSimpleLevel *level(int type, const std::wstring &name) {
    TXshLevel *level = scene.createNewLevel(type, name);
    require(level && level->getSimpleLevel(), "setup: no level");
    return level->getSimpleLevel();
  }
  // Rows r0..r1 of column c show fid of level
  void place(TXshLevel *level, int c, int r0, int r1, int fid) {
    for (int r = r0; r <= r1; ++r)
      xsh()->setCell(r, c, TXshCell(level, TFrameId(fid)));
  }
  std::vector<Issue> check(bool unusedStyles = false) {
    Options options;
    options.unusedStyles   = unusedStyles;
    options.movieFrameRate = [](const TFilePath &) { return 0.0; };
    return SceneChecker::check(&scene, options);
  }
};

// Gives the level a file that exists, and marks it saved
void giveFile(TXshSimpleLevel *sl, const QString &fileName) {
  QString path = QDir(g_dir).filePath(fileName);
  QFile file(path);
  require(file.open(QIODevice::WriteOnly), "setup: cannot write a file");
  file.write("x");
  file.close();
  sl->setPath(toPath(path), true);
  sl->setDirtyFlag(false);
}

//------------------------------------------------------------------

void testMissingDrawings() {
  Scene s;
  TXshSimpleLevel *a = s.vectorLevel(L"A", 2);
  TXshSimpleLevel *b = s.vectorLevel(L"B", 3);
  // A: 1, 1, 3, 3, 3, 2: drawing 3 is missing on rows 3 to 5
  s.place(a, 0, 0, 1, 1);
  s.place(a, 0, 2, 4, 3);
  s.place(a, 0, 5, 5, 2);
  // B: all there
  s.place(b, 1, 0, 5, 1);
  s.place(b, 1, 6, 6, 3);

  std::vector<Issue> found = of(s.check(), MissingDrawing);
  require(found.size() == 1, "missing drawing: not found once");
  const Issue &issue = found[0];
  require(issue.severity == Error && issue.levelName == L"A" &&
              issue.column == 0 && issue.row == 2 && issue.rowCount == 3 &&
              issue.xsheet == s.xsh() && issue.subXsheet.empty(),
          "missing drawing: wrong place");
  require(issue.text.contains("A") && issue.text.contains("3"),
          "missing drawing: the text does not name it");

  // Two separate runs of the same missing drawing are two issues
  s.place(a, 0, 7, 7, 3);
  require(of(s.check(), MissingDrawing).size() == 2,
          "missing drawing: separate runs merged");

  // A level whose saved file is gone has no drawings: the file is the issue
  giveFile(a, "A.pli");
  QFile::remove(QDir(g_dir).filePath("A.pli"));
  std::vector<Issue> gone = s.check();
  require(of(gone, MissingDrawing).empty(),
          "missing drawing: reported for a level without its file");
  require(of(gone, MissingFile, L"A").size() == 1,
          "missing drawing: the missing file is not reported");
}

void testSubXsheets() {
  Scene s;
  TXshSimpleLevel *a = s.vectorLevel(L"A", 1);
  TXshLevel *sub     = s.scene.createNewLevel(CHILD_XSHLEVEL, L"sub");
  require(sub && sub->getChildLevel(), "setup: no sub-xsheet");
  TXsheet *child = sub->getChildLevel()->getXsheet();
  // Inside: drawing 1, then a missing drawing 4 and an empty column
  child->setCell(0, 0, TXshCell(a, TFrameId(1)));
  child->setCell(1, 0, TXshCell(a, TFrameId(4)));
  child->insertColumn(1);
  child->setCell(0, 2, TXshCell(a, TFrameId(1)));
  // Outside: the sub-xsheet's frames 1 and 2 (of 2), then frame 5
  s.place(sub, 0, 0, 0, 1);
  s.place(sub, 0, 1, 1, 2);
  std::vector<Issue> issues  = s.check();
  std::vector<Issue> missing = of(issues, MissingDrawing);
  require(missing.size() == 1 && missing[0].subXsheet == L"sub" &&
              missing[0].xsheet == child && missing[0].row == 1 &&
              missing[0].column == 0 && missing[0].levelName == L"A",
          "sub-xsheet: the drawing missing inside is not found");
  std::vector<Issue> empty = of(issues, EmptyColumn);
  require(empty.size() == 1 && empty[0].xsheet == child && empty[0].column == 1,
          "sub-xsheet: the empty column inside is not found");

  s.place(sub, 0, 2, 3, 5);
  missing = of(s.check(), MissingDrawing, L"sub");
  require(missing.size() == 1 && missing[0].row == 2 &&
              missing[0].rowCount == 2 && missing[0].subXsheet.empty(),
          "sub-xsheet: a frame past its end is not found");

  // A sub-xsheet used twice is walked once
  s.place(sub, 1, 0, 1, 1);
  require(of(s.check(), MissingDrawing, L"A").size() == 1,
          "sub-xsheet: walked more than once");
}

void testFiles() {
  Scene s;
  TXshSimpleLevel *a = s.vectorLevel(L"A", 1);
  s.place(a, 0, 0, 0, 1);
  // A new level: not saved yet, which is not an error
  std::vector<Issue> issues = s.check();
  require(of(issues, UnsavedLevel, L"A").size() == 1 &&
              of(issues, UnsavedLevel)[0].severity == Info,
          "files: the new level is not reported as unsaved");
  require(of(issues, MissingFile).empty(),
          "files: a new level is reported missing");

  // Saved, and its file exists: nothing
  giveFile(a, "A.pli");
  issues = s.check();
  require(of(issues, UnsavedLevel).empty() && of(issues, MissingFile).empty(),
          "files: an existing file is reported");

  // Saved, and its file is gone
  QFile::remove(QDir(g_dir).filePath("A.pli"));
  issues = s.check();
  require(of(issues, MissingFile, L"A").size() == 1 &&
              of(issues, MissingFile)[0].severity == Error,
          "files: the missing file is not found");

  // A path coded with a folder alias is decoded: +drawings is in the project
  a->setPath(TFilePath("+drawings/A.pli"), true);
  TFilePath decoded = s.scene.decodeFilePath(a->getPath());
  require(decoded != a->getPath(), "files: the alias is not decoded");
  QDir().mkpath(
      QString::fromStdWString(decoded.getParentDir().getWideString()));
  QFile file(QString::fromStdWString(decoded.getWideString()));
  require(file.open(QIODevice::WriteOnly), "files: cannot write the level");
  file.close();
  require(of(s.check(), MissingFile).empty(),
          "files: the coded path is not decoded");
  file.remove();
  require(of(s.check(), MissingFile).size() == 1,
          "files: the coded path is found without its file");
}

void testColumnsAndCast() {
  Scene s;
  TXshSimpleLevel *a = s.vectorLevel(L"A", 1);
  TXshSimpleLevel *b = s.vectorLevel(L"B", 1);
  s.place(a, 0, 0, 2, 1);
  s.place(a, 2, 0, 2, 1);
  std::vector<Issue> issues = s.check();
  require(of(issues, EmptyColumn).size() == 1 &&
              of(issues, EmptyColumn)[0].column == 1,
          "columns: the empty column between two is not found");
  require(of(issues, UnusedLevel).size() == 1 &&
              of(issues, UnusedLevel, L"B").size() == 1,
          "cast: the unused level is not found");

  s.place(b, 1, 0, 0, 1);
  issues = s.check();
  require(of(issues, EmptyColumn).empty(),
          "columns: a column with cells is reported empty");
  require(of(issues, UnusedLevel).empty(),
          "cast: a level used is reported unused");

  // A level used only inside a sub-xsheet is used
  TXshSimpleLevel *c = s.vectorLevel(L"C", 1);
  TXshLevel *sub     = s.scene.createNewLevel(CHILD_XSHLEVEL, L"sub");
  sub->getChildLevel()->getXsheet()->setCell(0, 0, TXshCell(c, TFrameId(1)));
  s.place(sub, 3, 0, 0, 1);
  require(of(s.check(), UnusedLevel).empty(),
          "cast: a level used in a sub-xsheet is reported unused");
}

// The level's palette; an empty stuff folder has no default palettes, so a
// new palette (styles 0 and 1) is given to the level
TPalette *paletteOf(TXshSimpleLevel *sl) {
  if (!sl->getPalette()) sl->setPalette(new TPalette());
  require(sl->getPalette()->getPageCount() > 0,
          "setup: the level's palette has no page");
  return sl->getPalette();
}

int addStyle(TPalette *palette, const TPixel32 &color,
             const std::wstring &name = L"") {
  TPalette::Page *page = palette->getPage(0);
  int id               = page->getStyleId(page->addStyle(color));
  if (!name.empty()) palette->getStyle(id)->setName(name);
  return id;
}

void testDuplicateStyles() {
  Scene s;
  TXshSimpleLevel *a = s.vectorLevel(L"A", 1);
  s.place(a, 0, 0, 0, 1);
  TPalette *palette = paletteOf(a);
  // The default palette has no duplicates
  require(of(s.check(), DuplicateStyle).empty(),
          "duplicates: the default palette has some");

  int red   = addStyle(palette, TPixel32(200, 10, 10), L"red");
  int other = addStyle(palette, TPixel32(200, 10, 11), L"other");
  require(of(s.check(), DuplicateStyle).empty(),
          "duplicates: a different colour is a duplicate");

  // The same colour under another name
  int copy = addStyle(palette, TPixel32(200, 10, 10), L"red copy");
  std::vector<Issue> found = of(s.check(), DuplicateStyle);
  require(found.size() == 1 && found[0].styleId == copy &&
              found[0].levelName == L"A",
          "duplicates: the copy is not found");
  require(found[0].text.contains(QString::number(red)),
          "duplicates: the text does not name the original");
  (void)other;

  // A palette shared by two levels is checked once
  TXshSimpleLevel *b = s.vectorLevel(L"B", 1);
  b->setPalette(palette);
  s.place(b, 1, 0, 0, 1);
  require(of(s.check(), DuplicateStyle).size() == 1,
          "duplicates: a shared palette is checked twice");
}

void testUnusedStyles() {
  Scene s;
  TXshSimpleLevel *t = s.level(TZP_XSHLEVEL, L"T");
  s.place(t, 0, 0, 0, 1);
  TPalette *palette = paletteOf(t);
  int paint         = addStyle(palette, TPixel32(10, 200, 10));
  int unused        = addStyle(palette, TPixel32(10, 10, 200));
  // A drawing with ink 1 and the paint style
  TRasterCM32P ras(8, 8);
  ras->fill(TPixelCM32(1, paint, 0));
  TToonzImageP ti(ras, ras->getBounds());
  t->setFrame(TFrameId(1), ti);

  std::set<int> used = usedStyleIds(t);
  require(used.count(1) && used.count(paint) && !used.count(unused),
          "unused styles: wrong styles of a raster drawing");

  // Off by default
  require(of(s.check(), UnusedStyle).empty(),
          "unused styles: checked without the option");
  std::vector<Issue> found = of(s.check(true), UnusedStyle, L"T");
  bool hasUnused = false, hasUsed = false;
  for (const Issue &issue : found) {
    hasUnused |= issue.styleId == unused;
    hasUsed |=
        issue.styleId == paint || issue.styleId == 1 || issue.styleId == 0;
  }
  require(hasUnused, "unused styles: the unused style is not found");
  require(!hasUsed, "unused styles: a used style is reported");

  // Painting with it makes it used
  ras->fill(TPixelCM32(unused, paint, 0));
  t->setFrame(TFrameId(1), TToonzImageP(ras, ras->getBounds()));
  for (const Issue &issue : of(s.check(true), UnusedStyle, L"T"))
    require(issue.styleId != unused, "unused styles: a used ink is reported");

  // Vector strokes count
  TXshSimpleLevel *v = s.vectorLevel(L"V", 1);
  s.place(v, 1, 0, 0, 1);
  int strokeStyle                 = addStyle(paletteOf(v), TPixel32(1, 2, 3));
  TVectorImageP vi                = v->getFrame(TFrameId(1), true);
  std::vector<TThickPoint> points = {TThickPoint(0, 0, 1), TThickPoint(5, 5, 1),
                                     TThickPoint(10, 0, 1)};
  TStroke *stroke                 = new TStroke(points);
  stroke->setStyle(strokeStyle);
  vi->addStroke(stroke);
  require(usedStyleIds(v).count(strokeStyle),
          "unused styles: a vector stroke's style is not used");
  for (const Issue &issue : of(s.check(true), UnusedStyle, L"V"))
    require(issue.styleId != strokeStyle,
            "unused styles: the stroke's style is reported");
}

void testDpi() {
  Scene s;
  TPointD cameraDpi  = s.scene.getCurrentCamera()->getDpi();
  TXshSimpleLevel *r = s.level(OVL_XSHLEVEL, L"R");
  s.place(r, 0, 0, 0, 1);
  LevelProperties *lp = r->getProperties();
  lp->setDpiPolicy(LevelProperties::DP_CustomDpi);
  lp->setDpi(cameraDpi);
  require(of(s.check(), LevelDpi).empty(),
          "dpi: a level at the camera DPI is reported");

  lp->setDpi(cameraDpi.x * 0.5);
  std::vector<Issue> found = of(s.check(), LevelDpi, L"R");
  require(found.size() == 1 && found[0].severity == Warning,
          "dpi: a level at half the camera DPI is not found");
  require(found[0].text.contains("200"),
          "dpi: the text does not say the size on screen");

  // The image DPI policy uses the image's DPI
  lp->setDpiPolicy(LevelProperties::DP_ImageDpi);
  lp->setImageDpi(cameraDpi);
  require(of(s.check(), LevelDpi).empty(),
          "dpi: the image DPI policy reads the custom DPI");
  lp->setImageDpi(TPointD());
  require(of(s.check(), LevelDpi, L"R").size() == 1,
          "dpi: an image without DPI is not found");

  // Vector levels have no DPI
  TXshSimpleLevel *v = s.vectorLevel(L"V", 1);
  s.place(v, 1, 0, 0, 1);
  v->getProperties()->setDpiPolicy(LevelProperties::DP_CustomDpi);
  v->getProperties()->setDpi(cameraDpi.x * 3);
  require(of(s.check(), LevelDpi, L"V").empty(),
          "dpi: a vector level is checked");
}

void testFrameRates() {
  Scene s;
  TOutputProperties *output = s.scene.getProperties()->getOutputProperties();
  output->setFrameRate(24);
  TXshSoundLevel *snd = new TXshSoundLevel(L"snd");
  snd->setType(SND_XSHLEVEL);
  snd->setScene(&s.scene);  // at the output frame rate
  s.scene.getLevelSet()->insertLevel(snd);
  s.xsh()->setCell(0, 0, TXshCell(snd, TFrameId(0)));
  require(s.xsh()->getColumn(0) && s.xsh()->getColumn(0)->getSoundColumn(),
          "setup: no sound column");
  require(of(s.check(), FrameRate).empty(),
          "frame rate: a sound at the output rate is reported");
  snd->setFrameRate(30);
  std::vector<Issue> found = of(s.check(), FrameRate, L"snd");
  require(found.size() == 1 && found[0].severity == Warning,
          "frame rate: a sound at another rate is not found");

  // Movies: their rate comes from the file
  Scene m;
  m.scene.getProperties()->getOutputProperties()->setFrameRate(24);
  TXshSimpleLevel *movie = m.level(OVL_XSHLEVEL, L"clip");
  giveFile(movie, "clip.mp4");
  m.place(movie, 0, 0, 0, 1);
  double movieRate = 24;
  Options options;
  options.movieFrameRate = [&](const TFilePath &path) {
    require(path.getType() == "mp4", "frame rate: not the movie's path");
    return movieRate;
  };
  require(of(check(&m.scene, options), FrameRate).empty(),
          "frame rate: a movie at the output rate is reported");
  movieRate = 30;
  require(of(check(&m.scene, options), FrameRate, L"clip").size() == 1,
          "frame rate: a movie at another rate is not found");
  movieRate = 0;  // unknown
  require(of(check(&m.scene, options), FrameRate).empty(),
          "frame rate: a movie of unknown rate is reported");
}

void testResolution() {
  Scene s;
  s.place(s.vectorLevel(L"A", 1), 0, 0, 0, 1);
  TCamera *camera = s.scene.getCurrentCamera();
  camera->setSize(TDimensionD(16, 9));
  camera->setRes(TDimension(1920, 1080));
  TOutputProperties *output = s.scene.getProperties()->getOutputProperties();
  output->setPath(TFilePath("+outputs/out.mp4"));
  require(of(s.check(), Resolution).empty(),
          "resolution: the default output is reported");

  // Pixels that are not square
  camera->setRes(TDimension(1920, 1440));
  std::vector<Issue> found = of(s.check(), Resolution);
  require(found.size() == 1 && found[0].severity == Warning &&
              found[0].text.contains("square"),
          "resolution: pixels that are not square are not found");
  camera->setRes(TDimension(1920, 1080));

  // A shrunk output
  TRenderSettings rs = output->getRenderSettings();
  rs.m_shrinkX = rs.m_shrinkY = 2;
  output->setRenderSettings(rs);
  found = of(s.check(), Resolution);
  require(found.size() == 1 && found[0].severity == Info &&
              found[0].text.contains("960x540"),
          "resolution: the shrunk output is not found");

  // Shrunk by 8, an mp4 would be 240x135: odd
  rs.m_shrinkX = rs.m_shrinkY = 8;
  output->setRenderSettings(rs);
  found    = of(s.check(), Resolution);
  bool odd = false;
  for (const Issue &issue : found)
    odd |= issue.severity == Warning && issue.text.contains("even");
  require(odd, "resolution: an odd movie size is not found");
  output->setPath(TFilePath("+outputs/out..png"));
  for (const Issue &issue : of(s.check(), Resolution))
    require(!issue.text.contains("even"),
            "resolution: an odd image size is reported");
  rs.m_shrinkX = rs.m_shrinkY = 1;
  output->setRenderSettings(rs);

  // A preview camera of another size
  TStageObjectTree *tree = s.xsh()->getStageObjectTree();
  TStageObjectId second  = TStageObjectId::CameraId(1);
  TCamera *previewCamera = s.xsh()->getStageObject(second)->getCamera();
  previewCamera->setSize(TDimensionD(16, 9));
  previewCamera->setRes(TDimension(1920, 1080));
  tree->setCurrentPreviewCameraId(second);
  require(of(s.check(), Resolution).empty(),
          "resolution: a preview camera of the same size is reported");
  previewCamera->setRes(TDimension(1280, 720));
  found = of(s.check(), Resolution);
  require(found.size() == 1 && found[0].text.contains("1280x720"),
          "resolution: a preview camera of another size is not found");
}

void testOrderAndNames() {
  Scene s;
  TXshSimpleLevel *a = s.vectorLevel(L"A", 1);
  s.place(a, 0, 0, 0, 2);                                      // an error
  s.vectorLevel(L"B", 1);                                      // an info
  s.scene.getCurrentCamera()->setRes(TDimension(1920, 1440));  // a warning
  std::vector<Issue> issues = s.check();
  require(issues.size() >= 3, "order: issues missing");
  for (size_t i = 1; i < issues.size(); ++i)
    require(issues[i - 1].severity > issues[i].severity ||
                (issues[i - 1].severity == issues[i].severity &&
                 issues[i - 1].kind <= issues[i].kind),
            "order: not sorted by severity, then kind");
  require(issues.front().severity == Error, "order: errors are not first");

  std::set<QString> names;
  for (int k = 0; k < KindCount; ++k) {
    QString name = kindName(Kind(k));
    require(!name.isEmpty() && !kindTitle(Kind(k)).isEmpty(),
            "names: a kind has no name");
    names.insert(name);
  }
  require((int)names.size() == KindCount, "names: two kinds share a name");
  require(kindName(MissingDrawing) == "missingDrawing" &&
              severityName(Error) == "error" &&
              severityName(Warning) == "warning" &&
              severityName(Info) == "info",
          "names: the script names changed");
}

void testStatistics() {
  Scene s;
  Statistics empty = statistics(&s.scene);
  require(empty.drawings == 0 && empty.levels == 0 && empty.columns == 0 &&
              empty.frames == 0 && empty.sceneFileSize == -1,
          "statistics: an empty scene is not empty");

  TXshSimpleLevel *a = s.vectorLevel(L"A", 3);
  TXshSimpleLevel *b = s.vectorLevel(L"B", 2);
  TXshSimpleLevel *c = s.vectorLevel(L"C", 5);  // in the cast only
  s.place(a, 0, 0, 9, 1);
  s.place(b, 2, 0, 4, 1);
  addStyle(paletteOf(a), TPixel32(1, 2, 3));
  paletteOf(b);
  c->setPalette(a->getPalette());  // a shared palette counts once
  Statistics stats = statistics(&s.scene);
  require(stats.drawings == 5, "statistics: wrong drawings");
  require(stats.levels == 3, "statistics: wrong levels");
  require(stats.columns == 2, "statistics: wrong columns");
  require(stats.frames == 10, "statistics: wrong frames");
  int styles = a->getPalette()->getStyleInPagesCount() +
               b->getPalette()->getStyleInPagesCount();
  require(stats.styles == styles && styles == 5, "statistics: wrong styles");

  TFilePath scenePath = toPath(QDir(g_dir).filePath("stats.tnz"));
  s.scene.save(scenePath, nullptr, false);
  Statistics saved = statistics(&s.scene);
  require(saved.sceneFileSize > 0 &&
              saved.sceneFileSize ==
                  QFile(QDir(g_dir).filePath("stats.tnz")).size(),
          "statistics: wrong scene file size");
}

}  // namespace

int main(int argc, char *argv[]) {
  QCoreApplication app(argc, argv);
  try {
    QTemporaryDir dir;
    require(dir.isValid(), "no temporary directory");
    // The scenes need a stuff folder: an empty one gives the defaults
    QString stuffPath = QDir(dir.path()).filePath("stuff");
    QDir().mkpath(QDir(stuffPath).filePath("profiles/layouts/rooms/Default"));
    TEnv::setStuffDir(TFilePath(stuffPath));
    g_dir = QDir(dir.path()).filePath("files");
    QDir().mkpath(g_dir);

    testMissingDrawings();
    testSubXsheets();
    testFiles();
    testColumnsAndCast();
    testDuplicateStyles();
    testUnusedStyles();
    testDpi();
    testFrameRates();
    testResolution();
    testOrderAndNames();
    testStatistics();
  } catch (const TException &e) {
    std::cerr << "scenechecker_test: " << ::to_string(e.getMessage())
              << std::endl;
    return 1;
  } catch (const std::exception &e) {
    std::cerr << "scenechecker_test: " << e.what() << std::endl;
    return 1;
  }
  std::cout << "ok: scene checker" << std::endl;
  return 0;
}
