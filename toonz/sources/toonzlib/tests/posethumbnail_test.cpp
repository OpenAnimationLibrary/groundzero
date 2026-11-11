// Regression test of pose thumbnails: a pose keeps its picture through save
// and load, and a pose without one, as saved by older versions, loads as
// before.

#include "toonz/poselibrary.h"
#include "tfilepath.h"
#include "tstream.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QImage>
#include <QTemporaryDir>

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

TFilePath toPath(const QString &path) { return TFilePath(path.toStdWString()); }

PoseLibrary roundTrip(const PoseLibrary &library, const QString &path) {
  {
    TOStream os(toPath(path));
    require(os.checkStatus(), "save: cannot open the file");
    os.openChild("poses");
    library.saveData(os);
    os.closeChild();
  }
  PoseLibrary loaded;
  TIStream is(toPath(path));
  require(bool(is), "load: cannot open the file");
  std::string tag;
  require(is.matchTag(tag) && tag == "poses", "load: no poses tag");
  loaded.loadData(is);
  return loaded;
}

}  // namespace

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  try {
    QTemporaryDir dir;
    require(dir.isValid(), "no temporary directory");

    QImage image(12, 8, QImage::Format_ARGB32);
    image.fill(QColor(200, 40, 10));
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    require(image.save(&buffer, "PNG"), "cannot encode the fixture image");

    PoseLibrary library;
    PoseLibrary::Pose pictured;
    pictured.m_name = "Wave";
    pictured.m_thumbnail = png;
    library.addPose(pictured);
    PoseLibrary::Pose plain;
    plain.m_name = "Rest";
    library.addPose(plain);

    const PoseLibrary loaded = roundTrip(library, dir.filePath("poses.xml"));
    require(loaded.getPoseCount() == 2, "a pose was lost");
    require(loaded.getPose(0).m_name == "Wave" &&
                loaded.getPose(0).m_thumbnail == png,
            "the thumbnail did not survive save and load");
    QImage decoded;
    require(decoded.loadFromData(loaded.getPose(0).m_thumbnail, "PNG") &&
                decoded.size() == image.size(),
            "the loaded thumbnail is not a readable PNG");
    require(loaded.getPose(1).m_name == "Rest" &&
                loaded.getPose(1).m_thumbnail.isEmpty(),
            "a pose without a thumbnail gained one");
    std::cout << "posethumbnail: PASS" << std::endl;
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "posethumbnail: FAIL: " << e.what() << std::endl;
    return 1;
  }
}
