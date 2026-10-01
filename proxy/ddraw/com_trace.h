// Pass-through wrappers around the DirectDraw 7 / Direct3D 7 objects randy31_orig.dll creates, counting
// every COM call. Installed by redirecting randy31_orig's imports of DirectDrawCreateEx and
// DirectDrawEnumerateExA (nothing else in the process is affected). Enabled with RANDYVK_DDRAW=trace.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ddraw.h>
#include <d3d.h>

#include <cstdint>
#include <cstdio>

namespace rvkproxy {

// Called once randy31_orig.dll is loaded. Returns true if the hooks were installed.
bool InstallDDrawHooks(HMODULE randyOrig);
bool ComTraceActive();

// Per-method call counters (index = generated method number) and their names.
unsigned ComMethodCount();
const char* ComMethodName(unsigned index);
uint32_t ComCallCount(unsigned index);
void ComDetailSummary(FILE* f);           // argument summaries of selected calls (com_trace_detail.cpp)

// ---- used by the generated wrappers ----
void CountComCall(unsigned index);

template <typename I>
class TraceBase {
public:
    explicit TraceBase(I* real) : m_real(real) {}
    I* Real() const { return m_real; }

protected:
    I* m_real;
};

void* LookupWrapper(void* real);
void RegisterWrapper(void* real, void* wrapper);
void UnregisterWrapper(void* real, void* wrapper);
bool IsWrapper(const void* p);

// Wrapper for a real interface pointer (created on first sight). Takes over the reference the caller got.
template <typename W, typename I>
I* Wrap(I* real)
{
    if (!real)
        return nullptr;
    if (IsWrapper(real))                         // already ours (e.g. handed back unchanged)
        return real;
    if (void* w = LookupWrapper(real))
        return static_cast<I*>(static_cast<W*>(w));
    W* w = new W(real);
    RegisterWrapper(real, static_cast<W*>(w));
    return w;
}

// The real interface behind a wrapper; anything that isn't one of ours is passed through.
template <typename W, typename I>
I* Unwrap(I* p)
{
    if (!p || !IsWrapper(static_cast<W*>(p)))
        return p;
    return static_cast<W*>(p)->Real();
}

}  // namespace rvkproxy
