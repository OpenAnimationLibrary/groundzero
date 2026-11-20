#pragma once

#include "tfilepath.h"
#include "tsound.h"

#include <QByteArray>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

#include <utility>
#include <vector>

class ToonzScene;
class TXshLevel;
class TXshSoundLevel;

namespace GroundzeroAutomation {
class Context;
struct Document;

constexpr qint64 AutomationAudioFileBytes = 160LL * 1024 * 1024;

// Validated layout of an admitted WAV or AIFF source.
struct AutomationAudioInfo final {
  QString container;
  qint64 sampleRate  = 0;
  int channels       = 0;
  int bitsPerSample  = 0;
  int sampleType     = 0;
  qint64 sampleCount = 0;
  qint64 pcmBytes    = 0;
  // SHA-256 of the little-endian interleaved samples the native reader yields.
  QByteArray pcmSha256;
};

// Validate a whole WAV or AIFF file before any native reader sees it.
//
// Args:
//   bytes: Complete file content, at most AutomationAudioFileBytes.
// Returns:
//   The admitted format, sample count and PCM digest.
// Throws:
//   Error: invalid_asset for corrupt files, unsupported_format for audio
//   outside the admitted codecs, resource_limit for size or duration.
AutomationAudioInfo parseAutomationAudio(const QByteArray& bytes);

// Return the SHA-256 of a native track's interleaved sample memory.
QByteArray automationTrackSha256(const TSoundTrackP& track);

// Return strict descriptors for the detached audio operations.
QJsonArray audioOperations();

// Execute a revision-bound audio operation after dispatcher validation.
//
// Args:
//   context: Worker ownership, path permissions and mutation tracking.
//   method: Registered audio operation name.
//   params: Strict-schema arguments including the document revision.
// Returns:
//   The operation's bounded result; the dispatcher adds the receipt fields.
// Throws:
//   Error: Invalid placement, timing, locks, limits, files or native failures.
QJsonValue executeAudio(Context& context, const QString& method,
                        const QJsonObject& params);

// Require the sound sources and columns of a scene to fit the worker profile
// that save, render and reopen admit.
void validateAutomationAudio(ToonzScene& scene);

// PCM digest of a document's sound source, as audio.inspect reports it.
//
// Throws:
//   Error: invalid_params when the level is not a sound source.
QString automationSoundPcmSha256(Document& document, TXshLevel* level);

// Decoded PCM bytes a sound source adds to the document asset estimate.
qint64 automationSoundBytes(TXshSoundLevel& level);

// Whether any sound column of the scene contains a clip.
bool automationSceneHasSoundClips(ToonzScene& scene);

// Give every sound source the scene frame rate after a rate change.
void setAutomationSoundFrameRate(ToonzScene& scene, double fps);

// Exclude every sound column from a private render snapshot's mix.
void muteAutomationSound(ToonzScene& scene);

// Copy each sound source byte-exactly into a package being written.
//
// Args:
//   scene: Admitted scene whose sources are verified against their PCM.
//   directory: Package staging directory with an existing assets folder.
// Returns:
//   The sounds, sound_columns and frame_rate manifest keys, or an empty
//   object when the scene has no sound.
QJsonObject writeAutomationSounds(ToonzScene& scene, const QString& directory);

// Where writeAutomationSounds copies the sound source at cast index `index`,
// relative to the package directory.
QString automationSoundPackagePath(int index, const TXshSoundLevel& level);

// List the sound sources whose private copy a render snapshot could not read.
//
// A snapshot copies every sound source of the scene, exposed or not, after
// verifying it against its decoded track.
//
// Args:
//   scene: Admitted scene.
// Returns:
//   One {kind: "sound", name, code} row per missing, unreadable or changed
//   source, at most 64.
QJsonArray missingAutomationSounds(ToonzScene& scene);

// Decode and compare a reopened package's sound with its manifest.
//
// Throws:
//   Error: invalid_package when any source, column, clip or rate differs.
void reopenAutomationSounds(ToonzScene& scene, const QJsonObject& manifest);

// Copy a reopened scene's sound sources into private worker storage, so the
// document stops depending on its package files.
//
// Returns:
//   Verified file metadata for Document::soundSources, keyed by cast level.
QHash<TXshLevel*, QJsonObject> adoptAutomationSounds(Context& context,
                                                     ToonzScene& scene);

// Point sound sources at their package assets while a scene file is written.
class AutomationSoundPaths final {
public:
  explicit AutomationSoundPaths(ToonzScene& scene);
  ~AutomationSoundPaths();
  AutomationSoundPaths(const AutomationSoundPaths&)            = delete;
  AutomationSoundPaths& operator=(const AutomationSoundPaths&) = delete;

private:
  std::vector<std::pair<TXshSoundLevel*, TFilePath>> m_paths;
};

#ifdef BUILD_UI_PORT_TESTS
void runAutomationAudioTests(Context& context);
#endif
}  // namespace GroundzeroAutomation
