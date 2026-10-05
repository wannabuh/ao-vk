// Randy_t::GetDevices natively (part of [Native] Device=on): every DirectDraw adapter with its caps, display modes
// and Direct3D devices, into DisplaySystem's std::vector<Randy_t::DeviceDesc_t>. DisplaySystem destroys the vector
// with its own (VS2010) code, so the records keep the original's layout, strings and heap (msvcr100).
//
// DeviceDesc_t (0x834 bytes): +0x000 the adapter's GUID (GUID_NULL: the primary), +0x010 std::string description,
// +0x02C std::string driver, +0x048 HMONITOR, +0x04C DDCAPS (0x17C), +0x1C8 windowed on the primary, +0x1C9 can render
// windowed (DDCAPS2_CANRENDERWINDOWED), +0x1CA the primary, +0x1CC display modes (128 x {u16 width, u16 height, u8
// bits, pad}), +0x4CC their count, +0x4D0 Direct3D devices (8 x 0x6C: GUID, Randy_t::HardwareLevel_e, W-buffer,
// std::string description, name, and one never set), +0x830 their count.
// DirectDraw comes from randy31_orig's own imports (the proxy points them at rvk): DirectDrawEnumerateExA at
// 0x1008A000, DirectDrawCreateEx at 0x1008A004.
#include "native/devices.h"
#include "native/vc10.h"

#include <ddraw.h>

#include <cstddef>
#include <cstring>

namespace rnative::devices {

namespace {

HMODULE g_orig;

struct Mode {
    uint16_t width, height;
    uint8_t bits, pad;
};
static_assert(sizeof(Mode) == 6, "display mode");

struct D3dDevice {
    GUID guid;
    int32_t level;                                  // Randy_t::HardwareLevel_e: 2 T&L HAL, 1 HAL, 0 other
    bool wBuffer;                                   // D3DPRASTERCAPS_WBUFFER
    vc10::String description, name, unused;
};
static_assert(sizeof(D3dDevice) == 0x6C, "Randy_t::DeviceDesc_t device");

struct DeviceDesc {
    GUID guid;
    vc10::String description, driver;
    HMONITOR monitor;
    uint8_t caps[0x17C];                            // DDCAPS
    bool windowedPrimary, canRenderWindowed, primary;
    Mode modes[128];
    uint8_t modeCount;
    D3dDevice devices[8];
    uint8_t deviceCount;
};
static_assert(sizeof(DeviceDesc) == 0x834, "Randy_t::DeviceDesc_t");
static_assert(offsetof(DeviceDesc, caps) == 0x4C && offsetof(DeviceDesc, modes) == 0x1CC &&
                  offsetof(DeviceDesc, modeCount) == 0x4CC && offsetof(DeviceDesc, devices) == 0x4D0 &&
                  offsetof(DeviceDesc, deviceCount) == 0x830,
              "Randy_t::DeviceDesc_t layout");

constexpr GUID kTnLHalDevice = {0xF5049E78, 0x4861, 0x11D2, {0xA4, 0x07, 0x00, 0xA0, 0xC9, 0x06, 0x29, 0xA8}};
constexpr GUID kHalDevice = {0x84E63DE0, 0x46AA, 0x11CF, {0x81, 0x6F, 0x00, 0x00, 0xC0, 0x20, 0x15, 0x6E}};
constexpr GUID kDirect3D7 = {0xF5049E77, 0x4861, 0x11D2, {0xA4, 0x07, 0x00, 0xA0, 0xC9, 0x06, 0x29, 0xA8}};
constexpr GUID kDirectDraw7 = {0x15E65EC0, 0x3B9C, 0x11D2, {0xB9, 0x2F, 0x00, 0x60, 0x97, 0x97, 0xEA, 0x5B}};

template <typename T>
T Import(uint32_t rva) { return *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(g_orig) + rva); }

template <typename... A>
HRESULT Com(void* object, uint32_t offset, A... args)
{
    using Fn = HRESULT(__stdcall*)(void*, A...);
    return reinterpret_cast<Fn>((*static_cast<void***>(object))[offset / 4])(object, args...);
}

void Assign(vc10::String& s, const char* text) { s.assign(text, std::strlen(text)); }

void Init(DeviceDesc& d)                            // FUN_10044540: the strings empty (the rest as it comes)
{
    d.description.init();
    d.driver.init();
    for (D3dDevice& dev : d.devices) {
        dev.description.init();
        dev.name.init();
        dev.unused.init();
    }
}

void Destroy(DeviceDesc& d)                         // FUN_10044594
{
    for (D3dDevice& dev : d.devices) {
        dev.unused.release();
        dev.name.release();
        dev.description.release();
    }
    d.driver.release();
    d.description.release();
}

// FUN_10041693: display modes but 8 and 24 bits, at most 128.
HRESULT __stdcall EnumMode(const DDSURFACEDESC2* mode, DeviceDesc* d)
{
    const uint32_t bits = mode->ddpfPixelFormat.dwRGBBitCount;
    if (bits != 8 && bits != 24 && d->modeCount < 0x80) {
        Mode& m = d->modes[d->modeCount++];
        m.width = uint16_t(mode->dwWidth);
        m.height = uint16_t(mode->dwHeight);
        m.bits = uint8_t(bits);
    }
    return DDENUMRET_OK;
}

// FUN_10043545: a Direct3D device (no limit, as the original: DirectDraw has at most a few).
HRESULT __stdcall EnumD3dDevice(const char* description, const char* name, const uint8_t* desc7, DeviceDesc* d)
{
    D3dDevice& dev = d->devices[d->deviceCount++];
    Assign(dev.description, description);
    Assign(dev.name, name);
    std::memcpy(&dev.guid, desc7 + 0xC4, sizeof(GUID));   // D3DDEVICEDESC7::deviceGUID
    dev.level = dev.guid == kTnLHalDevice ? 2 : dev.guid == kHalDevice ? 1 : 0;
    dev.wBuffer = (*reinterpret_cast<const uint32_t*>(desc7 + 0x44) >> 18) & 1;   // dpcTriCaps.dwRasterCaps
    return 1;                                       // D3DENUMRET_OK
}

// FUN_10043ce4: one adapter, appended to the vector if DirectDraw opens on it.
BOOL __stdcall EnumAdapter(GUID* guid, char* description, char* driver, vc10::Vector<DeviceDesc>* out,
                           HMONITOR monitor)
{
    DeviceDesc d;
    Init(d);
    d.primary = !guid;
    d.guid = guid ? *guid : GUID{};
    Assign(d.description, description);
    Assign(d.driver, driver);
    d.monitor = monitor;
    using CreateFn = HRESULT(WINAPI*)(GUID*, void**, REFIID, IUnknown*);
    void* dd = nullptr;
    if (Import<CreateFn>(0x8A004)(guid, &dd, kDirectDraw7, nullptr) == 0) {
        std::memset(d.caps, 0, sizeof(d.caps));
        *reinterpret_cast<uint32_t*>(d.caps) = sizeof(d.caps);
        Com(dd, 0x2C, static_cast<void*>(d.caps), static_cast<void*>(nullptr));   // GetCaps
        DDDEVICEIDENTIFIER2 id;
        if (Com(dd, 0x6C, &id, DWORD(0)) == 0) {   // GetDeviceIdentifier: its names win
            if (id.szDescription[0]) Assign(d.description, id.szDescription);
            if (id.szDriver[0]) Assign(d.driver, id.szDriver);
        }
        d.canRenderWindowed = (*reinterpret_cast<const uint32_t*>(d.caps + 8) >> 19) & 1;   // dwCaps2
        d.windowedPrimary = !monitor && d.canRenderWindowed;
        d.modeCount = 0;
        Com(dd, 0x20, DWORD(0), static_cast<void*>(nullptr), static_cast<void*>(&d),
            reinterpret_cast<void*>(&EnumMode));   // EnumDisplayModes
        d.deviceCount = 0;
        void* d3d = nullptr;
        Com(dd, 0x00, &kDirect3D7, &d3d);           // QueryInterface (not checked, as the original)
        Com(d3d, 0x0C, reinterpret_cast<void*>(&EnumD3dDevice), static_cast<void*>(&d));   // EnumDevices
        Com(d3d, 0x08);                             // Release
        Com(dd, 0x08);
        out->push_back(d);                          // moved in: VS2010 strings move bytewise
        return DDENUMRET_OK;
    }
    Destroy(d);
    return DDENUMRET_OK;
}

// ?GetDevices@Randy_t@@SAXAAV?$vector@UDeviceDesc_t@Randy_t@@...@std@@@Z
void __cdecl GetDevices(vc10::Vector<DeviceDesc>* out)
{
    using EnumerateFn = HRESULT(WINAPI*)(void*, void*, DWORD);
    Import<EnumerateFn>(0x8A000)(reinterpret_cast<void*>(&EnumAdapter), out,
                                 DDENUM_ATTACHEDSECONDARYDEVICES | DDENUM_DETACHEDSECONDARYDEVICES |
                                     DDENUM_NONDISPLAYDEVICES);
}

}  // namespace

void Install(HMODULE orig)
{
    g_orig = orig;
    struct Entry {
        uint32_t rva;
        void* target;
        const char* what;
    };
#define FN(f) reinterpret_cast<void*>(&f)
    const Entry entries[] = {
        {0x43F0C, FN(GetDevices), "Randy_t::GetDevices"},
        {0x43CE4, FN(EnumAdapter), "Randy_t::GetDevices adapter (FUN_10043ce4)"},
        {0x41693, FN(EnumMode), "Randy_t::GetDevices display mode (FUN_10041693)"},
        {0x43545, FN(EnumD3dDevice), "Randy_t::GetDevices Direct3D device (FUN_10043545)"},
    };
#undef FN
    int installed = 0;
    for (const Entry& e : entries) installed += Replace(orig, e.rva, e.target, e.what) ? 1 : 0;
    Log("device enumeration: %d of %d functions native", installed, int(sizeof(entries) / sizeof(entries[0])));
}

}  // namespace rnative::devices
