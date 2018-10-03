/*
    Lightmetrica - Copyright (c) 2018 Hisanari Otsu
    Distributed under MIT license. See LICENSE file for details.
*/

#include <lm/logger.h>
#include <spdlog/async.h>
#include <spdlog/sinks/stdout_color_sinks.h>

LM_NAMESPACE_BEGIN(LM_NAMESPACE::log::detail)

// ----------------------------------------------------------------------------

// Logger implementation
// - Ignores message when the logger is not initialized.
class Impl {
public:

    static Impl& instance() {
        static Impl instance;
        return instance;
    }

public:

    void init() {
        if (stdoutLogger_) { shutdown(); }
        stdoutLogger_ = spdlog::stdout_color_mt("lm_stdout");
    }

    void shutdown() {
        stdoutLogger_ = nullptr;
        spdlog::shutdown();
    }

    void log(LogLevel level, const char* filename, int line, const char* message) {
        if (!stdoutLogger_) { return; }
        stdoutLogger_->log(spdlog::level::level_enum(level), indentationString_ + message);
    }

    void updateIndentation(int n) {
        indentation_ += n;
        if (indentation_ > 0) {
            indentationString_ = std::string(2 * indentation_, '.') + " ";
        }
        else {
            indentation_ = 0;
            indentationString_ = "";
        }
    }

private:

    int indentation_ = 0;
    std::string indentationString_;
    std::shared_ptr<spdlog::logger> stdoutLogger_;

};

// ----------------------------------------------------------------------------

LM_PUBLIC_API void init() {
    Impl::instance().init();
}

LM_PUBLIC_API void shutdown() {
    Impl::instance().shutdown();
}

LM_PUBLIC_API void log(LogLevel level, const char* filename, int line, const char* message) {
    Impl::instance().log(level, filename, line, message);
}

LM_PUBLIC_API void updateIndentation(int n) {
    Impl::instance().updateIndentation(n);
}

// ----------------------------------------------------------------------------

LM_NAMESPACE_END(LM_NAMESPACE::log::detail)
