#pragma once

#ifndef BATCHES_H
#define BATCHES_H

#include <QObject>
#include <map>
#include <memory>
#include <set>
#include <vector>
#include <QString>
#include "tfarmtask.h"
#include "tfilepath.h"
#include "tthread.h"
#include "filebrowserpopup.h"

class TFarmController;
struct BatchExecutionState;
struct BatchExecutionRecord;
namespace BatchTaskExecution {
struct Plan;
struct Identity;
struct Outcome;
struct TestHooks;
}  // namespace BatchTaskExecution

//------------------------------------------------------------------------------

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4786)
#endif

//------------------------------------------------------------------------------
class TaskTreeModel;

class BatchesController final : public QObject {  // singleton
  Q_OBJECT
public:
  static BatchesController *instance();

  void setDirtyFlag(bool state);
  bool getDirtyFlag() const { return m_dirtyFlag; }
  QString getListName() const;
  void addComposerTask(const TFilePath &taskFilePath);
  void addCleanupTask(const TFilePath &taskFilePath);

  void addTask(const QString &id, TFarmTask *task, bool doUpdate = true);
  void removeTask(const QString &id);
  void removeAllTasks();
  bool canEditTask(const QString& id) const;
  bool setTaskChunkSize(const QString& id, int chunkSize);
  bool setTaskChunkSettings(const QString& id, int from, int to, int chunkSize,
                            const TFilePath& outputPath);

#ifdef BUILD_UI_PORT_TESTS
  void setExecutionTestHooks(
      std::shared_ptr<BatchTaskExecution::TestHooks> hooks);
  TThread::Executor& executionTestExecutor() { return m_localExecutor; }
  size_t pendingExecutionCount() const;
  quint64 taskGeneration() const;
#endif

  // Legacy farm messages have no run token. Deliver their values on this
  // object's thread.
  void postTaskProgress(const QString& id, bool successful);

  int getTaskCount() const;
  QString getTaskId(int index) const;
  TFarmTask *getTask(int index) const;
  TFarmTask *getTask(const QString &id) const;
  bool getTaskInfo(const QString &id, QString &parent, QString &name,
                   TaskState &status);
  TaskState getTaskStatus(const QString &id) const;
  void loadTask(bool isRenderTask);

  void getTasks(const QString &parentId, std::vector<QString> &tasks) const;
  void registerTasksTree(TaskTreeModel *tree);
  void unregisterTasksTree(TaskTreeModel *tree);

  TaskTreeModel *getTasksTree() {
    return m_tasksTrees.empty() ? nullptr : *m_tasksTrees.begin();
  }

  void startAll();
  void start(const QString &taskId);
  void stopAll();
  void stop(const QString &taskId);

  void load();
  void doLoad(const TFilePath &fp);
  void save();
  void saveas();
  void doSave(const TFilePath &fp = TFilePath());

  class Observer {
  public:
    virtual ~Observer();
    virtual void update() = 0;
  };

  void attach(Observer *obs);
  void detach(Observer *obs);
  void notify();

  void setController(TFarmController *controller);
  TFarmController *getController() const;
  void update();  // aggiorna lo stato del Batch interrogando il FarmController

public slots:
  void onExit(bool &);

protected:
  BatchesController();
  ~BatchesController();

  // non implementati
  BatchesController(const BatchesController &);
  void operator=(const BatchesController &);

private:
  bool m_dirtyFlag;
  TFilePath m_filepath;
  std::map<QString, TFarmTask *> m_tasks;
  std::set<Observer *> m_observers;
  int m_localControllerPortNumber;
  TFarmController *m_controller;
  std::map<QString, QString> m_farmIdsTable;
  std::set<TaskTreeModel *> m_tasksTrees;
  TThread::Executor m_localExecutor;
  std::unique_ptr<BatchExecutionState> m_execution;

  static BatchesController *m_instance;

private:
  static inline QString taskBusyStr();
  std::shared_ptr<BatchTaskExecution::Plan> captureTask(TFarmTask* task);
  void startFarmTask(TFarmTask* task);
  void suspendFarmTask(const QString& id);
  void submitAllTasks();
  void submitTask(std::shared_ptr<BatchTaskExecution::Plan> plan);
  void applyOutcome(quint64 submission,
                    const BatchTaskExecution::Outcome& outcome);
  TFarmTask* liveTask(const BatchTaskExecution::Identity& identity) const;
  void applyTaskOutcome(TFarmTask& task, const BatchExecutionRecord& record,
                        const BatchTaskExecution::Outcome& outcome);
  void retireTask(quint64 submission, bool failed);
  void stopSubmission(quint64 submission);
  void shutdownExecution();
  bool tasksAreEditable() const;
  void releaseTasks();
  std::vector<std::unique_ptr<TFarmTask>> replaceChunks(
      TFarmTaskGroup& group, int from, int to, int chunkSize,
      const TFilePath& outputPath);
};
//-----------------------------------------------------------------------------
class LoadTaskListPopup final : public FileBrowserPopup {
  Q_OBJECT

public:
  LoadTaskListPopup();

  bool execute() override;
};

//-----------------------------------------------------------------------------

class LoadTaskPopup final : public FileBrowserPopup {
  Q_OBJECT

  bool m_isRenderTask;

public:
  LoadTaskPopup();

  bool execute() override;
  void open(bool isRenderTask);
};

//-----------------------------------------------------------------------------

class SaveTaskListPopup final : public FileBrowserPopup {
  Q_OBJECT

public:
  SaveTaskListPopup();

  bool execute() override;
};

//------------------------------------------------------------------------------

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#endif
