#pragma once

#include <cstdint>

namespace inv {

// Phase 2.7/2.11/2.13/2.14 — live keyboard/mouse → physical axes.
// Prefer DirectInput8; fall back / hybrid with GetAsyncKeyState.
// Logical (virtual) axes come from Input.mapAxis / Controller.user_*.

void input_live_enable(bool on);
bool input_live_enabled();
bool input_di8_ready();        // keyboard device
bool input_di8_mouse_ready();  // mouse device
void input_live_shutdown();

// Last poll's relative mouse deltas (normalized-ish), from DI when ready.
void input_mouse_rel(float* dx, float* dy, float* dz);

// Sample keyboard/mouse into input_set_axis / input_set_last_key.
// Device 0: DIK scan codes (RCDIK_*). Device 1: mouse phys 0..4.
void input_live_poll();

// PE Input_lastKeyEvent @ 0x00556E00 — DI8 Acquire+GetDeviceData key-down;
// return DIK scan | (ToAsciiEx ascii << 16), or 0.
int32_t input_last_key_event();

// PE Engine_WndProc @ 0x004B8000 LABEL_34: WM_MOUSEMOVE/LBUTTON{DOWN,UP}/
// RBUTTON{DOWN,UP} → NDC 2*(px/w)-1, 2*(py/h)-1 (Windows Y, top=-1).
void input_wndproc_mouse(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp);
bool input_syscursor_ndc(float* x, float* y);
void input_syscursor_set_ndc(float x, float y);
uint32_t input_syscursor_buttons();
void input_syscursor_set_buttons(uint32_t mk);
// PE Engine_SysCursorLock @ 0x004B7C50 / Unlock @ 0x004B7C90.
void input_syscursor_lock();
void input_syscursor_unlock();
bool input_syscursor_locked();

// Virtual axis ids (mirror Input.java) — use with user_GetAxisVal / mapAxis.
constexpr int32_t kAxisMoveLR = 4;
constexpr int32_t kAxisMoveUD = 5;
constexpr int32_t kAxisMoveFB = 6;
constexpr int32_t kAxisTurnLR = 1;  // AXIS_TURN_LEFTRIGHT
constexpr int32_t kAxisThrottle = 28;
constexpr int32_t kAxisBrake = 29;
constexpr int32_t kAxisNitro = 31;      // Input.AXIS_NITRO
constexpr int32_t kAxisSelect = 34;
constexpr int32_t kAxisCancel = 35;
constexpr int32_t kAxisCursorX = 42;
constexpr int32_t kAxisCursorY = 43;
constexpr int32_t kAxisCursorZ = 44;
constexpr int32_t kAxisCursorBtn1 = 45;
constexpr int32_t kAxisCursorBtn2 = 46;
constexpr int32_t kAxisCursorBtn3 = 47;
constexpr int32_t kAxisHandbrake = 48;  // Input.AXIS_HANDBRAKE
constexpr int32_t kAxisClutch = 49;     // Input.AXIS_CLUTCH
constexpr int32_t kAxisGearUpDown = 50; // Input.AXIS_GEAR_UPDOWN
constexpr int32_t kAxisMenuUp = 55;
constexpr int32_t kAxisMenuDown = 56;
constexpr int32_t kAxisMenuLeft = 57;
constexpr int32_t kAxisMenuRight = 58;

// Physical mouse axis ids (ControlSet.defaults MOUSE column).
constexpr int32_t kMousePhysX = 0;
constexpr int32_t kMousePhysY = 1;
constexpr int32_t kMousePhysWheel = 2;
constexpr int32_t kMousePhysBtn1 = 3;
constexpr int32_t kMousePhysBtn2 = 4;

// Common DIK (mirror Input.RCDIK_* / dinput.h).
constexpr int32_t kDikReturn = 0x1C;
constexpr int32_t kDikEscape = 0x01;
constexpr int32_t kDikSpace = 0x39;
constexpr int32_t kDikLeft = 0xCB;
constexpr int32_t kDikRight = 0xCD;
constexpr int32_t kDikUp = 0xC8;
constexpr int32_t kDikDown = 0xD0;

}  // namespace inv
