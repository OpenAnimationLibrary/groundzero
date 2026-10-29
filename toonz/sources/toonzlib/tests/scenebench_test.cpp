// SPDX-License-Identifier: BSD-3-Clause

// Benchmark of scene save and load (master plan E6, profiling step; see
// doc/scene_io_profile.md). It builds a large synthetic scene in memory (level
// columns with many cells, keyframed pegbars and column objects, one FX node
// per column), then times, each phase --repeat times (3 by default):
//   - the scene file written plain and compressed (ToonzScene::saveCopy, the
//     writer of every scene save, with the Compress Scene Files preference
//     off and on);
//   - the scene file read back (ToonzScene::loadTnzFile) and freed;
//   - sub-phases: the xsheet alone through TOStream (plain, buffered to a
//     file, and compressed, to memory then one write), a raw write
//     and read of the same bytes, the LZ4 decompression of TIStream, a
//     TIStream walk of every tag and value without building objects, a
//     character scan (skipCurrentTag), and replicas of two per cell steps of
//     the loader (the frame id regular expression of TXshLevelColumn and the
//     level lookups of fixBiancoProblem in toonzscene.cpp);
//   - a separate sparse scene with an empty interior column and a configurable
//     empty tail, checking unchanged serialization and edit-time bound changes.
// Each result is printed as "NOTICE: <phase> <ms> ms (<count> <unit>)" with
// the median of the runs; the CI workflow turns these lines into annotations.
// A phase that throws prints "NOTICE: <phase> failed: <what>" and the others
// still run. Any failed phase or round-trip comparison makes the exit code 1.
//
// Usage: scenebench_test [--repeat N] [--columns N] [--rows N] [--levels N]
//                        [--pegbars N] [--trailing-columns N]

#include "toonz/toonzscene.h"
#include "toonz/txsheet.h"
#include "toonz/txshcell.h"
#include "toonz/txshcolumn.h"
#include "toonz/txshlevel.h"
#include "toonz/txshleveltypes.h"
#include "toonz/levelset.h"
#include "toonz/tproject.h"
#include "toonz/preferences.h"
#include "toonz/tstageobject.h"
#include "toonz/tstageobjectid.h"
#include "toonz/tstageobjecttree.h"
#include "toonz/fxdag.h"
#include "toonz/tcolumnfxset.h"
#include "tfx.h"
#include "tparamcontainer.h"
#include "tdoubleparam.h"
#include "tstream.h"
#include "tfilepath.h"
#include "tenv.h"
#include "tconvert.h"
#include "texception.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace {

struct Config {
  int repeat  = 3;
  int columns = 200;
  int rows    = 500;
  int levels  = 200;
  int pegbars = 20;
  int trailingColumns = 512;
};

bool g_failed = false;

TFilePath toPath(const QString &path) { return TFilePath(path.toStdWString()); }

qint64 fileSize(const QString &path) {
  const QFileInfo file(path);
  if (!file.isFile() || file.size() <= 0)
    throw std::runtime_error("missing or empty output " + path.toStdString());
  return file.size();
}

QByteArray readBytes(const QString &path) {
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly))
    throw std::runtime_error("cannot read " + path.toStdString());
  const QByteArray data = f.readAll();
  if (f.error() != QFile::NoError)
    throw std::runtime_error("cannot read " + path.toStdString());
  return data;
}

std::string formatMs(double ms) {
  std::ostringstream os;
  os << std::fixed << std::setprecision(1) << ms;
  return os.str();
}

//-----------------------------------------------------------------------------

// The time spent between start() and stop(), so that a phase can leave its
// setup and cleanup out of the measure.
class Lap {
  QElapsedTimer m_timer;
  qint64 m_ns    = 0;
  bool m_running = false;

public:
  void start() {
    m_timer.start();
    m_running = true;
  }
  void stop() {
    if (!m_running) return;
    m_ns += m_timer.nsecsElapsed();
    m_running = false;
  }
  double ms() const { return m_ns / 1e6; }
};

struct PhaseResult {
  bool m_ok      = false;
  double m_ms    = 0;
  qint64 m_count = 0;
};

double median(std::vector<double> values) {
  if (values.empty()) return 0;
  std::sort(values.begin(), values.end());
  size_t n = values.size();
  return n % 2 ? values[n / 2] : (values[n / 2 - 1] + values[n / 2]) / 2;
}

void printFailure(const std::string &phase, const std::string &what) {
  g_failed = true;
  std::cout << "NOTICE: " << phase << " failed: " << what << std::endl;
}

// Runs the body \a repeat times and prints the median. The body returns the
// count shown next to the time (bytes, cells...).
PhaseResult runPhase(const std::string &phase, const char *unit, int repeat,
                     const std::function<qint64(Lap &)> &body) {
  PhaseResult result;
  std::vector<double> times;
  for (int i = 0; i < repeat; i++) {
    Lap lap;
    try {
      result.m_count = body(lap);
    } catch (const TException &e) {
      printFailure(phase, ::to_string(e.getMessage()));
      return result;
    } catch (const std::exception &e) {
      printFailure(phase, e.what());
      return result;
    } catch (...) {
      printFailure(phase, "unknown exception");
      return result;
    }
    lap.stop();
    times.push_back(lap.ms());
    std::cout << "  " << phase << ", run " << i + 1 << ": "
              << formatMs(lap.ms()) << " ms" << std::endl;
  }
  result.m_ok = true;
  result.m_ms = median(times);
  std::cout << "NOTICE: " << phase << " " << formatMs(result.m_ms) << " ms ("
            << result.m_count << " " << unit << ")" << std::endl;
  return result;
}

//-----------------------------------------------------------------------------

struct SceneStats {
  long long m_cells    = 0;
  long long m_frameSum = 0;  // a checksum of the frame ids and their places
  int m_columns        = 0;
  int m_levels         = 0;
  int m_keyframes      = 0;
  int m_fxs            = 0;
  int m_terminalFxs    = 0;
  // Outside the timed load: compare exact drawing identities and positions,
  // including frame suffixes, rather than relying on a colliding checksum.
  std::vector<std::tuple<int, int, std::wstring, TFrameId>> m_drawings;
};

SceneStats sceneStats(ToonzScene &scene) {
  SceneStats stats;
  TXsheet *xsh    = scene.getXsheet();
  stats.m_columns = xsh->getColumnCount();
  for (int c = 0; c < stats.m_columns; c++) {
    TXshColumn *column    = xsh->getColumn(c);
    TXshCellColumn *cells = column ? column->getCellColumn() : nullptr;
    int r0 = 0, r1 = -1;
    if (!cells || !cells->getRange(r0, r1)) continue;
    for (int r = r0; r <= r1; r++) {
      const TXshCell &cell = cells->getCell(r);
      if (cell.isEmpty()) continue;
      stats.m_cells++;
      stats.m_drawings.emplace_back(c, r, cell.m_level->getName(),
                                   cell.m_frameId);
      stats.m_frameSum +=
          (long long)cell.m_frameId.getNumber() * (c % 7 + 1) * (r % 5 + 1);
    }
  }
  stats.m_levels = scene.getLevelSet()->getLevelCount();

  TStageObjectTree *tree                 = xsh->getStageObjectTree();
  const TStageObject::Channel channels[] = {
      TStageObject::T_X, TStageObject::T_Y, TStageObject::T_Angle};
  for (int i = 0; i < tree->getStageObjectCount(); i++) {
    TStageObject *object = tree->getStageObject(i);
    for (TStageObject::Channel channel : channels)
      stats.m_keyframes += object->getParam(channel)->getKeyframeCount();
  }
  stats.m_fxs = xsh->getFxDag()->getInternalFxs()->getFxCount();
  stats.m_terminalFxs = xsh->getFxDag()->getTerminalFxs()->getFxCount();
  return stats;
}

// All structure counts and every drawing placement must survive the round trip.
void checkLoaded(const std::string &phase, const SceneStats &saved,
                 const SceneStats &loaded) {
  if (loaded.m_cells != saved.m_cells ||
      loaded.m_frameSum != saved.m_frameSum ||
      loaded.m_levels != saved.m_levels ||
      loaded.m_columns != saved.m_columns ||
      loaded.m_drawings != saved.m_drawings) {
    g_failed = true;
    std::cout << "NOTICE: " << phase << " check failed: cells " << saved.m_cells
              << " saved, " << loaded.m_cells << " loaded; frame checksum "
              << saved.m_frameSum << " saved, " << loaded.m_frameSum
              << " loaded; levels " << saved.m_levels << " saved, "
              << loaded.m_levels << " loaded" << std::endl;
    std::cerr << phase << ": the loaded scene differs from the saved one"
              << std::endl;
  }
  if (loaded.m_keyframes != saved.m_keyframes || loaded.m_fxs != saved.m_fxs ||
      loaded.m_terminalFxs != saved.m_terminalFxs) {
    g_failed = true;
    std::cout << "NOTICE: " << phase << " differs: keyframes "
              << saved.m_keyframes << " saved, " << loaded.m_keyframes
              << " loaded; fx nodes " << saved.m_fxs << " saved, "
              << loaded.m_fxs << " loaded; terminal fx nodes "
              << saved.m_terminalFxs << " saved, " << loaded.m_terminalFxs
              << " loaded" << std::endl;
  }
}

//-----------------------------------------------------------------------------

// Builds the synthetic scene: a third of the columns with random frame ids
// (one <cell> entry per cell), a third on twos and a third on ones (long runs
// saved as one entry), each column with its own vector level (without
// drawings: only the scene file is measured), parented to a keyframed pegbar,
// with keys on its own x and a blend FX node between the column and the
// xsheet node.
void buildScene(ToonzScene &scene, const Config &cfg) {
  TXsheet *xsh = scene.getXsheet();

  std::vector<TXshLevel *> levels;
  for (int i = 0; i < cfg.levels; i++) {
    TXshLevel *level =
        scene.createNewLevel(PLI_XSHLEVEL, L"L" + std::to_wstring(i + 1));
    if (!level) throw std::runtime_error("cannot create a level");
    levels.push_back(level);
  }
  if (levels.empty()) throw std::runtime_error("no levels");

  for (int c = 0; c < cfg.columns; c++) {
    TXshLevel *level = levels[c % levels.size()];
    unsigned seed    = 12345u + 7919u * c;
    for (int r = 0; r < cfg.rows; r++) {
      int frame;
      if (c % 3 == 0) {
        seed  = seed * 1103515245u + 12345u;
        frame = 1 + (seed >> 16) % 48;
      } else if (c % 3 == 1)
        frame = 1 + (r / 2) % 24;
      else
        frame = 1 + r % 24;
      xsh->setCell(r, c, TXshCell(level, TFrameId(frame)));
    }
  }

  for (int p = 0; p < cfg.pegbars; p++) {
    TStageObject *pegbar = xsh->getStageObject(TStageObjectId::PegbarId(p));
    for (int f = 0; f < cfg.rows; f += 4) {
      pegbar->getParam(TStageObject::T_X)->setValue(f, std::sin(0.05 * f + p));
      pegbar->getParam(TStageObject::T_Y)->setValue(f, std::cos(0.05 * f + p));
      pegbar->getParam(TStageObject::T_Angle)->setValue(f, (f * 3 + p) % 90);
    }
  }

  FxDag *dag = xsh->getFxDag();
  for (int c = 0; c < cfg.columns; c++) {
    TStageObjectId columnId = TStageObjectId::ColumnId(c);
    if (cfg.pegbars > 0)
      xsh->setStageObjectParent(columnId,
                                TStageObjectId::PegbarId(c % cfg.pegbars));
    TStageObject *object = xsh->getStageObject(columnId);
    for (int f = c % 10; f < cfg.rows; f += 10)
      object->getParam(TStageObject::T_X)->setValue(f, 0.1 * (f % 7));

    TXshColumn *column = xsh->getColumn(c);
    TFx *columnFx      = column ? column->getFx() : nullptr;
    TFx *fx            = TFx::create("blendFx");
    if (!fx) throw std::runtime_error("benchmark blendFx is not registered");
    dag->assignUniqueId(fx);
    dag->getInternalFxs()->addFx(fx);
    if (columnFx && fx->getInputPortCount() > 0) {
      fx->getInputPort(0)->setFx(columnFx);
      dag->removeFromXsheet(columnFx);
    }
    dag->addToXsheet(fx);
    if (TDoubleParam *value =
            dynamic_cast<TDoubleParam *>(fx->getParams()->getParam("value")))
      for (int f = 0; f < cfg.rows; f += 8) value->setValue(f, (f * 3) % 100);
  }
  xsh->updateFrameCount();
}

//-----------------------------------------------------------------------------

// Reads every tag and value up to the end tag of the open one (left to the
// caller), without building any object: the parse cost of TIStream alone.
long long walkTags(TIStream &is, long long &cellTags) {
  long long count = 0;
  std::string tagName, value;
  for (;;) {
    if (is.matchTag(tagName)) {
      count++;
      if (tagName == "cell") cellTags++;
      if (!is.isBeginEndTag()) {
        count += walkTags(is, cellTags);
        if (!is.matchEndTag()) throw TException(tagName + ": missing end tag");
      }
    } else if (is.eos())
      return count;
    else
      is >> value;
  }
}

long long walkFile(const TFilePath &fp, long long &cellTags) {
  TIStream is(fp);
  if (!is) throw TException("cannot open the scene file");
  std::string tagName;
  if (!is.matchTag(tagName)) throw TException("no root tag");
  long long count = 1 + walkTags(is, cellTags);
  if (!is.matchEndTag()) throw TException("the root tag is not closed");
  return count;
}

// Writes only the xsheet, as ToonzScene::writeSceneFile does inside <xsheet>.
qint64 writeXsheet(TXsheet *xsh, const QString &path, bool compressed) {
  {
    TOStream os(toPath(path), compressed);
    if (!os.checkStatus()) throw TException("cannot open the output stream");
    os.openChild("xsheet");
    os << *xsh;
    os.closeChild();
    if (!os.checkStatus()) throw TException("cannot write the xsheet");
  }  // a compressed stream writes its file here
  return fileSize(path);
}

// Turns the Compress Scene Files preference on for a scope (not saved).
struct CompressScenes {
  CompressScenes() {
    Preferences::instance()->setValue(compressSceneFiles, true, false);
  }
  ~CompressScenes() {
    Preferences::instance()->setValue(compressSceneFiles, false, false);
  }
};

//-----------------------------------------------------------------------------

int parseInt(const QStringList &args, int &i, int fallback, int lo, int hi) {
  if (i + 1 >= args.size()) return fallback;
  bool ok   = false;
  int value = args[++i].toInt(&ok);
  return ok ? std::max(lo, std::min(hi, value)) : fallback;
}

Config parseArguments(const QStringList &args) {
  Config cfg;
  for (int i = 1; i < args.size(); i++) {
    const QString &arg = args[i];
    if (arg == "--repeat")
      cfg.repeat = parseInt(args, i, cfg.repeat, 1, 99);
    else if (arg == "--columns")
      cfg.columns = parseInt(args, i, cfg.columns, 1, 10000);
    else if (arg == "--rows")
      cfg.rows = parseInt(args, i, cfg.rows, 1, 100000);
    else if (arg == "--levels")
      cfg.levels = parseInt(args, i, cfg.levels, 1, 10000);
    else if (arg == "--pegbars")
      cfg.pegbars = parseInt(args, i, cfg.pegbars, 0, 1000);
    else if (arg == "--trailing-columns")
      cfg.trailingColumns = parseInt(args, i, cfg.trailingColumns, 1, 10000);
    else
      std::cerr << "ignored argument " << arg.toStdString() << std::endl;
  }
  return cfg;
}

// Keep this separate from the dense scene so its historical timings remain
// comparable. Empty interior columns must survive; only the empty tail is
// omitted. Bounds must be recomputed when later edits expand or shrink the sheet.
void runSparseScene(const Config &cfg, const QString &dirPath) {
  const QDir dir(dirPath);
  ToonzScene scene;
  TProjectManager::instance()->initializeScene(&scene);
  TXsheet *xsh = scene.getXsheet();
  TXshLevel *level = scene.createNewLevel(PLI_XSHLEVEL, L"Sparse");
  if (!level) throw std::runtime_error("cannot create sparse fixture level");
  xsh->setCell(0, 0, TXshCell(level, TFrameId(1)));
  xsh->setCell(5, 2, TXshCell(level, TFrameId(2)));
  xsh->getStageObject(TStageObjectId::ColumnId(1))
      ->getParam(TStageObject::T_X)->setValue(4, 12.5);
  // setCell does not create stage objects when column-name linking is off,
  // but insertColumn materializes every earlier column's stage object. Create
  // those initial objects before taking the byte reference, so padding adds
  // only metadata beyond the saved boundary.
  for (int c = 0; c < xsh->getColumnCount(); ++c)
    xsh->getStageObject(TStageObjectId::ColumnId(c));
  const SceneStats expected = sceneStats(scene);
  const QString referencePath = dir.filePath("sparse-reference.txt");
  writeXsheet(xsh, referencePath, false);
  const QByteArray reference = readBytes(referencePath);

  for (int c = 0; c < cfg.trailingColumns; ++c)
    xsh->insertColumn(xsh->getColumnCount());
  const int paddedColumns = xsh->getColumnCount();

  auto checkRoundTrip = [&](const QString &path, const SceneStats &stats) {
    ToonzScene loaded;
    loaded.loadTnzFile(toPath(path));
    checkLoaded("sparse scene", stats, sceneStats(loaded));
    TXsheet *loadedXsh = loaded.getXsheet();
    if (!loadedXsh->getColumn(1) || !loadedXsh->isColumnEmpty(1) ||
        loadedXsh->getStageObject(TStageObjectId::ColumnId(1))
                ->getParam(TStageObject::T_X)->getValue(4) != 12.5)
      throw std::runtime_error("sparse save lost interior column state");
  };

  for (bool compressed : {false, true}) {
    Preferences::instance()->setValue(compressSceneFiles, compressed, false);
    const QString path = dir.filePath(
        compressed ? "sparse-compressed.tnz" : "sparse-plain.tnz");
    runPhase(compressed ? "save sparse compressed" : "save sparse plain",
             "trailing columns", cfg.repeat, [&](Lap &lap) -> qint64 {
               lap.start();
               scene.saveCopy(toPath(path));
               lap.stop();
               fileSize(path);
               checkRoundTrip(path, expected);
               if (xsh->getColumnCount() != paddedColumns ||
                   xsh->getFirstFreeColumnIndex() != expected.m_columns)
                 throw std::runtime_error("save mutated sparse column bounds");
               return cfg.trailingColumns;
             });
  }
  Preferences::instance()->setValue(compressSceneFiles, false, false);

  const QString paddedPath = dir.filePath("sparse-padded.txt");
  writeXsheet(xsh, paddedPath, false);
  const QByteArray padded = readBytes(paddedPath);
  if (padded != reference) {
    const auto difference = std::mismatch(reference.cbegin(), reference.cend(),
                                          padded.cbegin(), padded.cend());
    throw std::runtime_error(
        "empty tail changed serialized xsheet contents at byte " +
        std::to_string(difference.first - reference.cbegin()) +
        " (reference length " + std::to_string(reference.size()) +
        ", padded length " + std::to_string(padded.size()) + ")");
  }

  const QString editedPath = dir.filePath("sparse-edited.tnz");
  const int lastColumn = paddedColumns - 1;
  xsh->setCell(7, lastColumn, TXshCell(level, TFrameId(3)));
  const SceneStats expanded = sceneStats(scene);
  scene.saveCopy(toPath(editedPath));
  checkRoundTrip(editedPath, expanded);
  xsh->clearCells(7, lastColumn);
  scene.saveCopy(toPath(editedPath));
  checkRoundTrip(editedPath, expected);

  // An entirely empty sheet must retain its zero-column boundary as well.
  ToonzScene emptyScene;
  TProjectManager::instance()->initializeScene(&emptyScene);
  const SceneStats emptyExpected = sceneStats(emptyScene);
  for (int c = 0; c < cfg.trailingColumns; ++c)
    emptyScene.getXsheet()->insertColumn(c);
  const QString emptyPath = dir.filePath("sparse-empty.tnz");
  emptyScene.saveCopy(toPath(emptyPath));
  ToonzScene emptyLoaded;
  emptyLoaded.loadTnzFile(toPath(emptyPath));
  checkLoaded("empty sparse scene", emptyExpected, sceneStats(emptyLoaded));
}

void run(const Config &cfg, const QString &dirPath) {
  const QDir dir(dirPath);
  const QString plainPath = dir.filePath("plain.tnz");
  const QString lz4Path   = dir.filePath("compressed.tnz");
  const QString xshPath   = dir.filePath("xsheet.txt");
  const QString rawPath   = dir.filePath("raw.bin");
  const int repeat        = cfg.repeat;

  Preferences::instance()->setValue(compressSceneFiles, false, false);

  ToonzScene scene;
  TProjectManager::instance()->initializeScene(&scene);

  QElapsedTimer buildTimer;
  buildTimer.start();
  try {
    buildScene(scene, cfg);
  } catch (const TException &e) {
    printFailure("build scene", ::to_string(e.getMessage()));
    return;
  } catch (const std::exception &e) {
    printFailure("build scene", e.what());
    return;
  }
  const double buildMs   = buildTimer.nsecsElapsed() / 1e6;
  const SceneStats saved = sceneStats(scene);
  std::cout << "NOTICE: scene " << cfg.columns << " columns x " << cfg.rows
            << " rows: " << saved.m_cells << " cells, " << saved.m_levels
            << " levels, " << cfg.pegbars << " pegbars, " << saved.m_keyframes
            << " keyframes, " << saved.m_fxs << " fx nodes; median of "
            << repeat << " runs" << std::endl;
  std::cout << "NOTICE: build scene " << formatMs(buildMs) << " ms ("
            << saved.m_cells << " cells)" << std::endl;

  // Saves
  PhaseResult savePlain =
      runPhase("save plain", "bytes", repeat, [&](Lap &lap) -> qint64 {
        lap.start();
        scene.saveCopy(toPath(plainPath));
        lap.stop();
        return fileSize(plainPath);
      });
  PhaseResult saveLz4 =
      runPhase("save compressed", "bytes", repeat, [&](Lap &lap) -> qint64 {
        CompressScenes compress;
        lap.start();
        scene.saveCopy(toPath(lz4Path));
        lap.stop();
        return fileSize(lz4Path);
      });
  runPhase("xsheet saveData plain stream", "bytes", repeat,
           [&](Lap &lap) -> qint64 {
             lap.start();
             qint64 size = writeXsheet(scene.getXsheet(), xshPath, false);
             lap.stop();
             return size;
           });
  runPhase("xsheet saveData compressed stream", "bytes", repeat,
           [&](Lap &lap) -> qint64 {
             lap.start();
             qint64 size = writeXsheet(scene.getXsheet(), xshPath, true);
             lap.stop();
             return size;
           });
  runPhase("write plain bytes", "bytes", repeat, [&](Lap &lap) -> qint64 {
    const QByteArray data = readBytes(plainPath);
    lap.start();
    QFile f(rawPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
      throw std::runtime_error("cannot write the raw file");
    if (f.write(data) != data.size())
      throw std::runtime_error("short write of the raw file");
    f.close();
    lap.stop();
    return data.size();
  });

  // Loads
  bool plainChecked = false, lz4Checked = false;
  PhaseResult loadPlain =
      runPhase("load plain", "bytes", repeat, [&](Lap &lap) -> qint64 {
        std::unique_ptr<ToonzScene> loaded(new ToonzScene());
        lap.start();
        loaded->loadTnzFile(toPath(plainPath));
        lap.stop();
        if (!plainChecked) {
          checkLoaded("load plain", saved, sceneStats(*loaded));
          plainChecked = true;
        }
        return fileSize(plainPath);
      });
  PhaseResult loadLz4 =
      runPhase("load compressed", "bytes", repeat, [&](Lap &lap) -> qint64 {
        std::unique_ptr<ToonzScene> loaded(new ToonzScene());
        lap.start();
        loaded->loadTnzFile(toPath(lz4Path));
        lap.stop();
        if (!lz4Checked) {
          checkLoaded("load compressed", saved, sceneStats(*loaded));
          lz4Checked = true;
        }
        return fileSize(lz4Path);
      });
  runPhase("free loaded scene", "bytes", repeat, [&](Lap &lap) -> qint64 {
    std::unique_ptr<ToonzScene> loaded(new ToonzScene());
    loaded->loadTnzFile(toPath(plainPath));
    lap.start();
    loaded.reset();
    lap.stop();
    return fileSize(plainPath);
  });
  runPhase("read plain bytes", "bytes", repeat, [&](Lap &lap) -> qint64 {
    lap.start();
    const QByteArray data = readBytes(plainPath);
    lap.stop();
    return data.size();
  });
  runPhase("open compressed", "bytes", repeat, [&](Lap &lap) -> qint64 {
    lap.start();
    {
      TIStream is(toPath(lz4Path));  // reads and decompresses the file
      if (!is) throw TException("cannot open the compressed scene");
    }
    lap.stop();
    return fileSize(lz4Path);
  });
  long long cellTags = 0;
  runPhase("parse plain", "bytes", repeat, [&](Lap &lap) -> qint64 {
    long long cells = 0;
    lap.start();
    long long tags = walkFile(toPath(plainPath), cells);
    lap.stop();
    if (!cellTags)
      std::cout << "  parse plain: " << tags << " tags, " << cells
                << " <cell> entries" << std::endl;
    cellTags = cells;
    return fileSize(plainPath);
  });
  runPhase("parse compressed", "bytes", repeat, [&](Lap &lap) -> qint64 {
    long long cells = 0;
    lap.start();
    walkFile(toPath(lz4Path), cells);
    lap.stop();
    return fileSize(lz4Path);
  });
  runPhase("scan plain", "bytes", repeat, [&](Lap &lap) -> qint64 {
    lap.start();
    {
      TIStream is(toPath(plainPath));
      std::string tagName;
      if (!is || !is.matchTag(tagName)) throw TException("no root tag");
      is.skipCurrentTag();  // reads character by character to the end tag
    }
    lap.stop();
    return fileSize(plainPath);
  });

  // Replicas of per cell steps of the loader, run once per <cell> entry
  const qint64 entries = cellTags > 0 ? cellTags : saved.m_cells;
  runPhase("cell frame id regex", "cells", repeat, [&](Lap &lap) -> qint64 {
    // TXshLevelColumn::loadData -> qstringToFrameId, per <cell>
    const QString text = QString::fromStdString(TFrameId(7).expand());
    qint64 matched     = 0;
    lap.start();
    for (qint64 i = 0; i < entries; i++) {
      QRegularExpression rx(QString("^%1$").arg(TFilePath::fidRegExpStr()));
      if (rx.match(text).hasMatch()) matched++;
    }
    lap.stop();
    if (matched != entries) throw std::runtime_error("the frame id is lost");
    return entries;
  });
  runPhase("cell frame id expand", "cells", repeat, [&](Lap &lap) -> qint64 {
    // TXshLevelColumn::saveData -> TFrameId::expand, per <cell>
    qint64 length = 0;
    lap.start();
    for (qint64 i = 0; i < entries; i++)
      length += TFrameId(1 + int(i % 48)).expand().size();
    lap.stop();
    if (length <= 0) throw std::runtime_error("empty frame ids");
    return entries;
  });
  runPhase("cell level lookups", "cells", repeat, [&](Lap &lap) -> qint64 {
    // fixBiancoProblem (toonzscene.cpp) after every load, here on the built
    // scene: a copy of the cell and two lookups by level name per cell
    TXsheet *xsh        = scene.getXsheet();
    TLevelSet *levelSet = scene.getLevelSet();
    qint64 cells = 0, found = 0;
    lap.start();
    for (int c = 0; c < xsh->getColumnCount(); c++) {
      TXshColumn *column         = xsh->getColumn(c);
      TXshCellColumn *cellColumn = column ? column->getCellColumn() : nullptr;
      int r0 = 0, r1 = -1;
      if (!cellColumn || !cellColumn->getRange(r0, r1)) continue;
      for (int r = r0; r <= r1; r++) {
        TXshCell cell = cellColumn->getCell(r);
        if (cell.isEmpty()) continue;
        cells++;
        TXshLevel *level = cell.m_level.getPointer();
        levelSet->insertLevel(level);  // already there: no change
        if (levelSet->getLevel(level->getName()) == level) found++;
      }
    }
    lap.stop();
    if (found != cells) throw std::runtime_error("a level is not found");
    return cells;
  });

  // Summary
  auto ms = [](const PhaseResult &r) {
    return r.m_ok ? formatMs(r.m_ms) + " ms" : std::string("failed");
  };
  std::ostringstream summary;
  summary << "summary " << cfg.columns << "x" << cfg.rows << ": plain "
          << savePlain.m_count << " bytes, save " << ms(savePlain) << ", load "
          << ms(loadPlain) << "; compressed " << saveLz4.m_count
          << " bytes, save " << ms(saveLz4) << ", load " << ms(loadLz4);
  if (savePlain.m_ok && saveLz4.m_ok && savePlain.m_count > 0)
    summary << " (" << (100 * saveLz4.m_count / savePlain.m_count)
            << "% of plain)";
  std::cout << "NOTICE: " << summary.str() << std::endl;

  scene.clear();  // removes the untitled scene folder
}

}  // namespace

int main(int argc, char *argv[]) {
  QCoreApplication app(argc, argv);
  const Config cfg = parseArguments(app.arguments());

  try {
    QTemporaryDir dir;
    if (!dir.isValid()) throw std::runtime_error("no temporary directory");

    // The scenes need a stuff folder (for the sandbox project and the
    // preferences): use an empty one, so that the defaults are used
    QString stuffPath = QDir(dir.path()).filePath("stuff");
    if (!QDir().mkpath(
            QDir(stuffPath).filePath("profiles/layouts/rooms/Default")))
      throw std::runtime_error("cannot create temporary stuff directory");
    TEnv::setStuffDir(TFilePath(stuffPath));
    Preferences::instance();

    QString workPath = QDir(dir.path()).filePath("scenes");
    if (!QDir().mkpath(workPath))
      throw std::runtime_error("cannot create temporary scene directory");
    run(cfg, workPath);
    runSparseScene(cfg, workPath);
  } catch (const TException &e) {
    printFailure("scenebench", ::to_string(e.getMessage()));
  } catch (const std::exception &e) {
    printFailure("scenebench", e.what());
  } catch (...) {
    printFailure("scenebench", "unknown exception");
  }

  if (g_failed) {
    std::cerr << "scenebench_test: a phase or round-trip comparison failed"
              << std::endl;
    return 1;
  }
  std::cout << "scenebench_test done" << std::endl;
  return 0;
}
