"""Qualify explicit SDK plugin loading in the pinned core build copy.

Native modules are trusted executable code. This patch supplies the same
registration ABI on supported platforms; it does not enable scene-driven or
environment-driven discovery.
"""


def replace_once(source, old, new, label):
    if source.count(old) != 1:
        raise RuntimeError(f"Pinned SDK patch no longer matches: {label}")
    return source.replace(old, new, 1)


def apply_sdk(directory):
    path = directory / "ExprFunc.cpp"
    source = path.read_text(encoding="utf-8")
    source = replace_once(source,
        "#ifndef SEEXPR_WIN32\n#include <dlfcn.h>\n#include <dirent.h>\n#endif",
        "#include <algorithm>\n#include <cctype>\n#include <filesystem>\n"
        "#include <mutex>\n#include <set>\n#include <sstream>\n"
        "#ifdef SEEXPR_WIN32\n#include <windows.h>\n"
        "#else\n#include <dlfcn.h>\n#endif",
        "platform module headers")
    source = replace_once(source, "FuncTable* Functions = 0;",
        "FuncTable* Functions = 0;\n"
        "std::recursive_mutex PluginMutex;\n"
        "std::set<std::filesystem::path> PluginPaths;",
        "explicit loader state")
    cleanup_begin = source.index("void ExprFunc::cleanup() {")
    cleanup_end = source.index("\nconst ExprFunc* ExprFunc::lookup", cleanup_begin)
    source = source[:cleanup_begin] + r'''void ExprFunc::cleanup() {
    // All Expressions using registered functions must be destroyed before
    // cleanup. Registry objects are released while module code is still live.
    std::lock_guard<std::recursive_mutex> pluginLock(PluginMutex);
    std::vector<void*> handles;
    {
        SeExprInternal2::AutoMutex locker(mutex);
        delete Functions;
        Functions = nullptr;
        handles.swap(dynlib);
        PluginPaths.clear();
    }
    for (void* handle : handles) {
#ifdef SEEXPR_WIN32
        FreeLibrary(reinterpret_cast<HMODULE>(handle));
#else
        dlclose(handle);
#endif
    }
}
''' + source[cleanup_end:]
    loaders_begin = source.index("\n#ifndef SEEXPR_WIN32\n\n#if defined(__APPLE__)")
    source = source[:loaders_begin] + r'''
namespace {
bool sdkPluginName(const std::filesystem::path& path) {
    std::string extension = path.extension().u8string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const std::string name = path.filename().u8string();
    if (name.compare(0, 6, "SeExpr") != 0) return false;
#ifdef SEEXPR_WIN32
    return extension == ".dll";
#elif defined(__APPLE__)
    return extension == ".so" || extension == ".dylib";
#else
    return extension == ".so";
#endif
}
}

void ExprFunc::loadPlugins(const char* paths) {
    if (!paths || !*paths) return;
#ifdef SEEXPR_WIN32
    const char separator = ';';
#else
    const char separator = ':';
#endif
    std::istringstream input(paths);
    std::string entry;
    while (std::getline(input, entry, separator)) {
        if (entry.empty()) continue;
        try {
            const auto path = std::filesystem::u8path(entry);
            std::error_code error;
            if (std::filesystem::is_regular_file(path, error)) {
                loadPlugin(path.u8string().c_str());
            } else if (std::filesystem::is_directory(path, error)) {
                std::vector<std::filesystem::path> candidates;
                for (std::filesystem::directory_iterator current(path, error), end;
                     !error && current != end; current.increment(error)) {
                    if (current->is_regular_file(error) && sdkPluginName(current->path()))
                        candidates.push_back(current->path());
                }
                std::sort(candidates.begin(), candidates.end());
                for (const auto& candidate : candidates)
                    loadPlugin(candidate.u8string().c_str());
                if (error) std::cerr << "SeExpr: plugin directory could not be read\n";
            } else {
                std::cerr << "SeExpr: explicit plugin location is unavailable\n";
            }
        } catch (const std::exception&) {
            std::cerr << "SeExpr: explicit plugin location is invalid\n";
        }
    }
}

void ExprFunc::loadPlugin(const char* name) {
    if (!name || !*name) return;
    std::lock_guard<std::recursive_mutex> pluginLock(PluginMutex);
    std::filesystem::path path;
    try {
        path = std::filesystem::canonical(std::filesystem::u8path(name));
    } catch (const std::exception&) {
        std::cerr << "SeExpr: explicit plugin file is unavailable\n";
        return;
    }
    if (PluginPaths.find(path) != PluginPaths.end()) return;
    for (const auto& loaded : PluginPaths) {
        std::error_code error;
        if (std::filesystem::equivalent(loaded, path, error) && !error) return;
    }
    init();
    typedef void (*initfn_v3)(ExprFunc::Define3);
#ifdef SEEXPR_WIN32
    HMODULE module = LoadLibraryExW(path.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    void* handle = reinterpret_cast<void*>(module);
    initfn_v3 initialize = module ?
        reinterpret_cast<initfn_v3>(GetProcAddress(module, "SeExpr2PluginInit")) : nullptr;
#else
    void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    initfn_v3 initialize = handle ?
        reinterpret_cast<initfn_v3>(dlsym(handle, "SeExpr2PluginInit")) : nullptr;
#endif
    if (!handle || !initialize) {
        std::cerr << "SeExpr: plugin is unavailable or lacks SeExpr2PluginInit\n";
        if (handle) {
#ifdef SEEXPR_WIN32
            FreeLibrary(reinterpret_cast<HMODULE>(handle));
#else
            dlclose(handle);
#endif
        }
        return;
    }
    // Keep code alive before publishing any callback. A trusted initializer
    // may throw after registering a function, so its handle remains pinned
    // until explicit registry cleanup even if initialization reports failure.
    dynlib.push_back(handle);
    PluginPaths.insert(path);
    try {
        initialize(static_cast<ExprFunc::Define3>(&ExprFunc::define));
    } catch (const std::exception&) {
        std::cerr << "SeExpr: plugin initialization failed\n";
    } catch (...) {
        std::cerr << "SeExpr: plugin initialization failed\n";
    }
}
}
'''
    path.write_text(source, encoding="utf-8", newline="\n")
