#include "tseexpr.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {
void print(const TSeExpr::Value& value) {
  if (value.type == TSeExpr::Type::String) {
    std::cout << value.text;
    return;
  }
  if (value.number.size() > 1) std::cout << '[';
  for (std::size_t i = 0; i < value.number.size(); ++i) {
    if (i) std::cout << ',';
    std::cout << std::setprecision(17) << value.number[i];
  }
  if (value.number.size() > 1) std::cout << ']';
}

bool evaluate(const std::string& source, const std::vector<TSeExpr::Variable>& values,
              TSeExpr::Value& result) {
  // Match the calculator's numeric/vector convenience while preserving owned
  // string results. Recompilation is appropriate for one user-entered line.
  TSeExpr::Options options;
  for (const auto type : {TSeExpr::Type::Scalar, TSeExpr::Type::Vector,
                          TSeExpr::Type::String}) {
    options.resultType = type;
    TSeExpr::Program program;
    if (program.compile(source, values, {}, options) && program.evaluate(result))
      return true;
    if (type == TSeExpr::Type::String) std::cerr << program.errorMessage() << '\n';
  }
  return false;
}

int calculator(int argc, char** argv) {
  if (argc == 2) {
    TSeExpr::Value value;
    if (!evaluate(argv[1], {}, value)) return 1;
    print(value); std::cout << '\n';
    return 0;
  }
  if (argc != 1) {
    std::cerr << "Usage: seexpr-calc [expression]\n";
    return 2;
  }
  std::vector<TSeExpr::Variable> history;
  std::string line;
  while (std::getline(std::cin, line)) {
    if (line == "q" || line == "quit") break;
    if (line.empty()) continue;
    TSeExpr::Value value;
    if (evaluate(line, history, value)) {
      print(value); std::cout << '\n';
      // The upstream calculator's _0, _1, ... references remain available.
      // Keep a bounded history instead of indexing past its original array.
      if (history.size() < 256)
        history.push_back({"_" + std::to_string(history.size()), value});
    } else {
      return 1;
    }
  }
  return 0;
}

int graph(int argc, char** argv) {
  if (argc < 2 || argc > 4) {
    std::cerr << "Usage: seexpr-graph expression [xmin xmax]\n";
    return 2;
  }
  double xmin = -10, xmax = 10;
  if (argc == 4) {
    char *endMin = nullptr, *endMax = nullptr;
    xmin = std::strtod(argv[2], &endMin); xmax = std::strtod(argv[3], &endMax);
    if (!endMin || *endMin || !endMax || *endMax || !std::isfinite(xmin) ||
        !std::isfinite(xmax) || xmin >= xmax) return 2;
  } else if (argc != 2) return 2;
  TSeExpr::Options options;
  options.resultType = TSeExpr::Type::Scalar;
  TSeExpr::Program program;
  if (!program.compile(argv[1], {{"x", TSeExpr::Value::scalar(xmin)}}, {}, options)) {
    std::cerr << program.errorMessage() << '\n'; return 1;
  }
  constexpr int width = 72, height = 25;
  std::vector<double> values;
  double ymin = std::numeric_limits<double>::max(), ymax = -ymin;
  for (int i = 0; i < width; ++i) {
    program.setScalar("x", xmin + (xmax - xmin) * i / (width - 1));
    TSeExpr::Value value;
    if (!program.evaluate(value)) {
      std::cerr << program.errorMessage() << '\n'; return 1;
    }
    values.push_back(value.number[0]);
    ymin = std::min(ymin, values.back()); ymax = std::max(ymax, values.back());
  }
  if (ymin == ymax) { ymin -= 1; ymax += 1; }
  std::vector<std::string> rows(height, std::string(width, ' '));
  for (int x = 0; x < width; ++x) {
    const int row = std::clamp(int(std::lround((ymax - values[x]) / (ymax - ymin) * (height - 1))),
                               0, height - 1);
    rows[row][x] = '#';
  }
  for (const auto& row : rows) std::cout << row << '\n';
  return 0;
}
}

int main(int argc, char** argv) {
  const std::string name = argc ? argv[0] : "";
  return name.find("graph") != std::string::npos ? graph(argc, argv) : calculator(argc, argv);
}
