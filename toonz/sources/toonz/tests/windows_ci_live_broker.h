#pragma once

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <QJsonObject>
#include <QString>

namespace GroundzeroWindowsLiveTest {

// Hosted-only test setup. Never reads grant bytes or changes the live grant
// DACL.
QJsonObject createLiveReparseFixtures(const QString& stageRoot,
                                      const QString& fixtureRoot,
                                      HANDLE ownerToken);

}  // namespace GroundzeroWindowsLiveTest
#endif
