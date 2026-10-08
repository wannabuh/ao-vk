// Material maps for RDB textures, side-loaded from <client>\randy-vk\materials\ (any size each):
//   <type>_<id>_n.png    tangent-space normal map (OpenGL convention, as Blender bakes); <type> is the full-quality
//                        table (1010004 world, 1010006 ground, 1010011 character skins), which also covers its lower
//                        levels
//   <type>_<id>_orm.png  PBR material, glTF packing: R occlusion, G roughness, B metallic
//   <type>_<id>_r.png, _m.png, _ao.png   ... or separate greyscale roughness / metallic / occlusion maps (packed here;
//                        a missing one is roughness 1, metallic 0, occlusion 1). Ignored when there is an _orm.png.
//   <type>_<id>_d.png    albedo (diffuse colour): drawn instead of the game's texture, alpha included
//   <type>_<id>_e.png    emissive: the light the surface gives off (black = none), added after its lighting
//   <id>_<suffix>        any of these for any type (fallback)
// ao-assets writes these names (python -m aoassets material-template / import).
//
// Hot reload (RANDYVK_HOTRELOAD=1): both materials\ and live\ (the ao-assets workbench's previews of checked-out
// assets) are watched; when a texture's files change, its maps are decoded again and replace what every surface of it
// had (a map that's gone is removed). A texture id with any file in live\ takes its maps from there only;
// <type>_<id>_live.png alone means "previewed, no maps".
//
// Identity (see HOOKED_EXPORTS in tools/gen_interface.py; the hooks call the original, then record the identity on the
// surface_t's IDirectDrawSurface7 when it is one of ours):
// - World textures: DisplaySystem's RDBTexture_t hands randy an AnarchyTexCreator_t {+0x40 type, +0x44 id}, whose
//   create method calls the exported TextureStreamCreator::CreateTexture with the creator as `this`.
// - Ground textures: RDBGroundTexture_t {DbObject_t: +0x08 type, +0x0C id, +0x20 its RTexture_t once made} uses a
//   plain TextureStreamCreator, created and called inside randy. What crosses the export table is the
//   RTexture_t(name, TextureCreator*) constructor it calls; the RDBGroundTexture_t making it is found by RTTI in the
//   caller's saved registers and stack, and the new RTexture_t's surface_t (+0x30) gets its identity.
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "rvk_backend.h"

#include "material_maps.h"

namespace rvkproxy {

// {6a1f0e52-2d43-4b8e-9c55-7f3a0b1d9e10}
const GUID IID_RvkSurface = {0x6a1f0e52, 0x2d43, 0x4b8e, {0x9c, 0x55, 0x7f, 0x3a, 0x0b, 0x1d, 0x9e, 0x10}};

namespace {

// ---------------------------------------------------------------- creator identity (MSVC RTTI)
bool Readable(const void* p, size_t n)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!p || !VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT)
        return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))
        return false;
    auto end = static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
    return static_cast<const uint8_t*>(p) + n <= end;
}

enum class Kind { Other, AnarchyTexCreator, RDBGroundTexture };

// Class of a polymorphic object from its vtable's complete object locator (x86: vtable[-1] -> COL, COL+12 -> type
// descriptor, name at +8, e.g. ".?AVAnarchyTexCreator_t@@").
Kind ClassOf(const void* object)
{
    static std::unordered_map<const void*, Kind> cache;   // vtable -> class
    if (!Readable(object, 4))
        return Kind::Other;
    const void* vtable = *static_cast<void* const*>(object);
    auto it = cache.find(vtable);
    if (it != cache.end())
        return it->second;
    Kind kind = Kind::Other;
    auto vt = static_cast<const uint8_t* const*>(vtable);
    if (Readable(vt - 1, 4)) {
        const uint8_t* col = vt[-1];
        if (Readable(col, 16)) {
            const uint8_t* td = *reinterpret_cast<const uint8_t* const*>(col + 12);
            static const char kTex[] = ".?AVAnarchyTexCreator_t@@", kGround[] = ".?AVRDBGroundTexture_t@@";
            if (Readable(td + 8, sizeof(kTex)) && std::memcmp(td + 8, kTex, sizeof(kTex)) == 0)
                kind = Kind::AnarchyTexCreator;
            else if (Readable(td + 8, sizeof(kGround)) && std::memcmp(td + 8, kGround, sizeof(kGround)) == 0)
                kind = Kind::RDBGroundTexture;
        }
    }
    cache.emplace(vtable, kind);
    return kind;
}

uint32_t U32(const void* p, size_t offset)
{
    return *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(p) + offset);
}

// An RDBGroundTexture_t still making its texture: a ground texture type, no RTexture_t yet.
bool IsGroundMaker(const void* p)
{
    if (!p || (reinterpret_cast<uintptr_t>(p) & 3) || !Readable(p, 0x28) || ClassOf(p) != Kind::RDBGroundTexture)
        return false;
    uint32_t type = U32(p, 0x08);
    return (type == 1010006 || type == 1010021 || type == 1010022) && U32(p, 0x20) == 0;
}

// The lower texture quality levels are other tables with the same ids: the ground's 1010021/22 for 1010006, the
// world's 1010016/17 for 1010004. A map is named after the full-quality table and serves every level.
uint32_t FullQualityType(uint32_t type)
{
    switch (type) {
    case 1010021: case 1010022: return 1010006;
    case 1010016: case 1010017: return 1010004;
    case 1010019: case 1010020: return 1010011;
    default: return type;
    }
}

uint64_t KeyOf(uint32_t type, uint32_t id) { return uint64_t(FullQualityType(type)) << 32 | id; }

// ---------------------------------------------------------------- material folders
struct Folder {
    std::string dir;
    std::unordered_map<std::string, uint64_t> files;   // lower-case name -> last write time ^ size (hot reload)
};
Folder g_mat, g_live;                         // materials\ (applied maps), live\ (workbench previews, hot reload only)
std::unordered_set<uint64_t> g_liveKeys;      // texture ids with any file in live\ (their maps come from there only)
bool g_scanned = false;
bool g_hot = false;                           // RANDYVK_HOTRELOAD=1

std::string Lower(std::string s)
{
    for (char& c : s) c = char(tolower(static_cast<unsigned char>(c)));
    return s;
}

void ScanFolder(Folder& f)
{
    f.files.clear();
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((f.dir + "*.png").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        uint64_t stamp = (uint64_t(fd.ftLastWriteTime.dwHighDateTime) << 32 | fd.ftLastWriteTime.dwLowDateTime)
                         ^ (uint64_t(fd.nFileSizeHigh) << 32 | fd.nFileSizeLow);
        f.files.emplace(Lower(fd.cFileName), stamp);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

// "<type>_<id>_..." -> its texture key (0 for other names).
uint64_t KeyOfName(const std::string& name)
{
    unsigned type = 0, id = 0;
    char sep = 0;
    if (std::sscanf(name.c_str(), "%u_%u%c", &type, &id, &sep) == 3 && sep == '_' && type >= 1000000)
        return KeyOf(type, id);
    return 0;
}

void RebuildLiveKeys()
{
    g_liveKeys.clear();
    for (auto& [name, stamp] : g_live.files)
        if (uint64_t key = KeyOfName(name))
            g_liveKeys.insert(key);
}

void Scan()
{
    if (g_scanned)
        return;
    g_scanned = true;
    char path[MAX_PATH] = {};
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&Scan), &self);
    GetModuleFileNameA(self, path, MAX_PATH);
    char* slash = std::strrchr(path, '\\');
    if (slash) slash[1] = 0; else path[0] = 0;
    g_mat.dir = std::string(path) + "randy-vk\\materials\\";
    g_live.dir = std::string(path) + "randy-vk\\live\\";
    char hot[8] = {};
    g_hot = GetEnvironmentVariableA("RANDYVK_HOTRELOAD", hot, sizeof(hot)) && hot[0] == '1';
    ScanFolder(g_mat);
    if (g_hot) {
        CreateDirectoryA(g_live.dir.c_str(), nullptr);
        ScanFolder(g_live);
        RebuildLiveKeys();
    }
    RvkLog("materials: %u files in %s%s", unsigned(g_mat.files.size()), g_mat.dir.c_str(),
           g_hot ? "; hot reload on (watching it and live\\)" : "");
}

std::string FindMap(uint32_t type, uint32_t id, const char* suffix)
{
    Scan();
    const Folder& f = g_liveKeys.count(KeyOf(type, id)) ? g_live : g_mat;
    for (uint32_t t : {FullQualityType(type), type}) {
        std::string a = std::to_string(t) + "_" + std::to_string(id) + suffix;
        if (f.files.count(a)) return f.dir + a;
    }
    std::string b = std::to_string(id) + suffix;
    if (f.files.count(b)) return f.dir + b;
    return {};
}

// ---------------------------------------------------------------- map loading (rvk/material_maps.h)
// Decoding takes time (a 1024 x 1024 normal map and material about 100 ms), so it runs on a worker thread: a
// texture asks for its maps (AttachMaterialMaps), the worker decodes them, and PollMaterialMaps (each present, on the
// game's thread like every device call) uploads them to every surface still waiting for that texture id. Decoded
// maps stay in a small cache: a texture's other quality levels, and textures made again for the same id (zoning),
// follow soon after.
constexpr size_t kCacheBytes = 96u << 20;          // decoded maps kept (the client is a 32-bit process)
constexpr size_t kUploadBytesPerFrame = 48u << 20; // uploads spread over frames past this

rvk::maps::MaterialFiles FilesFor(uint32_t type, uint32_t id)
{
    rvk::maps::MaterialFiles f;
    f.normal = FindMap(type, id, "_n.png");
    f.packed = FindMap(type, id, "_orm.png");
    f.parts[0] = FindMap(type, id, "_ao.png");
    f.parts[1] = FindMap(type, id, "_r.png");
    f.parts[2] = FindMap(type, id, "_m.png");
    f.albedo = FindMap(type, id, "_d.png");
    f.emissive = FindMap(type, id, "_e.png");
    return f;
}

using DecodedPtr = std::shared_ptr<const rvk::maps::Decoded>;

// Worker thread and its queues (never destroyed: the thread may still wait on them while the process exits).
struct Worker {
    std::mutex mutex;
    std::condition_variable wake;
    struct Job { uint64_t key; uint32_t gen; rvk::maps::MaterialFiles files; };
    struct Done { uint64_t key; uint32_t gen; DecodedPtr decoded; };
    std::deque<Job> jobs;
    std::vector<Done> done;
};
Worker* g_worker = nullptr;

void WorkerLoop(Worker* w)
{
    for (;;) {
        Worker::Job job;
        {
            std::unique_lock<std::mutex> lock(w->mutex);
            w->wake.wait(lock, [w] { return !w->jobs.empty(); });
            job = std::move(w->jobs.front());
            w->jobs.pop_front();
        }
        auto decoded = std::make_shared<rvk::maps::Decoded>(rvk::maps::Decode(job.files));
        std::lock_guard<std::mutex> lock(w->mutex);
        w->done.push_back({job.key, job.gen, std::move(decoded)});
    }
}

// Game thread only from here.
struct Waiter {
    RSurface* top;
    rvk::Texture* texture;                         // the texture it asked for (a re-created one asks again)
};
std::unordered_map<uint64_t, std::vector<Waiter>> g_waiting;   // by texture id, until its maps are decoded
std::list<std::pair<uint64_t, DecodedPtr>> g_cache;            // most recently used first
std::unordered_map<uint64_t, std::list<std::pair<uint64_t, DecodedPtr>>::iterator> g_cacheIndex;
size_t g_cacheBytes = 0;
std::deque<std::pair<Waiter, DecodedPtr>> g_uploads;           // decoded, not yet uploaded (kUploadBytesPerFrame)
std::unordered_map<uint64_t, uint32_t> g_gen;                  // bumped by a hot reload: older decodes are dropped

DecodedPtr CacheGet(uint64_t key)
{
    auto it = g_cacheIndex.find(key);
    if (it == g_cacheIndex.end())
        return nullptr;
    g_cache.splice(g_cache.begin(), g_cache, it->second);
    return it->second->second;
}

void CacheErase(uint64_t key)
{
    auto it = g_cacheIndex.find(key);
    if (it == g_cacheIndex.end())
        return;
    g_cacheBytes -= it->second->second->Bytes();
    g_cache.erase(it->second);
    g_cacheIndex.erase(it);
}

void CachePut(uint64_t key, DecodedPtr d)
{
    if (g_cacheIndex.count(key) || d->Bytes() > kCacheBytes / 2)
        return;
    g_cache.emplace_front(key, d);
    g_cacheIndex[key] = g_cache.begin();
    g_cacheBytes += d->Bytes();
    while (g_cacheBytes > kCacheBytes && !g_cache.empty()) {
        g_cacheBytes -= g_cache.back().second->Bytes();
        g_cacheIndex.erase(g_cache.back().first);
        g_cache.pop_back();
    }
}

unsigned g_attached = 0;

void Upload(const Waiter& w, const rvk::maps::Decoded& d)
{
    if (w.top->texture != w.texture || !g_rvk.device)
        return;                                    // made again since: that one asks for itself
    RSurface* top = w.top;
    rvk::maps::Replace(*g_rvk.device, w.texture, d);   // exactly these maps (a hot reload may have removed some)
    ++g_attached;
    if (g_attached <= 64 || (g_attached & (g_attached - 1)) == 0)
        RvkLog("materials: RDB texture %u:%u gets%s%s%s%s%s%s (%u attached)", top->rdbType, top->rdbId,
               d.normal.Empty() ? "" : " [normal map]", d.orm.Empty() ? "" : " [PBR material ",
               d.orm.Empty() ? "" : (d.ormFrom + "]").c_str(), d.albedo.Empty() ? "" : " [albedo map]",
               d.emissive.Empty() ? "" : " [emissive map]", d.Bytes() ? "" : " nothing", g_attached);
}

}  // namespace

void HotReload();

void PollMaterialMaps()
{
    HotReload();
    if (g_worker) {
        std::vector<Worker::Done> done;
        {
            std::lock_guard<std::mutex> lock(g_worker->mutex);
            done.swap(g_worker->done);
        }
        for (auto& [key, gen, decoded] : done) {
            if (gen != g_gen[key])
                continue;                          // decoded from files a hot reload replaced since
            if (!decoded->errors.empty())
                RvkLog("materials: %s", decoded->errors.c_str());
            CachePut(key, decoded);
            auto it = g_waiting.find(key);
            if (it == g_waiting.end())
                continue;
            for (const Waiter& w : it->second)
                g_uploads.emplace_back(w, decoded);
            g_waiting.erase(it);
        }
    }
    size_t bytes = 0;
    while (!g_uploads.empty() && bytes < kUploadBytesPerFrame) {
        auto [w, d] = std::move(g_uploads.front());
        g_uploads.pop_front();
        bytes += d->Bytes();
        Upload(w, *d);
    }
}

namespace {
std::unordered_set<RSurface*> g_identified;       // surfaces holding an RDB texture (for DescribeTexture)
std::unordered_map<uint64_t, std::string> g_names;   // RDB texture -> the name the game gave it
}  // namespace

void ForgetMaterialMaps(RSurface* top)
{
    g_identified.erase(top);
    if (g_waiting.empty() && g_uploads.empty())
        return;
    for (auto& [key, list] : g_waiting)
        list.erase(std::remove_if(list.begin(), list.end(), [top](const Waiter& w) { return w.top == top; }),
                   list.end());
    g_uploads.erase(std::remove_if(g_uploads.begin(), g_uploads.end(),
                                   [top](const auto& u) { return u.first.top == top; }),
                    g_uploads.end());
}

namespace {

unsigned g_registered = 0;

unsigned g_ground = 0;

void Register(void* surface_t, uint32_t type, uint32_t id, const char* name)
{
    if (!surface_t || !Readable(surface_t, 4))
        return;
    auto* dds = *static_cast<IUnknown**>(surface_t);
    if (!dds)
        return;
    RSurface* s = nullptr;
    if (FAILED(dds->QueryInterface(IID_RvkSurface, reinterpret_cast<void**>(&s))) || !s)
        return;                                       // not our backend (D7VK): nothing to attach to
    RSurface* top = s->top ? s->top : s;
    top->rdbType = type;
    top->rdbId = id;
    g_identified.insert(top);
    if (name && Readable(name, 1)) {
        std::string& known = g_names[KeyOf(type, id)];
        if (known.empty())
            known.assign(name, strnlen(name, 120));
    }
    bool ground = type == 1010006 || type == 1010021 || type == 1010022;
    g_ground += ground;
    if (++g_registered <= 8 || (g_registered & (g_registered - 1)) == 0 || (ground && g_ground <= 4))
        RvkLog("materials: RDB texture %u:%u is a %lux%lu surface (%u textures identified)", type, id,
               top->desc.dwWidth, top->desc.dwHeight, g_registered);
    if (top->texture)
        AttachMaterialMaps(top);
    s->Release();
}

void RegisterCreator(void* surface, const void* creator, const char* name)
{
    if (ClassOf(creator) != Kind::AnarchyTexCreator || !Readable(creator, 0x48))
        return;
    static const unsigned index = ComIndex("rvk::Materials");
    ComScope scope(index);
    Register(surface, U32(creator, 0x40), U32(creator, 0x44), name);
}

using CreateFromBitmap = void*(__thiscall*)(void* self, void* bitmap, const char* name);
using CreateFromStream = void*(__thiscall*)(void* self, void* stream, const char* name);
using RTextureFromCreator = void*(__thiscall*)(void* self, const char* name, void* creator);

template <typename F>
F Original(const char* mangled)
{
    HMODULE orig = GetModuleHandleA("randy31_orig.dll");
    if (!orig) orig = LoadLibraryA("randy31_orig.dll");
    return reinterpret_cast<F>(GetProcAddress(orig, mangled));
}

}  // namespace

std::string DescribeTexture(const rvk::Texture* texture, std::string* reference)
{
    if (!texture)
        return "nothing (no textured 3D surface there)";
    char buf[512];
    for (RSurface* top : g_identified) {
        if (top->texture != texture)
            continue;
        const uint32_t full = FullQualityType(top->rdbType);
        auto it = g_names.find(KeyOf(top->rdbType, top->rdbId));
        std::string maps;
        for (const char* suffix : {"_n.png", "_orm.png", "_r.png", "_m.png", "_ao.png", "_d.png", "_e.png"})
            if (!FindMap(top->rdbType, top->rdbId, suffix).empty())
                maps += std::string(" ") + suffix;
        std::snprintf(buf, sizeof(buf), "RDB texture %u:%u%s%s%s, %lux%lu; maps:%s", full, top->rdbId,
                      it != g_names.end() ? " '" : "", it != g_names.end() ? it->second.c_str() : "",
                      it != g_names.end() ? "'" : "", top->desc.dwWidth, top->desc.dwHeight,
                      maps.empty() ? " none" : maps.c_str());
        if (reference) {
            char ref[32];
            std::snprintf(ref, sizeof(ref), "%u:%u", full, top->rdbId);
            *reference = ref;
        }
        return buf;
    }
    std::snprintf(buf, sizeof(buf), "a texture that isn't an RDB texture (%ux%u; made by the game itself)",
                  texture->Width(), texture->Height());
    return buf;
}

void AttachMaterialMaps(RSurface* top)
{
    if (!top || !top->rdbId || !top->texture || top->materialsFor == top->texture || !g_rvk.device)
        return;
    top->materialsFor = top->texture;
    uint64_t key = KeyOf(top->rdbType, top->rdbId);
    Waiter waiter{top, top->texture};
    if (DecodedPtr cached = CacheGet(key)) {
        g_uploads.emplace_back(waiter, cached);
        return;
    }
    auto it = g_waiting.find(key);
    if (it != g_waiting.end()) {                  // being decoded already
        it->second.push_back(waiter);
        return;
    }
    rvk::maps::MaterialFiles files = FilesFor(top->rdbType, top->rdbId);
    if (!files.Any())
        return;
    g_waiting[key].push_back(waiter);
    if (!g_worker) {
        g_worker = new Worker;
        std::thread(WorkerLoop, g_worker).detach();
    }
    {
        std::lock_guard<std::mutex> lock(g_worker->mutex);
        g_worker->jobs.push_back({key, g_gen[key], std::move(files)});
    }
    g_worker->wake.notify_one();
}

// ---------------------------------------------------------------- hot reload
namespace {
HANDLE g_watch[2] = {};
bool g_watchTried = false;
DWORD g_lastCheck = 0, g_changedAt = 0;
bool g_changed = false;

void Changed(const std::unordered_map<std::string, uint64_t>& before, const std::unordered_map<std::string, uint64_t>& now,
             std::unordered_set<uint64_t>* keys)
{
    for (auto& [name, stamp] : now) {
        auto it = before.find(name);
        if (it == before.end() || it->second != stamp)
            if (uint64_t key = KeyOfName(name)) keys->insert(key);
    }
    for (auto& [name, stamp] : before)
        if (!now.count(name))
            if (uint64_t key = KeyOfName(name)) keys->insert(key);
}

void Reload(uint64_t key)
{
    ++g_gen[key];
    CacheErase(key);
    g_waiting.erase(key);
    g_uploads.erase(std::remove_if(g_uploads.begin(), g_uploads.end(),
                                   [key](const auto& u) { return KeyOf(u.first.top->rdbType, u.first.top->rdbId) == key; }),
                    g_uploads.end());
    const uint32_t type = uint32_t(key >> 32), id = uint32_t(key);
    const bool any = FilesFor(type, id).Any();
    unsigned surfaces = 0;
    for (RSurface* top : g_identified) {
        if (!top->texture || KeyOf(top->rdbType, top->rdbId) != key)
            continue;
        ++surfaces;
        top->materialsFor = nullptr;
        if (any)
            AttachMaterialMaps(top);
        else if (g_rvk.device)
            rvk::maps::Replace(*g_rvk.device, top->texture, rvk::maps::Decoded{});   // its maps are gone
    }
    RvkLog("hot reload: RDB texture %u:%u from %s, %u surface(s) in use", type, id,
           !any ? "nothing (maps removed)" : g_liveKeys.count(key) ? "live\\ (preview)" : "materials\\", surfaces);
}
}  // namespace

void HotReload()
{
    Scan();
    if (!g_hot)
        return;
    const DWORD now = GetTickCount();
    if (now - g_lastCheck < 200)
        return;
    g_lastCheck = now;
    if (!g_watchTried) {
        g_watchTried = true;
        const Folder* folders[2] = {&g_mat, &g_live};
        for (int i = 0; i < 2; ++i) {
            HANDLE h = FindFirstChangeNotificationA(folders[i]->dir.c_str(), FALSE,
                                                    FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE |
                                                    FILE_NOTIFY_CHANGE_SIZE);
            g_watch[i] = h == INVALID_HANDLE_VALUE ? nullptr : h;
            if (!g_watch[i])
                RvkLog("hot reload: can't watch %s (error %lu)", folders[i]->dir.c_str(), GetLastError());
        }
    }
    for (HANDLE& h : g_watch)
        if (h && WaitForSingleObject(h, 0) == WAIT_OBJECT_0) {
            FindNextChangeNotification(h);
            g_changed = true;
            g_changedAt = now;
        }
    if (!g_changed || now - g_changedAt < 300)       // files may still be being written
        return;
    g_changed = false;
    auto mat = std::move(g_mat.files), live = std::move(g_live.files);
    ScanFolder(g_mat);
    ScanFolder(g_live);
    RebuildLiveKeys();
    std::unordered_set<uint64_t> keys;
    Changed(mat, g_mat.files, &keys);
    Changed(live, g_live.files, &keys);
    for (uint64_t key : keys)
        Reload(key);
}

}  // namespace rvkproxy

using namespace rvkproxy;

// The hooked exports (thiscall: `this` in ecx; __fastcall's unused edx keeps the stack arguments in place).
extern "C" void* __fastcall rvk_CreateTextureBitmap(void* self, void*, void* bitmap, const char* name)
{
    static auto original = Original<CreateFromBitmap>("?CreateTexture@TextureStreamCreator@@QAEPAVsurface_t@@PAVLBitmap_t@@PBD@Z");
    void* surface = original(self, bitmap, name);
    RegisterCreator(surface, self, name);
    return surface;
}

extern "C" void* __fastcall rvk_CreateTextureStream(void* self, void*, void* stream, const char* name)
{
    static auto original = Original<CreateFromStream>("?CreateTexture@TextureStreamCreator@@QAEPAVsurface_t@@PAVPositionIO_t@fun@@PBD@Z");
    void* surface = original(self, stream, name);
    RegisterCreator(surface, self, name);
    return surface;
}

// RTexture_t::RTexture_t(char const*, TextureCreator*): world textures are identified by the CreateTexture hooks above;
// a plain creator may be an RDBGroundTexture_t's. Its `this` is in a callee-saved register of the caller, which this
// function's prologue pushes just below its return address, or on the caller's stack above it.
extern "C" void* __fastcall rvk_RTextureFromCreator(void* self, void*, const char* name, void* creator)
{
    const void* maker = nullptr;
    if (ClassOf(creator) != Kind::AnarchyTexCreator) {
        auto* sp = reinterpret_cast<const uint32_t*>(_AddressOfReturnAddress()) - 8;
        for (int i = 0; !maker && i < 264 && Readable(sp + i, 4); ++i)
            if (IsGroundMaker(reinterpret_cast<const void*>(uintptr_t(sp[i]))))
                maker = reinterpret_cast<const void*>(uintptr_t(sp[i]));
    }
    static auto original = Original<RTextureFromCreator>("??0RTexture_t@@QAE@PBDPAVTextureCreator@@@Z");
    void* texture = original(self, name, creator);
    if (maker && texture && Readable(static_cast<uint8_t*>(texture) + 0x30, 4)) {
        static const unsigned index = ComIndex("rvk::Materials");
        ComScope scope(index);
        Register(*reinterpret_cast<void**>(static_cast<uint8_t*>(texture) + 0x30), U32(maker, 0x08), U32(maker, 0x0C),
                 name);
    }
    return texture;
}

