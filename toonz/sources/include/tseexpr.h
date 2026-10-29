#pragma once

#ifndef TSEEXPR_INCLUDED
#define TSEEXPR_INCLUDED

#include "tcommon.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <exception>
#include <memory>
#include <string>
#include <vector>

#undef DVAPI
#ifdef TNZBASE_EXPORTS
#define DVAPI DV_EXPORT_API
#else
#define DVAPI DV_IMPORT_API
#endif

// Native ownership and value types. No SeExpr ABI objects escape this API.
namespace TSeExpr {

// Render hosts can detect GUI-safe expression failures at a batch boundary.
// The counter carries no source text or document data.
DVAPI std::uint64_t failureCount();
DVAPI void reportFailure();

enum class Type { Scalar, Vector, String };
enum class Lifetime { Constant, Uniform, Varying };
enum class Backend { Interpreter, LLVM };

struct DVAPI Value {
  Type type = Type::Scalar;
  std::vector<double> number{0.0};
  std::string text;

  static Value scalar(double value);
  static Value vector(double x, double y, double z);
  static Value numeric(std::vector<double> values);
  static Value string(std::string value);
  bool isFinite() const;
};

struct Error {
  std::string message;
  int begin = 0;  // UTF-8 source byte offsets, end exclusive.
  int end = 0;
};

struct Variable {
  std::string name;  // Without the optional source '$' prefix.
  Value value;
  Lifetime lifetime = Lifetime::Varying;
};

struct Function {
  std::string name;
  std::string documentation;
  Type resultType = Type::Scalar;
  std::vector<Type> argumentTypes;
  // -1 means exactly argumentTypes.size(). A variadic signature repeats its
  // last argument type; an empty signature with maxArgs > 0 accepts scalars.
  int minArgs = -1;
  int maxArgs = -1;
  std::function<Value(const std::vector<Value> &)> evaluate;
  int resultDimension = 3;
  std::vector<int> argumentDimensions;  // Vector arguments default to 3.
};

struct FunctionInfo {
  std::string name;
  std::string documentation;
  int minArgs = 0;
  int maxArgs = 0;
  bool threadSafe = true;
};

struct Options {
  Backend backend = Backend::Interpreter;
  Type resultType = Type::Vector;
  int resultDimension = 3;  // Used for Type::Vector; scalar/string have dimension 1.
  std::size_t maxSourceBytes = 65536;
  std::size_t maxNesting = 128;
  std::size_t maxVariables = 512;
  // The upstream interpreter supplies operations for dimensions 1 through 16.
  std::size_t maxDimension = 16;
  std::size_t maxInstructions = 1000000;
  std::size_t maxStringBytes = 1048576;
  // Rendering hosts reject console-output functions explicitly. General
  // authoring/evaluation tools may opt in; this does not change the grammar.
  bool allowConsoleOutput = false;
  bool requireFiniteResult = true;
  std::function<bool()> cancelled;
  // Optional host exception formatter (for native exceptions that do not
  // derive from std::exception). It must not throw.
  std::function<std::string(std::exception_ptr)> exceptionMessage;
};

// Each Program owns a complete interpreter and its custom functions. Use a
// separate Program per render worker; concurrent calls on one Program are not
// supported. Reentrant calls or mutation from a compile/evaluation callback
// are rejected; distinct Programs may be nested. Moving an active Program
// throws before transferring ownership. Returned Values own their storage.
class DVAPI Program {
public:
  Program();
  ~Program();
  Program(Program &&);
  Program &operator=(Program &&);
  Program(const Program &) = delete;
  Program &operator=(const Program &) = delete;

  bool compile(const std::string &source,
               const std::vector<Variable> &variables = {},
               const std::vector<Function> &functions = {},
               const Options &options = Options());
  bool isValid() const;
  bool setVariable(const std::string &name, const Value &value);
  bool setScalar(const std::string &name, double value);
  bool setVector(const std::string &name, double x, double y, double z);
  bool setString(const std::string &name, const std::string &value);
  bool evaluate(Value &result);
  // Sequentially evaluates rows with the same compiled expression. Results
  // are cleared on any error; successful rows retain their last input values.
  bool evaluateBatch(const std::vector<std::vector<Variable>> &rows,
                     std::vector<Value> &results);

  const std::string &source() const;
  Type resultType() const;
  const std::vector<Error> &errors() const;
  std::string errorMessage() const;
  std::vector<std::string> usedVariables() const;
  std::vector<std::string> usedFunctions() const;

  static std::vector<FunctionInfo> builtinFunctions();
  // Native host extensions are separate from the audited upstream registry.
  static std::vector<FunctionInfo> nativeFunctions();
  static bool backendAvailable(Backend backend);
  static std::string engineVersion();
  static unsigned int functionPolicyVersion();

private:
  class Imp;
  std::unique_ptr<Imp> m_imp;
};

}  // namespace TSeExpr

#endif
