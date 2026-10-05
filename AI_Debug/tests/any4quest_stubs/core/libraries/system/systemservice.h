#pragma once
namespace Libraries::SystemService {
enum class OrbisSystemServiceEventType { ResetVrPosition };
struct OrbisSystemServiceEvent { OrbisSystemServiceEventType event_type; };
inline void PushSystemServiceEvent(const OrbisSystemServiceEvent&) {}
}
