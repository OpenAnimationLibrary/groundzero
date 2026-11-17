#pragma once

#include <QJsonArray>
#include <QString>

void prepareMcpLivePrivateDirectory(const QString& path);
QJsonArray createMcpLiveInsecureFiles(const QString& sessionPath);
void removeMcpLiveInsecureFiles();
