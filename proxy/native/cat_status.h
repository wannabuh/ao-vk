// CATStatus_t and CATStdioStatus_t natively (randy-vk.ini [Native] Scene=on): the character library's diagnostics.
// CATStatus_t is just a vtable; CATStdioStatus_t adds a FILE* (at +8) and an owns-it flag (+4), and its five virtual
// printers write "CatStatus:" / "CatWarning1" / "CatWarning2" / "CatError" / "CatDebug" lines to it. The FILE* is the
// caller's (the game passes one it opened with msvcr100), so the formatting and closing go through msvcr100's CRT.
#pragma once

#include "native/native.h"

namespace rnative::catstatus {

void Install(HMODULE orig);

}  // namespace rnative::catstatus
