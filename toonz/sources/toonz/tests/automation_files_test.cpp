#include "automation.h"
#include "automation_files.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QTemporaryDir>

#include <functional>
#include <iostream>
#include <stdexcept>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace GroundzeroAutomation {
namespace {

void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}

void write(const QString& path, const QByteArray& bytes) {
  QFile file(path);
  require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(),
          "Cannot write a fixture");
}

QSet<QString> names(const QVector<EntryInfo>& entries) {
  QSet<QString> result;
  for (const EntryInfo& entry : entries) result.insert(entry.fileName());
  return result;
}

QString sandbox(QTemporaryDir& directory, const QString& name) {
  const QString path = directory.filePath(name);
  require(QDir().mkpath(path), "Cannot create a fixture folder");
  return QFileInfo(path).canonicalFilePath();
}

bool throwsDenied(const std::function<void()>& call) {
  try {
    call();
  } catch (const Error& error) {
    return error.code == "path_denied";
  }
  return false;
}

void checkListing(const QString& root) {
  QTemporaryDir temporary(root + "/files-XXXXXX");
  const QString folder = sandbox(temporary, "list");
  write(folder + "/a.txt", "a");
  QDir().mkpath(folder + "/sub");
  QVector<EntryInfo> entries;
  require(listApprovedDirectory(folder, entries) == ListResult::Listed,
          "A folder must list");
  require(names(entries) == QSet<QString>({"a.txt", "sub"}),
          "A listing holds every entry and no dot entries");
  for (const EntryInfo& entry : entries)
    require(entry.isDir() == (entry.fileName() == "sub") &&
                entry.isFile() == (entry.fileName() == "a.txt") &&
                !entry.isSymLink(),
            "An entry is described by its own kind");
  QVector<EntryInfo> limited;
  require(listApprovedDirectory(folder, limited, 1) == ListResult::TooMany &&
              limited.size() == 1,
          "A listing stops at its bound");
  require(
      listApprovedDirectory(folder + "/missing", limited) == ListResult::Failed,
      "A missing folder does not list");
}

#ifndef _WIN32
void swapFolder(const QString& folder, const QString& target) {
  require(
      QFile::rename(folder, folder + ".kept") && QFile::link(target, folder),
      "Cannot swap a fixture folder");
}

void checkLinks(const QString& root) {
  QTemporaryDir temporary(root + "/files-XXXXXX");
  const QString base    = sandbox(temporary, "links");
  const QString outside = sandbox(temporary, "outside");
  write(base + "/real.txt", "real");
  write(outside + "/secret.txt", "secret");
  require(QFile::link(outside + "/secret.txt", base + "/link.txt"),
          "Cannot link a fixture");
  QVector<EntryInfo> entries;
  require(listApprovedDirectory(base, entries) == ListResult::Listed,
          "A folder with links must list");
  for (const EntryInfo& entry : entries)
    require(entry.isSymLink() == (entry.fileName() == "link.txt") &&
                !(entry.isSymLink() && (entry.isFile() || entry.isDir())),
            "A link is only a link");
  QFile file;
  require(throwsDenied([&] { openApprovedFile(file, base + "/link.txt"); }),
          "A link is never opened");
  require(openApprovedFile(file, base + "/real.txt") == OpenResult::Opened &&
              file.readAll() == "real",
          "A regular file opens");
}

void checkSwaps(const QString& root) {
  QTemporaryDir temporary(root + "/files-XXXXXX");
  const QString base    = sandbox(temporary, "swaps");
  const QString outside = sandbox(temporary, "outside");
  write(outside + "/decoy.txt", "decoy");
  QDir().mkpath(outside + "/deep");
  QDir().mkpath(base + "/inner/deep");
  write(base + "/inner/deep/real.txt", "real");
  QVector<EntryInfo> entries;
  swapFolder(base + "/inner", outside);
  require(
      throwsDenied([&] { listApprovedDirectory(base + "/inner", entries); }),
      "A folder replaced before it is pinned is refused");
  require(throwsDenied(
              [&] { listApprovedDirectory(base + "/inner/deep", entries); }),
          "A folder below a replaced ancestor is refused");
  require(entries.isEmpty(), "A refused listing returns nothing");
  QFile::remove(base + "/inner");
  QFile::rename(base + "/inner.kept", base + "/inner");
#ifdef BUILD_UI_PORT_TESTS
  armPathSwap(base + "/inner/deep", outside, "pinned");
  entries.clear();
  require(listApprovedDirectory(base + "/inner/deep", entries) ==
              ListResult::Listed,
          "A folder swapped after it was pinned still lists");
  require(QFileInfo(base + "/inner/deep.fired").exists(),
          "The swap hook must fire");
  require(names(entries) == QSet<QString>({"real.txt"}),
          "A pinned listing never shows the replacement");
#endif
}

void checkHardLinks(const QString& root) {
  QTemporaryDir temporary(root + "/files-XXXXXX");
  const QString base    = sandbox(temporary, "hard");
  const QString outside = sandbox(temporary, "outside");
  write(outside + "/target.txt", "outside original");
  require(::link(QFile::encodeName(outside + "/target.txt").constData(),
                 QFile::encodeName(base + "/dest.txt").constData()) == 0,
          "Cannot hard link a fixture");
  write(base + "/staged.txt", "replacement");
  require(!publishApproved(base + "/staged.txt", base + "/dest.txt"),
          "A publication never replaces an existing name");
  QFile target(outside + "/target.txt");
  require(target.open(QIODevice::ReadOnly) &&
              target.readAll() == "outside original",
          "A hard link must not let a publication change the file behind it");
  require(publishApproved(base + "/staged.txt", base + "/fresh.txt") &&
              QFile(base + "/fresh.txt").exists(),
          "A publication to a free name succeeds");
}
#endif

}  // namespace

void runAutomationFilesTests(Context& context) {
  const QString root = context.workspace();
  checkListing(root);
#ifndef _WIN32
  checkLinks(root);
  checkSwaps(root);
  checkHardLinks(root);
#endif
  std::cerr << "automation-files-test: PASS\n";
}

}  // namespace GroundzeroAutomation
