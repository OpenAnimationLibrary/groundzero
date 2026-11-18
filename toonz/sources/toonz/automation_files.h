#pragma once

#include <QDateTime>
#include <QString>
#include <QTemporaryDir>
#include <QStringList>
#include <QVector>

class QFile;

namespace GroundzeroAutomation {
class Context;
}

namespace GroundzeroAutomation {

// Opening, listing and publishing files at paths that Context::resolvePath or
// a shared root walk approved, without trusting those paths a second time. A
// path that was approved can be replaced by a link before it is used, so the
// directory that holds the file is opened by descriptor (a handle on Windows),
// proven to be the approved directory, and only then is the file opened,
// listed or published relative to it. Where the platform cannot do that, the
// call fails; it never falls back to a path-based open.

enum class OpenResult { Opened, NotRegular, Failed };

// Open the regular file at an approved absolute path for reading.
//
// The directory is opened without following links and must still be the
// approved directory; the name is opened without following a link and without
// blocking, so a named pipe or a device is reported as NotRegular.
//
// Throws:
//   Error: path_denied when a component of the path was redirected.
OpenResult openApprovedFile(QFile& file, const QString& path);

enum class PublishResult { Published, Exists, Unsupported, Failed };

// Move a staged file or directory to the approved, not yet existing path.
//
// The destination directory is pinned as for openApprovedFile and nothing is
// ever replaced. The move is one atomic create-if-absent step: an exclusive
// rename where the platform has one (macOS, Linux), otherwise, for a single
// file, a hard link of the finished file followed by dropping the staged name.
// A destination that appears at any moment yields Exists. Where neither
// primitive is available on the destination's file system (and always for a
// folder without an exclusive rename) the result is Unsupported and nothing
// is written; there is no check-then-rename fallback. Failed covers every other
// reason: another volume, no permission, no space.
//
// Throws:
//   Error: path_denied when a component of the path was redirected.
PublishResult publishApproved(const QString& staged, const QString& path);

// Record the publication as started and publish, or throw: failureCode when
// the destination exists or the move fails, publication_unsupported (with the
// mutation mark taken back, since nothing was written) when the file system
// has no atomic way to create the name.
void publishStaged(Context& context, const QString& staged, const QString& path,
                   const QString& failureCode, const QString& failureMessage);

// One entry of a pinned directory, described without following links. A
// symbolic link, a junction or any other reparse point is a link: it is
// neither a file nor a directory here.
class EntryInfo final {
public:
  enum class Kind { File, Directory, Link, Other };

  EntryInfo() = default;
  EntryInfo(const QString& directory, const QString& name, Kind kind,
            qint64 size, qint64 modifiedMs, bool writable)
      : m_directory(directory)
      , m_name(name)
      , m_kind(kind)
      , m_size(size)
      , m_modifiedMs(modifiedMs)
      , m_writable(writable) {}

  QString fileName() const { return m_name; }
  QString absolutePath() const { return m_directory; }
  QString absoluteFilePath() const { return m_directory + '/' + m_name; }
  QString suffix() const {
    const int dot = m_name.lastIndexOf('.');
    return dot < 0 ? QString() : m_name.mid(dot + 1);
  }
  bool isDir() const { return m_kind == Kind::Directory; }
  bool isFile() const { return m_kind == Kind::File; }
  bool isSymLink() const { return m_kind == Kind::Link; }
  bool isJunction() const { return m_kind == Kind::Link; }
  bool isWritable() const { return m_writable; }
  qint64 size() const { return m_size; }
  QDateTime lastModified() const {
    return QDateTime::fromMSecsSinceEpoch(m_modifiedMs);
  }

private:
  QString m_directory;
  QString m_name;
  Kind m_kind         = Kind::Other;
  qint64 m_size       = 0;
  qint64 m_modifiedMs = 0;
  bool m_writable     = false;
};

// Remove a folder and everything under it, entry by entry through descriptors
// pinned to each level, never following a link. Companion files that a file
// system creates and removes with their owner (the "._" files macOS keeps on
// FAT and exFAT) vanish while the folder is being emptied, so each folder is
// listed again until it is empty. A missing folder counts as removed. Returns
// false when something could not be removed.
//
// Throws:
//   Error: path_denied when a component of the path was redirected.
bool removeApprovedTree(const QString& directory);

// A private folder made from a QTemporaryDir template and removed with
// removeApprovedTree, unless kept, so a refusal leaves nothing on any file
// system.
class PrivateDirectory final {
public:
  explicit PrivateDirectory(const QString& pattern) : m_directory(pattern) {
    m_directory.setAutoRemove(false);
  }
  PrivateDirectory(const PrivateDirectory&)            = delete;
  PrivateDirectory& operator=(const PrivateDirectory&) = delete;
  ~PrivateDirectory();

  bool isValid() const { return m_directory.isValid(); }
  QString path() const { return m_directory.path(); }
  QString filePath(const QString& name) const {
    return m_directory.filePath(name);
  }
  // False keeps the folder, as after its contents were published elsewhere.
  void setAutoRemove(bool remove) { m_remove = remove; }

private:
  QTemporaryDir m_directory;
  bool m_remove = true;
};

enum class ListResult { Listed, Failed, TooMany };

// List the entries of the approved directory, in no particular order and
// without "." and "..", through a descriptor pinned to it. At most `maximum`
// entries are returned when it is positive; more yield TooMany. Failed means
// the directory cannot be opened or read.
//
// Throws:
//   Error: path_denied when a component of the path was redirected.
ListResult listApprovedDirectory(const QString& directory,
                                 QVector<EntryInfo>& entries, int maximum = 0);

#ifdef BUILD_UI_PORT_TESTS
// Test-only: the next time `stage` is reached with a path at or below `path`
// (stages: validated, after a path is approved; pinned, after its directory is
// pinned), replace `path` with a symbolic link to `target`, once, and leave a
// `.fired` witness beside it.
void armPathSwap(const QString& path, const QString& target,
                 const QString& stage);
void firePathSwap(const QString& stage, const QString& path);
// Test-only publication faults, any of: exclusive_rename_refused (the kernel
// refuses the exclusive rename), hard_link_refused, racing_destination (another
// party creates the destination right before the move).
void armPublishFaults(const QStringList& faults);
// Native domain test of this module.
class Context;
void runAutomationFilesTests(Context& context);
#endif

}  // namespace GroundzeroAutomation
