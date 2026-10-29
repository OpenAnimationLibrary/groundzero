#include <SeExpr2/ExprFunc.h>

namespace {
// Closing the host's handle need not unmap the module or reset static state.
// The fixture exposes an explicit reset so tests can count new initializer
// calls independently of the operating system's module residency policy.
int registrations = 0;
double offset(double value) { return value + 17.0; }
double registrationCount() { return registrations; }
double resetRegistrationCount() {
  registrations = 0;
  return registrations;
}
}

#ifdef _WIN32
#define PLUGIN_EXPORT __declspec(dllexport)
#else
#define PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

// This is the upstream C++ registration ABI. Use the host callback instead of
// a private registry from a statically linked plugin core.
extern "C" PLUGIN_EXPORT void SeExpr2PluginInit(SeExpr2::ExprFunc::Define3 define) {
  ++registrations;
  define("sdk_fixture_offset", SeExpr2::ExprFunc(offset),
         "float sdk_fixture_offset(float value)\nAdd the fixture offset.");
  define("sdk_fixture_registrations", SeExpr2::ExprFunc(registrationCount),
         "float sdk_fixture_registrations()\nReturn fixture registration count.");
  define("sdk_fixture_reset_registrations", SeExpr2::ExprFunc(resetRegistrationCount),
         "float sdk_fixture_reset_registrations()\nReset the fixture registration count.");
}
