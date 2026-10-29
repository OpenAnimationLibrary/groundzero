// Regression test of the pure part of Rig Mapping (toonz/rigmapping.h):
// matching the parts of two rigs by name and by position, and the rule that
// turns the keys of the source into those of the target.

#include "toonz/rigmapping.h"

#include <cmath>
#include <initializer_list>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using namespace RigMapping;

namespace {

void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

bool near(double a, double b) { return std::abs(a - b) < 1e-9; }

// The target of source s, or -1.
int targetOf(const std::vector<Pair> &pairs, int s) {
  for (const Pair &pair : pairs)
    if (pair.source == s) return pair.target;
  return -1;
}

Confidence confidenceOf(const std::vector<Pair> &pairs, int s) {
  for (const Pair &pair : pairs)
    if (pair.source == s) return pair.confidence;
  throw std::runtime_error("no pair for the source");
}

// No part is in two pairs.
bool isOneToOne(const std::vector<Pair> &pairs) {
  std::set<int> sources, targets;
  for (const Pair &pair : pairs) {
    if (!sources.insert(pair.source).second) return false;
    if (!targets.insert(pair.target).second) return false;
  }
  return true;
}

//------------------------------------------------------------------

void testNames() {
  require(normalizeName("Arm_L") == "arm l", "normalise: underscore");
  require(normalizeName("arm l") == "arm l", "normalise: space");
  require(normalizeName("  ARM-L ") == "arm l", "normalise: dash and trim");
  require(normalizeName("Arm.L") == "arm l", "normalise: dot");
  require(normalizeName("Arm__ _L") == "arm l",
          "normalise: a run of separators");
  require(normalizeName("_._") == "", "normalise: only separators");
  require(normalizeName("") == "", "normalise: empty");

  require(compactName("arm l") == "arml", "compact");

  require(stripNumberSuffix("arm l 2") == "arm l", "suffix: separated");
  require(stripNumberSuffix("leg1") == "leg", "suffix: glued");
  require(stripNumberSuffix("leg") == "leg", "suffix: none");
  require(stripNumberSuffix("12") == "12", "suffix: only a number");
  require(stripNumberSuffix("l2r") == "l2r", "suffix: number inside");

  require(trimName("  Head\t") == "Head", "trim");

  require(isDefaultName(""), "default: empty");
  require(isDefaultName("Col3"), "default: Col3");
  require(isDefaultName("col 12"), "default: col 12");
  require(isDefaultName("Peg2"), "default: Peg2");
  require(isDefaultName("Peg_2"), "default: Peg_2");
  require(isDefaultName("Camera1"), "default: Camera1");
  require(isDefaultName("Table"), "default: Table");
  require(isDefaultName("Vertex_003"), "default: Vertex_003");
  require(isDefaultName("vertex 7"), "default: vertex 7");
  require(!isDefaultName("Head"), "default: Head is not");
  require(!isDefaultName("Column"), "default: a bare Column is a name");
  require(!isDefaultName("Arm_L"), "default: Arm_L is not");
  require(!isDefaultName("Col3L"), "default: Col3L is not");
  require(!isDefaultName("Root"), "default: Root is a name");
  require(!isDefaultName("3"), "default: a number is a name");

  require(std::string(confidenceName(Exact)) == "Same name" &&
              std::string(confidenceName(Position)) == "By position",
          "confidence names");
}

//------------------------------------------------------------------

// Torso with Head, Arm_L, Arm_R, Leg_L, Leg_R
std::vector<Node> biped(const std::vector<std::string> &names) {
  std::vector<Node> nodes;
  nodes.push_back(Node(names[0], -1));
  for (std::size_t i = 1; i < names.size(); i++)
    nodes.push_back(Node(names[i], 0));
  return nodes;
}

void testExact() {
  std::vector<Node> source =
      biped({"Torso", "Head", "Arm_L", "Arm_R", "Leg_L", "Leg_R"});
  // the same parts, in another order
  std::vector<Node> target =
      biped({"Torso", "Leg_R", "Arm_R", "Head", "Leg_L", "Arm_L"});
  std::vector<Pair> pairs = match(source, target);
  require(pairs.size() == 6, "exact: every part is paired");
  require(isOneToOne(pairs), "exact: one to one");
  require(targetOf(pairs, 0) == 0, "exact: torso");
  require(targetOf(pairs, 1) == 3, "exact: head");
  require(targetOf(pairs, 2) == 5, "exact: left arm");
  require(targetOf(pairs, 3) == 2, "exact: right arm");
  require(targetOf(pairs, 4) == 4, "exact: left leg");
  require(targetOf(pairs, 5) == 1, "exact: right leg");
  for (const Pair &pair : pairs)
    require(pair.confidence == Exact, "exact: confidence");
  // sorted by source
  for (std::size_t i = 0; i < pairs.size(); i++)
    require(pairs[i].source == int(i), "exact: sorted by source");
}

void testCaseAndSeparators() {
  std::vector<Node> source = biped({"Torso", "Head", "Arm_L", "Arm_R"});
  std::vector<Node> target = biped({"torso", "HEAD", "arm l", "Arm-R"});
  std::vector<Pair> pairs  = match(source, target);
  require(pairs.size() == 4, "separators: every part is paired");
  require(targetOf(pairs, 2) == 2, "separators: Arm_L and arm l");
  require(targetOf(pairs, 3) == 3, "separators: Arm_R and Arm-R");
  require(confidenceOf(pairs, 2) == Normalized, "separators: confidence");
  require(confidenceOf(pairs, 1) == Normalized, "case: confidence");
  require(confidenceOf(pairs, 0) == Normalized, "case: root confidence");

  // an exact name wins over a normalised one
  source = biped({"R", "Arm_L", "arm l"});
  target = biped({"R", "arm l", "Arm L"});
  pairs  = match(source, target);
  require(targetOf(pairs, 2) == 1, "exact first: arm l");
  require(confidenceOf(pairs, 2) == Exact, "exact first: confidence");
  // the other one is left with the one left, by name it is the same name
  require(targetOf(pairs, 1) == 2, "exact first: the rest");
  require(confidenceOf(pairs, 1) == Normalized, "exact first: rest confidence");

  // "ArmL" and "Arm_L" are the same name
  source = biped({"R", "ArmL"});
  target = biped({"R", "Arm_L"});
  pairs  = match(source, target);
  require(targetOf(pairs, 1) == 1 && confidenceOf(pairs, 1) == Normalized,
          "separators: glued");
}

void testTrailingNumber() {
  std::vector<Node> source = biped({"Rig", "Arm_L_1", "Head2"});
  std::vector<Node> target = biped({"Rig", "arm l", "Head"});
  std::vector<Pair> pairs  = match(source, target);
  require(targetOf(pairs, 1) == 1, "number: Arm_L_1 and arm l");
  require(confidenceOf(pairs, 1) == IgnoringNumber, "number: confidence");
  require(targetOf(pairs, 2) == 2, "number: Head2 and Head");

  // the number can be ignored on both sides
  source = biped({"Rig", "Arm_1"});
  target = biped({"Rig", "Arm 2"});
  pairs  = match(source, target);
  require(targetOf(pairs, 1) == 1 && confidenceOf(pairs, 1) == IgnoringNumber,
          "number: different numbers");

  // two candidates for a name: none is chosen by the name
  source = biped({"Rig", "Leg_1", "Leg_2"});
  target = biped({"Rig", "Leg", "Foot", "Tail"});
  Options noPosition;
  noPosition.matchByPosition = false;
  pairs                      = match(source, target, noPosition);
  require(targetOf(pairs, 1) == -1 && targetOf(pairs, 2) == -1,
          "number: ambiguous source");
  source = biped({"Rig", "Leg"});
  target = biped({"Rig", "Leg_1", "Leg_2"});
  pairs  = match(source, target, noPosition);
  require(targetOf(pairs, 1) == -1, "number: ambiguous target");

  // and it can be turned off
  Options noNumbers;
  noNumbers.ignoreNumberSuffix = false;
  noNumbers.matchByPosition    = false;
  source                       = biped({"Rig", "Arm_L_1"});
  target                       = biped({"Rig", "arm l"});
  pairs                        = match(source, target, noNumbers);
  require(targetOf(pairs, 1) == -1, "number: off");
  require(targetOf(pairs, 0) == 0, "number: off, the roots still go together");

  // a name that is only a number is a name
  source = biped({"Rig", "1", "2"});
  target = biped({"Rig", "2", "1"});
  pairs  = match(source, target);
  require(targetOf(pairs, 1) == 2 && targetOf(pairs, 2) == 1,
          "number: names that are numbers");
}

void testPosition() {
  // no names: the places in the tree decide
  std::vector<Node> source = {Node("Col1", -1), Node("Col2", 0),
                              Node("Col3", 0), Node("Col4", 1)};
  std::vector<Node> target = {Node("Col7", -1), Node("Col8", 0),
                              Node("Col9", 0), Node("Col10", 1)};
  std::vector<Pair> pairs  = match(source, target);
  require(pairs.size() == 4, "position: every part is paired");
  for (int i = 0; i < 4; i++) {
    require(targetOf(pairs, i) == i, "position: same place");
    require(confidenceOf(pairs, i) == Position, "position: confidence");
  }

  // default names are not names, whatever the case and the separator
  source = {Node("Col3", -1), Node("Peg_2", 0), Node("Col5", 0)};
  target = {Node("COL 3", -1), Node("Col5", 0), Node("peg2", 0)};
  pairs  = match(source, target);
  require(confidenceOf(pairs, 0) == Position, "default: root by position");
  require(targetOf(pairs, 1) == 1 && confidenceOf(pairs, 1) == Position,
          "default: Peg_2 goes by position, not to peg2");
  require(targetOf(pairs, 2) == 2 && confidenceOf(pairs, 2) == Position,
          "default: Col5 goes by position, not to Col5");

  // without the position they stay unmatched
  Options noPosition;
  noPosition.matchByPosition = false;
  noPosition.pairSingleRoots = false;
  pairs                      = match(source, target, noPosition);
  require(pairs.empty(), "default: unmatched without the position");

  // empty names too
  source = {Node("", -1), Node("", 0)};
  target = {Node("", -1), Node("", 0)};
  require(match(source, target, noPosition).empty(),
          "default: empty names are not matched");

  // different counts: nothing says which are the extra parts
  source = {Node("Rig", -1), Node("", 0), Node("", 0)};
  target = {Node("Rig", -1), Node("", 0), Node("", 0), Node("", 0)};
  pairs  = match(source, target);
  require(pairs.size() == 1 && targetOf(pairs, 0) == 0,
          "position: not unambiguous");

  // a name is matched even if it is not at the same place
  source = {Node("Rig", -1), Node("", 0), Node("Wing", 1)};
  target = {Node("Rig", -1), Node("Wing", 0), Node("", 0)};
  pairs  = match(source, target);
  require(targetOf(pairs, 2) == 1, "position: the name wins");
  require(confidenceOf(pairs, 2) == Exact, "position: the name wins (2)");
  // the other children are 1 and 1 now
  require(targetOf(pairs, 1) == 2 && confidenceOf(pairs, 1) == Position,
          "position: what is left");

  // the deeper groups are found through the pairs of the upper ones
  source = {Node("Rig", -1), Node("A", 0), Node("", 1), Node("", 1)};
  target = {Node("Rig", -1), Node("a", 0), Node("", 1), Node("", 1)};
  pairs  = match(source, target);
  require(targetOf(pairs, 2) == 2 && targetOf(pairs, 3) == 3,
          "position: grandchildren in order");

  // the order of the siblings is the order in the list
  source = {Node("", -1), Node("", 0), Node("", 0), Node("", 0)};
  target = {Node("", -1), Node("", 0), Node("", 0), Node("", 0)};
  pairs  = match(source, target);
  require(targetOf(pairs, 1) == 1 && targetOf(pairs, 2) == 2 &&
              targetOf(pairs, 3) == 3,
          "position: sibling order");
}

void testRoots() {
  // the roots chosen go together whatever their names
  std::vector<Node> source = {Node("Bob", -1), Node("Head", 0)};
  std::vector<Node> target = {Node("Alice", -1), Node("Head", 0)};
  std::vector<Pair> pairs  = match(source, target);
  require(targetOf(pairs, 0) == 0 && confidenceOf(pairs, 0) == Position,
          "roots: paired");
  require(targetOf(pairs, 1) == 1 && confidenceOf(pairs, 1) == Exact,
          "roots: the head goes by name");

  // even if the name of a root is the one of a part of the other rig
  source = {Node("Head", -1), Node("Hand", 0)};
  target = {Node("Body", -1), Node("Head", 0)};
  pairs  = match(source, target);
  require(targetOf(pairs, 0) == 0, "roots: the roots go together");
  Options loose;
  loose.pairSingleRoots = false;
  pairs                 = match(source, target, loose);
  require(targetOf(pairs, 0) == 1, "roots: free, by name");

  // names that say the roots are the same
  source = {Node("Rig 1", -1)};
  target = {Node("rig_2", -1)};
  pairs  = match(source, target);
  require(pairs.size() == 1 && confidenceOf(pairs, 0) == IgnoringNumber,
          "roots: confidence from the names");

  // two roots in a rig: no root rule, by name and by position
  source = {Node("A", -1), Node("B", -1)};
  target = {Node("B", -1), Node("C", -1)};
  pairs  = match(source, target);
  require(targetOf(pairs, 1) == 0 && confidenceOf(pairs, 1) == Exact,
          "roots: several roots by name");
  require(targetOf(pairs, 0) == 1 && confidenceOf(pairs, 0) == Position,
          "roots: several roots, what is left by position");
}

void testNoDoubleMapping() {
  // two sources with the same name, one target of that name
  std::vector<Node> source = {Node("Rig", -1), Node("Hand", 0),
                              Node("Hand", 0)};
  std::vector<Node> target = {Node("Rig", -1), Node("Hand", 0)};
  Options noPosition;
  noPosition.matchByPosition = false;
  std::vector<Pair> pairs    = match(source, target, noPosition);
  require(isOneToOne(pairs), "double: one to one");
  require(pairs.size() == 1, "double: ambiguous names are not matched");
  pairs = match(source, target);
  require(isOneToOne(pairs), "double: one to one with the position");
  require(targetOf(pairs, 1) == -1 && targetOf(pairs, 2) == -1,
          "double: the position does not choose either");

  // the same names under different parents: the parents decide
  source = {Node("Rig", -1), Node("Arm_L", 0), Node("Hand", 1),
            Node("Arm_R", 0), Node("Hand", 3)};
  target = {Node("Rig", -1), Node("Arm_R", 0), Node("Hand", 1),
            Node("Arm_L", 0), Node("Hand", 3)};
  pairs  = match(source, target);
  require(pairs.size() == 5, "double: all paired");
  require(isOneToOne(pairs), "double: hands, one to one");
  require(targetOf(pairs, 1) == 3, "double: Arm_L");
  require(targetOf(pairs, 2) == 4, "double: the hand of Arm_L");
  require(confidenceOf(pairs, 2) == Exact, "double: hand confidence");
  require(targetOf(pairs, 3) == 1, "double: Arm_R");
  require(targetOf(pairs, 4) == 2, "double: the hand of Arm_R");

  // all the names the same
  source = {Node("X", -1), Node("X", 0), Node("X", 0), Node("X", 0)};
  target = {Node("X", -1), Node("X", 0), Node("X", 0)};
  pairs  = match(source, target);
  require(isOneToOne(pairs), "double: all X, one to one");
}

void testSourceLargerThanTarget() {
  std::vector<Node> source = biped(
      {"Torso", "Head", "Arm_L", "Arm_R", "Leg_L", "Leg_R", "Tail", "Wing"});
  std::vector<Node> target = biped({"Torso", "Arm_R", "Head", "Leg_L"});
  std::vector<Pair> pairs  = match(source, target);
  require(isOneToOne(pairs), "larger: one to one");
  require(pairs.size() == 4, "larger: only the target's parts are paired");
  require(targetOf(pairs, 1) == 2, "larger: head");
  require(targetOf(pairs, 3) == 1, "larger: right arm");
  require(targetOf(pairs, 4) == 3, "larger: left leg");
  for (int unmatched : {2, 5, 6, 7})
    require(targetOf(pairs, unmatched) == -1, "larger: extra parts unpaired");

  // and with the smaller rig as the source
  pairs = match(target, source);
  require(pairs.size() == 4, "smaller: all the parts are paired");
  require(targetOf(pairs, 2) == 1, "smaller: head");
}

void testEmptyAndSingle() {
  std::vector<Node> none, one = {Node("Head", -1)};
  require(match(none, none).empty(), "empty: both");
  require(match(none, one).empty(), "empty: no source");
  require(match(one, none).empty(), "empty: no target");

  std::vector<Pair> pairs = match(one, one);
  require(pairs.size() == 1 && confidenceOf(pairs, 0) == Exact,
          "single: same name");

  std::vector<Node> other = {Node("Tail", -1)};
  pairs                   = match(one, other);
  require(pairs.size() == 1 && confidenceOf(pairs, 0) == Position,
          "single: the roots go together");
  Options loose;
  loose.pairSingleRoots = false;
  loose.matchByPosition = false;
  require(match(one, other, loose).empty(), "single: not without the options");

  std::vector<Node> unnamed = {Node("Col1", -1)};
  pairs                     = match(unnamed, unnamed);
  require(pairs.size() == 1 && confidenceOf(pairs, 0) == Position,
          "single: default names are not names");
}

void testBadParents() {
  // a parent out of range, itself or in a loop does not break anything
  std::vector<Node> source = {Node("A", 5), Node("B", 1), Node("C", 2),
                              Node("D", -1)};
  std::vector<Node> target = {Node("A", 1), Node("B", 0), Node("D", 7)};
  std::vector<Pair> pairs  = match(source, target);
  require(isOneToOne(pairs), "bad parents: one to one");
  require(targetOf(pairs, 0) == 0 && targetOf(pairs, 1) == 1 &&
              targetOf(pairs, 3) == 2,
          "bad parents: names still match");
}

void testPlasticVertices() {
  // the vertices of a Plastic skeleton: names only, in a flat list
  Options options;
  options.pairSingleRoots = false;
  options.matchByPosition = false;
  std::vector<Node> source, target;
  for (const char *name : {"Root", "Vertex_001", "Elbow_L", "Hand_L"})
    source.push_back(Node(name, -1));
  for (const char *name : {"Hand_l", "Vertex_001", "root", "Vertex_002"})
    target.push_back(Node(name, -1));
  std::vector<Pair> pairs = match(source, target, options);
  require(pairs.size() == 2, "plastic: default names left out");
  require(targetOf(pairs, 0) == 2, "plastic: Root");
  require(targetOf(pairs, 3) == 0, "plastic: Hand_L");
  require(targetOf(pairs, 1) == -1, "plastic: Vertex_001 not matched");
}

//------------------------------------------------------------------

std::vector<KeyValue> keys(std::initializer_list<KeyValue> list) {
  return std::vector<KeyValue>(list);
}

void testValues() {
  const std::vector<KeyValue> source = keys({{10, 5}, {20, 8}, {30, 2}});

  // offset: the target value at the first key plus the change of the source
  std::vector<KeyValue> result = retargetKeys(source, ValueRule::Offset, 100);
  require(result.size() == 3, "offset: count");
  require(near(result[0].value, 100), "offset: first key is the target value");
  require(near(result[1].value, 103), "offset: change +3");
  require(near(result[2].value, 97), "offset: change -3");
  require(near(result[1].frame, 20) && near(result[2].frame, 30),
          "offset: frames kept");

  // the same target value: nothing changes
  result = retargetKeys(source, ValueRule::Offset, 5);
  require(near(result[1].value, 8) && near(result[2].value, 2),
          "offset: same rest value");

  // scale: the ratio to the first value
  result =
      retargetKeys(keys({{0, 1}, {10, 2}, {20, 0.5}}), ValueRule::Scale, 3);
  require(near(result[0].value, 3), "scale: first key");
  require(near(result[1].value, 6), "scale: twice");
  require(near(result[2].value, 1.5), "scale: half");

  // a negative (flipped) target
  result = retargetKeys(keys({{0, 1}, {10, 2}}), ValueRule::Scale, -1);
  require(near(result[0].value, -1) && near(result[1].value, -2),
          "scale: flipped target");

  // a source scale of 0 has no ratio: it falls back to the change
  result = retargetKeys(keys({{0, 0}, {10, 2}}), ValueRule::Scale, 3);
  require(near(result[0].value, 3) && near(result[1].value, 5),
          "scale: zero first value");

  // absolute: as they are
  result = retargetKeys(source, ValueRule::Absolute, 100);
  require(near(result[0].value, 5) && near(result[1].value, 8) &&
              near(result[2].value, 2),
          "absolute: unchanged");

  // the first key is the one with the lowest frame, not the first of the list
  result =
      retargetKeys(keys({{30, 2}, {10, 5}, {20, 8}}), ValueRule::Offset, 100);
  require(near(result[1].value, 100) && near(result[0].value, 97) &&
              near(result[2].value, 103),
          "offset: unsorted keys");

  // no keys
  require(retargetKeys(std::vector<KeyValue>(), ValueRule::Offset, 1).empty(),
          "no keys");

  // the map, for the speed handles
  ValueMap map = retargetMap(ValueRule::Scale, 2, 6);
  require(near(map.factor, 3) && near(map.offset, 0), "map: scale");
  map = retargetMap(ValueRule::Offset, 2, 6);
  require(near(map.factor, 1) && near(map.offset, 4), "map: offset");
  map = retargetMap(ValueRule::Absolute, 2, 6);
  require(near(map(7), 7), "map: absolute");
}

}  // namespace

int main() {
  try {
    testNames();
    testExact();
    testCaseAndSeparators();
    testTrailingNumber();
    testPosition();
    testRoots();
    testNoDoubleMapping();
    testSourceLargerThanTarget();
    testEmptyAndSingle();
    testBadParents();
    testPlasticVertices();
    testValues();
  } catch (const std::exception &e) {
    std::cerr << "rigmapping_test failed: " << e.what() << std::endl;
    return 1;
  }
  std::cout << "rigmapping_test passed" << std::endl;
  return 0;
}
