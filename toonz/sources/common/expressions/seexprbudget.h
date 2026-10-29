#pragma once

#include <cstddef>
#include <functional>
#include <exception>

namespace TSeExprInternal {

// Internal hook used by the qualified interpreter build. With no native
// evaluation guard installed, standalone SDK evaluation behaves as upstream.
class EvaluationBudgetGuard {
public:
  EvaluationBudgetGuard(std::size_t operations,
                        const std::function<bool()> &cancelled,
                        std::size_t maxStringBytes = 1048576,
                        std::size_t maxAstDepth = 128);
  ~EvaluationBudgetGuard();
  EvaluationBudgetGuard(const EvaluationBudgetGuard &) = delete;
  EvaluationBudgetGuard &operator=(const EvaluationBudgetGuard &) = delete;

private:
  friend void instructionStep();
  friend bool instructionStepNoThrow() noexcept;
  friend void captureEvaluationFailure(std::exception_ptr) noexcept;
  friend void rethrowEvaluationFailure();
  friend void checkStringBytes(std::size_t);
  friend void checkAstDepth(std::size_t);
  std::size_t m_remaining;
  unsigned int m_untilCancellation = 0;
  const std::function<bool()> &m_cancelled;
  EvaluationBudgetGuard *m_previous;
  std::exception_ptr m_failure;
  std::size_t m_maxStringBytes;
  std::size_t m_maxAstDepth;
};

void instructionStep();
bool instructionStepNoThrow() noexcept;
void captureEvaluationFailure(std::exception_ptr failure) noexcept;
void rethrowEvaluationFailure();
void checkStringBytes(std::size_t bytes);
bool checkStringBytesNoThrow(std::size_t bytes) noexcept;
void checkAstDepth(std::size_t depth);

}  // namespace TSeExprInternal
