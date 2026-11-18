#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <set>

class TFx;
class TMacroFx;
class ToonzScene;

namespace GroundzeroAutomation {
class Context;
struct Document;

// Validate bounded native macro ownership without modifying scene state.
void validateAutomationMacros(ToonzScene& scene,
                              const std::set<TFx*>& topLevel);
// Rebuild missing native member indexes only on a validated private loaded
// scene.
void indexAutomationMacroMembers(ToonzScene& scene);
void validateAutomationMacroCandidate(ToonzScene& scene, TMacroFx* macro,
                                      const std::set<TFx*>& replaced);
QJsonObject describeAutomationMacro(Document& document, TMacroFx* macro);
QByteArray automationMacroFingerprint(ToonzScene& scene,
                                      const QString& scratchRoot);
QJsonArray macroOperations();
QJsonValue executeMacro(Context& context, Document& document,
                        const QString& method, const QJsonObject& params);

#ifdef BUILD_UI_PORT_TESTS
// Test-only: end the process, as a crash would, while a macro roundtrip
// verification holds its scratch folder.
void setMacroVerifyCrash(bool armed);
void setAutomationMacroFailurePoint(int point);
void runAutomationMacroTests(Context& context);
#endif
}  // namespace GroundzeroAutomation
