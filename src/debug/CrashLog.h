#ifndef _CRASH_LOG_H_
#define _CRASH_LOG_H_

// Debug aid (Windows): on a crash, prints the stack with function names and lines to stderr
// (needs the .pdb next to the exe: build with Compile.ps1 -Config RelWithDebInfo).
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <cstdio>
#pragma comment(lib, "dbghelp.lib")

inline LONG WINAPI CrashLogFilter(EXCEPTION_POINTERS* info)
{
    HANDLE process = GetCurrentProcess();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(process, nullptr, TRUE);
    std::fprintf(stderr, "CRASH: exception %08lX at %p\n", info->ExceptionRecord->ExceptionCode, info->ExceptionRecord->ExceptionAddress);
    if (info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && info->ExceptionRecord->NumberParameters >= 2)
        std::fprintf(stderr, "  access violation %s address %p\n", info->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
                     reinterpret_cast<void*>(info->ExceptionRecord->ExceptionInformation[1]));
    void* frames[48];
    const USHORT n = CaptureStackBackTrace(0, 48, frames, nullptr);
    // The frames of the handler itself come first; the crash site is the exception address.
    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256];
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(buffer);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    auto print = [&](void* addr) {
        DWORD64 disp = 0;
        DWORD line_disp = 0;
        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof(line);
        if (SymFromAddr(process, reinterpret_cast<DWORD64>(addr), &disp, sym))
        {
            if (SymGetLineFromAddr64(process, reinterpret_cast<DWORD64>(addr), &line_disp, &line))
                std::fprintf(stderr, "  %s  (%s:%lu)\n", sym->Name, line.FileName, line.LineNumber);
            else
                std::fprintf(stderr, "  %s +0x%llx\n", sym->Name, static_cast<unsigned long long>(disp));
        }
        else std::fprintf(stderr, "  %p\n", addr);
    };
    print(info->ExceptionRecord->ExceptionAddress);
    // Walk the stack from the faulting context.
    CONTEXT ctx = *info->ContextRecord;
    STACKFRAME64 frame{};
    frame.AddrPC.Offset = ctx.Rip; frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = ctx.Rbp; frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = ctx.Rsp; frame.AddrStack.Mode = AddrModeFlat;
    for (int i = 0; i < 24; ++i)
    {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, GetCurrentThread(), &frame, &ctx, nullptr, SymFunctionTableAccess64,
                         SymGetModuleBase64, nullptr))
            break;
        if (frame.AddrPC.Offset == 0) break;
        print(reinterpret_cast<void*>(frame.AddrPC.Offset));
    }
    (void)n; (void)frames;
    std::fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}

inline void InstallCrashLog() { SetUnhandledExceptionFilter(CrashLogFilter); }
#else
inline void InstallCrashLog() {}
#endif

#endif // _CRASH_LOG_H_
