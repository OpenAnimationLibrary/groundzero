#include "golden.h"

#include "tregion.h"
#include "trasterimage.h"
#include "tstroke.h"
#include "ttoonzimage.h"
#include "tundo.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QSysInfo>
#include <QTextStream>

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace brushstroke {

namespace {

// 17 significant digits read back as the same double.
std::string number(double value) {
  return QString::number(value, 'g', 17).toStdString();
}

void recordRegion(Record& record, const std::string& label,
                  const TRegion& region, int depth) {
  const TRectD box = region.getBBox();
  record.lines.push_back(
      label + " region depth=" + std::to_string(depth) +
      " style=" + std::to_string(region.getStyle()) + " subregions=" +
      std::to_string(region.getSubregionCount()) + " box " + number(box.x0) +
      " " + number(box.y0) + " " + number(box.x1) + " " + number(box.y1));
  for (UINT i = 0; i < region.getSubregionCount(); ++i)
    recordRegion(record, label, *region.getSubregion(i), depth + 1);
}

// A pixel as one number that does not depend on the byte order.
quint32 packed(const TPixel32& pixel) {
  return quint32(pixel.m) << 24 | quint32(pixel.r) << 16 |
         quint32(pixel.g) << 8 | quint32(pixel.b);
}

quint32 packed(const TPixelCM32& pixel) {
  return quint32(pixel.getInk()) << 20 | quint32(pixel.getPaint()) << 8 |
         quint32(pixel.getTone());
}

template <class RasterP>
QImage packedImage(const RasterP& raster) {
  QImage image(raster->getLx(), raster->getLy(), QImage::Format_ARGB32);
  for (int y = 0; y < raster->getLy(); ++y) {
    QRgb* line = reinterpret_cast<QRgb*>(image.scanLine(y));
    for (int x = 0; x < raster->getLx(); ++x)
      line[x] = packed(raster->pixels(y)[x]);
  }
  return image;
}

std::string digest(const QImage& image) {
  QCryptographicHash hash(QCryptographicHash::Sha256);
  for (int y = 0; y < image.height(); ++y) {
    const QRgb* line = reinterpret_cast<const QRgb*>(image.constScanLine(y));
    for (int x = 0; x < image.width(); ++x) {
      const char bytes[4] = {char(line[x] >> 24), char(line[x] >> 16),
                             char(line[x] >> 8), char(line[x])};
      hash.addData(bytes, 4);
    }
  }
  return hash.result().toHex().toStdString();
}

int countPainted(const QImage& image, quint32 empty) {
  int count = 0;
  for (int y = 0; y < image.height(); ++y) {
    const QRgb* line = reinterpret_cast<const QRgb*>(image.constScanLine(y));
    for (int x = 0; x < image.width(); ++x)
      if (line[x] != empty) ++count;
  }
  return count;
}

void recordPacked(Record& record, const std::string& label, const char* kind,
                  const QImage& image, const TRect& savebox, quint32 empty) {
  record.lines.push_back(
      label + " raster kind=" + kind + " size=" +
      std::to_string(image.width()) + "x" + std::to_string(image.height()) +
      " savebox=" + std::to_string(savebox.x0) + "," +
      std::to_string(savebox.y0) + "," + std::to_string(savebox.x1) + "," +
      std::to_string(savebox.y1) + " painted=" +
      std::to_string(countPainted(image, empty)) + " sha256=" + digest(image));
  record.images.push_back(image);
}

QString textPath(const QString& dir, const std::string& name) {
  return dir + "/" + QString::fromStdString(name) + ".txt";
}

QString imagePath(const QString& dir, const std::string& name, int index) {
  return dir + "/" + QString::fromStdString(name) + "." +
         QString::number(index) + ".png";
}

bool isNumber(const QString& token, double& value) {
  bool ok = false;
  value   = token.toDouble(&ok);
  return ok;
}

bool closeEnough(double a, double b) {
  return std::fabs(a - b) <= 1e-6 + 1e-9 * std::max(std::fabs(a), std::fabs(b));
}

// "<label> raster ..." or "afterUndo <label> raster ...": a line that has an
// image.
bool isRasterLine(const QString& line) {
  return line.contains(" raster kind=");
}

QString field(const QString& line, const QString& key) {
  for (const QString& token : line.split(' '))
    if (token.startsWith(key + "=")) return token.mid(key.size() + 1);
  return QString();
}

bool tonesClose(int a, int b) { return std::abs(a - b) <= 4; }

bool pixelClose(QRgb a, QRgb b, bool colourMap) {
  if (a == b) return true;
  if (!colourMap) {
    for (int shift = 0; shift < 32; shift += 8)
      if (!tonesClose((a >> shift) & 0xff, (b >> shift) & 0xff)) return false;
    return true;
  }
  const int toneA = a & 0xff, toneB = b & 0xff;
  if (!tonesClose(toneA, toneB)) return false;
  if (((a >> 8) & 0xfff) != ((b >> 8) & 0xfff)) return false;
  // The ink of a pixel no ink covers is not visible.
  return (a >> 20) == (b >> 20) || (toneA >= 251 && toneB >= 251);
}

// The raster line of a tolerant comparison: same kind and size, savebox
// within a pixel, and nearly all pixels within the tolerance.
void compareRaster(const QString& expected, const QString& actual,
                   const QImage& expectedImage, const QImage& actualImage,
                   Comparison& result) {
  const std::string label =
      expected.left(expected.indexOf(" raster kind=")).toStdString();
  if (field(expected, "kind") != field(actual, "kind") ||
      field(expected, "size") != field(actual, "size") ||
      expectedImage.size() != actualImage.size()) {
    result.problems.push_back(label + ": raster kind or size differs");
    return;
  }
  const QStringList boxA = field(expected, "savebox").split(',');
  const QStringList boxB = field(actual, "savebox").split(',');
  for (int i = 0; i < 4; ++i)
    if (std::abs(boxA.value(i).toInt() - boxB.value(i).toInt()) > 1) {
      result.problems.push_back(
          label + ": savebox " + field(actual, "savebox").toStdString() +
          ", expected " + field(expected, "savebox").toStdString());
      return;
    }
  const bool colourMap = field(expected, "kind") == "cm32";
  int different = 0, far = 0;
  for (int y = 0; y < expectedImage.height(); ++y) {
    const QRgb* a =
        reinterpret_cast<const QRgb*>(expectedImage.constScanLine(y));
    const QRgb* b = reinterpret_cast<const QRgb*>(actualImage.constScanLine(y));
    for (int x = 0; x < expectedImage.width(); ++x) {
      if (a[x] != b[x]) ++different;
      if (!pixelClose(a[x], b[x], colourMap)) ++far;
    }
  }
  const int allowed = std::max(2, field(expected, "changed").toInt() / 100);
  if (far > allowed)
    result.problems.push_back(label + ": " + std::to_string(far) +
                              " pixels differ by more than the tolerance (" +
                              std::to_string(allowed) + " allowed)");
  else if (different)
    result.note += " " + label + ": " + std::to_string(different) +
                   " pixels differ, " + std::to_string(far) +
                   " beyond 4 levels;";
}

bool tokensMatch(const QString& expected, const QString& actual) {
  const QStringList a = expected.split(' '), b = actual.split(' ');
  if (a.size() != b.size()) return false;
  for (int i = 0; i < a.size(); ++i) {
    if (a[i] == b[i]) continue;
    double x, y;
    if (!isNumber(a[i], x) || !isNumber(b[i], y) || !closeEnough(x, y))
      return false;
  }
  return true;
}

}  // namespace

//-----------------------------------------------------------------------------

void recordVector(Record& record, const std::string& label,
                  const TVectorImageP& image) {
  require(image, label + ": the drawing is not a vector image");
  record.lines.push_back(
      label + " strokes=" + std::to_string(image->getStrokeCount()) +
      " regions=" + std::to_string(image->getRegionCount()) +
      " insideGroup=" + std::to_string(image->isInsideGroup()));
  for (UINT i = 0; i < image->getStrokeCount(); ++i) {
    const TStroke* stroke                  = image->getStroke(i);
    const TStroke::OutlineOptions& options = stroke->outlineOptions();
    record.lines.push_back(
        label + " stroke " + std::to_string(i) +
        " style=" + std::to_string(stroke->getStyle()) +
        " loop=" + std::to_string(stroke->isSelfLoop()) +
        " cap=" + std::to_string(options.m_capStyle) +
        " join=" + std::to_string(options.m_joinStyle) + " miter=" +
        number(options.m_miterLower) + "/" + number(options.m_miterUpper) +
        " group=" + std::to_string(image->getGroupDepth(i)) +
        " points=" + std::to_string(stroke->getControlPointCount()));
    for (int p = 0; p < stroke->getControlPointCount(); ++p) {
      const TThickPoint point = stroke->getControlPoint(p);
      record.lines.push_back(label + " point " + number(point.x) + " " +
                             number(point.y) + " " + number(point.thick));
    }
  }
  for (UINT i = 0; i < image->getRegionCount(); ++i)
    recordRegion(record, label, *image->getRegion(i), 0);
}

void recordRaster(Record& record, const std::string& label,
                  const TImageP& image) {
  if (TToonzImageP toonz = image) {
    recordPacked(record, label, "cm32", packedImage(toonz->getRaster()),
                 toonz->getSavebox(), packed(TPixelCM32()));
    return;
  }
  TRasterImageP raster = image;
  TRaster32P pixels = raster ? TRaster32P(raster->getRaster()) : TRaster32P();
  require(pixels, label + ": the drawing is not a 32 bit raster image");
  recordPacked(record, label, "rgbm32", packedImage(pixels),
               raster->getSavebox(), 0);
}

namespace {
Record levelState(Fixture& fixture) {
  Record state;
  std::vector<TFrameId> fids;
  fixture.level()->getFids(fids);
  for (const TFrameId& fid : fids) {
    const TImageP image     = fixture.level()->getFrame(fid, false);
    const std::string label = "frame" + std::to_string(fid.getNumber());
    if (TVectorImageP vector = image)
      recordVector(state, label, vector);
    else
      recordRaster(state, label, image);
  }
  return state;
}
}  // namespace

Record snapshot(Fixture& fixture) { return levelState(fixture); }

Record changedFrom(Record state, const Record& reference) {
  size_t image = 0;
  for (std::string& line : state.lines) {
    if (!isRasterLine(QString::fromStdString(line))) continue;
    const size_t index = image++;
    if (index >= reference.images.size() ||
        reference.images[index].size() != state.images[index].size())
      continue;
    const QImage &a = reference.images[index], &b = state.images[index];
    int changed = 0;
    for (int y = 0; y < a.height(); ++y) {
      const QRgb* p = reinterpret_cast<const QRgb*>(a.constScanLine(y));
      const QRgb* q = reinterpret_cast<const QRgb*>(b.constScanLine(y));
      for (int x = 0; x < a.width(); ++x)
        if (p[x] != q[x]) ++changed;
    }
    line += " changed=" + std::to_string(changed);
  }
  return state;
}

void append(Record& record, const Record& state, const std::string& prefix) {
  for (const std::string& line : state.lines)
    record.lines.push_back(prefix + line);
  record.images.insert(record.images.end(), state.images.begin(),
                       state.images.end());
}

void recordUndo(Record& record, Fixture& fixture, const Record& before) {
  TUndoManager* undo = TUndoManager::manager();
  const int steps    = undo->getHistoryCount();
  const Record after = levelState(fixture);
  bool undone = true, redone = true;
  for (int i = 0; i < steps; ++i) undone = undo->undo() && undone;
  const Record afterUndo = levelState(fixture);
  for (int i = 0; i < steps; ++i) redone = undo->redo() && redone;
  const Record afterRedo = levelState(fixture);
  record.lines.push_back("undo steps=" + std::to_string(steps) +
                         " undone=" + std::to_string(undone) + " restored=" +
                         std::to_string(afterUndo.lines == before.lines) +
                         " redone=" + std::to_string(redone) + " repeated=" +
                         std::to_string(afterRedo.lines == after.lines));
  if (afterUndo.lines != before.lines)
    append(record, changedFrom(afterUndo, after), "afterUndo ");
  if (afterRedo.lines != after.lines)
    append(record, changedFrom(afterRedo, before), "afterRedo ");
}

void recordChange(Record& record, const Record& before, const Record& after) {
  append(record, changedFrom(after, before), "");
}

//-----------------------------------------------------------------------------

QString platformKey() {
#if defined(_WIN32)
  const QString system = "windows";
#elif defined(__APPLE__)
  const QString system = "macos";
#else
  const QString system = "linux";
#endif
  return system + "-" + QSysInfo::buildCpuArchitecture() + "-qt" +
         QString::number(QT_VERSION_MAJOR);
}

bool goldenExists(const QString& dir, const std::string& name) {
  return QFile::exists(textPath(dir, name));
}

Record loadGolden(const QString& dir, const std::string& name) {
  Record record;
  QFile file(textPath(dir, name));
  require(file.open(QIODevice::ReadOnly | QIODevice::Text),
          "cannot read golden data " + textPath(dir, name).toStdString());
  QTextStream stream(&file);
  while (!stream.atEnd()) {
    const QString line = stream.readLine();
    if (line.isEmpty()) continue;
    record.lines.push_back(line.toStdString());
    if (!isRasterLine(line)) continue;
    const QString path = imagePath(dir, name, int(record.images.size()));
    QImage image(path);
    require(!image.isNull(), "cannot read golden image " + path.toStdString());
    record.images.push_back(image.convertToFormat(QImage::Format_ARGB32));
  }
  return record;
}

void saveGolden(const QString& dir, const std::string& name,
                const Record& record) {
  require(QDir().mkpath(dir), "cannot create " + dir.toStdString());
  QFile file(textPath(dir, name));
  require(file.open(QIODevice::WriteOnly | QIODevice::Truncate),
          "cannot write golden data " + textPath(dir, name).toStdString());
  QByteArray text;
  for (const std::string& line : record.lines)
    text += QByteArray::fromStdString(line) + '\n';
  require(file.write(text) == text.size(), "short write of golden data");
  for (int i = 0; i < int(record.images.size()); ++i)
    require(
        record.images[i].save(imagePath(dir, name, i), "PNG"),
        "cannot write golden image " + imagePath(dir, name, i).toStdString());
}

Comparison compare(const Record& expected, const Record& actual, Mode mode) {
  Comparison result;
  if (expected.lines.size() != actual.lines.size())
    result.problems.push_back(
        "the result has " + std::to_string(actual.lines.size()) +
        " lines, the golden data " + std::to_string(expected.lines.size()));
  const size_t count = std::min(expected.lines.size(), actual.lines.size());
  int image          = 0;
  for (size_t i = 0; i < count && result.problems.size() < 8; ++i) {
    const QString a   = QString::fromStdString(expected.lines[i]);
    const QString b   = QString::fromStdString(actual.lines[i]);
    const bool raster = isRasterLine(a) && isRasterLine(b);
    if (raster) ++image;
    if (a == b) continue;
    if (mode == Mode::Tolerant && raster &&
        image <= int(expected.images.size()) &&
        image <= int(actual.images.size())) {
      compareRaster(a, b, expected.images[image - 1], actual.images[image - 1],
                    result);
      continue;
    }
    if (mode == Mode::Tolerant && !raster && tokensMatch(a, b)) {
      if (result.note.find("numbers") == std::string::npos)
        result.note += " numbers differ within 1e-6;";
      continue;
    }
    result.problems.push_back("line " + std::to_string(i + 1) + ":\n    got  " +
                              actual.lines[i] + "\n    want " +
                              expected.lines[i]);
  }
  return result;
}

}  // namespace brushstroke
