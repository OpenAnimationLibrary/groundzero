// SPDX-License-Identifier: BSD-3-Clause

// Regression test of the script engine and of the script bindings that don't
// need files (scenes are built in memory, with a temporary stuff folder): it
// checks what the Script Console would print for a set of commands.

#include "toonz/scriptengine.h"
#include "toonz/preferences.h"
#include "tenv.h"
#include "tfilepath.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJSValue>
#include <QMutex>
#include <QMutexLocker>
#include <QTemporaryDir>
#include <QThread>

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

struct Output {
  int m_type;
  QString m_text;
};

class Console {
  ScriptEngine &m_engine;
  QMutex m_mutex;
  QList<Output> m_outputs;

public:
  explicit Console(ScriptEngine &engine) : m_engine(engine) {
    // the output is emitted by the script thread
    QObject::connect(
        &m_engine, &ScriptEngine::output, &m_engine,
        [this](int type, const QString &text) {
          QMutexLocker locker(&m_mutex);
          m_outputs.append(Output{type, text});
        },
        Qt::DirectConnection);
  }

  void start(const QString &cmd) {
    {
      QMutexLocker locker(&m_mutex);
      m_outputs.clear();
    }
    m_engine.evaluate(cmd);
  }

  QList<Output> finish(const QString &cmd) {
    if (!m_engine.wait(60000))
      throw std::runtime_error("timeout evaluating: " + cmd.toStdString());
    QMutexLocker locker(&m_mutex);
    return m_outputs;
  }

  QList<Output> evaluate(const QString &cmd) {
    start(cmd);
    return finish(cmd);
  }
};

std::string typeName(int type) {
  switch (type) {
  case ScriptEngine::SimpleText:
    return "SimpleText";
  case ScriptEngine::Warning:
    return "Warning";
  case ScriptEngine::ExecutionError:
    return "ExecutionError";
  case ScriptEngine::SyntaxError:
    return "SyntaxError";
  case ScriptEngine::EvaluationResult:
    return "EvaluationResult";
  case ScriptEngine::UndefinedEvaluationResult:
    return "UndefinedEvaluationResult";
  default:
    return "?";
  }
}

std::string describe(const QList<Output> &outputs) {
  std::string s;
  for (const Output &output : outputs)
    s +=
        "\n    " + typeName(output.m_type) + ": " + output.m_text.toStdString();
  return outputs.isEmpty() ? std::string("\n    (no output)") : s;
}

int failures = 0;

void fail(const QString &cmd, const std::string &expected,
          const QList<Output> &outputs) {
  ++failures;
  std::cerr << "FAIL: " << cmd.toStdString() << "\n  expected: " << expected
            << "\n  got:" << describe(outputs) << std::endl;
}

// cmd must print exactly the given outputs
void expect(Console &console, const QString &cmd,
            const QList<Output> &expected) {
  QList<Output> outputs = console.evaluate(cmd);
  bool ok               = outputs.size() == expected.size();
  for (int i = 0; ok && i < outputs.size(); ++i)
    ok = outputs[i].m_type == expected[i].m_type &&
         outputs[i].m_text == expected[i].m_text;
  if (!ok) fail(cmd, describe(expected), outputs);
}

void expect(Console &console, const QString &cmd, int type,
            const QString &text) {
  expect(console, cmd, QList<Output>() << Output{type, text});
}

// cmd must print just an error containing text
void expectError(Console &console, const QString &cmd, const QString &text) {
  QList<Output> outputs = console.evaluate(cmd);
  if (outputs.size() != 1 || outputs[0].m_type != ScriptEngine::SyntaxError ||
      !outputs[0].m_text.contains(text))
    fail(cmd, "an error containing \"" + text.toStdString() + "\"", outputs);
}

void testOutput(Console &console) {
  const int text   = ScriptEngine::SimpleText;
  const int result = ScriptEngine::EvaluationResult;

  expect(console, "1 + 1", result, "2");
  expect(console, "'abc'", result, "\"abc\"");
  expect(console, "[1, 'a', true]", result, "[1,\"a\",true]");
  expect(console, "var x = 3", ScriptEngine::UndefinedEvaluationResult,
         "undefined");
  expect(console, "x * 2", result, "6");

  // print() returns the void object: nothing else is printed
  expect(console, "print('hello', 1, true, [2, 'b'])", text,
         "hello 1 true [2,b]");
  expect(console, "warning('careful')", ScriptEngine::Warning, "careful");
  expect(console, "print(void 0); 5",
         QList<Output>() << Output{text, "undefined"} << Output{result, "5"});
  expect(console, "typeof this['void']", result, "\"object\"");
  expect(console, "this['void']", QList<Output>());

  expectError(console, "undefinedFunction()", "ReferenceError");
  expectError(console, "1 +", "SyntaxError");
  expectError(console, "throw new Error('my error')", "Error: my error");

  expect(console, "ToonzVersion", result, "\"7.1\"");
}

void testBindings(Console &console) {
  const int text   = ScriptEngine::SimpleText;
  const int result = ScriptEngine::EvaluationResult;

  // constructors, toString() and string conversion
  expect(console, "new Image()", result, "Empty image");
  expect(console, "Image()", result, "Empty image");
  expect(console, "print(new Image())", text, "Empty image");
  expect(console, "'<' + new Transform() + '>'", result, "\"<Identity>\"");
  expect(console, "String(new OutlineVectorizer())", result,
         "\"Outline Vectorizer\"");
  expect(console, "new Image() instanceof Image", result, "true");
  expect(console, "new Image() instanceof Transform", result, "false");
  expect(console, "new Rasterizer()", result, "Rasterizer");
  expect(console, "new CenterlineVectorizer()", result,
         "Centerline Vectorizer");
  expect(console, "new ToonzRasterConverter()", result, "ToonzRasterConverter");
  expect(console, "new ImageBuilder()", result, "ImageBuilder(Empty image)");

  // methods returning objects, overloads
  expect(console, "new Transform().translate(1, 2)", result,
         "Translation(1,2)");
  expect(console, "new Transform().scale(2)", result, "Scale(200%)");
  expect(console, "new Transform().scale(2, 3)", result, "Scale(200%, 300%)");

  // properties
  expect(console, "var v = new OutlineVectorizer(); v.accuracy = 7; v.accuracy",
         result, "7");
  expect(console, "v.preservePaintedAreas = false; v.preservePaintedAreas",
         result, "false");
  expect(console, "v.transparentColor = '#ff0000'; v.transparentColor", result,
         "\"#ff0000\"");
  expectError(console, "v.transparentColor = 'notacolor'",
              "Invalid color: notacolor");
  expect(console, "typeof new Image().id", result, "\"number\"");

  // images
  expect(console,
         "var b = new ImageBuilder(4, 3, 'Raster'); b.fill('red'); "
         "[b.image.type, b.image.width, b.image.height]",
         result, "[\"Raster\",4,3]");
  // chained calls return the same script object, with its script properties
  expect(console, "b.fill('blue') === b", result, "true");
  expect(console, "b.tag = 'mine'; b.fill('green').tag", result, "\"mine\"");
  expect(console, "b.image", result, "Raster image ( 4 x 3 )");
  expect(console, "new ImageBuilder().add(b.image).add(b.image).image.width",
         result, "4");
  expect(console, "new ImageBuilder().add(b.image, new Transform())", result,
         "ImageBuilder(Raster image)");

  // file paths
  expect(console, "new FilePath('/tmp/a.png').extension", result, "\"png\"");
  expect(console, "new FilePath('/tmp/a.png').withExtension('tif')", result,
         "\"/tmp/a.tif\"");
  expect(console, "new FilePath('/tmp/a.png').name", result, "\"a\"");
  expectError(console, "new FilePath('/tmp').concat('/abs')",
              "can't concatenate an absolute path");

  // argument checks
  expectError(console, "new Image(1, 2)",
              "Expected 0-1 argument(s) in the Image constructor, got 2");
  expectError(console, "new Image(42)",
              "Argument doesn't look like a file path : 42");
  expectError(console, "new Image('/nonexistent/image.png')",
              "File /nonexistent/image.png doesn't exist");
  expectError(console, "new ImageBuilder(10)",
              "Bad argument count. expected: width,height[,type]");
  expectError(console, "new ImageBuilder('a', 'b')",
              "Bad arguments: expected width,height[,type]");
  expectError(console, "new ImageBuilder(4, 4, 'Vector')",
              "Bad argument (Vector): should be 'Raster' or ToonzRaster'");
  expectError(console, "new ImageBuilder().fill('notacolor')",
              "notacolor is not a valid color");
  expectError(console, "new ImageBuilder().add(42)",
              "Bad argument (42): should be an Image (not empty)");
  expectError(console, "new ImageBuilder().add(b.image, 42)",
              "Bad argument (42): should be a Transformation");
  expectError(console, "new OutlineVectorizer().vectorize(new Transform())",
              "Bad argument (Identity): should be an Image or a Level");
  expectError(console, "new ToonzRasterConverter().convert()",
              "Expected one argument (a raster Level or a raster Image)");
  expectError(console, "ToonzRasterConverter.convert(new Image())",
              "Can't convert a Empty image");
  expectError(console, "new Image().save('/tmp/x.png')",
              "Can't save an empty image");

  // the errors can be caught by the scripts
  expect(console,
         "try { new Image().save('x.png') } catch (e) { print('caught', "
         "e.message) }",
         text, "caught Can't save an empty image");
}

void testStageObjects(Console &console) {
  const int result = ScriptEngine::EvaluationResult;

  // a new scene has the camera and the table; frames are xsheet rows from 0
  expect(console, "var s = new Scene(); s.getStageObjects()", result,
         "[StageObject(Camera1),StageObject(Table)]");
  expect(console,
         "s.insertColumn(0); s.insertColumn(0); "
         "s.getStageObjects().map(function (o) { return o.id; })",
         result, "[\"Camera1\",\"Table\",\"Col1\",\"Col2\"]");
  expect(console, "var col = s.getStageObject('Col1'); col", result,
         "StageObject(Col1)");
  expect(console,
         "[col.id, col.name, col.type, col.parent, col instanceof StageObject]",
         result, "[\"Col1\",\"Col1\",\"Column\",\"Table\",true]");

  // channels: default values, keyframes, interpolation (linear by default)
  expect(console,
         "[col.getValue('x', 10), col.getValue('scale', 3), "
         "col.getKeyframes('x').length]",
         result, "[0,1,0]");
  expect(console, "col.setKeyframe('x', 0, 2).setKeyframe('x', 10, 12)", result,
         "StageObject(Col1)");
  expect(
      console,
      "[col.getValue('x', 5), col.getValue('x', 2.5), col.getValue('x', 20)]",
      result, "[7,4.5,12]");
  expect(console,
         "[col.getKeyframes('x'), col.isKeyframe('x', 10), "
         "col.isKeyframe('x', 5), col.isKeyframe('y', 10)]",
         result, "[[0,10],true,false,false]");
  expect(console,
         "col.setValue('Angle', 4, 45); [col.getValue('angle', 4), "
         "col.getValue('angle', 0), col.getKeyframes('ANGLE')]",
         result, "[45,45,[4]]");
  // a key added inside a segment keeps the segment interpolation
  expect(console,
         "col.setKeyframe('y', 0, 0).setKeyframe('y', 20, 20)"
         ".setKeyframe('y', 10, 0); [col.getValue('y', 15), "
         "col.getKeyframes('y')]",
         result, "[10,[0,10,20]]");
  // removing a missing key does nothing
  expect(console,
         "col.removeKeyframe('x', 10).removeKeyframe('x', 3); "
         "[col.getKeyframes('x'), col.getValue('x', 5)]",
         result, "[[0],2]");

  // names
  expect(console,
         "col.name = 'BG'; [col.name, col.id, String(col), "
         "s.getStageObject('BG').id]",
         result, "[\"BG\",\"Col1\",\"StageObject(Col1: BG)\",\"Col1\"]");
  expect(console, "col.name = ''; col.name", result, "\"Col1\"");

  // pegbars and parents
  expect(console,
         "var peg = s.newPegbar(); [String(peg), peg.type, peg.parent]", result,
         "[\"StageObject(Peg1)\",\"Pegbar\",\"Table\"]");
  expect(console,
         "col.parent = 'Peg1'; peg.name = 'Rig'; "
         "[col.parent, s.getStageObject('Rig').id]",
         result, "[\"Rig\",\"Peg1\"]");
  expect(console,
         "s.getStageObject('Col2').parent = 'Col1'; "
         "s.getStageObject('Col2').parent",
         result, "\"Col1\"");
  expect(
      console,
      "[s.getStageObject('Camera1').type, s.getStageObject('Camera1').parent, "
      "s.getStageObject('Table').parent]",
      result, "[\"Camera\",\"\",\"\"]");
  expect(console, "col.parent = ''; col.parent", result, "\"Table\"");
  expect(console, "s.getStageObjects().map(function (o) { return o.name; })",
         result, "[\"Camera1\",\"Table\",\"Rig\",\"Col1\",\"Col2\"]");

  // center
  expect(console,
         "col.setCenter(0, 1, 2); var c = col.getCenter(0); [c.x, c.y]", result,
         "[1,2]");

  // errors
  expectError(console, "s.getStageObject('Col3')",
              "Stage object not found: Col3");
  expectError(console, "s.getStageObject('Peg2')",
              "Stage object not found: Peg2");
  expectError(console, "s.getStageObject('Nothing')",
              "Stage object not found: Nothing");
  expectError(console, "col.getValue('rotation', 0)",
              "Bad channel name (rotation): must be one of x, y, z, so, angle, "
              "scalex, scaley, scale, shearx, sheary, path, rotx, roty");
  expectError(console, "col.getKeyframes(3)", "Bad channel name (3)");
  expectError(console, "col.getValue('x', 'a')",
              "Bad frame (a): expected a number");
  expectError(console, "col.setKeyframe('x', 1.5, 3)",
              "Bad frame (1.5): expected an integer >= 0");
  expectError(console, "col.removeKeyframe('x', -1)",
              "Bad frame (-1): expected an integer >= 0");
  expectError(console, "col.setKeyframe('x', 1, 'abc')",
              "Bad value (abc): expected a number");
  expectError(console, "col.setCenter(0, 'a', 1)",
              "Bad x (a): expected a number");
  expectError(console, "col.parent = 'Nope'", "Stage object not found: Nope");
  expectError(console, "col.parent = 'Col1'", "Can't link Col1 to itself");
  expectError(console, "col.parent = 'Peg1'; peg.parent = 'Col1'",
              "Can't link Peg1 to Col1: Col1 is linked to Peg1");
  expectError(console, "s.getStageObject('Table').parent = 'Peg1'",
              "The Table can't have a parent");
  expectError(console, "new StageObject()",
              "StageObject can't be created directly");
  // the object of a deleted column
  expectError(console,
              "var col2 = s.getStageObject('Col2'); s.deleteColumn(1); "
              "col2.getValue('x', 0)",
              "The stage object Col2 is no longer in the scene");
  expect(console, "String(col2)", result, "\"StageObject (removed)\"");
}

void testRun(Console &console, const QString &dirPath) {
  const int result = ScriptEngine::EvaluationResult;

  QString path = QDir(dirPath).filePath("test.js");
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly))
    throw std::runtime_error("can't write " + path.toStdString());
  file.write("var fromFile = 6 * 7;\nfromFile\n");
  file.close();

  QString badPath = QDir(dirPath).filePath("bad.js");
  QFile badFile(badPath);
  if (!badFile.open(QIODevice::WriteOnly))
    throw std::runtime_error("can't write " + badPath.toStdString());
  badFile.write("var a = 1;\n\nundefinedFunction();\n");
  badFile.close();

  auto quoted = [](QString s) {
    return "\"" + s.replace("\\", "\\\\").replace("\"", "\\\"") + "\"";
  };

  expect(console, "run(" + quoted(path) + ")", result, "42");
  // the variables of the script remain defined
  expect(console, "fromFile + 1", result, "43");
  expect(console, "run(new FilePath(" + quoted(path) + "))", result, "42");
  expectError(console, "run(" + quoted(badPath) + ")",
              "at line 3 of " + badPath);
  expectError(console, "run()", "expected one parameter");
  expectError(console, "run(1, 2)", "expected one parameter");
  expectError(console, "run(42)", "Argument doesn't look like a file path");
  QString missing = QDir(dirPath).filePath("missing.js");
  expectError(console, "run(" + quoted(missing) + ")",
              "can't read file " + missing);
}

void testInterrupt(ScriptEngine &engine, Console &console) {
  const QString cmd = "var i = 0; while (true) { i++; }";
  console.start(cmd);
  QThread::msleep(300);
  if (!engine.isEvaluating()) {
    fail(cmd, "still evaluating", QList<Output>());
    return;
  }
  engine.interrupt();
  QList<Output> outputs = console.finish(cmd);
  if (!outputs.isEmpty()) fail(cmd, "(no output)", outputs);

  // the next evaluations are not interrupted
  expect(console, "1 + 2", ScriptEngine::EvaluationResult, "3");
}

void testDefineFunction(ScriptEngine &engine, Console &console) {
  engine.defineFunction(
      "twice", [](QJSEngine *, const QList<QJSValue> &args) -> QJSValue {
        return QJSValue(args.size() * 2);
      });
  expect(console, "twice(1, 2, 3)", ScriptEngine::EvaluationResult, "6");
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
    // created on the main thread, as in the application
    Preferences::instance();

    ScriptEngine engine;
    Console console(engine);

    testOutput(console);
    testBindings(console);
    testStageObjects(console);
    testRun(console, dir.path());
    testInterrupt(engine, console);
    testDefineFunction(engine, console);
  } catch (const std::exception &e) {
    std::cerr << "FAIL: " << e.what() << std::endl;
    return 1;
  }

  if (failures) {
    std::cerr << failures << " failure(s)" << std::endl;
    return 1;
  }
  std::cout << "scriptengine_test passed" << std::endl;
  return 0;
}
