// Included in the preset test's anonymous namespace, after its require helper.
// These fixtures exercise publication boundaries without timing-dependent races.
void testLibraryResourceLifetime() {
  Record record;
  {
    QTemporaryDir worker;
    require(worker.isValid(), "cannot create worker resource fixture");
    auto image = std::make_shared<Image>();
    image->width = image->height = 1;
    image->color = true;
    image->pixels = {{1, 0, 0, 1}};
    Record original;
    original.preset.name = "Original retained worker resource";
    original.preset.tip.shape = Shape::Bitmap;
    original.preset.tip.images = {image};
    original.preset.texture.image = image;
    QString error;
    const auto wire = encodeWorkerRecord(original, worker.path(), error);
    require(error.isEmpty() && !wire.isEmpty(), qPrintable(error));
    record = decodeWorkerRecord(wire, worker.path(), error);
    require(error.isEmpty() && record.preset.tip.images.size() == 1 &&
                record.preset.tip.images.front() ==
                record.preset.texture.image, "worker resource ownership missing");
  }
  QTemporaryDir library;
  require(library.isValid(), "cannot create library fixture");
  QString error;
  const auto installed = installCollection(library.path(), {record}, error);
  require(error.isEmpty() && installed.size() == 1, qPrintable(error));
  const auto restored = load(installed.front(), error);
  require(error.isEmpty() && restored.preset.tip.images.size() == 1 &&
              restored.preset.tip.images.front() ==
              restored.preset.texture.image &&
              restored.preset.tip.images.front()->pixels.front().r == 1,
          "installation retained an expired worker-file dependency");
}

void testLibraryEnumeration() {
  require(isLibraryStagingPath(".brush-import-original/one.gzbrush") &&
              isLibraryStagingPath("nested/.ABR-IMPORT-original/tip.png") &&
              isLibraryStagingPath("nested/.brush-install-original/one.gzbrush") &&
              !isLibraryStagingPath("pack-original/one.gzbrush"),
          "reserved transaction path classification changed");
  QTemporaryDir library;
  require(library.isValid(), "cannot create library fixture");
  for (const auto &name : {QString(".brush-import-original"),
                           QString(".abr-import-original"),
                           QString(".brush-install-original")}) {
    require(QDir().mkpath(library.filePath(name)), "cannot stage picker fixture");
    for (const auto &file : {QString("one.gzbrush"), QString("tip.png")}) {
      QFile member(QDir(library.filePath(name)).filePath(file));
      require(member.open(QIODevice::WriteOnly | QIODevice::NewOnly) &&
                  member.write("private staging") == 15, "cannot stage picker member");
    }
  }
  Record original;
  QString error;
  const auto installed = installCollection(library.path(), {original}, error);
  require(error.isEmpty() && installed.size() == 1, qPrintable(error));
  require(publishedLibraryFiles(library.path(), {"*.gzbrush"}) == QStringList{
              QDir(library.path()).relativeFilePath(installed.front())},
          "picker exposed a staged preset or hid a committed preset");
  QFile tip(library.filePath("handmade.png"));
  require(tip.open(QIODevice::WriteOnly | QIODevice::NewOnly) &&
              tip.write("original tip") == 12, "cannot create handmade picker member");
  tip.close();
  require(publishedLibraryFiles(library.path(), {"*.png"}) ==
              QStringList{"handmade.png"}, "legacy picker exposed staged tips");
}

#ifdef RASTERBRUSH_SANDBOX_TESTING
void testLibraryPublicationTransactions() {
  struct ResetObserver {
    ~ResetObserver() { Detail::setLibraryInstallObserver({}); }
  } reset;
  Record first, second;
  first.preset.name = "Original publication first";
  second.preset.name = "Original publication second";
  second.preset.size.value = 36;
  for (bool collection : {false, true})
    for (bool matching : {false, true}) {
      QTemporaryDir library;
      require(library.isValid(), "cannot create library fixture");
      QString error, target;
      QByteArray expected("original competing contents");
      bool entered = false;
      Detail::setLibraryInstallObserver(
          [&](const char *phase, const QString &source, const QString &destination) {
        if (std::strcmp(phase, "before-publish")) return true;
        entered = true;
        target = destination;
        if (collection) {
          require(QDir().mkdir(destination), "cannot create competing directory");
          if (matching)
            for (const auto &name : QDir(source).entryList(QDir::Files))
              require(QFile::copy(QDir(source).filePath(name),
                                   QDir(destination).filePath(name)),
                      "cannot publish competing collection member");
        } else {
          if (matching) {
            QFile staged(source);
            require(staged.open(QIODevice::ReadOnly), "cannot read staged member");
            expected = staged.readAll();
          }
          QFile winner(destination);
          require(winner.open(QIODevice::WriteOnly | QIODevice::NewOnly) &&
                      winner.write(expected) == expected.size() && winner.flush(),
                  "cannot publish competing file");
        }
        return true;
      });
      const auto result = collection ? installCollection(library.path(), {first, second}, error)
                                     : QStringList{install(library.path(), first, error)};
      require(entered && (matching ? error.isEmpty() && result.size() == (collection ? 2 : 1)
                                       && !result.front().isEmpty()
                                   : !error.isEmpty() &&
                                       (collection ? result.empty() : result.front().isEmpty())),
              "publication collision was not verified without replacement");
      Detail::setLibraryInstallObserver({});
      if (collection)
        require(QDir(target).entryList(QDir::Files).size() == (matching ? 2 : 0),
                "competing empty directory was replaced by staged members");
      else
        require(readBoundedRegularFile(target, 64 * 1024 * 1024, error) == expected &&
                    error.isEmpty(), "competing preset contents were overwritten");
      require(QDir(library.path()).entryList(QDir::AllEntries | QDir::Hidden |
                  QDir::NoDotAndDotDot).size() == 1, "collision left private staging");
    }
  for (int fault = 0; fault < 4; ++fault) {
    QTemporaryDir library;
    require(library.isValid(), "cannot create library fixture");
    QString error;
    std::atomic_bool cancelled(false);
    int writes = 0;
    bool injected = false;
    Detail::setLibraryInstallObserver(
        [&](const char *phase, const QString &, const QString &) {
      if (!std::strcmp(phase, "before-write") && ++writes == 2 && fault == 0) {
        injected = true;
        return false;
      }
      if (!std::strcmp(phase, "before-publish") && fault == 1) {
        injected = true;
        return false;
      }
      if ((!std::strcmp(phase, "after-write") && fault == 2) ||
          (!std::strcmp(phase, "before-publish") && fault == 3)) {
        injected = true;
        cancelled.store(true, std::memory_order_relaxed);
      }
      return true;
    });
    require(installCollection(library.path(), {first, second}, error, &cancelled).empty() &&
                !error.isEmpty() && injected, "transaction fault returned installed members");
    Detail::setLibraryInstallObserver({});
    require(QDir(library.path()).entryList(QDir::AllEntries | QDir::Hidden |
                QDir::NoDotAndDotDot).empty(), "fault or cancellation left a partial collection");
  }
  QTemporaryDir library;
  require(library.isValid(), "cannot create library fixture");
  QString error;
  const auto installed = installCollection(library.path(), {first, second}, error);
  require(error.isEmpty() && installed.size() == 2, qPrintable(error));
  std::atomic_bool cancelled(false);
  int reads = 0;
  Detail::setLibraryInstallObserver(
      [&](const char *phase, const QString &, const QString &) {
    if (!std::strcmp(phase, "before-reuse") && ++reads == 2)
      cancelled.store(true, std::memory_order_relaxed);
    return true;
  });
  require(installCollection(library.path(), {first, second}, error, &cancelled).empty() &&
              error.contains("cancelled") && reads == 2, "cancelled reimport returned success");
  Detail::setLibraryInstallObserver({});
  require(QDir(library.path()).entryList(QDir::AllEntries | QDir::Hidden |
              QDir::NoDotAndDotDot).size() == 1, "cancelled reimport damaged the library");
  for (const auto &path : installed)
    require(!load(path, error).preset.name.empty() && error.isEmpty(),
            "cancelled reimport changed a committed member");
#ifndef _WIN32
  const auto extra = QDir(QFileInfo(installed.front()).absolutePath()).filePath("unrelated-fifo");
  const auto native = QFile::encodeName(extra);
  require(mkfifo(native.constData(), 0600) == 0, "cannot create extra FIFO fixture");
  require(installCollection(library.path(), {first, second}, error).empty() &&
              !error.isEmpty(), "collection identity ignored an unexpected system entry");
#endif
}
#endif

#ifdef RASTERBRUSH_SANDBOX_TESTING
void testLibraryEncodedResourceCancellation() {
  struct ResetObserver {
    ~ResetObserver() { Detail::setLibraryInstallObserver({}); }
  } reset;
  auto red = std::make_shared<Image>();
  red->width = red->height = 1;
  red->color = true;
  red->pixels = {{1, 0, 0, 1}};
  Record record;
  record.preset.name = "Original shared encoding resource";
  record.preset.tip.shape = Shape::Bitmap;
  record.preset.tip.images = {red, red};
  record.preset.texture.image = red;
  int encoded = 0;
  Detail::setLibraryInstallObserver(
      [&](const char *phase, const QString &, const QString &) {
    if (!std::strcmp(phase, "after-resource-encode")) ++encoded;
    return true;
  });
  QTemporaryDir library;
  require(library.isValid(), "cannot create shared encoding fixture");
  QString error;
  const auto installed = installCollection(library.path(), {record}, error);
  require(error.isEmpty() && installed.size() == 1 && encoded == 1,
          "shared image references repeated the bounded resource encoding");
  Detail::setLibraryInstallObserver({});
  auto blue = std::make_shared<Image>(*red);
  blue->pixels = {{0, 0, 1, 1}};
  record.preset.tip.images[1] = blue;
  std::atomic_bool cancelled(false);
  encoded = 0;
  Detail::setLibraryInstallObserver(
      [&](const char *phase, const QString &, const QString &) {
    if (!std::strcmp(phase, "after-resource-encode")) {
      ++encoded;
      cancelled.store(true, std::memory_order_relaxed);
    }
    return true;
  });
  QTemporaryDir rejected;
  require(rejected.isValid(), "cannot create resource cancellation fixture");
  require(installCollection(rejected.path(), {record}, error, &cancelled).empty() &&
              error.contains("cancelled") && encoded == 1,
          "resource cancellation continued encoding or published a package");
  require(QDir(rejected.path()).entryList(QDir::AllEntries | QDir::Hidden |
              QDir::NoDotAndDotDot).empty(),
          "resource cancellation left staged or installed package files");
}
#endif
