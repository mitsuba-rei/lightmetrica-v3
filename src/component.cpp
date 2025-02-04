/*
    Lightmetrica - Copyright (c) 2019 Hisanari Otsu
    Distributed under MIT license. See LICENSE file for details.
*/

#include <pch.h>
#include <lm/component.h>
#include <lm/logger.h>

#if LM_PLATFORM_WINDOWS
#include <Windows.h>
#elif LM_PLATFORM_LINUX || LM_PLATFORM_APPLE
#include <dlfcn.h>
#endif

LM_NAMESPACE_BEGIN(LM_NAMESPACE::comp::detail)

// ----------------------------------------------------------------------------

// Platform-independent abstruction of shared library.
class SharedLibrary {
public:
    // Load library.
    bool load(const std::string& path) {
        #if LM_PLATFORM_WINDOWS
        const auto p = path + ".dll";
        #elif LM_PLATFORM_LINUX
        const auto p = path + ".so";
        #elif LM_PLATFORM_APPLE
        const auto p = path + ".dylib";
        #endif

        #if LM_PLATFORM_WINDOWS
        handle = LoadLibraryA(p.c_str());
        if (!handle) {
            LM_ERROR("Failed to load library or its dependencies [path='{}']", p);
            LM_INDENT();
            LM_ERROR(getLastErrorAsString());
            return false;
        }
        #elif LM_PLATFORM_LINUX || LM_PLATFORM_APPLE
        handle = dlopen(p.c_str(), RTLD_LAZY | RTLD_LOCAL);
        if (!handle) {
            LM_ERROR("Failed to load library or its dependencies [path='{}']", p);
            LM_INDENT();
            LM_ERROR(dlerror());
            return false;
        }
        #endif

        return true;
    }

    // Unload library.
    bool unload() {
        #if LM_PLATFORM_WINDOWS
        if (!FreeLibrary(handle)) {
            LM_ERROR("Failed to free library");
            LM_INDENT();
            LM_ERROR(getLastErrorAsString());
            return false;
        }
        #elif LM_PLATFORM_LINUX || LM_PLATFORM_APPLE
        if (dlclose(handle) != 0) {
            LM_ERROR("Failed to free library");
            LM_INDENT();
            LM_ERROR(dlerror());
            return false;
        }
        #endif

        return true;
    }

    // Retrieve an address of an exported symbol.
    void* getFuncPointer(const std::string& symbol) const {
        #if LM_PLATFORM_WINDOWS
        void* address = (void*)GetProcAddress(handle, symbol.c_str());
        if (address == nullptr) {
            LM_ERROR("Failed to get address of '{}'", symbol);
            LM_INDENT();
            LM_ERROR(getLastErrorAsString());
            return nullptr;
        }
        #elif LM_PLATFORM_LINUX || LM_PLATFORM_APPLE
        void* address = dlsym(handle, symbol.c_str());
        if (address == nullptr) {
            LM_ERROR("Failed to get address of '{}'", symbol);
            LM_INDENT();
            LM_ERROR(dlerror());
            return nullptr;
        }
        #endif

        return address;
    }

private:

    #if LM_PLATFORM_WINDOWS
    auto getLastErrorAsString() const -> std::string {
        DWORD error = GetLastError();
        if (error == 0) {
            return std::string();
        }

        LPSTR buffer = nullptr;
        size_t size = FormatMessageA(
            FORMAT_MESSAGE_ALLOCATE_BUFFER |
            FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
            NULL, error, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
            (LPSTR)&buffer, 0, NULL);
        std::string message(buffer, size);
        LocalFree(buffer);

        // Message obtained by FormatMessage contains newline. Remove it
        return std::regex_replace(message, std::regex("\r\n"), " ");
    }
    #endif

private:
    
    #if LM_PLATFORM_WINDOWS
    HMODULE handle;
    #elif LM_PLATFORM_LINUX || LM_PLATFORM_APPLE
    void* handle;
    #endif

};

// ----------------------------------------------------------------------------

class ComponentContext {
public:
    static ComponentContext& instance() {
        static ComponentContext instance;
        return instance;
    }

public:
    Component* createComp(const std::string& key) {
        auto it = funcMap_.find(key);
        if (it == funcMap_.end()) {
            LM_ERROR("Missing component [key='{}']. Check if", key);
            LM_ERROR("- Key is wrong");
            LM_ERROR("- Component with the key is not registered");
            LM_ERROR("- Plugin containing the component is not loaded");
            return nullptr;
        }
        auto* p = it->second.createFunc();
        Access::key(p) = key;
        Access::createFunc(p) = it->second.createFunc;
        Access::releaseFunc(p) = it->second.releaseFunc;
        return p;
    }

    void reg(
        const std::string& key,
        const Component::CreateFunction& createFunc,
        const Component::ReleaseFunction& releaseFunc) {
        if (funcMap_.find(key) != funcMap_.end()) {
            LM_WARN("Component is already registered [key='{}'], overriding", key);
        }
        funcMap_[key] = CreateAndReleaseFunctions{ createFunc, releaseFunc };
    }

    void unreg(const std::string& key) {
        funcMap_.erase(key);
    }

    bool loadPlugin(const std::string& p) {
        #if LM_DEBUG_MODE
        fs::path path(p + "-debug");
        #else
        fs::path path(p);
        #endif

        LM_INFO("Loading plugin [name='{}']", path.filename().string());
        LM_INDENT();

        // Load plugin
        std::unique_ptr<SharedLibrary> plugin(new SharedLibrary);
        #if LM_PLATFORM_WINDOWS
        const auto parent = path.parent_path().string();
        SetDllDirectory(parent.c_str());
        #endif
        if (!plugin->load(path.string())) {
            LM_WARN("Failed to load library [path='{}']", path.string());
            return false;
        }
        #if LM_PLATFORM_WINDOWS
        SetDllDirectory(nullptr);
        #endif

        plugins_.push_back(std::move(plugin));
        LM_INFO("Successfully loaded");
        return true;
    }

    void loadPluginDirectory(const std::string& directory) {
        // Skip if directory does not exist
        if (!fs::is_directory(fs::path(directory))) {
            LM_WARN("Missing plugin directory [directory='{}']. Skipping.", directory);
            return;
        }

        // File format
        #if LM_PLATFORM_WINDOWS
        const std::regex pluginNameExp("([0-9a-z_]+)\\.dll$");
        #elif LM_PLATFORM_LINUX
        const std::regex pluginNameExp("^([0-9a-z_]+)\\.so$");
        #elif LM_PLATFORM_APPLE
        const std::regex pluginNameExp("^([0-9a-z_]+)\\.dylib$");
        #endif

        // Enumerate dynamic libraries in #pluginDir
        fs::directory_iterator endIter;
        for (fs::directory_iterator it(directory); it != endIter; ++it) {
            if (!fs::is_regular_file(it->status())) {
                continue;
            }
            std::cmatch match;
            auto filename = it->path().filename().string();
            if (!std::regex_match(filename.c_str(), match, pluginNameExp)) {
                continue;
            }
            if (!loadPlugin(it->path().stem().string().c_str())) {
                continue;
            }
        }
    }

    void unloadPlugins() {
        for (auto& plugin : plugins_) { plugin->unload(); }
        plugins_.clear();
    }

    void foreachRegistered(const std::function<void(const std::string& name)>& func) {
        for (const auto& [k, v] : funcMap_) {
            func(k);
        }
    }

    void registerRootComp(Component* p) {
        root_ = p;
    }

    Component* get(const std::string& locator) {
        if (!root_) {
            LM_ERROR("Root component has not registered [name='{}'].", locator);
            return nullptr;
        }

        if (locator.empty()) {
            LM_ERROR("Locator is empty [loc='{}']", locator);
            return nullptr;
        }

        // Given 'xxx.yyy.zzz', returns the pair of 'xxx' and 'yyy.zzz'.
        const auto splitFirst = [&](const std::string& s) -> std::tuple<std::string, std::string> {
            const auto i = s.find_first_of('.', 0);
            if (i == std::string::npos) {
                return { s, "" };
            }
            return { s.substr(0, i), s.substr(i + 1) };
        };

        // Trace down from the root
        const auto [s0, r0] = splitFirst(locator);
        if (s0 != "$") {
            LM_ERROR("Locator must start with '$' [loc='{}'].", locator);
            return nullptr;
        }
        auto remaining = r0;
        auto* curr = root_;
        while (curr && !remaining.empty()) {
            const auto [s, r] = splitFirst(remaining);
            curr = curr->underlying(s);
            remaining = r;
        }

        if (!curr) {
            LM_ERROR("Failed to find a component with locator [loc='{}']", locator);
        }

        return curr;
    }

private:
    // Registered implementations
    struct CreateAndReleaseFunctions
    {
        Component::CreateFunction createFunc;
        Component::ReleaseFunction releaseFunc;
    };
    std::unordered_map<std::string, CreateAndReleaseFunctions> funcMap_;

    // Loaded plugins
    std::vector<std::unique_ptr<SharedLibrary>> plugins_;

    // Root component
    Component* root_ = nullptr;
};

// ----------------------------------------------------------------------------

LM_PUBLIC_API Component* createComp(const std::string& key) {
    return ComponentContext::instance().createComp(key);
}

LM_PUBLIC_API void reg(
    const std::string& key,
    const Component::CreateFunction& createFunc,
    const Component::ReleaseFunction& releaseFunc) {
    ComponentContext::instance().reg(key, createFunc, releaseFunc);
}

LM_PUBLIC_API void unreg(const std::string& key) {
    ComponentContext::instance().unreg(key);
}

LM_PUBLIC_API bool loadPlugin(const std::string& path) {
    return ComponentContext::instance().loadPlugin(path);
}

LM_PUBLIC_API void loadPluginDirectory(const std::string& directory) {
    ComponentContext::instance().loadPluginDirectory(directory);
}

LM_PUBLIC_API void unloadPlugins() {
    ComponentContext::instance().unloadPlugins();
}

LM_PUBLIC_API void foreachRegistered(const std::function<void(const std::string& name)>& func) {
    ComponentContext::instance().foreachRegistered(func);
}

LM_PUBLIC_API void registerRootComp(Component* p) {
    ComponentContext::instance().registerRootComp(p);
}

LM_PUBLIC_API Component* get(const std::string& locator) {
    return ComponentContext::instance().get(locator);
}

// ----------------------------------------------------------------------------

LM_NAMESPACE_END(LM_NAMESPACE::comp::detail)
