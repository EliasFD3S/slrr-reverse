#include "render_d3d9.hpp"
#include "host_objects.hpp"
#include "input_win32.hpp"

#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#endif

namespace inv {
namespace {

bool g_live = false;

// Soft PE mirrors of Input_* globals @ 0x007686E4..0x007686F8
// (Input_ResetGlobals @ 0x0054DD50).
float g_input_frame_dt_ms = 0.f;     // Input_frameDtMs @ 0x007686F4
float g_input_poll_dt_ms = 0.f;      // Input_pollDtMs @ 0x007686E4
float g_input_time_scale = 1.f;      // Input_timeScale @ 0x007686E8
uint32_t g_input_logical_eval_stamp = 0;  // Input_logicalEvalStamp @ 0x007686EC
uint32_t g_input_device_count = 0;   // Input_deviceCount @ 0x007686F0

// Soft PE Controller/device blob for Input_DeviceList_Push + tickAxes.
// PE next @ +0x2144; Player embeds at player+0x1C (malloc 0x2510 @ 47981A) —
// full Player ctor/dtor OOS. Host owns standalone 0x2148 blobs.
constexpr size_t kInputDeviceBlobSize = 0x2148;
constexpr size_t kInputDeviceAxisCount = 76;
constexpr size_t kInputDeviceAxisStride = 0x30;
constexpr size_t kInputDeviceFeedbackCount = 152;
constexpr size_t kInputDeviceFeedbackStride = 0x20;
constexpr size_t kInputDeviceNextOff = 0x2144;
constexpr size_t kInputDeviceAxesOff = 0x04;
constexpr size_t kInputDeviceFeedbackOff = 0xE44;
// PE Input_k1000f @ 0x005F0910 (Input_tick fmul); IDB u32 0x447A0000 = 1000.f.
constexpr uint32_t kInputK1000Bits = 0x447A0000u;
// PE Input_ffbStrengthEmulated @ 0x0061AB14 — mutable (.data); Engine_ApplyConfigGfx
// writes Config FFB_strength_emulated * 1e-6 @ 0x00428091. Soft keeps IDB default
// until Config apply is hosted (OOS). IDB u32 0x35C9539C.
constexpr uint32_t kFlt61AB14Bits = 0x35C9539Cu;
// PE Input_axisDampScale @ 0x0061AB18 — IDB u32 0x3C23D70A = 0.01f.
constexpr uint32_t kFlt61AB18Bits = 0x3C23D70Au;
constexpr uint32_t kFlt0_1Bits = 0x3DCCCCCDu;  // initAxesBlob +0x0C
constexpr uint32_t kFlt1_0Bits = 0x3F800000u;

struct InputDeviceBlobSoft {
  alignas(4) uint8_t b[kInputDeviceBlobSize]{};
};

float bits_f32(uint32_t bits) {
  float v = 0.f;
  std::memcpy(&v, &bits, sizeof(v));
  return v;
}

// Soft mirrors of PE .data read by Input_Device_tickAxes @ 0x0054DBE0.
// Not cleared by Input_ResetGlobals (PE leaves them to Config).
float g_input_ffb_strength_emulated = bits_f32(kFlt61AB14Bits);  // @ 0x0061AB14
float g_input_axis_damp_scale = bits_f32(kFlt61AB18Bits);        // @ 0x0061AB18

float& device_f32(InputDeviceBlobSoft* d, size_t off) {
  return *reinterpret_cast<float*>(d->b + off);
}

int32_t& device_i32(InputDeviceBlobSoft* d, size_t off) {
  return *reinterpret_cast<int32_t*>(d->b + off);
}

InputDeviceBlobSoft*& device_next(InputDeviceBlobSoft* d) {
  return *reinterpret_cast<InputDeviceBlobSoft**>(d->b + kInputDeviceNextOff);
}

std::vector<std::unique_ptr<InputDeviceBlobSoft>> g_input_device_owned;
InputDeviceBlobSoft* g_input_device_list = nullptr;  // @ 0x007686F8

void input_device_list_ensure_soft();

#ifdef _WIN32
LARGE_INTEGER g_input_qpc_freq{};   // Input_qpcFrequency @ 0x0076F9A8
LARGE_INTEGER g_input_qpc_last{};   // Input_qpcLast @ 0x00777440
bool g_input_qpc_inited = false;
float g_input_last_frame_dt_sec = 0.f;
float g_input_poll_qpc_dt_ms = 0.f;  // Input_qpcDtMs @ 0x0076F9A0
#endif

// Soft PE DirectInput device table @ 0x0076F9B0 (Input_diDevices) /
// count @ 0x00777434 (Input_diDeviceCount). Slot stride 490 dwords (0x7A8).
// Soft only registers SysKeyboard (type 1) + SysMouse (type 2); joy type 3 OOS.
constexpr int kInputDiMaxSoft = 2;
constexpr int kInputDiStateDwords = 68;  // joy GetDeviceState 272; kb 256 B
constexpr int kInputDiAxisFloatMax = 16;
constexpr int kInputMouseAxisCountSoft = 10;  // PE initDevices mouse n_axes
// PE Input_kMouseAccumScale @ 0x005F3AC8 — IDB u32 0x3C800000 = 0.015625f (1/64).
constexpr float kInputMouseAccumScale = 0.015625f;
constexpr float kInputMouseAbsScale = 0.005f;       // PE raw/qpcDtMs * 0.005
constexpr float kInputMouseSensStep = 0.01f;        // PE (-1-axis)*0.01
// PE button physId window: v13 >= 0x1000 && v13 < i[13]+4096.
constexpr uint32_t kInputMouseBtnPhysBase = 4096u;  // 0x1000
// PE mouse absolute physIds from initDevices @ 0x00556150.
constexpr uint32_t kInputMousePhysAbsX = 32770u;  // 0x8002
constexpr uint32_t kInputMousePhysAbsY = 32772u;  // 0x8004
constexpr uint32_t kInputMousePhysAbsZ = 32776u;  // 0x8008
// PE DIERR_INPUTLOST @ pollDevices joy Acquire retry @ 0x00556C5C.
constexpr long kDiErrInputLost = static_cast<long>(0x8007001EL);

struct InputDiSlotSoft {
#ifdef _WIN32
  IDirectInputDevice8A* device = nullptr;  // PE i[0]
  IDirectInputEffect* ffb_fx[3]{};         // PE i[486..488] @ +0x798/+0x79C/+0x7A0
#endif
  int32_t type = 0;         // i[12] — 1=kb, 2=mouse, 3=joy
  int32_t button_base = 0;  // i[13] — mouse next-button counter (ends at 4)
  int32_t n_axes = 0;       // i[14]
  int32_t state[kInputDiStateDwords]{};  // i[401..] GetDeviceState buffer
  float axis_f[kInputDiAxisFloatMax]{};  // i[17..] smoothed floats
  int32_t axis_obj[kInputDiAxisFloatMax]{};  // i[81..] physId / obj type
};

int32_t g_input_di_count = 0;  // Input_diDeviceCount @ 0x00777434
float g_input_mouse_axis_scale = 0.5f;  // Input_mouseSensScale @ 0x00777438
// Soft PE Input_forceFeedbackEnabled @ 0x00777448 (IDB default 0).
int32_t g_input_force_feedback_enabled = 0;
InputDiSlotSoft g_input_di_slots[kInputDiMaxSoft]{};

void input_di_table_reset_soft() {
  g_input_di_count = 0;
  g_input_mouse_axis_scale = 0.5f;
  g_input_force_feedback_enabled = 0;
  for (int i = 0; i < kInputDiMaxSoft; ++i) g_input_di_slots[i] = InputDiSlotSoft{};
}

void input_di_add_named_axis_soft(InputDiSlotSoft& slot, int32_t phys_id) {
  // Soft Input_DiDevice_addNamedAxis @ 0x00556550 — name strcpy OOS.
  if (slot.n_axes < 0 || slot.n_axes >= kInputDiAxisFloatMax) return;
  slot.axis_obj[slot.n_axes] = phys_id;
  slot.axis_f[slot.n_axes] = 0.f;
  ++slot.n_axes;
}

#ifdef _WIN32
void input_di_bind_devices_soft();  // after g_di_kb / g_di_mouse
#endif

// Soft stand-in for keyboard+mouse registration inside Input_initDevices
// @ 0x00556150 (joy EnumDevices OOS).
void input_di_table_ensure_soft() {
  if (g_input_di_count > 0) {
#ifdef _WIN32
    input_di_bind_devices_soft();
#endif
    return;
  }
  InputDiSlotSoft& kb = g_input_di_slots[0];
  kb = InputDiSlotSoft{};
  kb.type = 1;
  kb.n_axes = 256;  // PE Input_diDeviceAxisCount @ 0x0076F9E8

  InputDiSlotSoft& mouse = g_input_di_slots[1];
  mouse = InputDiSlotSoft{};
  mouse.type = 2;
  // PE order: abs X/Y/Z (0x8002/4/8) → M.Button1..4 → relative 2/4/8.
  mouse.axis_obj[0] = static_cast<int32_t>(kInputMousePhysAbsX);
  mouse.axis_obj[1] = static_cast<int32_t>(kInputMousePhysAbsY);
  mouse.axis_obj[2] = static_cast<int32_t>(kInputMousePhysAbsZ);
  mouse.n_axes = 3;
  for (int b = 0; b < 4; ++b) {
    const int32_t v = mouse.button_base;
    mouse.button_base = v + 1;
    input_di_add_named_axis_soft(mouse, v + static_cast<int32_t>(kInputMouseBtnPhysBase));
  }
  input_di_add_named_axis_soft(mouse, 2);
  input_di_add_named_axis_soft(mouse, 4);
  input_di_add_named_axis_soft(mouse, 8);
  g_input_di_count = 2;
  g_input_mouse_axis_scale = 0.5f;  // PE Input_mouseSensScale @ 0x00777438
#ifdef _WIN32
  input_di_bind_devices_soft();
#endif
}

// PE flt_64959C / flt_649598 — WndProc NDC (SysCursor path).
bool g_syscursor_has = false;
float g_syscursor_nx = 0.f;
float g_syscursor_ny = 0.f;
int32_t g_syscursor_px = 0;
int32_t g_syscursor_py = 0;
uint32_t g_syscursor_mk = 0;
bool g_syscursor_locked = false;

#ifdef _WIN32
IDirectInput8A* g_di = nullptr;
IDirectInputDevice8A* g_di_kb = nullptr;
IDirectInputDevice8A* g_di_mouse = nullptr;
bool g_di_tried = false;
bool g_di_kb_ok = false;
bool g_di_mouse_ok = false;
float g_mouse_rel_x = 0.f;
float g_mouse_rel_y = 0.f;
float g_mouse_rel_z = 0.f;

// Soft PE i[0] bind — Input_initDevices stores CreateDevice ptrs in table slots.
void input_di_bind_devices_soft() {
  if (g_input_di_count <= 0) return;
  if (g_input_di_slots[0].type == 1) g_input_di_slots[0].device = g_di_kb;
  if (g_input_di_count > 1 && g_input_di_slots[1].type == 2)
    g_input_di_slots[1].device = g_di_mouse;
}

void di8_shutdown() {
  if (g_di_mouse) {
    g_di_mouse->Unacquire();
    g_di_mouse->Release();
    g_di_mouse = nullptr;
  }
  if (g_di_kb) {
    g_di_kb->Unacquire();
    g_di_kb->Release();
    g_di_kb = nullptr;
  }
  if (g_di) {
    g_di->Release();
    g_di = nullptr;
  }
  g_di_kb_ok = false;
  g_di_mouse_ok = false;
  // Clear soft table device ptrs (PE releases via initDevices teardown OOS).
  for (int i = 0; i < kInputDiMaxSoft; ++i) {
    g_input_di_slots[i].device = nullptr;
    g_input_di_slots[i].ffb_fx[0] = g_input_di_slots[i].ffb_fx[1] =
        g_input_di_slots[i].ffb_fx[2] = nullptr;
  }
}

HWND di_hwnd() {
  HWND hwnd = static_cast<HWND>(render_d3d9_hwnd());
  return hwnd ? hwnd : GetDesktopWindow();
}

bool di8_init_keyboard() {
  if (g_di_kb_ok) return true;
  if (!g_di) return false;

  HRESULT hr = g_di->CreateDevice(GUID_SysKeyboard, &g_di_kb, nullptr);
  if (FAILED(hr) || !g_di_kb) {
    std::fprintf(stderr, "[input] CreateDevice(keyboard) failed hr=0x%08lX\n",
                 static_cast<unsigned long>(hr));
    if (g_di_kb) {
      g_di_kb->Release();
      g_di_kb = nullptr;
    }
    return false;
  }
  hr = g_di_kb->SetDataFormat(&c_dfDIKeyboard);
  if (FAILED(hr)) {
    g_di_kb->Release();
    g_di_kb = nullptr;
    return false;
  }
  hr = g_di_kb->SetCooperativeLevel(di_hwnd(),
                                    DISCL_BACKGROUND | DISCL_NONEXCLUSIVE);
  if (FAILED(hr)) {
    g_di_kb->Release();
    g_di_kb = nullptr;
    return false;
  }
  g_di_kb->Acquire();
  g_di_kb_ok = true;
  std::printf("[input] DirectInput8 keyboard ready\n");
  return true;
}

bool di8_init_mouse() {
  if (g_di_mouse_ok) return true;
  if (!g_di) return false;

  HRESULT hr = g_di->CreateDevice(GUID_SysMouse, &g_di_mouse, nullptr);
  if (FAILED(hr) || !g_di_mouse) {
    std::fprintf(stderr, "[input] CreateDevice(mouse) failed hr=0x%08lX\n",
                 static_cast<unsigned long>(hr));
    if (g_di_mouse) {
      g_di_mouse->Release();
      g_di_mouse = nullptr;
    }
    return false;
  }
  hr = g_di_mouse->SetDataFormat(&c_dfDIMouse2);
  if (FAILED(hr)) {
    // Older format fallback.
    hr = g_di_mouse->SetDataFormat(&c_dfDIMouse);
  }
  if (FAILED(hr)) {
    g_di_mouse->Release();
    g_di_mouse = nullptr;
    return false;
  }
  hr = g_di_mouse->SetCooperativeLevel(di_hwnd(),
                                       DISCL_BACKGROUND | DISCL_NONEXCLUSIVE);
  if (FAILED(hr)) {
    g_di_mouse->Release();
    g_di_mouse = nullptr;
    return false;
  }
  g_di_mouse->Acquire();
  g_di_mouse_ok = true;
  std::printf("[input] DirectInput8 mouse ready\n");
  return true;
}

bool di8_init() {
  if (g_di_kb_ok && g_di_mouse_ok) return true;
  if (g_di_tried && !g_di) return false;
  g_di_tried = true;

  if (!g_di) {
    HRESULT hr =
        DirectInput8Create(GetModuleHandleA(nullptr), DIRECTINPUT_VERSION,
                           IID_IDirectInput8A, reinterpret_cast<void**>(&g_di),
                           nullptr);
    if (FAILED(hr) || !g_di) {
      std::fprintf(stderr, "[input] DirectInput8Create failed hr=0x%08lX\n",
                   static_cast<unsigned long>(hr));
      di8_shutdown();
      return false;
    }
  }

  di8_init_keyboard();
  di8_init_mouse();
  return g_di_kb_ok;
}

bool dik_down(const BYTE keys[256], int dik) {
  return (keys[dik] & 0x80) != 0;
}

bool poll_di_keyboard(BYTE keys[256]) {
  if (!di8_init() || !g_di_kb) return false;
  HRESULT hr = g_di_kb->GetDeviceState(256, keys);
  if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED) {
    g_di_kb->Acquire();
    hr = g_di_kb->GetDeviceState(256, keys);
  }
  return SUCCEEDED(hr);
}

bool poll_di_mouse(DIMOUSESTATE2* st) {
  if (!g_di || !g_di_mouse_ok || !g_di_mouse || !st) return false;
  std::memset(st, 0, sizeof(*st));
  HRESULT hr = g_di_mouse->GetDeviceState(sizeof(DIMOUSESTATE2), st);
  if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED) {
    g_di_mouse->Acquire();
    hr = g_di_mouse->GetDeviceState(sizeof(DIMOUSESTATE2), st);
  }
  if (FAILED(hr)) {
    // Maybe device was created with c_dfDIMouse (smaller state).
    DIMOUSESTATE st1{};
    hr = g_di_mouse->GetDeviceState(sizeof(DIMOUSESTATE), &st1);
    if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED) {
      g_di_mouse->Acquire();
      hr = g_di_mouse->GetDeviceState(sizeof(DIMOUSESTATE), &st1);
    }
    if (FAILED(hr)) return false;
    st->lX = st1.lX;
    st->lY = st1.lY;
    st->lZ = st1.lZ;
    std::memcpy(st->rgbButtons, st1.rgbButtons, 4);
    return true;
  }
  return true;
}

bool key_down_vk(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

int32_t vk_to_dik(int vk) {
  switch (vk) {
    case VK_ESCAPE:
      return DIK_ESCAPE;
    case '1':
      return DIK_1;
    case '2':
      return DIK_2;
    case VK_RETURN:
      return DIK_RETURN;
    case VK_SPACE:
      return DIK_SPACE;
    case VK_LEFT:
      return DIK_LEFT;
    case VK_RIGHT:
      return DIK_RIGHT;
    case VK_UP:
      return DIK_UP;
    case VK_DOWN:
      return DIK_DOWN;
    case 'A':
      return DIK_A;
    case 'B':
      return DIK_B;
    case 'C':
      return DIK_C;
    case 'D':
      return DIK_D;
    case 'E':
      return DIK_E;
    case 'F':
      return DIK_F;
    case 'G':
      return DIK_G;
    case 'H':
      return DIK_H;
    case 'I':
      return DIK_I;
    case 'J':
      return DIK_J;
    case 'K':
      return DIK_K;
    case 'L':
      return DIK_L;
    case 'M':
      return DIK_M;
    case 'N':
      return DIK_N;
    case 'O':
      return DIK_O;
    case 'P':
      return DIK_P;
    case 'Q':
      return DIK_Q;
    case 'R':
      return DIK_R;
    case 'S':
      return DIK_S;
    case 'T':
      return DIK_T;
    case 'U':
      return DIK_U;
    case 'V':
      return DIK_V;
    case 'W':
      return DIK_W;
    case 'X':
      return DIK_X;
    case 'Y':
      return DIK_Y;
    case 'Z':
      return DIK_Z;
    default:
      return vk & 0xFF;
  }
}

int32_t di8_last_key_event() {
  // PE Input_lastKeyEvent @ 0x00556E00
  if (!di8_init() || !g_di_kb) return 0;
  HRESULT hr = g_di_kb->Acquire();
  if (FAILED(hr)) return 0;

  DIDEVICEOBJECTDATA ev{};
  DWORD count = 1;
  hr = g_di_kb->GetDeviceData(sizeof(DIDEVICEOBJECTDATA), &ev, &count, 0);
  if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED) {
    g_di_kb->Acquire();
    hr = g_di_kb->GetDeviceData(sizeof(DIDEVICEOBJECTDATA), &ev, &count, 0);
  }
  if (FAILED(hr) || count != 1) return 0;

  const unsigned uCode = ev.dwOfs;
  const signed char key_down = static_cast<signed char>(ev.dwData & 0xFF);
  if (uCode >= 0x100 || key_down >= 0) return 0;

  WORD ch[2]{};
  const HKL layout = GetKeyboardLayout(0);
  const UINT vk = MapVirtualKeyExA(uCode, MAPVK_VSC_TO_VK_EX, layout);
  BYTE ks[256]{};
  if (GetKeyboardState(ks))
    ToAsciiEx(vk, uCode, ks, ch, 0, layout);
  return static_cast<int32_t>(uCode |
                              (static_cast<unsigned>(ch[0]) << 16));
}

void poll_last_key_vk() {
  static const int kScan[] = {
      VK_ESCAPE, VK_RETURN, VK_SPACE, VK_LEFT, VK_RIGHT, VK_UP, VK_DOWN,
      'A',       'B',       'C',      'D',     'E',      'F',   'G',
      'H',       'I',       'J',      'K',     'L',      'M',   'N',
      'O',       'P',       'Q',      'R',     'S',      'T',   'U',
      'V',       'W',       'X',      'Y',     'Z',      '1',   '2'};
  for (int vk : kScan) {
    if (key_down_vk(vk)) {
      input_set_last_key(vk_to_dik(vk), false);
      return;
    }
  }
  input_set_last_key(0, false);
}

void poll_physical_keyboard(const BYTE* keys, bool di) {
  // Physical axis index = DIK. Collides with virtual AXIS_* ids, so getAxis
  // is physical-only; logical values come from mapAxis / user_GetAxisVal.
  struct Pair {
    int dik;
    int vk;
  };
  static const Pair kKeys[] = {
      {DIK_ESCAPE, VK_ESCAPE}, {DIK_RETURN, VK_RETURN},
      {DIK_SPACE, VK_SPACE},   {DIK_LEFT, VK_LEFT},
      {DIK_RIGHT, VK_RIGHT},   {DIK_UP, VK_UP},
      {DIK_DOWN, VK_DOWN},     {DIK_A, 'A'},
      {DIK_B, 'B'},            {DIK_C, 'C'},
      {DIK_D, 'D'},            {DIK_E, 'E'},
      {DIK_F, 'F'},            {DIK_G, 'G'},
      {DIK_H, 'H'},            {DIK_I, 'I'},
      {DIK_J, 'J'},            {DIK_K, 'K'},
      {DIK_L, 'L'},            {DIK_M, 'M'},
      {DIK_N, 'N'},            {DIK_O, 'O'},
      {DIK_P, 'P'},            {DIK_Q, 'Q'},
      {DIK_R, 'R'},            {DIK_S, 'S'},
      {DIK_T, 'T'},            {DIK_U, 'U'},
      {DIK_V, 'V'},            {DIK_W, 'W'},
      {DIK_X, 'X'},            {DIK_Y, 'Y'},
      {DIK_Z, 'Z'},            {DIK_1, '1'},
      {DIK_2, '2'},            {DIK_F1, VK_F1},
      {DIK_F2, VK_F2},         {DIK_F5, VK_F5},
      {DIK_F7, VK_F7},         {DIK_F8, VK_F8},
      {DIK_F12, VK_F12},       {DIK_COMMA, VK_OEM_COMMA},
      {DIK_PERIOD, VK_OEM_PERIOD},
      {DIK_NUMPADENTER, VK_RETURN},
      {DIK_NUMPADPLUS, VK_ADD},
      {DIK_NUMPADMINUS, VK_SUBTRACT},
      {DIK_PGUP, VK_PRIOR},
      {DIK_PGDN, VK_NEXT},
  };
  for (const Pair& p : kKeys) {
    const bool pressed =
        (di && dik_down(keys, p.dik)) || key_down_vk(p.vk);
    input_set_axis(0, p.dik, pressed ? 1.f : 0.f);
  }
}

void poll_mouse_axes() {
  // Absolute cursor for OSD (Win32). Relative deltas from DI when available.
  // Map through the render HWND client rect so hit-tests match the backbuffer
  // (screen-normalized coords drift when the window is not fullscreen).
  POINT pt{};
  GetCursorPos(&pt);
  float cx = 0.f;
  float cy = 0.f;
  HWND hwnd = static_cast<HWND>(render_d3d9_hwnd());
  if (hwnd && render_d3d9_ready() && render_d3d9_width() > 0 &&
      render_d3d9_height() > 0) {
    POINT client = pt;
    ScreenToClient(hwnd, &client);
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const float cw = static_cast<float>(rc.right - rc.left);
    const float ch = static_cast<float>(rc.bottom - rc.top);
    if (cw > 1.f && ch > 1.f) {
      cx = (static_cast<float>(client.x) / cw) * 2.f - 1.f;
      cy = 1.f - (static_cast<float>(client.y) / ch) * 2.f;
      if (cx < -1.f) cx = -1.f;
      if (cx > 1.f) cx = 1.f;
      if (cy < -1.f) cy = -1.f;
      if (cy > 1.f) cy = 1.f;
    }
  } else if (render_d3d9_ready() && render_d3d9_width() > 0) {
    cx = (static_cast<float>(pt.x) /
          static_cast<float>(GetSystemMetrics(SM_CXSCREEN))) *
             2.f -
         1.f;
    cy = 1.f - (static_cast<float>(pt.y) /
                static_cast<float>(GetSystemMetrics(SM_CYSCREEN))) *
                   2.f;
  }

  // Prefer soft DI mouse slot filled by Input_pollDevices (avoid double
  // GetDeviceState which zeros relative deltas on the second read).
  bool used_soft = false;
  for (int i = 0; i < g_input_di_count && i < kInputDiMaxSoft; ++i) {
    if (g_input_di_slots[i].type != 2) continue;
    used_soft = true;
    break;
  }
  if (!used_soft) {
    g_mouse_rel_x = 0.f;
    g_mouse_rel_y = 0.f;
    g_mouse_rel_z = 0.f;
    DIMOUSESTATE2 mst{};
    if (poll_di_mouse(&mst)) {
      g_mouse_rel_x = static_cast<float>(mst.lX) / 64.f;
      g_mouse_rel_y = -static_cast<float>(mst.lY) / 64.f;
      g_mouse_rel_z = static_cast<float>(mst.lZ) / 120.f;
      if (g_mouse_rel_x > 1.f) g_mouse_rel_x = 1.f;
      if (g_mouse_rel_x < -1.f) g_mouse_rel_x = -1.f;
      if (g_mouse_rel_y > 1.f) g_mouse_rel_y = 1.f;
      if (g_mouse_rel_y < -1.f) g_mouse_rel_y = -1.f;
      if (g_mouse_rel_z > 1.f) g_mouse_rel_z = 1.f;
      if (g_mouse_rel_z < -1.f) g_mouse_rel_z = -1.f;
    }
  }

  // Host overlay: phys 0/1 stay client NDC for OSD hit-tests (PE abs rates
  // are soft-read via input_read_physical_axis / g_axes 0..9 from publish).
  input_set_axis(1, kMousePhysX, cx);
  input_set_axis(1, kMousePhysY, cy);
}
#endif

}  // namespace

int32_t input_last_key_event() {
#ifdef _WIN32
  return di8_last_key_event();
#else
  return 0;
#endif
}

void input_live_enable(bool on) {
  g_live = on;
#ifdef _WIN32
  if (on) {
    if (!g_di) g_di_tried = false;
    di8_init();
    // PE sub_556150 / Input_initDevices @ 0x00556521: seed QPF/QPC before first pollDevices.
    QueryPerformanceFrequency(&g_input_qpc_freq);
    QueryPerformanceCounter(&g_input_qpc_last);
    g_input_qpc_inited = true;
    g_input_poll_qpc_dt_ms = 0.f;
  }
#endif
  // Soft stand-in for Player ctor Push @ 0x00479959 (device = player+0x1C).
  // Soft DI table stand-in for kb+mouse slots from sub_556150.
  if (on) {
    input_device_list_ensure_soft();
    input_di_table_ensure_soft();
  }
}

bool input_live_enabled() { return g_live; }

bool input_di8_ready() {
#ifdef _WIN32
  return g_di_kb_ok;
#else
  return false;
#endif
}

bool input_di8_mouse_ready() {
#ifdef _WIN32
  return g_di_mouse_ok;
#else
  return false;
#endif
}

void input_mouse_rel(float* dx, float* dy, float* dz) {
#ifdef _WIN32
  if (dx) *dx = g_mouse_rel_x;
  if (dy) *dy = g_mouse_rel_y;
  if (dz) *dz = g_mouse_rel_z;
#else
  if (dx) *dx = 0.f;
  if (dy) *dy = 0.f;
  if (dz) *dz = 0.f;
#endif
}

void input_live_shutdown() {
#ifdef _WIN32
  input_syscursor_unlock();
  di8_shutdown();
  g_di_tried = false;
  g_mouse_rel_x = g_mouse_rel_y = g_mouse_rel_z = 0.f;
  g_input_qpc_inited = false;
  g_input_last_frame_dt_sec = 0.f;
  g_input_poll_qpc_dt_ms = 0.f;
#endif
  // Soft Input_ResetGlobals @ 0x0054DD50 (full DI teardown OOS).
  g_input_frame_dt_ms = 0.f;
  g_input_poll_dt_ms = 0.f;
  g_input_time_scale = 1.f;
  g_input_logical_eval_stamp = 0;
  g_input_device_count = 0;
  g_input_device_list = nullptr;
  g_input_device_owned.clear();
  input_di_table_reset_soft();
  g_live = false;
}

namespace {

#ifdef _WIN32
// Soft PE Input_pollDevices keyboard case 1 @ 0x00556D98.
// GetDeviceState(256, i+401); on fail Acquire (vt+28); Acquire fail →
// memset 64 dwords; else retry GetDeviceState (PE does not check 2nd hr).
void input_di_poll_keyboard_slot(InputDiSlotSoft& slot) {
  IDirectInputDevice8A* dev = slot.device ? slot.device : g_di_kb;
  if (!dev) {
    std::memset(slot.state, 0, 256);
    return;
  }
  HRESULT hr = dev->GetDeviceState(256, slot.state);
  if (hr != 0) {
    if (dev->Acquire() != 0) {
      // PE LABEL_43: ecx=0x40 → rep stosd 64 dwords @ state.
      std::memset(slot.state, 0, 64 * sizeof(int32_t));
      return;
    }
    dev->GetDeviceState(256, slot.state);
  }
}

// Soft PE Input_pollDevices mouse case 2 @ 0x00556CD5.
// PE GetDeviceState size 16 (DIMOUSESTATE @ i+401); Soft may have created
// DIMouse2 — try 16 first (stock), then sizeof(DIMOUSESTATE2).
bool input_di_mouse_get_state_soft(IDirectInputDevice8A* dev,
                                   InputDiSlotSoft& slot) {
  if (!dev) return false;
  HRESULT hr = dev->GetDeviceState(16, slot.state);
  if (hr == 0) return true;
  if (dev->Acquire() == 0) {
    hr = dev->GetDeviceState(16, slot.state);
    if (hr == 0) return true;
  }
  // Soft DIMouse2 path (host init prefers c_dfDIMouse2).
  DIMOUSESTATE2 st2{};
  hr = dev->GetDeviceState(sizeof(DIMOUSESTATE2), &st2);
  if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED) {
    if (dev->Acquire() != 0) return false;
    hr = dev->GetDeviceState(sizeof(DIMOUSESTATE2), &st2);
  }
  if (hr != 0) return false;
  slot.state[0] = static_cast<int32_t>(st2.lX);
  slot.state[1] = static_cast<int32_t>(st2.lY);
  slot.state[2] = static_cast<int32_t>(st2.lZ);
  std::memcpy(&slot.state[3], st2.rgbButtons, 4);
  return true;
}

void input_di_poll_mouse_slot(InputDiSlotSoft& slot) {
  IDirectInputDevice8A* dev = slot.device ? slot.device : g_di_mouse;
  if (!input_di_mouse_get_state_soft(dev, slot)) {
    // PE @ 0x00556CFF: zero state dwords 401..404 (lX/lY/lZ/buttons).
    slot.state[0] = slot.state[1] = slot.state[2] = slot.state[3] = 0;
    g_mouse_rel_x = g_mouse_rel_y = g_mouse_rel_z = 0.f;
    return;
  }

  // PE @ 0x00556D11: for each axis, obj type 2/4/8 → accum * sens * 0.015625.
  for (int a = 0; a < slot.n_axes && a < kInputDiAxisFloatMax; ++a) {
    double raw = 0.0;
    bool accum = false;
    switch (slot.axis_obj[a]) {
      case 2:
        raw = static_cast<double>(slot.state[0]);
        accum = true;
        break;
      case 4:
        raw = static_cast<double>(slot.state[1]);
        accum = true;
        break;
      case 8:
        raw = static_cast<double>(slot.state[2]);
        accum = true;
        break;
      default:
        break;
    }
    if (accum) {
      float v = static_cast<float>(
          raw * static_cast<double>(g_input_mouse_axis_scale) *
              static_cast<double>(kInputMouseAccumScale) +
          static_cast<double>(slot.axis_f[a]));
      if (v > 1.f) v = 1.f;
      if (v < -1.f) v = -1.f;
      slot.axis_f[a] = v;
    } else {
      // PE LABEL_33: clamp only (abs/button floats stay).
      float v = slot.axis_f[a];
      if (v > 1.f) v = 1.f;
      if (v < -1.f) v = -1.f;
      slot.axis_f[a] = v;
    }
  }

  // Host relative burst (normalized) for overlays — PE keeps accum in table.
  g_mouse_rel_x = static_cast<float>(slot.state[0]) / 64.f;
  g_mouse_rel_y = -static_cast<float>(slot.state[1]) / 64.f;
  g_mouse_rel_z = static_cast<float>(slot.state[2]) / 120.f;
  if (g_mouse_rel_x > 1.f) g_mouse_rel_x = 1.f;
  if (g_mouse_rel_x < -1.f) g_mouse_rel_x = -1.f;
  if (g_mouse_rel_y > 1.f) g_mouse_rel_y = 1.f;
  if (g_mouse_rel_y < -1.f) g_mouse_rel_y = -1.f;
  if (g_mouse_rel_z > 1.f) g_mouse_rel_z = 1.f;
  if (g_mouse_rel_z < -1.f) g_mouse_rel_z = -1.f;
}

// Soft PE Input_pollDevices joy case 3 @ 0x00556C43.
// Poll → Acquire retry (≤10× DIERR_INPUTLOST) → optional FFB Start →
// GetDeviceState(272). Soft registers no type-3 slots (EnumDevices OOS).
void input_di_poll_joy_slot(InputDiSlotSoft& slot) {
  IDirectInputDevice8A* dev = slot.device;
  if (!dev) return;

  // PE: IDirectInputDevice8::Poll (vt+100 / +0x64).
  if (dev->Poll() < 0) {
    int tries = 1;
    HRESULT acq = 0;
    while (true) {
      acq = dev->Acquire();
      if (acq != kDiErrInputLost) break;
      if (tries++ > 10) {
        // PE ecx=0x44 → memset 68 dwords (272 B joy state).
        std::memset(slot.state, 0, 68 * sizeof(int32_t));
        return;
      }
    }
    if (acq < 0) return;  // PE @ 0x00556C77 break

    // PE @ 0x00556C7D: if Input_forceFeedbackEnabled → Effect::Start(1,0).
    if (g_input_force_feedback_enabled != 0) {
      for (IDirectInputEffect* fx : slot.ffb_fx) {
        if (fx) fx->Start(1, 0);
      }
    }
  }
  // PE @ 0x00556CC2: GetDeviceState(272, i+401) even when Poll succeeded.
  dev->GetDeviceState(272, slot.state);
}

// Soft PE Input_readPhysicalAxis @ 0x00557430 — type 1/2 only; joy OOS→0.
float input_read_physical_axis_soft(int device, int axis) {
  if (device < 0 || device >= g_input_di_count || device >= kInputDiMaxSoft)
    return 0.f;
  InputDiSlotSoft& slot = g_input_di_slots[device];
  if (slot.type == 1) {
    if (axis < 0 || axis >= slot.n_axes) return 0.f;
    const auto* keys = reinterpret_cast<const signed char*>(slot.state);
    return keys[axis & 0xFF] < 0 ? 1.f : 0.f;
  }
  if (slot.type != 2) {
    // type 3 joy / unknown — OOS soft returns 0 (PE has full joy path).
    return 0.f;
  }

  // PE: (unsigned)axis > 0xFFFFFF9A → axis in [-101,-1]: set sens, clear floats.
  if (static_cast<uint32_t>(axis) > 0xFFFFFF9Au) {
    g_input_mouse_axis_scale =
        static_cast<float>(-1 - axis) * kInputMouseSensStep;
    for (int i = 0; i < slot.n_axes && i < kInputDiAxisFloatMax; ++i)
      slot.axis_f[i] = 0.f;
    return 0.f;
  }
  if (axis < 0 || axis >= slot.n_axes || axis >= kInputDiAxisFloatMax)
    return 0.f;

  const uint32_t phys = static_cast<uint32_t>(slot.axis_obj[axis]);
  const uint32_t btn_hi =
      static_cast<uint32_t>(slot.button_base) + kInputMouseBtnPhysBase;
  if (phys >= kInputMouseBtnPhysBase && phys < btn_hi) {
    // PE: *((char*)slot + phys - 2480) with state @ +1604 → rgbButtons.
    const unsigned bi = phys - kInputMouseBtnPhysBase;
    if (bi >= 4u) return 0.f;
    const auto* btns = reinterpret_cast<const signed char*>(&slot.state[3]);
    return btns[bi] < 0 ? 1.f : 0.f;
  }

  float result = 0.f;
  if (phys == kInputMousePhysAbsX) {
    if (g_input_poll_qpc_dt_ms <= 0.f) return 0.f;
    result = static_cast<float>(slot.state[0]) / g_input_poll_qpc_dt_ms *
             kInputMouseAbsScale;
  } else if (phys == kInputMousePhysAbsY) {
    if (g_input_poll_qpc_dt_ms <= 0.f) return 0.f;
    result = static_cast<float>(slot.state[1]) / g_input_poll_qpc_dt_ms *
             kInputMouseAbsScale;
  } else if (phys == kInputMousePhysAbsZ) {
    if (g_input_poll_qpc_dt_ms <= 0.f) return 0.f;
    result = static_cast<float>(slot.state[2]) / g_input_poll_qpc_dt_ms *
             kInputMouseAbsScale;
  } else {
    // Relative physId 2/4/8 → smoothed float at axis_f[axis].
    const uint32_t r = phys - 2u;
    if (r != 0u) {
      const uint32_t r2 = r - 2u;
      if (r2 != 0u && r2 != 4u) return 0.f;
    }
    result = slot.axis_f[axis];
  }

  if (result > 1.f) return 1.f;
  if (result < -1.f) return -1.f;
  return result;
}
#endif  // _WIN32

#ifndef _WIN32
float input_read_physical_axis_soft(int /*device*/, int /*axis*/) { return 0.f; }
#endif

// Publish soft DI (+ hybrid VK) into host g_axes — PE readers use DI table.
void input_publish_phys_from_di_soft() {
#ifdef _WIN32
  BYTE keys[256]{};
  bool di = false;
  if (g_input_di_count > 0 && g_input_di_slots[0].type == 1) {
    std::memcpy(keys, g_input_di_slots[0].state, 256);
    di = g_di_kb_ok;
  }
  // Hybrid VK for smoke keys + soft DI bytes for full 0..255 phys.
  poll_physical_keyboard(keys, di);
  if (di) {
    for (int dik = 0; dik < 256; ++dik) {
      if ((keys[dik] & 0x80) != 0) input_set_axis(0, dik, 1.f);
    }
  }

  // Soft PE mouse axes 0..9 via readPhysicalAxis; then NDC overlay on 0/1.
  for (int a = 0; a < kInputMouseAxisCountSoft; ++a) {
    float v = input_read_physical_axis_soft(1, a);
    // Hybrid VK buttons if DI mouse not acquired (soft state may be zero).
    if (a == 3 && key_down_vk(VK_LBUTTON)) v = 1.f;
    if (a == 4 && key_down_vk(VK_RBUTTON)) v = 1.f;
    if (a == 5 && key_down_vk(VK_MBUTTON)) v = 1.f;
    input_set_axis(1, a, v);
  }
  poll_mouse_axes();  // overlays phys 0/1 with client NDC for OSD
#endif
}
// Soft PE Input_pollDevices @ 0x00556BC0 size 0x23c.
// QPF+QPC → Input_qpcDtMs @ 0x0076F9A0; walk Input_diDevices stride 490
// dwords (Soft: kInputDiMaxSoft slots); cases 1/2/3 @ 0x00556C2F.
float input_poll_devices_soft() {
#ifdef _WIN32
  if (g_live) {
    di8_init();
    input_di_table_ensure_soft();  // also rebinds i[0] device ptrs
  }

  LARGE_INTEGER now{};
  // PE always QueryPerformanceFrequency each call (Input_qpcFrequency).
  QueryPerformanceFrequency(&g_input_qpc_freq);
  QueryPerformanceCounter(&now);
  // PE always subtracts Input_qpcLast (seeded by initDevices @ 0x00556521).
  // Soft zeros first sample only when QPC was never seeded.
  if (!g_input_qpc_inited) {
    g_input_qpc_last = now;
    g_input_qpc_inited = true;
    g_input_poll_qpc_dt_ms = 0.f;
  } else {
    const double ticks =
        static_cast<double>(now.QuadPart - g_input_qpc_last.QuadPart);
    g_input_qpc_last = now;
    if (g_input_qpc_freq.QuadPart > 0)
      g_input_poll_qpc_dt_ms = static_cast<float>(
          ticks * 1000.0 / static_cast<double>(g_input_qpc_freq.QuadPart));
    else
      g_input_poll_qpc_dt_ms = 0.f;
  }

  for (int i = 0; i < g_input_di_count && i < kInputDiMaxSoft; ++i) {
    InputDiSlotSoft& slot = g_input_di_slots[i];
    switch (slot.type) {
      case 1:
        input_di_poll_keyboard_slot(slot);
        break;
      case 2:
        input_di_poll_mouse_slot(slot);
        break;
      case 3:
        input_di_poll_joy_slot(slot);  // EnumDevices type-3 slots OOS
        break;
      default:
        break;
    }
  }

  // Host g_axes publish inside pollDevices so Input_tick order matches PE
  // (sample before tickAxes / script getAxis readers).
  if (g_live) input_publish_phys_from_di_soft();
  return g_input_poll_qpc_dt_ms;
#else
  return 0.f;
#endif
}

// Soft PE Input_Device_initAxesBlob @ 0x0054D580 — 76×0x30 defaults +
// 152×0x20 feedback axisIdx clear + next=0 + hdr=0.
void input_device_init_axes_blob_soft(InputDeviceBlobSoft* d) {
  if (!d) return;
  std::memset(d->b, 0, kInputDeviceBlobSize);
  const float f01 = bits_f32(kFlt0_1Bits);
  const float f10 = bits_f32(kFlt1_0Bits);
  for (size_t i = 0; i < kInputDeviceAxisCount; ++i) {
    const size_t base = kInputDeviceAxesOff + i * kInputDeviceAxisStride;
    // PE writes from dword this+1: val,flag,unk,0.1, five×1.0, target,rate,decay.
    device_f32(d, base + 0x00) = 0.f;
    device_i32(d, base + 0x04) = 0;
    device_i32(d, base + 0x08) = 0;
    device_f32(d, base + 0x0C) = f01;
    device_f32(d, base + 0x10) = f10;
    device_f32(d, base + 0x14) = f10;
    device_f32(d, base + 0x18) = f10;
    device_f32(d, base + 0x1C) = f10;
    device_f32(d, base + 0x20) = f10;
    device_f32(d, base + 0x24) = 0.f;
    device_f32(d, base + 0x28) = 0.f;
    device_f32(d, base + 0x2C) = 0.f;
  }
  // Feedback map: PE only zeros axisIdx dword every 0x20 (this+913).
  for (size_t j = 0; j < kInputDeviceFeedbackCount; ++j) {
    device_i32(d, kInputDeviceFeedbackOff + j * kInputDeviceFeedbackStride) = 0;
  }
  device_next(d) = nullptr;
  device_i32(d, 0) = 0;
}

// Soft PE Input_DeviceList_Push @ 0x0054D5E0.
int input_device_list_push_soft(InputDeviceBlobSoft* node) {
  if (!node) return 0;
  device_next(node) = g_input_device_list;
  ++g_input_device_count;
  g_input_device_list = node;
  return 1;
}

// Soft stand-in for Player ctor Push @ 0x00479959 (device = player+0x1C).
void input_device_list_ensure_soft() {
  if (g_input_device_list != nullptr) return;
  auto node = std::make_unique<InputDeviceBlobSoft>();
  input_device_init_axes_blob_soft(node.get());
  InputDeviceBlobSoft* raw = node.get();
  g_input_device_owned.push_back(std::move(node));
  input_device_list_push_soft(raw);
}

// Soft PE Input_DiDevice_getType @ 0x00558130 — table @ 0x76F9B0 type i[12].
int input_di_device_get_type_soft(int di_index) {
  if (di_index < 0 || di_index >= g_input_di_count) return 0;
  if (di_index >= kInputDiMaxSoft) return 0;
  return g_input_di_slots[di_index].type;
}

// Soft PE Input_DiDevice_setFeedback @ 0x00558260 — FFB path needs type==3.
int input_di_device_set_feedback_soft(int di_index, int /*mode*/, float /*force*/,
                                      float /*magnitude*/) {
  if (di_index < 0 || di_index >= g_input_di_count) return -1;
  // Soft g_input_force_feedback_enabled mirrors @ 0x00777448; joy FFB
  // Start lives in pollDevices case 3 @ 0x00556C7D (initEffects OOS).
  return 0;
}

// Soft PE Input_Device_tickAxes @ 0x0054DBE0 size 0x168.
// thiscall device*; sole caller Input_tick @ 0x0054DDD5.
void input_device_tick_axes_soft(InputDeviceBlobSoft* node) {
  if (!node) return;
  // PE loads Input_ffbStrengthEmulated@61AB14 + Input_axisDampScale@61AB18
  // + Input_timeScale@7686E8 + Input_frameDtMs@7686F4 each axis body.
  const float kSmooth = g_input_ffb_strength_emulated;
  const float kDecay = g_input_axis_damp_scale;
  const float ts = g_input_time_scale;
  const float dt = g_input_frame_dt_ms;

  // Pass 1 @ 0x54DBEC: ecx = device+0x28; 76 slots stride 0x30 (v2 += 12 floats).
  // Skip if axisIsDigital ([ecx-20h] / base+0x04) != 0.
  for (size_t i = 0; i < kInputDeviceAxisCount; ++i) {
    const size_t base = kInputDeviceAxesOff + i * kInputDeviceAxisStride;
    if (device_i32(node, base + 0x04) != 0) continue;  // PE [ecx-20h] flag

    float* cur = &device_f32(node, base + 0x00);       // [ecx-24h]
    const float target = device_f32(node, base + 0x24);  // [ecx]
    const float rate = device_f32(node, base + 0x28);    // [ecx+4]
    const float decay = device_f32(node, base + 0x2C);   // [ecx+8]

    float delta = 0.f;
    if (rate != 0.f) {
      delta = target - *cur;
      // step = delta * rate * timeScale * frameDtMs * ffbStrengthEmulated
      const float step = delta * rate * ts * dt * kSmooth;
      if ((step + *cur - target) * (*cur - target) >= 0.f) delta = step;
    }
    if (decay > 0.f) {
      float w = kDecay * decay * ts * dt;
      if (w >= 1.f) w = 1.f;
      delta *= (1.f - w);
    }
    float next = delta + *cur;
    if (next > 1.f)
      next = 1.f;
    else if (next < -1.f)
      next = -1.f;
    *cur = next;
  }

  // Pass 2 @ 0x54DCE4: 152×0x20 map @ +0xE44 (axisIdx,diIndex,mode,…);
  // PE v7 = this+914 floats (= +0xE48 = diIndex); *(v7-1)=axisIdx.
  // Only DiDevice type==3 → Input_DiDevice_setFeedback @ 0x00558260.
  for (size_t j = 0; j < kInputDeviceFeedbackCount; ++j) {
    const size_t fb = kInputDeviceFeedbackOff + j * kInputDeviceFeedbackStride;
    const int32_t axis_idx = device_i32(node, fb + 0x00);
    const int32_t di_index = device_i32(node, fb + 0x04);
    const int32_t mode = device_i32(node, fb + 0x08);
    if (mode >= 1) continue;  // PE: if (v7[1] < 1)
    if (axis_idx < 0 ||
        static_cast<size_t>(axis_idx) >= kInputDeviceAxisCount)
      continue;
    const size_t ab =
        kInputDeviceAxesOff + static_cast<size_t>(axis_idx) * kInputDeviceAxisStride;
    if (device_i32(node, ab + 0x04) == 0) continue;  // need digital axis
    if (input_di_device_get_type_soft(di_index) != 3) continue;
    // PE @ 0x54DD2E: setFeedback(di, mode, -target@+0x24, rate@+0x28).
    const float neg_target = -device_f32(node, ab + 0x24);
    const float rate = device_f32(node, ab + 0x28);
    (void)input_di_device_set_feedback_soft(di_index, mode, neg_target, rate);
  }
}

}  // namespace

void input_tick(float frame_dt_sec) {
  // PE Input_tick @ 0x0054DDA0 size 0x46 (19 insn).
  // Sole caller: Engine_MainLoop @ 0x00428AAD push Engine_frameDt @ 0x0063C534.
  // Disasm order (register-faithful):
  //   fld a1; fmul Input_k1000f@5F0910; fstp Input_frameDtMs@7686F4
  //   call Input_pollDevices@556BC0          ; result on FPU
  //   mov ecx, Input_logicalEvalStamp@7686EC ; load stamp while FPU holds pollDt
  //   fstp Input_pollDtMs@7686E4
  //   mov esi, Input_deviceListHead@7686F8
  //   inc ecx; test esi; mov stamp, ecx
  //   jz done; do { tickAxes(esi); esi=[esi+2144h] } while esi
  // Host pulse: input_live_poll (checkHotkeys adjacency) — MainLoop hook OOS.
  g_input_frame_dt_ms = frame_dt_sec * bits_f32(kInputK1000Bits);
  const float poll_dt = input_poll_devices_soft();  // Input_pollDevices @ 0x00556BC0
  uint32_t stamp = g_input_logical_eval_stamp;      // load before store pollDt (PE)
  g_input_poll_dt_ms = poll_dt;
  InputDeviceBlobSoft* head = g_input_device_list;  // @ 0x007686F8
  ++stamp;
  g_input_logical_eval_stamp = stamp;               // @ 0x007686EC
  // Walk uses cached head (PE: test esi after mov from head; next @ +0x2144).
  for (InputDeviceBlobSoft* n = head; n != nullptr; n = device_next(n)) {
    input_device_tick_axes_soft(n);  // Input_Device_tickAxes @ 0x0054DBE0
  }
}

float input_frame_dt_ms() { return g_input_frame_dt_ms; }
float input_poll_dt_ms() { return g_input_poll_dt_ms; }
float input_qpc_dt_ms() {
#ifdef _WIN32
  return g_input_poll_qpc_dt_ms;
#else
  return 0.f;
#endif
}
float input_mouse_sens_scale() { return g_input_mouse_axis_scale; }
uint32_t input_logical_eval_stamp() { return g_input_logical_eval_stamp; }
uint32_t input_device_list_count() { return g_input_device_count; }

float input_read_physical_axis(int32_t device, int32_t axis) {
  return input_read_physical_axis_soft(device, axis);
}

void input_device_list_ensure() { input_device_list_ensure_soft(); }

void input_live_poll() {
  if (!g_live) return;
#ifdef _WIN32
  // Soft Input_tick first (PE MainLoop @ 0x00428AAD order): pollDevices fills
  // DI + g_axes, then ++logicalEvalStamp + tickAxes. lastKey / SysCursor are
  // adjacent host extras (not inside PE Input_tick body).
  input_tick(g_input_last_frame_dt_sec);
  g_input_last_frame_dt_sec = g_input_poll_dt_ms * 0.001f;

  bool di_key = false;
  if (g_di_kb_ok && g_input_di_count > 0 && g_input_di_slots[0].type == 1) {
    const BYTE* keys =
        reinterpret_cast<const BYTE*>(g_input_di_slots[0].state);
    static const int kScan[] = {
        DIK_ESCAPE, DIK_RETURN, DIK_SPACE, DIK_LEFT, DIK_RIGHT, DIK_UP, DIK_DOWN,
        DIK_A,      DIK_B,      DIK_C,     DIK_D,    DIK_E,     DIK_F,  DIK_G,
        DIK_H,      DIK_I,      DIK_J,     DIK_K,    DIK_L,     DIK_M,  DIK_N,
        DIK_O,      DIK_P,      DIK_Q,     DIK_R,    DIK_S,     DIK_T,  DIK_U,
        DIK_V,      DIK_W,      DIK_X,     DIK_Y,    DIK_Z,     DIK_1,  DIK_2};
    for (int dik : kScan) {
      if (dik_down(keys, dik)) {
        input_set_last_key(dik, false);
        di_key = true;
        break;
      }
    }
  }
  if (!di_key) poll_last_key_vk();

  java_io_MouseCursor_tickSysCursor();
#else
  input_tick(0.f);
  java_io_MouseCursor_tickSysCursor();
#endif
}

void input_wndproc_mouse(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp) {
#ifdef _WIN32
  // PE 0x004B8000 LABEL_34: 0x200-0x202, 0x204-0x205 only.
  if (msg != 0x200 && msg != 0x201 && msg != 0x202 && msg != 0x204 &&
      msg != 0x205)
    return;
  HWND h = static_cast<HWND>(hwnd);
  RECT rc{};
  if (!h || !GetClientRect(h, &rc) || rc.right <= 0 || rc.bottom <= 0) return;
  const unsigned px = static_cast<unsigned>(lp & 0xFFFFu);
  const unsigned py = static_cast<unsigned>((lp >> 16) & 0xFFFFu);
  g_syscursor_px = static_cast<int32_t>(px);
  g_syscursor_py = static_cast<int32_t>(py);
  g_syscursor_mk = static_cast<uint32_t>(wp) |
                   (g_syscursor_mk & 0xFFFF0000u);
  const double w = static_cast<double>(rc.right);
  const double hgt = static_cast<double>(rc.bottom);
  g_syscursor_nx = static_cast<float>(2.0 * (static_cast<double>(px) / w) - 1.0);
  g_syscursor_ny =
      static_cast<float>(2.0 * (static_cast<double>(py) / hgt) - 1.0);
  g_syscursor_has = true;
#else
  (void)hwnd;
  (void)msg;
  (void)wp;
  (void)lp;
#endif
}

bool input_syscursor_ndc(float* x, float* y) {
  if (!g_syscursor_has) return false;
  if (x) *x = g_syscursor_nx;
  if (y) *y = g_syscursor_ny;
  return true;
}

void input_syscursor_set_ndc(float x, float y) {
  g_syscursor_nx = x;
  g_syscursor_ny = y;
  g_syscursor_has = true;
}

uint32_t input_syscursor_buttons() { return g_syscursor_mk & 0xFFFFu; }

void input_syscursor_set_buttons(uint32_t mk) {
  g_syscursor_mk = (g_syscursor_mk & 0xFFFF0000u) | (mk & 0xFFFFu);
}

void input_syscursor_lock() {
#ifdef _WIN32
  // PE Engine_SysCursorLock @ 0x004B7C50: GetCursorPos → RECT {x,y,x,y}
  // (degenerate clip = pin pixel) → ClipCursor; Engine_SysCursorLocked=1.
  POINT pt{};
  GetCursorPos(&pt);
  RECT rc{};
  rc.left = pt.x;
  rc.top = pt.y;
  rc.right = pt.x;
  rc.bottom = pt.y;
  ClipCursor(&rc);
  g_syscursor_locked = true;
#else
  g_syscursor_locked = true;
#endif
}

void input_syscursor_unlock() {
#ifdef _WIN32
  // PE 0x004B7C90: flag=0; ClipCursor(NULL).
  g_syscursor_locked = false;
  ClipCursor(nullptr);
#else
  g_syscursor_locked = false;
#endif
}

bool input_syscursor_locked() { return g_syscursor_locked; }

}  // namespace inv
