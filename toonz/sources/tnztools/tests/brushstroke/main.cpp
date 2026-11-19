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
//   brushstroke_test <golden folder> --tolerant compare with the reference
//                                               platform within the tolerance
//                                               even where exact data exists
//
// The golden folder is tnztools/tests/brushstroke/golden. It holds one folder
// per platform ("macos-arm64-qt5"); the gestures are in ../gestures. A
// platform with a folder of its own must reproduce it exactly. Any other
// platform is compared with the reference platform within the tolerance
// described in golden.h, and the differences it let through are printed.

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

// Returns whether the case matched (or was recorded).
bool check(const Case& test, const Record& record, const Options& options) {
  const QString own       = options.goldenRoot + "/" + platformKey();
  const QString reference = options.goldenRoot + "/" + referencePlatform;
  if (options.update) {
    saveGolden(own, test.name, record);
    std::cout << "brushstroke: " << test.name << " recorded" << std::endl;
    return true;
  }
  const bool exact  = !options.tolerant && goldenExists(own, test.name);
  const QString dir = exact ? own : reference;
  if (!goldenExists(dir, test.name)) {
    std::cout << "brushstroke: " << test.name
              << " FAIL: no golden data; record it with --update" << std::endl;
    return false;
  }
  const Comparison result = compare(loadGolden(dir, test.name), record,
                                    exact ? Mode::Exact : Mode::Tolerant);
  if (result.problems.empty()) {
    std::cout << "brushstroke: " << test.name
              << (exact ? " exact" : " within tolerance:") << result.note
              << std::endl;
    return true;
  }
  std::cout << "brushstroke: " << test.name << " FAIL ("
            << (exact ? "exact" : "tolerant") << " against "
            << QDir(dir).dirName().toStdString() << ")" << std::endl;
  for (const std::string& problem : result.problems)
    std::cout << "  " << problem << std::endl;
  return false;
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
  int failed = 0, ran = 0;
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
      ++failed;
      continue;
    }
    if (!check(test, record, options)) ++failed;
  }
  fixture.begin();
  require(ran > 0, "no case matches the filter");
  std::cout << "brushstroke: " << (ran - failed) << " of " << ran
            << " cases passed on " << platformKey().toStdString() << std::endl;
  return failed ? 1 : 0;
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
