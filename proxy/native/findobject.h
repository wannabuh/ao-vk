// fun::FindObjectFor<T> natively (findobject.cpp): randy-vk.ini [Native] Scene=on. The serialize library's
// per-class object lookup the archive readers use: ArchiveStream_c::DoFindObject (through serialize.dll), then an
// __RTDynamicCast to the wanted class. Each original instantiation is one rva and one target RTTI descriptor.
#pragma once

#include "native/native.h"

namespace rnative::findobject {

void Install(HMODULE orig);

}  // namespace rnative::findobject
