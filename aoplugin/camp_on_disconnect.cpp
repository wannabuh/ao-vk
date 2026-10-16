// camp_on_disconnect.cpp - a lost server connection goes back to the login screen, as /camp does.
//
// How the client ends a session (GUI.dll, FlowControlModule_t):
//   - ActivateGameClosing(QuitStatus_e) does it. Status 2 is "to login": the user config saved, the screen cleared,
//     m_bIsReturningToLogin set - N3IsClosedMessage then stops short of closing the game. Any other status closes the
//     game; N3IsClosedMessage starts anarchy.exe (the launcher) with the status, and 3 / 4 send the disconnect report
//     (BugReport_t::ReportDisconnect) first.
//   - /camp to login (StartQuitToLoginMessage) only asks the server and sets m_eLoggingOutTimed = 2. The server drops
//     the connection when the camp is over, and ServerLostMessage passes m_eLoggingOutTimed on: ActivateGameClosing(2).
//   - With no camp running, ServerLostMessage passes 3 (connection lost), and ServerTimeoutMessage (also reached from
//     ClientDynelTimedOutMessage) passes 4 (timed out).
//
// So both of those pass 2 instead, and a disconnect takes the path a finished /camp takes. A camp to the system
// (m_eLoggingOutTimed 1, Alt+F4 / quit) still closes the game. The disconnect report is no longer sent.

#include "camp_on_disconnect.h"

#include <cstdint>

namespace aovk::camp {

namespace {

const AopHost* g_host;

// GUI.dll FlowControlModule_t::ServerLostMessage (10028d50):
//   10028d5c  83 3D C4 60 27 10 00   cmp dword ptr [m_eQuitStatus], 0
//   10028d63  75 08                  jne ret
//   10028d65  6A 03                  push 3              ; -> push 2
//   10028d67  E8 28 F4 FF FF         call ActivateGameClosing
constexpr uint32_t kServerLostRva = 0x28d63;
constexpr uint8_t kServerLostOriginal[] = {0x75, 0x08, 0x6A, 0x03, 0xE8, 0x28, 0xF4, 0xFF, 0xFF};
constexpr uint8_t kServerLostPatched[] = {0x75, 0x08, 0x6A, 0x02, 0xE8, 0x28, 0xF4, 0xFF, 0xFF};

// GUI.dll FlowControlModule_t::ServerTimeoutMessage (10028d6e):
//   10028d75  75 08                  jne ret
//   10028d77  6A 04                  push 4              ; -> push 2
//   10028d79  E8 16 F4 FF FF         call ActivateGameClosing
constexpr uint32_t kServerTimeoutRva = 0x28d75;
constexpr uint8_t kServerTimeoutOriginal[] = {0x75, 0x08, 0x6A, 0x04, 0xE8, 0x16, 0xF4, 0xFF, 0xFF};
constexpr uint8_t kServerTimeoutPatched[] = {0x75, 0x08, 0x6A, 0x02, 0xE8, 0x16, 0xF4, 0xFF, 0xFF};

bool Patch(uint32_t rva, const uint8_t* original, const uint8_t* patched, uint32_t length) {
    void* at = g_host->moduleRva(g_host, "GUI.dll", rva);
    return at && g_host->patch(g_host, at, original, patched, length) != 0;
}

}  // namespace

void Attach(const AopHost* host) { g_host = host; }

void GameReady() {
    const bool lost = Patch(kServerLostRva, kServerLostOriginal, kServerLostPatched, sizeof(kServerLostPatched));
    const bool timeout =
        Patch(kServerTimeoutRva, kServerTimeoutOriginal, kServerTimeoutPatched, sizeof(kServerTimeoutPatched));
    g_host->log(g_host, "disconnect: lost connection %s, timeout %s", lost ? "to login" : "stock (not patched)",
                timeout ? "to login" : "stock (not patched)");
}

}  // namespace aovk::camp
