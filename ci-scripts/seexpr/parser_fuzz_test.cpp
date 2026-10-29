#include "tseexpr.h"

#include <SeExpr2/Expression.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace TSeExpr;

void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

// A fixed PRNG and bounded grammar keep failures reproducible across platforms.
// No wall-clock seed, arbitrary callbacks or unbounded source loops are used.
class Random {
  std::uint32_t state = 0x53e3c721u;

public:
  std::uint32_t next() {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
  }
  std::size_t pick(std::size_t size) { return next() % size; }
};

struct Scalar {
  std::string source;
  double expected;
};

Scalar scalar(Random &random, int depth) {
  if (depth == 0) {
    switch (random.pick(4)) {
    case 0: return {"$x", 2.0};
    case 1: return {"$y", -3.0};
    case 2: return {"0.5", 0.5};
    default: {
      const auto value = random.pick(10);
      return {std::to_string(value), static_cast<double>(value)};
    }
    }
  }
  const Scalar left = scalar(random, depth - 1);
  const Scalar right = scalar(random, depth - 1);
  switch (random.pick(6)) {
  case 0:
    return {"(" + left.source + "+" + right.source + ")",
            left.expected + right.expected};
  case 1:
    return {"(" + left.source + "-" + right.source + ")",
            left.expected - right.expected};
  case 2:
    return {"(" + left.source + "*" + right.source + ")",
            left.expected * right.expected};
  case 3:
    return {"min(" + left.source + "," + right.source + ")",
            std::min(left.expected, right.expected)};
  case 4:
    return {"max(" + left.source + "," + right.source + ")",
            std::max(left.expected, right.expected)};
  default:
    return {"($x>0?" + left.source + ":" + right.source + ")",
            left.expected};
  }
}

Options options(Type type = Type::Scalar) {
  Options result;
  result.resultType = type;
  result.maxSourceBytes = 4096;
  result.maxNesting = 64;
  result.maxInstructions = 10000;
  result.maxStringBytes = 4096;
  return result;
}

const std::vector<Variable> inputs = {
    {"x", Value::scalar(2)}, {"y", Value::scalar(-3)}};

void checkDiagnostics(const Program &program, std::size_t size) {
  require(!program.errors().empty() && !program.errorMessage().empty(),
          "Rejected generated source has no diagnostic");
  for (const auto &error : program.errors())
    require(!error.message.empty() && error.begin >= 0 &&
                error.end >= error.begin &&
                static_cast<std::size_t>(error.end) <= size,
            "Generated source has an invalid diagnostic range");
}

void recover(Program &program, SeExpr2::Expression &syntax) {
  syntax.setExpr("5*7+2");
  require(syntax.syntaxOK() && syntax.isValid() &&
              syntax.evalFP()[0] == 37,
          "SDK parser failed to recover from a generated source");
  require(program.compile("$x*10+$y", inputs, {}, options()),
          "Native compiler failed to recover from a generated source");
  Value result;
  require(program.evaluate(result) && result.type == Type::Scalar &&
              result.number.size() == 1 && result.number[0] == 17,
          "Generated source corrupted subsequent native evaluation");
}

struct Counts {
  std::size_t candidates = 0, rejected = 0, evaluated = 0;
};

void candidateImpl(const std::string &source, Type type, bool mustReject,
                   Program &program, SeExpr2::Expression &syntax, Counts &counts) {
  require(source.size() <= 4096, "Generated corpus exceeded its source bound");
  ++counts.candidates;

  // Exercise the parser itself even when the application preflight would
  // reject punctuation, dimensions or an embedded null before parsing.
  syntax.setExpr(source);
  const bool parsed = syntax.syntaxOK();
  if (mustReject) require(!parsed, "Malformed generated source passed SDK parsing");
  if (!parsed)
    require(!syntax.getErrors().empty(),
            "SDK parser rejected generated source without diagnostics");

  const bool compiled = program.compile(source, inputs, {}, options(type));
  if (mustReject)
    require(!compiled, "Malformed generated source passed native compilation");
  require(program.source() == source,
          "Native compiler lost generated source needed for repair");
  if (!compiled) {
    ++counts.rejected;
    require(!program.isValid(), "Rejected generated source remained executable");
    checkDiagnostics(program, source.size());
  } else {
    // Mutations can remain valid. They still must either return a finite,
    // correctly shaped value or report a bounded evaluation error.
    Value result = Value::scalar(12345);
    if (program.evaluate(result)) {
      ++counts.evaluated;
      require(result.type == type && result.isFinite(),
              "Generated source returned an invalid typed value");
      if (type != Type::String)
        require(result.number.size() == (type == Type::Scalar ? 1u : 3u),
                "Generated source changed its result dimension");
    } else {
      checkDiagnostics(program, source.size());
      require(result.type == Type::Scalar && result.number.size() == 1 &&
                  result.number[0] == 12345,
              "Failed generated evaluation exposed a partial result");
    }
  }
  // Reuse both evaluator objects after every candidate, not just at exit.
  recover(program, syntax);
}

void candidate(const std::string &source, Type type, bool mustReject,
               Program &program, SeExpr2::Expression &syntax, Counts &counts) {
  try {
    candidateImpl(source, type, mustReject, program, syntax, counts);
  } catch (const std::exception &error) {
    throw std::runtime_error("Generated parser case " +
                             std::to_string(counts.candidates) + ": " + error.what());
  }
}

std::string mutate(const std::string &source, Random &random, unsigned mode) {
  static const char tokens[] = "()[]{};,:?+-*/^=!$\"'012az#\n";
  std::string result = source;
  const std::size_t position = random.pick(result.size() + 1);
  switch (mode) {
  case 0:
    result.insert(position, 1, tokens[random.pick(sizeof(tokens) - 1)]);
    break;
  case 1:
    if (position < result.size()) result.erase(position, 1);
    break;
  case 2:
    if (position < result.size())
      result[position] = tokens[random.pick(sizeof(tokens) - 1)];
    break;
  case 3: result.resize(position); break;
  case 4:
    result.insert(position, result.substr(0, std::min(position, std::size_t(12))));
    break;
  default: result.insert(position, 1, '\0'); break;
  }
  return result;
}

void corpus() {
  Random random;
  Program program;
  SeExpr2::Expression syntax(SeExpr2::Expression::UseInterpreter);
  syntax.setDesiredReturnType(SeExpr2::ExprType().FP(1).Varying());
  Counts counts;
  for (int index = 0; index < 192; ++index) {
    const Scalar generated = scalar(random, 3);
    require(program.compile(generated.source, inputs, {}, options()),
            "Valid generated arithmetic source did not compile");
    Value value;
    require(program.evaluate(value) && value.number.size() == 1 &&
                std::abs(value.number[0] - generated.expected) <
                    1e-10 * std::max(1.0, std::abs(generated.expected)),
            "Generated arithmetic disagreed with its independent oracle");
    candidate(generated.source + "+ ;", Type::Scalar, true,
              program, syntax, counts);
    for (unsigned mode = 0; mode < 6; ++mode)
      candidate(mutate(generated.source, random, mode), Type::Scalar, false,
                program, syntax, counts);
  }

  const std::vector<std::pair<std::string, Type>> grammar = {
      {"a=$x; if(a>0){a=a+1;}else{a=3;} a+$y", Type::Scalar},
      {"def FLOAT twice(FLOAT a){a*2} twice($x)", Type::Scalar},
      {"def FLOAT[3] splat(FLOAT a){a} splat($x)", Type::Vector},
      {"[$x,$y,0.5]", Type::Vector},
      {"ccurve(0.5,0,[0,0,0],1,1,[1,1,1],1)", Type::Vector},
      {"curve($x,0,1,1,2,3,1)", Type::Scalar},
      {"\"cel\"+\"-paint\"", Type::String},
      {"def STRING mark(STRING a){a+\"!\"} mark(\"cel\")", Type::String},
      {"# controls\na=2; a*$x", Type::Scalar},
      {"getVar(\"unbound\",$x)", Type::Scalar}};
  for (const auto &seed : grammar) {
    require(program.compile(seed.first, inputs, {}, options(seed.second)),
            "A valid grammar seed did not compile before mutation");
    Value value;
    require(program.evaluate(value) && value.type == seed.second && value.isFinite(),
            "A valid grammar seed failed evaluation before mutation");
    candidate(seed.first, seed.second, false, program, syntax, counts);
    candidate(seed.first + "\n+ ;", seed.second, true, program, syntax, counts);
    for (int repeat = 0; repeat < 16; ++repeat)
      for (unsigned mode = 0; mode < 6; ++mode)
        candidate(mutate(seed.first, random, mode), seed.second, false,
                  program, syntax, counts);
  }

  // Flat, unary and partial operator trees stress parser-error cleanup beyond
  // ordinary punctuation nesting, while staying within the source-byte bound.
  for (std::size_t length : {32u, 127u, 512u, 1024u}) {
    std::string flat = "1";
    for (std::size_t index = 0; index < length; ++index) flat += "+1";
    candidate(flat + "+ ;", Type::Scalar, true, program, syntax, counts);
    candidate(std::string(length, '-') + ";", Type::Scalar, true,
              program, syntax, counts);
  }

  require(counts.candidates == 2332 && counts.rejected > 500 &&
              counts.evaluated > 20,
          "Generated parser corpus did not exercise both rejection and execution");
  std::cout << "SeExpr deterministic parser fuzz: PASS candidates="
            << counts.candidates << " rejected=" << counts.rejected
            << " evaluated=" << counts.evaluated << '\n';
}
}  // namespace

int main() {
  try {
    corpus();
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "SeExpr deterministic parser fuzz: FAIL: " << error.what() << '\n';
    return 1;
  }
}
