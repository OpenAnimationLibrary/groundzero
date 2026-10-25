// SPDX-License-Identifier: BSD-3-Clause

// Regression test of the Python engine and of the opentoonz Python module
// (built only with WITH_PYTHON): it checks what the Python Console would print
// for a set of lines. Scenes are built in memory, with a temporary stuff
// folder, as in scriptengine_test.

#include "toonz/pythonengine.h"
#include "toonz/preferences.h"
#include "tenv.h"
#include "tfilepath.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QVariant>

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

int failures = 0;

class Console {
  PythonEngine *m_engine;
  QString m_out, m_err;

public:
  explicit Console(PythonEngine *engine) : m_engine(engine) {
    QObject::connect(
        m_engine, &PythonEngine::output, m_engine,
        [this](int type, const QString &text) {
          if (type == PythonEngine::StdErr)
            m_err += text;
          else
            m_out += text;
        },
        Qt::DirectConnection);
  }

  void clear() {
    m_out.clear();
    m_err.clear();
  }
  const QString &out() const { return m_out; }
  const QString &err() const { return m_err; }

  // pushes the line, returns whether more lines are needed
  bool push(const QString &line) {
    clear();
    return m_engine->push(line);
  }

  bool runFile(const QString &path) {
    clear();
    return m_engine->runFile(path);
  }
};

void fail(const QString &what, const QString &expected,
          const Console &console) {
  ++failures;
  std::cerr << "FAIL: " << what.toStdString()
            << "\n  expected: " << expected.toStdString()
            << "\n  stdout: " << console.out().toStdString()
            << "\n  stderr: " << console.err().toStdString() << std::endl;
}

// line must print exactly out (and nothing to stderr)
void expect(Console &console, const QString &line, const QString &out) {
  bool more = console.push(line);
  if (more || console.out() != out || !console.err().isEmpty())
    fail(line, out, console);
}

// line must print nothing
void run(Console &console, const QString &line) { expect(console, line, ""); }

// line must print an error containing text
void expectError(Console &console, const QString &line, const QString &text) {
  bool more = console.push(line);
  if (more || !console.out().isEmpty() || !console.err().contains(text))
    fail(line, "an error containing \"" + text + "\"", console);
}

void testConsole(Console &console) {
  expect(console, "1 + 1", "2\n");
  expect(console, "'abc'", "'abc'\n");
  expect(console, "print('hello', 3)", "hello 3\n");
  run(console, "x = 3");
  expect(console, "x * 2", "6\n");

  // multi-line statements
  if (!console.push("def twice(v):")) fail("def twice(v):", "more", console);
  if (!console.push("    return v * 2")) fail("return", "more", console);
  if (console.push("")) fail("(empty line)", "no more", console);
  expect(console, "twice(21)", "42\n");

  // an incomplete statement can be discarded
  if (!console.push("for i in range(3):")) fail("for", "more", console);
  PythonEngine::instance()->resetInput();
  expect(console, "x", "3\n");

  expectError(console, "1 / 0", "ZeroDivisionError");
  expectError(console, "1 = 2", "SyntaxError");
  expectError(console, "import sys; print('to stderr', file=sys.stderr)",
              "to stderr");
  // the console stays usable
  expectError(console, "raise SystemExit(3)", "SystemExit");
  expectError(console, "input()", "stdin");
  expect(console, "x", "3\n");
}

void testFunctions(Console &console) {
  // defined before the start of the interpreter (see main())
  expect(console, "opentoonz.echo(1, 'a', [2.5, None], {'k': True})",
         "[1, 'a', [2.5, None], {'k': True}]\n");
  expectError(console, "opentoonz.fail()", "ValueError: failed");

  // replaced after the start
  PythonEngine::instance()->defineFunction(
      "echo", [](const QVariantList &args) { return QVariant(args.size()); });
  expect(console, "opentoonz.echo(1, 2)", "2\n");
}

void testScene(Console &console) {
  run(console, "s = opentoonz.Scene()");
  expect(console, "print(s, s.frameCount, s.columnCount)",
         "Scene (0 frames) 0 0\n");
  run(console, "lvl = s.newLevel('Vector', 'A')");
  expect(console, "print(lvl, lvl.type, lvl.name, lvl.frameCount)",
         "Vector level (A, 0 frames) Vector A 0\n");
  expect(console, "s.setCell(0, 0, lvl, 1) is s", "True\n");
  run(console, "_ = s.setCell(1, 0, 'A', '2a')");
  run(console, "_ = s.setCell(2, 0, {'level': lvl, 'fid': 3})");
  expect(console, "print(s.frameCount, s.columnCount)", "3 1\n");
  expect(console, "c = s.getCell(1, 0); print(c['level'].name, c['fid'])",
         "A 0002a\n");
  expect(console, "s.getCell(0, 0)['fid']", "1\n");
  run(console, "_ = s.setCell(2, 0, None)");
  expect(console, "print(s.getCell(2, 0), s.frameCount)", "None 2\n");
  expect(console, "[l.name for l in s.getLevels()]", "['A']\n");
  expect(console, "s.getLevel('B')", "");
  expectError(console, "s.setCell(0, 0, 'B', 1)",
              "Level 'B' is not included in the scene");
  expectError(console, "s.setCell(0, 0, lvl, 'x')",
              "does not look like a FrameId");
  expectError(console, "s.newLevel('Vector', 'A')", "is already used");
  expectError(console, "s.newLevel('Bitmap', 'B')", "Bad level type");
  expectError(console, "s.load('no/such/scene.tnz')", "FileNotFoundError");
  expectError(console, "opentoonz.Level(42)",
              "Argument doesn't look like a file path");
}

void testStageObjects(Console &console) {
  run(console, "s = opentoonz.Scene()");
  expect(console, "[o.id for o in s.getStageObjects()]",
         "['Camera1', 'Table']\n");
  run(console, "_ = s.insertColumn(0).insertColumn(0)");
  expect(console, "[o.id for o in s.getStageObjects()]",
         "['Camera1', 'Table', 'Col1', 'Col2']\n");
  run(console, "col = s.getStageObject('Col1')");
  expect(console, "print(col, col.id, col.name, col.type, col.parent)",
         "StageObject(Col1) Col1 Col1 Column Table\n");
  expect(console, "col == s.getStageObject('Col1')", "True\n");

  // channels: default values, keyframes, interpolation (linear by default)
  expect(console,
         "[col.getValue('x', 10), col.getValue('scale', 3), "
         "len(col.getKeyframes('x'))]",
         "[0.0, 1.0, 0]\n");
  expect(console, "col.setKeyframe('x', 0, 2).setKeyframe('x', 10, 12) is col",
         "True\n");
  expect(
      console,
      "[col.getValue('x', 5), col.getValue('x', 2.5), col.getValue('x', 20)]",
      "[7.0, 4.5, 12.0]\n");
  expect(console,
         "[col.getKeyframes('x'), col.isKeyframe('x', 10), "
         "col.isKeyframe('x', 5), col.isKeyframe('y', 10)]",
         "[[0, 10], True, False, False]\n");
  run(console, "_ = col.setValue('Angle', 4.0, 45)");
  expect(console,
         "[col.getValue('angle', 4), col.getValue('angle', 0), "
         "col.getKeyframes('ANGLE')]",
         "[45.0, 45.0, [4]]\n");
  run(console,
      "_ = col.setKeyframe('y', 0, 0).setKeyframe('y', 20, 20)"
      ".setKeyframe('y', 10, 0)");
  expect(console, "[col.getValue('y', 15), col.getKeyframes('y')]",
         "[10.0, [0, 10, 20]]\n");
  run(console, "_ = col.removeKeyframe('x', 10).removeKeyframe('x', 3)");
  expect(console, "[col.getKeyframes('x'), col.getValue('x', 5)]",
         "[[0], 2.0]\n");

  // names and parents
  run(console, "col.name = 'BG'");
  expect(console, "print(col.name, col.id, col, s.getStageObject('BG').id)",
         "BG Col1 StageObject(Col1: BG) Col1\n");
  run(console, "col.name = ''");
  expect(console, "col.name", "'Col1'\n");
  run(console, "peg = s.newPegbar()");
  expect(console, "print(peg, peg.type, peg.parent)",
         "StageObject(Peg1) Pegbar Table\n");
  run(console, "col.parent = 'Peg1'");
  expect(console, "col.parent", "'Peg1'\n");
  run(console, "col.parent = ''");
  expect(console, "col.parent", "'Table'\n");

  // center
  expect(console, "col.setCenter(0, 1, 2).getCenter(0)", "(1.0, 2.0)\n");

  // errors
  expectError(console, "s.getStageObject('Col3')",
              "LookupError: Stage object not found: Col3");
  expectError(console, "col.getValue('rotation', 0)",
              "ValueError: Bad channel name (rotation): must be one of x, y, "
              "z, so, angle, scalex, scaley, scale, shearx, sheary, path, rotx, "
              "roty");
  expectError(console, "col.getKeyframes(3)", "Bad channel name (3)");
  expectError(console, "col.getValue('x', 'a')",
              "TypeError: Bad frame ('a'): expected a number");
  expectError(console, "col.setKeyframe('x', 1.5, 3)",
              "ValueError: Bad frame (1.5): expected an integer >= 0");
  expectError(console, "col.removeKeyframe('x', -1)",
              "Bad frame (-1): expected an integer >= 0");
  expectError(console, "col.setKeyframe('x', True, 3)",
              "Bad frame (True): expected an integer >= 0");
  expectError(console, "col.setKeyframe('x', 1, float('nan'))",
              "Bad value (nan): expected a number");
  expectError(console, "col.parent = 'Col1'", "Can't link Col1 to itself");
  expectError(console, "s.getStageObject('Table').parent = 'Peg1'",
              "The Table can't have a parent");
  expectError(console, "opentoonz.StageObject()", "TypeError");
  run(console, "col2 = s.getStageObject('Col2')");
  run(console, "_ = s.deleteColumn(1)");
  expectError(console, "col2.getValue('x', 0)",
              "The stage object Col2 is no longer in the scene");
  expect(console, "str(col2)", "'StageObject (removed)'\n");
}

void testRunFile(Console &console, const QString &dirPath) {
  QString path = QDir(dirPath).filePath("test.py");
  {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
      throw std::runtime_error("can't write " + path.toStdString());
    file.write(
        "import os, sys, opentoonz\n"
        "print(__name__, os.path.basename(__file__), "
        "os.path.basename(sys.argv[0]))\n"
        "print(opentoonz.Scene().frameCount)\n"
        "value = 7\n");
  }
  if (!console.runFile(path) ||
      console.out() != "__main__ test.py test.py\n0\n" ||
      !console.err().isEmpty())
    fail("runFile(test.py)", "__main__ test.py test.py\n0\n", console);
  // the file runs in its own namespace
  expectError(console, "value", "NameError");

  QString errorPath = QDir(dirPath).filePath("error.py");
  {
    QFile file(errorPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
      throw std::runtime_error("can't write " + errorPath.toStdString());
    file.write(
        "print('before')\n"
        "def f():\n"
        "    raise KeyError('boom')\n"
        "f()\n");
  }
  if (console.runFile(errorPath) || console.out() != "before\n" ||
      !console.err().contains("Traceback") ||
      !console.err().contains("error.py\", line 3") ||
      !console.err().contains("KeyError: 'boom'") ||
      console.err().contains("run_file"))
    fail("runFile(error.py)", "a KeyError traceback", console);

  QString exitPath = QDir(dirPath).filePath("exit.py");
  {
    QFile file(exitPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
      throw std::runtime_error("can't write " + exitPath.toStdString());
    file.write("import sys\nsys.exit(0)\nprint('not printed')\n");
  }
  if (!console.runFile(exitPath) || !console.out().isEmpty() ||
      !console.err().isEmpty())
    fail("runFile(exit.py)", "nothing", console);

  if (console.runFile(QDir(dirPath).filePath("missing.py")) ||
      !console.err().contains("FileNotFoundError"))
    fail("runFile(missing.py)", "a FileNotFoundError", console);
}

}  // namespace

int main(int argc, char *argv[]) {
  QCoreApplication app(argc, argv);

  try {
    QTemporaryDir dir;
    if (!dir.isValid()) throw std::runtime_error("no temporary directory");

    // The scenes need a stuff folder (for the sandbox project and the
    // preferences): use an empty one, so that the defaults are used
    QString stuffPath = QDir(dir.path()).filePath("stuff");
    QDir().mkpath(QDir(stuffPath).filePath("profiles/layouts/rooms/Default"));
    TEnv::setStuffDir(TFilePath(stuffPath));
    Preferences::instance();

    PythonEngine *engine = PythonEngine::instance();
    Console console(engine);

    // defined before the interpreter starts
    engine->defineFunction(
        "echo", [](const QVariantList &args) { return QVariant(args); });
    engine->defineFunction("fail", [](const QVariantList &) -> QVariant {
      throw std::invalid_argument("failed");
    });
    if (engine->isStarted()) throw std::runtime_error("started too early");
    if (!engine->start()) {
      std::cerr << console.err().toStdString();
      throw std::runtime_error("Python could not be started");
    }
    std::cout << "Python " << engine->getVersion().toStdString() << std::endl;

    testConsole(console);
    testFunctions(console);
    testScene(console);
    testStageObjects(console);
    testRunFile(console, dir.path());
  } catch (const std::exception &e) {
    std::cerr << "FAIL: " << e.what() << std::endl;
    return 1;
  }

  if (failures) {
    std::cerr << failures << " failure(s)" << std::endl;
    return 1;
  }
  std::cout << "pythonengine_test passed" << std::endl;
  return 0;
}
