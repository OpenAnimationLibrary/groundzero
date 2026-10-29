#pragma once

#include "tseexpr.h"

#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace TSeExprQualification {

inline void equality(TSeExpr::Backend backend) {
  using namespace TSeExpr;
  for (const int dimension : {1, 3, 4, 16}) {
    const std::string declared = "FLOAT[" + std::to_string(dimension) + "] UNIFORM";
    const std::vector<std::pair<std::string, bool>> cases{
        {"$left==$right", true},
        {"$left!=$right", false},
        {"$left==$scalar", true},
        {"$scalar==$left", true},
        {"$left!=$scalar", false},
        {"$scalar!=$left", false},
        {"def FLOAT UNIFORM same(" + declared +
             " a,FLOAT UNIFORM b){a==b} same($left,$scalar)", true},
        {"def FLOAT UNIFORM different(" + declared +
             " a,FLOAT UNIFORM b){a!=b} different($left,$scalar)", false}};
    for (const auto &fixture : cases) {
      const Value matching = Value::numeric(std::vector<double>(dimension, 2.0));
      std::vector<double> changed(static_cast<std::size_t>(dimension), 2.0);
      changed.back() = 3.0;
      Options options;
      options.backend = backend;
      options.resultType = Type::Scalar;
      Program program;
      if (!program.compile(fixture.first,
                           {{"left", matching, Lifetime::Uniform},
                            {"right", matching, Lifetime::Uniform},
                            {"scalar", Value::scalar(2.0), Lifetime::Uniform}},
                           {}, options))
        throw std::runtime_error("Equality fixture did not compile: " + fixture.first +
                                 ": " + program.errorMessage());
      const auto expect = [&](bool expected) {
        Value result;
        if (!program.evaluate(result))
          throw std::runtime_error("Equality fixture failed evaluation: " + fixture.first +
                                   ": " + program.errorMessage());
        if (result.type != Type::Scalar || result.number.size() != 1 ||
            result.number[0] != (expected ? 1.0 : 0.0))
          throw std::runtime_error("Incorrect whole-vector equality at dimension " +
                                   std::to_string(dimension) + ": " + fixture.first);
      };
      expect(fixture.second);
      if (!program.setVariable("left", Value::numeric(changed)))
        throw std::runtime_error("Could not update a uniform equality input");
      expect(!fixture.second);
      if (!program.setVariable("left", matching))
        throw std::runtime_error("Could not restore a uniform equality input");
      expect(fixture.second);
    }

    // Inequality remains the complement of equality when an input component
    // is unordered. Both backends may expose IEEE values to trusted hosts.
    std::vector<double> unordered(static_cast<std::size_t>(dimension), 2.0);
    unordered.back() = std::numeric_limits<double>::quiet_NaN();
    for (const auto &fixture : std::vector<std::pair<std::string, double>>{
             {"$left==$right", 0.0}, {"$left!=$right", 1.0}}) {
      Options options;
      options.backend = backend;
      options.resultType = Type::Scalar;
      options.requireFiniteResult = false;
      Program program;
      if (!program.compile(fixture.first,
                           {{"left", Value::numeric(unordered)},
                            {"right", Value::numeric(unordered)}}, {}, options))
        throw std::runtime_error("Unordered equality fixture did not compile: " + program.errorMessage());
      Value result;
      if (!program.evaluate(result) || result.number.size() != 1 ||
          result.number[0] != fixture.second)
        throw std::runtime_error("Unordered equality differs at dimension " + std::to_string(dimension));
    }
  }
}

}  // namespace TSeExprQualification
