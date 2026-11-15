#include "batchtaskviewintegrationtest.h"

#include "batches.h"
#include "tasksviewer.h"
#include "tmsgcore.h"

#include <QComboBox>
#include <QGridLayout>
#include <QItemSelectionModel>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStringList>
#include <QTimer>

#include <stdexcept>
#include <vector>

namespace {

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

template <class Widget>
Widget* fieldForLabel(TaskSheet& sheet, const char* text) {
  for (QLabel* label : sheet.findChildren<QLabel*>()) {
    if (label->text() != TaskSheet::tr(text)) continue;
    auto* layout = qobject_cast<QGridLayout*>(label->parentWidget()->layout());
    require(layout, "batch task editor label has no grid layout");
    int row, column, rowSpan, columnSpan;
    layout->getItemPosition(layout->indexOf(label), &row, &column, &rowSpan,
                            &columnSpan);
    QWidget* widget = nullptr;
    for (int next = column + columnSpan; next < layout->columnCount(); ++next) {
      auto* item = layout->itemAtPosition(row, next);
      if (item && item->widget()) {
        widget = item->widget();
        break;
      }
    }
    require(widget, "batch task editor label has no field in its row");
    auto* field = qobject_cast<Widget*>(widget);
    require(field, "batch task editor field has an unexpected widget type");
    return field;
  }
  throw std::runtime_error("batch task editor field label is missing");
}

QStringList dependencies(const TFarmTask& task) {
  QStringList ids;
  for (int i = 0; i < task.m_dependencies->getTaskCount(); ++i)
    ids.push_back(task.m_dependencies->getTaskId(i));
  return ids;
}

void checkBusyName(TasksViewer& viewer, TFarmTaskGroup& task) {
  TaskSheet& sheet = *viewer.m_taskSheet;
  auto* name       = fieldForLabel<DVGui::LineEdit>(sheet, "Name:");
  require(!name->isEnabled(), "submitted task name editor remained enabled");
  const QString original = task.m_name;
  viewer.stopTimer();
  name->setText(original + " refused edit");
  require(QMetaObject::invokeMethod(&sheet, "setName", Qt::DirectConnection),
          "batch task name edit slot was not invoked");
  require(task.m_name == original && name->text() == original,
          "submitted name edit changed the task or left stale editor text");
  require(sheet.getCurrentTask() == &task && !sheet.widget()->isHidden(),
          "refused name edit lost the selected task editor");
  require(viewer.m_timer->isActive(),
          "refused name edit did not resume refresh");
}

void checkBusyDependencies(TaskSheet& sheet, TFarmTaskGroup& task) {
  auto* added     = sheet.findChild<QListWidget*>("tasksRemoveBox");
  auto* available = sheet.findChild<QListWidget*>("tasksAddBox");
  require(added && available && added->count() > 0 && available->count() > 0,
          "batch task dependency fixture has no existing or candidate edge");
  require(!added->isEnabled() && !available->isEnabled(),
          "submitted dependency editors remained enabled");
  int guardedButtons = 0;
  for (QPushButton* button : sheet.findChildren<QPushButton*>()) {
    if (button->text() != TaskSheet::tr("<< Add") &&
        button->text() != TaskSheet::tr("Remove >>"))
      continue;
    require(!button->isEnabled(),
            "submitted dependency button remained enabled");
    ++guardedButtons;
  }
  require(guardedButtons == 2, "batch task dependency buttons are missing");
  const QStringList original = dependencies(task);
  const int candidateCount   = available->count();
  added->item(0)->setSelected(true);
  require(QMetaObject::invokeMethod(&sheet, "onRemoved", Qt::DirectConnection,
                                    Q_ARG(bool, true)),
          "batch task dependency removal slot was not invoked");
  require(dependencies(task) == original && added->count() == original.size(),
          "submitted dependency removal changed the edge or editor list");
  available->item(0)->setSelected(true);
  require(QMetaObject::invokeMethod(&sheet, "onAdded", Qt::DirectConnection,
                                    Q_ARG(bool, true)),
          "batch task dependency addition slot was not invoked");
  require(
      dependencies(task) == original && available->count() == candidateCount,
      "submitted dependency addition changed the edge or editor list");
}

void checkBusyChunks(TaskSheet& sheet, TFarmTaskGroup& task) {
  auto* chunkSize = fieldForLabel<DVGui::LineEdit>(sheet, "Frames per Chunk:");
  require(!chunkSize->isEnabled(), "submitted chunk editor remained enabled");
  const int originalSize         = task.m_chunkSize;
  const int originalFrom         = task.m_from;
  const int originalTo           = task.m_to;
  const TFilePath originalOutput = task.m_outputPath;
  std::vector<TFarmTask*> children;
  QStringList childIds;
  for (int i = 0; i < task.getTaskCount(); ++i) {
    children.push_back(task.getTask(i));
    childIds.push_back(task.getTask(i)->m_id);
  }
  require(children.size() >= 2 && originalSize > 1,
          "batch task chunk fixture needs multiple nontrivial chunks");
  chunkSize->setText(QString::number(originalSize - 1));
  require(
      QMetaObject::invokeMethod(&sheet, "setChunkSize", Qt::DirectConnection),
      "batch task chunk edit slot was not invoked");
  require(task.m_chunkSize == originalSize && task.m_from == originalFrom &&
              task.m_to == originalTo && task.m_outputPath == originalOutput &&
              chunkSize->text() == QString::number(originalSize),
          "submitted chunk edit changed settings or left stale editor text");
  require(task.getTaskCount() == static_cast<int>(children.size()),
          "submitted chunk edit changed the child count");
  for (int i = 0; i < task.getTaskCount(); ++i) {
    require(
        task.getTask(i) == children[i] &&
            BatchesController::instance()->getTask(childIds[i]) == children[i],
        "submitted chunk edit replaced a child or its controller entry");
  }
}

void checkBusyThreads(TaskSheet& sheet, TFarmTaskGroup& task) {
  auto* threads      = fieldForLabel<QComboBox>(sheet, "Dedicated CPUs:");
  const int original = task.m_threadsIndex;
  require(threads->count() > 1 && original >= 0 && original < threads->count(),
          "batch task CPU fixture needs a valid selector value");
  const int attempted = (original + 1) % threads->count();
  std::vector<int> childThreads;
  for (int i = 0; i < task.getTaskCount(); ++i)
    childThreads.push_back(task.getTask(i)->m_threadsIndex);
  require(!threads->isEnabled(), "submitted CPU selector remained enabled");
  {
    const QSignalBlocker blocker(threads);
    threads->setCurrentIndex(attempted);
  }
  require(
      QMetaObject::invokeMethod(&sheet, "setThreadsCombo", Qt::DirectConnection,
                                Q_ARG(int, attempted)),
      "batch task CPU edit slot was not invoked");
  require(
      task.m_threadsIndex == original && threads->currentIndex() == original,
      "submitted CPU edit changed the task or failed to restore selection");
  for (int i = 0; i < task.getTaskCount(); ++i)
    require(task.getTask(i)->m_threadsIndex == childThreads[i],
            "submitted CPU edit changed an owned child");
}

void checkIdleName(TasksViewer& viewer, TFarmTask& task) {
  viewer.setSelected(&task);
  TaskSheet& sheet = *viewer.m_taskSheet;
  auto* name       = fieldForLabel<DVGui::LineEdit>(sheet, "Name:");
  require(name->isEnabled(), "unrelated idle task editor remained disabled");
  const QString original = task.m_name;
  const QString edited   = original + " accepted edit";
  name->setText(edited);
  require(QMetaObject::invokeMethod(&sheet, "setName", Qt::DirectConnection),
          "idle batch task name edit slot was not invoked");
  require(task.m_name == edited && name->text() == edited &&
              BatchesController::instance()->getDirtyFlag(),
          "unrelated idle task edit was not accepted and marked dirty");
  name->setText(original);
  require(QMetaObject::invokeMethod(&sheet, "setName", Qt::DirectConnection),
          "idle batch task name restoration slot was not invoked");
  require(task.m_name == original,
          "idle batch task fixture name was not restored");
}

void checkIdleLeafEdits(TasksViewer& viewer, TFarmTask& task) {
  require(task.m_isComposerTask && !dynamic_cast<TFarmTaskGroup*>(&task) &&
              task.m_to - task.m_from >= 2,
          "idle leaf fixture needs a composer task with an editable range");
  viewer.setSelected(&task);
  TaskSheet& sheet        = *viewer.m_taskSheet;
  auto* from              = fieldForLabel<DVGui::LineEdit>(sheet, "From:");
  auto* to                = fieldForLabel<DVGui::LineEdit>(sheet, "To:");
  auto* output            = fieldForLabel<DVGui::LineEdit>(sheet, "Output:");
  const int originalFrom  = task.m_from;
  const int originalTo    = task.m_to;
  const int originalChunk = task.m_chunkSize;
  const TFilePath originalOutput = task.m_outputPath;
  const TFilePath editedOutput(originalOutput.getWideString() + L".edited.png");

  from->setText(QString::number(originalFrom + 1));
  require(QMetaObject::invokeMethod(&sheet, "setFrom", Qt::DirectConnection),
          "idle leaf From slot was not invoked");
  require(task.m_from == originalFrom + 1 && task.m_to == originalTo,
          "idle leaf From edit was refused or changed the end frame");
  to->setText(QString::number(originalTo - 1));
  require(QMetaObject::invokeMethod(&sheet, "setTo", Qt::DirectConnection),
          "idle leaf To slot was not invoked");
  require(task.m_to == originalTo - 1 && task.m_from == originalFrom + 1,
          "idle leaf To edit was refused or changed the start frame");
  output->setText(QString::fromStdWString(editedOutput.getWideString()));
  require(QMetaObject::invokeMethod(&sheet, "setOutput", Qt::DirectConnection),
          "idle leaf Output slot was not invoked");
  require(task.m_outputPath == editedOutput &&
              task.m_chunkSize == originalChunk &&
              sheet.getCurrentTask() == &task &&
              BatchesController::instance()->getTask(task.m_id) == &task,
          "idle leaf Output edit was refused or replaced the selected task");

  from->setText(QString::number(originalFrom));
  require(QMetaObject::invokeMethod(&sheet, "setFrom", Qt::DirectConnection),
          "idle leaf From restoration slot was not invoked");
  to->setText(QString::number(originalTo));
  require(QMetaObject::invokeMethod(&sheet, "setTo", Qt::DirectConnection),
          "idle leaf To restoration slot was not invoked");
  output->setText(QString::fromStdWString(originalOutput.getWideString()));
  require(QMetaObject::invokeMethod(&sheet, "setOutput", Qt::DirectConnection),
          "idle leaf Output restoration slot was not invoked");
  require(task.m_from == originalFrom && task.m_to == originalTo &&
              task.m_outputPath == originalOutput &&
              task.m_chunkSize == originalChunk,
          "idle leaf fixture settings were not restored");
}

void invokeSelectedTasks(const QStringList& taskIds, const char* method) {
  TasksViewer viewer;
  auto* model = qobject_cast<TaskTreeModel*>(viewer.m_treeView->model());
  require(model && !taskIds.isEmpty(),
          "task selection fixture needs a model and selected task IDs");
  const QModelIndex taskList = model->index(0, 0);
  int selected               = 0;
  for (int row = 0; row < model->rowCount(taskList); ++row) {
    const QModelIndex index = model->index(row, 0, taskList);
    auto* item = static_cast<TaskTreeModel::Item*>(index.internalPointer());
    TFarmTask* task = item->getTask();
    if (!task || !taskIds.contains(task->m_id)) continue;
    viewer.m_treeView->selectionModel()->select(
        index, QItemSelectionModel::Select | QItemSelectionModel::Rows);
    ++selected;
  }
  require(selected == taskIds.size(),
          "task selection fixture could not select every requested task row");
  require(QMetaObject::invokeMethod(model, method, Qt::DirectConnection,
                                    Q_ARG(bool, false)),
          "selected task command slot was not invoked");
}

}  // namespace

void startSelectedBatchTasks(const QStringList& taskIds) {
  invokeSelectedTasks(taskIds, "start");
}

void stopSelectedBatchTasks(const QStringList& taskIds) {
  invokeSelectedTasks(taskIds, "stop");
}

void exerciseBatchTaskChunkCollision(const QString& taskId) {
  auto* controller = BatchesController::instance();
  auto* task       = dynamic_cast<TFarmTaskGroup*>(controller->getTask(taskId));
  require(task && task->m_chunkSize > 1 && controller->canEditTask(taskId),
          "chunk collision fixture needs an editable group");
  TasksViewer viewer;
  viewer.setSelected(task);
  TaskSheet& sheet = *viewer.m_taskSheet;
  auto* field      = fieldForLabel<DVGui::LineEdit>(sheet, "Frames per Chunk:");
  const int chunkSize = task->m_chunkSize;
  const bool dirty    = controller->getDirtyFlag();
  std::vector<TFarmTask*> children;
  for (int i = 0; i < task->getTaskCount(); ++i)
    children.push_back(task->getTask(i));
  bool warned = false;
  QObject callbacks;
  QObject::connect(TMsgCore::instance(), &TMsgCore::sendMessage, &callbacks,
                   [&](int, const QString& message) {
                     warned |= message.contains("replacement chunk ID");
                   });
  viewer.stopTimer();
  field->setText(QString::number(chunkSize - 1));
  require(
      QMetaObject::invokeMethod(&sheet, "setChunkSize", Qt::DirectConnection),
      "chunk collision edit slot was not invoked");
  require(warned && task->m_chunkSize == chunkSize &&
              field->text() == QString::number(chunkSize) &&
              sheet.getCurrentTask() == task && viewer.m_timer->isActive() &&
              controller->getDirtyFlag() == dirty,
          "chunk collision did not report failure and restore the editor");
  require(task->getTaskCount() == static_cast<int>(children.size()),
          "chunk collision changed the owned child count");
  for (int i = 0; i < task->getTaskCount(); ++i)
    require(task->getTask(i) == children[i] &&
                controller->getTask(children[i]->m_id) == children[i],
            "chunk collision replaced a child or its controller entry");
}

void exerciseBatchTaskViewGuards(const QString& affectedRootId,
                                 const QString& idleId) {
  auto* controller = BatchesController::instance();
  auto* affected =
      dynamic_cast<TFarmTaskGroup*>(controller->getTask(affectedRootId));
  auto* idle = dynamic_cast<TFarmTaskGroup*>(controller->getTask(idleId));
  require(
      affected && affected->m_isComposerTask && idle,
      "batch task view fixture needs a composer group and unrelated idle task");
  require(!controller->canEditTask(affectedRootId) &&
              controller->canEditTask(idleId),
          "batch task view fixture did not retain only its submitted task");
  const bool wasDirty = controller->getDirtyFlag();
  TasksViewer viewer;
  viewer.setSelected(affected);
  checkBusyName(viewer, *affected);
  checkBusyDependencies(*viewer.m_taskSheet, *affected);
  checkBusyChunks(*viewer.m_taskSheet, *affected);
  checkBusyThreads(*viewer.m_taskSheet, *affected);
  require(controller->getDirtyFlag() == wasDirty,
          "refused task edits changed the task-list dirty flag");
  checkIdleName(viewer, *idle);
  require(idle->getTaskCount() > 0,
          "idle batch group fixture needs an owned leaf");
  checkIdleLeafEdits(viewer, *idle->getTask(0));
}
