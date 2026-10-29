#pragma once

#include "trasterfx.h"
#include "trasterfxrenderdata.h"
#include "tseexpr.h"

#include <algorithm>
#include <iomanip>
#include <locale>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// Immutable render-local host attributes. No expression can read a scene,
// GUI object, process environment or external renderer through this interface.
namespace TSeExprAttributes {
using Values = std::map<std::string, TSeExpr::Value>;

inline bool first(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
inline bool next(char c) { return first(c) || (c >= '0' && c <= '9') || c == '.'; }
inline bool identifier(const std::string &value) {
  return !value.empty() && value.size() <= 64 && first(value[0]) &&
         std::all_of(value.begin() + 1, value.end(), next);
}
inline std::string name(const std::string &value) {
  const auto separator = value.find("::");
  if (separator == std::string::npos ||
      !identifier(value.substr(0, separator)) ||
      !identifier(value.substr(separator + 2)))
    throw std::runtime_error("SeExpr attributes require namespace::name identifiers");
  return value;
}
inline void validate(const Values &values) {
  if (values.size() > 512)
    throw std::runtime_error("SeExpr attribute snapshot exceeds 512 bindings");
  for (const auto &entry : values) {
    name(entry.first);
    const auto &value = entry.second;
    if (!value.isFinite() ||
        (value.type != TSeExpr::Type::Scalar && value.type != TSeExpr::Type::Vector) ||
        value.number.size() != (value.type == TSeExpr::Type::Scalar ? 1u : 3u))
      throw std::runtime_error("SeExpr attributes require finite scalar or three-component values");
  }
}

struct Snapshot {
  Values options, attributes;

  std::string identity() const {
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << "seexpr-attributes-v1" << std::setprecision(17);
    for (const auto *table : {&attributes, &options}) {
      stream << '[';
      for (const auto &entry : *table) {
        stream << entry.first.size() << ':' << entry.first << '='
               << (entry.second.type == TSeExpr::Type::Scalar ? 's' : 'v');
        for (double component : entry.second.number) stream << ',' << component;
        stream << ';';
      }
      stream << ']';
    }
    return stream.str();
  }
};

class RenderData final : public TRasterFxRenderData {
public:
  const Snapshot values;
  explicit RenderData(Snapshot snapshot) : values(std::move(snapshot)) {
    validate(values.options);
    validate(values.attributes);
  }
  bool operator==(const TRasterFxRenderData &other) const override {
    const auto *data = dynamic_cast<const RenderData *>(&other);
    return data && values.identity() == data->values.identity();
  }
  std::string toString() const override { return values.identity(); }
  float typeIndex() const override { return 470.0f; }
};

inline Snapshot snapshot(const TRenderSettings &settings,
                         const Values &explicitOptions = {}) {
  Snapshot result;
  // Traversal appends nearer providers. Assignment preserves that precedence
  // without retaining the render-data object or any native parameter owner.
  for (const auto &item : settings.m_data) {
    const auto *data = dynamic_cast<const RenderData *>(item.getPointer());
    if (!data) continue;
    for (const auto &entry : data->values.attributes)
      result.attributes[entry.first] = entry.second;
    for (const auto &entry : data->values.options)
      result.options[entry.first] = entry.second;
  }
  for (const auto &entry : explicitOptions) result.options[entry.first] = entry.second;
  validate(result.attributes);
  validate(result.options);
  return result;
}

inline std::set<std::string> referencedNames(const std::vector<std::string> &sources) {
  std::set<std::string> result;
  for (const auto &source : sources) {
    char quote = 0;
    bool escaped = false, comment = false;
    for (std::size_t index = 0; index < source.size(); ++index) {
      const char c = source[index];
      if (comment) { if (c == '\n') comment = false; continue; }
      if (quote) {
        if (escaped) escaped = false;
        else if (c == '\\') escaped = true;
        else if (c == quote) quote = 0;
        continue;
      }
      if (c == '#') { comment = true; continue; }
      if (c == '"' || c == '\'') { quote = c; continue; }
      if (c != '$' || index + 1 >= source.size() || !first(source[index + 1])) continue;
      const std::size_t start = index + 1;
      std::size_t end = start + 1;
      while (end < source.size() && next(source[end])) ++end;
      if (end + 2 >= source.size() || source.compare(end, 2, "::") || !first(source[end + 2])) continue;
      end += 3;
      while (end < source.size() && next(source[end])) ++end;
      result.insert(name(source.substr(start, end - start)));
      index = end - 1;
    }
  }
  if (result.size() > 512)
    throw std::runtime_error("SeExpr expression exceeds 512 namespaced attributes");
  return result;
}

inline void appendVariables(std::vector<TSeExpr::Variable> &variables,
                            const Snapshot &snapshot,
                            const std::vector<std::string> &sources) {
  for (const auto &key : referencedNames(sources)) {
    auto attribute = snapshot.attributes.find(key);
    auto option = snapshot.options.find(key);
    TSeExpr::Value value = attribute != snapshot.attributes.end() ? attribute->second
        : option != snapshot.options.end() ? option->second
        : TSeExpr::Value::vector(0, 0, 0);
    // Renderer float attributes are greyscale tuples in the upstream adapter,
    // including indexed access to all three components.
    if (value.type == TSeExpr::Type::Scalar)
      value = TSeExpr::Value::vector(value.number[0], value.number[0], value.number[0]);
    const auto existing = std::find_if(variables.begin(), variables.end(),
        [&](const TSeExpr::Variable &variable) { return variable.name == key; });
    if (existing == variables.end()) variables.push_back({key, std::move(value)});
    else existing->value = std::move(value);
  }
}
}  // namespace TSeExprAttributes
