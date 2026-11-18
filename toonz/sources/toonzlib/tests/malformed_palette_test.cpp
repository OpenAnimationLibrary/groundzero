// SPDX-License-Identifier: BSD-3-Clause

// Damaged palette files (.tpl) must fail to load with a TException, and a
// palette with a custom texture must load when no scene is open. The
// palettes are written by TPalette::saveData and edited the way a
// file-mutation probe found: a reference frame range of two billion frames,
// and a texture style whose path is in a folder.
//
//   malformed_palette_test            run every case
//   malformed_palette_test <case>...  run the named cases

#include "toonz/imagestyles.h"
#include "toonz/toonzscene.h"
#include "tconvert.h"
#include "tenv.h"
#include "texception.h"
#include "tfilepath.h"
#include "tnzimage.h"
#include "tpalette.h"
#include "tstream.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>

#include <algorithm>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

TFilePath toPath(const QString& path) { return TFilePath(path.toStdWString()); }

// As the studio palette reads a .tpl file
std::unique_ptr<TPalette> loadPalette(const QString& path) {
  TIStream is(toPath(path));
  require(bool(is), "cannot open " + path.toStdString());
  std::string tag;
  require(is.matchTag(tag) && tag == "palette", "no <palette> tag");
  std::unique_ptr<TPalette> palette(new TPalette());
  palette->loadData(is);
  is.matchEndTag();
  return palette;
}

QByteArray readFile(const QString& path) {
  QFile file(path);
  require(file.open(QIODevice::ReadOnly), "cannot read " + path.toStdString());
  return file.readAll();
}

QString writeFile(const QString& dir, const QString& name,
                  const QByteArray& text) {
  const QString path = QDir(dir).filePath(name + ".tpl");
  QFile file(path);
  require(file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
              file.write(text) == text.size(),
          "cannot write " + path.toStdString());
  return path;
}

// A palette with a color style and a reference level of frames 1-3 and 5
QByteArray makeSeed(const QString& dir) {
  TPaletteP palette(new TPalette());
  palette->getPage(0)->addStyle(palette->addStyle(TPixel32(200, 10, 10)));
  palette->setRefImgPath(TFilePath("reference..png"));
  palette->setRefLevelFids({TFrameId(1), TFrameId(2), TFrameId(3), TFrameId(5)},
                           true);
  const QString path = QDir(dir).filePath("seed.tpl");
  {
    TOStream os(toPath(path));
    std::map<std::string, std::string> attributes;
    attributes["name"] = "seed";
    os.openChild("palette", attributes);
    palette->saveData(os);
    os.closeChild();
  }
  return readFile(path);
}

QByteArray withFids(const QByteArray& seed, const QByteArray& fids) {
  const QByteArray saved = "fids=\"1-3,5\"";
  require(seed.contains(saved),
          "the saved palette has no " + saved.toStdString());
  return QByteArray(seed).replace(saved, "fids=\"" + fids + "\"");
}

// A texture style in a folder: its path is decoded through the scene. After
// the path: color, pattern, type, scale, rotation, displacement, contrast.
QByteArray withTexture(const QByteArray& seed, const QString& path) {
  const QByteArray styles = "<styles>";
  require(seed.contains(styles), "the saved palette has no <styles>");
  const QByteArray style = "<styles><style>texture_1 4 \"" + path.toUtf8() +
                           "\" 255 0 0 255 0 0 1 0 0 0 1</style>";
  return QByteArray(seed).replace(styles, style);
}

void expectRejected(const QString& dir, const QString& name,
                    const QByteArray& text) {
  std::string error;
  try {
    loadPalette(writeFile(dir, name, text));
  } catch (const TException& e) {
    error = ::to_string(e.getMessage());
  }
  require(!error.empty(), name.toStdString() + ": the damaged palette loaded");
  std::cout << "  " << name.toStdString() << ": rejected ("
            << error.substr(0, 60) << ")" << std::endl;
}

TTextureStyle* firstTexture(TPalette* palette) {
  for (int i = 0; i < palette->getStyleCount(); ++i)
    if (auto* texture = dynamic_cast<TTextureStyle*>(palette->getStyle(i)))
      return texture;
  return nullptr;
}

//-----------------------------------------------------------------------------
// Cases

struct Context {
  QString dir;
  QByteArray seed;
};

// <refImgPath fids="..."> lists the reference frames as ranges
void referenceFrames(const Context& c) {
  std::unique_ptr<TPalette> palette =
      loadPalette(writeFile(c.dir, "seed", c.seed));
  require(palette->getRefLevelFids().size() == 4,
          "the valid palette lost its reference frames");
  std::cout << "  seed: loaded" << std::endl;
  palette = loadPalette(
      writeFile(c.dir, "fids-limit", withFids(c.seed, "1-1000000")));
  require(palette->getRefLevelFids().size() == 1000000,
          "a million reference frames did not load");
  std::cout << "  fids-limit: loaded" << std::endl;
  expectRejected(c.dir, "fids-over", withFids(c.seed, "1-1000001"));
  expectRejected(c.dir, "fids-ranges-over",
                 withFids(c.seed, "1-600000,700001-1300000"));
  expectRejected(c.dir, "fids-2e9", withFids(c.seed, "1-2000000000"));
  expectRejected(c.dir, "fids-int-max", withFids(c.seed, "0-2147483647"));
  expectRejected(c.dir, "fids-dash", withFids(c.seed, "1,-"));
}

// Loads a palette with a texture style at path, with scene as the current
// scene, and checks the width of its texture: 128 is the empty placeholder.
void expectTexture(const Context& c, const QString& name, const QString& path,
                   ToonzScene* scene, int width) {
  TImageStyle::setCurrentScene(scene);
  std::unique_ptr<TPalette> palette;
  try {
    palette = loadPalette(writeFile(c.dir, name, withTexture(c.seed, path)));
  } catch (...) {
    TImageStyle::setCurrentScene(nullptr);
    throw;
  }
  TImageStyle::setCurrentScene(nullptr);
  TTextureStyle* texture = firstTexture(palette.get());
  require(texture && texture->getTexture() &&
              texture->getTexture()->getLx() == width,
          name.toStdString() + ": the texture is not " + std::to_string(width) +
              " pixels wide");
  std::cout << "  " << name.toStdString() << ": loaded" << std::endl;
}

// Custom textures resolve their folder through the current scene
void textures(const Context& c) {
  const QString image = QDir(c.dir).filePath("texture.png");
  QImage pixels(4, 4, QImage::Format_ARGB32);
  pixels.fill(Qt::red);
  require(pixels.save(image), "cannot write the texture image");

  expectTexture(c, "texture-absolute-no-scene", image, nullptr, 128);
  expectTexture(c, "texture-project-folder-no-scene", "+extras/texture.png",
                nullptr, 128);
  ToonzScene scene;
  expectTexture(c, "texture-with-scene", image, &scene, 4);
  // A sequence without files has no frame to take the image from
  expectTexture(c, "texture-empty-sequence",
                QDir(c.dir).filePath("missing..png"), &scene, 128);
}

struct Case {
  const char* name;
  void (*run)(const Context&);
};

const Case kCases[] = {{"fids", referenceFrames}, {"texture", textures}};

}  // namespace

int main(int argc, char* argv[]) {
  QCoreApplication application(argc, argv);
  std::vector<std::string> selected(argv + 1, argv + argc);
  int failures = 0;
  try {
    QTemporaryDir storage;
    require(storage.isValid(), "cannot create a temporary directory");
    const QString stuff = QDir(storage.path()).filePath("stuff");
    require(
        QDir().mkpath(QDir(stuff).filePath("profiles/layouts/rooms/Default")),
        "cannot create the stuff directory");
    TEnv::setStuffDir(toPath(stuff));
    initImageIo();
    Context context{storage.path(), makeSeed(storage.path())};
    for (const Case& c : kCases) {
      if (!selected.empty() &&
          std::find(selected.begin(), selected.end(), c.name) == selected.end())
        continue;
      std::cout << "case " << c.name << std::endl;
      try {
        c.run(context);
      } catch (const std::exception& error) {
        std::cerr << "FAIL " << c.name << ": " << error.what() << std::endl;
        ++failures;
      } catch (const TException& error) {
        std::cerr << "FAIL " << c.name << ": "
                  << ::to_string(error.getMessage()) << std::endl;
        ++failures;
      }
    }
  } catch (const std::exception& error) {
    std::cerr << "FAIL setup: " << error.what() << std::endl;
    ++failures;
  } catch (const TException& error) {
    std::cerr << "FAIL setup: " << ::to_string(error.getMessage()) << std::endl;
    ++failures;
  }
  std::cout << (failures ? "malformed_palette: FAILED"
                         : "malformed_palette: PASS")
            << std::endl;
  return failures ? 1 : 0;
}
