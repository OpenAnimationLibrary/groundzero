#include <SeExpr2/ExprFunc.h>
#include <SeExpr2/Expression.h>

#include <cmath>
#include <filesystem>
#include <iomanip>
#include <initializer_list>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {
void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

std::string formulaContext(const char* source, const char* phase) {
  return std::string(phase) + ": scalar formula `" + source + "`";
}

std::string number(double value) {
  std::ostringstream output;
  output << std::setprecision(17) << value;
  return output.str();
}

double evaluateExpression(const char* source, const char* phase) {
  SeExpr2::Expression expression(source, SeExpr2::ExprType().FP(1),
                               SeExpr2::Expression::UseInterpreter);
  const auto context = formulaContext(source, phase);
  const bool valid = expression.isValid();
  require(valid, context + " did not bind: " + expression.parseError());
  require(expression.returnType().isFP(1),
          context + " expected Float, actual " + expression.returnType().toString());
  const double* result = expression.evalFP();
  require(result != nullptr, context + " returned no scalar result");
  const double actual = result[0];
  require(std::isfinite(actual),
          context + " expected a finite result, actual " + number(actual));
  return actual;
}

void checkExpression(const char* source, double expected, const char* phase) {
  const double actual = evaluateExpression(source, phase);
  require(std::abs(actual - expected) < 1e-12,
          formulaContext(source, phase) + " expected " + number(expected) +
              ", actual " + number(actual));
}

void requireUnregistered(const char* phase) {
  for (const char* name : {"sdk_fixture_offset", "sdk_fixture_registrations",
                           "sdk_fixture_reset_registrations"}) {
    require(!SeExpr2::ExprFunc::lookup(name),
            std::string(phase) + ": plugin function remains registered: " + name);
    require(SeExpr2::ExprFunc::getDocString(name).empty(),
            std::string(phase) + ": plugin help remains registered: " + name);
  }
}
}

int main(int argc, char** argv) {
  try {
    require(argc == 3, "plugin test requires valid and invalid fixture modules");
    const auto plugin = std::filesystem::absolute(std::filesystem::u8path(argv[1]));
    const auto invalid = std::filesystem::absolute(std::filesystem::u8path(argv[2]));
    SeExpr2::ExprFunc::init();
    requireUnregistered("before explicit loading");
    SeExpr2::ExprFunc::loadPlugin(invalid.u8string().c_str());
    requireUnregistered("after loading invalid module");
    SeExpr2::ExprFunc::loadPlugin(plugin.u8string().c_str());
    checkExpression("sdk_fixture_offset(5)", 22, "first explicit load");
    checkExpression("sdk_fixture_registrations()", 1, "first explicit load");
    require(SeExpr2::ExprFunc::getDocString("sdk_fixture_offset").find("offset") != std::string::npos,
            "plugin help was not published");
    SeExpr2::ExprFunc::loadPlugin(plugin.u8string().c_str());
    checkExpression("sdk_fixture_registrations()", 1, "duplicate first explicit load");
    // Establish a known counter baseline explicitly. Both a newly mapped
    // module and a resident module must then increment exactly once on reload.
    checkExpression("sdk_fixture_reset_registrations()", 0, "before first cleanup");
    checkExpression("sdk_fixture_registrations()", 0, "before first cleanup");
    // Every evaluator is destroyed before cleanup. Registry objects must be
    // removed before releasing the host's module handles; the OS may keep the
    // module mapped, so cleanup does not promise to reset module static state.
    SeExpr2::ExprFunc::cleanup();
    SeExpr2::ExprFunc::init();
    requireUnregistered("after first cleanup");
    SeExpr2::ExprFunc::loadPlugins(plugin.parent_path().u8string().c_str());
    checkExpression("sdk_fixture_offset(-2)", 15, "directory reload");
    checkExpression("sdk_fixture_registrations()", 1, "directory reload");
    require(SeExpr2::ExprFunc::getDocString("sdk_fixture_offset").find("offset") != std::string::npos,
            "directory reload did not republish plugin help");
    SeExpr2::ExprFunc::loadPlugin(plugin.u8string().c_str());
    checkExpression("sdk_fixture_registrations()", 1,
                    "duplicate explicit load after reload");
    SeExpr2::ExprFunc::loadPlugins(plugin.parent_path().u8string().c_str());
    checkExpression("sdk_fixture_registrations()", 1,
                    "duplicate directory load after reload");
    checkExpression("sdk_fixture_offset(5)", 22, "after duplicate directory load");
    SeExpr2::ExprFunc::cleanup();
    SeExpr2::ExprFunc::init();
    requireUnregistered("after second cleanup");
    SeExpr2::ExprFunc::cleanup();
    // A second cleanup must not close stale native handles again.
    SeExpr2::ExprFunc::cleanup();
    std::cout << "SeExpr trusted SDK plugins: PASS\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
