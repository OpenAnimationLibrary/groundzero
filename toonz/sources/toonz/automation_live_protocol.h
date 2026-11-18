#pragma once

#include "automation_live.h"

namespace GroundzeroAutomation {
namespace Live {

constexpr int maximumRequestBytes  = 64 * 1024;
constexpr int maximumResponseBytes = 1024 * 1024;
constexpr int maximumReceiptBytes  = 4096;

// Parse one complete UTF-8 JSON object, rejecting duplicate keys and depth >16.
QJsonObject parseMessage(const QByteArray& bytes, int maximumBytes);
// Throw invalid_params when any object in a parsed JSON text repeats a key.
// The caller has already parsed `bytes` as valid JSON.
void rejectDuplicateKeys(const QByteArray& bytes);
// Encode one object with the four-byte unsigned big-endian length prefix.
QByteArray frameMessage(const QJsonObject& value, int maximumBytes);
void validateRequest(const QJsonObject& request);
void validateParams(const QString& method, const QJsonObject& params);
bool isWrite(const QString& method);
QJsonObject capabilities(const QString& mode);
QJsonObject liveError(const Error& error);

#ifdef BUILD_UI_PORT_TESTS
void runAutomationLiveProtocolTests();
void runAutomationLiveRaceTests();
#endif

}  // namespace Live
}  // namespace GroundzeroAutomation
