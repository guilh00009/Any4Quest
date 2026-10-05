// SPDX-FileCopyrightText: Copyright 2026 Any4Quest contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/vr/move_capture.h"
#include <cassert>
#include <iostream>
#include <filesystem>
#include <sstream>
using namespace Core::Vr::Diagnostics;
static size_t rows(const std::string& path) {
 std::ifstream in(path); std::string line; size_t n=0;
 while(std::getline(in,line)) ++n;
 return n;
}
struct CommaDecimal : std::numpunct<char> { char do_decimal_point() const override {return ',';} };
int main() {
 const auto previous_locale=std::locale();
 std::locale::global(std::locale(previous_locale,new CommaDecimal));
 const auto dir=std::filesystem::temp_directory_path()/
     ("any4quest-capture-test-"+std::to_string(Capture::Clock::now().time_since_epoch().count()));
 std::filesystem::create_directories(dir);
 Capture c; Record r{};r.kind=4;r.sequence=9;r.values[0]=1.25f;
 const auto now=Capture::Clock::now();
 const auto bounded=(dir/"bounded.csv").string();
 assert(c.Start(bounded,now));assert(!c.Start(bounded,now));
 c.Push(r,now);c.Push(r,now+std::chrono::seconds{119});
 c.Push(r,now+std::chrono::seconds{120});assert(!c.Active());c.Wait();
 assert(c.WriteSucceeded());assert(rows(bounded)==4);
 { std::ifstream in(bounded);std::string content((std::istreambuf_iterator<char>(in)),{});
   assert(content.find(",1.25,")!=std::string::npos); }
 std::locale::global(previous_locale);
 const auto full=(dir/"full.csv").string();assert(c.Start(full));
 const auto began=Capture::Clock::now();
 std::array<std::thread,4> threads;
 for(unsigned t=0;t<4;++t) threads[t]=std::thread([&,t]{
   for(unsigned i=0;i<15000;++i){Record x{};x.kind=2;x.hand=t;x.sequence=i;c.Push(x);}
 });
 for(auto& thread:threads)thread.join();
 const auto elapsed=std::chrono::duration<double,std::micro>(Capture::Clock::now()-began).count();
 c.Push(r);assert(!c.Active());c.Wait();assert(c.WriteSucceeded());
 assert(rows(full)==Capture::Capacity+2);
 assert(c.Start((dir/"manual.csv").string()));c.Push(r);c.Stop();c.Stop();c.Wait();
 assert(rows((dir/"manual.csv").string())==3);
 assert(c.Start((dir/"missing"/"fail.csv").string()));c.Push(r);c.Stop();c.Wait();assert(!c.WriteSucceeded());
 std::cout << "PASS deadline, hard cap, concurrent writers, manual marker save, restart, write failure\n"
           << "Record bytes=" << sizeof(Record) << " storage cap=" << sizeof(Record)*Capture::Capacity
           << " measured 4-writer insertion us/record=" << elapsed/Capture::Capacity << '\n';
 // Test-owned, uniquely-created temporary files only.
 std::filesystem::remove_all(dir);
}
