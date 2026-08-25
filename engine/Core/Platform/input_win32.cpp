#include "render_d3d9.hpp"
#include "host_objects.hpp"
#include "input_win32.hpp"

#include <cstdio>
#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#endif

namespace inv {
namespace {

bool g_live = false;

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

  g_mouse_rel_x = 0.f;
  g_mouse_rel_y = 0.f;
  g_mouse_rel_z = 0.f;
  bool btn1 = key_down_vk(VK_LBUTTON);
  bool btn2 = key_down_vk(VK_RBUTTON);

  DIMOUSESTATE2 mst{};
  if (poll_di_mouse(&mst)) {
    // Normalize relative motion roughly to [-1,1] per poll burst.
    g_mouse_rel_x = static_cast<float>(mst.lX) / 64.f;
    g_mouse_rel_y = -static_cast<float>(mst.lY) / 64.f;  // DI Y down-positive
    g_mouse_rel_z = static_cast<float>(mst.lZ) / 120.f;  // wheel notches
    if (g_mouse_rel_x > 1.f) g_mouse_rel_x = 1.f;
    if (g_mouse_rel_x < -1.f) g_mouse_rel_x = -1.f;
    if (g_mouse_rel_y > 1.f) g_mouse_rel_y = 1.f;
    if (g_mouse_rel_y < -1.f) g_mouse_rel_y = -1.f;
    if (g_mouse_rel_z > 1.f) g_mouse_rel_z = 1.f;
    if (g_mouse_rel_z < -1.f) g_mouse_rel_z = -1.f;
    btn1 = btn1 || (mst.rgbButtons[0] & 0x80) != 0;
    btn2 = btn2 || (mst.rgbButtons[1] & 0x80) != 0;
  }

  // ControlSet.defaults: phys 0=X 1=Y 2=wheel 3=btn1 4=btn2.
  input_set_axis(1, kMousePhysX, cx);
  input_set_axis(1, kMousePhysY, cy);
  input_set_axis(1, kMousePhysWheel, g_mouse_rel_z);
  input_set_axis(1, kMousePhysBtn1, btn1 ? 1.f : 0.f);
  input_set_axis(1, kMousePhysBtn2, btn2 ? 1.f : 0.f);
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
  }
#endif
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
#endif
  g_live = false;
}

void input_live_poll() {
  if (!g_live) return;
#ifdef _WIN32
  BYTE keys[256]{};
  const bool di = poll_di_keyboard(keys);
  // Hybrid: DI8 is the stock path; OR with GetAsyncKeyState so injected
  // keys (keybd_event / SendInput) still register in smokes and overlays.
  poll_physical_keyboard(keys, di);

  bool di_key = false;
  if (di) {
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

  poll_mouse_axes();
  java_io_MouseCursor_tickSysCursor();
#else
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
