#pragma once

#include "cadmiumconsensus.h"
#include "tfilepath.h"

#include <QSize>

namespace Cadmium {

constexpr qint64 MaximumSessionBytes = 16LL * 1024 * 1024;
constexpr int MaximumSessionDrawings = 512;

struct SessionReference {
  TFrameId fid;
  QByteArray fingerprint;
  int regionCount = 0;
};

struct SessionDrawing {
  TFrameId fid;
  QByteArray targetFingerprint;
  QByteArray labelsFingerprint;
  QSize canvas = QSize(0, 0);
  QVector<SessionReference> references;
  QVector<RegionProposal> regions;
  bool ready = false;
  bool reviewed = false;
  bool applied = false;
};

struct SessionOptions {
  RasterSource source = RasterSource::TransparentLines;
  double paperThreshold = 0.92;
  double lineThreshold = 0.2;
  bool extractionApproved = false;
  bool repaint = false;
  QString lineLevelId;
  QByteArray lineSourceKey;
};

struct Session {
  QString levelId;
  Kind kind = Kind::ToonzRaster;
  SessionOptions options;
  QVector<SessionDrawing> drawings;
  int selectedDrawing = -1;
};

QByteArray sessionOptionsDigest(const SessionOptions &);
QByteArray sessionLabelsDigest(const Frame &);
bool validateSession(const Session &, QString &error);
bool validateSessionAgainstFrames(const SessionDrawing &, const Frame &target,
                                  const QVector<Frame> &references, Kind,
                                  QString &error);

// A saved scene and a stable level identity select one private cache entry.
// The application skips disk persistence while the scene is untitled.
QString sessionFilePath(const QString &cacheRoot, const QString &scenePath,
                        const QString &levelId);
bool saveSession(const QString &cacheRoot, const QString &scenePath,
                 const Session &, QString &error);
bool loadSession(const QString &cacheRoot, const QString &scenePath,
                 const QString &levelId, Session &, QString &error);
bool removeSession(const QString &cacheRoot, const QString &scenePath,
                   const QString &levelId, QString &error);
bool copySession(const QString &cacheRoot, const QString &oldScenePath,
                 const QString &newScenePath, const QString &levelId,
                 QString &error);

// A source digest discovers first-use sessions whose new level identity has
// not yet been written into the scene. Artwork is still validated on restore.
QString sessionAliasPath(const QString &cacheRoot, const QString &scenePath,
                         const QByteArray &sourceKey);
bool saveSessionWithAlias(const QString &cacheRoot, const QString &scenePath,
                          const QByteArray &sourceKey, const Session &,
                          QString &error);
bool loadSessionWithAlias(const QString &cacheRoot, const QString &scenePath,
                          const QString &currentLevelId, const QByteArray &sourceKey,
                          Session &, QString &error);
bool removeSessionWithAlias(const QString &cacheRoot, const QString &scenePath,
                            const QString &currentLevelId, const QByteArray &sourceKey,
                            QString &error);

}  // namespace Cadmium
