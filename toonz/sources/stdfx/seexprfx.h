#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <string>
#include <vector>

namespace SeExprFx {
struct Control {
  QString id, name, label, type;
  QJsonObject definition;
  bool active = true;
};
struct Document {
  QJsonObject json;
  QString rgb, alpha;
  QString backend;
  std::vector<Control> controls;
};
Document parseDocument(const std::wstring &text);
std::wstring initialDocument(bool generator);
}  // namespace SeExprFx
