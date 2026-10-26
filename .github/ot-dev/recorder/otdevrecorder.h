#pragma once

#include <QElapsedTimer>
#include <QImage>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QRect>
#include <QTimer>

class QAction;
class QDockWidget;
class QGridLayout;
class QLabel;
class QMainWindow;
class QRubberBand;
class QWidget;

// Built and installed only by the OT-Dev Windows CI injection step.
class OtDevRecorder final : public QObject {
public:
  OtDevRecorder(QMainWindow *window, const QString &preferencesPath,
                const QString &portableRoot, const QString &encoder);
  ~OtDevRecorder() override;
  void requestRecording();
  void setPaused(bool paused);
  void stop();
  void setPanelVisible(bool visible);
  void registerCommands();  // Application adapter; not needed by Qt-only tests.
  static void addInterfacePreference(QGridLayout *layout);

  static QImage capture(QMainWindow *window, const QSize &size,
                        bool includeCursor    = true,
                        QWidget *regionWindow = nullptr,
                        const QRect &region   = QRect());
  static QStringList encoderArguments(const QSize &size, const QString &output);

private:
  void begin();
  void tick();
  void finishClip();
  bool publishClip();
  void fail(const QString &message);
  void updateStatus(const QString &text);
  bool captureAllowed() const;
  bool regionAvailable() const;
  void selectRegion();
  void finishSelection(bool accept, bool updateControls = true);
  void savePanelState();
  void createCommands();
  bool eventFilter(QObject *watched, QEvent *event) override;

  QPointer<QMainWindow> m_window;
  QString m_preferencesPath, m_portableRoot, m_encoderPath;
  QString m_outputDir, m_outputFile, m_encoderError;
  QAction *m_start, *m_pause, *m_stop, *m_selectRegion, *m_clearRegion;
  QLabel *m_status, *m_regionStatus;
  QPointer<QDockWidget> m_panel;
  QAction *m_togglePanel, *m_openFolder;
  QList<QAction *> m_commands;
  QPointer<QWidget> m_regionWindow, m_selectionWindow;
  QPointer<QRubberBand> m_selectionBand;
  QRect m_region;
  QPoint m_selectionOrigin;
  QProcess m_encoder;
  QTimer m_timer, m_finishTimer;
  QElapsedTimer m_diskCheck, m_backpressure;
  bool m_enabled = false, m_finishing = false, m_prompting = false;
  bool m_paused = false, m_selecting = false, m_hasRegion = false;
  bool m_panelVisible  = true;
  bool m_consented     = false;
  bool m_errorReported = false;
  int m_frames         = 0;
};
