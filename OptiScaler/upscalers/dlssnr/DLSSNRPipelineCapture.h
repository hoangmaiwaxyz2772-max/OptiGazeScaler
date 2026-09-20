#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>
#include <array>

namespace DLSSNRPipelineTrace
{
// Caller serializes access. Events contain identifiers only, never COM owners.
// A bounded capture must explicitly report lost events; absence of an event
// cannot establish absence of a dependency.
struct Event
{
    const char* kind = nullptr;
    uintptr_t object = 0, resource = 0;
    uint64_t value = 0, ordinal = 0, cpuUs = 0;
    uint32_t thread = 0, present = 0;
    uint64_t call = 0;
    uint32_t result = 0;
    std::array<void*, 16> stack {};
    uint16_t stackSize = 0;
};
class Capture
{
  public:
    static constexpr size_t Capacity = 8192;
    static constexpr unsigned PresentLimit = 6;
    bool active = false;
    unsigned presents = 0;
    uint64_t lost = 0, ordinal = 0;
    std::vector<Event> events;

    void Start()
    {
        events.clear();
        events.reserve(Capacity);
        lost = ordinal = presents = 0;
        active = true;
    }
    void Add(Event event)
    {
        if (!active) return;
        event.ordinal = ++ordinal;
        event.present = presents;
        if (events.size() == Capacity) ++lost;
        else events.push_back(event);
    }
    bool EndPresent()
    {
        if (!active) return false;
        // Reaching the display-call limit starts draining. The owner stops at
        // its next NR boundary (or timeout/off), retaining post-Present signals.
        return ++presents == PresentLimit;
    }
};
}
