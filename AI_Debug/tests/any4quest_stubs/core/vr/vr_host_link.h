#pragma once
namespace Core::Vr {
class HostLink {
public:
    static HostLink& Instance() { static HostLink value; return value; }
    bool Start() { return false; }
};
}
