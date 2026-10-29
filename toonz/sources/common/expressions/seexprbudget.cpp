#include "seexprbudget.h"

#include <stdexcept>
#include <utility>

namespace TSeExprInternal {
namespace {
thread_local EvaluationBudgetGuard *currentBudget = nullptr;
}

EvaluationBudgetGuard::EvaluationBudgetGuard(
    std::size_t operations, const std::function<bool()> &cancelled,
    std::size_t maxStringBytes, std::size_t maxAstDepth)
    : m_remaining(operations)
    , m_cancelled(cancelled)
    , m_previous(currentBudget)
    , m_maxStringBytes(maxStringBytes)
    , m_maxAstDepth(maxAstDepth) {
  currentBudget = this;
}

EvaluationBudgetGuard::~EvaluationBudgetGuard() { currentBudget = m_previous; }

void instructionStep() {
  // Nested channel/custom-function evaluations count against every caller's
  // budget as well as their own. Exceptions unwind the guard stack normally.
  for (auto *budget = currentBudget; budget; budget = budget->m_previous) {
    if (budget->m_failure) std::rethrow_exception(budget->m_failure);
    if (budget->m_remaining == 0)
      throw std::runtime_error("Expression instruction budget exceeded.");
    --budget->m_remaining;
    if (budget->m_untilCancellation == 0) {
      budget->m_untilCancellation = 255;
      if (budget->m_cancelled && budget->m_cancelled())
        throw std::runtime_error("Expression evaluation cancelled.");
    } else {
      --budget->m_untilCancellation;
    }
  }
}

void captureEvaluationFailure(std::exception_ptr failure) noexcept {
  if (currentBudget && !currentBudget->m_failure)
    currentBudget->m_failure = std::move(failure);
}

bool instructionStepNoThrow() noexcept {
  try {
    instructionStep();
    return true;
  } catch (...) {
    captureEvaluationFailure(std::current_exception());
    return false;
  }
}

void rethrowEvaluationFailure() {
  for (auto *budget = currentBudget; budget; budget = budget->m_previous)
    if (budget->m_failure) std::rethrow_exception(budget->m_failure);
}

void checkStringBytes(std::size_t bytes) {
  for (auto *budget = currentBudget; budget; budget = budget->m_previous)
    if (bytes > budget->m_maxStringBytes)
      throw std::runtime_error("Expression string size limit exceeded.");
}

bool checkStringBytesNoThrow(std::size_t bytes) noexcept {
  try {
    checkStringBytes(bytes);
    return true;
  } catch (...) {
    captureEvaluationFailure(std::current_exception());
    return false;
  }
}

void checkAstDepth(std::size_t depth) {
  if (!currentBudget && depth > 512)
    throw std::runtime_error("Expression syntax tree depth exceeds the SDK limit.");
  for (auto *budget = currentBudget; budget; budget = budget->m_previous)
    if (depth > budget->m_maxAstDepth)
      throw std::runtime_error("Expression syntax tree depth exceeds the configured limit.");
}

}  // namespace TSeExprInternal
