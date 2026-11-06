#pragma once

#include "toonz/rasterbrushlibrary.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDirIterator>
#include <QEvent>
#include <QListWidget>
#include <QMap>
#include <QSaveFile>
#include <cstring>

// Private installed-application fixture, included after the common integration
// helpers. All pixels/names/bytes are authored here; this is native conversion
// and transaction coverage, never Photoshop behavior or reference evidence.
namespace {
QByteArray originalCollectionImportAbr() {
  auto be = [](quint64 value, int count = 4) {
    QByteArray result;
    for (int index = count - 1; index >= 0; --index)
      result.append(char(value >> (8 * index)));
    return result;
  };
  auto id = [&](const QByteArray &value) { return be(value.size()) + value; };
  auto text = [&](const QString &value) {
    QByteArray result = be(value.size());
    for (const auto unit : value) result += be(unit.unicode(), 2);
    return result;
  };
  auto field = [&](const char *key, const char *kind, const QByteArray &value) {
    return id(key) + kind + value;
  };
  auto number = [&](double value, const char *unit) {
    quint64 bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return QByteArray(unit) + be(bits, 8);
  };
  auto object = [&](const char *kind, int count, const QByteArray &fields) {
    return text("") + id(kind) + be(count) + fields;
  };
  auto section = [&](const char *kind, const QByteArray &bytes) {
    QByteArray result = QByteArray("8BIM") + kind + be(bytes.size()) + bytes;
    while (result.size() % 4) result += char(0);
    return result;
  };
  const QByteArray sampleId("00000000-0000-0000-0000-000000000009");
  QByteArray sample(301, char(0));
  sample[0] = char(sampleId.size());
  sample.replace(1, sampleId.size(), sampleId);
  for (int value : {0, 0, 2, 2}) sample += be(value);
  sample += be(8, 2) + char(0) + char(0) + char(64) + char(128) + char(255);
  QByteArray samples = be(sample.size()) + sample;
  while (samples.size() % 4) samples += char(0);
  QByteArray presets;
  const QStringList names{"Original GUI Collection First", "Original GUI Collection Second",
                          "Original GUI Collection Third"};
  for (int index = 0; index < names.size(); ++index) {
    const auto brush = object("sampledBrush", 9,
        field("Dmtr", "UntF", number(12 + index * 8, "#Pxl")) +
        field("Angl", "UntF", number(0, "#Ang")) +
        field("Rndn", "UntF", number(75, "#Prc")) +
        field("Spcn", "UntF", number(25, "#Prc")) +
        field("Intr", "bool", QByteArray(1, char(1))) +
        field("flipX", "bool", QByteArray(1, char(0))) +
        field("flipY", "bool", QByteArray(1, char(0))) +
        field("Nm  ", "TEXT", text("Original asymmetric shared mask")) +
        field("sampledData", "TEXT", text(QString::fromLatin1(sampleId))));
    presets += QByteArray("Objc") + object("brushPreset", 3,
        field("Nm  ", "TEXT", text(names[index])) + field("Brsh", "Objc", brush) +
        field("useTipDynamics", "bool", QByteArray(1, char(0))));
  }
  const auto descriptor = be(16) + object("null", 1,
      field("Brsh", "VlLs", be(3) + presets));
  return be(6, 2) + be(2, 2) + section("samp", samples) + section("patt", {}) +
         section("desc", descriptor);
}

QMap<QString, QByteArray> collectionLibrarySnapshot(const QString &directory) {
  QMap<QString, QByteArray> result;
  QDirIterator entries(directory, QDir::AllEntries | QDir::NoDotAndDotDot |
      QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
  const QDir root(directory);
  while (entries.hasNext()) {
    entries.next();
    const auto info = entries.fileInfo();
    const auto relative = root.relativeFilePath(info.filePath());
    if (info.isSymLink()) result["link:" + relative] = "link";
    else if (info.isDir()) result["directory:" + relative] = "directory";
    else {
      QString error;
      const auto bytes = RasterBrush::readBoundedRegularFile(info.filePath(), 64 * 1024 * 1024, error);
      require(error.isEmpty(), "cannot snapshot installed collection member");
      result["file:" + relative] = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex() +
          ":" + QByteArray::number(info.size()) + ":" +
          QByteArray::number(info.lastModified().toMSecsSinceEpoch()) + ":" +
          QByteArray::number(int(info.permissions()));
    }
  }
  return result;
}

class CollectionProgressObserver final : public QObject {
  std::function<void(QDialog *)> shown;
public:
  explicit CollectionProgressObserver(std::function<void(QDialog *)> callback)
      : shown(std::move(callback)) { qApp->installEventFilter(this); }
  ~CollectionProgressObserver() override { if (qApp) qApp->removeEventFilter(this); }
  bool eventFilter(QObject *object, QEvent *event) override {
    if (event->type() == QEvent::Show) {
      auto dialog = qobject_cast<QDialog *>(object);
      if (dialog && dialog->isWindow() && dialog->windowTitle() == "Import Raster Brush") shown(dialog);
    }
    return false;
  }
};

void runRasterBrushCollectionImportIntegration(TTool *tool, TEnumProperty *property,
                                               TXshSimpleLevel *level, TUndoManager *undo) {
  checkpoint("qualifying real collection import selection, cancellation and rollback");
  require(tool && property && level && undo && undo->getHistoryCount() == 0,
          "collection UI fixture needs an idle native tool and empty undo history");
  const auto originalSelection = property->getValue();
  const auto baseline = raster(level, 1)->clone();
  const QString library = ToonzFolder::getLibraryFolder().getQString() + "/rasterbrushes";
#ifdef Q_OS_WIN
  constexpr auto pathCase = Qt::CaseInsensitive;
#else
  constexpr auto pathCase = Qt::CaseSensitive;
#endif
  QString isolationError;
  const QString qualificationRoot = QDir::current().canonicalPath();
  const QString isolatedLibraryRoot = QDir(library).canonicalPath();
  const auto marker = RasterBrush::readBoundedRegularFile(
      QDir::current().filePath(".groundzero-native-ui-test"), 128, isolationError);
  require(isolationError.isEmpty() && marker == "groundzero-native-ui-test\n" &&
              !qualificationRoot.isEmpty() && !isolatedLibraryRoot.isEmpty() &&
              isolatedLibraryRoot.startsWith(qualificationRoot + "/", pathCase),
          "collection GUI fixture requires the disposable marked library profile");
  QTemporaryDir fixture;
  require(fixture.isValid(), "cannot create collection UI source fixture");
  const QString input = fixture.filePath("original-ui-collection.abr");
  QFile source(input);
  const auto sourceBytes = originalCollectionImportAbr();
  require(source.open(QIODevice::WriteOnly | QIODevice::NewOnly) &&
              source.write(sourceBytes) == sourceBytes.size() && source.flush(),
          "cannot stage original GUI collection ABR");
  source.close();
  const QStringList names{"Original GUI Collection First", "Original GUI Collection Second",
                          "Original GUI Collection Third"};
  enum class Action { CancelPicker, CancelWorker, CancelChooser, EmptySelection,
                      CancelConversion, Subset, All };
  struct Result {
    bool picker = false, worker = false, chooser = false, acceptance = false;
    bool cancelledWorker = false, expired = false;
    QString failure, warning;
  };
  auto run = [&](Action action, bool expectConflict = false) {
    Result result;
    struct RestoreDialogs {
      bool previous = QApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
      RestoreDialogs() { QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true); }
      ~RestoreDialogs() { QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, previous); }
    } dialogs;
    QObject context;
    QElapsedTimer deadline;
    deadline.start();
    auto fail = [&](const QString &message, QDialog *dialog = nullptr) {
      if (result.failure.isEmpty()) result.failure = message;
      if (dialog) dialog->reject();
    };
    CollectionProgressObserver progress([&](QDialog *dialog) {
      result.worker = true;
      if (action != Action::CancelWorker || result.cancelledWorker) return;
      const auto buttons = dialog->findChildren<QPushButton *>();
      if (buttons.size() != 1) { fail("import progress has no unique Cancel button", dialog); return; }
      result.cancelledWorker = true;
      // Synchronous Show delivery precedes the nested modal event loop's
      // finished callback. Even a fast completed worker cannot bypass this
      // real Cancel click and have its discarded result installed.
      buttons.front()->click();
    });
    QTimer driver;
    QObject::connect(&driver, &QTimer::timeout, &context, [&] {
      auto modal = QApplication::activeModalWidget();
      auto dialog = qobject_cast<QDialog *>(modal);
      if (deadline.elapsed() > 15000) {
        result.expired = true;
        fail("collection import exceeded its bounded modal deadline", dialog);
        return;
      }
      if (!modal) return;
      if (auto picker = qobject_cast<QFileDialog *>(modal)) {
        if (result.picker) return;
        result.picker = true;
        if (action == Action::CancelPicker) picker->reject();
        else if (!submitFileDialog(picker, input)) fail("collection source picker did not select original ABR", picker);
        return;
      }
      if (modal->windowTitle() == "Import Raster Brush") return;
      if (dialog && modal->windowTitle() == "Import Brush Collection") {
        if (result.chooser) return;
        result.chooser = true;
        auto list = dialog->findChild<QListWidget *>();
        auto buttons = dialog->findChild<QDialogButtonBox *>();
        if (!list || list->count() != 3 || !buttons || !buttons->button(QDialogButtonBox::Ok)) {
          fail("installed worker did not supply three selectable original presets", dialog); return;
        }
        for (int index = 0; index < list->count(); ++index)
          if (list->item(index)->text() != names[index] ||
              list->item(index)->checkState() != Qt::Checked ||
              !(list->item(index)->flags() & Qt::ItemIsUserCheckable)) {
            fail("collection chooser lost original name, order or default selection", dialog); return;
          }
        if (action == Action::CancelChooser) { dialog->reject(); return; }
        for (int index = 0; index < list->count(); ++index)
          list->item(index)->setCheckState(action == Action::EmptySelection ||
              (action != Action::All && index == 1) ? Qt::Unchecked : Qt::Checked);
        buttons->button(QDialogButtonBox::Ok)->click();
        return;
      }
      if (modal->objectName() == "nativeBrushApproximationAcceptance") {
        if (result.acceptance) return;
        result.acceptance = true;
        auto report = qobject_cast<QMessageBox *>(modal);
        auto accept = report ? report->findChild<QPushButton *>("nativeBrushAcceptApproximation") : nullptr;
        if (!report || !accept || report->defaultButton() != report->button(QMessageBox::Cancel) ||
            !report->text().contains("Static native conversion") ||
            !report->detailedText().contains(names[0]) || !report->detailedText().contains(names[2]) ||
            report->detailedText().contains(names[1]) != (action == Action::All)) {
          fail("collection acknowledgment omitted selected conversions or included unchecked presets", dialog); return;
        }
        if (action == Action::CancelConversion) report->reject();
        else accept->click();
        return;
      }
      if (dialog) {
        QStringList text;
        if (auto message = qobject_cast<QMessageBox *>(dialog))
          text << message->text() << message->detailedText();
        for (auto label : dialog->findChildren<QLabel *>()) text << label->text();
        if (result.warning.isEmpty()) result.warning = text.join("\n");
        if (!expectConflict) fail("unexpected collection import warning", dialog);
        else dialog->reject();
      }
    });
    driver.start(10);
    property->setValue(L"<import>");
    const bool handled = tool->onPropertyChanged("Native Brush:");
    driver.stop();
    require(handled && result.picker && !result.expired && result.failure.isEmpty(),
            qPrintable("collection UI workflow failed: " + result.failure + "\n" + result.warning));
    if (!expectConflict) require(result.warning.isEmpty(), qPrintable(result.warning));
    else require(result.warning.contains("Existing brush collection content conflicts with its identity"),
                 qPrintable("collection conflict did not display its actual content-verification failure: " + result.warning));
    require(same(baseline, raster(level, 1)) && undo->getHistoryCount() == 0,
            "collection import changed drawing pixels or undo history");
    for (const auto &entry : property->getRange())
      require(!RasterBrush::isLibraryStagingPath(QString::fromStdWString(entry)),
              "native selector exposed an uncommitted collection member");
    return result;
  };
  auto unchanged = [&](Action action) {
    const auto files = collectionLibrarySnapshot(library);
    const auto selected = property->getValue();
    const auto range = property->getRange();
    const auto result = run(action);
    require(collectionLibrarySnapshot(library) == files && property->getValue() == selected &&
                property->getRange() == range,
            "cancelled or empty collection import changed library, selector or selection");
    if (action == Action::CancelPicker)
      require(!result.worker && !result.chooser && !result.acceptance,
              "cancelled source picker entered resource decoding");
    else if (action == Action::CancelWorker)
      require(result.worker && result.cancelledWorker && !result.chooser && !result.acceptance,
              "actual worker Cancel did not discard the whole imported collection");
    else if (action == Action::CancelChooser || action == Action::EmptySelection)
      require(result.worker && result.chooser && !result.acceptance,
              "cancelled or empty chooser advanced to conversion acceptance");
    else require(result.worker && result.chooser && result.acceptance,
                 "conversion rejection did not traverse the real collection UI");
  };
  for (Action action : {Action::CancelPicker, Action::CancelWorker, Action::CancelChooser,
                        Action::EmptySelection, Action::CancelConversion}) unchanged(action);
  auto added = [](const TEnumProperty::Range &before, const TEnumProperty::Range &after) {
    QStringList result;
    for (const auto &entry : after)
      if (std::find(before.begin(), before.end(), entry) == before.end())
        result << QString::fromStdWString(entry);
    return result;
  };
  const auto beforeSubset = property->getRange();
  const auto subsetResult = run(Action::Subset);
  const auto subset = added(beforeSubset, property->getRange());
  require(subsetResult.worker && subsetResult.chooser && subsetResult.acceptance && subset.size() == 2 &&
              subset.contains(QString::fromStdWString(property->getValue())),
          "GUI subset import did not install and select exactly its checked presets");
  const auto subsetFiles = collectionLibrarySnapshot(library);
  const auto subsetRange = property->getRange();
  const auto subsetSelection = property->getValue();
  run(Action::Subset);
  require(collectionLibrarySnapshot(library) == subsetFiles && property->getRange() == subsetRange &&
              property->getValue() == subsetSelection,
          "identical GUI collection reimport duplicated or rewrote installed presets");
  auto ownedMember = [&](const QString &relative) {
    require(!QDir::isAbsolutePath(relative) && QDir::cleanPath(relative) == relative &&
                !relative.split('/').contains("..") && relative.endsWith(".gzbrush"),
            "collection fixture member path is not a safe relative preset");
    const QString root = QDir(library).canonicalPath();
    const QFileInfo member(QDir(library).filePath(relative));
    const auto canonical = member.canonicalFilePath();
    require(!root.isEmpty() && member.isFile() && !member.isSymLink() &&
                root == isolatedLibraryRoot && canonical.startsWith(root + "/", pathCase),
            "collection fixture member escaped its isolated library");
    return canonical;
  };
  auto readback = [&](const QStringList &members, const QStringList &expectedNames) {
    QStringList actualNames;
    QString commonResource;
    for (const auto &relative : members) {
      QString error;
      const auto imported = RasterBrush::importBrushProcess(
          QCoreApplication::applicationFilePath(), ownedMember(relative), error);
      require(error.isEmpty() && imported.complete && imported.records.size() == 1,
              "GUI-installed collection member failed packaged worker reopen");
      const auto &record = imported.records.front();
      const QString name = QString::fromStdString(record.preset.name);
      const int ordinal = names.indexOf(name);
      require(ordinal >= 0 && record.preset.profile == "groundzero-native-1" &&
                  record.preset.size.value == 12 + ordinal * 8 &&
                  record.provenance.value("sourcePresetOrdinal").toInt() == ordinal + 1 &&
                  record.provenance.value("conversionKind") == "static-native" &&
                  !record.provenance.value("sourceReferenceVerified").toBool() &&
                  record.preset.tip.images.size() == 1 && record.preset.tip.images.front(),
              "GUI-installed collection lost its original identity or native-conversion boundary");
      const auto &image = *record.preset.tip.images.front();
      require(image.width == 2 && image.height == 2 && !image.color && image.pixels.size() == 4,
              "GUI-installed collection lost its original mask resource");
      const int alpha[]{0, 64, 128, 255};
      for (size_t index = 0; index < image.pixels.size(); ++index)
        require(image.pixels[index].r == 0 && image.pixels[index].g == 0 &&
                    image.pixels[index].b == 0 && image.pixels[index].a == alpha[index] / 255.0,
                "GUI-installed collection changed the original sampled alpha");
      const auto resources = RasterBrush::resources(record, error);
      require(error.isEmpty() && resources.size() == 1 &&
                  (commonResource.isEmpty() || commonResource == resources.front().hash),
              "shared collection sample changed its content identity after installation");
      commonResource = resources.front().hash;
      actualNames << name;
    }
    auto expected = expectedNames;
    actualNames.sort(); expected.sort();
    require(sameFiles(actualNames, expected), "GUI installed unchecked presets or lost checked presets");
  };
  readback(subset, {names[0], names[2]});

  checkpoint("selecting a GUI-installed collection member through the real browser");
  const auto target = std::find_if(subset.cbegin(), subset.cend(), [](const QString &path) {
    return QFileInfo(path).fileName().startsWith("Original_GUI_Collection_Third-");
  });
  require(target != subset.cend(), "checked third preset is missing from the actual selector");
  const QString browseTarget = *target;
  {
    QObject context;
    bool seen = false, failed = false;
    QElapsedTimer deadline; deadline.start();
    QTimer driver;
    QObject::connect(&driver, &QTimer::timeout, &context, [&] {
      auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
      if (!dialog) return;
      if (deadline.elapsed() > 15000) { failed = true; dialog->reject(); return; }
      if (dialog->windowTitle() == "Import Raster Brush") return; // Selected members use the installed worker too.
      if (dialog->windowTitle() != "Find Native Brush") { failed = true; dialog->reject(); return; }
      if (seen) return;
      seen = true;
      auto list = dialog->findChild<QListWidget *>();
      auto buttons = dialog->findChild<QDialogButtonBox *>();
      QListWidgetItem *chosen = nullptr;
      if (list) for (int index = 0; index < list->count(); ++index) {
        const auto path = list->item(index)->data(Qt::UserRole).toString();
        if (RasterBrush::isLibraryStagingPath(path)) failed = true;
        if (path == browseTarget) chosen = list->item(index);
      }
      if (!chosen || !buttons || !buttons->button(QDialogButtonBox::Ok) || failed) {
        failed = true; dialog->reject(); return;
      }
      list->setCurrentItem(chosen);
      buttons->button(QDialogButtonBox::Ok)->click();
    });
    driver.start(10);
    property->setValue(L"<browse>");
    const bool handled = tool->onPropertyChanged("Native Brush:");
    driver.stop();
    require(handled && seen && !failed && property->getValue() == browseTarget.toStdWString(),
            "actual brush browser did not select a GUI-installed collection member");
    tap(tool);
    require(!same(baseline, raster(level, 1)) && undo->getHistoryCount() == 1 &&
                undo->undo() && same(baseline, raster(level, 1)),
            "GUI-selected collection brush failed real painting or exact undo");
    undo->reset();
  }
  auto selectMember = [&](const std::wstring &member) {
    QObject context;
    bool expired = false;
    QString warning;
    QElapsedTimer deadline; deadline.start();
    QTimer driver;
    QObject::connect(&driver, &QTimer::timeout, &context, [&] {
      auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
      if (deadline.elapsed() > 15000) {
        expired = true;
        if (dialog) dialog->reject();
        return;
      }
      if (!dialog || dialog->windowTitle() == "Import Raster Brush") return;
      QStringList text;
      if (auto message = qobject_cast<QMessageBox *>(dialog))
        text << message->text() << message->detailedText();
      for (auto label : dialog->findChildren<QLabel *>()) text << label->text();
      if (warning.isEmpty()) warning = text.join("\n");
      dialog->reject();
    });
    driver.start(10);
    property->setValue(member);
    const bool handled = tool->onPropertyChanged("Native Brush:");
    driver.stop();
    require(handled && !expired && warning.isEmpty() && property->getValue() == member,
            qPrintable("actual collection member selection failed: " + warning));
  };
  const auto beforeAll = property->getRange();
  run(Action::All);
  const auto all = added(beforeAll, property->getRange());
  require(all.size() == 3, "full GUI collection import did not add exactly three presets");
  readback(all, names);
  for (const auto &member : all) {
    selectMember(member.toStdWString());
    tap(tool);
    require(!same(baseline, raster(level, 1)) && undo->getHistoryCount() == 1 &&
                undo->undo() && same(baseline, raster(level, 1)),
            "a selected collection member failed actual painting or exact undo");
    undo->reset();
  }
  selectMember(browseTarget.toStdWString());

  checkpoint("rolling back a real collection reimport against a conflicting installed winner");
  // Only a newly installed, read-back fixture member may be changed. The
  // restoration guard is armed before writing, including assertion failures.
  const QString corruptPath = ownedMember(all.front());
  QString error;
  const auto original = RasterBrush::readBoundedRegularFile(corruptPath, 64 * 1024 * 1024, error);
  require(error.isEmpty() && !original.isEmpty(), "cannot retain original fixture member for restoration");
  auto writeOwned = [&](const QByteArray &bytes) {
    if (ownedMember(all.front()) != corruptPath) return false;
    QSaveFile file(corruptPath);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
  };
  struct RestoreMember {
    std::function<bool()> restore;
    bool armed = true;
    ~RestoreMember() {
      if (armed) try {
        if (!restore()) std::cerr << "collection fixture could not restore its owned member" << std::endl;
      } catch (...) { std::cerr << "collection fixture member restoration failed" << std::endl; }
    }
    bool finish() { if (!restore()) return false; armed = false; return true; }
  } restore{[&] { return writeOwned(original); }};
  require(writeOwned("original GUI fixture conflicting winner\n"),
          "cannot author owned collection content conflict");
  const auto conflictFiles = collectionLibrarySnapshot(library);
  const auto conflictRange = property->getRange();
  const auto conflictSelection = property->getValue();
  const auto conflict = run(Action::All, true);
  require(conflict.worker && conflict.chooser && conflict.acceptance &&
              collectionLibrarySnapshot(library) == conflictFiles &&
              property->getRange() == conflictRange && property->getValue() == conflictSelection,
          "failed collection publication overwrote winner bytes, changed selection or left staged members");
  require(restore.finish(), "cannot restore original owned collection member after conflict");
  const auto restoredFiles = collectionLibrarySnapshot(library);
  run(Action::All);
  require(collectionLibrarySnapshot(library) == restoredFiles && property->getRange() == conflictRange,
          "GUI collection retry duplicated or rewrote the restored original collection");
  readback(all, names);
  selectMember(originalSelection);
  require(property->getValue() == originalSelection &&
              same(baseline, raster(level, 1)) && undo->getHistoryCount() == 0,
          "collection UI fixture did not restore original selection and document state");
}
} // namespace
