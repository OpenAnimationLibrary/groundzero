// A valid shared library without the SeExpr entry point exercises rejection
// and handle cleanup independently of a missing file.
#ifdef _WIN32
#define PLUGIN_EXPORT __declspec(dllexport)
#else
#define PLUGIN_EXPORT __attribute__((visibility("default")))
#endif
extern "C" PLUGIN_EXPORT int unrelated_sdk_entry() { return 3; }
