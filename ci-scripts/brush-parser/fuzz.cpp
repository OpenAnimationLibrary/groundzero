#include "toonz/rasterbrushimport.h"
#include <QCoreApplication>
#include <cstdint>
#include <cstdlib>

extern "C" int LLVMFuzzerInitialize(int *, char ***) {
  static int argc = 1;
  static char name[] = "brush-import-fuzz";
  static char *argv[] = {name, nullptr};
  static QCoreApplication application(argc, argv);
  return 0;
}
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, size_t size) {
  if (size < 2 || size > 65536)
    return 0;
  static const char *formats[] = {"gzbrush", "kpp",    "gbr",
                                  "gih",     "bundle", "abr"};
  QString error;
  auto result = RasterBrush::importBrush(
      QByteArray(reinterpret_cast<const char *>(data + 1), int(size - 1)),
      QString::fromLatin1(formats[data[0] % 6]), error);
  if (!error.isEmpty() && !result.records.empty())
    std::abort();
  for (const auto &record : result.records) {
    if (record.referenceVerified ||
        !RasterBrush::validate(record.preset).empty())
      std::abort();
    auto bytes = RasterBrush::encode(record, error);
    if (error.isEmpty()) {
      auto restored = RasterBrush::decode(bytes, error);
      if (!error.isEmpty() || !restored.semanticallySupported ||
          restored.referenceVerified)
        std::abort();
    }
  }
  return 0;
}
