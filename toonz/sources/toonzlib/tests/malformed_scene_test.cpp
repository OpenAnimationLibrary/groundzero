// SPDX-License-Identifier: BSD-3-Clause

// Damaged scene files must fail to load with a TException, without running
// out of time, memory or stack, and valid scenes at the loader limits must
// still load. The damaged files are edits of a scene this test writes with
// ToonzScene::saveCopy; the edits are the ones a file-mutation probe found:
// cell runs billions of rows long, text where a loader loops over tags, bad
// stage object ids and sub-xsheets nested 20000 deep.
//
// Every scene is loaded on a thread with a 1 MB stack, the size of the main
// thread on Windows. A watchdog fails the test when one case runs longer than
// 20 seconds or the process peak memory passes 1 GB.
//
//   malformed_scene_test            run every case
//   malformed_scene_test <case>...  run the named cases

#include "toonz/fxdag.h"
#include "toonz/levelset.h"
#include "toonz/preferences.h"
#include "toonz/tcolumnfx.h"
#include "toonz/tcolumnfxset.h"
#include "toonz/toonzscene.h"
#include "toonz/tproject.h"
#include "toonz/txshcell.h"
#include "toonz/txshleveltypes.h"
#include "toonz/txshsimplelevel.h"
#include "toonz/txshsoundtextcolumn.h"
#include "toonz/txshzeraryfxcolumn.h"
#include "toonz/txsheet.h"
#include "tconvert.h"
#include "tenv.h"
#include "texception.h"
#include "tfilepath.h"
#include "tfx.h"
#include "tstreamlimits.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QThread>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif

namespace {

const qint64 kMemoryLimit      = qint64(1) << 30;
const int kCaseSeconds         = 20;
const unsigned kLoadStackBytes = 1u << 20;
const int kMaxRow              = TXsheet::maxRowCount;
const int kMaxColumns          = TXsheet::maxColumnCount;
const int kDeepNesting         = 20000;
const std::string kNoMatch     = "the scene to damage has no";

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

qint64 peakResidentBytes() {
#if defined(_WIN32)
  PROCESS_MEMORY_COUNTERS counters{};
  counters.cb = sizeof(counters);
  if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
    return qint64(counters.PeakWorkingSetSize);
  return 0;
#else
  rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) != 0) return 0;
#if defined(__APPLE__)
  return qint64(usage.ru_maxrss);  // bytes
#else
  return qint64(usage.ru_maxrss) * 1024;  // KiB
#endif
#endif
}

//-----------------------------------------------------------------------------
// The watchdog ends the process: a hang or a runaway allocation in a loader
// can't be interrupted from the outside.

std::atomic<bool> g_watchdogStop{false};
std::atomic<long long> g_caseDeadline{0};  // steady clock ms, 0 = idle
std::atomic<const char*> g_currentCase{"setup"};

long long nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void watch() {
  while (!g_watchdogStop) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const long long deadline = g_caseDeadline;
    if (deadline != 0 && nowMs() > deadline) {
      std::cerr << "FAIL " << g_currentCase.load() << ": still loading after "
                << kCaseSeconds << " seconds" << std::endl;
      std::_Exit(1);
    }
    if (peakResidentBytes() > kMemoryLimit) {
      std::cerr << "FAIL " << g_currentCase.load() << ": peak memory over "
                << (kMemoryLimit >> 20) << " MB" << std::endl;
      std::_Exit(1);
    }
  }
}

//-----------------------------------------------------------------------------

struct LoadResult {
  bool loaded = false;
  std::string error;  // a TException message
  std::string other;  // any other exception
  int columns = 0, frames = 0;
};

// Loads on a thread with the stack size of the Windows main thread, where
// scenes are opened.
LoadResult loadScene(const QString& path) {
  LoadResult result;
  std::unique_ptr<QThread> thread(QThread::create([&result, path]() {
    try {
      ToonzScene scene;
      scene.loadTnzFile(TFilePath(path.toStdWString()));
      result.loaded  = true;
      result.columns = scene.getXsheet()->getColumnCount();
      result.frames  = scene.getXsheet()->getFrameCount();
    } catch (const TException& error) {
      result.error = ::to_string(error.getMessage());
    } catch (const std::exception& error) {
      result.other = error.what();
    } catch (...) {
      result.other = "unknown exception";
    }
  }));
  thread->setStackSize(kLoadStackBytes);
  thread->start();
  thread->wait();
  return result;
}

QString writeScene(const QString& dir, const QString& name,
                   const QByteArray& text) {
  const QString path = QDir(dir).filePath(name + ".tnz");
  QFile file(path);
  require(file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
              file.write(text) == text.size(),
          "cannot write " + path.toStdString());
  return path;
}

void expectRejected(const QString& dir, const QString& name,
                    const QByteArray& text) {
  const LoadResult result = loadScene(writeScene(dir, name, text));
  require(result.other.empty(), name.toStdString() +
                                    ": failed with an exception that is not "
                                    "a TException: " +
                                    result.other);
  require(!result.loaded, name.toStdString() + ": the damaged scene loaded");
  std::cout << "  " << name.toStdString() << ": rejected ("
            << result.error.substr(result.error.rfind('\n') + 1) << ")"
            << std::endl;
}

LoadResult expectLoaded(const QString& dir, const QString& name,
                        const QByteArray& text) {
  const LoadResult result = loadScene(writeScene(dir, name, text));
  require(result.loaded, name.toStdString() + ": the valid scene failed: " +
                             result.error + result.other);
  std::cout << "  " << name.toStdString() << ": loaded" << std::endl;
  return result;
}

// Replaces the first match of a pattern; "\\1" to "\\9" in the replacement
// stand for the captured groups. Fails when nothing matches, so a change in
// the writer can't make a case load the valid scene instead.
QByteArray edit(const QByteArray& text, const QString& pattern,
                const QString& replacement) {
  QString scene = QString::fromUtf8(text);
  QRegularExpression re(pattern,
                        QRegularExpression::DotMatchesEverythingOption);
  const QRegularExpressionMatch match = re.match(scene);
  require(match.hasMatch(), kNoMatch + " " + pattern.toStdString());
  QString with = replacement;
  for (int group = re.captureCount(); group >= 1; --group)
    with.replace(QString("\\%1").arg(group), match.captured(group));
  scene.replace(match.capturedStart(), match.capturedLength(), with);
  return scene.toUtf8();
}

// The first "<row> <count>" of a column's cells
QByteArray editRun(const QByteArray& text, const QString& column, qint64 row,
                   qint64 count) {
  return edit(text, "(<" + column + " id='\\d+'>.*?<cell>\\s*)-?\\d+\\s+\\d+",
              QString("\\1%1 %2").arg(row).arg(count));
}

QByteArray insertBeforeLast(const QByteArray& text, const QByteArray& tag,
                            const QByteArray& content) {
  QByteArray result = text;
  const int at      = result.lastIndexOf(tag);
  require(at >= 0, kNoMatch + " " + tag.toStdString());
  return result.insert(at, content);
}

//-----------------------------------------------------------------------------
// The valid scene: one column of each kind with cells, an FX between the
// first column and the output, and the camera.

QByteArray makeSeed(const QString& dir) {
  ToonzScene scene;
  TProjectManager::instance()->initializeScene(&scene);
  TXsheet* xsh = scene.getXsheet();

  TXshLevel* drawing = scene.createNewLevel(PLI_XSHLEVEL, L"Drawing");
  require(drawing, "cannot create a vector level");
  for (int row = 0; row < 6; ++row)
    xsh->setCell(row, 0, TXshCell(drawing, TFrameId(1 + row / 2)));

  TXshLevel* palette = scene.createNewLevel(PLT_XSHLEVEL, L"Palette");
  require(palette, "cannot create a palette level");
  xsh->setCell(0, 1, TXshCell(palette, TFrameId(1)));

  auto* card = new TXshZeraryFxColumn(3);
  card->getZeraryColumnFx()->setZeraryFx(TFx::create("colorCardFx"));
  xsh->insertColumn(2, card);

  auto* mesh = new TXshSimpleLevel(L"Mesh");
  mesh->setType(MESH_XSHLEVEL);
  mesh->setScene(&scene);
  scene.getLevelSet()->insertLevel(mesh);
  xsh->setCell(0, 3, TXshCell(mesh, TFrameId(1)));
  xsh->setCell(1, 3, TXshCell(mesh, TFrameId(1)));

  auto* text = new TXshSoundTextColumn();
  text->createSoundTextLevel(0, {"a", "<none>", "b"});
  xsh->insertColumn(4, text);

  FxDag* dag  = xsh->getFxDag();
  TFx* invert = TFx::create("invertFx");
  require(invert, "invertFx is not registered");
  dag->assignUniqueId(invert);
  dag->getInternalFxs()->addFx(invert);
  invert->getInputPort(0)->setFx(xsh->getColumn(0)->getFx());
  dag->removeFromXsheet(xsh->getColumn(0)->getFx());
  dag->addToXsheet(invert);
  xsh->updateFrameCount();

  const QString path = QDir(dir).filePath("seed.tnz");
  scene.saveCopy(TFilePath(path.toStdWString()));
  QFile file(path);
  require(file.open(QIODevice::ReadOnly), "cannot read the saved scene");
  return file.readAll();
}

// A scene whose levels are sub-xsheets each holding the next one, written
// inline in the first cell of its first column.
QByteArray nestedSubXsheets(int depth) {
  QByteArray head, tail;
  for (int d = 0; d < depth; ++d) {
    const int id     = 10 + 3 * d;
    const bool inner = d + 1 < depth;
    head += QString(
                "<childLevel id='%1'><xsheet id='%2'><columns>"
                "<levelColumn id='%3'><cells>%4")
                .arg(id)
                .arg(id + 1)
                .arg(id + 2)
                .arg(inner ? "<cell>0 1 " : "")
                .toUtf8();
    tail.prepend(QByteArray(inner ? " 1 0</cell>" : "") +
                 "</cells></levelColumn></columns></xsheet>"
                 "<name>\"sub\"</name></childLevel>");
  }
  return "<tnz version='71.1' framecount='1'><levelSet><levels>" + head + tail +
         "</levels></levelSet></tnz>";
}

// FX each written inline as the input of the previous one
QByteArray nestedFxInputs(int depth) {
  QByteArray head, tail;
  for (int d = 0; d < depth; ++d) {
    const bool inner = d + 1 < depth;
    head += QString("<invertFx id='%1'><ports>%2")
                .arg(100 + d)
                .arg(inner ? "<source>" : "")
                .toUtf8();
    tail.prepend(QByteArray(inner ? "</source>" : "") + "</ports></invertFx>");
  }
  return "<tnz version='71.1' framecount='1'><xsheet><fxnodes><internal>"
         "<fxnode>" +
         head + tail + "</fxnode></internal></fxnodes></xsheet></tnz>";
}

// An unknown FX parameter, which the loader skips tag by tag
QByteArray nestedUnknownParam(int depth) {
  return "<tnz version='71.1' framecount='1'><xsheet><fxnodes><internal>"
         "<fxnode><invertFx id='100'><params><unknown>" +
         QByteArray("<a>").repeated(depth) +
         QByteArray("</a>").repeated(depth) +
         "</unknown></params></invertFx></fxnode></internal></fxnodes>"
         "</xsheet></tnz>";
}

//-----------------------------------------------------------------------------
// Cases

struct Context {
  QString dir;
  QByteArray seed;
};

void validScene(const Context& c) {
  const LoadResult result = expectLoaded(c.dir, "seed", c.seed);
  require(result.columns == 5 && result.frames == 6,
          "the valid scene loaded " + std::to_string(result.columns) +
              " columns and " + std::to_string(result.frames) + " frames");
}

// Rows: each column kind refuses cells outside [0, maxRowCount); the last
// row still loads.
void cellRows(const Context& c) {
  const QByteArray& s = c.seed;
  const QString level = "levelColumn";
  expectLoaded(c.dir, "level-last-row", editRun(s, level, kMaxRow - 2, 2));
  expectRejected(c.dir, "level-count-over", editRun(s, level, 0, kMaxRow + 1));
  expectRejected(c.dir, "level-row-over", editRun(s, level, kMaxRow, 1));
  expectRejected(c.dir, "level-row-negative", editRun(s, level, -5, 2));
  expectRejected(c.dir, "level-count-2e9", editRun(s, level, 0, 2000000000));
  expectRejected(c.dir, "level-row-2e9", editRun(s, level, 2000000000, 2));
  expectRejected(c.dir, "level-far-pair",
                 edit(s,
                      "(<levelColumn id='\\d+'>.*?<cell>)(\\s*\\d+\\s+\\d+)"
                      "(\\s*<level id='\\d+'/>[^<]*</cell>)",
                      "\\1\\2\\3<cell>1500000000 1\\3"));
  for (const char* column : {"meshColumn", "paletteColumn", "zeraryFxColumn"})
    expectRejected(c.dir, QString(column) + "-count-over",
                   editRun(s, column, 0, kMaxRow + 1));
  expectRejected(c.dir, "zerary-count-2e9",
                 editRun(s, "zeraryFxColumn", 0, 2000000000));
  const QString text = "(<soundTextColumn id='\\d+'>.*?<cell>\\s*)\"[0-9-]+\"";
  expectRejected(c.dir, "sound-text-range-over",
                 edit(s, text, QString("\\1\"0-%1\"").arg(kMaxRow)));
  expectRejected(c.dir, "sound-text-range-int-max",
                 edit(s, text, "\\1\"0-2147483647\""));
}

// Loops that read tags until an end tag: text or a <tag/> there was never
// read, so the loop never ended.
void tagLoops(const Context& c) {
  const QString ports = "(<invertFx id='\\d+'>.*?<ports>)";
  expectRejected(c.dir, "fx-ports-text", edit(c.seed, ports, "\\1 pegbar "));
  expectRejected(c.dir, "fx-ports-empty-tag",
                 edit(c.seed, ports, "\\1<source/>"));
  expectRejected(c.dir, "fx-params-text",
                 edit(c.seed, "(<invertFx id='\\d+'>\\s*<params>)", "\\1 x "));
  expectRejected(c.dir, "keyframes-text",
                 edit(c.seed, "(<sx>)", "\\1<keyframes> x </keyframes>"));
}

// Stage object ids: an index the id can't hold, a column past the limit, a
// number that is not one or does not fit an int, and camera settings in a
// stage object that is not a camera.
void stageObjectIds(const Context& c) {
  const QString camera = "id=\"Camera1\"";
  for (const char* id : {"Camera-0", "Camera0", "Col0", "Col10001", "Colx",
                         "Peg99999999999", "1879048191", "99999999999"})
    expectRejected(c.dir, QString("id-%1").arg(id),
                   edit(c.seed, camera, QString("id=\"%1\"").arg(id)));
  expectRejected(c.dir, "id-unknown-with-camera",
                 edit(c.seed, camera, "id=\"Xamera1\""));
  expectRejected(c.dir, "id-col-2e9",
                 edit(c.seed, "id=\"Col1\"", "id=\"Col2000000000\""));
  expectLoaded(
      c.dir, "id-last-column",
      edit(c.seed, "id=\"Col1\"", QString("id=\"Col%1\"").arg(kMaxColumns)));
}

// Folded columns: runs past the last column an xsheet may have
void foldedColumns(const Context& c) {
  const QByteArray end = "</xsheet>";
  auto fan             = [&c, &end](qint64 index, qint64 count) {
    return insertBeforeLast(
        c.seed, end,
        QString("<columnFan>%1 %2</columnFan>").arg(index).arg(count).toUtf8());
  };
  expectLoaded(c.dir, "fan-all-columns", fan(0, kMaxColumns));
  expectRejected(c.dir, "fan-count-over", fan(0, kMaxColumns + 1));
  expectRejected(c.dir, "fan-index-over", fan(kMaxColumns, 1));
  expectRejected(c.dir, "fan-index-negative", fan(-1, 1));
  expectRejected(c.dir, "fan-index-2e9", fan(2000000000, 1));
}

// Nesting: each nested tag is a recursion of a loader
void nesting(const Context& c) {
  // A sub-xsheet nests 6 tags, an FX input 3
  const int depth = int(TStreamLimits::maxTagDepth);
  expectLoaded(c.dir, "sub-xsheets-under-limit",
               nestedSubXsheets(depth / 6 - 10));
  expectLoaded(c.dir, "fx-inputs-under-limit", nestedFxInputs(depth / 3 - 10));
  expectRejected(c.dir, "sub-xsheets-20000", nestedSubXsheets(kDeepNesting));
  expectRejected(c.dir, "fx-inputs-20000", nestedFxInputs(kDeepNesting));
  expectRejected(c.dir, "unknown-param-20000",
                 nestedUnknownParam(kDeepNesting));
}

struct Case {
  const char* name;
  void (*run)(const Context&);
};

const Case kCases[] = {{"valid", validScene},  {"rows", cellRows},
                       {"loops", tagLoops},    {"ids", stageObjectIds},
                       {"fan", foldedColumns}, {"nesting", nesting}};

}  // namespace

int main(int argc, char* argv[]) {
  QCoreApplication application(argc, argv);
  std::vector<std::string> selected(argv + 1, argv + argc);
  std::thread watchdog(watch);
  int failures = 0;
  try {
    QTemporaryDir storage;
    require(storage.isValid(), "cannot create a temporary directory");
    const QString stuff = QDir(storage.path()).filePath("stuff");
    require(
        QDir().mkpath(QDir(stuff).filePath("profiles/layouts/rooms/Default")),
        "cannot create the stuff directory");
    TEnv::setStuffDir(TFilePath(stuff.toStdWString()));
    Preferences::instance()->setValue(compressSceneFiles, false);
    Context context{storage.path(), makeSeed(storage.path())};
    for (const Case& c : kCases) {
      if (!selected.empty() &&
          std::find(selected.begin(), selected.end(), c.name) == selected.end())
        continue;
      std::cout << "case " << c.name << std::endl;
      g_currentCase  = c.name;
      g_caseDeadline = nowMs() + kCaseSeconds * 1000;
      try {
        c.run(context);
      } catch (const std::exception& error) {
        std::cerr << "FAIL " << c.name << ": " << error.what() << std::endl;
        ++failures;
      }
      g_caseDeadline = 0;
    }
  } catch (const std::exception& error) {
    std::cerr << "FAIL setup: " << error.what() << std::endl;
    ++failures;
  } catch (const TException& error) {
    std::cerr << "FAIL setup: " << ::to_string(error.getMessage()) << std::endl;
    ++failures;
  }
  g_watchdogStop = true;
  watchdog.join();
  std::cout << (failures ? "malformed_scene: FAILED" : "malformed_scene: PASS")
            << std::endl;
  return failures ? 1 : 0;
}
