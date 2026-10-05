// Timer natively (randy-vk.ini [Native] Scene=on): a stopwatch on QueryPerformanceCounter.
//
// Timer (0x28 bytes): +0x00 the counter at Start, +0x08 the ticks accumulated by Stop, +0x18 running, +0x20 the tick
// length (1 / frequency). GetSec is `ticks * tick length` in x87: the 64-bit tick count is loaded exactly (fildll),
// multiplied by the double tick length (rounded to double, the x87 precision control) and stored to a float. That is
// `float(double(ticks) * tickLength)` while the count fits a double, which is every realistic run.
//
// The original computes the tick length once (0x28a4a, guarded) into its global 0x17bee8; we write the same place, so
// anything of the original's reading it sees the same value.
#include "native/timer.h"

#include <windows.h>

#include <cstdint>

namespace rnative::timer {

namespace {

HMODULE g_orig;

template <typename T>
T& Field(void* object, uint32_t offset) { return *reinterpret_cast<T*>(static_cast<uint8_t*>(object) + offset); }
template <typename T>
T& Global(uint32_t rva) { return *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(g_orig) + rva); }

constexpr uint32_t kSecondsPerTick = 0x17BEE8, kComputed = 0x17BEF0;

struct Timer {
    int64_t start;              // +0x00
    int64_t accumulated;        // +0x08
    uint8_t unused[8];          // +0x10
    uint8_t running;            // +0x18
    uint8_t pad[7];             // +0x19
    double secondsPerTick;      // +0x20
};
static_assert(sizeof(Timer) == 0x28, "Timer");

double SecondsPerTick()   // FUN_10028a4a
{
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    return 1.0 / double(frequency.QuadPart);
}

int64_t Now()   // FUN_10028aca
{
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return counter.QuadPart;
}

void* __fastcall Construct(Timer* t, void*)   // Timer::Timer (FUN_10028a62), which leaves +0 as the original does
{
    if (!(Global<uint8_t>(kComputed) & 1)) {
        Global<uint8_t>(kComputed) |= 1;
        Global<double>(kSecondsPerTick) = SecondsPerTick();
    }
    t->accumulated = 0;
    t->secondsPerTick = Global<double>(kSecondsPerTick);
    t->running = 0;
    return t;
}

float __fastcall GetSec(Timer* t, void*)   // Timer::GetSec
{
    if (t->running) return float(double(Now() - t->start) * t->secondsPerTick);
    return float(double(t->accumulated) * t->secondsPerTick);
}

float __fastcall GetMilliSec(Timer* t, void*)   // Timer::GetMilliSec
{
    return float(double(GetSec(t, nullptr)) * 1000.0);
}

void __fastcall Start(Timer* t, void*)   // Timer::Start
{
    if (t->running) return;
    t->running = 1;
    t->start = Now();
}

void __fastcall Stop(Timer* t, void*)   // Timer::Stop
{
    if (!t->running) return;
    t->running = 0;
    t->accumulated += Now() - t->start;
}

}  // namespace

void Install(HMODULE orig)
{
    if (GetMode("Scene", Mode::Off) != Mode::On) return;
    g_orig = orig;
    struct Entry {
        uint32_t rva;
        void* target;
        const char* what;
    };
#define FN(f) reinterpret_cast<void*>(&f)
    const Entry entries[] = {
        {0x28A4A, FN(SecondsPerTick), "Timer tick length (FUN_10028a4a)"},
        {0x28A62, FN(Construct), "Timer::Timer"},
        {0x28ACA, FN(Now), "Timer counter (FUN_10028aca)"},
        {0x28AE1, FN(GetSec), "Timer::GetSec"},
        {0x28B1C, FN(GetMilliSec), "Timer::GetMilliSec"},
        {0x28C05, FN(Start), "Timer::Start"},
        {0x28C1E, FN(Stop), "Timer::Stop"},
    };
#undef FN
    int installed = 0;
    for (const Entry& e : entries) installed += Replace(orig, e.rva, e.target, e.what) ? 1 : 0;
    Log("timer: %d of %d functions native", installed, int(sizeof(entries) / sizeof(entries[0])));
}

}  // namespace rnative::timer
