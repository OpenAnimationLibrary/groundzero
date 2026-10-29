#include "seexprbudget.h"

#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>

// JIT callbacks must return through generated frames normally. The owning
// Program reports the saved exception after evaluation returns to native C++.
extern "C" char *GroundzeroSeExprConcat(const char *first, const char *second,
                                        char **owner) noexcept {
  if (!TSeExprInternal::instructionStepNoThrow()) return nullptr;
  try {
    const std::size_t firstSize = first ? std::strlen(first) : 0;
    const std::size_t secondSize = second ? std::strlen(second) : 0;
    if (firstSize > std::numeric_limits<std::size_t>::max() - secondSize - 1)
      throw std::bad_alloc();
    const std::size_t size = firstSize + secondSize;
    TSeExprInternal::checkStringBytes(size);
    char *joined = static_cast<char *>(std::malloc(size + 1));
    if (!joined) throw std::bad_alloc();
    if (firstSize) std::memcpy(joined, first, firstSize);
    if (secondSize) std::memcpy(joined + firstSize, second, secondSize);
    joined[size] = '\0';
    std::free(*owner);
    *owner = joined;
    return joined;
  } catch (...) {
    TSeExprInternal::captureEvaluationFailure(std::current_exception());
    return nullptr;
  }
}
