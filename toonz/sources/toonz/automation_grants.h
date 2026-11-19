#pragma once

#include "automation.h"

#include <QJsonObject>
#include <QStringList>

namespace GroundzeroAutomation {

// The value of --grants: a comma-separated subset of inspect, draft, file and
// job that contains inspect. Returns it sorted; invalid_argument otherwise.
QStringList parseGrants(const QString& value);

// The value of --domains: toolset names of this binary, or all for every one.
// Returns them sorted with core; invalid_argument for an unknown, repeated or
// mixed name.
QStringList parseDomains(const QString& value, const QStringList& toolsets);

// automation.capabilities.authority: the launch authority of this process and
// the SHA-256 of its canonical compact JSON.
QJsonObject authorityReport(const Context& context);

// Refuse with grant_denied, before anything is recorded, a call whose tool
// needs a grant class or toolset this session was not granted. The classes a
// tool needs are its effect declaration's grants; discarding a read-only
// document needs no draft.
void requireGrants(Context& context, const QJsonObject& descriptor,
                   const QJsonObject& params);

}  // namespace GroundzeroAutomation
