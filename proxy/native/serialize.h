// serialize.dll (the game's fun:: object archives) as Randy calls it: Serializable_c, ObjectArchive_c, Message_c,
// ArchiveStream_c. Resolved once by name (Get()); null members if the DLL or an export is missing.
#pragma once

#include "native/vc10.h"

#include <windows.h>

#include <cstdint>

namespace rnative::serialize {

struct Api {
    void*(__fastcall* construct)(void* self, void*) = nullptr;
    void*(__fastcall* constructFrom)(void* self, void*, void* archive) = nullptr;
    void(__fastcall* destroy)(void* self, void*) = nullptr;
    void*(__fastcall* getStream)(void* archive, void*) = nullptr;
    int32_t(__fastcall* addInt32)(void* message, void*, const char* name, int32_t value) = nullptr;
    int32_t(__fastcall* addString)(void* message, void*, const char* name, const vc10::String* value) = nullptr;
    int32_t(__fastcall* findString)(void* message, void*, const char* name, vc10::String* out, int32_t index) = nullptr;
    int32_t(__fastcall* addObject)(void* stream, void*, const char* name, const void* object) = nullptr;
    int32_t(__fastcall* findInt32)(const void* message, void*, const char* name, int32_t* out, int32_t index) = nullptr;
    int32_t(__fastcall* findFloat)(const void* message, void*, const char* name, float* out, int32_t index) = nullptr;
    int32_t(__fastcall* findRgb)(const void* message, void*, const char* name, float* out, int32_t index) = nullptr;
    int32_t(__fastcall* addFloat)(void* message, void*, const char* name, float value) = nullptr;
    int32_t(__fastcall* addRgb)(void* message, void*, const char* name, const float* rgb) = nullptr;
    int32_t(__fastcall* addBool)(void* message, void*, const char* name, bool value) = nullptr;
    int32_t(__fastcall* addVector3)(void* message, void*, const char* name, const float* v) = nullptr;
    int32_t(__fastcall* addQuat)(void* message, void*, const char* name, const float* q) = nullptr;
    int32_t(__fastcall* addMatrix4)(void* message, void*, const char* name, const float* m) = nullptr;
    int32_t(__fastcall* findVector3)(const void* message, void*, const char* name, float* out, int32_t index) = nullptr;
    int32_t(__fastcall* findQuat)(const void* message, void*, const char* name, float* out, int32_t index) = nullptr;
    int32_t(__fastcall* findMatrix4)(const void* message, void*, const char* name, float* out, int32_t index) = nullptr;
    bool complete = false;
};

const Api& Get();

}  // namespace rnative::serialize
