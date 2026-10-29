#include "texpression.h"
#include "tgrammar.h"
#include "tseexpr.h"
#include "tdoubleparam.h"
#include "tdoublekeyframe.h"
#include "tparamchange.h"
#include "tstream.h"
#include "tunit.h"
#include "tundo.h"
#include "texception.h"
#include "toonz/txsheet.h"
#include "toonz/txsheetexpr.h"
#include "toonz/tstageobject.h"
#include "toonz/tstageobjecttree.h"
#include "toonz/doubleparamcmd.h"

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}
void expectNear(double actual, double expected, const char *message) {
  require(std::abs(actual - expected) < 1e-8, message);
}

void dialectAndTime() {
  TSyntax::Grammar grammar;
  double fps = 24.0;
  grammar.setFrameRateProvider([&]() { return fps; });
  TExpression expression;
  expression.setGrammar(&grammar);
  expression.setText("sin(90)");
  double value = 0.0;
  require(expression.evaluate(0.0, 1.0, 1.0, nullptr, value), "native trig did not evaluate");
  expectNear(value, 1.0, "native degree trig changed");
  expression.setText(TExpression::makeSeExprText("sin($pi/2)"));
  require(expression.evaluate(0.0, 1.0, 1.0, nullptr, value), "SeExpr radian trig did not evaluate");
  expectNear(value, 1.0, "SeExpr radian trig is incorrect");
  expression.setText(TExpression::makeSeExprText("$frame + $rframe + $t + $time"));
  require(expression.evaluate(.5, 25.0, 3.0, nullptr, value), "animation context did not evaluate");
  expectNear(value, 29.5, "frame, relative frame or seconds convention is incorrect");
  fps = 48.0;
  require(expression.evaluate(.5, 25.0, 3.0, nullptr, value), "fps change did not evaluate");
  expectNear(value, 29.0, "seconds did not follow the live fps provider");

  expression.setText(TExpression::makeSeExprText("1/($frame-2)"));
  require(expression.evaluate(0.0, 1.0, 1.0, nullptr, value), "valid formula did not evaluate");
  expectNear(value, -1.0, "unexpected initial formula value");
  const auto failures = TSeExpr::failureCount();
  require(!expression.evaluate(0.0, 2.0, 2.0, nullptr, value), "nonfinite animation result was accepted");
  expectNear(value, -1.0, "failed animation did not retain its last valid preview");
  require(!expression.getError().empty() && TSeExpr::failureCount() > failures,
          "failed evaluation did not retain diagnostics and report a failure");
  require(expression.evaluate(0.0, 3.0, 3.0, nullptr, value), "formula did not recover after valid inputs");
  expectNear(value, 1.0, "recovered animation value is incorrect");
  require(expression.getError().empty(), "recovered formula retained a stale runtime error");

  expression.setText(TExpression::makeSeExprText("channel(sprintf(\"%s\", \"col1.x\"))"));
  require(!expression.isValid(), "dynamic channel reference escaped static dependency binding");
  expression.setText(TExpression::makeSeExprText("noise("));
  require(!expression.isValid() && !expression.getError().empty(), "invalid saved formula lacked an error");
  require(TExpression::seExprSource(expression.getText()) == "noise(", "invalid source was lost");
}

TDoubleKeyframe key(double frame, double value, TDoubleKeyframe::Type type,
                    const std::string &text = {}) {
  TDoubleKeyframe result(frame, value);
  result.m_prevType = TDoubleKeyframe::None;
  result.m_type = type;
  result.m_expressionText = text;
  return result;
}

void copiedExpressionOwners() {
  TSyntax::Grammar grammar;
  for (const std::string source : {
           std::string("cycle(2)+1"),
           TExpression::makeSeExprText("channel(\"cycle(2)\")+1")}) {
    TDoubleParamP constructed, assigned(new TDoubleParam), copied(new TDoubleParam);
    {
      TDoubleParamP original(new TDoubleParam);
      original->setGrammar(&grammar);
      original->setKeyframe(key(0, 2, TDoubleKeyframe::Linear));
      original->setKeyframe(key(1, 0, TDoubleKeyframe::Expression, source));
      original->setKeyframe(key(4, 0, TDoubleKeyframe::Linear));
      expectNear(original->getValue(2), 3, "cycle owner fixture did not evaluate");
      constructed = new TDoubleParam(*original);
      *assigned = *original;
      copied->copy(original.getPointer());
      // Distinct starting values make stale owners observable even before the
      // original is destroyed, rather than relying only on a dangling pointer.
      constructed->setKeyframe(0, key(0, 20, TDoubleKeyframe::Linear));
      assigned->setKeyframe(0, key(0, 30, TDoubleKeyframe::Linear));
      copied->setKeyframe(0, key(0, 40, TDoubleKeyframe::Linear));
      expectNear(constructed->getValue(2), 21, "copy constructor retained the source expression owner");
      expectNear(assigned->getValue(2), 31, "assignment retained the source expression owner");
      expectNear(copied->getValue(2), 41, "parameter copy retained the source expression owner");
    }
    expectNear(constructed->getValue(2), 21, "copied expression owner expired with its source");
    expectNear(assigned->getValue(2), 31, "assigned expression owner expired with its source");
    expectNear(copied->getValue(2), 41, "parameter expression owner expired with its source");
  }
}

struct Observer : TParamObserver {
  int propagated = 0;
  void onChange(const TParamChange &change) override {
    if (!change.m_keyframeChanged) ++propagated;
  }
};

void nativeReferencesAndCycles() {
  TXsheet xsheet;
  TStageObject *first = xsheet.getStageObject(TStageObjectId::ColumnId(0));
  TStageObject *second = xsheet.getStageObject(TStageObjectId::ColumnId(1));
  TDoubleParam *input = first->getParam(TStageObject::T_X);
  TDoubleParam *other = second->getParam(TStageObject::T_X);
  input->setMeasureName("");
  other->setMeasureName("");
  input->setKeyframe(key(0, 3.0, TDoubleKeyframe::Linear));
  input->setKeyframe(key(10, 13.0, TDoubleKeyframe::Linear));

  TDoubleParamP output(new TDoubleParam);
  output->setGrammar(xsheet.getStageObjectTree()->getGrammar());
  const std::string source = TExpression::makeSeExprText(
      "channel(\"col1.x\", $frame-1) * 2 + channel(\"col1.x\")");
  output->setKeyframe(key(0, 0, TDoubleKeyframe::Expression, source));
  output->setKeyframe(key(10, 0, TDoubleKeyframe::Linear));
  expectNear(output->getValue(5), input->getValue(4) * 2 + input->getValue(5),
       "SeExpr channel sampling did not use the native frame convention");
  Observer observer;
  output->addObserver(&observer);
  input->setValue(5, 17.0);
  require(observer.propagated > 0, "native reference changes did not notify the SeExpr owner");
  expectNear(output->getValue(5), input->getValue(4) * 2 + input->getValue(5),
       "native reference update retained a stale value");
  output->removeObserver(&observer);

  TExpression dependency;
  dependency.setGrammar(output->getGrammar());
  dependency.setText(source);
  QSet<int> columns;
  QSet<TDoubleParam *> parameters;
  referenceParams(dependency, columns, parameters);
  require(columns.contains(0) && parameters.contains(input),
          "native reference visitor did not enumerate SeExpr dependencies");
  require(dependsOn(dependency, input), "direct SeExpr cycle was not detected");

  // Native first -> second; the proposed SeExpr second -> first must be rejected.
  input->setKeyframe(key(0, 0, TDoubleKeyframe::Expression, "col2.x"));
  dependency.setOwnerParameter(other);
  dependency.setText(TExpression::makeSeExprText("channel(\"col1.x\")"));
  require(dependsOn(dependency, other), "mixed native/SeExpr cycle was not detected");
}

void cyclicDependencySearch() {
  TXsheet xsheet;
  TStageObject *first = xsheet.getStageObject(TStageObjectId::ColumnId(0));
  TStageObject *second = xsheet.getStageObject(TStageObjectId::ColumnId(1));
  TStageObject *third = xsheet.getStageObject(TStageObjectId::ColumnId(2));
  TStageObject *fourth = xsheet.getStageObject(TStageObjectId::ColumnId(3));
  TDoubleParam *a = first->getParam(TStageObject::T_X);
  TDoubleParam *b = second->getParam(TStageObject::T_X);
  TDoubleParam *target = third->getParam(TStageObject::T_X);
  TDoubleParam *weightTarget = fourth->getParam(TStageObject::T_X);
  TDoubleParamP removed(new TDoubleParam);
  auto setExpression = [](TDoubleParam *curve, const std::string &source) {
    curve->setKeyframe(key(0, 0, TDoubleKeyframe::Expression, source));
    curve->setKeyframe(key(10, 0, TDoubleKeyframe::Linear));
  };
  TExpression dependency;
  dependency.setGrammar(xsheet.getStageObjectTree()->getGrammar());
  dependency.setText("col1.x*3");

  // Prepend undo can shift a curve onto its old reference before the manager
  // rewrites its text. An ignored, removed curve can query that temporary
  // self-cycle first, depending on the order of the manager's pointer set.
  setExpression(a, TExpression::makeSeExprText(
      "channel(\"col1.x\") + channel(\"col3.x\")"));
  require(!dependsOn(dependency, removed.getPointer()),
          "unrelated dependency query did not terminate through a self-cycle");
  require(dependsOn(dependency, target),
          "dependency after a self-cycle was not found");
  require(dependsOn(a, a), "actual self-reference was not found");
  require(!dependsOn(target, target),
          "a constant curve was treated as its own dependency");
  a->clearKeyframes();

  // Keep searching after revisiting a node in a mixed native/SeExpr cycle.
  setExpression(a, "col2.x");
  setExpression(b, TExpression::makeSeExprText(
      "channel(\"col1.x\") + channel(\"col3.x\")"));
  dependency.setText("col1.x");
  require(!dependsOn(dependency, removed.getPointer()),
          "unrelated dependency query did not terminate through a mutual cycle");
  require(dependsOn(dependency, target),
          "dependency after a mutual cycle was not found");
  require(dependsOn(a, b) && dependsOn(b, a),
          "mixed native/SeExpr cycle lost a reachable dependency");
  // Break the stored calculator links before editing or destroying this graph.
  a->clearKeyframes();
  b->clearKeyframes();
  dependency.setText("0");

  // Two stage objects can share a base curve while retaining different
  // effective offset layers. Visiting the shared base through the first
  // reference must still search the second reference's weight and channel.
  std::unique_ptr<TStageObjectParams> shared(second->getParams());
  shared->m_x = a;
  second->assignParams(shared.get(), false);
  require(second->getParam(TStageObject::T_X) == a,
          "shared-base dependency fixture did not retain its curve");
  second->insertOffsetLayer(0, second->createOffsetLayer(L"Dependency"));
  TDoubleParam *offset = second->getOffsetParam(0, TStageObject::T_X);
  TDoubleParam *weight = second->getOffsetLayer(0).m_weight.getPointer();
  setExpression(offset, "col2.x + col3.x");
  setExpression(weight, TExpression::makeSeExprText("channel(\"col4.x\")"));
  dependency.setText("col1.x + col2.x");
  require(!dependsOn(dependency, removed.getPointer()),
          "unrelated dependency query did not terminate through an offset cycle");
  require(dependsOn(dependency, target),
          "shared base curve hid another reference's offset dependency");
  require(dependsOn(dependency, weightTarget),
          "shared base curve hid another reference's weight dependency");
  offset->clearKeyframes();
  weight->clearKeyframes();
}

void rewriteReferences() {
  const std::string source = TExpression::makeSeExprText(
      "$label=\"col1.x\"; channel(\"col1.x\") + channel('col2.x', $frame) # channel(\"col1.x\")\n");
  const std::string rewritten = TExpression::rewriteSeExprReferences(source,
      [](const std::string &reference) {
        return reference == "col1.x" ? "col4.x" : reference;
      });
  require(rewritten.find("$label=\"col1.x\"") != std::string::npos,
          "reference repair changed unrelated string data");
  require(rewritten.find("channel(\"col4.x\")") != std::string::npos,
          "reference repair missed a bound native reference");
  require(rewritten.find("channel('col2.x', $frame)") != std::string::npos,
          "reference repair changed a different reference");
  require(rewritten.find("# channel(\"col1.x\")") != std::string::npos,
          "reference repair changed a comment");
}

void persistenceAndUndo() {
  TSyntax::Grammar grammar;
  TDoubleParamP curve(new TDoubleParam);
  curve->setGrammar(&grammar);
  const std::string source = TExpression::makeSeExprText("$frame + $rframe + $t");
  curve->setKeyframe(key(0, 0, TDoubleKeyframe::Expression, source));
  curve->setKeyframe(key(10, 0, TDoubleKeyframe::Linear));
  const double before = curve->getValue(4.5);
  TDoubleParamP clone(dynamic_cast<TDoubleParam *>(curve->clone()));
  expectNear(clone->getValue(4.5), before, "cloning lost SeExpr evaluation or fractional-frame semantics");

  QTemporaryDir directory;
  require(directory.isValid(), "temporary scene fixture unavailable");
  TFilePath path(directory.filePath("expression.tnz").toStdWString());
  {
    TOStream stream(path);
    require(stream.checkStatus(), "could not write expression curve");
    stream.openChild("curve");
    curve->saveData(stream);
    stream.closeChild();
  }
  QFile saved(directory.filePath("expression.tnz"));
  require(saved.open(QIODevice::ReadOnly), "could not inspect expression scene");
  const QByteArray bytes = saved.readAll();
  require(bytes.contains("<SeExpr1>"), "scene did not store an explicit versioned SeExpr tag");
  require(!bytes.contains("seexpr1:"), "scene redundantly stored the in-memory dialect marker");
  TDoubleParamP loaded(new TDoubleParam);
  loaded->setGrammar(&grammar);
  {
    TIStream stream(path);
    std::string tag;
    require(stream.matchTag(tag) && tag == "curve", "expression scene root was missing");
    loaded->loadData(stream);
    require(stream.matchEndTag(), "expression scene root was not closed");
  }
  require(loaded->getKeyframe(0).m_expressionText == source,
          "save/reopen changed SeExpr source or dialect");
  expectNear(loaded->getValue(4.5), before, "reopened SeExpr curve evaluated differently");

  TUndoManager *undo = TUndoManager::manager();
  undo->reset();
  {
    KeyframeSetter setter(loaded.getPointer(), 0);
    setter.setExpression(TExpression::makeSeExprText("42"));
  }
  expectNear(loaded->getValue(4.5), 42.0, "native expression edit did not apply");
  require(undo->undo(), "native expression edit had no undo");
  expectNear(loaded->getValue(4.5), before, "native undo did not restore source/evaluation");
  require(undo->redo(), "native expression edit had no redo");
  expectNear(loaded->getValue(4.5), 42.0, "native redo did not restore the edit");
  undo->reset();

  TExpression shared;
  shared.setGrammar(&grammar);
  shared.setText(TExpression::makeSeExprText(
      "def FLOAT square(FLOAT x) {x*x} square($frame)"));
  require(shared.isValid(), "local animation function did not compile");
  std::atomic<bool> correct{true};
  std::vector<std::thread> threads;
  for (int worker = 0; worker < 4; ++worker)
    threads.emplace_back([&shared, &correct, worker]() {
      for (int i = 1; i <= 100; ++i) {
        const double frame = i + worker * .25;
        double value = 0;
        if (!shared.evaluate(0, frame, frame, nullptr, value) ||
            std::abs(value - frame * frame) > 1e-8)
          correct = false;
      }
    });
  for (auto &thread : threads) thread.join();
  require(correct, "parallel animation evaluation shared mutable interpreter state");
}

// A host can encounter a corrupted dependency graph before the normal editing
// cycle checks run. Exercise the actual channel bridge with two concurrently
// entered curves whose native reference callbacks point at one another.
class CyclicPeerNode final : public TSyntax::CalculatorNode {
  TExpression *m_peer;
public:
  CyclicPeerNode(TSyntax::Calculator *calculator, TExpression *peer)
      : CalculatorNode(calculator), m_peer(peer) {}
  double compute(double vars[3]) const override {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    double result = 0;
    if (!m_peer->evaluate(vars[0], vars[1], vars[2], nullptr, result))
      throw TException("Cyclic native reference failed evaluation");
    return result;
  }
  void accept(TSyntax::CalculatorNodeVisitor &) override {}
};

class CyclicPeerPattern final : public TSyntax::Pattern {
  TExpression *m_peer;
public:
  explicit CyclicPeerPattern(TExpression *peer) : m_peer(peer) {}
  std::string getFirstKeyword() const override { return "peer"; }
  bool matchToken(const std::vector<TSyntax::Token> &previous,
                  const TSyntax::Token &token) const override {
    return previous.empty() && token.getText() == "peer";
  }
  bool isFinished(const std::vector<TSyntax::Token> &,
                  const TSyntax::Token &) const override { return true; }
  TSyntax::TokenType getTokenType(const std::vector<TSyntax::Token> &,
                                  const TSyntax::Token &) const override {
    return TSyntax::Variable;
  }
  void createNode(TSyntax::Calculator *calculator,
                  std::vector<TSyntax::CalculatorNode *> &stack,
                  const std::vector<TSyntax::Token> &) const override {
    stack.push_back(new CyclicPeerNode(calculator, m_peer));
  }
};

void concurrentCorruptCycle() {
  TSyntax::Grammar leftGrammar, rightGrammar;
  TExpression left, right;
  leftGrammar.addPattern(new CyclicPeerPattern(&right));
  rightGrammar.addPattern(new CyclicPeerPattern(&left));
  left.setGrammar(&leftGrammar); right.setGrammar(&rightGrammar);
  left.setText(TExpression::makeSeExprText("channel(\"peer\")"));
  right.setText(TExpression::makeSeExprText("channel(\"peer\")"));
  require(left.isValid() && right.isValid(), "cyclic host fixture did not bind its native references");
  std::atomic<bool> finished{false}, successful{true}, begin{false};
  // A lock-order regression must produce a bounded test failure, never a
  // permanently blocked join that hides the original diagnostic.
  std::thread watchdog([&] {
    for (int tick = 0; tick < 500 && !finished; ++tick)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (!finished) {
      std::cerr << "seexpr_animation: FAIL: cyclic graph workers deadlocked" << std::endl;
      std::_Exit(2);
    }
  });
  auto run = [&](TExpression &expression) {
    while (!begin) std::this_thread::yield();
    for (int iteration = 0; iteration < 10; ++iteration) {
      double result = 0;
      if (expression.evaluate(0, 1, 1, nullptr, result) || expression.getError().empty())
        successful = false;
    }
  };
  std::thread first([&] { run(left); }), second([&] { run(right); });
  begin = true;
  first.join(); second.join();
  finished = true; watchdog.join();
  require(successful, "corrupted native cycle returned a successful value or lost its diagnostic");
}
}  // namespace

int main(int argc, char **argv) {
  QCoreApplication application(argc, argv);
  try {
    dialectAndTime();
    copiedExpressionOwners();
    nativeReferencesAndCycles();
    cyclicDependencySearch();
    rewriteReferences();
    persistenceAndUndo();
    concurrentCorruptCycle();
    std::cout << "seexpr_animation: PASS\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "seexpr_animation: FAIL: " << error.what() << '\n';
  } catch (const TException &) {
    std::cerr << "seexpr_animation: FAIL: native exception\n";
  }
  return 1;
}
