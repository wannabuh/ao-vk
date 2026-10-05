// A test of tools/port-status.py's reachability (gone_trap.cpp): RANDYVK_GONE_TRAP=<file of rvas> puts a breakpoint at
// the start of each function it calls unreachable; one that runs anyway is logged ("gone function ran"), once.
#pragma once

#include "native/native.h"

namespace rnative::gonetrap {

void Install(HMODULE orig);

}  // namespace rnative::gonetrap
