#include "automation.h"
#include "automation_files.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QTemporaryDir>

#include <atomic>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <thread>

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

QByteArray contents(const QString& path) {
  QFile file(path);
  return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// Watches a destination name while it is published: every look must find it
// absent or complete, never partly written.
class Observer final {
public:
  Observer(const QString& path, qint64 whole) : m_path(path), m_whole(whole) {
    m_thread = std::thread([this] {
      while (!m_stop) {
        const qint64 size =
            QFileInfo(m_path).exists() ? QFileInfo(m_path).size() : -1;
        if (size >= 0 && size != m_whole) m_partial = true;
      }
    });
  }
  ~Observer() { finish(); }
  bool sawPartial() {
    finish();
    return m_partial;
  }

private:
  void finish() {
    m_stop = true;
    if (m_thread.joinable()) m_thread.join();
  }
  QString m_path;
  qint64 m_whole;
  std::atomic<bool> m_stop{false};
  std::atomic<bool> m_partial{false};
  std::thread m_thread;
};

void checkPublicationPrimitives(const QString& root) {
  QTemporaryDir temporary(root + "/files-XXXXXX");
  const QString base = sandbox(temporary, "publish");
  const QByteArray big(8 * 1024 * 1024, 'x');
  const auto stage = [&](const QString& name) {
    write(base + "/" + name, big);
    return base + "/" + name;
  };
  for (const QStringList& faults :
       {QStringList{}, QStringList{"exclusive_rename_refused"}}) {
    armPublishFaults(faults);
    const QString destination = base + "/out-" + QString::number(faults.size());
    Observer observer(destination, big.size());
    require(publishApproved(stage("a-" + QString::number(faults.size())),
                            destination) == PublishResult::Published,
            "A free name is published whichever primitive is available");
    require(!observer.sawPartial(),
            "A destination is never visible partly written");
    require(contents(destination) == big, "The published file is complete");
  }
  armPublishFaults({"racing_destination"});
  require(
      publishApproved(stage("b"), base + "/raced") == PublishResult::Exists &&
          contents(base + "/raced") == "other party\n",
      "A destination created at the last moment is never replaced");
  armPublishFaults({"racing_destination", "exclusive_rename_refused"});
  require(publishApproved(stage("c"), base + "/raced-link") ==
                  PublishResult::Exists &&
              contents(base + "/raced-link") == "other party\n",
          "The hard-link primitive never replaces a destination either");
  armPublishFaults({"exclusive_rename_refused", "hard_link_refused"});
  require(publishApproved(stage("d"), base + "/refused") ==
                  PublishResult::Unsupported &&
              !QFileInfo(base + "/refused").exists(),
          "Without an atomic primitive the publication is refused");
  QDir().mkpath(base + "/folder-staged");
  armPublishFaults({"exclusive_rename_refused"});
  require(publishApproved(base + "/folder-staged", base + "/folder") ==
                  PublishResult::Unsupported &&
              !QFileInfo(base + "/folder").exists(),
          "A folder has no atomic fallback and is refused");
  armPublishFaults({"racing_destination"});
  require(publishApproved(base + "/folder-staged", base + "/folder-raced") ==
                  PublishResult::Exists &&
              QDir(base + "/folder-raced")
                  .entryList(QDir::NoDotAndDotDot)
                  .isEmpty(),
          "A folder created at the last moment is never replaced");
  armPublishFaults({});
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
  require(publishApproved(base + "/staged.txt", base + "/dest.txt") ==
              PublishResult::Exists,
          "A publication never replaces an existing name");
  QFile target(outside + "/target.txt");
  require(target.open(QIODevice::ReadOnly) &&
              target.readAll() == "outside original",
          "A hard link must not let a publication change the file behind it");
  require(publishApproved(base + "/staged.txt", base + "/fresh.txt") ==
                  PublishResult::Published &&
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
  checkPublicationPrimitives(root);
#endif
  std::cerr << "automation-files-test: PASS\n";
}

}  // namespace GroundzeroAutomation
