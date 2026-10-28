#include "cadmiumreviewview.h"

#include <QApplication>
#include <QComboBox>
#include <QMouseEvent>
#include <QSlider>
#include <QWheelEvent>

#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

QColor pixel(CadmiumReviewView &view, const QPointF &point, int pane = 0) {
  QImage image(view.size(), QImage::Format_ARGB32);
  image.fill(Qt::transparent);
  view.render(&image);
  return image.pixelColor(view.viewportPointForImage(point, pane));
}

void mouse(CadmiumReviewView &view, QEvent::Type type, const QPoint &point,
           Qt::MouseButton button, Qt::MouseButtons buttons) {
  QWidget *canvas = view.findChild<QWidget *>("cadmiumReviewCanvas");
  require(canvas, "review canvas is missing");
  const QPoint local = canvas->mapFrom(&view, point);
  QMouseEvent event(type, QPointF(local), button, buttons, Qt::NoModifier);
  QCoreApplication::sendEvent(canvas, &event);
}
}

int main(int argc, char **argv) {
  qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
  QApplication app(argc, argv);
  try {
    CadmiumReviewView view;
    view.resize(860, 440);
    view.show();
    app.processEvents();
    QImage before(8, 4, QImage::Format_ARGB32);
    QImage after(before.size(), before.format()), reference(before.size(), before.format());
    before.fill(Qt::red); after.fill(Qt::blue); reference.fill(Qt::green);
    QVector<int> labels(32, 0);
    for (int y = 1; y < 3; ++y)
      for (int x = 1; x < 7; ++x) labels[y * 8 + x] = x < 4 ? 1 : 2;
    view.setImages(before, after, reference);
    view.setRegions(before.size(), labels);
    view.setZoom(16.0);
    view.setPan(QPointF(13.0, -9.0));
    for (int pane : {0, 1}) {
      require(view.regionAt(view.viewportPointForImage(QPointF(2.5, 1.5), pane)) == 1,
              "first region hit changed between comparison panes");
      require(view.regionAt(view.viewportPointForImage(QPointF(5.5, 2.5), pane)) == 2,
              "second region hit changed between comparison panes");
      require(view.regionAt(view.viewportPointForImage(QPointF(7.5, 0.5), pane)) == 0,
              "protected exterior lost its zero label");
      require(view.regionAt(view.viewportPointForImage(QPointF(8.0, 1.5), pane)) == -1,
              "the right image boundary was accepted as an interior pixel");
    }
    require(pixel(view, QPointF(2.5, 1.5), 0) == QColor(Qt::red) &&
                pixel(view, QPointF(2.5, 1.5), 1) == QColor(Qt::blue),
            "side-by-side views did not display distinct before and after images");

    int activations = 0, contexts = 0, selected = -1;
    QObject::connect(&view, &CadmiumReviewView::regionActivated,
                     [&activations, &selected](int region) { ++activations; selected = region; });
    QObject::connect(&view, &CadmiumReviewView::regionContextMenuRequested,
                     [&contexts, &selected](int region, const QPoint &) { ++contexts; selected = region; });
    const QPoint first = view.viewportPointForImage(QPointF(2.5, 1.5), 1);
    mouse(view, QEvent::MouseButtonPress, first, Qt::LeftButton, Qt::LeftButton);
    mouse(view, QEvent::MouseButtonRelease, first, Qt::LeftButton, Qt::NoButton);
    require(activations == 1 && selected == 1, "click did not select its native region");
    const QPoint second = view.viewportPointForImage(QPointF(5.5, 1.5));
    mouse(view, QEvent::MouseButtonPress, second, Qt::RightButton, Qt::RightButton);
    require(contexts == 1 && selected == 2, "context decision used a different region from the canvas hit");
    const QPointF previousPan = view.pan();
    mouse(view, QEvent::MouseButtonPress, first, Qt::LeftButton, Qt::LeftButton);
    mouse(view, QEvent::MouseMove, first + QPoint(31, 17), Qt::NoButton, Qt::LeftButton);
    mouse(view, QEvent::MouseButtonRelease, first + QPoint(31, 17), Qt::LeftButton, Qt::NoButton);
    require(view.pan() == previousPan + QPointF(31, 17) && activations == 1,
            "dragging artwork selected a region instead of synchronously panning");
    require(view.regionAt(view.viewportPointForImage(QPointF(2.5, 1.5), 1)) == 1,
            "native hit testing did not follow the pan transform");

    QWidget *canvas = view.findChild<QWidget *>("cadmiumReviewCanvas");
    const QPoint anchor = view.viewportPointForImage(QPointF(5.5, 1.5), 1);
    const QPoint local = canvas->mapFrom(&view, anchor);
    const double oldZoom = view.zoom();
    QWheelEvent wheel(QPointF(local), QPointF(canvas->mapToGlobal(local)), QPoint(), QPoint(0, 120),
                      Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(canvas, &wheel);
    require(view.zoom() > oldZoom && view.regionAt(anchor) == 2,
            "wheel zoom moved the region away from the pointer anchor");

    for (CadmiumReviewView::Mode mode : {
             CadmiumReviewView::Mode::Wipe, CadmiumReviewView::Mode::ReferenceOverlay,
             CadmiumReviewView::Mode::Before, CadmiumReviewView::Mode::After}) {
      view.setMode(mode);
      view.setZoom(16.0);
      view.setPan(QPointF(-19, 11));
      require(view.regionAt(view.viewportPointForImage(QPointF(5.5, 1.5))) == 2,
              "a comparison mode changed the native region coordinates");
    }
    view.setPan(QPointF());
    view.setMode(CadmiumReviewView::Mode::Wipe);
    view.setWipePosition(0.0);
    require(pixel(view, QPointF(2.5, 1.5)) == QColor(Qt::blue),
            "wipe start did not display the after image");
    view.setWipePosition(1.0);
    require(pixel(view, QPointF(2.5, 1.5)) == QColor(Qt::red),
            "wipe end did not display the before image");
    view.setWipePosition(0.5);
    const QRect canvasArea = view.canvasRect();
    const QPoint divider(canvasArea.center().x(), canvasArea.top() + 60);
    mouse(view, QEvent::MouseButtonPress, divider, Qt::LeftButton, Qt::LeftButton);
    mouse(view, QEvent::MouseMove, divider + QPoint(100, 0), Qt::NoButton, Qt::LeftButton);
    mouse(view, QEvent::MouseButtonRelease, divider + QPoint(100, 0), Qt::LeftButton, Qt::NoButton);
    require(view.wipePosition() > 0.5 && activations == 1,
            "dragging the comparison divider panned or selected artwork");

    view.setMode(CadmiumReviewView::Mode::ReferenceOverlay);
    auto *opacity = view.findChild<QSlider *>("cadmiumReviewOpacity");
    require(opacity && opacity->isEnabled(), "reference opacity control is unavailable");
    view.setOverlayOpacity(1.0);
    require(pixel(view, QPointF(2.5, 1.5)) == QColor(Qt::green),
            "opaque reference overlay did not display the reference");
    opacity->setValue(0);
    require(view.overlayOpacity() == 0.0 && pixel(view, QPointF(2.5, 1.5)) == QColor(Qt::blue),
            "reference opacity control did not reveal the after image");
    view.setSelectedRegion(1);
    require(pixel(view, QPointF(2.5, 1.5)) != QColor(Qt::blue),
            "selected native region was not highlighted");
    view.setSelectedRegion(-1);
    QSet<int> protectedRegions;
    protectedRegions.insert(2);
    view.setProtectedRegions(protectedRegions);
    require(pixel(view, QPointF(5.5, 1.5)) != QColor(Qt::blue) &&
                pixel(view, QPointF(2.5, 1.5)) == QColor(Qt::blue),
            "protected region highlight changed an unrelated region");
    view.fitToWindow();
    require(view.pan().isNull() && view.zoom() > 0.0 &&
                view.regionAt(view.viewportPointForImage(QPointF(2.5, 1.5))) == 1,
            "fit did not reset the shared pan while preserving region mapping");

    view.setRegions(QSize(8, 4), QVector<int>(31, 1));
    require(view.regionAt(view.viewportPointForImage(QPointF(2.5, 1.5))) == -1,
            "incomplete native labels were accepted");
    view.setRegions(before.size(), labels);
    view.clear(QStringLiteral("No proposal selected"));
    require(view.regionAt(view.canvasRect().center()) == -1,
            "clearing the review retained an editable native region");
    view.setImages(before, after);
    view.setMode(CadmiumReviewView::Mode::ReferenceOverlay);
    require(!opacity->isEnabled(), "missing reference left its opacity control enabled");
    std::cout << "Cadmium interactive review checks passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
  }
  return 1;
}
