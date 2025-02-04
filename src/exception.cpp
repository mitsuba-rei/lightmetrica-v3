/*
    Lightmetrica - Copyright (c) 2019 Hisanari Otsu
    Distributed under MIT license. See LICENSE file for details.
*/

#include <pch.h>
#include <lm/core.h>
#include <lm/exception.h>

#if LM_PLATFORM_WINDOWS
#include <Windows.h>
#include <eh.h>
#include <DbgHelp.h>
#pragma comment(lib, "Dbghelp.lib")
#endif

// ----------------------------------------------------------------------------

#define LM_EXCEPTION_ERROR_CODE(m, code) m[code] = #code

LM_NAMESPACE_BEGIN(LM_NAMESPACE::exception::detail)

class ExceptionContext_Default : public ExceptionContext {
private:
    int start_;   // Skips first n entries of the stack trace
    int stacks_;  // Number of entries of stack trace (0: disable)

public:
    ExceptionContext_Default() {
        #if LM_PLATFORM_WINDOWS
        // Handle structured exception as C++ exception
        _set_se_translator([](unsigned int code, PEXCEPTION_POINTERS data) {
            LM_UNUSED(data);

            // Map of the error code descriptions
            std::unordered_map<unsigned int, std::string> m;
            LM_EXCEPTION_ERROR_CODE(m, EXCEPTION_ACCESS_VIOLATION);
            LM_EXCEPTION_ERROR_CODE(m, EXCEPTION_DATATYPE_MISALIGNMENT);
            LM_EXCEPTION_ERROR_CODE(m, EXCEPTION_BREAKPOINT);
            LM_EXCEPTION_ERROR_CODE(m, EXCEPTION_SINGLE_STEP);
            LM_EXCEPTION_ERROR_CODE(m, EXCEPTION_ARRAY_BOUNDS_EXCEEDED);
            LM_EXCEPTION_ERROR_CODE(m, EXCEPTION_FLT_DENORMAL_OPERAND);
            LM_EXCEPTION_ERROR_CODE(m, EXCEPTION_FLT_DIVIDE_BY_ZERO);
            LM_EXCEPTION_ERROR_CODE(m, EXCEPTION_FLT_INEXACT_RESULT);
            LM_EXCEPTION_ERROR_CODE(m, EXCEPTION_FLT_INVALID_OPERATION);
            LM_EXCEPTION_ERROR_CODE(m, EXCEPTION_FLT_OVERFLOW);
            LM_EXCEPTION_ERROR_CODE(m, EXCEPTION_FLT_STACK_CHECK);
            LM_EXCEPTION_ERROR_CODE(m, EXCEPTION_FLT_UNDERFLOW);
            LM_EXCEPTION_ERROR_CODE(m, EXCEPTION_INT_DIVIDE_BY_ZERO);
            LM_EXCEPTION_ERROR_CODE(m, EXCEPTION_INT_OVERFLOW);
            LM_EXCEPTION_ERROR_CODE(m, EXCEPTION_PRIV_INSTRUCTION);
            LM_EXCEPTION_ERROR_CODE(m, EXCEPTION_IN_PAGE_ERROR);
            LM_EXCEPTION_ERROR_CODE(m, EXCEPTION_ILLEGAL_INSTRUCTION);
            LM_EXCEPTION_ERROR_CODE(m, EXCEPTION_NONCONTINUABLE_EXCEPTION);
            LM_EXCEPTION_ERROR_CODE(m, EXCEPTION_STACK_OVERFLOW);
            LM_EXCEPTION_ERROR_CODE(m, EXCEPTION_INVALID_DISPOSITION);
            LM_EXCEPTION_ERROR_CODE(m, EXCEPTION_GUARD_PAGE);
            LM_EXCEPTION_ERROR_CODE(m, EXCEPTION_INVALID_HANDLE);

            // Print error message
            const std::string desc = m[code];
            LM_ERROR("Structured exception [desc='{}']", desc);
            exception::stackTrace();

            throw std::runtime_error(m[code]);
        });

        // Handle denormals as zero
        _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);

        // Enable floating point exceptions
        enableFPEx();
        #endif
    }

    ~ExceptionContext_Default() {
        #if LM_PLATFORM_WINDOWS
        disableFPEx();
        _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_OFF);
        _set_se_translator(nullptr);
        #endif
    }

private:
    #if LM_PLATFORM_WINDOWS
    unsigned int setFPExState(unsigned int state) const {
        // Get current floating-point control word
        unsigned int old;
        _controlfp_s(&old, 0, 0);

        // Set a new control word
        unsigned int current;
        _controlfp_s(&current, state, _MCW_EM);
        LM_UNUSED(current);

        return old;
    }
    #endif

public:
    virtual bool construct(const Json& prop) override {
        start_  = json::value(prop, "start", 3);
        stacks_ = json::value(prop, "stacks", 0);
        return true;
    }

    virtual void enableFPEx() override {
        #if LM_PLATFORM_WINDOWS
        setFPExState((unsigned int)(~(_EM_INVALID | _EM_ZERODIVIDE)));
        #endif
    }

    virtual void disableFPEx() override {
        #if LM_PLATFORM_WINDOWS
        setFPExState(_CW_DEFAULT);
        #endif
    }

    virtual void stackTrace() override {
        if (stacks_ == 0) {
            return;
        }

        #if LM_PLATFORM_WINDOWS
        LM_ERROR("Stack trace");
        LM_INDENT();

        // Get necessary function
        using CaptureStackBackTraceFunc= USHORT(WINAPI*)(__in ULONG, __in ULONG, __out PVOID*, __out_opt PULONG);
        auto RtlCaptureStackBackTrace = CaptureStackBackTraceFunc(
            GetProcAddress(LoadLibrary("kernel32.dll"), "RtlCaptureStackBackTrace"));
    
        // Prepare for capturing symbols
        auto process = GetCurrentProcess();
        SymInitialize(process, NULL, TRUE);

        // Allocate symbol information
        SYMBOL_INFO* symbol;
        symbol = (SYMBOL_INFO *)calloc(sizeof(SYMBOL_INFO) + 256 * sizeof(char), 1);
        symbol->MaxNameLen = 255;
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);

        // Print captured stack frame
        constexpr int MaxCallers = 62;
        void* callersStack[MaxCallers];
        const int frames = RtlCaptureStackBackTrace(0, MaxCallers, callersStack, NULL);
        for (int i = start_; i < std::min(frames, start_ + stacks_); i++) {
            SymFromAddr(process, (DWORD64)(callersStack[i]), 0, symbol);
            LM_ERROR("{}: {}", i - start_, symbol->Name);
        }
        free(symbol);
        #endif
    }
};

LM_COMP_REG_IMPL(ExceptionContext_Default, "exception::default");

LM_NAMESPACE_END(LM_NAMESPACE::exception::detail)

// ----------------------------------------------------------------------------

LM_NAMESPACE_BEGIN(LM_NAMESPACE::exception)

using Instance = comp::detail::ContextInstance<detail::ExceptionContext>;

LM_PUBLIC_API void init(const std::string& type, const Json& prop) {
    Instance::init(type, prop);
}

LM_PUBLIC_API void shutdown() {
    Instance::shutdown();
}

LM_PUBLIC_API void enableFPEx() {
    Instance::get().enableFPEx();
}

LM_PUBLIC_API void disableFPEx() {
    Instance::get().disableFPEx();
}

LM_PUBLIC_API void stackTrace() {
    Instance::get().stackTrace();
}

LM_NAMESPACE_END(LM_NAMESPACE::exception)
