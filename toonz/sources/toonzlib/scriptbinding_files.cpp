

#include "toonz/scriptbinding_files.h"
#include <QFile>
#include <QFileInfo>
#include <QDirIterator>
#include "tsystem.h"

namespace TScriptBinding {

//===========================================================================

FilePath::FilePath(const QString &filePath) : m_filePath(filePath) {}

FilePath::FilePath(const TFilePath &filePath)
    : m_filePath(QString::fromStdWString(filePath.getWideString())) {}

FilePath::~FilePath() {}

QJSValue FilePath::ctor(QJSEngine *engine, const QJSValueList &args) {
  FilePath *file = new FilePath();
  if (args.size() == 1) {
    file->m_filePath = args[0].toString();
  }
  return file->create(engine, file);
}

QString FilePath::toString() const { return tr("\"%1\"").arg(m_filePath); }

QString FilePath::getExtension() const {
  return QString::fromStdString(getToonzFilePath().getType());
}

QJSValue FilePath::setExtension(const QString &extension) {
  TFilePath fp = getToonzFilePath().withType(extension.toStdString());
  m_filePath   = QString::fromStdWString(fp.getWideString());
  return self();
}

QString FilePath::getName() const {
  return QString::fromStdString(getToonzFilePath().getName());
}

void FilePath::setName(const QString &name) {
  TFilePath fp = getToonzFilePath().withName(name.toStdString());
  m_filePath   = QString::fromStdWString(fp.getWideString());
}

QJSValue FilePath::getParentDirectory() const {
  FilePath *result = new FilePath(getToonzFilePath().getParentDir());
  return create(engine(), result);
}

void FilePath::setParentDirectory(const QJSValue &folder) {
  TFilePath fp;
  if (checkFilePath(engine(), folder, fp)) {
    m_filePath = QString::fromStdWString(
        getToonzFilePath().withParentDir(fp).getWideString());
  }
}

QJSValue FilePath::withExtension(const QString &extension) {
  TFilePath fp = getToonzFilePath().withType(extension.toStdString());
  return create(engine(), new FilePath(fp));
}

QJSValue FilePath::withName(const QString &extension) {
  TFilePath fp = getToonzFilePath().withName(extension.toStdString());
  return create(engine(), new FilePath(fp));
}

QJSValue FilePath::withParentDirectory(const QJSValue &parentDirectoryArg) {
  TFilePath parentDirectory;
  if (!checkFilePath(engine(), parentDirectoryArg, parentDirectory))
    return QJSValue();
  else
    return create(
        engine(),
        new FilePath(getToonzFilePath().withParentDir(parentDirectory)));
}

bool FilePath::exists() const { return QFile(m_filePath).exists(); }

QDateTime FilePath::lastModified() const {
  return QFileInfo(m_filePath).lastModified();
}

TFilePath FilePath::getToonzFilePath() const {
  return TFilePath(m_filePath.toStdWString());
}

bool FilePath::isDirectory() const { return QFileInfo(m_filePath).isDir(); }

QJSValue FilePath::concat(const QJSValue &value) const {
  TFilePath fp;
  if (!checkFilePath(engine(), value, fp)) return QJSValue();

  if (fp.isAbsolute())
    return throwError(
        tr("can't concatenate an absolute path : %1").arg(value.toString()));
  fp = getToonzFilePath() + fp;
  return create(engine(), new FilePath(fp));
}

QJSValue FilePath::files() const {
  if (!isDirectory()) {
    return throwError(tr("%1 is not a directory").arg(toString()));
  }
  TFilePathSet fpset;
  try {
    TSystem::readDirectory(fpset, getToonzFilePath());
    QJSValue result     = engine()->newArray();
    quint32 index       = 0;
    for (TFilePathSet::iterator it = fpset.begin(); it != fpset.end(); ++it) {
      FilePath *res = new FilePath(*it);
      result.setProperty(index++, res->create<FilePath>(engine()));
    }
    return result;
  } catch (...) {
    return throwError(tr("can't read directory %1").arg(toString()));
  }
}

bool checkFilePath(QJSEngine *engine, const QJSValue &value, TFilePath &fp) {
  FilePath *filePath = cast<FilePath>(value);
  if (filePath) {
    fp = filePath->getToonzFilePath();
  } else if (value.isString()) {
    fp = TFilePath(value.toString().toStdWString());
  } else {
    engine->throwError(
        QObject::tr("Argument doesn't look like a file path : %1")
            .arg(value.toString()));
    return false;
  }
  return true;
}

}  // namespace TScriptBinding
