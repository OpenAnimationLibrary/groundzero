#pragma once

#include <QStringList>

void startSelectedBatchTasks(const QStringList& taskIds);
void stopSelectedBatchTasks(const QStringList& taskIds);
void exerciseBatchTaskChunkCollision(const QString& taskId);

/** Exercises TaskSheet edits while the controller retains a submitted task.
 *
 * Args:
 *   affectedRootId: Submitted composer group with chunks and a dependency.
 *   idleId: Unrelated idle group with a composer leaf, also a dependency
 * candidate.
 *
 * Throws:
 *   std::runtime_error: If an edit violates task or editor state expectations.
 */
void exerciseBatchTaskViewGuards(const QString& affectedRootId,
                                 const QString& idleId);
