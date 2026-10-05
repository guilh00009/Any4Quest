// SPDX-License-Identifier: GPL-2.0-or-later
// Actual HostLink and Runtime over SOCK_SEQPACKET; only platform/log dependencies are stubs.
#include <array>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "core/vr/vr_host_link.h"
#include "core/vr/vr_move_input.h"
#include "core/vr/vr_runtime.h"

using Clock = std::chrono::steady_clock;
namespace Protocol = Core::Vr::Protocol;

#ifdef MOCK_HOST_SOCKET
// Test-only socket syscalls. The production HostLink ReadLoop and Runtime are unchanged.
// This is used where a sandbox forbids even Unix-domain socket creation.
namespace {
constexpr int CoreFd = 10000, PeerFd = 10001;
std::mutex socket_mutex;
std::condition_variable socket_ready;
std::deque<std::vector<char>> incoming, outgoing;
bool socket_closed{};
}
extern "C" {
int __wrap_socket(int, int, int) { return CoreFd; }
int __wrap_connect(int fd, const sockaddr*, socklen_t) { assert(fd == CoreFd); return 0; }
ssize_t __wrap_send(int fd, const void* data, size_t size, int) {
    std::scoped_lock lock{socket_mutex};
    if (socket_closed) { errno = EPIPE; return -1; }
    const auto* bytes = static_cast<const char*>(data);
    (fd == PeerFd ? incoming : outgoing).emplace_back(bytes, bytes + size);
    socket_ready.notify_all();
    return static_cast<ssize_t>(size);
}
ssize_t __wrap_recvmsg(int fd, msghdr* message, int) {
    assert(fd == CoreFd && message->msg_iovlen == 1);
    std::unique_lock lock{socket_mutex};
    socket_ready.wait(lock, [] { return socket_closed || !incoming.empty(); });
    if (incoming.empty()) return 0;
    auto packet = std::move(incoming.front());
    incoming.pop_front();
    const auto copied = std::min(packet.size(), message->msg_iov[0].iov_len);
    std::memcpy(message->msg_iov[0].iov_base, packet.data(), copied);
    message->msg_flags = copied < packet.size() ? MSG_TRUNC : 0;
    message->msg_controllen = 0;
    return copied;
}
ssize_t __wrap_recv(int fd, void* data, size_t size, int flags) {
    assert(fd == PeerFd);
    std::scoped_lock lock{socket_mutex};
    if (outgoing.empty()) { errno = EAGAIN; return -1; }
    auto packet = std::move(outgoing.front());
    outgoing.pop_front();
    const auto copied = std::min(size, packet.size());
    std::memcpy(data, packet.data(), copied);
    return (flags & MSG_TRUNC) != 0 ? packet.size() : copied;
}
int __wrap_close(int) {
    std::scoped_lock lock{socket_mutex};
    socket_closed = true;
    socket_ready.notify_all();
    return 0;
}
}
#endif
namespace Libraries::Kernel {
u64 PS4_SYSV_ABI sceKernelGetProcessTime() {
    return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count();
}
}
static u64 Now() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}
template <typename Predicate> void Wait(Predicate predicate) {
    const auto deadline = Clock::now() + std::chrono::seconds{2};
    while (!predicate()) {
        assert(Clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
}

int main(int argc, char** argv) {
    assert(argc == 2);
#ifndef MOCK_HOST_SOCKET
    const int server = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (server < 0) {
        std::perror("test socket(AF_UNIX, SOCK_SEQPACKET)");
        return errno == EPERM || errno == EACCES ? 77 : 1;
    }
    assert(server >= 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    assert(std::strlen(argv[1]) < sizeof(address.sun_path));
    std::strcpy(address.sun_path, argv[1]);
    assert(bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    assert(listen(server, 1) == 0);
#endif
    setenv("SHADPS4_VR_SOCKET", argv[1], 1);
    setenv("SHADPS4_VR_INPUT_MODE", "move", 1);
    setenv("SHADPS4_VR", "0", 1); // Start the socket explicitly after runtime configuration.
    auto& runtime = Core::Vr::Runtime::Instance();
    runtime.Configure(false, false);
    auto& link = Core::Vr::HostLink::Instance();
    assert(link.Start());
#ifdef MOCK_HOST_SOCKET
    const int peer = PeerFd;
#else
    const int peer = accept4(server, nullptr, nullptr, SOCK_CLOEXEC);
    assert(peer >= 0);
#endif
    const auto send_packet = [&](const void* data, size_t size) {
        assert(send(peer, data, size, MSG_NOSIGNAL) == static_cast<ssize_t>(size));
    };
    Protocol::MoveState base;
    base.flags = Protocol::MoveState::Connected | Protocol::MoveState::Tracked |
                 Protocol::MoveState::LinearVelocityValid;
    base.position[0] = 0.2f;
    base.buttons = Core::Vr::MoveInput::Cross;
    base.trigger = 0.5f;
    const auto valid = [&](uint32_t hand) {
        auto packet = base;
        packet.hand = hand;
        packet.sample_time_ns = Now();
        const auto sequence = runtime.GetMove(hand).device.sequence;
        send_packet(&packet, sizeof(packet));
        Wait([&] { return runtime.GetMove(hand).device.sequence > sequence; });
    };
    valid(0);
    assert(runtime.GetMove(0).connected && !runtime.GetMove(1).connected);
    assert(runtime.GetMove(0).trigger == 128);
    assert(runtime.GetMove(0).buttons & Core::Vr::MoveInput::Cross);
    const auto invalid = [&](auto mutate, int size_delta = 0) {
        auto packet = base;
        packet.sample_time_ns = Now();
        mutate(packet);
        std::array<char, sizeof(packet) + 16> bytes{};
        std::memcpy(bytes.data(), &packet, sizeof(packet));
        const auto sequence = runtime.GetMove(0).device.sequence;
        send_packet(bytes.data(), sizeof(packet) + size_delta);
        valid(1); // An ordered marker proves the malformed packet has been handled.
        assert(runtime.GetMove(0).device.sequence == sequence);
    };
    invalid([](auto&) {}, -1);
    invalid([](auto&) {}, 1);
    invalid([](auto& p) { p.version = 99; });
    invalid([](auto& p) { p.header.magic = 0; });
    invalid([](auto& p) { p.hand = 2; });
    invalid([](auto& p) { p.flags |= 0x80000000; });
    invalid([](auto& p) { p.flags = Protocol::MoveState::Tracked; });
    invalid([](auto& p) { p.flags = Protocol::MoveState::Connected |
                                  Protocol::MoveState::LinearVelocityValid; });
    invalid([](auto& p) { p.sample_time_ns = 0; });
    invalid([](auto& p) { p.sample_time_ns -= 1'000'000'000; });
    invalid([](auto& p) { p.sample_time_ns += 1'000'000'000; });
    invalid([](auto& p) { p.buttons = 0xffff; });
    invalid([](auto& p) { p.trigger = 1.5f; });
    invalid([](auto& p) { p.trigger = std::numeric_limits<float>::quiet_NaN(); });
    invalid([](auto& p) { p.position[0] = std::numeric_limits<float>::infinity(); });

    const auto drain = [&] {
        char bytes[256];
        while (recv(peer, bytes, sizeof(bytes), MSG_DONTWAIT) > 0) {}
    };
    const auto feedback = [&](uint8_t intensity) {
        bool found = false;
        Wait([&] {
            Protocol::MoveFeedback result;
            const auto size = recv(peer, &result, sizeof(result), MSG_DONTWAIT | MSG_TRUNC);
            if (size == sizeof(result) && result.header.type == Protocol::MessageType::MoveFeedback &&
                result.hand == 0 && result.intensity == intensity) found = true;
            return found;
        });
    };
    valid(0);
    drain();
    runtime.SetMoveVibration(0, 127);
    feedback(127); // Runtime listener routes left independently.
    drain();
    valid(0);
    feedback(127); // Unchanged vibration is renewed by healthy input packets.

    close(peer);
    Wait([&] { return !link.IsConnected() && !runtime.GetMove(0).connected &&
                    !runtime.GetMove(1).connected; });
    assert(runtime.GetMoveFeedback(0).intensity == 0);
#ifndef MOCK_HOST_SOCKET
    close(server);
    unlink(argv[1]);
    puts("Quest Move real Unix transport: passed");
#else
    puts("Quest Move production parser/runtime with mocked socket syscalls: passed");
#endif
}
