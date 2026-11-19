#pragma once

#include <QJsonObject>
#include <QString>

class ToonzScene;
class TXshSimpleLevel;

namespace GroundzeroAutomation {

class Context;

/// Record what the native saver writes for the top sheet of a scene.
///
/// The record has three sections, each a JSON object: `sheet` (saved columns
/// with their flags, parent and a digest of their cell runs), `cameras` (every
/// camera in native index order with its roles and a curve digest) and
/// `scene_properties` (output and preview blocks, background, field guide,
/// markers, ruler guides and column filters).
///
/// Args:
///   scene: The scene as it is about to be saved, or as it was reopened.
///
/// Returns:
///   An object keyed by section name.
QJsonObject automationSheetRecord(ToonzScene& scene);

/// Record the interpretation settings of one level as the saver writes them.
QJsonObject automationLevelSettings(TXshSimpleLevel& level);

/// Require a reopened scene to carry the sheet record that was saved.
///
/// Args:
///   expected: The manifest holding the saved sections.
///   reopened: The scene loaded back from the package.
///
/// Throws:
///   Error: `invalid_package` naming the first differing section and path.
void requireAutomationSheetRecord(const QJsonObject& expected,
                                  ToonzScene& reopened);

/// Require two records of one section to be equal.
///
/// Throws:
///   Error: `invalid_package` with `details.section` and `details.path`.
void requireEqualRecord(const QString& section, const QJsonValue& expected,
                        const QJsonValue& actual);

/// Run the sheet record domain tests.
void runAutomationSheetRecordTests(Context& context);

}  // namespace GroundzeroAutomation
