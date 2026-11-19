#pragma once

#include "automation.h"

#include "tfilepath.h"

#include <QJsonArray>
#include <QJsonObject>

#include <map>
#include <vector>

class TFx;
class TPalette;
class TXshColumn;
class TXshLevel;

namespace GroundzeroAutomation {

// Editable import of a user scene (contract w2-drafts-receipts section 6,
// amended by w2-persistence section 9). The plan classifies a read-only
// document against the editable profile in its scene host: every item the
// profile cannot hold falls into one loss kind or blocks the import, and
// nothing is decoded or written. The import job applies the same
// classification to a fresh snapshot and writes a verified package.

constexpr int ImportClassifierVersion = 1;

// One classified item: the native object the loss acts on, its handles in the
// classified document, and what the plan says about it.
struct ImportItem final {
  TXshLevel* level   = nullptr;
  TXshColumn* column = nullptr;
  TFx* fx            = nullptr;
  // A palette style (style_not_admitted) or -1.
  int style = -1;
  // The drawing an exposure names (exposure_not_admitted).
  TFrameId frame;
  QString levelId;
  QString columnId;
  QString fxId;
  QJsonArray sheetPath;
  QString name;
  QString detail;
};

// A loss kind: its effect and every item it acts on, in classification order.
struct ImportLoss final {
  QString effect;
  std::vector<ImportItem> items;
};

struct ImportPlan final {
  std::map<QString, ImportLoss> losses;
  std::vector<ImportItem> unclassified;
  // The first editable bound the kept content breaks, or empty.
  QString blockedLimit;
  qint64 blockedValue   = 0;
  qint64 blockedMaximum = 0;
  std::map<QString, int> conversions;
  QJsonObject estimate;

  bool importable() const {
    return blockedLimit.isEmpty() && unclassified.empty();
  }
};

// Replace every palette style the editable profile refuses (not a plain
// solid style, linked to a studio palette, or animated through such a style)
// with an unlinked solid style of its main colour and name; returns their ids.
std::vector<int> admitImportStyles(TPalette* palette);
// Classify a read-only document whose levels its host has loaded.
ImportPlan classifyImport(Document& document);
// "plan:" + SHA-256 of the canonical plan with every item's handles.
QString importPlanDigest(const ImportPlan& plan, const QString& fingerprint);
// The plan with names and details only, for comparing two classifications of
// the same source made through different handles.
QByteArray importPlanShape(const ImportPlan& plan);

QJsonArray importOperations();
QJsonValue executeImport(Context& context, const QString& method,
                         const QJsonObject& params);

#ifdef BUILD_UI_PORT_TESTS
void runAutomationImportTests(Context& context);
#endif

}  // namespace GroundzeroAutomation
