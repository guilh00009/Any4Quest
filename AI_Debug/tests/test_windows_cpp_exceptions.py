# SPDX-FileCopyrightText: Copyright 2026 Any4Quest contributors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Native Windows regression: exercise the actual production vectored handler.

Run from an existing Visual Studio x64 developer environment, passing --source
shadps4-arm64-main/src/core/signals.cpp and --compiler /path/to/clang++.exe.
The extracted production handler uses stub dispatch/shutdown endpoints so fatal
negative controls can be checked without terminating the test process.
"""
import argparse, pathlib, re, subprocess, tempfile
parser = argparse.ArgumentParser()
parser.add_argument('--source', type=pathlib.Path, required=True)
parser.add_argument('--compiler', required=True)
args = parser.parse_args()
source = args.source.read_text()
start = source.index('static LONG WINAPI SignalHandler(')
end = source.index('\n#else', start)
handler = source[start:end]
constants = '\n'.join(re.findall(r'static constexpr DWORD MS_\w+ = [^;]+;', source))
preamble = r"""
#include <windows.h>
#include <cstring>
#include <cstdint>
#include <stdexcept>
#include <cstdio>
#include <cstdlib>
#define LOG_DEBUG(...) ((void)0)
#define LOG_CRITICAL(...) ((void)0)
static int shutdowns = 0;
namespace Common { template <typename T> struct Singleton { static T* Instance() {static T t; return &t;} }; }
namespace Core {
struct Signals {
 static const Signals* Instance() {static Signals s; return &s;}
 static inline bool handle = false;
 bool DispatchAccessViolation(EXCEPTION_POINTERS*,void*) const {return handle;}
 bool DispatchIllegalInstruction(EXCEPTION_POINTERS*) const {return handle;}
};
struct Emulator {void Shutdown() {++shutdowns;}};
}
"""
main = r"""
}
static void check(bool ok, const char* message) {if(!ok){std::fprintf(stderr,"FAIL: %s (shutdowns=%d)\n",message,shutdowns);std::exit(1);}}
int main() {
 auto registration=AddVectoredExceptionHandler(0,Core::SignalHandler);
 check(registration!=nullptr,"register native VEH");
 int caught=0, unwound=0;
 struct Guard {int& n; ~Guard(){++n;}};
 try {Guard g{unwound}; throw std::runtime_error("runtime-owned recoverable exception");}
 catch(const std::runtime_error&) {++caught;}
 try {try {Guard g{unwound}; throw 42;} catch(int) {throw;}}
 catch(int value) {check(value==42,"rethrow value");++caught;}
 check(RemoveVectoredExceptionHandler(registration)!=0,"remove native VEH");
 check(caught==2 && unwound==2,"C++ catches and destructors execute");
 check(shutdowns==0,"caught first-chance C++ exceptions must not shut down emulator");
 EXCEPTION_RECORD record{}; CONTEXT context{}; EXCEPTION_POINTERS pointers{&record,&context};
 record.ExceptionCode=EXCEPTION_ACCESS_VIOLATION;
 Core::Signals::handle=true;
 check(Core::SignalHandler(&pointers)==EXCEPTION_CONTINUE_EXECUTION && shutdowns==0,"handled access violation preserved");
 record.ExceptionCode=EXCEPTION_ILLEGAL_INSTRUCTION;
 check(Core::SignalHandler(&pointers)==EXCEPTION_CONTINUE_EXECUTION && shutdowns==0,"handled illegal instruction preserved");
 Core::Signals::handle=false;
 check(Core::SignalHandler(&pointers)==EXCEPTION_CONTINUE_SEARCH && shutdowns==1,"fatal illegal instruction preserved");
 record.ExceptionCode=EXCEPTION_ACCESS_VIOLATION;
 check(Core::SignalHandler(&pointers)==EXCEPTION_CONTINUE_SEARCH && shutdowns==2,"fatal access violation preserved");
 record.ExceptionCode=EXCEPTION_BREAKPOINT;
 check(Core::SignalHandler(&pointers)==EXCEPTION_CONTINUE_SEARCH && shutdowns==2,"breakpoint preserved");
 puts("PASS: native C++ throw/catch/rethrow/unwind and fatal/handled exception controls");
}
"""
with tempfile.TemporaryDirectory(prefix='any4quest-veh-') as temp:
    cpp=pathlib.Path(temp)/'test.cpp'; exe=pathlib.Path(temp)/'test.exe'
    cpp.write_text(preamble+constants+'\nnamespace Core {\n'+handler+main)
    subprocess.run([args.compiler,'-std=c++20','-fexceptions','-fcxx-exceptions',str(cpp),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
