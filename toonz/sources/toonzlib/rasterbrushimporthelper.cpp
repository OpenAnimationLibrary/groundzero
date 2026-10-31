#include "toonz/rasterbrushimport.h"
#include <QCoreApplication>
#include <cstdio>
int main(int argc, char **argv) {
  QCoreApplication application(argc, argv);
  const auto args = application.arguments();
#ifdef __APPLE__
  if (args.size() == 4 && args.at(1) == "--raster-brush-import-stream")
    return RasterBrush::runMacImportStream(args.at(2), args.at(3),
                                           RasterBrush::runImportWorker);
#elif defined(_WIN32)
  if (args.size() == 3 && args.at(1) == "--qualify-brush-import") {
    QString error;
    const auto result = RasterBrush::importBrushProcess(
        application.applicationFilePath(), args.at(2), error);
    if (!error.isEmpty() || !result.complete || result.records.empty()) {
      std::fprintf(stderr, "%s\n", error.toUtf8().constData());
      return 1;
    }
    return 0;
  }
  if (args.size() == 4 && args.at(1) == "--raster-brush-import-worker")
    return RasterBrush::runImportWorker(args.at(2), args.at(3));
#endif
  return 2;
}
