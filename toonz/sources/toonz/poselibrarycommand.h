#pragma once

#include "toonz/poselibrary.h"
#include <QObject>
#include <QPointer>

class ToonzScene;
class QDialog;

// A modal edit belongs to the scene and complete library it was opened for.
// Scene changes invalidate it permanently (even if the same address is reused).
// Notification-driven changes close the dialog; the final comparison also
// catches mutations such as column remapping which do not emit that signal.
class PoseLibraryEditContext final : public QObject {
  ToonzScene *m_scene;
  PoseLibrary m_before;
  bool m_valid;
  QPointer<QDialog> m_dialog;
  void invalidate();

public:
  PoseLibraryEditContext();
  const PoseLibrary &before() const { return m_before; }
  bool isCurrent() const;
  int exec(QDialog &dialog);
};

// Commits the complete library (including its base) as one native undo step.
// Returns false for an unchanged library or a scene which is no longer current.
bool replacePoseLibrary(ToonzScene *scene, const PoseLibrary &library,
                        const QString &label);
