#include "automation.h"
#include "automation_files.h"
#include "automation_ownership.h"
#include "automation_root_publish.h"

#include <QCryptographicHash>
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

void checkTreeRemoval(const QString& root) {
  QTemporaryDir temporary(root + "/files-XXXXXX");
  const QString base    = sandbox(temporary, "tree");
  const QString outside = sandbox(temporary, "outside");
  write(outside + "/keep.txt", "keep");
  QDir().mkpath(base + "/staging/a/b");
  write(base + "/staging/a/b/deep.txt", "deep");
  write(base + "/staging/top.txt", "top");
  write(base + "/staging/._top.txt", "companion");
  require(QFile::link(outside, base + "/staging/link"),
          "Cannot link a fixture");
  require(removeApprovedTree(base + "/staging"),
          "A tree with links must be removed");
  require(!QFileInfo(base + "/staging").exists(), "The tree is gone");
  require(contents(outside + "/keep.txt") == "keep",
          "Removing a tree never follows a link out of it");
  require(removeApprovedTree(base + "/staging"),
          "A missing tree counts as removed");
  QDir().mkpath(base + "/swapped/inner");
  write(outside + "/inner-keep.txt", "keep");
  require(QFile::rename(base + "/swapped", base + "/swapped.kept") &&
              QFile::link(outside, base + "/swapped"),
          "Cannot swap a fixture");
  require(throwsDenied([&] { removeApprovedTree(base + "/swapped/inner"); }) ||
              !removeApprovedTree(base + "/swapped/inner"),
          "A redirected tree is refused");
  require(contents(outside + "/inner-keep.txt") == "keep",
          "A redirected removal leaves the target alone");
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

// Output-root paths: the grammar, and a walk that never follows a link or a
// case variant of a stored name.
void checkRootWalk(const QString& workspace) {
  require(rootPathProblem("renders/shot-01", 1).isEmpty(),
          "valid path refused");
  for (const QString& bad :
       {QString(), QString("/abs"), QString("a/../b"), QString("a/./b"),
        QString("a\\b"), QString(".groundzero-staging/x"), QString("a//b"),
        QString(QString("c/").repeated(17))})
    require(!rootPathProblem(bad, 0).isEmpty(), "invalid root path admitted");
  require(!rootPathProblem("ok/-dash", 1).isEmpty(),
          "a new name starting with a dash was admitted");
  QTemporaryDir directory(workspace + "/root-walk-XXXXXX");
  const QString root = QFileInfo(directory.path()).canonicalFilePath();
  require(QDir(root).mkpath("Shots/One"), "cannot create the fixture");
  const int reached = openRootDirectory(root, "Shots/One");
  require(reached >= 0, "an existing directory was not reached");
  ::close(reached);
  require(openRootDirectory(root, "Shots/Missing") < 0, "a missing one was");
  require(QFile::link(root + "/Shots", root + "/Linked"), "cannot link");
  bool refused = false;
  try {
    openRootDirectory(root, "Linked/One");
  } catch (const Error& error) {
    refused = error.code == "path_denied";
  }
  require(refused, "a linked component was followed");
  refused = false;
  try {
    const int variant = openRootDirectory(root, "shots/One");
    if (variant >= 0) ::close(variant);
    // A case-sensitive volume has no such name: also not reached.
    refused = variant < 0;
  } catch (const Error& error) {
    refused = error.code == "path_denied";
  }
  require(refused, "a case variant of a stored name was reached");
}

StagedEntry stagedEntry(const QString& tree, const QString& relative) {
  QFile file(tree + '/' + relative);
  require(file.open(QIODevice::ReadOnly), "Cannot read a staged fixture");
  const QByteArray bytes = file.readAll();
  return {relative, bytes.size(),
          QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()};
}

Error publishError(const std::function<void()>& call) {
  try {
    call();
  } catch (const Error& error) {
    return error;
  }
  return Error("none", "The call was not refused.");
}

QStringList entriesOf(const QString& directory) {
  return QDir(directory).entryList(
      QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
      QDir::Name);
}

void checkContainerRecovery(const QString& root) {
  const QString name      = ".groundzero-staging-0123456789abcdef";
  const QString container = root + '/' + name;
  require(QDir().mkpath(container + "/pkg/assets"),
          "Cannot create a container");
  write(container + "/pkg/assets/a.png", "a");
  write(container + "/pkg/extra.txt", "x");
  const QSet<QString> listed{"assets/a.png"};
  {
    OwnershipClaim live;
    require(live.acquire(container), "Cannot claim the container");
    const ContainerCleanup held =
        removeAbandonedContainer(root, QString(), name, "pkg", listed);
    require(held.held && !held.removed &&
                QFileInfo::exists(container + "/pkg/assets/a.png"),
            "a container a live worker holds was touched");
  }
  const ContainerCleanup partial =
      removeAbandonedContainer(root, QString(), name, "pkg", listed);
  require(!partial.removed && !QFileInfo::exists(container + "/pkg/assets") &&
              partial.residue == QStringList({"pkg", "pkg/extra.txt"}),
          "recovery removed an unlisted file or kept a listed one");
  require(QFile::remove(container + "/pkg/extra.txt"), "Cannot clear residue");
  require(
      removeAbandonedContainer(root, QString(), name, "pkg", listed).removed &&
          !QFileInfo::exists(container),
      "an abandoned container stayed");
}

void checkRootPublication(Context& context) {
  QTemporaryDir directory(context.workspace() + "/root-publish-XXXXXX");
  const QString root   = sandbox(directory, "root");
  const QString staged = sandbox(directory, "staged");
  require(QDir().mkpath(staged + "/assets") && QDir().mkpath(root + "/shots"),
          "Cannot create the publication fixture");
  write(staged + "/scene.tnz", "scene");
  write(staged + "/assets/a.png", QByteArray(3 * 1024 * 1024 + 7, 'a'));
  const QVector<StagedEntry> entries{stagedEntry(staged, "assets/a.png"),
                                     stagedEntry(staged, "scene.tnz")};
  const auto publish = [&](const QString& parent, const QString& name) {
    return publishError([&] {
      publishToRoot(context, staged, {"renders", root, parent, name}, entries,
                    {}, "package_write_failed");
    });
  };
  require(publish("shots", "pkg").code == "none", "publication failed");
  require(entriesOf(root + "/shots") == QStringList{"pkg"} &&
              stagedEntry(root + "/shots/pkg", "assets/a.png").sha256 ==
                  entries.first().sha256,
          "the published copy differs or its container stayed");
  require(publish("shots", "pkg").code == "already_exists",
          "an existing package was not refused");
  require(publish("absent", "pkg").code == "missing_parent",
          "a missing parent was not refused");
  write(staged + "/scene.tnz", "changed");
  const Error changed = publish("shots", "pkg2");
  require(changed.details.value("reason") == "staged_changed" &&
              entriesOf(root + "/shots") == QStringList{"pkg"},
          "a changed staged file was published or left a container");
  checkContainerRecovery(root);
}

}  // namespace

void runAutomationFilesTests(Context& context) {
  const QString root = context.workspace();
  checkListing(root);
#ifndef _WIN32
  checkLinks(root);
  checkSwaps(root);
  checkHardLinks(root);
  checkPublicationPrimitives(root);
  checkTreeRemoval(root);
  checkRootWalk(root);
  checkRootPublication(context);
#endif
  std::cerr << "automation-files-test: PASS\n";
}

}  // namespace GroundzeroAutomation
