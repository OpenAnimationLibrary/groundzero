#include "automation.h"
#include "automation_fx.h"
#include "automation_fx_journal.h"
#include "automation_scene_host.h"

#include "tconst.h"
#include "tdoublekeyframe.h"
#include "tdoubleparam.h"
#include "tfx.h"
#include "tfxattributes.h"
#include "tmacrofx.h"
#include "tparamcontainer.h"
#include "tparamset.h"
#include "toonz/fxdag.h"
#include "toonz/tcolumnfx.h"
#include "toonz/tcolumnfxset.h"
#include "toonz/toonzscene.h"
#include "toonz/txsheet.h"
#include "toonz/txshcolumn.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QUuid>

#include <algorithm>
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

}  // namespace

class FxGraphJournal final {
public:
  bool started            = false;
  quint64 baseRevision    = 0;
  quint64 currentRevision = 0;
  States current;
  std::deque<Step> steps;
  qint64 bytes = 0;
  std::map<QString, quint64> sequence;
  quint64 nextSequence = 1;
  std::map<QString, Order> order;
  std::map<int, QString> groupHandles;
  std::map<QString, int> groupIds;
  std::map<QString, QString> linkHandles;
};

namespace {

void addBytes(QCryptographicHash& hash, const QByteArray& bytes) {
  const qint64 size = bytes.size();
  hash.addData(reinterpret_cast<const char*>(&size), sizeof(size));
  hash.addData(bytes);
}

void addNumber(QCryptographicHash& hash, double value) {
  hash.addData(reinterpret_cast<const char*>(&value), sizeof(value));
}

void addKeyframe(QCryptographicHash& hash, const TDoubleKeyframe& key) {
  for (double value :
       {double(key.m_type), double(key.m_prevType), key.m_frame, key.m_value,
        double(key.m_isKeyframe), double(key.m_step), key.m_speedIn.x,
        key.m_speedIn.y, key.m_speedOut.x, key.m_speedOut.y,
        double(key.m_linkedHandles), double(key.m_fileParams.m_fieldIndex),
        key.m_similarShapeOffset})
    addNumber(hash, value);
  addBytes(hash, QByteArray::fromStdString(key.m_expressionText));
  addBytes(hash, QByteArray::fromStdString(key.m_unitName));
  addBytes(hash,
           QString::fromStdWString(key.m_fileParams.m_path.getWideString())
               .toUtf8());
}

// Every value and key field of a parameter, so that equal digests mean equal
// parameters.
void addParameter(QCryptographicHash& hash, TParam* param, int depth) {
  if (depth > 16)
    throw Error("resource_limit", "Parameter nesting is too deep.");
  if (auto* number = dynamic_cast<TDoubleParam*>(param)) {
    addNumber(hash, number->getDefaultValue());
    for (int index = 0; index < number->getKeyframeCount(); ++index)
      addKeyframe(hash, number->getKeyframe(index));
    return;
  }
  if (auto* set = dynamic_cast<TParamSet*>(param)) {
    for (int index = 0; index < set->getParamCount(); ++index)
      addParameter(hash, set->getParam(index).getPointer(), depth + 1);
    return;
  }
  std::set<double> frames;
  param->getKeyframes(frames);
  frames.insert(0);
  for (double frame : frames) {
    addNumber(hash, frame);
    addBytes(hash, QByteArray::fromStdString(param->getValueAlias(frame, 17)));
  }
}

QString parameterDigest(TParam* param) {
  QCryptographicHash hash(QCryptographicHash::Sha256);
  addParameter(hash, param, 0);
  return QString::fromLatin1(hash.result().left(16).toHex());
}

void addParameters(QJsonObject& digests, TFx* fx, const QString& prefix) {
  TParamContainer* params = fx->getParams();
  for (int index = 0; index < params->getParamCount(); ++index) {
    const TParamVar* entry = params->getParamVar(index);
    if (entry->isHidden() || entry->isObsolete()) continue;
    digests.insert(prefix + QString::fromStdString(entry->getName()),
                   parameterDigest(entry->getParam()));
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

class Snapshotter final {
public:
  Snapshotter(Document& document, FxGraphJournal& journal)
      : m_document(document)
      , m_journal(journal)
      , m_sheet(document.scene->getXsheet())
      , m_graph(m_sheet->getFxDag()) {
    for (auto it = document.effects.constBegin();
         it != document.effects.constEnd(); ++it)
      m_handles.insert({it.value(), it.key()});
  }

  States take() {
    const std::vector<TFx*> nodes = sheetNodes(m_sheet);
    assignSequences(nodes);
    for (TFx* fx : nodes) m_byNative.insert({nativeFx(fx), fx});
    for (TFx* fx : nodes) addNode(fx);
    TFx* output = m_graph->getCurrentOutputFx();
    if (output) put(entityKey(CurrentOutput), handle(output), {});
    addGroups();
    return std::move(m_states);
  }

  QString handle(TFx* fx) {
    const auto found = m_handles.find(fx);
    if (found != m_handles.end()) return found->second;
    const QString id = m_document.idForFx(fx);
    m_handles.insert({fx, id});
    return id;
  }

  QJsonValue linkedGroup(TFx* node) {
    const QStringList members = ringMembers(node);
    if (members.size() < 2) return QJsonValue();
    return linkHandle(members.first());
  }

  QStringList ringMembers(TFx* node) {
    TFx* native = nativeFx(node);
    std::vector<std::pair<quint64, QString>> members;
    TFx* member = native;
    for (int guard = 0; guard < kMaximumRingMembers; ++guard) {
      const auto owner = m_byNative.find(member);
      if (owner != m_byNative.end()) {
        const QString id = handle(owner->second);
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

private:
  quint64 sequenceOf(const QString& id) {
    const auto found = m_journal.sequence.find(id);
    return found == m_journal.sequence.end()
               ? std::numeric_limits<quint64>::max()
               : found->second;
  }

  void assignSequences(const std::vector<TFx*>& nodes) {
    std::vector<std::pair<std::tuple<int, int, std::wstring>, TFx*>> fresh;
    for (TFx* fx : nodes)
      if (!m_journal.sequence.count(handle(fx)))
        fresh.push_back({seenOrder(m_graph, fx), fx});
    std::sort(fresh.begin(), fresh.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& entry : fresh)
      m_journal.sequence[handle(entry.second)] = m_journal.nextSequence++;
  }

  void put(const QString& key, const QJsonValue& state, const Order& order) {
    m_states[key]        = state;
    m_journal.order[key] = order;
  }

  QString linkHandle(const QString& fxHandle) {
    QString& link = m_journal.linkHandles[fxHandle];
    if (link.isEmpty())
      link = "fxlink_" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    return link;
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

  QJsonObject parameters(TFx* fx, QJsonArray& members) {
    QJsonObject digests;
    if (auto* macro = dynamic_cast<TMacroFx*>(fx)) {
      for (const auto& member : macro->getFxs()) {
        const QString key = QString::fromStdWString(member->getFxId());
        members.append(key + ":" + QString::fromStdString(member->getFxType()));
        addParameters(digests, member.getPointer(), key + "/");
      }
      return digests;
    }
    addParameters(digests, nativeFx(fx), {});
    return digests;
  }

  void addNode(TFx* fx) {
    const QString id    = handle(fx);
    const quint64 order = m_journal.sequence[id];
    TFx* native         = nativeFx(fx);
    const bool terminal = m_graph->getTerminalFxs()->containsFx(fx);
    QJsonArray portNames, members;
    for (int index = 0; index < fx->getInputPortCount(); ++index) {
      const QString port = QString::fromStdString(fx->getInputPortName(index));
      portNames.append(port);
      TFx* source = fx->getInputPort(index)->getFx();
      if (source)
        put(entityKey(Edge, id, port), handle(source), {Edge, order, index});
    }
    if (terminal) put(entityKey(Terminal, id), true, {Terminal, order, 0});
    const QJsonObject digests = parameters(fx, members);
    put(entityKey(Node, id),
        QJsonObject{{"type", QString::fromStdString(native->getFxType())},
                    {"name", QString::fromStdWString(native->getName())},
                    {"enabled", fx->getAttributes()->isEnabled() &&
                                    native->getAttributes()->isEnabled()},
                    {"terminal", terminal},
                    {"generator_wrapper",
                     dynamic_cast<TZeraryColumnFx*>(fx) != nullptr},
                    {"macro", dynamic_cast<TMacroFx*>(fx) != nullptr},
                    {"position", position(fx)},
                    {"group_ids", groupIds(fx)},
                    {"linked_group_id", linkedGroup(fx)},
                    {"port_names", portNames},
                    {"parameters", digests},
                    {"macro_members", members}},
        {Node, order, 0});
    addLinkedGroup(fx);
  }

  void addLinkedGroup(TFx* fx) {
    const QStringList members = ringMembers(fx);
    if (members.size() < 2 || members.first() != handle(fx)) return;
    put(entityKey(LinkedGroup, linkHandle(members.first())),
        QJsonArray::fromStringList(members),
        {LinkedGroup, m_journal.sequence[members.first()], 0});
  }

  void addGroups() {
    struct Seen final {
      QString name;
      int members   = 0;
      quint64 first = std::numeric_limits<quint64>::max();
    };
    std::map<int, Seen> groups;
    for (TFx* fx : sheetNodes(m_sheet)) {
      const quint64 order   = m_journal.sequence[handle(fx)];
      const QStack<int> ids = fx->getAttributes()->getGroupIdStack();
      const QStack<std::wstring> names =
          fx->getAttributes()->getGroupNameStack();
      for (int index = 0; index < ids.size(); ++index) {
        Seen& seen = groups[ids[index]];
        ++seen.members;
        if (order >= seen.first) continue;
        seen.first = order;
        seen.name  = QString::fromStdWString(names.value(index));
      }
    }
    for (const auto& entry : groups)
      put(entityKey(Group, automationFxGroupHandle(m_document, entry.first)),
          QJsonObject{{"name", entry.second.name},
                      {"member_count", entry.second.members}},
          {Group, entry.second.first, entry.first});
  }

  Document& m_document;
  FxGraphJournal& m_journal;
  TXsheet* m_sheet;
  FxDag* m_graph;
  std::map<TFx*, QString> m_handles;
  std::map<TFx*, TFx*> m_byNative;
  States m_states;
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

// The states that differ between two snapshots, with their state before.
States changedBefore(const States& before, const States& after) {
  States changed;
  for (const auto& entry : before) {
    const auto found = after.find(entry.first);
    if (found == after.end() || found->second != entry.second)
      changed.insert(entry);
  }
  for (const auto& entry : after)
    if (!before.count(entry.first)) changed.insert({entry.first, QJsonValue()});
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
    const auto found  = journal.order.find(entry.first);
    const Order order = found == journal.order.end()
                            ? Order{keyKind(entry.first), 0, 0}
                            : found->second;
    keys.push_back({order, entry.first});
  }
  std::sort(keys.begin(), keys.end());
  std::vector<QString> result;
  for (const auto& entry : keys) result.push_back(entry.second);
  return result;
}

QJsonValue currentState(const FxGraphJournal& journal, const QString& key) {
  const auto found = journal.current.find(key);
  return found == journal.current.end() ? QJsonValue() : found->second;
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
  Step step;
  step.revision = document->revision;
  States after  = Snapshotter(*document, journal).take();
  step.before   = changedBefore(journal.current, after);
  if (journal.currentRevision + 1 != document->revision) {
    journal.steps.clear();
    journal.bytes        = 0;
    journal.baseRevision = document->revision - 1;
  }
  journal.current         = std::move(after);
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
  auto found              = journal.sequence.find(id);
  if (found != journal.sequence.end()) return found->second;
  return journal.sequence[id] = journal.nextSequence++;
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
  for (const auto& entry : Snapshotter(document, journal).take()) {
    const QStringList parts = keyParts(entry.first);
    const Kind kind         = keyKind(entry.first);
    if (kind == Node) nodes.insert(parts.value(1), entry.second);
    if (kind == LinkedGroup) linked.insert(parts.value(1), entry.second);
    if (kind == Group) groups.insert(parts.value(1), entry.second);
  }
  return {{"nodes", nodes}, {"linked_groups", linked}, {"groups", groups}};
}

}  // namespace GroundzeroAutomation
