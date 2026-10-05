// SPDX-License-Identifier: GPL-2.0-or-later
#include <atomic>
#include <array>
#include <cassert>
#include <cstdint>
#include <deque>
#include <iostream>
#include <memory>
#include <optional>
#include <utility>
#include <vector>
#include "core/libraries/avplayer/avplayer_buffering.h"
using namespace Libraries::AvPlayer;
#define LOG_INFO(...) ((void)0)
static bool ShouldTraceCount(u64) { return false; }
struct AvPlayerFrameInfoEx {
    void* p_data{}; u64 timestamp{};
    struct { struct { float aspect_ratio{}; u32 width{},height{}; } video; } details;
};
struct AvPlayerFrameInfo {
    u8* p_data{}; u64 timestamp{};
    struct { struct { float aspect_ratio{}; u32 width{},height{}; } video; } details;
};
struct Frame {
    std::unique_ptr<std::array<u8,16>> buffer;
    AvPlayerFrameInfoEx info;
};
template<class T> struct Queue {
    std::deque<T> items; bool lose_next_pop{};
    auto Size() const { return items.size(); }
    auto& Front() { return items.front(); }
    void Push(T value) { items.push_back(std::move(value)); }
    std::optional<T> Pop() {
        if (lose_next_pop) { lose_next_pop=false; return std::nullopt; }
        if (items.empty()) return std::nullopt;
        T v=std::move(items.front()); items.pop_front(); return std::move(v);
    }
};
enum class AvPlayerAvSyncMode { Default, None };
struct State { AvPlayerAvSyncMode mode=AvPlayerAvSyncMode::Default;
    auto GetSyncMode() const { return mode; } };
struct Signal { unsigned count{}; void Notify() { ++count; } };
struct AvPlayerSource {
    State m_state; bool active=true; u64 time=1000;
    std::atomic_bool m_is_paused=false,m_is_eof=false;
    std::optional<int> m_audio_stream_index;
    std::optional<u64> m_last_audio_ts;
    std::atomic<u64> m_atomic_last_audio_ts{};
    Queue<Frame> m_video_frames;
    Queue<std::unique_ptr<std::array<u8,16>>> m_video_buffers;
    Queue<int> m_audio_frames;
    std::optional<Frame> m_current_video_frame;
    Signal m_video_buffers_cv;
    std::atomic<u64> m_trace_video_data_inactive_count{},m_trace_video_data_empty_count{},
        m_trace_video_data_ahead_count{},m_trace_video_data_success_count{};
    bool IsActive() const { return active; }
    u64 CurrentTime() const { return time; }
    bool GetVideoData(AvPlayerFrameInfo&);
    bool GetVideoData(AvPlayerFrameInfoEx&);
};
// PRODUCTION_METHODS
static unsigned checks{};
#define CHECK(x) do { ++checks; if (!(x)) { std::cerr << __LINE__ << ": " #x " failed\n"; std::exit(1); } } while(0)
static Frame MakeFrame(u8 pattern,u64 timestamp) {
    Frame f{std::make_unique<std::array<u8,16>>(),{}};
    f.buffer->fill(pattern); f.info.p_data=f.buffer->data(); f.info.timestamp=timestamp;
    f.info.details.video={1.5f,4,4}; return f;
}
template<class Info> void FalsePolls() {
    for (unsigned cause=0; cause<6; ++cause) {
        AvPlayerSource s; Info info{};
        s.m_video_frames.Push(MakeFrame(0x21,0));
        CHECK(s.GetVideoData(info)); const auto* pointer=static_cast<const u8*>(info.p_data);
        if(cause==1) s.m_is_paused=true;
        if(cause==2) s.active=false;
        if(cause>=3) s.m_video_frames.Push(MakeFrame(0x42,2000));
        if(cause==3) {s.m_audio_stream_index=0; s.m_last_audio_ts=1000;}
        if(cause==5) {s.m_video_frames.lose_next_pop=true; s.m_state.mode=AvPlayerAvSyncMode::None;}
        CHECK(!s.GetVideoData(info));
        CHECK(info.p_data==pointer && info.timestamp==0);
        // A decoder can immediately overwrite any returned buffer. No-new-frame must
        // keep the last delivered image owned, even when the consumer polls faster.
        std::vector<std::unique_ptr<std::array<u8,16>>> decoder_owned;
        while(auto free=s.m_video_buffers.Pop()) {
            (*free)->fill(0xee);
            decoder_owned.push_back(std::move(*free));
        }
        CHECK(pointer[0]==0x21);
        CHECK(s.m_current_video_frame.has_value());
        CHECK((*s.m_current_video_frame->buffer)[0]==0x21);
        CHECK(pointer[0]==0x21 && s.m_video_buffers_cv.count==0);
    }
}
int main() {
    FalsePolls<AvPlayerFrameInfo>(); FalsePolls<AvPlayerFrameInfoEx>();
    AvPlayerSource s; AvPlayerFrameInfoEx info{};
    // Exactly two buffers, the real minimum. One is displayed while the other
    // is decoded/queued: holding the old image cannot starve replacement delivery.
    s.m_video_frames.Push(MakeFrame(1,0)); s.m_video_buffers.Push(MakeFrame(0,0).buffer);
    CHECK(s.GetVideoData(info));
    for(unsigned i=2;i<1002;++i) {
        auto free=s.m_video_buffers.Pop(); CHECK(free.has_value());
        (*free)->fill(static_cast<u8>(i));
        Frame next{std::move(*free),{}}; next.info.p_data=next.buffer->data(); next.info.timestamp=i;
        s.m_video_frames.Push(std::move(next));
        s.time=i;
        CHECK(s.GetVideoData(info));
        CHECK(static_cast<const u8*>(info.p_data)[0]==static_cast<u8>(i));
        CHECK(info.timestamp==i && s.m_video_buffers.Size()==1);
    }
    CHECK(s.m_video_buffers_cv.count==1000);
    // EOF with empty audio must still deliver queued tail frames, then retain last.
    s.m_is_eof=true; s.m_audio_stream_index=0; s.m_last_audio_ts=0;
    auto free=s.m_video_buffers.Pop(); Frame tail{std::move(*free),{}};
    tail.buffer->fill(0x77);tail.info.p_data=tail.buffer->data();tail.info.timestamp=9000;
    s.m_video_frames.Push(std::move(tail)); CHECK(s.GetVideoData(info));
    CHECK(!s.GetVideoData(info) && static_cast<const u8*>(info.p_data)[0]==0x77);
    CHECK(s.m_current_video_frame && s.m_video_buffers.Size()==1);
    std::cout<<"PASS "<<checks<<" checks: actual video APIs, failed polls retain frame, two-buffer progress, EOF\n";
}
