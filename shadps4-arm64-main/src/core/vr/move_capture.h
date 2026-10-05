// SPDX-FileCopyrightText: Copyright 2026 Any4Quest contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <locale>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace Core::Vr::Diagnostics {
// Numeric records avoid formatting and file I/O on the tracking/render threads.
// Field layout is documented in MOVE-CAPTURE.txt alongside the launcher.
struct Record {
    unsigned kind{}, hand{}, flags{}, generation{};
    unsigned long long receipt_us{}, sequence{}, host_sequence{}, requested_us{}, returned_us{};
    long long xr_pose_ns{}, xr_display_ns{}, pose_process_us{};
    std::array<float, 48> values{};
};
inline std::atomic<unsigned> seat_generation{};
inline thread_local unsigned long long host_sequence{};
class Capture {
public:
    static constexpr size_t Capacity = 60000;
    static constexpr auto Duration = std::chrono::seconds{120};
    using Clock = std::chrono::steady_clock;
    static Capture& Instance() { static Capture capture; return capture; }
    static bool Enabled() {
        static const bool enabled = [] {
#ifdef _WIN32
            char* value = nullptr; size_t length = 0;
            if (_dupenv_s(&value, &length, "SHADPS4_MOVE_CAPTURE") != 0) return false;
            const bool on = value && std::string_view(value) == "1";
            std::free(value); return on;
#else
            const char* value = std::getenv("SHADPS4_MOVE_CAPTURE");
            return value && std::string_view(value) == "1";
#endif
        }();
        return enabled;
    }
    bool Exporting() const { return exporting.load(std::memory_order_acquire); }
    bool Active() const { return active.load(std::memory_order_relaxed); }
    bool Start(const std::string& filename, Clock::time_point now = Clock::now()) {
        std::scoped_lock lock{mutex};
        if (active || exporting) return false;
        if (writer.joinable()) writer.join();
        records.clear(); records.reserve(Capacity);
        path = filename; deadline = now + Duration; dropped = 0; write_ok = false;
        active.store(true, std::memory_order_release);
        return true;
    }
    void Push(const Record& record, Clock::time_point now = Clock::now()) {
        if (!Active()) return;
        bool finish = false;
        {
            std::scoped_lock lock{mutex};
            if (!active) return;
            if (now >= deadline) finish = true;
            else if (records.size() < Capacity) records.push_back(record);
            else { ++dropped; finish = true; }
        }
        if (finish) Stop();
    }
    void Tick(Clock::time_point now = Clock::now()) {
        if (!Active()) return;
        bool finish;
        { std::scoped_lock lock{mutex}; finish = active && now >= deadline; }
        if (finish) Stop();
    }
    void Stop() {
        std::scoped_lock lock{mutex};
        if (!active.exchange(false)) return;
        exporting = true;
        auto data = std::move(records);
        auto filename = path;
        const auto lost = dropped;
        writer = std::thread([this, data = std::move(data), filename, lost] {
            std::ofstream out(filename, std::ios::out | std::ios::trunc);
            out.imbue(std::locale::classic());
            if (out) {
                out << "# schema=1 capacity=" << Capacity << " dropped=" << lost << '\n';
                out << "kind,hand,flags,generation,receipt_us,sequence,host_sequence,requested_us,returned_us,xr_pose_ns,xr_display_ns,pose_process_us";
                for (int i=0;i<48;++i) out << ",v" << i;
                out << '\n' << std::setprecision(9);
                for (const auto& r : data) {
                    out << r.kind << ',' << r.hand << ',' << r.flags << ',' << r.generation << ','
                        << r.receipt_us << ',' << r.sequence << ',' << r.host_sequence << ',' << r.requested_us << ','
                        << r.returned_us << ',' << r.xr_pose_ns << ',' << r.xr_display_ns << ',' << r.pose_process_us;
                    for (auto v : r.values) out << ',' << v;
                    out << '\n';
                }
            }
            out.flush();
            write_ok.store(bool(out), std::memory_order_release);
            exporting.store(false, std::memory_order_release);
        });
    }
    void Wait() { if (writer.joinable()) writer.join(); }
    bool WriteSucceeded() const { return write_ok.load(std::memory_order_acquire); }
    ~Capture() { Stop(); Wait(); }
private:
    std::mutex mutex;
    std::atomic<bool> active{}, exporting{}, write_ok{};
    Clock::time_point deadline;
    std::vector<Record> records;
    std::string path;
    size_t dropped{};
    std::thread writer;
};
template <typename Pose> void PutPose(Record& r, unsigned index, const Pose& p) {
    r.values[index]=p.position.x; r.values[index+1]=p.position.y; r.values[index+2]=p.position.z;
    r.values[index+3]=p.orientation.x; r.values[index+4]=p.orientation.y;
    r.values[index+5]=p.orientation.z; r.values[index+6]=p.orientation.w;
}
template <typename V> void PutVector(Record& r, unsigned index, const V& v) {
    r.values[index]=v.x; r.values[index+1]=v.y; r.values[index+2]=v.z;
}
} // namespace Core::Vr::Diagnostics
