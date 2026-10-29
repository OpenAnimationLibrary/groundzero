#include <SeExpr2/ExprFunc.h>
#include <SeExpr2/Expression.h>

#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

void checkExpression(const char* source, double expected) {
  SeExpr2::Expression expression(source, SeExpr2::ExprType().FP(1),
                               SeExpr2::Expression::UseInterpreter);
  require(expression.isValid(), "loaded plugin formula did not bind");
  require(std::abs(expression.evalFP()[0] - expected) < 1e-12,
          "loaded plugin returned the wrong value");
}
}

int main(int argc, char** argv) {
  try {
    require(argc == 3, "plugin test requires valid and invalid fixture modules");
    const auto plugin = std::filesystem::absolute(std::filesystem::u8path(argv[1]));
    const auto invalid = std::filesystem::absolute(std::filesystem::u8path(argv[2]));
    SeExpr2::ExprFunc::init();
    require(!SeExpr2::ExprFunc::lookup("sdk_fixture_offset"),
            "plugin function appeared before explicit loading");
    SeExpr2::ExprFunc::loadPlugin(invalid.u8string().c_str());
    require(!SeExpr2::ExprFunc::lookup("sdk_fixture_offset"),
            "invalid module changed the registry");
    SeExpr2::ExprFunc::loadPlugin(plugin.u8string().c_str());
    checkExpression("sdk_fixture_offset(5)", 22);
    checkExpression("sdk_fixture_registrations()", 1);
    require(SeExpr2::ExprFunc::getDocString("sdk_fixture_offset").find("offset") != std::string::npos,
            "plugin help was not published");
    SeExpr2::ExprFunc::loadPlugin(plugin.u8string().c_str());
    checkExpression("sdk_fixture_registrations()", 1);
    // Every evaluator is destroyed before unloading. The registry's owned
    // function objects are destroyed before the underlying native module.
    SeExpr2::ExprFunc::cleanup();
    SeExpr2::ExprFunc::init();
    require(!SeExpr2::ExprFunc::lookup("sdk_fixture_offset"),
            "plugin function survived explicit cleanup");
    SeExpr2::ExprFunc::loadPlugins(plugin.parent_path().u8string().c_str());
    checkExpression("sdk_fixture_offset(-2)", 15);
    checkExpression("sdk_fixture_registrations()", 1);
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
