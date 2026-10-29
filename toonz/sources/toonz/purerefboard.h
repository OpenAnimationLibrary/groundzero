#pragma once

#include <QList>
#include <QString>

class QWidget;

// PureRef remains the authoring application. Only its documented image
// exports are read here; a .pur file is never parsed or written by Groundzero.
namespace PureRefBoard {

struct Item {
  QString id;  // relative export filename, not an index into a changing list
  QString label;
  QString path;
};

struct Snapshot {
  QString source;
  bool includeImages = false;
  QList<Item> items;  // overview first
};

QString sourcePath(const QString &path);
QString executablePath(const QString &path);

// Loading a cached snapshot never starts PureRef and works without the source.
bool load(const QString &source, const QString &cacheRoot, Snapshot &snapshot,
          QString &error);

// Export to a new directory, validate, then atomically publish its manifest.
// A failed/canceled refresh leaves the last successful snapshot untouched.
bool importBoard(const QString &executable, const QString &source,
                 const QString &cacheRoot, bool includeImages,
                 Snapshot &snapshot, QString &error, QWidget *parent = nullptr,
                 int timeoutMs = 120000);

}  // namespace PureRefBoard
