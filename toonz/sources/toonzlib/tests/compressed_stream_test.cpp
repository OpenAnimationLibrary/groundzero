// SPDX-License-Identifier: BSD-3-Clause

// Round-trip test of the compressed scene file format: TOStream written with
// compression (an LZ4 frame behind the "TABc" header, see tstream.h) and read
// back by TIStream, the reader of ToonzScene::loadTnzFile, which recognizes it
// from the magic. The tags and values read from the compressed file must
// equal those of the same document written plain. It also checks that
// damaged compressed files are rejected instead of hanging or reading
// uninitialised memory.
//
// Built by the CMake target compressed_stream_test (BUILD_STREAM_TESTS), or by
// hand against the built libraries, for example on Linux:
//   g++ -std=c++17 -fPIC -I toonz/sources/include -I toonz/sources/toonzqt \
//       $(pkg-config --cflags Qt5Core) compressed_stream_test.cpp \
//       -L<build>/lib -ltnzbase -ltnzcore $(pkg-config --libs Qt5Core)

#include "tstream.h"
#include "tstreamlimits.h"
#include "texception.h"
#include "tfilepath.h"

#include <QByteArray>
#include <QFile>
#include <QString>
#include <QTemporaryDir>

#include <iostream>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

const int kItemCount = 20000;  // large enough for several LZ4 blocks

void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

TFilePath toPath(const QString &path) { return TFilePath(path.toStdWString()); }

// A document shaped like a scene: attributes on the root, text and numbers,
// nested tags, and a long list.
void writeDocument(const TFilePath &fp, bool compressed) {
  TOStream os(fp, compressed);
  require(os.checkStatus(), "cannot open the output stream");

  std::map<std::string, std::string> attr;
  attr["version"]    = "71.0";
  attr["framecount"] = "12";
  os.openChild("tnz", attr);
  os.child("generator") << std::string("compressed_stream_test");
  os.openChild("properties");
  os.child("name") << std::string("a \"quoted\" name");
  os.child("ratio") << 0.25;
  os.closeChild();
  os.openChild("items");
  for (int i = 0; i < kItemCount; i++) os.child("item") << i * 7;
  os.closeChild();
  os.closeChild();
}

// Flattens the document into "tag/subtag=value" lines.
void readTags(TIStream &is, const std::string &parent,
              std::vector<std::string> &out) {
  std::string tag;
  while (is.matchTag(tag)) {
    std::string path = parent + "/" + tag;
    if (tag == "properties" || tag == "items") {
      readTags(is, path, out);
    } else if (tag == "item") {
      int value;
      is >> value;
      out.push_back(path + "=" + std::to_string(value));
    } else if (tag == "ratio") {
      double value;
      is >> value;
      out.push_back(path + "=" + std::to_string(value));
    } else if (tag == "generator" || tag == "name") {
      std::string value;
      is >> value;
      out.push_back(path + "=" + value);
    } else
      throw std::runtime_error("unexpected tag " + tag);
    if (!is.matchEndTag()) throw std::runtime_error("missing end tag " + tag);
  }
}

std::vector<std::string> readDocument(const TFilePath &fp) {
  std::vector<std::string> out;
  TIStream is(fp);
  require(bool(is), "cannot open the input stream");

  std::string tag;
  require(is.matchTag(tag) && tag == "tnz", "the root tag is not tnz");
  std::string version;
  int frameCount = 0;
  require(is.getTagParam("version", version) && version == "71.0",
          "the version attribute is lost");
  require(is.getTagParam("framecount", frameCount) && frameCount == 12,
          "the framecount attribute is lost");
  out.push_back("tnz version=" + version +
                " framecount=" + std::to_string(frameCount));
  readTags(is, "tnz", out);
  require(is.matchEndTag(), "the root tag is not closed");
  return out;
}

QByteArray readBytes(const QString &path) {
  QFile f(path);
  require(f.open(QIODevice::ReadOnly), "cannot read a file");
  return f.readAll();
}

void writeBytes(const QString &path, const QByteArray &bytes) {
  QFile f(path);
  require(f.open(QIODevice::WriteOnly | QIODevice::Truncate),
          "cannot write a file");
  f.write(bytes);
}

bool isRejected(const QString &path) {
  try {
    readDocument(toPath(path));
  } catch (const TException &) {
    return true;
  }
  return false;
}

void run() {
  QTemporaryDir dir;
  require(dir.isValid(), "no temporary directory");
  const QString plainPath = dir.filePath("plain.tnz");
  const QString lz4Path   = dir.filePath("compressed.tnz");

  writeDocument(toPath(plainPath), false);
  writeDocument(toPath(lz4Path), true);

  // The compressed file has the documented header and is smaller
  QByteArray plain = readBytes(plainPath), lz4 = readBytes(lz4Path);
  require(plain.startsWith('<'), "a plain file must start with a tag");
  require(lz4.startsWith("TABc"), "the compressed file lacks the magic");
  require(lz4.size() > 16 && lz4.size() < plain.size() / 4,
          "the compressed file is not smaller");

  // Both read back to the same document, with the expected content
  std::vector<std::string> fromPlain = readDocument(toPath(plainPath));
  std::vector<std::string> fromLz4   = readDocument(toPath(lz4Path));
  require(fromPlain == fromLz4, "the compressed file reads back differently");
  require(fromLz4.size() == size_t(kItemCount) + 4, "unexpected tag count");
  require(fromLz4[1] == "tnz/generator=compressed_stream_test",
          "the generator text is lost");
  require(fromLz4[2] == "tnz/properties/name=a \"quoted\" name",
          "the quoted text is lost");
  require(fromLz4.back() ==
              "tnz/items/item=" + std::to_string((kItemCount - 1) * 7),
          "the last item is lost");

  // Damaged files are rejected
  const QString badPath = dir.filePath("bad.tnz");

  writeBytes(badPath, lz4.left(lz4.size() / 2));
  require(isRejected(badPath), "a truncated payload was accepted");

  // Every incomplete modern header, including magic alone, must fail before
  // the reader interprets its lengths or byte-order marker.
  for (int length = 1; length < 16; ++length) {
    writeBytes(badPath, lz4.left(length));
    require(isRejected(badPath), "a truncated TABc header was accepted");
  }

  // The old format stores native-size lengths. Retain its successful read
  // path while checking each possible truncation of those fields as well.
  QByteArray legacy("TNZC", 4);
  const size_t legacyOutputSize = size_t(plain.size());
  const size_t legacyInputSize  = size_t(lz4.size() - 16);
  legacy.append(reinterpret_cast<const char *>(&legacyOutputSize),
                sizeof(size_t));
  legacy.append(reinterpret_cast<const char *>(&legacyInputSize),
                sizeof(size_t));
  const int legacyHeaderSize = legacy.size();
  legacy.append(lz4.mid(16));
  writeBytes(badPath, legacy);
  require(readDocument(toPath(badPath)) == fromPlain,
          "the legacy compressed format reads back differently");
  for (int length = 1; length < legacyHeaderSize; ++length) {
    writeBytes(badPath, legacy.left(length));
    require(isRejected(badPath), "a truncated TNZC header was accepted");
  }

  // A valid foreign-endian header must still work. An unknown marker must
  // not be treated as an instruction to swap otherwise plausible sizes.
  QByteArray swapped = lz4;
  for (int field = 4; field < 16; field += 4)
    for (int byte = 0; byte < 4; ++byte)
      swapped[field + byte] = lz4[field + 3 - byte];
  writeBytes(badPath, swapped);
  require(readDocument(toPath(badPath)) == fromPlain,
          "the foreign-endian compressed format reads back differently");
  swapped.replace(4, 4, QByteArray(4, '\0'));
  writeBytes(badPath, swapped);
  require(isRejected(badPath), "an invalid byte-order marker was accepted");

  // A declared decompressed size smaller than the data (the bytes 8..11 hold
  // it in host order): used to loop forever
  QByteArray shortSize = lz4;
  shortSize.replace(8, 4, QByteArray(4, '\0'));
  shortSize[8] = 1;
  writeBytes(badPath, shortSize);
  require(isRejected(badPath), "a too small decompressed size was accepted");

  // A declared decompressed size that the payload cannot reach
  QByteArray hugeSize = lz4;
  hugeSize.replace(8, 4, QByteArray("\xff\xff\xff\x3f", 4));
  writeBytes(badPath, hugeSize);
  require(isRejected(badPath), "an impossible decompressed size was accepted");

  // Exercise the same bounds used by scene publication without allocating
  // huge rasters/documents. A plausible compressed length must not permit a
  // declared document larger than the supported format limit.
  require(TStreamLimits::validCompressedSizes(2000000, 1u << 28),
          "the maximum supported document size was rejected");
  require(!TStreamLimits::validCompressedSizes(2000000, (1u << 28) + 1),
          "scene publication would accept an unreadable document size");
  require(!TStreamLimits::validCompressedSizes(100000001, 1),
          "scene publication would accept an unreadable payload size");
  QByteArray oversized = lz4;
  const TINT32 excessiveDocument = (1u << 28) + 1;
  std::memcpy(oversized.data() + 8, &excessiveDocument, sizeof(TINT32));
  writeBytes(badPath, oversized);
  require(isRejected(badPath), "an oversized document header was accepted");
}

}  // namespace

int main() {
  try {
    run();
  } catch (const std::exception &e) {
    std::cerr << "FAILED: " << e.what() << "\n";
    return 1;
  }
  std::cout << "compressed_stream_test passed\n";
  return 0;
}
