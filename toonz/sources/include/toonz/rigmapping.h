#pragma once

#ifndef RIGMAPPING_H
#define RIGMAPPING_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <map>
#include <string>
#include <vector>

// Rig Mapping: copies the animation of the parts of one rig onto the matching
// parts of another one. This header holds the pure part, with no dependency on
// the rest of the application: finding which part of the source rig goes with
// which part of the target rig (by name, then by position in the tree), and
// the rule that turns the source values into the target values. See
// doc/animate_rig_mapping.md.
namespace RigMapping {

//=============================================================================
// Names
//-----------------------------------------------------------------------------

inline bool isSeparator(char c) {
  return c == '_' || c == '-' || c == '.' || c == ' ' || c == '\t';
}

// Lower case (ASCII letters; fold the case of other letters beforehand), with
// every run of the separators _ - . and space, or a tab, turned into one
// space and none at either end: "Arm_L", "arm l" and " ARM-L " give "arm l".
inline std::string normalizeName(const std::string &name) {
  std::string result;
  bool pendingSpace = false;
  for (char c : name) {
    if (isSeparator(c)) {
      pendingSpace = !result.empty();
      continue;
    }
    if (pendingSpace) {
      result += ' ';
      pendingSpace = false;
    }
    if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    result += c;
  }
  return result;
}

// The normalised name without its spaces: "arm l" and "arml" are the same.
inline std::string compactName(const std::string &normalized) {
  std::string result;
  for (char c : normalized)
    if (c != ' ') result += c;
  return result;
}

// The normalised name without a trailing number: "arm l 2" gives "arm l" and
// "leg1" gives "leg". A name that is only a number is left as it is.
inline std::string stripNumberSuffix(const std::string &normalized) {
  std::size_t end = normalized.size();
  while (end > 0 && normalized[end - 1] >= '0' && normalized[end - 1] <= '9')
    end--;
  if (end == normalized.size()) return normalized;
  while (end > 0 && normalized[end - 1] == ' ') end--;
  if (end == 0) return normalized;
  return normalized.substr(0, end);
}

// The name without the spaces and tabs at either end.
inline std::string trimName(const std::string &name) {
  std::size_t first = 0, last = name.size();
  while (first < last && (name[first] == ' ' || name[first] == '\t')) first++;
  while (last > first && (name[last - 1] == ' ' || name[last - 1] == '\t'))
    last--;
  return name.substr(first, last - first);
}

// Whether the name is one the application makes up (an empty one, "Col3",
// "Peg2", "Camera1", "Table", the "Vertex_003" of the Plastic skeleton
// vertices). Such a name says nothing about what the part is, so it must not
// be used to match parts: "Col3" of a rig and "Col3" of another one are not
// the same part. The case and the separators do not matter.
inline bool isDefaultName(const std::string &name) {
  const std::string n = normalizeName(name);
  if (n.empty()) return true;
  std::size_t end = n.size();
  while (end > 0 && n[end - 1] >= '0' && n[end - 1] <= '9') end--;
  const bool hasNumber = end < n.size();
  std::string prefix   = n.substr(0, end);
  while (!prefix.empty() && prefix.back() == ' ') prefix.pop_back();
  if (prefix == "table") return true;
  static const char *const prefixes[] = {"col", "column", "peg",   "pegbar",
                                         "cam", "camera", "vertex"};
  for (const char *p : prefixes)
    if (prefix == p) return hasNumber;
  return false;
}

//=============================================================================
// Matching the parts of two rigs
//-----------------------------------------------------------------------------

// A part of a rig: its name and its parent in the same list (-1 for the
// root of the rig). The list must have the parts of a sibling group in the
// order they have in the rig; that order is used to match by position.
struct Node {
  std::string name;  // empty if the part has no name of its own
  int parent;

  Node() : parent(-1) {}
  Node(const std::string &n, int p) : name(n), parent(p) {}
};

// How sure a pair is, best first.
enum Confidence {
  Exact = 0,       // the same name
  Normalized,      // the same name but for the case and the separators
  IgnoringNumber,  // as above, once a trailing number is left out
  Position         // only the place in the tree says they go together
};

inline const char *confidenceName(Confidence c) {
  switch (c) {
  case Exact:
    return "Same name";
  case Normalized:
    return "Same name, other case or separators";
  case IgnoringNumber:
    return "Same name, number ignored";
  default:
    return "By position";
  }
}

struct Pair {
  int source;
  int target;
  Confidence confidence;
};

struct Options {
  bool pairSingleRoots;     // pass 0 below
  bool ignoreNumberSuffix;  // pass 3 below
  bool matchByPosition;     // pass 4 below

  Options()
      : pairSingleRoots(true)
      , ignoreNumberSuffix(true)
      , matchByPosition(true) {}
};

namespace Detail {

class Matcher {
  enum { LevelCount = 3 };

  const std::vector<Node> &m_source, &m_target;
  Options m_options;

  std::vector<int> m_sourceMatch, m_targetMatch;  // -1 if not matched
  std::vector<Confidence> m_confidence;           // by source
  std::vector<std::string> m_sourceKeys[LevelCount];
  std::vector<std::string> m_targetKeys[LevelCount];
  std::vector<std::vector<int>> m_sourceKids, m_targetKids;
  std::vector<int> m_sourceRoots, m_targetRoots;
  std::vector<int> m_order;  // source parts, the higher ones first

public:
  Matcher(const std::vector<Node> &source, const std::vector<Node> &target,
          const Options &options)
      : m_source(source)
      , m_target(target)
      , m_options(options)
      , m_sourceMatch(source.size(), -1)
      , m_targetMatch(target.size(), -1)
      , m_confidence(source.size(), Position) {
    buildKeys(source, m_sourceKeys);
    buildKeys(target, m_targetKeys);
    buildTree(source, m_sourceKids, m_sourceRoots);
    buildTree(target, m_targetKids, m_targetRoots);
    buildOrder();
  }

  std::vector<Pair> run() {
    // 0. The roots of two rigs that have a single one go together: they are
    //    the two rigs the user chose. The pair is as sure as their names say.
    if (m_options.pairSingleRoots && m_sourceRoots.size() == 1 &&
        m_targetRoots.size() == 1) {
      const int s = m_sourceRoots[0], t = m_targetRoots[0];
      Confidence confidence = Position;
      for (int level = LevelCount - 1; level >= 0; level--)
        if (!m_sourceKeys[level][s].empty() &&
            m_sourceKeys[level][s] == m_targetKeys[level][t])
          confidence = Confidence(level);
      link(s, t, confidence);
    }

    // 1. Names, in the whole rigs. A name has to be the one of a single
    //    unmatched part on each side. Exact names first, then the same names
    //    but for the case and the separators, then without the numbers.
    std::vector<int> allSource(m_source.size()), allTarget(m_target.size());
    for (std::size_t i = 0; i < allSource.size(); i++) allSource[i] = int(i);
    for (std::size_t i = 0; i < allTarget.size(); i++) allTarget[i] = int(i);
    for (int level = 0; level < LevelCount; level++)
      matchLevel(allSource, allTarget, level);

    // 2. Names again, among the children of the parts matched so far (the
    //    same name can be used by the hands of the two arms), and then the
    //    position: the children of matched parts that are still unmatched
    //    go together in order, if there are as many on each side. The
    //    highest group goes first and the names get the next turn, as the
    //    new pairs make new groups.
    for (;;) {
      if (matchNamesInGroups()) continue;
      if (m_options.matchByPosition && matchOneGroupByPosition()) continue;
      break;
    }

    std::vector<Pair> pairs;
    for (std::size_t s = 0; s < m_source.size(); s++)
      if (m_sourceMatch[s] >= 0) {
        Pair pair;
        pair.source     = int(s);
        pair.target     = m_sourceMatch[s];
        pair.confidence = m_confidence[s];
        pairs.push_back(pair);
      }
    return pairs;
  }

private:
  // The keys a part can be matched by, one for each level; empty if it can't.
  void buildKeys(const std::vector<Node> &nodes,
                 std::vector<std::string> (&keys)[LevelCount]) const {
    for (int level = 0; level < LevelCount; level++)
      keys[level].assign(nodes.size(), std::string());
    for (std::size_t i = 0; i < nodes.size(); i++) {
      const std::string &name = nodes[i].name;
      if (isDefaultName(name)) continue;
      keys[0][i]             = trimName(name);
      const std::string norm = normalizeName(name);
      keys[1][i]             = compactName(norm);
      if (m_options.ignoreNumberSuffix)
        keys[2][i] = compactName(stripNumberSuffix(norm));
    }
  }

  static void buildTree(const std::vector<Node> &nodes,
                        std::vector<std::vector<int>> &kids,
                        std::vector<int> &roots) {
    const int count = int(nodes.size());
    kids.assign(nodes.size(), std::vector<int>());
    for (int i = 0; i < count; i++) {
      const int parent = nodes[i].parent;
      if (parent < 0 || parent >= count || parent == i)
        roots.push_back(i);
      else
        kids[parent].push_back(i);
    }
  }

  // The source parts by depth, the shallowest first (a parent loop in the
  // input counts as very deep).
  void buildOrder() {
    const int count = int(m_source.size());
    std::vector<int> depth(m_source.size(), 0);
    for (int i = 0; i < count; i++) {
      int steps = 0, p = m_source[i].parent;
      while (p >= 0 && p < count && p != i && steps <= count) {
        p = m_source[p].parent;
        steps++;
      }
      depth[i] = steps;
    }
    m_order.resize(m_source.size());
    for (int i = 0; i < count; i++) m_order[i] = i;
    std::stable_sort(m_order.begin(), m_order.end(),
                     [&depth](int a, int b) { return depth[a] < depth[b]; });
  }

  void link(int source, int target, Confidence confidence) {
    m_sourceMatch[source] = target;
    m_targetMatch[target] = source;
    m_confidence[source]  = confidence;
  }

  std::vector<int> unmatchedSource(const std::vector<int> &indices) const {
    std::vector<int> result;
    for (int i : indices)
      if (m_sourceMatch[i] < 0) result.push_back(i);
    return result;
  }
  std::vector<int> unmatchedTarget(const std::vector<int> &indices) const {
    std::vector<int> result;
    for (int i : indices)
      if (m_targetMatch[i] < 0) result.push_back(i);
    return result;
  }

  // Matches the unmatched parts of the lists whose key at the level is the
  // key of one part of each list. Returns whether it matched any.
  bool matchLevel(const std::vector<int> &sources,
                  const std::vector<int> &targets, int level) {
    std::map<std::string, std::vector<int>> sourcesByKey, targetsByKey;
    for (int i : sources)
      if (m_sourceMatch[i] < 0 && !m_sourceKeys[level][i].empty())
        sourcesByKey[m_sourceKeys[level][i]].push_back(i);
    for (int i : targets)
      if (m_targetMatch[i] < 0 && !m_targetKeys[level][i].empty())
        targetsByKey[m_targetKeys[level][i]].push_back(i);

    bool found = false;
    for (const auto &entry : sourcesByKey) {
      std::map<std::string, std::vector<int>>::const_iterator it =
          targetsByKey.find(entry.first);
      if (it == targetsByKey.end()) continue;
      if (entry.second.size() != 1 || it->second.size() != 1) continue;
      link(entry.second[0], it->second[0], Confidence(level));
      found = true;
    }
    return found;
  }

  bool matchNamesInGroup(const std::vector<int> &sources,
                         const std::vector<int> &targets) {
    if (sources.empty() || targets.empty()) return false;
    bool found = false;
    for (int level = 0; level < LevelCount; level++)
      if (matchLevel(sources, targets, level)) found = true;
    return found;
  }

  bool matchNamesInGroups() {
    bool found = matchNamesInGroup(unmatchedSource(m_sourceRoots),
                                   unmatchedTarget(m_targetRoots));
    for (int s : m_order) {
      const int t = m_sourceMatch[s];
      if (t < 0) continue;
      if (matchNamesInGroup(unmatchedSource(m_sourceKids[s]),
                            unmatchedTarget(m_targetKids[t])))
        found = true;
    }
    return found;
  }

  // The unmatched parts go together in order if they are as many on each
  // side; with more on one side nothing says which are the extra ones.
  bool matchGroupByPosition(const std::vector<int> &sources,
                            const std::vector<int> &targets) {
    if (sources.empty() || sources.size() != targets.size()) return false;
    for (std::size_t k = 0; k < sources.size(); k++)
      link(sources[k], targets[k], Position);
    return true;
  }

  // Matches the highest group that can be, if any.
  bool matchOneGroupByPosition() {
    if (matchGroupByPosition(unmatchedSource(m_sourceRoots),
                             unmatchedTarget(m_targetRoots)))
      return true;
    for (int s : m_order) {
      const int t = m_sourceMatch[s];
      if (t < 0) continue;
      if (matchGroupByPosition(unmatchedSource(m_sourceKids[s]),
                               unmatchedTarget(m_targetKids[t])))
        return true;
    }
    return false;
  }
};

}  // namespace Detail

// Pairs the parts of the source rig with those of the target rig. Each part
// is in at most one pair, on either side. In order:
//   0. the roots, if each rig has a single one;
//   1. the same name, once in each rig, with the same case and separators;
//   2. the same name but for the case and the separators ("Arm_L", "arm l"),
//      once in each rig;
//   3. as 2, once a trailing number is left out ("Arm_L_1", "arm l");
//   4. the position in the tree: the children of two matched parts that are
//      still unmatched go together in order, if there are as many on each
//      side. The roots of the rigs are such a group.
// Names are matched in the whole rigs first, and then in the group of the
// children of each pair, so that the hands of two arms can have the same
// name. A default name (see isDefaultName()) is never used for a match by
// name. Parts without a match are not in the result, which is sorted by
// source.
inline std::vector<Pair> match(const std::vector<Node> &source,
                               const std::vector<Node> &target,
                               const Options &options = Options()) {
  if (source.empty() || target.empty()) return std::vector<Pair>();
  Detail::Matcher matcher(source, target, options);
  return matcher.run();
}

//=============================================================================
// Values
//-----------------------------------------------------------------------------

// How the values of a channel of the source go to the target.
enum class ValueRule {
  Offset,    // the target value at the first key, plus the change of the source
             // since its first key: positions, angles
  Scale,     // the target value at the first key, times the ratio of the source
             // to its first value: scale factors
  Absolute,  // the values of the source, as they are
};

// The map v -> v * factor + offset.
struct ValueMap {
  double factor;
  double offset;

  ValueMap() : factor(1.0), offset(0.0) {}
  double operator()(double value) const { return value * factor + offset; }
};

// The map from the values of the source to those of the target, when the
// first key of the source has the value firstSourceValue and the target has
// the value targetValueAtFirstKey at that frame. A scale rule falls back to
// the offset one if the first value of the source is 0 (no ratio then).
inline ValueMap retargetMap(ValueRule rule, double firstSourceValue,
                            double targetValueAtFirstKey) {
  ValueMap map;
  switch (rule) {
  case ValueRule::Absolute:
    break;
  case ValueRule::Offset:
    map.offset = targetValueAtFirstKey - firstSourceValue;
    break;
  case ValueRule::Scale:
    if (std::abs(firstSourceValue) > 1e-9)
      map.factor = targetValueAtFirstKey / firstSourceValue;
    else
      map.offset = targetValueAtFirstKey - firstSourceValue;
    break;
  }
  return map;
}

struct KeyValue {
  double frame;
  double value;

  KeyValue() : frame(0.0), value(0.0) {}
  KeyValue(double f, double v) : frame(f), value(v) {}
};

// The keys of the source with the values they get on the target. The first
// key is the one with the lowest frame; the frames do not change.
inline std::vector<KeyValue> retargetKeys(const std::vector<KeyValue> &keys,
                                          ValueRule rule,
                                          double targetValueAtFirstKey) {
  std::vector<KeyValue> result = keys;
  if (result.empty()) return result;
  std::size_t first = 0;
  for (std::size_t i = 1; i < result.size(); i++)
    if (result[i].frame < result[first].frame) first = i;
  const ValueMap map =
      retargetMap(rule, result[first].value, targetValueAtFirstKey);
  for (KeyValue &key : result) key.value = map(key.value);
  return result;
}

}  // namespace RigMapping

#endif
