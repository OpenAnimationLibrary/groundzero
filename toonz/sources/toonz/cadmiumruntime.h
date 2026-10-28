#pragma once

#include <QObject>
#include <QJsonObject>
#include <QQueue>
#include <QProcess>
#include <QTimer>
#include <atomic>

class QNetworkAccessManager;
class QNetworkReply;
class QThread;

// Owns one local inference worker. Artwork is never sent to remote hosts.
class CadmiumRuntime final : public QObject {
  Q_OBJECT
public:
  enum StartupMode { Off = 0, OnDemand = 1, Preload = 2 };
  Q_ENUM(StartupMode)
  static CadmiumRuntime *instance();
  ~CadmiumRuntime() override;
  int startupMode() const;
  void setStartupMode(int mode);
  QString executablePath() const;
  void setExecutablePath(const QString &path);
  QString modelsDirectory() const;
  void setModelsDirectory(const QString &path);
  int idleUnloadMinutes() const;
  void setIdleUnloadMinutes(int minutes);
  bool isReady() const;
  bool busy() const;
  QString statusText() const;

public slots:
  void start();
  void stop();
  void colorize(const QJsonObject &request);
  void segment(const QJsonObject &request);
  void preprocess(const QJsonObject &request);

signals:
  void statusChanged();
  void resultReady(const QJsonObject &response);
  void segmentReady(const QJsonObject &response);
  void preprocessReady(const QJsonObject &response);
  void failed(const QString &message);

private:
  explicit CadmiumRuntime(QObject *parent);
  struct Request { QString operation; QJsonObject body; qint64 bytes; };
  enum State { Stopped, Verifying, Starting, Warming, Ready, Stopping, Error };
  void setState(State state, const QString &text);
  void fail(const QString &message);
  void launch(quint64 generation);
  void checkHealth(quint64 generation);
  void warmModels(quint64 generation);
  void send(const QString &operation, const QJsonObject &body,
            quint64 generation, int internalStage = 0);
  void enqueue(const QString &operation, const QJsonObject &body);
  void next();
  void armIdleTimer();
  QProcess *m_process;
  QNetworkAccessManager *m_network;
  QNetworkReply *m_reply = nullptr;
  QThread *m_verifier = nullptr;
  QTimer m_healthTimer, m_deadline, m_idleTimer;
  QQueue<Request> m_queue;
  qint64 m_queuedBytes = 0;
  State m_state = Stopped;
  QString m_status;
  QString m_activeOperation;
  QByteArray m_output;
  quint16 m_port = 0;
  quint64 m_generation = 0;
  std::atomic<bool> m_cancelVerification{false};
  bool m_listening = false;
  bool m_tiled = false, m_gapAccelerated = false;
};
