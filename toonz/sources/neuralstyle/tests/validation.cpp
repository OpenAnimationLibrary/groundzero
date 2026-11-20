#include "toonz/neuralstyle.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <iostream>
#include <stdexcept>

namespace {
std::atomic<int> warningCount{0};

void recordMessage(QtMsgType type, const QMessageLogContext&,
                   const QString& message) {
  std::cerr << message.toStdString() << '\n';
  if (type != QtDebugMsg && type != QtInfoMsg) ++warningCount;
}

class MessageRecorder {
public:
  MessageRecorder() : previous(qInstallMessageHandler(recordMessage)) {}
  ~MessageRecorder() { qInstallMessageHandler(previous); }

private:
  QtMessageHandler previous;
};

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

QImage pixels(int side, QRgb color) {
  QImage image(side, side, QImage::Format_ARGB32);
  image.fill(color);
  return image;
}

void rejected(NeuralStyle::Session& session, const QImage& reference,
              const std::vector<QImage>& content, const QString& expected) {
  const QImage sentinel = pixels(3, qRgb(12, 34, 56));
  std::vector<QImage> result{sentinel};
  QString error;
  require(!session.styleBatch(reference, content, result, error),
          "Native failure unexpectedly succeeded");
  require(error == expected, qPrintable("Wrong native diagnostic: " + error));
  require(result.size() == 1 && result.front() == sentinel,
          "Failure published or cleared caller output");
}

void missingPackage(const QString& root) {
  const QImage reference = pixels(256, qRgb(22, 44, 66));
  const QImage content   = pixels(384, qRgb(88, 99, 111));
  const QString path     = QDir(root).filePath("not-installed");
  NeuralStyle::Session session(path);
  rejected(session, reference, {content},
           "Neural style component is not installed.");
  rejected(session, reference, {content},
           "Neural style component is not installed.");
  require(!QFile::exists(path),
          "Missing-component failure created component files");
  std::cout << "native missing component rejects repeatedly without result "
               "mutation PASS\n";
}

void imageValidation(const QString& root) {
  const QImage reference = pixels(256, qRgb(22, 44, 66));
  const QImage content   = pixels(384, qRgb(88, 99, 111));
  NeuralStyle::Session session(QDir(root).filePath("absent"));
  const QString sizeError =
      "Neural inputs require a 256-square reference and one to 120 frames.";
  rejected(session, pixels(255, Qt::black), {content}, sizeError);
  rejected(session, reference, {}, sizeError);
  rejected(session, reference, std::vector<QImage>(121, content), sizeError);
  rejected(session, reference, {pixels(383, Qt::black)},
           "Neural frames must be 384 square.");
  rejected(session, pixels(256, Qt::transparent), {content},
           "Composite neural inputs onto their neutral background before "
           "inference.");
  std::cout
      << "native shape, batch and alpha validation before package use PASS\n";
}

void writeManifest(const QString& directory, const QJsonObject& object) {
  QFile file(QDir(directory).filePath("package.json"));
  require(file.open(QIODevice::WriteOnly),
          "Create private invalid metadata fixture");
  const QByteArray bytes = QJsonDocument(object).toJson();
  require(file.write(bytes) == bytes.size(), "Write invalid manifest fixture");
}

void packageValidation(const QString& root) {
  const QImage reference = pixels(256, qRgb(22, 44, 66));
  const QImage content   = pixels(384, qRgb(88, 99, 111));
  NeuralStyle::Session session(root);
  writeManifest(root, {{"version", 99}});
  rejected(session, reference, {content},
           "Unsupported neural style component version.");
  QJsonObject manifest{
      {"version", 1}, {"protocol", 1}, {"runtimeVersion", "2.3.0"}};
  writeManifest(root, manifest);
  rejected(session, reference, {content}, "Invalid neural component size.");
  manifest["worker"] = QJsonObject{
      {"bytes", 4}, {"file", "../outside"}, {"sha256", QString(64, '0')}};
  writeManifest(root, manifest);
  rejected(session, reference, {content}, "Invalid neural component manifest.");
  manifest["worker"] = QJsonObject{
      {"bytes", 4}, {"file", "missing-worker"}, {"sha256", QString(64, '0')}};
  writeManifest(root, manifest);
  rejected(
      session, reference, {content},
      "Neural component is missing or has changed. Reinstall the component.");
  std::cout
      << "native unsupported, incomplete and unsafe component metadata PASS\n";
}

void cancellation(const QString& root) {
  NeuralStyle::Session session(root);
  const QImage sentinel = pixels(3, qRgb(12, 34, 56));
  QImage result         = sentinel;
  QString error;
  std::atomic_bool cancel{true};
  require(!session.style(pixels(256, Qt::black), pixels(384, Qt::white), result,
                         error, &cancel),
          "Pre-cancelled native request unexpectedly succeeded");
  require(error == "Neural styling cancelled." && result == sentinel,
          "Pre-cancelled request changed output or diagnostic");
  std::cout << "native pre-cancellation leaves output untouched PASS\n";
}
}  // namespace

int main(int argc, char** argv) {
  MessageRecorder messages;
  QCoreApplication application(argc, argv);
  try {
    QTemporaryDir root;
    require(root.isValid(), "Private synthetic fixture root exists");
    missingPackage(root.path());
    imageValidation(root.path());
    packageValidation(root.path());
    cancellation(root.path());
    require(warningCount.load() == 0,
            "Native validation and idle shutdown must not emit Qt warnings");
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "native neural validation: " << error.what() << '\n';
    return 1;
  }
}
