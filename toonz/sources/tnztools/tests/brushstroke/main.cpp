// Characterization of the brush and eraser tools: recorded pointer gestures
// are replayed through the tools the editor registers ("T_Brush" for vector,
// Toonz raster and raster levels, "T_Eraser" for the same three) and what
// they leave in the drawing is compared with golden data.
//
//   brushstroke_test <golden folder>            compare
//   brushstroke_test <golden folder> --update   record the golden data of
//                                               this platform
//   brushstroke_test <golden folder> --case <text>   only the cases whose
//                                               name contains <text>
//   brushstroke_test <golden folder> --tolerant compare as the other
//                                               platforms do
//
// The golden folder is tnztools/tests/brushstroke/golden; the gestures are in
// ../gestures. golden/macos-arm64-qt5 is the reference: every case, with its
// rasters as images. On that platform a result must equal it exactly.
//
// On another platform everything but the coordinates must equal it too:
// counts, order, styles, flags, saveboxes and every pixel. Coordinates,
// thicknesses and region boxes may differ by numberTolerance, because the
// processors and compilers do not round the stroke fit the same way. Where a
// platform gives another result for a known reason, golden/<platform> holds
// its own record of that case, which replaces the reference there. --update
// writes those records; a failing case prints its record, so that one can be
// taken from the log of a platform there is no machine for.

#include "cases.h"
#include "golden.h"

#include "timagecache.h"

#include <QApplication>
#include <QDir>
#include <QTemporaryDir>

#include <iostream>

namespace {

using namespace brushstroke;

const char* const referencePlatform = "macos-arm64-qt5";

// The largest difference a coordinate may have on another platform.
const double numberTolerance = 1e-6;

struct Options {
  QString goldenRoot;
  QString filter;
  bool update   = false;
  bool tolerant = false;
};

Options parse(int argc, char* argv[]) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const QString argument = QString::fromLocal8Bit(argv[i]);
    if (argument == "--update")
      options.update = true;
    else if (argument == "--tolerant")
      options.tolerant = true;
    else if (argument == "--case" && i + 1 < argc)
      options.filter = QString::fromLocal8Bit(argv[++i]);
    else if (options.goldenRoot.isEmpty() && !argument.startsWith("--"))
      options.goldenRoot = argument;
    else
      throw std::runtime_error("unknown argument " + argument.toStdString());
  }
  require(!options.goldenRoot.isEmpty(),
          "usage: brushstroke_test <golden folder> [--update] [--tolerant] "
          "[--case <text>]");
  return options;
}

struct Totals {
  int failed     = 0;
  int numbersOff = 0;
  double largest = 0;
  std::string where;  // the case with the largest difference
};

void printRecord(const Case& test, const Record& record) {
  std::cout << "  record of " << test.name << " on "
            << platformKey().toStdString() << ":" << std::endl;
  for (const std::string& line : record.lines)
    std::cout << "  | " << line << std::endl;
}

// Writes the golden data of this platform: the whole record on the reference
// platform, and elsewhere only a record the reference does not cover.
void update(const Case& test, const Record& record, const Options& options) {
  const QString own       = options.goldenRoot + "/" + platformKey();
  const QString reference = options.goldenRoot + "/" + referencePlatform;
  if (platformKey() == referencePlatform) {
    saveGolden(own, test.name, record, true);
    std::cout << "brushstroke: " << test.name << " recorded" << std::endl;
    return;
  }
  const bool covered =
      goldenExists(reference, test.name) &&
      compare(loadGolden(reference, test.name), record, numberTolerance)
          .problems.empty();
  if (covered)
    removeGolden(own, test.name);
  else
    saveGolden(own, test.name, record, false);
  std::cout << "brushstroke: " << test.name
            << (covered ? " equals the reference" : " recorded") << std::endl;
}

void check(const Case& test, const Record& record, const Options& options,
           Totals& totals) {
  const QString own       = options.goldenRoot + "/" + platformKey();
  const QString reference = options.goldenRoot + "/" + referencePlatform;
  const bool exact  = platformKey() == referencePlatform && !options.tolerant;
  const QString dir = !exact && goldenExists(own, test.name) ? own : reference;
  if (!goldenExists(dir, test.name)) {
    std::cout << "brushstroke: " << test.name
              << " FAIL: no golden data; record it with --update" << std::endl;
    ++totals.failed;
    return;
  }
  const Comparison result = compare(loadGolden(dir, test.name), record,
                                    exact ? 0.0 : numberTolerance);
  if (result.largest > totals.largest) {
    totals.largest = result.largest;
    totals.where   = test.name;
  }
  totals.numbersOff += result.numbersOff;
  std::cout << "brushstroke: " << test.name
            << (result.problems.empty() ? " ok" : " FAIL") << " against "
            << QDir(dir).dirName().toStdString();
  if (result.numbersOff)
    std::cout << ": " << result.numbersOff << " numbers differ, by up to "
              << result.largest;
  std::cout << std::endl;
  if (result.problems.empty()) return;
  ++totals.failed;
  for (size_t i = 0; i < result.problems.size() && i < 8; ++i)
    std::cout << "  " << result.problems[i] << std::endl;
  if (result.problems.size() > 8)
    std::cout << "  and " << result.problems.size() - 8 << " more lines"
              << std::endl;
  printRecord(test, record);
}

int run(const Options& options) {
  std::vector<Case> cases;
  addVectorBrushCases(cases);
  addToonzRasterBrushCases(cases);
  addFullColorBrushCases(cases);
  addEraserCases(cases);

  Application application;
  Fixture fixture(application,
                  QDir::cleanPath(QDir(options.goldenRoot).absolutePath() +
                                  "/../gestures"));
  Totals totals;
  int ran = 0;
  for (const Case& test : cases) {
    if (!QString(test.name).contains(options.filter)) continue;
    ++ran;
    Record record;
    try {
      fixture.begin();
      test.run(fixture, record);
    } catch (const std::exception& error) {
      std::cout << "brushstroke: " << test.name << " FAIL: " << error.what()
                << std::endl;
      ++totals.failed;
      continue;
    }
    if (options.update)
      update(test, record, options);
    else
      check(test, record, options, totals);
  }
  fixture.begin();
  require(ran > 0, "no case matches the filter");
  if (totals.numbersOff)
    std::cout << "brushstroke: " << totals.numbersOff
              << " numbers differ from the golden data, by up to "
              << totals.largest << " (" << totals.where << "); "
              << numberTolerance << " is allowed" << std::endl;
  std::cout << "brushstroke: " << (ran - totals.failed) << " of " << ran
            << " cases passed on " << platformKey().toStdString() << std::endl;
  return totals.failed ? 1 : 0;
}

}  // namespace

int main(int argc, char* argv[]) {
  try {
    const Options options = parse(argc, argv);
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QTemporaryDir root;
    require(root.isValid(), "cannot create the private profile folder");
    qputenv("TOONZROOT", root.path().toUtf8());
    require(QDir().mkpath(root.path() + "/profiles"),
            "cannot create the private profiles folder");
    qputenv("TOONZPROFILES", (root.path() + "/profiles").toUtf8());
    QApplication qtApplication(argc, argv);
    initializeProfile(root.path(), argv[0]);
    const int result = run(options);
    TImageCache::instance()->clear(true);
    return result;
  } catch (const std::exception& error) {
    std::cerr << "brushstroke: FAIL: " << error.what() << std::endl;
    return 1;
  }
}
