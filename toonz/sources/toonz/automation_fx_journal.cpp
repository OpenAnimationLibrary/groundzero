#include "automation.h"
#include "automation_fx.h"
#include "automation_effects.h"
#include "automation_fx_journal.h"
#include "automation_scene_host.h"

#include "tconst.h"
#include "tdoublekeyframe.h"
#include "tdoubleparam.h"
#include "tfx.h"
#include "tfxattributes.h"
#include "tmacrofx.h"
#include "tnotanimatableparam.h"
#include "tzeraryfx.h"
#include "tparamcontainer.h"
#include "tparamset.h"
#include "toonz/fxdag.h"
#include "toonz/tcolumnfx.h"
#include "toonz/tcolumnfxset.h"
#include "toonz/toonzscene.h"
#include "toonz/txsheet.h"
#include "toonz/txshcolumn.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QUuid>

#include <algorithm>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <set>
#include <tuple>
#include <vector>

namespace GroundzeroAutomation {
namespace {

constexpr int kMaximumSteps             = 1024;
constexpr qint64 kMaximumJournalBytes   = 2 * 1024 * 1024;
constexpr qint64 kMaximumStepBytes      = 256 * 1024;
constexpr int kMaximumStepEntries       = 512;
constexpr int kMaximumNetEntries        = 65536;
constexpr int kMaximumParametersChanged = 256;
constexpr int kMaximumRingMembers       = 1024;
constexpr char kSeparator               = '\x1f';

// Entity kinds in the order a delta lists them.
enum Kind { CurrentOutput, Node, Edge, Terminal, LinkedGroup, Group };
const char* const kKindNames[] = {"current_output", "node",         "edge",
                                  "terminal",       "linked_group", "group"};

// Entity key -> state; an absent entity is null. Keys are
// "<kind>\x1f<handle>[\x1f<port>]".
using States = std::map<QString, QJsonValue>;

// A 128-bit fingerprint: equal fingerprints mean equal states.
struct Digest final {
  quint64 a = 0;
  quint64 b = 0;
  bool operator==(const Digest& other) const {
    return a == other.a && b == other.b;
  }
  bool operator!=(const Digest& other) const { return !(*this == other); }
};

// One entity of a snapshot: its fingerprint and its state, built only when
// the fingerprint is new.
struct Entity final {
  Digest fingerprint;
  QJsonValue state;
};
using Entities = QHash<QString, Entity>;

struct Step final {
  quint64 revision = 0;
  States before;
  qint64 bytes  = 0;
  bool overflow = false;
};

// Where an entity sorts: its kind, the creation sequence of the effect it
// belongs to, and a port index.
using Order = std::tuple<int, quint64, int>;

QString entityKey(Kind kind, const QString& handle = {},
                  const QString& port = {}) {
  QString key = kKindNames[kind];
  if (!handle.isEmpty()) key += kSeparator + handle;
  if (!port.isNull()) key += kSeparator + port;
  return key;
}

QStringList keyParts(const QString& key) { return key.split(kSeparator); }

Kind keyKind(const QString& key) {
  const QString name = keyParts(key).value(0);
  for (int kind = CurrentOutput; kind <= Group; ++kind)
    if (name == kKindNames[kind]) return static_cast<Kind>(kind);
  throw Error("internal_error", "Unknown FX journal entity.");
}

// Two independent 64-bit lanes of a bijective mixer over 64-bit words: fast,
// and wide enough that equal fingerprints mean equal inputs in practice.
class Hasher final {
public:
  void word(quint64 value) {
    m_digest.a = mix(m_digest.a ^ value);
    m_digest.b = mix(m_digest.b + value + 0x9e3779b97f4a7c15ULL);
  }
  void number(double value) {
    quint64 bits;
    std::memcpy(&bits, &value, sizeof(bits));
    word(bits);
  }
  void bytes(const void* data, size_t size) {
    word(size);
    const char* text = static_cast<const char*>(data);
    for (size_t offset = 0; offset < size; offset += 8) {
      quint64 chunk = 0;
      std::memcpy(&chunk, text + offset, std::min<size_t>(8, size - offset));
      word(chunk);
    }
  }
  void text(const std::string& value) { bytes(value.data(), value.size()); }
  void text(const std::wstring& value) {
    bytes(value.data(), value.size() * sizeof(wchar_t));
  }
  void text(const QString& value) {
    bytes(value.utf16(), value.size() * sizeof(char16_t));
  }
  void digest(const Digest& value) {
    word(value.a);
    word(value.b);
  }
  Digest result() const { return m_digest; }

private:
  static quint64 mix(quint64 value) {
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
  }
  Digest m_digest{0x243f6a8885a308d3ULL, 0x13198a2e03707344ULL};
};

QString hex(const Digest& digest) {
  return QStringLiteral("%1%2")
      .arg(digest.a, 16, 16, QLatin1Char('0'))
      .arg(digest.b, 16, 16, QLatin1Char('0'));
}

}  // namespace

class FxGraphJournal final {
public:
  bool started            = false;
  quint64 baseRevision    = 0;
  quint64 currentRevision = 0;
  Entities current;
  std::deque<Step> steps;
  qint64 bytes = 0;
  QHash<QString, quint64> sequence;
  quint64 nextSequence = 1;
  QHash<QString, Order> order;
  std::map<int, QString> groupHandles;
  std::map<QString, int> groupIds;
  QHash<QString, QString> linkHandles;
};

namespace {

void addKeyframe(Hasher& hash, const TDoubleKeyframe& key) {
  for (double value :
       {double(key.m_type), double(key.m_prevType), key.m_frame, key.m_value,
        double(key.m_isKeyframe), double(key.m_step), key.m_speedIn.x,
        key.m_speedIn.y, key.m_speedOut.x, key.m_speedOut.y,
        double(key.m_linkedHandles), double(key.m_fileParams.m_fieldIndex),
        key.m_similarShapeOffset})
    hash.number(value);
  hash.text(key.m_expressionText);
  hash.text(key.m_unitName);
  hash.text(key.m_fileParams.m_path.getWideString());
}

bool addDiscrete(Hasher& hash, TParam* param) {
  if (auto* integer = dynamic_cast<TIntParam*>(param)) {
    hash.word(quint64(integer->getValue()));
    return true;
  }
  if (auto* boolean = dynamic_cast<TBoolParam*>(param)) {
    hash.word(boolean->getValue() ? 1 : 2);
    return true;
  }
  if (auto* enumeration = dynamic_cast<TEnumParam*>(param)) {
    hash.word(quint64(enumeration->getValue()));
    return true;
  }
  return false;
}

// Every value and key field of a parameter, so that equal digests mean equal
// parameters.
void addParameter(Hasher& hash, TParam* param, int depth) {
  if (depth > 16)
    throw Error("resource_limit", "Parameter nesting is too deep.");
  if (auto* number = dynamic_cast<TDoubleParam*>(param)) {
    hash.number(number->getDefaultValue());
    for (int index = 0; index < number->getKeyframeCount(); ++index)
      addKeyframe(hash, number->getKeyframe(index));
    return;
  }
  if (auto* set = dynamic_cast<TParamSet*>(param)) {
    for (int index = 0; index < set->getParamCount(); ++index)
      addParameter(hash, set->getParam(index).getPointer(), depth + 1);
    return;
  }
  if (addDiscrete(hash, param)) return;
  std::set<double> frames;
  param->getKeyframes(frames);
  frames.insert(0);
  for (double frame : frames) {
    hash.number(frame);
    hash.text(param->getValueAlias(frame, 17));
  }
}

using ParameterDigests = std::vector<std::pair<QString, Digest>>;

void addParameters(ParameterDigests& digests, TFx* fx, const QString& prefix) {
  TParamContainer* params = fx->getParams();
  for (int index = 0; index < params->getParamCount(); ++index) {
    const TParamVar* entry = params->getParamVar(index);
    if (entry->isHidden() || entry->isObsolete()) continue;
    Hasher hash;
    addParameter(hash, entry->getParam(), 0);
    digests.push_back(
        {prefix + QString::fromStdString(entry->getName()), hash.result()});
  }
}

TFx* nativeFx(TFx* fx) {
  auto* wrapper = dynamic_cast<TZeraryColumnFx*>(fx);
  return wrapper && wrapper->getZeraryFx() ? wrapper->getZeraryFx() : fx;
}

// The top sheet's nodes, as fx.graph lists them.
std::vector<TFx*> sheetNodes(TXsheet* sheet) {
  std::set<TFx*> members;
  FxDag* graph = sheet->getFxDag();
  graph->getInternalFxs()->getFxs(members);
  members.insert(graph->getXsheetFx());
  for (int index = 0; index < graph->getOutputFxCount(); ++index)
    members.insert(graph->getOutputFx(index));
  for (int index = 0; index < sheet->getColumnCount(); ++index) {
    TXshColumn* column = sheet->getColumn(index);
    if (column && column->getFx()) members.insert(column->getFx());
  }
  members.erase(nullptr);
  return {members.begin(), members.end()};
}

// A deterministic order for effects first seen together: the Xsheet node,
// outputs, columns by index, then other effects by native id.
std::tuple<int, int, std::wstring> seenOrder(FxDag* graph, TFx* fx) {
  if (fx == graph->getXsheetFx()) return {0, 0, L""};
  for (int index = 0; index < graph->getOutputFxCount(); ++index)
    if (graph->getOutputFx(index) == fx) return {1, index, L""};
  if (auto* column = dynamic_cast<TColumnFx*>(fx))
    return {2, column->getColumnIndex(), fx->getFxId()};
  return {3, 0, fx->getFxId()};
}

// What a node's fingerprint covers, gathered once per snapshot.
struct NodeFacts final {
  QString id;
  quint64 sequence = 0;
  TFx* native      = nullptr;
  bool terminal    = false;
  QStringList ports;
  QStringList sources;
  ParameterDigests parameters;
  QStringList members;
  QJsonValue link;
};

// Builds the entities of the top sheet's graph. A node's JSON state is
// rebuilt only when its fingerprint differs from the previous snapshot's.
class Snapshotter final {
public:
  Snapshotter(Document& document, FxGraphJournal& journal,
              const Entities* previous = nullptr)
      : m_document(document)
      , m_journal(journal)
      , m_previous(previous)
      , m_sheet(document.scene->getXsheet())
      , m_graph(m_sheet->getFxDag()) {
    for (auto it = document.effects.constBegin();
         it != document.effects.constEnd(); ++it)
      m_handles.insert(it.value(), it.key());
  }

  Entities take() {
    const std::vector<TFx*> nodes = sheetNodes(m_sheet);
    assignSequences(nodes);
    for (TFx* fx : nodes) m_byNative.insert(nativeFx(fx), fx);
    collectRings(nodes);
    for (TFx* fx : nodes) addNode(fx);
    TFx* output = m_graph->getCurrentOutputFx();
    if (output) putValue(entityKey(CurrentOutput), handle(output), {});
    addGroups();
    return std::move(m_entities);
  }

private:
  QString handle(TFx* fx) {
    const auto found = m_handles.constFind(fx);
    if (found != m_handles.constEnd()) return found.value();
    const QString id = m_document.idForFx(fx);
    m_handles.insert(fx, id);
    return id;
  }

  quint64 sequenceOf(const QString& id) const {
    return m_journal.sequence.value(id, std::numeric_limits<quint64>::max());
  }

  void assignSequences(const std::vector<TFx*>& nodes) {
    std::vector<std::pair<std::tuple<int, int, std::wstring>, TFx*>> fresh;
    for (TFx* fx : nodes)
      if (!m_journal.sequence.contains(handle(fx)))
        fresh.push_back({seenOrder(m_graph, fx), fx});
    std::sort(fresh.begin(), fresh.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& entry : fresh)
      m_journal.sequence.insert(handle(entry.second), m_journal.nextSequence++);
  }

  QString linkHandle(const QString& fxHandle) {
    QString& link = m_journal.linkHandles[fxHandle];
    if (link.isEmpty())
      link = "fxlink_" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    return link;
  }

  QStringList ringMembers(TFx* node) {
    TFx* native = nativeFx(node);
    std::vector<std::pair<quint64, QString>> members;
    TFx* member = native;
    for (int guard = 0; guard < kMaximumRingMembers; ++guard) {
      const auto owner = m_byNative.constFind(member);
      if (owner != m_byNative.constEnd()) {
        const QString id = handle(owner.value());
        members.push_back({sequenceOf(id), id});
      }
      member = member->getLinkedFx();
      if (!member || member == native) break;
    }
    std::sort(members.begin(), members.end());
    QStringList result;
    for (const auto& entry : members) result.append(entry.second);
    return result;
  }

  // Each parameter ring once: its group handle is the one registered for
  // its member with the lowest creation sequence.
  void collectRings(const std::vector<TFx*>& nodes) {
    for (TFx* fx : nodes) {
      TFx* native = nativeFx(fx);
      if (native->getLinkedFx() == native || m_rings.contains(handle(fx)))
        continue;
      const QStringList members = ringMembers(fx);
      if (members.size() < 2) continue;
      const QString group = linkHandle(members.first());
      for (const QString& member : members) m_rings.insert(member, group);
      putValue(entityKey(LinkedGroup, group),
               QJsonArray::fromStringList(members),
               {LinkedGroup, sequenceOf(members.first()), 0});
    }
  }

  Digest fingerprintOf(const QJsonValue& value) {
    Hasher hash;
    hash.bytes(
        QJsonDocument(QJsonArray{value})
            .toJson(QJsonDocument::Compact)
            .constData(),
        QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact).size());
    return hash.result();
  }

  void putValue(const QString& key, const QJsonValue& state,
                const Order& order) {
    Hasher hash;
    if (state.isString()) hash.text(state.toString());
    if (state.isBool()) hash.word(state.toBool() ? 1 : 2);
    if (state.isArray() || state.isObject()) hash.digest(fingerprintOf(state));
    m_entities.insert(key, Entity{hash.result(), state});
    m_journal.order.insert(key, order);
  }

  NodeFacts facts(TFx* fx) {
    NodeFacts node;
    node.id       = handle(fx);
    node.sequence = sequenceOf(node.id);
    node.native   = nativeFx(fx);
    node.terminal = m_graph->getTerminalFxs()->containsFx(fx);
    for (int index = 0; index < fx->getInputPortCount(); ++index) {
      node.ports.append(QString::fromStdString(fx->getInputPortName(index)));
      TFx* source = fx->getInputPort(index)->getFx();
      node.sources.append(source ? handle(source) : QString());
    }
    if (auto* macro = dynamic_cast<TMacroFx*>(fx)) {
      for (const auto& member : macro->getFxs()) {
        const QString key = QString::fromStdWString(member->getFxId());
        node.members.append(key + ":" +
                            QString::fromStdString(member->getFxType()));
        addParameters(node.parameters, member.getPointer(), key + "/");
      }
    } else {
      addParameters(node.parameters, node.native, {});
    }
    const auto ring = m_rings.constFind(node.id);
    if (ring != m_rings.constEnd()) node.link = ring.value();
    return node;
  }

  Digest fingerprint(TFx* fx, const NodeFacts& node) {
    Hasher hash;
    TFxAttributes* attributes = fx->getAttributes();
    hash.text(node.native->getFxType());
    hash.text(node.native->getName());
    hash.word(attributes->isEnabled() &&
              node.native->getAttributes()->isEnabled());
    hash.word(node.terminal);
    hash.word(dynamic_cast<TZeraryColumnFx*>(fx) != nullptr);
    hash.word(dynamic_cast<TMacroFx*>(fx) != nullptr);
    hash.number(attributes->getDagNodePos().x);
    hash.number(attributes->getDagNodePos().y);
    const QStack<int> groups = attributes->getGroupIdStack();
    hash.word(quint64(groups.size()));
    for (int id : groups) hash.word(quint64(id));
    hash.text(node.link.toString());
    for (const QString& port : node.ports) hash.text(port);
    for (const auto& parameter : node.parameters) {
      hash.text(parameter.first);
      hash.digest(parameter.second);
    }
    for (const QString& member : node.members) hash.text(member);
    return hash.result();
  }

  QJsonValue position(TFx* fx) {
    const TPointD point = fx->getAttributes()->getDagNodePos();
    if (point == TConst::nowhere) return QJsonValue();
    return QJsonObject{{"x", point.x}, {"y", point.y}};
  }

  QJsonArray groupIds(TFx* fx) {
    QJsonArray ids;
    for (int id : fx->getAttributes()->getGroupIdStack())
      ids.append(automationFxGroupHandle(m_document, id));
    return ids;
  }

  QJsonObject nodeState(TFx* fx, const NodeFacts& node) {
    QJsonObject digests;
    for (const auto& parameter : node.parameters)
      digests.insert(parameter.first, hex(parameter.second));
    return {
        {"type", QString::fromStdString(node.native->getFxType())},
        {"name", QString::fromStdWString(node.native->getName())},
        {"enabled", fx->getAttributes()->isEnabled() &&
                        node.native->getAttributes()->isEnabled()},
        {"terminal", node.terminal},
        {"generator_wrapper", dynamic_cast<TZeraryColumnFx*>(fx) != nullptr},
        {"macro", dynamic_cast<TMacroFx*>(fx) != nullptr},
        {"position", position(fx)},
        {"group_ids", groupIds(fx)},
        {"linked_group_id", node.link},
        {"port_names", QJsonArray::fromStringList(node.ports)},
        {"parameters", digests},
        {"macro_members", QJsonArray::fromStringList(node.members)}};
  }

  void addNode(TFx* fx) {
    const NodeFacts node = facts(fx);
    for (int index = 0; index < node.ports.size(); ++index)
      if (!node.sources[index].isEmpty())
        putValue(entityKey(Edge, node.id, node.ports[index]),
                 node.sources[index], {Edge, node.sequence, index});
    if (node.terminal)
      putValue(entityKey(Terminal, node.id), true,
               {Terminal, node.sequence, 0});
    const QString key      = entityKey(Node, node.id);
    const Digest digest    = fingerprint(fx, node);
    const Entity* previous = previousEntity(key);
    const bool reuse       = previous && previous->fingerprint == digest;
    m_entities.insert(
        key, Entity{digest,
                    reuse ? previous->state : QJsonValue(nodeState(fx, node))});
    m_journal.order.insert(key, Order{Node, node.sequence, 0});
    collectGroups(fx, node.sequence);
  }

  const Entity* previousEntity(const QString& key) const {
    if (!m_previous) return nullptr;
    const auto found = m_previous->constFind(key);
    return found == m_previous->constEnd() ? nullptr : &found.value();
  }

  void collectGroups(TFx* fx, quint64 order) {
    const QStack<int> ids = fx->getAttributes()->getGroupIdStack();
    if (ids.isEmpty()) return;
    const QStack<std::wstring> names = fx->getAttributes()->getGroupNameStack();
    for (int index = 0; index < ids.size(); ++index) {
      GroupSeen& seen = m_groups[ids[index]];
      ++seen.members;
      if (order >= seen.first) continue;
      seen.first = order;
      seen.name  = QString::fromStdWString(names.value(index));
    }
  }

  void addGroups() {
    for (const auto& entry : m_groups)
      putValue(
          entityKey(Group, automationFxGroupHandle(m_document, entry.first)),
          QJsonObject{{"name", entry.second.name},
                      {"member_count", entry.second.members}},
          {Group, entry.second.first, entry.first});
  }

  struct GroupSeen final {
    QString name;
    int members   = 0;
    quint64 first = std::numeric_limits<quint64>::max();
  };

  Document& m_document;
  FxGraphJournal& m_journal;
  const Entities* m_previous;
  TXsheet* m_sheet;
  FxDag* m_graph;
  QHash<TFx*, QString> m_handles;
  QHash<TFx*, TFx*> m_byNative;
  QHash<QString, QString> m_rings;
  std::map<int, GroupSeen> m_groups;
  Entities m_entities;
};

FxGraphJournal& journalOf(Document& document) {
  if (!document.fxJournal)
    document.fxJournal = std::make_shared<FxGraphJournal>();
  FxGraphJournal& journal = *document.fxJournal;
  if (!journal.started) {
    journal.started         = true;
    journal.baseRevision    = document.revision;
    journal.currentRevision = document.revision;
    journal.current         = Snapshotter(document, journal).take();
  }
  return journal;
}

qint64 jsonBytes(const States& states) {
  qint64 bytes = 0;
  for (const auto& entry : states)
    bytes += entry.first.size() * 2 +
             QJsonDocument(QJsonArray{entry.second}).toJson().size();
  return bytes;
}

void retain(FxGraphJournal& journal, Step step) {
  step.bytes = jsonBytes(step.before);
  if (step.bytes > kMaximumStepBytes) {
    step.before.clear();
    step.overflow = true;
  }
  journal.bytes += step.bytes;
  journal.steps.push_back(std::move(step));
  while (static_cast<int>(journal.steps.size()) > kMaximumSteps ||
         journal.bytes > kMaximumJournalBytes) {
    journal.bytes -= journal.steps.front().bytes;
    journal.baseRevision = journal.steps.front().revision;
    journal.steps.pop_front();
  }
}

// The entities whose fingerprints differ between two snapshots, with their
// state before.
States changedBefore(const Entities& before, const Entities& after) {
  States changed;
  for (auto entry = before.constBegin(); entry != before.constEnd(); ++entry) {
    const auto found = after.constFind(entry.key());
    if (found == after.constEnd() || found->fingerprint != entry->fingerprint)
      changed.insert({entry.key(), entry->state});
  }
  for (auto entry = after.constBegin(); entry != after.constEnd(); ++entry)
    if (!before.contains(entry.key()))
      changed.insert({entry.key(), QJsonValue()});
  return changed;
}

QJsonObject publicNode(QJsonObject state) {
  state.remove("parameters");
  state.remove("macro_members");
  return state;
}

QJsonArray aspects(const QJsonObject& before, const QJsonObject& after) {
  static const std::pair<const char*, const char*> fields[] = {
      {"name", "name"},
      {"enabled", "enabled"},
      {"port_names", "ports"},
      {"terminal", "terminal"},
      {"parameters", "parameters"},
      {"position", "position"},
      {"group_ids", "groups"},
      {"linked_group_id", "link"},
      {"macro_members", "macro_members"}};
  QJsonArray result;
  for (const auto& field : fields)
    if (before.value(field.first) != after.value(field.first))
      result.append(field.second);
  return result;
}

void addParametersChanged(QJsonObject& entry, const QJsonObject& before,
                          const QJsonObject& after) {
  QStringList names;
  const QJsonObject old = before.value("parameters").toObject();
  const QJsonObject now = after.value("parameters").toObject();
  QStringList keys      = old.keys() + now.keys();
  keys.removeDuplicates();
  std::sort(keys.begin(), keys.end());
  for (const QString& key : keys)
    if (old.value(key) != now.value(key)) names.append(key.left(64));
  entry.insert("parameters_truncated",
               names.size() > kMaximumParametersChanged);
  entry.insert("parameters_changed", QJsonArray::fromStringList(names.mid(
                                         0, kMaximumParametersChanged)));
}

QString nodeChange(const QJsonValue& before, const QJsonValue& after) {
  if (before.isNull()) return after.isNull() ? "transient" : "created";
  return after.isNull() ? "removed" : "changed";
}

QJsonObject nodeEntry(const QString& id, const QJsonValue& before,
                      const QJsonValue& after) {
  const QJsonObject old = before.toObject(), now = after.toObject();
  const bool both = !before.isNull() && !after.isNull();
  QJsonObject entry{
      {"kind", "node"},
      {"fx_id", id},
      {"change", nodeChange(before, after)},
      {"before", before.isNull() ? QJsonValue() : QJsonValue(publicNode(old))},
      {"after", after.isNull() ? QJsonValue() : QJsonValue(publicNode(now))},
      {"aspects", both ? aspects(old, now) : QJsonArray()}};
  if (both) {
    addParametersChanged(entry, old, now);
  } else {
    entry.insert("parameters_changed", QJsonArray());
    entry.insert("parameters_truncated", false);
  }
  return entry;
}

QString groupChange(const QJsonValue& before, const QJsonValue& after) {
  if (before.isNull()) return "created";
  if (after.isNull()) return "removed";
  return before.toObject().value("name") != after.toObject().value("name")
             ? "renamed"
             : "membership";
}

QJsonValue nameOf(const QJsonValue& state) {
  return state.isNull() ? QJsonValue() : state.toObject().value("name");
}

QJsonObject groupEntry(const QString& id, const QJsonValue& before,
                       const QJsonValue& after) {
  return {
      {"kind", "group"},
      {"group_id", id},
      {"change", groupChange(before, after)},
      {"name_before", nameOf(before)},
      {"name_after", nameOf(after)},
      {"member_count_before", before.toObject().value("member_count").toInt()},
      {"member_count_after", after.toObject().value("member_count").toInt()}};
}

QJsonArray handles(const QJsonValue& state) {
  return state.isArray() ? state.toArray() : QJsonArray();
}

QJsonObject entry(const QString& key, const QJsonValue& before,
                  const QJsonValue& after) {
  const QStringList parts = keyParts(key);
  switch (keyKind(key)) {
  case CurrentOutput:
    return {{"kind", "current_output"}, {"before", before}, {"after", after}};
  case Node:
    return nodeEntry(parts.value(1), before, after);
  case Edge:
    return {{"kind", "edge"},
            {"fx_id", parts.value(1)},
            {"port", parts.value(2)},
            {"before", before},
            {"after", after}};
  case Terminal:
    return {{"kind", "terminal"},
            {"fx_id", parts.value(1)},
            {"before", before.toBool()},
            {"after", after.toBool()}};
  case LinkedGroup:
    return {{"kind", "linked_group"},
            {"group_id", parts.value(1)},
            {"before_fx_ids", handles(before)},
            {"after_fx_ids", handles(after)}};
  case Group:
    return groupEntry(parts.value(1), before, after);
  }
  return {};
}

void collectStrings(const QJsonValue& value, QSet<QString>& strings) {
  if (value.isString()) strings.insert(value.toString());
  if (value.isArray())
    for (const QJsonValue& item : value.toArray())
      collectStrings(item, strings);
  if (value.isObject())
    for (const QJsonValue& item : value.toObject())
      collectStrings(item, strings);
}

bool requestedNode(const QString& tool, const QSet<QString>& named,
                   const QString& id, const QJsonObject& entry) {
  static const QStringList creators{"fx.create", "fx.duplicate",
                                    "fx.macro.create", "fx.macro.expand"};
  return named.contains(id) ||
         (entry.value("change") == "created" && creators.contains(tool));
}

// requested when the call's arguments name the entity, implicit otherwise.
QString cause(const QString& tool, const QSet<QString>& named,
              const QString& key, const QJsonObject& entry) {
  static const QStringList edgeTools{"fx.connect", "fx.disconnect",
                                     "fx.port_group.set"};
  const QString id = keyParts(key).value(1);
  bool requested   = false;
  switch (keyKind(key)) {
  case CurrentOutput:
    requested = tool == "fx.output.select";
    break;
  case Node:
    requested = requestedNode(tool, named, id, entry);
    break;
  case Edge:
    requested = edgeTools.contains(tool) && named.contains(id);
    break;
  case Terminal:
    requested = tool == "fx.terminal.set" && named.contains(id);
    break;
  case LinkedGroup:
    requested = tool.startsWith("fx.link.");
    break;
  case Group:
    requested = tool.startsWith("fx.group.");
    break;
  }
  return requested ? "requested" : "implicit";
}

std::vector<QString> ordered(const FxGraphJournal& journal,
                             const States& states) {
  std::vector<std::pair<Order, QString>> keys;
  for (const auto& entry : states) {
    const Order order =
        journal.order.value(entry.first, Order{keyKind(entry.first), 0, 0});
    keys.push_back({order, entry.first});
  }
  std::sort(keys.begin(), keys.end());
  std::vector<QString> result;
  for (const auto& entry : keys) result.push_back(entry.second);
  return result;
}

QJsonValue currentState(const FxGraphJournal& journal, const QString& key) {
  const auto found = journal.current.constFind(key);
  return found == journal.current.constEnd() ? QJsonValue() : found->state;
}

QJsonObject stepResult(const FxGraphJournal& journal, const Step& step,
                       const QString& tool, const QJsonObject& params) {
  QSet<QString> named;
  collectStrings(params, named);
  QJsonArray entries;
  int total = 0;
  for (const QString& key : ordered(journal, step.before)) {
    QJsonObject item =
        entry(key, step.before.at(key), currentState(journal, key));
    item.insert("cause", cause(tool, named, key, item));
    if (++total <= kMaximumStepEntries) entries.append(item);
  }
  return {{"from_revision", QString::number(step.revision - 1)},
          {"revision", QString::number(step.revision)},
          {"tool", tool},
          {"entries", entries},
          {"total_entries", total},
          {"truncated", total > kMaximumStepEntries},
          {"evidence_kind", "structural"}};
}

bool reportsDelta(const QString& tool) {
  static const QSet<QString> tools{"fx.create",        "fx.delete",
                                   "fx.connect",       "fx.disconnect",
                                   "fx.terminal.set",  "fx.output.select",
                                   "fx.parameter.set", "fx.parameter.set_key",
                                   "fx.rename",        "fx.enabled.set",
                                   "fx.duplicate",     "fx.preset.apply",
                                   "fx.macro.create",  "fx.macro.expand",
                                   "fx.group.create",  "fx.group.remove",
                                   "fx.group.rename",  "fx.layout.set",
                                   "fx.link.add",      "fx.link.remove",
                                   "fx.port_group.set"};
  return tools.contains(tool);
}

quint64 revisionParam(const QJsonObject& params, const QString& name) {
  bool valid           = false;
  const QString text   = requiredString(params, name);
  const quint64 number = text.toULongLong(&valid);
  if (!valid || text.size() > 20 || QString::number(number) != text)
    throw Error("invalid_params", name + " must be a decimal revision.",
                {{"field", name}});
  return number;
}

void requireTopSheet(Document& document, const QJsonObject& params) {
  const QJsonArray path = params.value("sheet_path").toArray();
  if (path.isEmpty()) return;
  resolveSheetPath(document, path);
  throw Error("delta_unavailable",
              "Deltas are journaled for the top sheet only; read fx.graph "
              "with this sheet_path instead.",
              {{"reason", "sheet_not_tracked"}});
}

// The before state of every entity changed after from_revision.
States netBefore(const FxGraphJournal& journal, quint64 from) {
  if (from < journal.baseRevision)
    throw Error(
        "delta_unavailable",
        "The journal no longer holds that revision; read fx.graph.",
        {{"reason", "window_exceeded"},
         {"oldest_from_revision", QString::number(journal.baseRevision)}});
  States before;
  for (const Step& step : journal.steps) {
    if (step.revision <= from) continue;
    if (step.overflow)
      throw Error("delta_unavailable",
                  "One write in the range changed too much to journal; read "
                  "fx.graph.",
                  {{"reason", "step_too_large"},
                   {"revision", QString::number(step.revision)}});
    for (const auto& entry : step.before) before.insert(entry);
  }
  return before;
}

QJsonArray netEntries(const FxGraphJournal& journal, const States& before) {
  QJsonArray entries;
  for (const QString& key : ordered(journal, before)) {
    const QJsonValue after = currentState(journal, key);
    const QJsonValue old   = before.at(key);
    const bool transient   = old.isNull() && after.isNull();
    if (old == after && !(transient && keyKind(key) == Node)) continue;
    QJsonObject item = entry(key, old, after);
    item.insert("cause", QJsonValue());
    entries.append(item);
    if (entries.size() > kMaximumNetEntries)
      throw Error("delta_unavailable",
                  "The net change is too large to page; read fx.graph.",
                  {{"reason", "delta_too_large"}});
  }
  return entries;
}

QJsonObject window(const FxGraphJournal& journal) {
  return {{"oldest_from_revision", QString::number(journal.baseRevision)},
          {"retained_steps", static_cast<int>(journal.steps.size())},
          {"retained_bytes", journal.bytes}};
}

}  // namespace

namespace {
// The document a write names, or null when it no longer exists (a discard)
// or is read-only.
Document* journaledDocument(Context& context, const QString& documentId) {
  if (documentId.isEmpty()) return nullptr;
  try {
    Document& document = context.document(documentId);
    return document.readOnly ? nullptr : &document;
  } catch (const Error& error) {
    if (error.code != "document_not_found") throw;
    return nullptr;
  }
}
}  // namespace

void prepareFxGraphJournal(Context& context, const QString& documentId) {
  if (Document* document = journaledDocument(context, documentId))
    journalOf(*document);
}

namespace {
#ifdef BUILD_UI_PORT_TESTS
bool verifyUnchangedSteps = true;
#endif

#ifdef BUILD_UI_PORT_TESTS
void requireUnchanged(Document& document, FxGraphJournal& journal,
                      const QString& tool) {
  const Entities after =
      Snapshotter(document, journal, &journal.current).take();
  if (!changedBefore(journal.current, after).empty())
    throw Error("internal_error",
                tool +
                    " changed the FX graph, but its effect declaration "
                    "says it cannot: set fxGraph in its row of "
                    "automation_effects.cpp.");
}
#endif

// A write whose effect declaration says it cannot change the FX graph keeps
// the journal's revisions continuous with an empty step and takes no
// snapshot. Test builds take the snapshot anyway and refuse a declaration
// that is wrong.
void recordUnchangedStep(Document& document, FxGraphJournal& journal,
                         const QString& tool) {
#ifdef BUILD_UI_PORT_TESTS
  if (verifyUnchangedSteps) requireUnchanged(document, journal, tool);
#else
  Q_UNUSED(tool);
#endif
  Step step;
  step.revision           = document.revision;
  journal.currentRevision = document.revision;
  retain(journal, std::move(step));
}

void resetIfDiscontinuous(FxGraphJournal& journal, quint64 revision) {
  if (journal.currentRevision + 1 == revision) return;
  journal.steps.clear();
  journal.bytes        = 0;
  journal.baseRevision = revision - 1;
}
}  // namespace

void recordFxGraphStep(Context& context, const QString& documentId,
                       const QString& tool, const QJsonObject& params,
                       bool advanced, QJsonObject* result) {
  Document* document = journaledDocument(context, documentId);
  if (!document) return;
  const bool reports      = result && reportsDelta(tool);
  const bool started      = document->fxJournal && document->fxJournal->started;
  FxGraphJournal& journal = journalOf(*document);
  if (!advanced || !started) {
    if (reports) result->insert("graph_delta", QJsonValue());
    return;
  }
  resetIfDiscontinuous(journal, document->revision);
  if (!changesFxGraph(tool)) {
    recordUnchangedStep(*document, journal, tool);
    return;
  }
  Step step;
  step.revision   = document->revision;
  Entities after  = Snapshotter(*document, journal, &journal.current).take();
  step.before     = changedBefore(journal.current, after);
  journal.current = std::move(after);
  journal.currentRevision = document->revision;
  const QJsonObject delta = stepResult(journal, step, tool, params);
  retain(journal, std::move(step));
  if (reports) result->insert("graph_delta", delta);
}

QJsonValue automationFxGraphDelta(Document& document,
                                  const QJsonObject& params) {
  requireTopSheet(document, params);
  FxGraphJournal& journal = journalOf(document);
  const quint64 from      = revisionParam(params, "from_revision");
  const int offset        = integerParam(params, "offset", 0, 65536, 0);
  const int limit         = integerParam(params, "limit", 1, 512, 256);
  if ((offset > 0 || params.contains("expected_revision")) &&
      (!params.contains("expected_revision") ||
       revisionParam(params, "expected_revision") != document.revision))
    throw Error("stale_revision",
                "The graph changed since the first page; start again at "
                "offset 0.",
                {{"revision", document.revisionText()}});
  if (from > document.revision)
    throw Error(
        "invalid_params", "from_revision is after the current revision.",
        {{"reason", "future_revision"}, {"revision", document.revisionText()}});
  const QJsonArray all = netEntries(journal, netBefore(journal, from));
  QJsonArray page;
  for (int index = offset; index < all.size() && page.size() < limit; ++index)
    page.append(all[index]);
  const int next = offset + page.size();
  return QJsonObject{
      {"document_id", document.id},
      {"from_revision", QString::number(from)},
      {"revision", document.revisionText()},
      {"sheet_path", params.value("sheet_path").toArray()},
      {"window", window(journal)},
      {"steps_covered", static_cast<int>(document.revision - from)},
      {"changes", page},
      {"total_changes", all.size()},
      {"offset", offset},
      {"limit", limit},
      {"next_offset", next < all.size() ? QJsonValue(next) : QJsonValue()},
      {"evidence_kind", "structural"}};
}

QJsonObject automationFxDeltaWindow(Document& document) {
  return window(journalOf(document));
}

quint64 automationFxSequence(Document& document, TFx* fx) {
  FxGraphJournal& journal = journalOf(document);
  const QString id        = document.idForFx(fx);
  const auto found        = journal.sequence.constFind(id);
  if (found != journal.sequence.constEnd()) return found.value();
  journal.sequence.insert(id, journal.nextSequence);
  return journal.nextSequence++;
}

QString automationFxGroupHandle(Document& document, int nativeGroupId) {
  if (!document.fxJournal)
    document.fxJournal = std::make_shared<FxGraphJournal>();
  FxGraphJournal& journal = *document.fxJournal;
  QString& handle         = journal.groupHandles[nativeGroupId];
  if (handle.isEmpty()) {
    handle = "fxgroup_" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    journal.groupIds[handle] = nativeGroupId;
  }
  return handle;
}

int automationFxGroupIdOrZero(Document& document, const QString& handle) {
  if (!document.fxJournal) return 0;
  const auto found = document.fxJournal->groupIds.find(handle);
  return found == document.fxJournal->groupIds.end() ? 0 : found->second;
}

QJsonObject automationFxGraphView(Document& document) {
  FxGraphJournal& journal = journalOf(document);
  QJsonObject nodes, linked, groups;
  const Entities entities =
      Snapshotter(document, journal, &journal.current).take();
  for (auto entry = entities.constBegin(); entry != entities.constEnd();
       ++entry) {
    const QStringList parts = keyParts(entry.key());
    const Kind kind         = keyKind(entry.key());
    if (kind == Node) nodes.insert(parts.value(1), entry->state);
    if (kind == LinkedGroup) linked.insert(parts.value(1), entry->state);
    if (kind == Group) groups.insert(parts.value(1), entry->state);
  }
  return {{"nodes", nodes}, {"linked_groups", linked}, {"groups", groups}};
}

QJsonValue automationFxLinkedGroup(Document& document, TFx* node) {
  FxGraphJournal& journal = journalOf(document);
  TFx* native             = nativeFx(node);
  if (native->getLinkedFx() == native) return QJsonValue();
  std::vector<std::pair<quint64, QString>> members;
  TFx* member = native;
  for (int guard = 0; guard < kMaximumRingMembers; ++guard) {
    TFx* owner = member;
    if (auto* zerary = dynamic_cast<TZeraryFx*>(member))
      if (zerary->getColumnFx()) owner = zerary->getColumnFx();
    members.push_back(
        {automationFxSequence(document, owner), document.idForFx(owner)});
    member = member->getLinkedFx();
    if (!member || member == native) break;
  }
  std::sort(members.begin(), members.end());
  QString& group = journal.linkHandles[members.front().second];
  if (group.isEmpty())
    group = "fxlink_" + QUuid::createUuid().toString(QUuid::WithoutBraces);
  QJsonArray ids;
  for (const auto& entry : members) ids.append(entry.second);
  return QJsonObject{{"group_id", group}, {"fx_ids", ids}};
}

QStringList automationFxDifferingParameters(TFx* first, TFx* second) {
  ParameterDigests a, b;
  addParameters(a, nativeFx(first), {});
  addParameters(b, nativeFx(second), {});
  QStringList names;
  for (size_t index = 0; index < a.size() && index < b.size(); ++index)
    if (a[index].first != b[index].first || a[index].second != b[index].second)
      names.append(a[index].first);
  return names;
}

#ifdef BUILD_UI_PORT_TESTS
void setFxJournalVerification(bool verify) { verifyUnchangedSteps = verify; }
#endif

}  // namespace GroundzeroAutomation
