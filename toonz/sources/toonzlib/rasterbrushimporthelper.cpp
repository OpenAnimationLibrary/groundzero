#include "toonz/rasterbrushimport.h"
#include <QCoreApplication>
int main(int argc, char **argv) {
  QCoreApplication application(argc, argv);
#ifdef __APPLE__
  const auto args = application.arguments();
  if (args.size() == 4 && args.at(1) == "--raster-brush-import-stream")
    return RasterBrush::runMacImportStream(args.at(2), args.at(3),
                                           RasterBrush::runImportWorker);
#endif
  return 2;
}
