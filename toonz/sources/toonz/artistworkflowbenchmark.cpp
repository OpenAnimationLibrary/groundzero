#include "artistworkflowbenchmark.h"
#include "nativeprocessmemory.h"
#include "sceneviewer.h"
#include "tapp.h"
#include "tools/tool.h"
#include "tools/toolhandle.h"
#include "tools/toolcommandids.h"
#include "toonz/tcolumnhandle.h"
#include "toonz/tframehandle.h"
#include "toonz/tpalettehandle.h"
#include "toonz/levelproperties.h"
#include "toonz/toonzscene.h"
#include "toonz/tscenehandle.h"
#include "toonz/txsheet.h"
#include "toonz/txshcell.h"
#include "toonz/txsheethandle.h"
#include "toonz/txshlevelhandle.h"
#include "toonz/txshleveltypes.h"
#include "toonz/txshsimplelevel.h"
#include "timage_io.h"
#include "tlevel_io.h"
#include "tpalette.h"
#include "tproperty.h"
#include "trasterimage.h"
#include "tstroke.h"
#include "tundo.h"
#include "tvectorimage.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMainWindow>
#include <QOpenGLContext>
#include <QSet>
#include <QSysInfo>
#include <QTemporaryDir>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}
void settle() {
  QCoreApplication::sendPostedEvents();
  QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
}
QJsonObject distribution(const std::vector<double> &values) {
  require(!values.empty(), "No benchmark samples were collected");
  auto sorted = values;
  std::sort(sorted.begin(), sorted.end());
  QJsonArray samples;
  for (double value : values) samples.append(value);
  const size_t middle = sorted.size() / 2;
  const double median = sorted.size() % 2 ? sorted[middle]
                                         : (sorted[middle - 1] + sorted[middle]) / 2;
  return {{"unit", "ms"}, {"samples", samples},
          {"median", median}, {"percentile_method", "nearest-rank"},
          {"p95", sorted[size_t(std::ceil(sorted.size() * .95)) - 1]}};
}
void hashImage(QCryptographicHash &hash, const QImage &image) {
  require(!image.isNull(), "The native viewer did not render a framebuffer");
  const QImage rgba = image.convertToFormat(QImage::Format_RGBA8888);
  for (int y = 0; y < rgba.height(); ++y)
    hash.addData(reinterpret_cast<const char *>(rgba.constScanLine(y)), rgba.width() * 4);
}
}  // namespace

int runArtistWorkflowBenchmark(QMainWindow *window) {
  try {
    require(window && window->isVisible(), "The benchmark requires a visible native application");
    auto *app = TApp::instance();
    auto *scene = app->getCurrentScene()->getScene();
    auto *xsheet = app->getCurrentXsheet()->getXsheet();
    require(scene && xsheet && xsheet->getFrameCount() == 0,
            "The benchmark requires an empty disposable scene");
    auto *viewer = app->getActiveViewer();
    if (!viewer) {
      for (auto *candidate : window->findChildren<SceneViewer *>())
        if (candidate->isVisible()) { viewer = candidate; break; }
    }
    require(viewer && viewer->isVisible(), "No visible native scene viewer");
    app->setActiveViewer(viewer);
    viewer->QWidget::setFocus(Qt::OtherFocusReason);
    QTemporaryDir output;
    require(output.isValid(), "Cannot create disposable benchmark resources");
    const int frameCount = 24, strokesPerDrawing = 96, rasterSize = 512;
    const int penStrokes = 12, pointsPerStroke = 96;
    scene->setSceneName(L"ArtistWorkloadFixture");
    auto *vector = scene->createNewLevel(PLI_XSHLEVEL, L"VectorWorkload")->getSimpleLevel();
    auto *raster = scene->createNewLevel(OVL_XSHLEVEL, L"RasterWorkload",
                                      TDimension(rasterSize, rasterSize), 120)->getSimpleLevel();
    require(vector && raster && vector->getPalette(), "Cannot create native workload levels");
    const TFilePath vectorPath(QDir(output.path()).filePath("vector.pli"));
    const TFilePath rasterPath(QDir(output.path()).filePath("raster..png"));
    vector->setPath(vectorPath);
    raster->setPath(rasterPath);
    raster->getProperties()->setImageRes(TDimension(rasterSize, rasterSize));
    raster->getProperties()->setImageDpi(TPointD(120, 120));
    raster->getProperties()->setDpi(120);
    raster->getProperties()->setBpp(32);
    for (int frame = 0; frame < frameCount; ++frame) {
      TVectorImageP drawing = new TVectorImage;
      drawing->setPalette(vector->getPalette());
      for (int i = 0; i < strokesPerDrawing; ++i) {
        const double x = (i % 12) * 35 - 210, y = (i / 12) * 35 - 140;
        std::vector<TThickPoint> points{
            TThickPoint(x, y, 1), TThickPoint(x + 12, y + 15 + frame, 3),
            TThickPoint(x + 25, y, 1)};
        auto *stroke = new TStroke(points);
        stroke->setStyle(1);
        drawing->addStroke(stroke);
      }
      vector->setFrame(TFrameId(frame + 1), drawing);
      TRaster32P pixels(rasterSize, rasterSize);
      pixels->fill(TPixel32(20 + frame * 5, 60, 100, 255));
      TRasterImageP image(pixels);
      image->setDpi(120, 120);
      raster->setFrame(TFrameId(frame + 1), image);
      require(xsheet->setCell(frame, 0, TXshCell(raster, TFrameId(frame + 1))) &&
                  xsheet->setCell(frame, 1, TXshCell(vector, TFrameId(frame + 1))),
              "Cannot expose benchmark drawings");
    }
    app->getCurrentXsheet()->notifyXsheetChanged();
    app->getCurrentColumn()->setColumnIndex(1);
    app->getCurrentFrame()->setFrame(0);
    app->getCurrentLevel()->setLevel(vector);
    app->getCurrentPalette()->setPalette(vector->getPalette(), 1);
    app->getCurrentTool()->setTool(T_Brush);
    auto *brush = app->getCurrentTool()->getTool();
    require(brush, "Native vector brush was not registered");
    QJsonObject brushOptions;
    for (int group = 0; group < 2; ++group) {
      if (auto *properties = brush->getProperties(group)) {
        for (int index = 0; index < properties->getPropertyCount(); ++index) {
          auto *property = properties->getProperty(index);
          brushOptions.insert(QString::number(group) + ":" + QString::fromStdString(property->getName()),
                              QString::fromStdString(property->getValueAsString()));
        }
      }
    }
    brush->setViewer(viewer);
    brush->updateMatrix();
    viewer->fitToCamera();
    settle();
    viewer->grabFramebuffer();  // Warm shader/context initialization separately.
    viewer->makeCurrent();
    require(viewer->context() && viewer->context()->isValid(), "Native GL context is unavailable");
    const auto *rendererText = viewer->context()->functions()->glGetString(GL_RENDERER);
    const QString renderer = rendererText
        ? QString::fromLatin1(reinterpret_cast<const char *>(rendererText)) : QString();
    viewer->doneCurrent();
    require(!renderer.isEmpty(), "Native GL renderer identification is unavailable");
    QElapsedTimer timer;
    std::vector<double> penProcessing, commitRendering;
    for (int stroke = 0; stroke < penStrokes; ++stroke) {
      brush->preLeftButtonDown();
      timer.start();
      for (int i = 0; i < pointsPerStroke; ++i) {
        TMouseEvent event;
        event.m_isTablet = true;
        event.m_button = Qt::LeftButton;
        event.m_buttons = i == pointsPerStroke - 1 ? Qt::NoButton : Qt::LeftButton;
        event.m_pressure = .2 + .7 * std::sin(3.141592653589793 * i / (pointsPerStroke - 1));
        const TPointD position(-180 + i * 3.0, -210 + stroke * 8 + 4 * std::sin(i * .2));
        event.m_pos = position;
        event.m_mousePos = QPointF(position.x, position.y);
        if (i == 0) brush->leftButtonDown(position, event);
        else if (i == pointsPerStroke - 1) brush->leftButtonUp(position, event);
        else brush->leftButtonDrag(position, event);
      }
      penProcessing.push_back(timer.nsecsElapsed() / 1000000.0);
      timer.restart();
      settle();
      require(!viewer->grabFramebuffer().isNull(), "Pen commit did not render");
      commitRendering.push_back(timer.nsecsElapsed() / 1000000.0);
    }
    const TVectorImageP drawn = vector->getFrame(TFrameId(1), false);
    require(drawn && drawn->getStrokeCount() == strokesPerDrawing + penStrokes,
            "Synthetic pressure traces did not create the expected native brush strokes");
    QCryptographicHash rendered(QCryptographicHash::Sha256);
    std::vector<double> scrub, sequential, translucent, profiled;
    QByteArray opaqueFirstFrame, translucentFirstFrame;
    for (int pass = 0; pass < 2; ++pass) {
      QSet<QByteArray> visibleFrames;
      app->getCurrentFrame()->setPlaying(pass == 1);
      for (int index = 0; index < frameCount; ++index) {
        const int frame = pass ? index : (index * 7) % frameCount;
        timer.restart();
        app->getCurrentFrame()->setFrame(frame);
        settle();
        const QImage image = viewer->grabFramebuffer();
        (pass ? sequential : scrub).push_back(timer.nsecsElapsed() / 1000000.0);
        hashImage(rendered, image);
        if (pass == 0 && frame == 0) {
          QCryptographicHash firstFrame(QCryptographicHash::Sha256);
          hashImage(firstFrame, image);
          opaqueFirstFrame = firstFrame.result();
        }
        require(image.width() >= 64 && image.height() >= 64, "Viewer is too small for the workload");
        QCryptographicHash content(QCryptographicHash::Sha256);
        // The changing raster is centered under the vector artwork. Check a
        // central patch, not an overlay label that might change on a stale view.
        hashImage(content, image.copy(image.width() / 2 - 16, image.height() / 2 - 16, 32, 32));
        visibleFrames.insert(content.result());
        require(app->getCurrentFrame()->getFrame() == frame, "Native frame navigation lost its target");
      }
      app->getCurrentFrame()->setPlaying(false);
      require(visibleFrames.size() == frameCount,
              "Native rendered artwork did not update for every exposed drawing");
    }
    // Scalar transparency is a common artist workload and can take a different
    // renderer path. Record it separately so a compositing regression is visible.
    app->getCurrentFrame()->setPlaying(false);
    for (int frame = 0; frame < frameCount; ++frame) {
      const TVectorImageP image = vector->getFrame(TFrameId(frame + 1), true);
      for (unsigned int stroke = 0; stroke < image->getStrokeCount(); ++stroke)
        image->getStroke(stroke)->setOpacity(.5);
    }
    app->getCurrentXsheet()->notifyXsheetChanged();
    for (int frame = 0; frame < frameCount; ++frame) {
      timer.restart();
      app->getCurrentFrame()->setFrame(frame);
      settle();
      const QImage image = viewer->grabFramebuffer();
      translucent.push_back(timer.nsecsElapsed() / 1000000.0);
      QCryptographicHash transparentFrame(QCryptographicHash::Sha256);
      hashImage(transparentFrame, image);
      if (frame == 0) {
        translucentFirstFrame = transparentFrame.result();
        require(transparentFrame.result() != opaqueFirstFrame,
                "Scalar opacity did not change the native rendered drawing");
      }
      hashImage(rendered, image);
    }
    const StrokeOpacity::Profile envelope{{0., .2}, {.5, 1.}, {1., .2}};
    for (int frame = 0; frame < frameCount; ++frame) {
      const TVectorImageP image = vector->getFrame(TFrameId(frame + 1), true);
      for (unsigned int stroke = 0; stroke < image->getStrokeCount(); ++stroke)
        require(image->getStroke(stroke)->setOpacityProfile(envelope),
                "Native workload opacity envelope was rejected");
    }
    app->getCurrentXsheet()->notifyXsheetChanged();
    for (int frame = 0; frame < frameCount; ++frame) {
      timer.restart();
      app->getCurrentFrame()->setFrame(frame);
      settle();
      const QImage image = viewer->grabFramebuffer();
      profiled.push_back(timer.nsecsElapsed() / 1000000.0);
      if (frame == 0) {
        QCryptographicHash profiledFrame(QCryptographicHash::Sha256);
        hashImage(profiledFrame, image);
        require(profiledFrame.result() != translucentFirstFrame,
                "Opacity envelopes did not change the native rendered drawing");
      }
      hashImage(rendered, image);
    }
    std::vector<double> saves;
    const TFilePath scenePath(QDir(output.path()).filePath("workload.tnz"));
    for (int pass = 0; pass < 3; ++pass) {
      // Include actual drawing resources each time, not only .tnz metadata.
      for (int frame = 0; frame < frameCount; ++frame) {
        vector->getFrame(TFrameId(frame + 1), true);
        raster->getFrame(TFrameId(frame + 1), true);
      }
      vector->setDirtyFlag(true);
      raster->setDirtyFlag(true);
      timer.restart();
      vector->save(vectorPath);
      raster->save(rasterPath);
      scene->saveCopy(scenePath);
      saves.push_back(timer.nsecsElapsed() / 1000000.0);
    }
    // Independent public I/O readers check data written on disk, bypassing the
    // scene's image cache. Performance samples without correctness are invalid.
    TLevelReaderP vectorReader(vectorPath), rasterReader(rasterPath);
    require(!!vectorReader && !!rasterReader, "Saved resource readers are unavailable");
    vectorReader->loadInfo();
    rasterReader->loadInfo();
    for (int frame = 0; frame < frameCount; ++frame) {
      const TVectorImageP actual = vectorReader->getFrameReader(frame + 1)->load();
      require(actual && actual->getStrokeCount() == strokesPerDrawing + (frame == 0 ? penStrokes : 0),
              "Saved vector drawing did not round-trip");
      for (unsigned int stroke = 0; stroke < actual->getStrokeCount(); ++stroke) {
        require(actual->getStroke(stroke)->getOpacity() == .5,
                "Saved scalar opacity did not round-trip");
        require(actual->getStroke(stroke)->getOpacityProfile() == envelope,
                "Saved opacity envelope did not round-trip");
      }
      const TRasterImageP image = rasterReader->getFrameReader(frame + 1)->load();
      require(!!image, "Saved raster drawing is unreadable");
      const TRaster32P pixels = image->getRaster();
      require(pixels && pixels->getLx() == rasterSize && pixels->getLy() == rasterSize &&
                  pixels->pixels(0)[0] == TPixel32(20 + frame * 5, 60, 100, 255),
              "Saved raster pixels did not round-trip");
    }
    ToonzScene reloaded;
    reloaded.loadTnzFile(scenePath);
    require(reloaded.getXsheet()->getFrameCount() == frameCount,
            "Saved scene exposures did not round-trip");
    require(reloaded.getXsheet()->getColumnCount() == 2,
            "Saved scene column structure did not round-trip");
    for (int row = 0; row < frameCount; ++row) {
      for (int column = 0; column < 2; ++column) {
        const TXshCell &cell = reloaded.getXsheet()->getCell(row, column);
        require(cell.getSimpleLevel() && cell.m_frameId == TFrameId(row + 1) &&
                    cell.getSimpleLevel()->getType() == (column ? PLI_XSHLEVEL : OVL_XSHLEVEL),
                "Saved scene changed drawing identity or exposure order");
      }
    }
    const QJsonObject report{
        {"schema", 1}, {"fixture", "native-artist-workload-v1"},
        {"platform", QSysInfo::productType()}, {"architecture", QSysInfo::currentCpuArchitecture()},
        {"qt", qVersion()}, {"renderer", renderer},
        {"frames", frameCount}, {"vector_strokes_per_drawing", strokesPerDrawing},
        {"brush_options", brushOptions},
        {"raster_dimensions", rasterSize}, {"pressure_points_per_stroke", pointsPerStroke},
        {"viewer_width", viewer->width()}, {"viewer_height", viewer->height()},
        {"synthetic_pressure_stroke_processing", distribution(penProcessing)},
        {"pen_commit_and_framebuffer", distribution(commitRendering)},
        {"random_frame_navigation_and_render", distribution(scrub)},
        {"sequential_playback_mode_frame_render", distribution(sequential)},
        {"translucent_vector_frame_render", distribution(translucent)},
        {"opacity_envelope_vector_frame_render", distribution(profiled)},
        {"drawing_resources_and_scene_save", distribution(saves)},
        {"peak_resident_bytes", QString::number(nativeProcessPeakResidentBytes())},
        {"peak_resident_source", nativeProcessPeakResidentSource()},
        {"framebuffer_checksum", QString::fromLatin1(rendered.result().toHex())},
        {"correctness", "all native strokes, saved drawings and exposures verified"},
        {"limits", "Synthetic tool input excludes device/OS pen latency. Frame stepping measures rendering capacity, not timer pacing. Peak resident memory is a cumulative process high-water mark, not an isolated workload allocation. CI hardware and software GL are not artist hardware."}};
    app->getCurrentScene()->setDirtyFlag(false);
    TUndoManager::manager()->reset();
    std::cout << "artist-workload-json: " << QJsonDocument(report).toJson(QJsonDocument::Compact).constData()
              << "\nartist-workload: PASS" << std::endl;
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "artist-workload: FAIL: " << error.what() << std::endl;
  } catch (...) {
    std::cerr << "artist-workload: FAIL: unexpected native exception" << std::endl;
  }
  TApp::instance()->getCurrentFrame()->setPlaying(false);
  TApp::instance()->getCurrentScene()->setDirtyFlag(false);
  TUndoManager::manager()->reset();
  return 1;
}
