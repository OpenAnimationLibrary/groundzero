#pragma once

#include <QObject>
#include <QCryptographicHash>
#include <QFile>
#include <QSaveFile>
#include <QVector>
#include <memory>

class QNetworkAccessManager;
class QNetworkReply;
class QLockFile;
class QProcess;

class CadmiumInstaller final : public QObject {
  Q_OBJECT
public:
  explicit CadmiumInstaller(QObject *parent = nullptr);
  ~CadmiumInstaller() override;
  bool busy() const { return m_busy; }
  static qint64 downloadSize(bool accelerated);

public slots:
  void install(const QString &directory, bool accelerated);
  void cancel();

signals:
  void progress(qint64 completed, qint64 total, const QString &file);
  void finished(const QString &directory);
  void failed(const QString &message);
  void cancelled();

private:
  struct Model { QString name; qint64 bytes; QByteArray digest; };
  static QVector<Model> models(bool accelerated);
  void next();
  void verifyExisting();
  void download();
  void readDownload();
  void completeDownload();
  void fail(const QString &message);
  void clearTransfer();
  QNetworkAccessManager *m_network;
  QNetworkReply *m_reply = nullptr;
  QProcess *m_nativeDownload = nullptr;
  bool m_nativeSucceeded = false;
  std::unique_ptr<QLockFile> m_lock;
  std::unique_ptr<QSaveFile> m_output;
  std::unique_ptr<QFile> m_existing;
  std::unique_ptr<QCryptographicHash> m_hash;
  QVector<Model> m_models;
  QString m_directory;
  int m_index = 0;
  qint64 m_completed = 0, m_received = 0, m_total = 0;
  bool m_busy = false;
  quint64 m_generation = 0;
};
