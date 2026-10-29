#include "seexprbudget.h"

#include <SeExpr2/ExprFuncStandard.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <vector>

// JIT callbacks must return through generated frames normally. The owning
// Program reports the saved exception after evaluation returns to native C++.
extern "C" char *GroundzeroSeExprConcat(const char *first, const char *second,
                                        char **owner) noexcept {
  static char empty[] = "";
  // A failed child value can still reach another generated string operation
  // before the host rethrows its saved error. Keep that temporary readable;
  // ownership remains with the node's existing allocation on every failure.
  if (!TSeExprInternal::instructionStepNoThrow()) return empty;
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
    return empty;
  }
}

extern "C" int GroundzeroSeExprStringCompare(const char *first,
                                             const char *second) noexcept {
  if (!TSeExprInternal::instructionStepNoThrow()) return 0;
  return std::strcmp(first ? first : "", second ? second : "");
}

extern "C" void GroundzeroSeExprStandardFunction(
    int kind, void *function, int count, double *arguments,
    double *result) noexcept {
  using Standard = SeExpr2::ExprFuncStandard;
  using SeExpr2::Vec3d;
  const auto type = static_cast<Standard::FuncType>(kind);
  const bool vectorResult = type == Standard::FUNC1VV ||
                            type == Standard::FUNC2VV ||
                            type == Standard::FUNCNVV;
  const int resultWidth = vectorResult ? 3 : 1;
  const auto clearResult = [&]() noexcept { std::fill_n(result, resultWidth, 0.0); };
  clearResult();
  if (!TSeExprInternal::instructionStepNoThrow()) return;
  try {
    if (!function || count < 0 || kind <= Standard::NONE || kind > Standard::FUNCNVV)
      throw std::runtime_error("Invalid standard expression function signature.");
    if (type >= Standard::FUNC0 && type <= Standard::FUNC6 &&
        count != kind - Standard::FUNC0)
      throw std::runtime_error("Standard expression function argument count differs from its signature.");
    if (((type == Standard::FUNC1V || type == Standard::FUNC1VV) && count != 1) ||
        ((type == Standard::FUNC2V || type == Standard::FUNC2VV) && count != 2))
      throw std::runtime_error("Standard vector function argument count differs from its signature.");
    const auto storeVector = [&](const Vec3d &value) {
      for (int component = 0; component < 3; ++component)
        result[component] = value[component];
    };
    const auto vectorArguments = [&]() {
      std::vector<Vec3d> values(static_cast<std::size_t>(count));
      for (int argument = 0; argument < count; ++argument)
        values[static_cast<std::size_t>(argument)] = Vec3d::copy(arguments + 3 * argument);
      return values;
    };
    // Invoke the real upstream C++ callback signatures in native frames. The
    // generated caller passes plain arrays, avoiding assumptions about the
    // platform's Vec3d return ABI and allowing every exception to be caught.
    switch (type) {
    case Standard::FUNC0:
      result[0] = reinterpret_cast<Standard::Func0 *>(function)();
      break;
    case Standard::FUNC1:
      result[0] = reinterpret_cast<Standard::Func1 *>(function)(arguments[0]);
      break;
    case Standard::FUNC2:
      result[0] = reinterpret_cast<Standard::Func2 *>(function)(
          arguments[0], arguments[1]);
      break;
    case Standard::FUNC3:
      result[0] = reinterpret_cast<Standard::Func3 *>(function)(
          arguments[0], arguments[1], arguments[2]);
      break;
    case Standard::FUNC4:
      result[0] = reinterpret_cast<Standard::Func4 *>(function)(
          arguments[0], arguments[1], arguments[2], arguments[3]);
      break;
    case Standard::FUNC5:
      result[0] = reinterpret_cast<Standard::Func5 *>(function)(
          arguments[0], arguments[1], arguments[2], arguments[3], arguments[4]);
      break;
    case Standard::FUNC6:
      result[0] = reinterpret_cast<Standard::Func6 *>(function)(
          arguments[0], arguments[1], arguments[2], arguments[3], arguments[4],
          arguments[5]);
      break;
    case Standard::FUNCN:
      result[0] = reinterpret_cast<Standard::Funcn *>(function)(count, arguments);
      break;
    case Standard::FUNC1V:
      result[0] = reinterpret_cast<Standard::Func1v *>(function)(
          Vec3d::copy(arguments));
      break;
    case Standard::FUNC2V:
      result[0] = reinterpret_cast<Standard::Func2v *>(function)(
          Vec3d::copy(arguments), Vec3d::copy(arguments + 3));
      break;
    case Standard::FUNCNV: {
      const auto values = vectorArguments();
      result[0] = reinterpret_cast<Standard::Funcnv *>(function)(count, values.data());
      break;
    }
    case Standard::FUNC1VV:
      storeVector(reinterpret_cast<Standard::Func1vv *>(function)(
          Vec3d::copy(arguments)));
      break;
    case Standard::FUNC2VV:
      storeVector(reinterpret_cast<Standard::Func2vv *>(function)(
          Vec3d::copy(arguments), Vec3d::copy(arguments + 3)));
      break;
    case Standard::FUNCNVV: {
      const auto values = vectorArguments();
      storeVector(reinterpret_cast<Standard::Funcnvv *>(function)(count, values.data()));
      break;
    }
    default:
      throw std::runtime_error("Unknown standard expression function signature.");
    }
    TSeExprInternal::rethrowEvaluationFailure();
  } catch (...) {
    clearResult();
    TSeExprInternal::captureEvaluationFailure(std::current_exception());
  }
}
