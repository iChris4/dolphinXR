// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/CrashReport.h"

#ifdef _WIN32

#include <cstdio>
#include <string>

#include <windows.h>

#include <dbghelp.h>

#include "Common/StringUtil.h"

namespace Common
{
namespace
{
// Built when installed: the filter runs in a crashed process, so it allocates as little as it can.
std::wstring s_directory;
LPTOP_LEVEL_EXCEPTION_FILTER s_previous_filter = nullptr;
volatile LONG s_reporting = 0;

constexpr int MAX_FRAMES = 64;

void WriteMinidump(const std::wstring& path, EXCEPTION_POINTERS* info)
{
  const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    return;
  MINIDUMP_EXCEPTION_INFORMATION exception{GetCurrentThreadId(), info, FALSE};
  MiniDumpWriteDump(
      GetCurrentProcess(), GetCurrentProcessId(), file,
      static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules), &exception,
      nullptr, nullptr);
  CloseHandle(file);
}

// One line per address: module+offset, then the function and the source line when the PDB has them.
void WriteAddress(FILE* file, HANDLE process, DWORD64 address)
{
  char module_path[MAX_PATH] = "?";
  const DWORD64 module_base = SymGetModuleBase64(process, address);
  if (module_base != 0)
  {
    GetModuleFileNameA(reinterpret_cast<HMODULE>(module_base), module_path, MAX_PATH);
  }
  const char* module_name = module_path;
  for (const char* c = module_path; *c != '\0'; ++c)
  {
    if (*c == '\\' || *c == '/')
      module_name = c + 1;
  }
  std::fprintf(file, "  %s+0x%llx", module_name,
               static_cast<unsigned long long>(address - module_base));

  alignas(SYMBOL_INFO) char symbol_buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] = {};
  SYMBOL_INFO* symbol = reinterpret_cast<SYMBOL_INFO*>(symbol_buffer);
  symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
  symbol->MaxNameLen = MAX_SYM_NAME;
  DWORD64 displacement = 0;
  if (SymFromAddr(process, address, &displacement, symbol))
  {
    std::fprintf(file, "  %s+0x%llx", symbol->Name, static_cast<unsigned long long>(displacement));
  }
  IMAGEHLP_LINE64 line{};
  line.SizeOfStruct = sizeof(line);
  DWORD line_displacement = 0;
  if (SymGetLineFromAddr64(process, address, &line_displacement, &line))
    std::fprintf(file, "  (%s:%lu)", line.FileName, line.LineNumber);
  std::fprintf(file, "\n");
}

void WriteStack(FILE* file, EXCEPTION_POINTERS* info)
{
  const HANDLE process = GetCurrentProcess();
  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
  // The PDB is found from the path the executable records, or next to it.
  if (!SymInitialize(process, nullptr, TRUE))
  {
    std::fprintf(file, "No symbols (SymInitialize failed: %lu).\n", GetLastError());
    return;
  }

  CONTEXT context = *info->ContextRecord;
  STACKFRAME64 frame{};
#if defined(_M_X86_64)
  constexpr DWORD MACHINE = IMAGE_FILE_MACHINE_AMD64;
  frame.AddrPC.Offset = context.Rip;
  frame.AddrFrame.Offset = context.Rbp;
  frame.AddrStack.Offset = context.Rsp;
#elif defined(_M_ARM_64)
  constexpr DWORD MACHINE = IMAGE_FILE_MACHINE_ARM64;
  frame.AddrPC.Offset = context.Pc;
  frame.AddrFrame.Offset = context.Fp;
  frame.AddrStack.Offset = context.Sp;
#endif
  frame.AddrPC.Mode = AddrModeFlat;
  frame.AddrFrame.Mode = AddrModeFlat;
  frame.AddrStack.Mode = AddrModeFlat;

  std::fprintf(file, "\nStack of the crashing thread:\n");
  for (int i = 0; i < MAX_FRAMES; ++i)
  {
    if (!StackWalk64(MACHINE, process, GetCurrentThread(), &frame, &context, nullptr,
                     SymFunctionTableAccess64, SymGetModuleBase64, nullptr) ||
        frame.AddrPC.Offset == 0)
    {
      break;
    }
    WriteAddress(file, process, frame.AddrPC.Offset);
  }
  SymCleanup(process);
}

LONG WINAPI ReportCrash(EXCEPTION_POINTERS* info)
{
  // One report: a second crash while writing it (or on another thread) goes straight on.
  if (InterlockedExchange(&s_reporting, 1) == 0)
  {
    SYSTEMTIME time;
    GetLocalTime(&time);
    wchar_t stamp[32];
    swprintf_s(stamp, L"%04u%02u%02u-%02u%02u%02u", time.wYear, time.wMonth, time.wDay, time.wHour,
               time.wMinute, time.wSecond);
    const std::wstring base = s_directory + L"crash-" + stamp;

    FILE* file = nullptr;
    if (_wfopen_s(&file, (base + L".txt").c_str(), L"w") == 0 && file != nullptr)
    {
      const EXCEPTION_RECORD& record = *info->ExceptionRecord;
      std::fprintf(file, "Dolphin crashed: exception 0x%08lx at 0x%p on thread %lu.\n",
                   record.ExceptionCode, record.ExceptionAddress, GetCurrentThreadId());
      if (record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record.NumberParameters >= 2)
      {
        std::fprintf(file, "It %s 0x%llx.\n",
                     record.ExceptionInformation[0] == 0 ? "read" :
                     record.ExceptionInformation[0] == 1 ? "wrote" :
                                                           "executed",
                     static_cast<unsigned long long>(record.ExceptionInformation[1]));
      }
      // Written first: a minidump of a crashed process can fail, the stack is the useful part.
      std::fflush(file);
      WriteStack(file, info);
      std::fclose(file);
    }
    WriteMinidump(base + L".dmp", info);
  }
  return s_previous_filter ? s_previous_filter(info) : EXCEPTION_CONTINUE_SEARCH;
}
}  // namespace

void InstallCrashReporter(const std::string& directory)
{
  s_directory = UTF8ToWString(directory);
  if (!s_directory.empty() && s_directory.back() != L'/' && s_directory.back() != L'\\')
    s_directory += L'/';
  s_previous_filter = SetUnhandledExceptionFilter(ReportCrash);
}
}  // namespace Common

#else

namespace Common
{
void InstallCrashReporter(const std::string&)
{
}
}  // namespace Common

#endif
