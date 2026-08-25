#include "render_d3d9.hpp"
#include "host_objects.hpp"
#include "rpak.hpp"
#include "input_win32.hpp"
#include "video_fmv.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstddef>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <wincodec.h>
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#endif

namespace inv {
namespace {

#ifdef _WIN32
HWND g_hwnd = nullptr;
IDirect3D9* g_d3d = nullptr;
IDirect3DDevice9* g_dev = nullptr;
WNDCLASSEXA g_wc{};
bool g_class_reg = false;
HICON g_stock_icon = nullptr;
HCURSOR g_stock_cursors[16] = {};
int32_t g_stock_cursor_id = 2;
bool g_assets_ok = false;
bool g_quit_requested = false;

std::string module_dir() {
  char buf[MAX_PATH];
  const DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
  if (!n || n >= MAX_PATH) return {};
  std::string p(buf, n);
  const auto slash = p.find_last_of("\\/");
  if (slash == std::string::npos) return {};
  return p.substr(0, slash);
}

bool file_exists_a(const std::string& path) {
  const DWORD a = GetFileAttributesA(path.c_str());
  return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::string find_stock_assets_root() {
  // Prefer assets copied next to the exe (CMake POST_BUILD).
  const std::string exe = module_dir();
  auto try_root = [](const std::string& cand) -> std::string {
    if (cand.empty()) return {};
    if (file_exists_a(cand + "\\Icon\\0000\\1.ico") ||
        file_exists_a(cand + "/Icon/0000/1.ico"))
      return cand;
    return {};
  };
  if (!exe.empty()) {
    if (std::string r = try_root(exe + "\\assets\\StreetLegal_Redline_exe");
        !r.empty())
      return r;
    if (std::string r = try_root(exe + "/assets/StreetLegal_Redline_exe");
        !r.empty())
      return r;
    if (std::string r =
            try_root(exe + "\\..\\..\\assets\\StreetLegal_Redline_exe");
        !r.empty())
      return r;
  }
  const char* cwd_tails[] = {
      "native/engine/assets/StreetLegal_Redline_exe",
      "../native/engine/assets/StreetLegal_Redline_exe",
      "../../native/engine/assets/StreetLegal_Redline_exe",
      "assets/StreetLegal_Redline_exe",
  };
  for (const char* t : cwd_tails) {
    if (std::string r = try_root(t); !r.empty()) return r;
  }
  return {};
}

bool load_stock_window_assets() {
  if (g_assets_ok) return true;
  const std::string root = find_stock_assets_root();
  if (root.empty()) return false;

  std::string ico = root + "\\Icon\\0000\\1.ico";
  if (!file_exists_a(ico)) ico = root + "/Icon/0000/1.ico";
  if (file_exists_a(ico)) {
    g_stock_icon = static_cast<HICON>(LoadImageA(
        nullptr, ico.c_str(), IMAGE_ICON, 0, 0,
        LR_LOADFROMFILE | LR_DEFAULTSIZE));
  }

  // RT_CURSOR names extracted from stock exe: 2.cur … 11.cur
  for (int id = 2; id <= 11; ++id) {
    char name[32];
    std::snprintf(name, sizeof(name), "%d.cur", id);
    std::string cur = root + "\\Cursor\\0409\\" + name;
    if (!file_exists_a(cur)) cur = root + "/Cursor/0409/" + name;
    if (!file_exists_a(cur)) continue;
    g_stock_cursors[id] = LoadCursorFromFileA(cur.c_str());
  }
  g_assets_ok = g_stock_icon != nullptr || g_stock_cursors[2] != nullptr;
  return g_assets_ok;
}

void apply_stock_cursor_to_window() {
  HCURSOR cur = nullptr;
  if (g_stock_cursor_id >= 0 &&
      g_stock_cursor_id < static_cast<int32_t>(sizeof(g_stock_cursors) /
                                               sizeof(g_stock_cursors[0])))
    cur = g_stock_cursors[g_stock_cursor_id];
  if (!cur) cur = g_stock_cursors[2];
  if (!cur) cur = LoadCursor(nullptr, IDC_ARROW);
  if (g_hwnd) SetClassLongPtrA(g_hwnd, GCLP_HCURSOR, reinterpret_cast<LONG_PTR>(cur));
  SetCursor(cur);
}
#endif

int32_t g_w = 0;
int32_t g_h = 0;
int32_t g_mode = 0;

struct Mode {
  int32_t w;
  int32_t h;
  int32_t depth;
};
std::vector<Mode> g_modes;

struct ViewportState {
  int32_t pri = 0;
  // Host stand-in for PE viewport-rect (vtable+0xC after sub_5447D0
  // 0x80000001): +0x10 left, +0x14 top, +0x18 width, +0x1C height.
  // resize @ 0x00481700 FSTPs unboxed Java (FFFF) here with no video_* scale.
  float x = 0.f;
  float y = 0.f;
  float w = 1.f;
  float h = 1.f;
  int32_t pending_clear = 0;
  bool active = false;
};
std::unordered_map<void*, ViewportState> g_viewports;
void* g_active_vp = nullptr;

struct CameraState {
  void* parent = nullptr;
  void* viewport = nullptr;
  int32_t pri = 0;
  float half_aov_deg = 45.f;
  float dmin = 0.1f;
  float dmax = 100.f;
  float lod_bias = 1.f;
  float lod_amp = 1.f;
  int32_t oc = 1;
  int32_t pt = 0;
  bool active = false;
  // Phase 2.35 — look-at view (identity if unset).
  bool lookat = false;
  float eye_x = 0, eye_y = 2.f, eye_z = -8.f;
  float at_x = 0, at_y = 1.f, at_z = 0;
};
std::unordered_map<void*, CameraState> g_cameras;
void* g_active_cam = nullptr;

struct FogState {
  bool enabled = false;
  int32_t color = 0;
  float near_z = 0.f;
  float far_z = 0.f;
};
FogState g_fog;

struct LightState {
  bool enabled = false;
  int32_t diffuse = 0x00ffffff;
  int32_t ambient = 0x00282c34;
  int32_t specular = 0x00ffffff;
};
LightState g_light;

struct FlareState {
  void* key = nullptr;
  void* tex = nullptr;
  int32_t color = 0xe4e4e4;
  float min_size = 1.f;
  float max_size = 10.f;
  int32_t count = 0;
  int32_t rays = 0;
  bool has_world = false;
  float wx = 0, wy = 0, wz = 0;
  bool has_screen = false;
  float sx = 0.55f, sy = 0.42f;
  bool behind = false;
};
std::vector<FlareState> g_flares;
int32_t g_flare_sprites_last = 0;
bool g_flares_enabled = true;

// PE RenderRef_applyLight @ 0x0048C9D0: byte * Light_byteToUnit (flt_5F13E0 = 1/256).
constexpr float kLightByteToUnit = 0.00390625f;

void rgb_bytes(int32_t rgb, float* r, float* g, float* b) {
  *r = static_cast<float>((rgb >> 16) & 0xff) * kLightByteToUnit;
  *g = static_cast<float>((rgb >> 8) & 0xff) * kLightByteToUnit;
  *b = static_cast<float>(rgb & 0xff) * kLightByteToUnit;
}

struct TextureState {
  std::string label;
  int32_t w = 0;
  int32_t h = 0;
  int32_t mips = 1;
  bool luma_alpha = false;
#ifdef _WIN32
  IDirect3DTexture9* tex = nullptr;
#endif
};
std::unordered_map<void*, TextureState> g_textures;
// Host analogue of g_GfxEngine/off_6187B0+0x64 (PE @ 0x0047C220).
// setGlobalEnvmap does not SetTexture; store only — do not bind here.
void* g_envmap = nullptr;

struct OsdRect {
  void* key = nullptr;
  float x = 0.f;
  float y = 0.f;
  float w = 2.f;
  float h = 2.f;
  void* texture = nullptr;
  int32_t pri = 0;
  uint32_t color = 0xFFFFFFFFu;
  bool visible = true;
};
std::vector<OsdRect> g_osd;

struct FontGlyph {
  float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
  float adv_px = 0;
};

struct FontState {
  std::string name;
  void* atlas_key = nullptr;
  int32_t atlas_w = 0;
  int32_t atlas_h = 0;
  int32_t px_height = 20;
  std::string charset;
  std::vector<FontGlyph> glyphs;
  bool ready = false;
};
std::unordered_map<void*, FontState> g_fonts;

struct TextState {
  void* font = nullptr;
  float x = 0.f;
  float y = 0.f;
  uint32_t color = 0xFFFFFFFFu;
  int32_t align = 2;  // LEFT
  std::string text;
  bool alive = false;
  bool visible = true;
};
std::unordered_map<void*, TextState> g_texts;

struct OsdText {
  void* text_key = nullptr;
  void* font = nullptr;
  float x = 0.f;
  float y = 0.f;
  uint32_t color = 0xFFFFFFFFu;
  int32_t align = 2;
  std::string text;
  int32_t pri = 0;
  bool visible = true;
};
std::vector<OsdText> g_osd_text;

struct MeshVertex {
  float x, y, z;
  float nx, ny, nz;
  float u, v;
};

struct MeshSubmesh {
  std::string name;
  std::vector<MeshVertex> verts;
  std::vector<uint16_t> indices;
  void* texture_key = nullptr;  // entry in g_textures (owned via mesh)
  std::string texture_path;
  uint32_t diffuse = 0xFF808080u;
};

struct MeshState {
  std::string label;
  std::vector<MeshSubmesh> subs;
  float bmin[3] = {0, 0, 0};
  float bmax[3] = {0, 0, 0};
  bool ready = false;
};
std::unordered_map<void*, MeshState> g_meshes;
std::vector<void*> g_mesh_queue;

struct MeshXform {
  float px = 0, py = 0, pz = 0;
  float oy = 0, op = 0, or_ = 0;
  float sx = 1, sy = 1, sz = 1;
  void* parent = nullptr;
  // When parent is set: Local * BoneLocal(parent, attach_bone) * ParentWorld.
  int32_t attach_bone = 0;
  // PE setColor @ 0x00480310 → slot+0xCC DWORD as-is (no 1/256).
  int32_t color = 0;
  int32_t color_set = 0;
};
std::unordered_map<void*, MeshXform> g_mesh_xforms;
// Per-mesh bone local poses (id 0 = root / identity unless written).
std::unordered_map<void*, std::unordered_map<int32_t, MeshXform>> g_mesh_bones;
// Per-mesh alias → bone id (0 reserved for bone00/root).
std::unordered_map<void*, std::unordered_map<std::string, int32_t>> g_mesh_bone_alias;
std::unordered_map<void*, int32_t> g_mesh_bone_next;  // next id to allocate (>=1)

#ifdef _WIN32
void mat_identity(D3DMATRIX* m) {
  std::memset(m, 0, sizeof(*m));
  m->m[0][0] = m->m[1][1] = m->m[2][2] = m->m[3][3] = 1.f;
}

void mat_mul(const D3DMATRIX& a, const D3DMATRIX& b, D3DMATRIX* out) {
  D3DMATRIX r{};
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j)
      r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] +
                  a.m[i][2] * b.m[2][j] + a.m[i][3] * b.m[3][j];
  *out = r;
}

void build_local_world(const MeshXform& xf, D3DMATRIX* out) {
  const float cy = std::cos(xf.oy), sy = std::sin(xf.oy);
  const float cp = std::cos(xf.op), sp = std::sin(xf.op);
  const float cr = std::cos(xf.or_), sr = std::sin(xf.or_);
  // R = Ry(yaw) * Rx(pitch) * Rz(roll)
  const float r00 = cy * cr + sy * sp * sr;
  const float r01 = cp * sr;
  const float r02 = -sy * cr + cy * sp * sr;
  const float r10 = -cy * sr + sy * sp * cr;
  const float r11 = cp * cr;
  const float r12 = sy * sr + cy * sp * cr;
  const float r20 = sy * cp;
  const float r21 = -sp;
  const float r22 = cy * cp;
  // Local = Scale * R * Translation  (row-vector: v*S*R*T)
  D3DMATRIX& m = *out;
  std::memset(&m, 0, sizeof(m));
  m.m[0][0] = r00 * xf.sx;
  m.m[0][1] = r01 * xf.sx;
  m.m[0][2] = r02 * xf.sx;
  m.m[1][0] = r10 * xf.sy;
  m.m[1][1] = r11 * xf.sy;
  m.m[1][2] = r12 * xf.sy;
  m.m[2][0] = r20 * xf.sz;
  m.m[2][1] = r21 * xf.sz;
  m.m[2][2] = r22 * xf.sz;
  m.m[3][0] = xf.px;
  m.m[3][1] = xf.py;
  m.m[3][2] = xf.pz;
  m.m[3][3] = 1.f;
}

// World = Local * ParentWorld (row-vector hierarchy).
// If attach_bone != 0 (or bone 0 has an explicit pose): insert bone local of parent.
bool resolve_world(void* key, D3DMATRIX* out, int depth) {
  if (!out) return false;
  if (!key || depth > 32) {
    mat_identity(out);
    return false;
  }
  MeshXform xf;
  auto it = g_mesh_xforms.find(key);
  if (it != g_mesh_xforms.end()) xf = it->second;
  D3DMATRIX local{};
  build_local_world(xf, &local);
  if (!xf.parent) {
    *out = local;
    return true;
  }
  D3DMATRIX parent_world{};
  resolve_world(xf.parent, &parent_world, depth + 1);
  D3DMATRIX bone_local{};
  mat_identity(&bone_local);
  auto bit = g_mesh_bones.find(xf.parent);
  if (bit != g_mesh_bones.end()) {
    auto b2 = bit->second.find(xf.attach_bone);
    if (b2 != bit->second.end()) build_local_world(b2->second, &bone_local);
  }
  D3DMATRIX bone_world{};
  mat_mul(bone_local, parent_world, &bone_world);
  mat_mul(local, bone_world, out);
  return true;
}
#endif

void ensure_modes() {
  if (!g_modes.empty()) return;
  g_modes.push_back({800, 600, 32});
  g_modes.push_back({1024, 768, 32});
  g_modes.push_back({1280, 720, 32});
  g_modes.push_back({1920, 1080, 32});
}

#ifdef _WIN32
LRESULT CALLBACK render_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
      // PE Engine_WndProc @ 0x004B8000: 0x200-0x202 and 0x204-0x205 only
      // (not L/R DBLCLK). Store NDC then DefWindowProc.
      input_wndproc_mouse(hwnd, msg, wp, lp);
      break;
    case WM_SETCURSOR:
      if (LOWORD(lp) == HTCLIENT) {
        apply_stock_cursor_to_window();
        return TRUE;
      }
      break;
    case WM_DESTROY:
      g_quit_requested = true;
      PostQuitMessage(0);
      return 0;
    case WM_CLOSE:
      DestroyWindow(hwnd);
      return 0;
    default:
      break;
  }
  return DefWindowProcA(hwnd, msg, wp, lp);
}

bool create_device(HWND hwnd, int32_t w, int32_t h) {
  if (g_dev) {
    g_dev->Release();
    g_dev = nullptr;
  }
  if (!g_d3d) {
    g_d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!g_d3d) {
      std::fprintf(stderr, "[render] Direct3DCreate9 failed\n");
      return false;
    }
  }

  D3DPRESENT_PARAMETERS pp{};
  pp.Windowed = TRUE;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.BackBufferFormat = D3DFMT_UNKNOWN;
  pp.BackBufferWidth = static_cast<UINT>(w);
  pp.BackBufferHeight = static_cast<UINT>(h);
  pp.hDeviceWindow = hwnd;
  pp.EnableAutoDepthStencil = TRUE;
  pp.AutoDepthStencilFormat = D3DFMT_D16;
  pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

  // 0x20 = D3DCREATE_SOFTWAREVERTEXPROCESSING (compat with thin SDKs).
  constexpr DWORD kCreateFlags = 0x00000020L;
  HRESULT hr = g_d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                   kCreateFlags, &pp, &g_dev);
  if (FAILED(hr)) {
    hr = g_d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_REF, hwnd,
                             kCreateFlags, &pp, &g_dev);
  }
  if (FAILED(hr) || !g_dev) {
    std::fprintf(stderr, "[render] CreateDevice failed hr=0x%08lX\n",
                 static_cast<unsigned long>(hr));
    return false;
  }
  g_w = w;
  g_h = h;
  return true;
}
#endif

}  // namespace

bool render_d3d9_ready() {
#ifdef _WIN32
  return g_dev != nullptr;
#else
  return false;
#endif
}

int32_t render_d3d9_width() { return g_w; }
int32_t render_d3d9_height() { return g_h; }

bool render_d3d9_open(int32_t width, int32_t height, const char* title) {
#ifdef _WIN32
  ensure_modes();
  if (width <= 0) width = 800;
  if (height <= 0) height = 600;
  if (!title || !title[0]) title = "SLRR Engine";

  load_stock_window_assets();

  HINSTANCE hi = GetModuleHandleA(nullptr);
  if (!g_class_reg) {
    g_wc = {};
    g_wc.cbSize = sizeof(g_wc);
    g_wc.style = CS_OWNDC;
    g_wc.lpfnWndProc = render_wnd_proc;
    g_wc.hInstance = hi;
    g_wc.hIcon = g_stock_icon ? g_stock_icon : LoadIcon(nullptr, IDI_APPLICATION);
    g_wc.hIconSm = g_stock_icon;
    g_wc.hCursor = g_stock_cursors[2] ? g_stock_cursors[2]
                                      : LoadCursor(nullptr, IDC_ARROW);
    g_wc.lpszClassName = "SLRREngineWnd";
    if (!RegisterClassExA(&g_wc)) {
      std::fprintf(stderr, "[render] RegisterClassEx failed\n");
      return false;
    }
    g_class_reg = true;
  }

  if (g_hwnd) {
    DestroyWindow(g_hwnd);
    g_hwnd = nullptr;
  }
  g_quit_requested = false;

  // Fixed client size matching the backbuffer — resizable chrome stretches
  // 4:3 content into widescreen and makes OSD text look crushed.
  constexpr DWORD kWndStyle =
      WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE;
  RECT rc{0, 0, width, height};
  AdjustWindowRect(&rc, kWndStyle, FALSE);
  g_hwnd = CreateWindowExA(
      0, g_wc.lpszClassName, title, kWndStyle, CW_USEDEFAULT, CW_USEDEFAULT,
      rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, hi, nullptr);
  if (!g_hwnd) {
    std::fprintf(stderr, "[render] CreateWindowEx failed\n");
    return false;
  }
  if (g_stock_icon) {
    SendMessageA(g_hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(g_stock_icon));
    SendMessageA(g_hwnd, WM_SETICON, ICON_SMALL,
                 reinterpret_cast<LPARAM>(g_stock_icon));
  }
  apply_stock_cursor_to_window();
  ShowWindow(g_hwnd, SW_SHOW);
  UpdateWindow(g_hwnd);

  if (!create_device(g_hwnd, width, height)) {
    DestroyWindow(g_hwnd);
    g_hwnd = nullptr;
    return false;
  }

  // Pick matching mode index if listed.
  for (size_t i = 0; i < g_modes.size(); ++i) {
    if (g_modes[i].w == width && g_modes[i].h == height) {
      g_mode = static_cast<int32_t>(i);
      break;
    }
  }

  std::printf("[render] D3D9 ready %dx%d hwnd=%p\n", g_w, g_h,
              static_cast<void*>(g_hwnd));
  return true;
#else
  (void)width;
  (void)height;
  (void)title;
  return false;
#endif
}

bool render_d3d9_assets_ready() {
#ifdef _WIN32
  if (!g_assets_ok) load_stock_window_assets();
  return g_assets_ok;
#else
  return false;
#endif
}

bool render_d3d9_set_stock_cursor(int32_t cursor_id) {
#ifdef _WIN32
  if (!g_assets_ok) load_stock_window_assets();
  if (cursor_id == 0) cursor_id = 2;
  if (cursor_id < 2 || cursor_id > 11) return false;
  if (!g_stock_cursors[cursor_id]) return false;
  g_stock_cursor_id = cursor_id;
  apply_stock_cursor_to_window();
  return true;
#else
  (void)cursor_id;
  return false;
#endif
}

int32_t render_d3d9_stock_cursor() {
#ifdef _WIN32
  return g_stock_cursor_id;
#else
  return 0;
#endif
}

bool render_d3d9_stock_icon_loaded() {
#ifdef _WIN32
  if (!g_assets_ok) load_stock_window_assets();
  return g_stock_icon != nullptr;
#else
  return false;
#endif
}

void render_d3d9_set_cursor_visible(int32_t visible) {
#ifdef _WIN32
  if (visible) {
    render_d3d9_set_stock_cursor(g_stock_cursor_id);
    while (ShowCursor(TRUE) < 0) {
    }
  } else {
    while (ShowCursor(FALSE) >= 0) {
    }
  }
#else
  (void)visible;
#endif
}

void render_d3d9_close() {
#ifdef _WIN32
  if (g_dev) {
    g_dev->Release();
    g_dev = nullptr;
  }
  if (g_d3d) {
    g_d3d->Release();
    g_d3d = nullptr;
  }
  if (g_hwnd) {
    DestroyWindow(g_hwnd);
    g_hwnd = nullptr;
  }
  g_w = g_h = 0;
#endif
  g_viewports.clear();
  g_active_vp = nullptr;
  g_cameras.clear();
  g_active_cam = nullptr;
  g_fog = FogState{};
  g_light = LightState{};
  g_flares.clear();
  g_flare_sprites_last = 0;
  for (auto& kv : g_textures) {
#ifdef _WIN32
    if (kv.second.tex) kv.second.tex->Release();
#endif
  }
  g_textures.clear();
  g_envmap = nullptr;
  g_osd.clear();
  g_osd_text.clear();
  g_fonts.clear();
  g_texts.clear();
  g_meshes.clear();
  g_mesh_queue.clear();
  g_mesh_xforms.clear();
  input_live_shutdown();
}

void* render_d3d9_hwnd() {
#ifdef _WIN32
  return g_hwnd;
#else
  return nullptr;
#endif
}

void* render_d3d9_device() {
#ifdef _WIN32
  return g_dev;
#else
  return nullptr;
#endif
}

void set_fullscreen_viewport() {
#ifdef _WIN32
  if (!g_dev || g_w <= 0 || g_h <= 0) return;
  D3DVIEWPORT9 dvp{};
  dvp.X = 0;
  dvp.Y = 0;
  dvp.Width = static_cast<DWORD>(g_w);
  dvp.Height = static_cast<DWORD>(g_h);
  dvp.MinZ = 0.f;
  dvp.MaxZ = 1.f;
  g_dev->SetViewport(&dvp);
#endif
}

void blit_video_quad(IDirect3DTexture9* tex, float left, float top, float right,
                     float bottom) {
#ifdef _WIN32
  struct Vtx {
    float x, y, z, rhw;
    DWORD color;
    float u, v;
  };
  const DWORD fvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;
  Vtx v[4] = {
      {left, top, 0.f, 1.f, 0xFFFFFFFFu, 0.f, 0.f},
      {right, top, 0.f, 1.f, 0xFFFFFFFFu, 1.f, 0.f},
      {left, bottom, 0.f, 1.f, 0xFFFFFFFFu, 0.f, 1.f},
      {right, bottom, 0.f, 1.f, 0xFFFFFFFFu, 1.f, 1.f},
  };
  set_fullscreen_viewport();
  g_dev->SetFVF(fvf);
  g_dev->SetRenderState(D3DRS_ZENABLE, FALSE);
  g_dev->SetRenderState(D3DRS_LIGHTING, FALSE);
  g_dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
  g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
  g_dev->SetTexture(0, tex);
  g_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
  g_dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
  g_dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
  g_dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
  g_dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(Vtx));
  g_dev->SetTexture(0, nullptr);
#else
  (void)tex;
  (void)left;
  (void)top;
  (void)right;
  (void)bottom;
#endif
}

void render_d3d9_draw_fullscreen_texture(void* d3d_texture) {
#ifdef _WIN32
  if (!g_dev || !d3d_texture || g_w <= 0 || g_h <= 0) return;
  auto* tex = static_cast<IDirect3DTexture9*>(d3d_texture);
  blit_video_quad(tex, 0.f, 0.f, static_cast<float>(g_w),
                  static_cast<float>(g_h));
#else
  (void)d3d_texture;
#endif
}

void render_d3d9_draw_video_texture(void* d3d_texture, int32_t src_w,
                                    int32_t src_h) {
#ifdef _WIN32
  if (!g_dev || !d3d_texture || g_w <= 0 || g_h <= 0) return;
  if (src_w <= 0 || src_h <= 0) {
    render_d3d9_draw_fullscreen_texture(d3d_texture);
    return;
  }
  auto* tex = static_cast<IDirect3DTexture9*>(d3d_texture);
  const float dw = static_cast<float>(g_w);
  const float dh = static_cast<float>(g_h);
  const float sa = static_cast<float>(src_w) / static_cast<float>(src_h);
  const float da = dw / dh;
  float left = 0.f, top = 0.f, right = dw, bottom = dh;
  // Stock TextureRenderer: fit video aspect inside the client rect.
  if (sa > da) {
    const float h = dw / sa;
    top = (dh - h) * 0.5f;
    bottom = top + h;
  } else {
    const float w = dh * sa;
    left = (dw - w) * 0.5f;
    right = left + w;
  }
  blit_video_quad(tex, left, top, right, bottom);
#else
  (void)d3d_texture;
  (void)src_w;
  (void)src_h;
#endif
}

void set_lookat_view(float eye_x, float eye_y, float eye_z, float at_x,
                     float at_y, float at_z);

void apply_active_viewport() {
#ifdef _WIN32
  if (!g_dev || !g_active_vp) return;
  auto it = g_viewports.find(g_active_vp);
  if (it == g_viewports.end()) return;
  const ViewportState& vp = it->second;
  D3DVIEWPORT9 dvp{};
  const float fw = static_cast<float>(g_w > 0 ? g_w : 800);
  const float fh = static_cast<float>(g_h > 0 ? g_h : 600);
  dvp.X = static_cast<DWORD>((vp.x > 0.f ? vp.x : 0.f) * fw);
  dvp.Y = static_cast<DWORD>((vp.y > 0.f ? vp.y : 0.f) * fh);
  dvp.Width = static_cast<DWORD>((vp.w > 1.f / fw ? vp.w : 1.f / fw) * fw);
  if (dvp.Width < 1) dvp.Width = 1;
  dvp.Height = static_cast<DWORD>((vp.h > 1.f / fh ? vp.h : 1.f / fh) * fh);
  if (dvp.Height < 1) dvp.Height = 1;
  if (dvp.X + dvp.Width > static_cast<DWORD>(fw))
    dvp.Width = static_cast<DWORD>(fw) - dvp.X;
  if (dvp.Y + dvp.Height > static_cast<DWORD>(fh))
    dvp.Height = static_cast<DWORD>(fh) - dvp.Y;
  dvp.MinZ = 0.f;
  dvp.MaxZ = 1.f;
  g_dev->SetViewport(&dvp);
#endif
}

void apply_active_camera() {
#ifdef _WIN32
  if (!g_dev || !g_active_cam) return;
  auto it = g_cameras.find(g_active_cam);
  if (it == g_cameras.end()) return;
  const CameraState& cam = it->second;

  float aspect = 4.f / 3.f;
  void* vp_key = cam.viewport ? cam.viewport : g_active_vp;
  if (vp_key) {
    auto vit = g_viewports.find(vp_key);
    if (vit != g_viewports.end()) {
      const float px_w =
          vit->second.w * static_cast<float>(g_w > 0 ? g_w : 1);
      const float px_h =
          vit->second.h * static_cast<float>(g_h > 0 ? g_h : 1);
      if (px_h > 0.f) aspect = px_w / px_h;
    }
  } else if (g_h > 0) {
    aspect = static_cast<float>(g_w) / static_cast<float>(g_h);
  }

  // Java passes half AOV in degrees; perspective uses tan(half).
  float half_rad = cam.half_aov_deg * 0.01745329252f;
  if (half_rad < 0.01f) half_rad = 0.01f;
  if (half_rad > 1.55f) half_rad = 1.55f;
  const float ys = 1.f / std::tan(half_rad);
  const float xs = ys / aspect;
  const float zn = cam.dmin > 1e-4f ? cam.dmin : 0.1f;
  const float zf = cam.dmax > zn + 1e-3f ? cam.dmax : zn + 100.f;
  const float q = zf / (zf - zn);

  D3DMATRIX proj{};
  proj.m[0][0] = xs;
  proj.m[1][1] = ys;
  proj.m[2][2] = q;
  proj.m[2][3] = 1.f;
  proj.m[3][2] = -q * zn;
  g_dev->SetTransform(D3DTS_PROJECTION, &proj);

  if (cam.lookat) {
    set_lookat_view(cam.eye_x, cam.eye_y, cam.eye_z, cam.at_x, cam.at_y,
                    cam.at_z);
  } else {
    D3DMATRIX view{};
    view.m[0][0] = view.m[1][1] = view.m[2][2] = view.m[3][3] = 1.f;
    g_dev->SetTransform(D3DTS_VIEW, &view);
  }

  if (g_fog.enabled) {
    const DWORD fog_col = D3DCOLOR_XRGB((g_fog.color >> 16) & 0xff,
                                        (g_fog.color >> 8) & 0xff,
                                        g_fog.color & 0xff);
    g_dev->SetRenderState(D3DRS_FOGENABLE, TRUE);
    g_dev->SetRenderState(D3DRS_FOGCOLOR, fog_col);
    g_dev->SetRenderState(D3DRS_FOGTABLEMODE, D3DFOG_LINEAR);
    float n = g_fog.near_z;
    float f = g_fog.far_z;
    g_dev->SetRenderState(D3DRS_FOGSTART, *reinterpret_cast<DWORD*>(&n));
    g_dev->SetRenderState(D3DRS_FOGEND, *reinterpret_cast<DWORD*>(&f));
  } else {
    g_dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
  }
#else
  (void)0;
#endif
}

void set_lookat_view(float eye_x, float eye_y, float eye_z, float at_x,
                     float at_y, float at_z) {
#ifdef _WIN32
  if (!g_dev) return;
  // D3DXMatrixLookAtLH-compatible view.
  float zx = at_x - eye_x;
  float zy = at_y - eye_y;
  float zz = at_z - eye_z;
  float zl = std::sqrt(zx * zx + zy * zy + zz * zz);
  if (zl < 1e-6f) zl = 1.f;
  zx /= zl;
  zy /= zl;
  zz /= zl;
  // xaxis = normalize(cross(up=(0,1,0), zaxis))
  float xx = zz;
  float xy = 0.f;
  float xz = -zx;
  float xl = std::sqrt(xx * xx + xy * xy + xz * xz);
  if (xl < 1e-6f) {
    xx = 1.f;
    xy = 0.f;
    xz = 0.f;
    xl = 1.f;
  }
  xx /= xl;
  xy /= xl;
  xz /= xl;
  // yaxis = cross(zaxis, xaxis)
  float yx = zy * xz - zz * xy;
  float yy = zz * xx - zx * xz;
  float yz = zx * xy - zy * xx;
  D3DMATRIX view{};
  view.m[0][0] = xx;
  view.m[0][1] = yx;
  view.m[0][2] = zx;
  view.m[1][0] = xy;
  view.m[1][1] = yy;
  view.m[1][2] = zy;
  view.m[2][0] = xz;
  view.m[2][1] = yz;
  view.m[2][2] = zz;
  view.m[3][0] = -(xx * eye_x + xy * eye_y + xz * eye_z);
  view.m[3][1] = -(yx * eye_x + yy * eye_y + yz * eye_z);
  view.m[3][2] = -(zx * eye_x + zy * eye_y + zz * eye_z);
  view.m[3][3] = 1.f;
  g_dev->SetTransform(D3DTS_VIEW, &view);
#else
  (void)eye_x;
  (void)eye_y;
  (void)eye_z;
  (void)at_x;
  (void)at_y;
  (void)at_z;
#endif
}

void draw_meshes() {
#ifdef _WIN32
  if (!g_dev || g_mesh_queue.empty()) return;

  auto xform_point = [](const D3DMATRIX& m, float x, float y, float z,
                        float* ox, float* oy, float* oz) {
    *ox = x * m.m[0][0] + y * m.m[1][0] + z * m.m[2][0] + m.m[3][0];
    *oy = x * m.m[0][1] + y * m.m[1][1] + z * m.m[2][1] + m.m[3][1];
    *oz = x * m.m[0][2] + y * m.m[1][2] + z * m.m[2][2] + m.m[3][2];
  };

  // Look-at around union of transformed AABBs.
  bool have_bounds = false;
  float bmin[3] = {0, 0, 0};
  float bmax[3] = {0, 0, 0};
  for (void* key : g_mesh_queue) {
    auto it = g_meshes.find(key);
    if (it == g_meshes.end() || !it->second.ready) continue;
    const MeshState& ms = it->second;
    D3DMATRIX world{};
    resolve_world(key, &world, 0);
    const float xs[2] = {ms.bmin[0], ms.bmax[0]};
    const float ys[2] = {ms.bmin[1], ms.bmax[1]};
    const float zs[2] = {ms.bmin[2], ms.bmax[2]};
    for (int i = 0; i < 2; ++i)
      for (int j = 0; j < 2; ++j)
        for (int k = 0; k < 2; ++k) {
          float wx, wy, wz;
          xform_point(world, xs[i], ys[j], zs[k], &wx, &wy, &wz);
          if (!have_bounds) {
            bmin[0] = bmax[0] = wx;
            bmin[1] = bmax[1] = wy;
            bmin[2] = bmax[2] = wz;
            have_bounds = true;
          } else {
            if (wx < bmin[0]) bmin[0] = wx;
            if (wy < bmin[1]) bmin[1] = wy;
            if (wz < bmin[2]) bmin[2] = wz;
            if (wx > bmax[0]) bmax[0] = wx;
            if (wy > bmax[1]) bmax[1] = wy;
            if (wz > bmax[2]) bmax[2] = wz;
          }
        }
  }
  if (have_bounds) {
    const float cx = 0.5f * (bmin[0] + bmax[0]);
    const float cy = 0.5f * (bmin[1] + bmax[1]);
    const float cz = 0.5f * (bmin[2] + bmax[2]);
    float ext = bmax[0] - bmin[0];
    if (bmax[1] - bmin[1] > ext) ext = bmax[1] - bmin[1];
    if (bmax[2] - bmin[2] > ext) ext = bmax[2] - bmin[2];
    if (ext < 1.f) ext = 1.f;
    set_lookat_view(cx, cy + ext * 0.35f, cz - ext * 1.6f, cx, cy, cz);
  }

  const DWORD fvf = D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1;
  g_dev->SetFVF(fvf);
  g_dev->SetRenderState(D3DRS_ZENABLE, TRUE);
  g_dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
  g_dev->SetRenderState(D3DRS_LIGHTING, TRUE);
  g_dev->SetRenderState(D3DRS_NORMALIZENORMALS, TRUE);
  g_dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
  g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
  g_dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
  g_dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
  g_dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);

  D3DLIGHT9 light{};
  light.Type = D3DLIGHT_DIRECTIONAL;
  float dr = 1.f, dg = 1.f, db = 1.f;
  float ar = 40.f / 255.f, ag = 44.f / 255.f, ab = 52.f / 255.f;
  float sr = 1.f, sg = 1.f, sb = 1.f;
  if (g_light.enabled) {
    rgb_bytes(g_light.diffuse, &dr, &dg, &db);
    rgb_bytes(g_light.ambient, &ar, &ag, &ab);
    rgb_bytes(g_light.specular, &sr, &sg, &sb);
  }
  light.Diffuse.r = dr;
  light.Diffuse.g = dg;
  light.Diffuse.b = db;
  light.Specular.r = sr;
  light.Specular.g = sg;
  light.Specular.b = sb;
  light.Direction.x = -0.4f;
  light.Direction.y = -0.7f;
  light.Direction.z = 0.5f;
  g_dev->SetLight(0, &light);
  g_dev->LightEnable(0, TRUE);
  // Packed ambient bytes (setLight a5 dir=0; D3DRS_AMBIENT is host bind).
  if (g_light.enabled) {
    const int32_t amb = g_light.ambient;
    g_dev->SetRenderState(
        D3DRS_AMBIENT,
        D3DCOLOR_XRGB((amb >> 16) & 0xff, (amb >> 8) & 0xff, amb & 0xff));
  } else {
    g_dev->SetRenderState(
        D3DRS_AMBIENT,
        D3DCOLOR_XRGB(static_cast<int>(ar * 255.f + 0.5f),
                      static_cast<int>(ag * 255.f + 0.5f),
                      static_cast<int>(ab * 255.f + 0.5f)));
  }

  for (void* key : g_mesh_queue) {
    auto it = g_meshes.find(key);
    if (it == g_meshes.end() || !it->second.ready) continue;
    D3DMATRIX world{};
    resolve_world(key, &world, 0);
    g_dev->SetTransform(D3DTS_WORLD, &world);

    const MeshXform* xf = nullptr;
    {
      auto xit = g_mesh_xforms.find(key);
      if (xit != g_mesh_xforms.end()) xf = &xit->second;
    }

    for (const MeshSubmesh& sm : it->second.subs) {
      if (sm.verts.empty() || sm.indices.size() < 3) continue;

      IDirect3DTexture9* tex = nullptr;
      if (sm.texture_key) {
        auto tit = g_textures.find(sm.texture_key);
        if (tit != g_textures.end()) tex = tit->second.tex;
      }
      g_dev->SetTexture(0, tex);
      if (tex) {
        g_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        g_dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        g_dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        g_dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        g_dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
      } else {
        g_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        g_dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        g_dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
      }

      D3DMATERIAL9 mat{};
      const uint32_t d =
          (xf && xf->color_set) ? static_cast<uint32_t>(xf->color)
                                : sm.diffuse;
      const float r = static_cast<float>((d >> 16) & 0xff) / 255.f;
      const float g = static_cast<float>((d >> 8) & 0xff) / 255.f;
      const float b = static_cast<float>(d & 0xff) / 255.f;
      if (xf && xf->color_set) {
        mat.Diffuse.r = mat.Ambient.r = r;
        mat.Diffuse.g = mat.Ambient.g = g;
        mat.Diffuse.b = mat.Ambient.b = b;
      } else {
        mat.Diffuse.r = mat.Ambient.r = r > 0.05f ? r : 0.75f;
        mat.Diffuse.g = mat.Ambient.g = g > 0.05f ? g : 0.78f;
        mat.Diffuse.b = mat.Ambient.b = b > 0.05f ? b : 0.82f;
      }
      mat.Diffuse.a = mat.Ambient.a = 1.f;
      g_dev->SetMaterial(&mat);

      const UINT ntri = static_cast<UINT>(sm.indices.size() / 3);
      g_dev->DrawIndexedPrimitiveUP(
          D3DPT_TRIANGLELIST, 0, static_cast<UINT>(sm.verts.size()), ntri,
          sm.indices.data(), D3DFMT_INDEX16, sm.verts.data(),
          sizeof(MeshVertex));
    }
  }

  g_dev->SetTexture(0, nullptr);
  g_dev->LightEnable(0, FALSE);
  g_dev->SetRenderState(D3DRS_LIGHTING, FALSE);
  g_dev->SetRenderState(D3DRS_NORMALIZENORMALS, FALSE);
#endif
}

void queue_flare_sprites() {
  if (!g_flares_enabled) {
    // Still strip prior OSD keys so disable takes effect immediately.
    for (const FlareState& f : g_flares) {
      if (!f.key) continue;
      for (int i = 0; i < 32; ++i) {
        void* k = reinterpret_cast<void*>(
            reinterpret_cast<uintptr_t>(f.key) ^
            (0xF1000000u + static_cast<uint32_t>(i)));
        render_d3d9_osd_remove_rect(k);
      }
      for (int i = 0; i < 16; ++i) {
        void* k = reinterpret_cast<void*>(
            reinterpret_cast<uintptr_t>(f.key) ^
            (0xF2000000u + static_cast<uint32_t>(i)));
        render_d3d9_osd_remove_rect(k);
      }
    }
    g_flare_sprites_last = 0;
    return;
  }
  // Remove prior flare OSD keys, then lay sprites along sun→center streak.
  for (const FlareState& f : g_flares) {
    if (!f.key) continue;
    for (int i = 0; i < 32; ++i) {
      void* k = reinterpret_cast<void*>(
          reinterpret_cast<uintptr_t>(f.key) ^
          (0xF1000000u + static_cast<uint32_t>(i)));
      render_d3d9_osd_remove_rect(k);
    }
    for (int i = 0; i < 16; ++i) {
      void* k = reinterpret_cast<void*>(
          reinterpret_cast<uintptr_t>(f.key) ^
          (0xF2000000u + static_cast<uint32_t>(i)));
      render_d3d9_osd_remove_rect(k);
    }
  }
  g_flare_sprites_last = 0;
  for (FlareState& f : g_flares) {
    if (!f.key || f.count <= 0) continue;
    float sun_x = 0.55f;
    float sun_y = 0.42f;
    f.behind = false;
    f.has_screen = false;
    if (f.has_world) {
      float px = 0, py = 0;
      if (!render_d3d9_project(f.wx, f.wy, f.wz, &px, &py)) {
        f.behind = true;
        continue;  // behind / invalid — hide
      }
      sun_x = px;
      sun_y = py;
      f.sx = px;
      f.sy = py;
      f.has_screen = true;
    }
    const int n = f.count > 24 ? 24 : f.count;
    uint32_t argb = static_cast<uint32_t>(f.color);
    if ((argb & 0xFF000000u) == 0) argb |= 0xC0000000u;
    for (int i = 0; i < n; ++i) {
      const float t =
          n <= 1 ? 0.f : static_cast<float>(i) / static_cast<float>(n - 1);
      const float x = sun_x + (0.f - sun_x) * (t * 1.6f);
      const float y = sun_y + (0.f - sun_y) * (t * 1.6f);
      float sz = f.min_size + (f.max_size - f.min_size) * t;
      sz *= 0.035f;
      if (sz < 0.02f) sz = 0.02f;
      if (sz > 0.55f) sz = 0.55f;
      void* k = reinterpret_cast<void*>(
          reinterpret_cast<uintptr_t>(f.key) ^
          (0xF1000000u + static_cast<uint32_t>(i)));
      render_d3d9_osd_set_rect_color(k, x, y, sz, sz, f.tex, argb, 60 + i);
      ++g_flare_sprites_last;
    }
    const int rays = f.rays > 12 ? 12 : f.rays;
    for (int r = 0; r < rays; ++r) {
      const float ang =
          (6.2831853f * static_cast<float>(r)) /
          static_cast<float>(rays > 0 ? rays : 1);
      const float len = f.min_size * 0.04f;
      const float x = sun_x + std::cos(ang) * len * 0.5f;
      const float y = sun_y + std::sin(ang) * len * 0.5f;
      void* k = reinterpret_cast<void*>(
          reinterpret_cast<uintptr_t>(f.key) ^
          (0xF2000000u + static_cast<uint32_t>(r)));
      render_d3d9_osd_set_rect_color(k, x, y, len * 0.15f, len, f.tex, argb,
                                     50 + r);
      ++g_flare_sprites_last;
    }
  }
}

void draw_osd_rects() {
#ifdef _WIN32
  if (!g_dev || g_osd.empty() || g_w <= 0 || g_h <= 0) return;

  // Stable-ish draw order: lower pri first (background), matching Osd usage
  // (createBG uses pri=-2, header -1).
  std::vector<OsdRect> ordered = g_osd;
  for (size_t i = 1; i < ordered.size(); ++i) {
    OsdRect key = ordered[i];
    size_t j = i;
    while (j > 0 && ordered[j - 1].pri > key.pri) {
      ordered[j] = ordered[j - 1];
      --j;
    }
    ordered[j] = key;
  }

  struct Vtx {
    float x, y, z, rhw;
    DWORD color;
    float u, v;
  };
  const DWORD fvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;
  set_fullscreen_viewport();
  g_dev->SetFVF(fvf);
  g_dev->SetRenderState(D3DRS_ZENABLE, FALSE);
  g_dev->SetRenderState(D3DRS_LIGHTING, FALSE);
  g_dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
  g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
  g_dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
  g_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
  g_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
  g_dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
  g_dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
  g_dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
  g_dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
  g_dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);

  const float sw = static_cast<float>(g_w);
  const float sh = static_cast<float>(g_h);
  for (const auto& r : ordered) {
    if (!r.visible) continue;
    IDirect3DTexture9* tex = nullptr;
    if (r.texture) {
      auto it = g_textures.find(r.texture);
      if (it != g_textures.end()) tex = it->second.tex;
    }
    g_dev->SetTexture(0, tex);
    if (tex) {
      g_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
      g_dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    } else {
      g_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
      g_dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
    }
    g_dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    g_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);

    // Host OSD blit: x right, y up; (0,0,2,2) covers [-1,1]² → full window.
    // (Stock scripts use the opposite Y via convertTextCoordinates — host
    // chrome negates Y when materializing stock layouts.)
    const float left = (r.x - r.w * 0.5f + 1.f) * 0.5f * sw;
    const float right = (r.x + r.w * 0.5f + 1.f) * 0.5f * sw;
    const float top = (1.f - (r.y + r.h * 0.5f)) * 0.5f * sh;
    const float bottom = (1.f - (r.y - r.h * 0.5f)) * 0.5f * sh;
    const DWORD col = r.color ? r.color : 0xFFFFFFFFu;

    Vtx v[4] = {
        {left, top, 0.f, 1.f, col, 0.f, 0.f},
        {right, top, 0.f, 1.f, col, 1.f, 0.f},
        {left, bottom, 0.f, 1.f, col, 0.f, 1.f},
        {right, bottom, 0.f, 1.f, col, 1.f, 1.f},
    };
    g_dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(Vtx));
  }

  g_dev->SetTexture(0, nullptr);
  g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
  g_dev->SetRenderState(D3DRS_ZENABLE, TRUE);
#endif
}

void draw_osd_texts() {
#ifdef _WIN32
  if (!g_dev || g_osd_text.empty() || g_w <= 0 || g_h <= 0) return;

  struct Vtx {
    float x, y, z, rhw;
    DWORD color;
    float u, v;
  };
  const DWORD fvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;
  set_fullscreen_viewport();
  g_dev->SetFVF(fvf);
  g_dev->SetRenderState(D3DRS_ZENABLE, FALSE);
  g_dev->SetRenderState(D3DRS_LIGHTING, FALSE);
  g_dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
  g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
  g_dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
  g_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
  g_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
  g_dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
  g_dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
  g_dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
  g_dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
  g_dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
  g_dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
  g_dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);

  const float sw = static_cast<float>(g_w);
  const float sh = static_cast<float>(g_h);

  for (const auto& t : g_osd_text) {
    if (!t.visible || !t.font || t.text.empty()) continue;
    auto fit = g_fonts.find(t.font);
    if (fit == g_fonts.end() || !fit->second.ready) continue;
    const FontState& font = fit->second;
    IDirect3DTexture9* tex = nullptr;
    if (font.atlas_key) {
      auto tit = g_textures.find(font.atlas_key);
      if (tit != g_textures.end()) tex = tit->second.tex;
    }
    if (!tex) continue;
    g_dev->SetTexture(0, tex);

    float width_px = 0.f;
    for (unsigned char ch : t.text) {
      if (ch == ' ') {
        width_px += static_cast<float>(font.px_height) * 0.35f;
        continue;
      }
      // Glyph slots are ASCII-indexed (see font SCX z ≈ -100*code).
      const size_t gi = static_cast<size_t>(ch);
      if (gi >= font.glyphs.size()) continue;
      width_px += font.glyphs[gi].adv_px;
    }
    // Text.java getHeight/getWidth: 2*px / video_{y,x}. t.y is top of line
    // (stock native Text.create), host Y up-positive.
    const float line_h_osd = 2.f * static_cast<float>(font.px_height) / sh;
    const float width_osd = 2.f * width_px / sw;
    float pen_x = t.x;
    if (t.align == 1) pen_x -= width_osd * 0.5f;       // CENTER
    else if (t.align == 0) pen_x -= width_osd;          // RIGHT
    // LEFT (2): pen at t.x — Text.java ALIGN_*
    const float top_y = t.y;
    const float bot_y = t.y - line_h_osd;
    const DWORD col = t.color;

    for (unsigned char ch : t.text) {
      float adv_px = static_cast<float>(font.px_height) * 0.35f;
      float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
      bool draw = false;
      if (ch != ' ') {
        const size_t gi = static_cast<size_t>(ch);
        if (gi < font.glyphs.size()) {
          const FontGlyph& g = font.glyphs[gi];
          adv_px = g.adv_px;
          u0 = g.u0;
          v0 = g.v0;
          u1 = g.u1;
          v1 = g.v1;
          draw = (u1 > u0 + 1e-6f && v1 > v0 + 1e-6f);
        }
      }
      const float adv_osd = 2.f * adv_px / sw;
      if (draw) {
        // Keep glyph pixel aspect from atlas UVs (not line_h vs adv mismatch).
        const float gw_px = (u1 - u0) * static_cast<float>(font.atlas_w);
        const float gh_px = (v1 - v0) * static_cast<float>(font.atlas_h);
        const float gw = 2.f * gw_px / sw;
        const float gh = 2.f * gh_px / sh;
        const float g_top_y = top_y;
        const float g_bot_y = top_y - gh;
        const float left = (pen_x + 1.f) * 0.5f * sw;
        const float right = (pen_x + gw + 1.f) * 0.5f * sw;
        const float top = (1.f - g_top_y) * 0.5f * sh;
        const float bottom = (1.f - g_bot_y) * 0.5f * sh;
        Vtx v[4] = {
            {left, top, 0.f, 1.f, col, u0, v0},
            {right, top, 0.f, 1.f, col, u1, v0},
            {left, bottom, 0.f, 1.f, col, u0, v1},
            {right, bottom, 0.f, 1.f, col, u1, v1},
        };
        g_dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(Vtx));
      }
      pen_x += adv_osd;
    }
  }

  g_dev->SetTexture(0, nullptr);
  g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
  g_dev->SetRenderState(D3DRS_ZENABLE, TRUE);
#endif
}

void render_d3d9_flush() {
#ifdef _WIN32
  if (g_dev) {
    apply_active_viewport();
    apply_active_camera();
    // Clear colour: fog tint when Scene/GroundRef.setFog is active, else slate.
    D3DCOLOR kClear = D3DCOLOR_XRGB(18, 24, 38);
    if (g_fog.enabled) {
      kClear = D3DCOLOR_XRGB((g_fog.color >> 16) & 0xff, (g_fog.color >> 8) & 0xff,
                             g_fog.color & 0xff);
    }
    DWORD clear_flags = D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER;
    if (g_active_vp) {
      auto it = g_viewports.find(g_active_vp);
      if (it != g_viewports.end() && it->second.pending_clear != 0) {
        clear_flags = 0;
        if (it->second.pending_clear & kViewportClearTarget)
          clear_flags |= D3DCLEAR_TARGET;
        if (it->second.pending_clear & kViewportClearDepth)
          clear_flags |= D3DCLEAR_ZBUFFER;
        it->second.pending_clear = 0;
      }
    }
    if (clear_flags != 0)
      g_dev->Clear(0, nullptr, clear_flags, kClear, 1.f, 0);
    if (SUCCEEDED(g_dev->BeginScene())) {
      // FMV + OSD use XYZRHW in full RT space — don't clip to a 3D viewport.
      set_fullscreen_viewport();
      video_fmv_present();
      apply_active_viewport();
      draw_meshes();
      queue_flare_sprites();
      set_fullscreen_viewport();
      draw_osd_rects();
      draw_osd_texts();
      g_dev->EndScene();
    }
    g_dev->Present(nullptr, nullptr, nullptr, nullptr);
    render_d3d9_pump(0);
  } else {
    // Headless: still rebuild flare OSD rects / sprite counts (smoke 2.108+).
    queue_flare_sprites();
  }
#else
  queue_flare_sprites();
#endif
  // Phase 2.118: Frontend.render.wait() — one Object.notify per flush
  // (including headless / no-device so LoadingScreen can pace).
  frontend_gfx_engine_frame_notify();
}

void render_d3d9_pump(int32_t ms) {
#ifdef _WIN32
  if (input_live_enabled()) input_live_poll();
  MSG msg{};
  const DWORD start = GetTickCount();
  for (;;) {
    while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) {
        g_quit_requested = true;
        return;
      }
      TranslateMessage(&msg);
      DispatchMessageA(&msg);
    }
    if (ms <= 0) break;
    if (static_cast<int32_t>(GetTickCount() - start) >= ms) break;
    Sleep(1);
  }
#else
  (void)ms;
#endif
}

bool render_d3d9_quit_requested() {
#ifdef _WIN32
  return g_quit_requested;
#else
  return false;
#endif
}

void render_d3d9_clear_quit() {
#ifdef _WIN32
  g_quit_requested = false;
#endif
}

void render_d3d9_request_quit() {
#ifdef _WIN32
  g_quit_requested = true;
#else
  // Headless / non-Win32: no message pump — flag unused.
#endif
}

int32_t render_d3d9_num_display_modes() {
  ensure_modes();
  return static_cast<int32_t>(g_modes.size());
}

int32_t render_d3d9_curr_display_mode() {
  // PE Gfx_GetCurrDisplayMode @ 0x004B9EA0: count<=0 → 0; else index scan.
  ensure_modes();
  const int32_t n = static_cast<int32_t>(g_modes.size());
  if (n <= 0) return 0;
  const int32_t cw = g_w;
  const int32_t ch = g_h;
  const int32_t cdepth = 32;
  for (int32_t idx = 0; idx < n; ++idx) {
    const Mode& m = g_modes[static_cast<size_t>(idx)];
    if (m.w == cw && m.h == ch && m.depth == cdepth) return idx;
  }
  return 0;
}

bool render_d3d9_change_video_mode(int32_t width, int32_t height,
                                   int32_t depth) {
  (void)depth;
  ensure_modes();
  if (width <= 0 || height <= 0) return false;
#ifdef _WIN32
  if (g_dev && g_hwnd) {
    // Resize window + reset device.
    RECT rc{0, 0, width, height};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    SetWindowPos(g_hwnd, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                 SWP_NOMOVE | SWP_NOZORDER);
    if (!create_device(g_hwnd, width, height)) return false;
  } else {
    if (!render_d3d9_open(width, height, "SLRR Engine")) return false;
  }
  for (size_t i = 0; i < g_modes.size(); ++i) {
    if (g_modes[i].w == width && g_modes[i].h == height) {
      g_mode = static_cast<int32_t>(i);
      return true;
    }
  }
  g_modes.push_back({width, height, depth > 0 ? depth : 32});
  g_mode = static_cast<int32_t>(g_modes.size() - 1);
  return true;
#else
  return false;
#endif
}

void render_d3d9_viewport_create(void* key, int32_t pri, float x, float y,
                                 float w, float h) {
  if (!key) return;
  ViewportState& vp = g_viewports[key];
  vp.pri = pri;
  vp.x = x;
  vp.y = y;
  vp.w = w;
  vp.h = h;
  vp.pending_clear = 0;
  vp.active = false;
}

void render_d3d9_viewport_destroy(void* key) {
  if (!key) return;
  if (g_active_vp == key) g_active_vp = nullptr;
  g_viewports.erase(key);
}

void render_d3d9_viewport_activate(void* key, int32_t renderflags) {
  // PE Viewport.activate(I)V @ 0x00481680: null handle no-op; flags unboxed but
  // unused. Host stand-in: bind active vp + pending_clear for flush Clear.
  if (!key) return;
  auto it = g_viewports.find(key);
  if (it == g_viewports.end()) {
    // Activate before create — treat as full-screen default.
    render_d3d9_viewport_create(key, 0, 0.f, 0.f, 1.f, 1.f);
    it = g_viewports.find(key);
  }
  if (g_active_vp && g_active_vp != key) {
    auto prev = g_viewports.find(g_active_vp);
    if (prev != g_viewports.end()) prev->second.active = false;
  }
  g_active_vp = key;
  it->second.active = true;
  it->second.pending_clear = renderflags & kViewportClearMask;
  apply_active_viewport();
}

void render_d3d9_viewport_deactivate(void* key) {
  // PE Viewport.deactivate()V @ 0x004816C0: null handle no-op; else
  // Engine_ViewportUnbind thiscall (ecx=off_6187B0). Host stand-in:
  // missing map entry == handle 0 (do not touch g_active_vp).
  if (!key) return;
  auto it = g_viewports.find(key);
  if (it == g_viewports.end()) return;
  it->second.active = false;
  if (g_active_vp == key) g_active_vp = nullptr;
}

void render_d3d9_viewport_resize(void* key, float x, float y, float w,
                                 float h) {
  // PE Viewport.resize(FFFF)V @ 0x00481700 size 0xa6: handle 0 / inner 0 /
  // rect 0 silent (no Mighty ERROR, no create). FSTP unboxed floats as-is
  // into rect+0x10 left / +0x14 top / +0x18 width / +0x1C height.
  // Host: map miss == handle 0. apply_active_viewport is D3D stand-in for
  // a live rect (PE has no SetViewport in this native).
  if (!key) return;
  auto it = g_viewports.find(key);
  if (it == g_viewports.end()) return;
  it->second.x = x;
  it->second.y = y;
  it->second.w = w;
  it->second.h = h;
  if (g_active_vp == key) apply_active_viewport();
}

float render_d3d9_viewport_get_aspect(void* key) {
  // PE Viewport_getAspect @ 0x004817B0 / inner 0x0048CDA0: missing handle →
  // 1.0 (flt_5F08F0). Else (rect+0x18 * display_w) / (rect+0x18 * display_h)
  // = display_w/display_h. Host: map miss == handle 0; g_w/g_h == those ints.
  auto it = g_viewports.find(key);
  if (it == g_viewports.end()) return 1.f;
  if (g_h <= 0) return 1.f;
  return static_cast<float>(g_w) / static_cast<float>(g_h);
}

float render_d3d9_viewport_get_width(void* key) {
  // PE getWidth @ 0x004817F0: FLD [rect+0x18] (normalized). Map miss → 0.f
  // stand-in for FLD [ESP+8] this-bits.
  auto it = g_viewports.find(key);
  return it == g_viewports.end() ? 0.f : it->second.w;
}

float render_d3d9_viewport_get_height(void* key) {
  // PE getHeight @ 0x00481870: FLD [rect+0x1C].
  auto it = g_viewports.find(key);
  return it == g_viewports.end() ? 0.f : it->second.h;
}

float render_d3d9_viewport_get_top(void* key) {
  // PE getTop @ 0x004818F0: FLD [rect+0x14].
  auto it = g_viewports.find(key);
  return it == g_viewports.end() ? 0.f : it->second.y;
}

float render_d3d9_viewport_get_left(void* key) {
  // PE getLeft @ 0x00481970: FLD [rect+0x10].
  auto it = g_viewports.find(key);
  return it == g_viewports.end() ? 0.f : it->second.x;
}

void* render_d3d9_viewport_active() { return g_active_vp; }

void render_d3d9_camera_create(void* key, void* parent, void* viewport,
                               int32_t pri, float half_aov_deg, float dmin,
                               float dmax, float lod_bias, float lod_amp,
                               int32_t oc, int32_t pt) {
  if (!key) return;
  CameraState& cam = g_cameras[key];
  cam.parent = parent;
  cam.viewport = viewport;
  cam.pri = pri;
  cam.half_aov_deg = half_aov_deg;
  cam.dmin = dmin;
  cam.dmax = dmax;
  cam.lod_bias = lod_bias;
  cam.lod_amp = lod_amp;
  cam.oc = oc;
  cam.pt = pt;
  cam.active = false;
  cam.lookat = false;
}

void render_d3d9_camera_destroy(void* key) {
  if (!key) return;
  if (g_active_cam == key) g_active_cam = nullptr;
  g_cameras.erase(key);
}

void render_d3d9_camera_activate(void* key, void* viewport, int32_t pri) {
  if (!key) return;
  auto it = g_cameras.find(key);
  if (it == g_cameras.end()) {
    render_d3d9_camera_create(key, nullptr, viewport, pri, 45.f, 0.1f, 100.f,
                              1.f, 1.f, 1, 0);
    it = g_cameras.find(key);
  }
  if (viewport) it->second.viewport = viewport;
  it->second.pri = pri;
  if (g_active_cam && g_active_cam != key) {
    auto prev = g_cameras.find(g_active_cam);
    if (prev != g_cameras.end()) prev->second.active = false;
  }
  g_active_cam = key;
  it->second.active = true;
  if (it->second.viewport) {
    render_d3d9_viewport_activate(it->second.viewport, kViewportClearDepth);
  }
  apply_active_camera();
}

void render_d3d9_camera_deactivate(void* key, void* /*viewport*/) {
  if (!key) return;
  auto it = g_cameras.find(key);
  if (it != g_cameras.end()) it->second.active = false;
  if (g_active_cam == key) g_active_cam = nullptr;
}

void* render_d3d9_camera_active() { return g_active_cam; }

float render_d3d9_camera_half_aov(void* key) {
  auto it = g_cameras.find(key);
  return it == g_cameras.end() ? 0.f : it->second.half_aov_deg;
}

float render_d3d9_camera_dmin(void* key) {
  auto it = g_cameras.find(key);
  return it == g_cameras.end() ? 0.f : it->second.dmin;
}

float render_d3d9_camera_dmax(void* key) {
  auto it = g_cameras.find(key);
  return it == g_cameras.end() ? 0.f : it->second.dmax;
}

void render_d3d9_camera_lookat(void* key, float eye_x, float eye_y, float eye_z,
                               float at_x, float at_y, float at_z) {
  if (!key) return;
  CameraState& cam = g_cameras[key];
  cam.lookat = true;
  cam.eye_x = eye_x;
  cam.eye_y = eye_y;
  cam.eye_z = eye_z;
  cam.at_x = at_x;
  cam.at_y = at_y;
  cam.at_z = at_z;
  if (g_active_cam == key) apply_active_camera();
}

void render_d3d9_camera_chase(void* key, float px, float py, float pz, float yaw,
                              float dist, float height, float look_height) {
  if (!key) return;
  if (dist < 0.5f) dist = 0.5f;
  const float fx = std::sin(yaw);
  const float fz = std::cos(yaw);
  const float eye_x = px - fx * dist;
  const float eye_y = py + height;
  const float eye_z = pz - fz * dist;
  const float at_x = px + fx * 2.f;
  const float at_y = py + look_height;
  const float at_z = pz + fz * 2.f;
  render_d3d9_camera_lookat(key, eye_x, eye_y, eye_z, at_x, at_y, at_z);
}

bool render_d3d9_camera_get_lookat(void* key, float* eye_x, float* eye_y,
                                   float* eye_z, float* at_x, float* at_y,
                                   float* at_z) {
  auto it = g_cameras.find(key);
  if (it == g_cameras.end() || !it->second.lookat) return false;
  const CameraState& cam = it->second;
  if (eye_x) *eye_x = cam.eye_x;
  if (eye_y) *eye_y = cam.eye_y;
  if (eye_z) *eye_z = cam.eye_z;
  if (at_x) *at_x = cam.at_x;
  if (at_y) *at_y = cam.at_y;
  if (at_z) *at_z = cam.at_z;
  return true;
}

namespace {
int32_t g_printscreen_count = 0;
std::string g_printscreen_last;
}  // namespace

bool render_d3d9_viewport_unproject(void* vp, void* cam, float vx, float vy,
                                    float* out_x, float* out_y, float* out_z) {
  if (!out_x || !out_y || !out_z) return false;
  void* ckey = cam ? cam : g_active_cam;
  float eye_x = 0, eye_y = 2.f, eye_z = -8.f;
  float at_x = 0, at_y = 0.f, at_z = 0.f;
  float half_aov = 45.f;
  if (ckey) {
    auto it = g_cameras.find(ckey);
    if (it != g_cameras.end()) {
      half_aov = it->second.half_aov_deg;
      if (it->second.lookat) {
        eye_x = it->second.eye_x;
        eye_y = it->second.eye_y;
        eye_z = it->second.eye_z;
        at_x = it->second.at_x;
        at_y = it->second.at_y;
        at_z = it->second.at_z;
      }
    }
  }
  float aspect = render_d3d9_viewport_get_aspect(vp);
  if (aspect < 0.1f) {
    const float ww = static_cast<float>(render_d3d9_width());
    const float wh = static_cast<float>(render_d3d9_height());
    aspect = (wh > 1.f) ? (ww / wh) : (4.f / 3.f);
  }
  float fx = at_x - eye_x;
  float fy = at_y - eye_y;
  float fz = at_z - eye_z;
  float flen = std::sqrt(fx * fx + fy * fy + fz * fz);
  if (flen < 1e-5f) {
    fx = 0.f;
    fy = 0.f;
    fz = 1.f;
    flen = 1.f;
  }
  fx /= flen;
  fy /= flen;
  fz /= flen;
  // World up; fall back if looking nearly vertical.
  float ux = 0.f, uy = 1.f, uz = 0.f;
  float rx = fy * uz - fz * uy;
  float ry = fz * ux - fx * uz;
  float rz = fx * uy - fy * ux;
  float rlen = std::sqrt(rx * rx + ry * ry + rz * rz);
  if (rlen < 1e-5f) {
    ux = 1.f;
    uy = 0.f;
    uz = 0.f;
    rx = fy * uz - fz * uy;
    ry = fz * ux - fx * uz;
    rz = fx * uy - fy * ux;
    rlen = std::sqrt(rx * rx + ry * ry + rz * rz);
  }
  rx /= rlen;
  ry /= rlen;
  rz /= rlen;
  ux = ry * fz - rz * fy;
  uy = rz * fx - rx * fz;
  uz = rx * fy - ry * fx;
  const float tan_a = std::tan(half_aov * 0.01745329252f);
  float dx = fx + rx * (vx * tan_a * aspect) + ux * (vy * tan_a);
  float dy = fy + ry * (vx * tan_a * aspect) + uy * (vy * tan_a);
  float dz = fz + rz * (vx * tan_a * aspect) + uz * (vy * tan_a);
  const float dlen = std::sqrt(dx * dx + dy * dy + dz * dz);
  if (dlen > 1e-6f) {
    dx /= dlen;
    dy /= dlen;
    dz /= dlen;
  }
  const float plane_y = at_y;
  float t = 10.f;
  if (std::fabs(dy) > 1e-5f) t = (plane_y - eye_y) / dy;
  if (t < 0.1f) t = 0.1f;
  if (t > 5000.f) t = 5000.f;
  *out_x = eye_x + dx * t;
  *out_y = eye_y + dy * t;
  *out_z = eye_z + dz * t;
  return true;
}

bool render_d3d9_print_screen(const char* path) {
  if (!path || !path[0]) return false;
  FILE* f = nullptr;
#if defined(_MSC_VER)
  fopen_s(&f, path, "wb");
#else
  f = std::fopen(path, "wb");
#endif
  if (!f) return false;
  // Tiny marker (not a real BMP) — enough for scripts to see a file appear.
  static const char kMagic[] = {'S', 'C', 'R', 'N'};
  std::fwrite(kMagic, 1, 4, f);
  std::fclose(f);
  ++g_printscreen_count;
  g_printscreen_last = path;
  return true;
}

int32_t render_d3d9_print_screen_count() { return g_printscreen_count; }

const char* render_d3d9_print_screen_last() {
  return g_printscreen_last.c_str();
}

void render_d3d9_set_fog(int32_t color_rgb, float near_z, float far_z) {
  // Host D3D stand-in. PE Camera.setFog @ 0x00486570 writes a nested fog
  // object (no SetRenderState); GroundRef.setFog @ 0x00486A20 sends packet 0x4A.
  g_fog.enabled = true;
  g_fog.color = color_rgb & 0x00ffffff;
  g_fog.near_z = near_z;
  g_fog.far_z = far_z > near_z ? far_z : near_z + 1.f;
}

void render_d3d9_clear_fog() { g_fog = FogState{}; }

bool render_d3d9_fog_enabled() { return g_fog.enabled; }

int32_t render_d3d9_fog_color() { return g_fog.color; }

float render_d3d9_fog_near() { return g_fog.near_z; }

float render_d3d9_fog_far() { return g_fog.far_z; }

void render_d3d9_set_light(int32_t diffuse_rgb, int32_t ambient_rgb,
                           int32_t specular_rgb) {
  g_light.enabled = true;
  g_light.diffuse = diffuse_rgb & 0x00ffffff;
  g_light.ambient = ambient_rgb & 0x00ffffff;
  g_light.specular = specular_rgb & 0x00ffffff;
}

void render_d3d9_clear_light() { g_light = LightState{}; }

bool render_d3d9_light_enabled() { return g_light.enabled; }

int32_t render_d3d9_light_diffuse() { return g_light.diffuse; }

int32_t render_d3d9_light_ambient() { return g_light.ambient; }

int32_t render_d3d9_light_specular() { return g_light.specular; }

void render_d3d9_set_flare(void* key, void* glow_tex, int32_t glow_color,
                           float glow_min, float glow_max, int32_t flare_count,
                           int32_t ray_count) {
  if (!key) return;
  for (FlareState& f : g_flares) {
    if (f.key == key) {
      f.tex = glow_tex;
      f.color = glow_color;
      f.min_size = glow_min;
      f.max_size = glow_max;
      f.count = flare_count;
      f.rays = ray_count;
      return;
    }
  }
  FlareState f;
  f.key = key;
  f.tex = glow_tex;
  f.color = glow_color;
  f.min_size = glow_min;
  f.max_size = glow_max;
  f.count = flare_count;
  f.rays = ray_count;
  g_flares.push_back(f);
}

void render_d3d9_set_flare_world(void* key, float wx, float wy, float wz) {
  if (!key) return;
  for (FlareState& f : g_flares) {
    if (f.key == key) {
      f.has_world = true;
      f.wx = wx;
      f.wy = wy;
      f.wz = wz;
      return;
    }
  }
}

void render_d3d9_clear_flare(void* key) {
  if (!key) {
    g_flares.clear();
    return;
  }
  for (size_t i = 0; i < g_flares.size();) {
    if (g_flares[i].key == key)
      g_flares.erase(g_flares.begin() + static_cast<std::ptrdiff_t>(i));
    else
      ++i;
  }
}

int32_t render_d3d9_flare_sources() {
  return static_cast<int32_t>(g_flares.size());
}

int32_t render_d3d9_flare_sprites_last() { return g_flare_sprites_last; }

void render_d3d9_set_flares_enabled(bool on) { g_flares_enabled = on; }

bool render_d3d9_flares_enabled() { return g_flares_enabled; }

bool render_d3d9_flare_screen_pos(void* key, float* sx, float* sy) {
  if (!key) return false;
  for (const FlareState& f : g_flares) {
    if (f.key != key) continue;
    if (!f.has_screen || f.behind) return false;
    if (sx) *sx = f.sx;
    if (sy) *sy = f.sy;
    return true;
  }
  return false;
}

bool render_d3d9_project(float wx, float wy, float wz, float* ndc_x,
                         float* ndc_y) {
  if (!ndc_x || !ndc_y) return false;
  void* ckey = g_active_cam;
  float eye_x = 0, eye_y = 2.f, eye_z = -8.f;
  float at_x = 0, at_y = 0.f, at_z = 0.f;
  float half_aov = 45.f;
  if (ckey) {
    auto it = g_cameras.find(ckey);
    if (it != g_cameras.end()) {
      half_aov = it->second.half_aov_deg;
      if (it->second.lookat) {
        eye_x = it->second.eye_x;
        eye_y = it->second.eye_y;
        eye_z = it->second.eye_z;
        at_x = it->second.at_x;
        at_y = it->second.at_y;
        at_z = it->second.at_z;
      }
    }
  }
  float aspect = 4.f / 3.f;
  if (g_active_vp) {
    aspect = render_d3d9_viewport_get_aspect(g_active_vp);
  }
  if (aspect < 0.1f) {
    const float ww = static_cast<float>(g_w > 0 ? g_w : 800);
    const float wh = static_cast<float>(g_h > 0 ? g_h : 600);
    aspect = (wh > 1.f) ? (ww / wh) : (4.f / 3.f);
  }
  float fx = at_x - eye_x;
  float fy = at_y - eye_y;
  float fz = at_z - eye_z;
  float flen = std::sqrt(fx * fx + fy * fy + fz * fz);
  if (flen < 1e-5f) {
    fx = 0.f;
    fy = 0.f;
    fz = 1.f;
    flen = 1.f;
  }
  fx /= flen;
  fy /= flen;
  fz /= flen;
  float ux = 0.f, uy = 1.f, uz = 0.f;
  float rx = fy * uz - fz * uy;
  float ry = fz * ux - fx * uz;
  float rz = fx * uy - fy * ux;
  float rlen = std::sqrt(rx * rx + ry * ry + rz * rz);
  if (rlen < 1e-5f) {
    ux = 1.f;
    uy = 0.f;
    uz = 0.f;
    rx = fy * uz - fz * uy;
    ry = fz * ux - fx * uz;
    rz = fx * uy - fy * ux;
    rlen = std::sqrt(rx * rx + ry * ry + rz * rz);
  }
  if (rlen < 1e-5f) return false;
  rx /= rlen;
  ry /= rlen;
  rz /= rlen;
  ux = ry * fz - rz * fy;
  uy = rz * fx - rx * fz;
  uz = rx * fy - ry * fx;
  const float px = wx - eye_x;
  const float py = wy - eye_y;
  const float pz = wz - eye_z;
  const float cam_z = px * fx + py * fy + pz * fz;
  if (cam_z < 0.05f) return false;
  const float cam_x = px * rx + py * ry + pz * rz;
  const float cam_y = px * ux + py * uy + pz * uz;
  const float tan_a = std::tan(half_aov * 0.01745329252f);
  if (tan_a < 1e-6f) return false;
  *ndc_x = cam_x / (cam_z * tan_a * aspect);
  *ndc_y = cam_y / (cam_z * tan_a);
  return true;
}

namespace {

#pragma pack(push, 1)
struct DdsPixelFormat {
  uint32_t size;
  uint32_t flags;
  uint32_t fourcc;
  uint32_t rgb_bit_count;
  uint32_t r_mask;
  uint32_t g_mask;
  uint32_t b_mask;
  uint32_t a_mask;
};
struct DdsHeader {
  uint32_t size;
  uint32_t flags;
  uint32_t height;
  uint32_t width;
  uint32_t pitch_or_linear;
  uint32_t depth;
  uint32_t mipmap_count;
  uint32_t reserved1[11];
  DdsPixelFormat pf;
  uint32_t caps;
  uint32_t caps2;
  uint32_t caps3;
  uint32_t caps4;
  uint32_t reserved2;
};
#pragma pack(pop)

constexpr uint32_t kDdsMagic = 0x20534444u;  // 'DDS '
constexpr uint32_t kDdpfFourcc = 0x4;
constexpr uint32_t kDdpfRgb = 0x40;
constexpr uint32_t kDdpfAlphapixels = 0x1;

uint32_t fourcc_u32(const char* s) {
  return static_cast<uint32_t>(static_cast<uint8_t>(s[0])) |
         (static_cast<uint32_t>(static_cast<uint8_t>(s[1])) << 8) |
         (static_cast<uint32_t>(static_cast<uint8_t>(s[2])) << 16) |
         (static_cast<uint32_t>(static_cast<uint8_t>(s[3])) << 24);
}

size_t dxt_level_size(uint32_t w, uint32_t h, bool dxt1) {
  const uint32_t bw = w > 0 ? ((w + 3) / 4) : 1;
  const uint32_t bh = h > 0 ? ((h + 3) / 4) : 1;
  return static_cast<size_t>(bw) * static_cast<size_t>(bh) *
         (dxt1 ? 8u : 16u);
}

bool upload_dds(TextureState& st, const uint8_t* data, size_t size) {
#ifdef _WIN32
  if (!g_dev || !data || size < 4 + sizeof(DdsHeader)) return false;
  uint32_t magic = 0;
  std::memcpy(&magic, data, 4);
  if (magic != kDdsMagic) return false;
  DdsHeader hdr{};
  std::memcpy(&hdr, data + 4, sizeof(hdr));
  if (hdr.size != 124 || hdr.pf.size != 32) return false;
  if (hdr.width == 0 || hdr.height == 0) return false;

  D3DFORMAT fmt = D3DFMT_UNKNOWN;
  bool compressed = false;
  bool dxt1 = false;
  uint32_t bpp = 0;
  if (hdr.pf.flags & kDdpfFourcc) {
    compressed = true;
    if (hdr.pf.fourcc == fourcc_u32("DXT1")) {
      fmt = D3DFMT_DXT1;
      dxt1 = true;
    } else if (hdr.pf.fourcc == fourcc_u32("DXT3")) {
      fmt = D3DFMT_DXT3;
    } else if (hdr.pf.fourcc == fourcc_u32("DXT5")) {
      fmt = D3DFMT_DXT5;
    } else {
      return false;
    }
  } else if (hdr.pf.flags & kDdpfRgb) {
    bpp = hdr.pf.rgb_bit_count / 8;
    if (bpp == 4)
      fmt = D3DFMT_A8R8G8B8;
    else if (bpp == 3)
      fmt = D3DFMT_R8G8B8;
    else if (bpp == 2)
      fmt = D3DFMT_R5G6B5;
    else
      return false;
  } else {
    return false;
  }

  int32_t mips = static_cast<int32_t>(hdr.mipmap_count);
  if (mips < 1) mips = 1;

  if (st.tex) {
    st.tex->Release();
    st.tex = nullptr;
  }
  // Reloading DDS replaces GPU tex — drop luma-alpha cache so createBG can
  // re-derive alpha (otherwise a 2nd GENERALBG stays opaque DXT1).
  st.luma_alpha = false;
  HRESULT hr =
      g_dev->CreateTexture(hdr.width, hdr.height, static_cast<UINT>(mips), 0,
                           fmt, D3DPOOL_MANAGED, &st.tex, nullptr);
  if (FAILED(hr) || !st.tex) return false;

  const uint8_t* src = data + 4 + sizeof(DdsHeader);
  size_t remain = size - (4 + sizeof(DdsHeader));
  uint32_t mw = hdr.width;
  uint32_t mh = hdr.height;
  for (int32_t level = 0; level < mips; ++level) {
    size_t level_bytes = 0;
    if (compressed) {
      level_bytes = dxt_level_size(mw, mh, dxt1);
    } else {
      level_bytes = static_cast<size_t>(mw) * static_cast<size_t>(mh) * bpp;
    }
    if (level_bytes > remain) {
      st.tex->Release();
      st.tex = nullptr;
      return false;
    }
    D3DLOCKED_RECT lr{};
    if (FAILED(st.tex->LockRect(static_cast<UINT>(level), &lr, nullptr, 0))) {
      st.tex->Release();
      st.tex = nullptr;
      return false;
    }
    if (compressed || lr.Pitch == static_cast<INT>(mw * bpp)) {
      std::memcpy(lr.pBits, src, level_bytes);
    } else {
      const uint8_t* row = src;
      uint8_t* dst = static_cast<uint8_t*>(lr.pBits);
      const size_t row_bytes = static_cast<size_t>(mw) * bpp;
      for (uint32_t y = 0; y < mh; ++y) {
        std::memcpy(dst, row, row_bytes);
        row += row_bytes;
        dst += lr.Pitch;
      }
    }
    st.tex->UnlockRect(static_cast<UINT>(level));
    src += level_bytes;
    remain -= level_bytes;
    if (mw > 1) mw /= 2;
    if (mh > 1) mh /= 2;
  }

  st.w = static_cast<int32_t>(hdr.width);
  st.h = static_cast<int32_t>(hdr.height);
  st.mips = mips;
  st.luma_alpha = false;
  return true;
#else
  (void)st;
  (void)data;
  (void)size;
  return false;
#endif
}

bool upload_jpeg_bgra(TextureState& st, const uint8_t* jpeg, size_t jpeg_size) {
#ifdef _WIN32
  if (!g_dev || !jpeg || jpeg_size < 4) return false;
  if (jpeg[0] != 0xff || jpeg[1] != 0xd8) return false;

  static bool com_inited = false;
  if (!com_inited) {
    const HRESULT chr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(chr) || chr == RPC_E_CHANGED_MODE) com_inited = true;
  }

  IWICImagingFactory* factory = nullptr;
  HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
  if (FAILED(hr) || !factory) return false;

  IWICStream* stream = nullptr;
  hr = factory->CreateStream(&stream);
  if (FAILED(hr) || !stream) {
    factory->Release();
    return false;
  }
  hr = stream->InitializeFromMemory(const_cast<BYTE*>(jpeg),
                                    static_cast<DWORD>(jpeg_size));
  if (FAILED(hr)) {
    stream->Release();
    factory->Release();
    return false;
  }

  IWICBitmapDecoder* decoder = nullptr;
  hr = factory->CreateDecoderFromStream(stream, nullptr,
                                        WICDecodeMetadataCacheOnLoad, &decoder);
  stream->Release();
  if (FAILED(hr) || !decoder) {
    factory->Release();
    return false;
  }

  IWICBitmapFrameDecode* frame = nullptr;
  hr = decoder->GetFrame(0, &frame);
  decoder->Release();
  if (FAILED(hr) || !frame) {
    factory->Release();
    return false;
  }

  IWICFormatConverter* conv = nullptr;
  hr = factory->CreateFormatConverter(&conv);
  if (FAILED(hr) || !conv) {
    frame->Release();
    factory->Release();
    return false;
  }
  hr = conv->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
                        nullptr, 0.0, WICBitmapPaletteTypeCustom);
  frame->Release();
  if (FAILED(hr)) {
    conv->Release();
    factory->Release();
    return false;
  }

  UINT w = 0, h = 0;
  conv->GetSize(&w, &h);
  if (w == 0 || h == 0 || w > 8192 || h > 8192) {
    conv->Release();
    factory->Release();
    return false;
  }
  const UINT stride = w * 4;
  const UINT bytes = stride * h;
  std::vector<uint8_t> pixels(bytes);
  hr = conv->CopyPixels(nullptr, stride, bytes, pixels.data());
  conv->Release();
  factory->Release();
  if (FAILED(hr)) return false;

  if (st.tex) {
    st.tex->Release();
    st.tex = nullptr;
  }
  hr = g_dev->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &st.tex,
                            nullptr);
  if (FAILED(hr) || !st.tex) return false;
  D3DLOCKED_RECT lr{};
  if (FAILED(st.tex->LockRect(0, &lr, nullptr, 0))) {
    st.tex->Release();
    st.tex = nullptr;
    return false;
  }
  if (lr.Pitch == static_cast<INT>(stride)) {
    std::memcpy(lr.pBits, pixels.data(), bytes);
  } else {
    const uint8_t* src = pixels.data();
    uint8_t* dst = static_cast<uint8_t*>(lr.pBits);
    for (UINT y = 0; y < h; ++y) {
      std::memcpy(dst, src, stride);
      src += stride;
      dst += lr.Pitch;
    }
  }
  st.tex->UnlockRect(0);
  st.w = static_cast<int32_t>(w);
  st.h = static_cast<int32_t>(h);
  st.mips = 1;
  return true;
#else
  (void)st;
  (void)jpeg;
  (void)jpeg_size;
  return false;
#endif
}

bool parse_ptx_jpeg(const uint8_t* data, size_t size, const uint8_t** jpeg_out,
                    size_t* jpeg_size_out, int32_t* w_out, int32_t* h_out) {
  if (!data || size < 40 || !jpeg_out || !jpeg_size_out) return false;
  uint32_t ver = 0, w = 0, h = 0, jsz = 0;
  std::memcpy(&ver, data + 4, 4);
  std::memcpy(&w, data + 8, 4);
  std::memcpy(&h, data + 12, 4);
  std::memcpy(&jsz, data + 16, 4);
  if (ver != 1 || w == 0 || h == 0 || jsz < 4) return false;
  constexpr size_t kHdr = 36;
  if (kHdr + static_cast<size_t>(jsz) > size) return false;
  if (data[kHdr] != 0xff || data[kHdr + 1] != 0xd8) return false;
  *jpeg_out = data + kHdr;
  *jpeg_size_out = jsz;
  if (w_out) *w_out = static_cast<int32_t>(w);
  if (h_out) *h_out = static_cast<int32_t>(h);
  return true;
}

}  // namespace

bool render_d3d9_texture_create_from_ptx(void* key, const uint8_t* data,
                                         size_t size, const char* label) {
  if (!key || !data) return false;
  const uint8_t* jpeg = nullptr;
  size_t jsz = 0;
  int32_t pw = 0, ph = 0;
  if (!parse_ptx_jpeg(data, size, &jpeg, &jsz, &pw, &ph)) return false;
  TextureState& st = g_textures[key];
  st.label = label ? label : "";
  if (upload_jpeg_bgra(st, jpeg, jsz)) return true;
  // Keep atlas size from PTX header so ResourceRef.load / skydome bind can
  // still attach the type (Scene.java RenderRef + maps.skydome textures).
  st.w = pw;
  st.h = ph;
  st.mips = 1;
  return st.w > 0 && st.h > 0;
}

bool render_d3d9_texture_create_from_memory(void* key, const uint8_t* data,
                                            size_t size, const char* label) {
  if (!key || !data || size == 0) return false;
  // PTX wrapper (skydome / city diffuse atlases).
  {
    const uint8_t* jpeg = nullptr;
    size_t jsz = 0;
    if (parse_ptx_jpeg(data, size, &jpeg, &jsz, nullptr, nullptr))
      return render_d3d9_texture_create_from_ptx(key, data, size, label);
  }
  TextureState& st = g_textures[key];
  st.label = label ? label : "";
  if (!upload_dds(st, data, size)) {
    // Keep registry entry for type bookkeeping even if upload failed / no device.
    if (size >= 128 && data[0] == 'D' && data[1] == 'D' && data[2] == 'S') {
      DdsHeader hdr{};
      std::memcpy(&hdr, data + 4, sizeof(hdr));
      st.w = static_cast<int32_t>(hdr.width);
      st.h = static_cast<int32_t>(hdr.height);
      st.mips = static_cast<int32_t>(hdr.mipmap_count > 0 ? hdr.mipmap_count : 1);
    }
#ifdef _WIN32
    return st.tex != nullptr;
#else
    return st.w > 0;
#endif
  }
  return true;
}

bool render_d3d9_texture_create_from_file(void* key, const char* path) {
  if (!key || !path || !path[0]) return false;
  FILE* f = std::fopen(path, "rb");
  if (!f) return false;
  if (std::fseek(f, 0, SEEK_END) != 0) {
    std::fclose(f);
    return false;
  }
  const long sz = std::ftell(f);
  if (sz <= 0) {
    std::fclose(f);
    return false;
  }
  std::fseek(f, 0, SEEK_SET);
  std::vector<uint8_t> buf(static_cast<size_t>(sz));
  const size_t n = std::fread(buf.data(), 1, buf.size(), f);
  std::fclose(f);
  if (n != buf.size()) return false;
  return render_d3d9_texture_create_from_memory(key, buf.data(), buf.size(),
                                                path);
}

bool render_d3d9_texture_create_solid(void* key, uint32_t argb, int32_t size) {
  if (!key || size < 2) return false;
  if (size > 64) size = 64;
  TextureState& st = g_textures[key];
  st.label = "solid";
  st.w = size;
  st.h = size;
  st.mips = 1;
#ifdef _WIN32
  if (!g_dev) return true;
  if (st.tex) {
    st.tex->Release();
    st.tex = nullptr;
  }
  if (FAILED(g_dev->CreateTexture(static_cast<UINT>(size),
                                  static_cast<UINT>(size), 1, 0, D3DFMT_A8R8G8B8,
                                  D3DPOOL_MANAGED, &st.tex, nullptr)) ||
      !st.tex)
    return false;
  D3DLOCKED_RECT lr{};
  if (FAILED(st.tex->LockRect(0, &lr, nullptr, 0))) {
    st.tex->Release();
    st.tex = nullptr;
    return false;
  }
  const float cx = (size - 1) * 0.5f;
  const float r = cx * 0.92f;
  const float r2 = r * r;
  for (int32_t y = 0; y < size; ++y) {
    auto* row = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(lr.pBits) +
                                            y * lr.Pitch);
    for (int32_t x = 0; x < size; ++x) {
      const float dx = static_cast<float>(x) - cx;
      const float dy = static_cast<float>(y) - cx;
      row[x] = (dx * dx + dy * dy <= r2) ? argb : 0x00000000u;
    }
  }
  st.tex->UnlockRect(0);
  return true;
#else
  (void)argb;
  return true;
#endif
}

namespace {

bool looks_like_dds(const uint8_t* data, size_t size) {
  return data && size >= 128 && data[0] == 'D' && data[1] == 'D' &&
         data[2] == 'S' && data[3] == ' ';
}

void normalize_slashes(std::string* s) {
  for (char& c : *s) {
    if (c == '\\') c = '/';
  }
}

std::vector<std::string> parse_sourcefile_paths(const uint8_t* data, size_t size) {
  std::vector<std::string> out;
  if (!data || size == 0) return out;
  size_t i = 0;
  while (i < size) {
    size_t line_end = i;
    while (line_end < size && data[line_end] != '\n' && data[line_end] != '\r')
      ++line_end;
    std::string line(reinterpret_cast<const char*>(data + i), line_end - i);
    // Trim
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t'))
      line.pop_back();
    size_t start = 0;
    while (start < line.size() && (line[start] == ' ' || line[start] == '\t'))
      ++start;
    line = line.substr(start);
    const char* k = "sourcefile";
    if (line.size() >= 10 &&
        std::strncmp(line.c_str(), k, 10) == 0) {
      size_t p = 10;
      if (p < line.size() && line[p] == '=') ++p;
      while (p < line.size() && (line[p] == ' ' || line[p] == '\t')) ++p;
      if (p < line.size()) {
        std::string path = line.substr(p);
        normalize_slashes(&path);
        out.push_back(path);
      }
    }
    i = line_end;
    while (i < size && (data[i] == '\n' || data[i] == '\r')) ++i;
  }
  return out;
}

bool path_ends_with_ci(const std::string& path, const std::string& suffix) {
  if (suffix.size() > path.size()) return false;
  for (size_t i = 0; i < suffix.size(); ++i) {
    char a = path[path.size() - suffix.size() + i];
    char b = suffix[i];
    if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
    if (a != b) return false;
  }
  return true;
}

}  // namespace

bool render_d3d9_texture_create_from_rpak(void* key, const uint8_t* blob,
                                          size_t blob_size,
                                          const char* entry_name,
                                          const char* entry_path) {
  if (!key) return false;
  if (looks_like_dds(blob, blob_size)) {
    return render_d3d9_texture_create_from_memory(key, blob, blob_size,
                                                  entry_path ? entry_path : "");
  }

  auto paths = parse_sourcefile_paths(blob, blob_size);
  std::string pick;
  if (!paths.empty()) {
    std::string want;
    if (entry_name && entry_name[0]) {
      want = entry_name;
      normalize_slashes(&want);
    }
    if (!want.empty()) {
      for (const auto& p : paths) {
        if (path_ends_with_ci(p, want) || path_ends_with_ci(p, want + ".dds")) {
          pick = p;
          break;
        }
      }
    }
    if (pick.empty()) {
      for (const auto& p : paths) {
        if (path_ends_with_ci(p, ".dds") || path_ends_with_ci(p, ".ptx")) {
          pick = p;
          break;
        }
      }
    }
    if (pick.empty()) pick = paths.front();
  }

  if (pick.empty() && entry_path && entry_path[0]) {
    pick = entry_path;
    normalize_slashes(&pick);
  }
  if (pick.empty() && entry_name && entry_name[0]) {
    pick = entry_name;
    normalize_slashes(&pick);
  }
  if (pick.empty()) return false;

  std::string resolved = rpak_resolve_path(pick.c_str());
  if (resolved.empty()) resolved = pick;
  if (render_d3d9_texture_create_from_file(key, resolved.c_str())) return true;

  // Fallback: try under textures/ next to a basename.
  const auto slash = pick.find_last_of('/');
  const std::string base =
      slash == std::string::npos ? pick : pick.substr(slash + 1);
  if (!base.empty() && base != pick) {
    resolved = rpak_resolve_path(base.c_str());
    if (!resolved.empty() &&
        render_d3d9_texture_create_from_file(key, resolved.c_str()))
      return true;
  }
  return false;
}

void render_d3d9_texture_destroy(void* key) {
  if (!key) return;
  auto it = g_textures.find(key);
  if (it == g_textures.end()) return;
#ifdef _WIN32
  if (it->second.tex) it->second.tex->Release();
#endif
  if (g_envmap == key) g_envmap = nullptr;
  g_textures.erase(it);
}

bool render_d3d9_texture_ready(void* key) {
  auto it = g_textures.find(key);
  if (it == g_textures.end()) return false;
#ifdef _WIN32
  // GPU upload preferred; PTX/JPEG may still expose decoded dims when WIC/D3D
  // upload fails (headless timing / COM). Stock skydome recipes need the bind.
  if (it->second.tex != nullptr) return true;
  return it->second.w > 0 && it->second.h > 0;
#else
  return it->second.w > 0;
#endif
}

namespace {

#ifdef _WIN32
void rgb565_to_rgb(uint16_t c, uint8_t* r, uint8_t* g, uint8_t* b) {
  *r = static_cast<uint8_t>(((c >> 11) & 31) * 255 / 31);
  *g = static_cast<uint8_t>(((c >> 5) & 63) * 255 / 63);
  *b = static_cast<uint8_t>((c & 31) * 255 / 31);
}

void decode_dxt1_block(const uint8_t* block, uint8_t out_bgra[16][4]) {
  const uint16_t c0 = static_cast<uint16_t>(block[0] | (block[1] << 8));
  const uint16_t c1 = static_cast<uint16_t>(block[2] | (block[3] << 8));
  uint8_t r[4], g[4], b[4];
  rgb565_to_rgb(c0, &r[0], &g[0], &b[0]);
  rgb565_to_rgb(c1, &r[1], &g[1], &b[1]);
  if (c0 > c1) {
    r[2] = static_cast<uint8_t>((2 * r[0] + r[1]) / 3);
    g[2] = static_cast<uint8_t>((2 * g[0] + g[1]) / 3);
    b[2] = static_cast<uint8_t>((2 * b[0] + b[1]) / 3);
    r[3] = static_cast<uint8_t>((r[0] + 2 * r[1]) / 3);
    g[3] = static_cast<uint8_t>((g[0] + 2 * g[1]) / 3);
    b[3] = static_cast<uint8_t>((b[0] + 2 * b[1]) / 3);
  } else {
    r[2] = static_cast<uint8_t>((r[0] + r[1]) / 2);
    g[2] = static_cast<uint8_t>((g[0] + g[1]) / 2);
    b[2] = static_cast<uint8_t>((b[0] + b[1]) / 2);
    r[3] = g[3] = b[3] = 0;
  }
  uint32_t bits = static_cast<uint32_t>(block[4]) |
                  (static_cast<uint32_t>(block[5]) << 8) |
                  (static_cast<uint32_t>(block[6]) << 16) |
                  (static_cast<uint32_t>(block[7]) << 24);
  for (int i = 0; i < 16; ++i) {
    const int idx = static_cast<int>((bits >> (2 * i)) & 3u);
    out_bgra[i][0] = b[idx];
    out_bgra[i][1] = g[idx];
    out_bgra[i][2] = r[idx];
    out_bgra[i][3] = 255;
  }
}
#endif

}  // namespace

// createBG plates (GENERALBG): raw luma→alpha makes letterbox bands glass.
// Remap into a heavy range so FMV still peeks through highlights only.
uint8_t luma_to_osd_bg_alpha(uint8_t rr, uint8_t gg, uint8_t bb) {
  int a = rr;
  if (gg > a) a = gg;
  if (bb > a) a = bb;
  constexpr int kFloor = 215;
  return static_cast<uint8_t>(kFloor + (a * (255 - kFloor)) / 255);
}

bool render_d3d9_texture_apply_luma_alpha(void* key) {
#ifdef _WIN32
  if (!g_dev || !key) return false;
  auto it = g_textures.find(key);
  if (it == g_textures.end() || !it->second.tex) return false;
  TextureState& st = it->second;
  if (st.luma_alpha) return true;

  D3DSURFACE_DESC desc{};
  if (FAILED(st.tex->GetLevelDesc(0, &desc)) || desc.Width == 0 ||
      desc.Height == 0)
    return false;

  IDirect3DTexture9* out = nullptr;
  if (FAILED(g_dev->CreateTexture(desc.Width, desc.Height, 1, 0,
                                  D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &out,
                                  nullptr)) ||
      !out)
    return false;

  D3DLOCKED_RECT src_lr{};
  D3DLOCKED_RECT dst_lr{};
  if (FAILED(st.tex->LockRect(0, &src_lr, nullptr, D3DLOCK_READONLY)) ||
      FAILED(out->LockRect(0, &dst_lr, nullptr, 0))) {
    if (src_lr.pBits) st.tex->UnlockRect(0);
    out->Release();
    return false;
  }

  auto* dst_base = static_cast<uint8_t*>(dst_lr.pBits);
  const int w = static_cast<int>(desc.Width);
  const int h = static_cast<int>(desc.Height);

  if (desc.Format == D3DFMT_DXT1) {
    const auto* src = static_cast<const uint8_t*>(src_lr.pBits);
    const int bw = (w + 3) / 4;
    const int bh = (h + 3) / 4;
    for (int by = 0; by < bh; ++by) {
      for (int bx = 0; bx < bw; ++bx) {
        uint8_t px[16][4];
        decode_dxt1_block(src + (by * bw + bx) * 8, px);
        for (int k = 0; k < 16; ++k) {
          const int x = bx * 4 + (k % 4);
          const int y = by * 4 + (k / 4);
          if (x >= w || y >= h) continue;
          const uint8_t bb = px[k][0], gg = px[k][1], rr = px[k][2];
          uint8_t* d = dst_base + y * dst_lr.Pitch + x * 4;
          d[0] = bb;
          d[1] = gg;
          d[2] = rr;
          d[3] = luma_to_osd_bg_alpha(rr, gg, bb);
        }
      }
    }
  } else {
    // Already uncompressed — rewrite alpha from luma, keep RGB.
    for (int y = 0; y < h; ++y) {
      const auto* s =
          static_cast<const uint8_t*>(src_lr.pBits) + y * src_lr.Pitch;
      auto* d = dst_base + y * dst_lr.Pitch;
      for (int x = 0; x < w; ++x) {
        uint8_t bb = 0, gg = 0, rr = 0;
        if (desc.Format == D3DFMT_A8R8G8B8 || desc.Format == D3DFMT_X8R8G8B8) {
          bb = s[x * 4 + 0];
          gg = s[x * 4 + 1];
          rr = s[x * 4 + 2];
        } else if (desc.Format == D3DFMT_R5G6B5) {
          const uint16_t c = static_cast<uint16_t>(s[x * 2] | (s[x * 2 + 1] << 8));
          rgb565_to_rgb(c, &rr, &gg, &bb);
        } else {
          out->UnlockRect(0);
          st.tex->UnlockRect(0);
          out->Release();
          return false;
        }
        d[x * 4 + 0] = bb;
        d[x * 4 + 1] = gg;
        d[x * 4 + 2] = rr;
        d[x * 4 + 3] = luma_to_osd_bg_alpha(rr, gg, bb);
      }
    }
  }

  out->UnlockRect(0);
  st.tex->UnlockRect(0);
  st.tex->Release();
  st.tex = out;
  st.mips = 1;
  st.luma_alpha = true;
  return true;
#else
  (void)key;
  return false;
#endif
}

int32_t render_d3d9_texture_width(void* key) {
  auto it = g_textures.find(key);
  return it == g_textures.end() ? 0 : it->second.w;
}

int32_t render_d3d9_texture_height(void* key) {
  auto it = g_textures.find(key);
  return it == g_textures.end() ? 0 : it->second.h;
}

const char* render_d3d9_texture_label(void* key) {
  auto it = g_textures.find(key);
  return it == g_textures.end() ? "" : it->second.label.c_str();
}

void render_d3d9_set_global_envmap(void* key) {
  // PE @ 0x0047C220: cmp current(+0x64) vs handle @ 0x47C24A → jz no-op.
  // handle 0 @ loc_47C2B8 zeros node (+0x58..+0x64). Else unlink/relink
  // list; host has no resource+0x48 list — assign g_envmap only.
  if (g_envmap == key) return;
  g_envmap = key;
}

void* render_d3d9_global_envmap() { return g_envmap; }

void render_d3d9_osd_clear() {
  g_osd.clear();
  g_osd_text.clear();
}

void render_d3d9_osd_add_rect(float x, float y, float w, float h, void* texture,
                              int32_t pri) {
  OsdRect r;
  r.x = x;
  r.y = y;
  r.w = w;
  r.h = h;
  r.texture = texture;
  r.pri = pri;
  g_osd.push_back(r);
}

void render_d3d9_osd_set_rect(void* key, float x, float y, float w, float h,
                              void* texture, int32_t pri) {
  render_d3d9_osd_set_rect_color(key, x, y, w, h, texture, 0xFFFFFFFFu, pri);
}

void render_d3d9_osd_set_rect_color(void* key, float x, float y, float w,
                                    float h, void* texture, uint32_t argb,
                                    int32_t pri) {
  if (key) {
    for (auto& r : g_osd) {
      if (r.key == key) {
        r.x = x;
        r.y = y;
        r.w = w;
        r.h = h;
        r.texture = texture;
        r.pri = pri;
        r.color = argb ? argb : 0xFFFFFFFFu;
        return;
      }
    }
  }
  OsdRect r;
  r.key = key;
  r.x = x;
  r.y = y;
  r.w = w;
  r.h = h;
  r.texture = texture;
  r.pri = pri;
  r.color = argb ? argb : 0xFFFFFFFFu;
  g_osd.push_back(r);
}

void render_d3d9_osd_set_rect_visible(void* key, int32_t visible) {
  if (!key) return;
  for (auto& r : g_osd) {
    if (r.key == key) {
      r.visible = visible != 0;
      return;
    }
  }
}

void render_d3d9_osd_remove_rect(void* key) {
  if (!key) return;
  for (size_t i = 0; i < g_osd.size();) {
    if (g_osd[i].key == key)
      g_osd.erase(g_osd.begin() + static_cast<std::ptrdiff_t>(i));
    else
      ++i;
  }
}

int32_t render_d3d9_osd_count() {
  return static_cast<int32_t>(g_osd.size());
}

int32_t render_d3d9_osd_text_count() {
  return static_cast<int32_t>(g_osd_text.size());
}

namespace {

int32_t font_px_height_from_name(const std::string& name) {
  std::string n = name;
  for (char& c : n) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  // Mirror Text.getFontSize RID table.
  if (n.find("simple40") != std::string::npos) return 40;
  if (n.find("slii24") != std::string::npos) return 24;
  if (n.find("simple20") != std::string::npos) return 20;
  if (n.find("slii17") != std::string::npos) return 15;
  if (n.find("slii11") != std::string::npos) return 10;
  if (n.find("console10") != std::string::npos) return 10;
  if (n.find("console5") != std::string::npos) return 7;
  if (n.find("sl28") != std::string::npos) return 28;
  if (n.find("sl14") != std::string::npos) return 14;
  return 20;
}

bool load_file_bytes(const std::string& path, std::vector<uint8_t>* out) {
  if (!out) return false;
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) {
    std::string resolved = rpak_resolve_path(path.c_str());
    if (!resolved.empty()) f = std::fopen(resolved.c_str(), "rb");
  }
  if (!f) return false;
  std::fseek(f, 0, SEEK_END);
  const long sz = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (sz <= 0 || sz > 32 * 1024 * 1024) {
    std::fclose(f);
    return false;
  }
  out->resize(static_cast<size_t>(sz));
  const bool ok = std::fread(out->data(), 1, out->size(), f) == out->size();
  std::fclose(f);
  return ok;
}

// Decode PNG/JPEG/etc via WIC into a font atlas.
// SLII*.png are LA: white fill + black outline with coverage alpha — preserve
// BGRA. Greyscale coverage atlases (no dark-with-alpha outline) still use
// white RGB + luma→alpha so Text.changeColor can tint.
bool upload_wic_font_atlas(void* key, const uint8_t* data, size_t size,
                           const char* label) {
#ifdef _WIN32
  if (!g_dev || !key || !data || size < 8) return false;

  static bool com_inited = false;
  if (!com_inited) {
    const HRESULT chr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(chr) || chr == RPC_E_CHANGED_MODE) com_inited = true;
  }

  IWICImagingFactory* factory = nullptr;
  HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
  if (FAILED(hr) || !factory) return false;

  IWICStream* stream = nullptr;
  hr = factory->CreateStream(&stream);
  if (FAILED(hr) || !stream) {
    factory->Release();
    return false;
  }
  hr = stream->InitializeFromMemory(const_cast<BYTE*>(data),
                                    static_cast<DWORD>(size));
  if (FAILED(hr)) {
    stream->Release();
    factory->Release();
    return false;
  }

  IWICBitmapDecoder* decoder = nullptr;
  hr = factory->CreateDecoderFromStream(stream, nullptr,
                                        WICDecodeMetadataCacheOnLoad, &decoder);
  stream->Release();
  if (FAILED(hr) || !decoder) {
    factory->Release();
    return false;
  }

  IWICBitmapFrameDecode* frame = nullptr;
  hr = decoder->GetFrame(0, &frame);
  decoder->Release();
  if (FAILED(hr) || !frame) {
    factory->Release();
    return false;
  }

  IWICFormatConverter* conv = nullptr;
  hr = factory->CreateFormatConverter(&conv);
  if (FAILED(hr) || !conv) {
    frame->Release();
    factory->Release();
    return false;
  }
  hr = conv->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
                        nullptr, 0.0, WICBitmapPaletteTypeCustom);
  frame->Release();
  if (FAILED(hr)) {
    conv->Release();
    factory->Release();
    return false;
  }

  UINT w = 0, h = 0;
  conv->GetSize(&w, &h);
  if (w == 0 || h == 0 || w > 8192 || h > 8192) {
    conv->Release();
    factory->Release();
    return false;
  }
  const UINT stride = w * 4;
  const UINT bytes = stride * h;
  std::vector<uint8_t> pixels(bytes);
  hr = conv->CopyPixels(nullptr, stride, bytes, pixels.data());
  conv->Release();
  factory->Release();
  if (FAILED(hr)) return false;

  // Detect baked outline (black RGB + non-zero alpha), e.g. SLII LA PNGs.
  bool outline_atlas = false;
  for (UINT i = 0; i < w * h; ++i) {
    const uint8_t* p = pixels.data() + i * 4;
    const int luma =
        (static_cast<int>(p[0]) + static_cast<int>(p[1]) + static_cast<int>(p[2])) /
        3;
    if (luma < 40 && p[3] > 16) {
      outline_atlas = true;
      break;
    }
  }
  if (!outline_atlas) {
    // Coverage mask: opaque RGB → alpha; RGB white for diffuse tint.
    for (UINT i = 0; i < w * h; ++i) {
      uint8_t* p = pixels.data() + i * 4;
      const int a = (static_cast<int>(p[0]) + p[1] + p[2]) / 3;
      p[0] = 0xff;
      p[1] = 0xff;
      p[2] = 0xff;
      p[3] = static_cast<uint8_t>(a);
    }
  }

  TextureState& st = g_textures[key];
  st.label = label ? label : "";
  if (st.tex) {
    st.tex->Release();
    st.tex = nullptr;
  }
  if (FAILED(g_dev->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
                                  &st.tex, nullptr)) ||
      !st.tex)
    return false;
  D3DLOCKED_RECT lr{};
  if (FAILED(st.tex->LockRect(0, &lr, nullptr, 0))) {
    st.tex->Release();
    st.tex = nullptr;
    return false;
  }
  if (lr.Pitch == static_cast<INT>(stride)) {
    std::memcpy(lr.pBits, pixels.data(), bytes);
  } else {
    const uint8_t* src = pixels.data();
    uint8_t* dst = static_cast<uint8_t*>(lr.pBits);
    for (UINT y = 0; y < h; ++y) {
      std::memcpy(dst, src, stride);
      src += stride;
      dst += lr.Pitch;
    }
  }
  st.tex->UnlockRect(0);
  st.w = static_cast<int32_t>(w);
  st.h = static_cast<int32_t>(h);
  st.mips = 1;
  return true;
#else
  (void)key;
  (void)data;
  (void)size;
  (void)label;
  return false;
#endif
}

bool upload_tga_font_atlas(void* key, const uint8_t* data, size_t size,
                           const char* label) {
#ifdef _WIN32
  if (!g_dev || !key || !data || size < 18) return false;
  const uint8_t id_len = data[0];
  const uint8_t cmap_type = data[1];
  const uint8_t img_type = data[2];
  const uint16_t cmap_first =
      static_cast<uint16_t>(data[3] | (data[4] << 8));
  const uint16_t cmap_len =
      static_cast<uint16_t>(data[5] | (data[6] << 8));
  const uint8_t cmap_entry_bits = data[7];
  const uint16_t w = static_cast<uint16_t>(data[12] | (data[13] << 8));
  const uint16_t h = static_cast<uint16_t>(data[14] | (data[15] << 8));
  const uint8_t bpp = data[16];
  const uint8_t desc = data[17];
  if (w == 0 || h == 0 || bpp != 8) return false;
  // Type 3 = greyscale; type 1 = indexed (SLII*_detect.tga).
  if (img_type != 3 && img_type != 1) return false;
  if (img_type == 1 && (cmap_type != 1 || cmap_len == 0 || cmap_entry_bits < 24))
    return false;

  const size_t cmap_bytes =
      img_type == 1
          ? static_cast<size_t>(cmap_len) * (cmap_entry_bits / 8)
          : 0;
  const size_t header = 18u + id_len + cmap_bytes;
  const size_t need = header + static_cast<size_t>(w) * static_cast<size_t>(h);
  if (size < need) return false;

  // Build 256 greyscale alpha samples from palette (or identity for type 3).
  uint8_t alpha_lut[256];
  for (int i = 0; i < 256; ++i) alpha_lut[i] = static_cast<uint8_t>(i);
  if (img_type == 1) {
    std::memset(alpha_lut, 0, sizeof(alpha_lut));
    const uint8_t* pal = data + 18u + id_len;
    const int epb = cmap_entry_bits / 8;
    for (uint16_t i = 0; i < cmap_len && i < 256; ++i) {
      const uint8_t* e = pal + static_cast<size_t>(i) * epb;
      // TGA palette is BGR(A).
      const uint8_t b = e[0], g = e[1], r = e[2];
      const int a = (static_cast<int>(r) + g + b) / 3;
      const int idx = static_cast<int>(cmap_first) + i;
      if (idx >= 0 && idx < 256) alpha_lut[idx] = static_cast<uint8_t>(a);
    }
  }

  TextureState& st = g_textures[key];
  st.label = label ? label : "";
  if (st.tex) {
    st.tex->Release();
    st.tex = nullptr;
  }
  if (FAILED(g_dev->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
                                  &st.tex, nullptr)) ||
      !st.tex)
    return false;

  D3DLOCKED_RECT lr{};
  if (FAILED(st.tex->LockRect(0, &lr, nullptr, 0))) {
    st.tex->Release();
    st.tex = nullptr;
    return false;
  }
  const bool top_left = (desc & 0x20) != 0;
  for (uint16_t y = 0; y < h; ++y) {
    const uint16_t src_y = top_left ? y : static_cast<uint16_t>(h - 1 - y);
    const uint8_t* src = data + header + static_cast<size_t>(src_y) * w;
    uint8_t* dst = static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch;
    for (uint16_t x = 0; x < w; ++x) {
      const uint8_t a = alpha_lut[src[x]];
      dst[x * 4 + 0] = 0xff;
      dst[x * 4 + 1] = 0xff;
      dst[x * 4 + 2] = 0xff;
      dst[x * 4 + 3] = a;
    }
  }
  st.tex->UnlockRect(0);
  st.w = w;
  st.h = h;
  st.mips = 1;
  return true;
#else
  (void)key;
  (void)data;
  (void)size;
  (void)label;
  return false;
#endif
}

// Invictus font SCX (INVO v3, hdr=0x38): vcount@0x40, verts@0x44, stride 44,
// uv at +24. Glyphs are indexed by ASCII code (z ≈ -100*ascii), 4 verts each.
bool parse_font_glyphs(const uint8_t* scx, size_t scx_size,
                       std::vector<FontGlyph>* out) {
  if (!scx || !out || scx_size < 0x80) return false;
  out->clear();
  if (std::memcmp(scx, "INVO", 4) != 0) return false;
  uint32_t ver = 0, vcount = 0;
  std::memcpy(&ver, scx + 4, 4);
  std::memcpy(&vcount, scx + 0x40, 4);
  if (ver != 3 || vcount < 4 || (vcount % 4) != 0 || vcount > 4096) return false;
  constexpr size_t kStride = 44;
  constexpr size_t kVStart = 0x44;
  const uint64_t need =
      static_cast<uint64_t>(kVStart) +
      static_cast<uint64_t>(vcount) * static_cast<uint64_t>(kStride);
  if (need > scx_size) return false;

  const size_t nglyphs = static_cast<size_t>(vcount / 4);
  out->reserve(nglyphs);
  int ok = 0;
  for (size_t g = 0; g < nglyphs; ++g) {
    float umin = 2.f, umax = -1.f, vmin = 2.f, vmax = -1.f;
    for (int i = 0; i < 4; ++i) {
      const size_t off =
          kVStart + (g * 4 + static_cast<size_t>(i)) * kStride;
      float u = 0, v = 0;
      std::memcpy(&u, scx + off + 24, 4);
      std::memcpy(&v, scx + off + 28, 4);
      if (!(u >= 0.f && u <= 1.01f && v >= 0.f && v <= 1.01f)) {
        umin = umax = vmin = vmax = 0.f;
        break;
      }
      if (u < umin) umin = u;
      if (u > umax) umax = u;
      if (v < vmin) vmin = v;
      if (v > vmax) vmax = v;
    }
    FontGlyph glyph;
    glyph.u0 = umin;
    glyph.v0 = vmin;
    glyph.u1 = umax;
    glyph.v1 = vmax;
    out->push_back(glyph);
    if ((umax - umin) > 0.002f && (vmax - vmin) > 0.015f) ++ok;
  }
  return ok >= 32;
}

}  // namespace

bool render_d3d9_font_load(void* key, const char* name) {
  if (!key || !name || !name[0]) return false;
  std::string n = name;
  // Strip path / extension.
  {
    const size_t slash = n.find_last_of("/\\");
    if (slash != std::string::npos) n = n.substr(slash + 1);
    const size_t dot = n.find_last_of('.');
    if (dot != std::string::npos) n = n.substr(0, dot);
  }

  std::string up = n;
  for (char& c : up) {
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
  }

  std::string scx = "frontend/meshes/Fonts/" + n + ".scx";
  std::string charset_path = "frontend/font.dat";

  std::vector<uint8_t> scx_bytes, atlas_bytes, cs_bytes;
  if (!load_file_bytes(scx, &scx_bytes)) {
    scx = "frontend/meshes/Fonts/" + n + ".scx";
    if (!load_file_bytes(scx, &scx_bytes)) return false;
  }
  if (!load_file_bytes(charset_path, &cs_bytes) || cs_bytes.empty()) return false;

  // Prefer real atlases (TGA then PNG). Never prefer *_detect.tga — those are
  // debug maps and produce garbage glyphs (SLII* ships as SLII24.png).
  enum class AtlasKind { Tga, Wic };
  AtlasKind kind = AtlasKind::Tga;
  std::string atlas_path;
  auto try_atlas = [&](const std::string& path, AtlasKind k) -> bool {
    if (!load_file_bytes(path, &atlas_bytes) || atlas_bytes.empty()) return false;
    atlas_path = path;
    kind = k;
    return true;
  };
  if (!try_atlas("frontend/textures/Fonts/" + n + ".tga", AtlasKind::Tga) &&
      !try_atlas("frontend/textures/Fonts/" + up + ".tga", AtlasKind::Tga) &&
      !try_atlas("frontend/textures/Fonts/" + n + ".png", AtlasKind::Wic) &&
      !try_atlas("frontend/textures/Fonts/" + up + ".png", AtlasKind::Wic) &&
      !try_atlas("frontend/textures/Fonts/" + up + "_detect.tga", AtlasKind::Tga) &&
      !try_atlas("frontend/textures/Fonts/" + n + "_detect.tga", AtlasKind::Tga)) {
    return false;
  }

  void* atlas_key =
      reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(key) + 0x20000u);
  const bool uploaded =
      (kind == AtlasKind::Tga)
          ? upload_tga_font_atlas(atlas_key, atlas_bytes.data(),
                                  atlas_bytes.size(), atlas_path.c_str())
          : upload_wic_font_atlas(atlas_key, atlas_bytes.data(),
                                  atlas_bytes.size(), atlas_path.c_str());
  if (!uploaded) {
    // No device / WIC fail: keep dims so font_ready + Text metrics still work
    // (Valocity HUD smoke without --window; mirrors PTX skydome path).
    int32_t pw = 0, ph = 0;
    bool dims_ok = false;
    if (kind == AtlasKind::Tga && atlas_bytes.size() >= 18) {
      pw = static_cast<int32_t>(
          atlas_bytes[12] | (static_cast<uint16_t>(atlas_bytes[13]) << 8));
      ph = static_cast<int32_t>(
          atlas_bytes[14] | (static_cast<uint16_t>(atlas_bytes[15]) << 8));
      dims_ok = pw > 0 && ph > 0;
    } else if (atlas_bytes.size() >= 24 && atlas_bytes[0] == 0x89 &&
               atlas_bytes[1] == 'P' && atlas_bytes[2] == 'N' &&
               atlas_bytes[3] == 'G') {
      const uint32_t w = (static_cast<uint32_t>(atlas_bytes[16]) << 24) |
                         (static_cast<uint32_t>(atlas_bytes[17]) << 16) |
                         (static_cast<uint32_t>(atlas_bytes[18]) << 8) |
                         static_cast<uint32_t>(atlas_bytes[19]);
      const uint32_t h = (static_cast<uint32_t>(atlas_bytes[20]) << 24) |
                         (static_cast<uint32_t>(atlas_bytes[21]) << 16) |
                         (static_cast<uint32_t>(atlas_bytes[22]) << 8) |
                         static_cast<uint32_t>(atlas_bytes[23]);
      if (w > 0 && h > 0 && w <= 8192 && h <= 8192) {
        pw = static_cast<int32_t>(w);
        ph = static_cast<int32_t>(h);
        dims_ok = true;
      }
    }
    if (!dims_ok) {
      render_d3d9_texture_destroy(atlas_key);
      return false;
    }
    TextureState& tst = g_textures[atlas_key];
    tst.label = atlas_path;
    tst.w = pw;
    tst.h = ph;
    tst.mips = 1;
  }
  std::printf("[font] %s atlas=%s\n", n.c_str(), atlas_path.c_str());

  FontState st;
  st.name = n;
  st.atlas_key = atlas_key;
  st.atlas_w = render_d3d9_texture_width(atlas_key);
  st.atlas_h = render_d3d9_texture_height(atlas_key);
  st.px_height = font_px_height_from_name(n);
  st.charset.assign(reinterpret_cast<const char*>(cs_bytes.data()),
                    cs_bytes.size());
  if (!parse_font_glyphs(scx_bytes.data(), scx_bytes.size(), &st.glyphs)) {
    render_d3d9_texture_destroy(atlas_key);
    return false;
  }
  for (FontGlyph& g : st.glyphs) {
    g.adv_px = (g.u1 - g.u0) * static_cast<float>(st.atlas_w);
    if (g.adv_px < 1.f) g.adv_px = static_cast<float>(st.px_height) * 0.35f;
  }
  st.ready = !st.glyphs.empty();
  {
    auto it = g_fonts.find(key);
    if (it != g_fonts.end() && it->second.atlas_key &&
        it->second.atlas_key != atlas_key)
      render_d3d9_texture_destroy(it->second.atlas_key);
  }
  g_fonts[key] = std::move(st);
  return g_fonts[key].ready;
}

bool render_d3d9_font_load_from_rid(void* key, int32_t res_id) {
  if (!key || res_id == 0) return false;
  const RpakEntry* ent = rpak_find_entry(res_id);
  if (!ent || ent->name.empty()) return false;
  return render_d3d9_font_load(key, ent->name.c_str());
}

bool render_d3d9_font_ready(void* key) {
  auto it = g_fonts.find(key);
  return it != g_fonts.end() && it->second.ready;
}

const char* render_d3d9_font_name(void* key) {
  auto it = g_fonts.find(key);
  if (it == g_fonts.end() || !it->second.ready) return "";
  return it->second.name.c_str();
}

int32_t render_d3d9_font_glyph_count(void* key) {
  auto it = g_fonts.find(key);
  if (it == g_fonts.end()) return 0;
  return static_cast<int32_t>(it->second.glyphs.size());
}

int32_t render_d3d9_font_px_height(void* key) {
  auto it = g_fonts.find(key);
  if (it == g_fonts.end() || !it->second.ready) return 0;
  return it->second.px_height;
}

float render_d3d9_font_measure_px(void* key, const char* text) {
  auto it = g_fonts.find(key);
  if (it == g_fonts.end() || !it->second.ready || !text) return 0.f;
  const FontState& font = it->second;
  float w = 0.f;
  for (const char* p = text; *p; ++p) {
    const unsigned char ch = static_cast<unsigned char>(*p);
    if (ch == ' ') {
      w += static_cast<float>(font.px_height) * 0.35f;
      continue;
    }
    const size_t gi = static_cast<size_t>(ch);
    if (gi >= font.glyphs.size()) continue;
    w += font.glyphs[gi].adv_px;
  }
  return w;
}

void render_d3d9_font_destroy(void* key) {
  auto it = g_fonts.find(key);
  if (it == g_fonts.end()) return;
  if (it->second.atlas_key) render_d3d9_texture_destroy(it->second.atlas_key);
  g_fonts.erase(it);
}

void render_d3d9_text_create(void* key, void* font, float x, float y) {
  if (!key) return;
  TextState& t = g_texts[key];
  t.font = font;
  t.x = x;
  t.y = y;
  t.color = 0xFFFFFFFFu;
  t.align = 2;
  t.text.clear();
  t.alive = true;
}

void render_d3d9_text_destroy(void* key) {
  if (!key) return;
  for (size_t i = 0; i < g_osd_text.size();) {
    if (g_osd_text[i].text_key == key)
      g_osd_text.erase(g_osd_text.begin() + static_cast<std::ptrdiff_t>(i));
    else
      ++i;
  }
  g_texts.erase(key);
}

void render_d3d9_text_set_color(void* key, uint32_t argb) {
  auto it = g_texts.find(key);
  if (it == g_texts.end()) return;
  it->second.color = argb;
  for (auto& ot : g_osd_text) {
    if (ot.text_key == key) ot.color = argb;
  }
}

void render_d3d9_text_set_align(void* key, int32_t align) {
  auto it = g_texts.find(key);
  if (it == g_texts.end()) return;
  it->second.align = align;
}

void render_d3d9_text_set_pos(void* key, float x, float y) {
  auto it = g_texts.find(key);
  if (it == g_texts.end()) return;
  it->second.x = x;
  it->second.y = y;
}

void render_d3d9_text_set_string(void* key, const char* utf8) {
  auto it = g_texts.find(key);
  if (it == g_texts.end()) return;
  it->second.text = utf8 ? utf8 : "";
}

void render_d3d9_text_set_visible(void* key, int32_t visible) {
  auto it = g_texts.find(key);
  if (it == g_texts.end()) return;
  it->second.visible = visible != 0;
  for (auto& ot : g_osd_text) {
    if (ot.text_key == key) ot.visible = it->second.visible;
  }
}

const char* render_d3d9_text_get_string(void* key) {
  auto it = g_texts.find(key);
  if (it == g_texts.end() || !it->second.alive) return "";
  return it->second.text.c_str();
}

void render_d3d9_text_update(void* key) {
  auto it = g_texts.find(key);
  if (it == g_texts.end() || !it->second.alive) return;
  const TextState& t = it->second;
  // Replace existing OSD entry for this text instance.
  for (size_t i = 0; i < g_osd_text.size();) {
    if (g_osd_text[i].text_key == key)
      g_osd_text.erase(g_osd_text.begin() + static_cast<std::ptrdiff_t>(i));
    else
      ++i;
  }
  if (t.text.empty() || !t.font) return;
  OsdText ot;
  ot.text_key = key;
  ot.font = t.font;
  ot.x = t.x;
  ot.y = t.y;
  ot.color = t.color;
  ot.align = t.align;
  ot.text = t.text;
  ot.pri = 0;
  ot.visible = t.visible;
  g_osd_text.push_back(std::move(ot));
}

namespace {

bool read_u32(const uint8_t* data, size_t size, size_t off, uint32_t* out) {
  if (off + 4 > size || !out) return false;
  std::memcpy(out, data + off, 4);
  return true;
}

std::string scx_mat_name(const uint8_t* data, size_t size, uint32_t off,
                         uint32_t end) {
  if (off + 24 > end || end > size) return {};
  // Observed at +0x14: big-endian length (or 3 zero pad + len byte), then ASCII.
  // e.g. 00 00 00 08 "F grill\0" / 00 00 00 08 "hood\0..."
  const uint32_t p = off + 0x14;
  if (p + 4 >= end) return {};
  uint32_t n = (static_cast<uint32_t>(data[p]) << 24) |
               (static_cast<uint32_t>(data[p + 1]) << 16) |
               (static_cast<uint32_t>(data[p + 2]) << 8) |
               static_cast<uint32_t>(data[p + 3]);
  if (n == 0 || n > 64 || p + 4 + n > end) return {};
  uint32_t len = n;
  while (len > 0 && data[p + 4 + len - 1] == 0) --len;
  if (len == 0) return {};
  for (uint32_t i = 0; i < len; ++i) {
    const uint8_t c = data[p + 4 + i];
    if (c < 32 || c > 126) return {};
  }
  return std::string(reinterpret_cast<const char*>(data + p + 4), len);
}

uint32_t scx_mat_diffuse(const uint8_t* data, size_t size, uint32_t off,
                         uint32_t end) {
  if (off + 64 > end || end > size) return 0xFF808080u;
  // First 0xAARRGGBB-ish dword with alpha 0xFF after the name field.
  for (uint32_t p = off + 0x30; p + 4 <= end; p += 4) {
    uint32_t c = 0;
    if (!read_u32(data, size, p, &c)) break;
    if ((c >> 24) == 0xffu) return c;
  }
  return 0xFF808080u;
}

std::string ascii_lower(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return s;
}

std::string mat_name_to_token(const std::string& name) {
  std::string out;
  out.reserve(name.size());
  for (char c : name) {
    if (c == ' ' || c == '-' || c == '/')
      out.push_back('_');
    else
      out.push_back(c);
  }
  return out;
}

std::string guess_car_prefix(const std::filesystem::path& scx_path) {
  // .../Baiern_data/meshes/hood.scx → Baiern
  std::filesystem::path cur = scx_path;
  for (int i = 0; i < 4 && !cur.empty(); ++i) {
    std::string stem = cur.stem().string();
    if (stem.size() > 5) {
      const std::string low = ascii_lower(stem);
      if (low.size() >= 5 && low.compare(low.size() - 5, 5, "_data") == 0)
        return stem.substr(0, stem.size() - 5);
    }
    if (ascii_lower(cur.filename().string()) == "meshes") {
      std::string parent = cur.parent_path().stem().string();
      const std::string low = ascii_lower(parent);
      if (low.size() >= 5 && low.compare(low.size() - 5, 5, "_data") == 0)
        return parent.substr(0, parent.size() - 5);
      if (!parent.empty()) return parent;
    }
    cur = cur.parent_path();
  }
  return {};
}

std::filesystem::path guess_textures_dir(const std::filesystem::path& scx_path) {
  namespace fs = std::filesystem;
  const fs::path parent = scx_path.parent_path();
  fs::path cand = parent / "textures";
  if (fs::is_directory(cand)) return cand;
  cand = parent.parent_path() / "textures";
  if (fs::is_directory(cand)) return cand;
  return parent.parent_path() / "textures";
}

std::string resolve_mat_texture_path(const std::filesystem::path& scx_path,
                                     const std::string& mat_name) {
  namespace fs = std::filesystem;
  if (mat_name.empty()) return {};
  const fs::path texdir = guess_textures_dir(scx_path);
  if (!fs::is_directory(texdir)) return {};

  const std::string prefix = guess_car_prefix(scx_path);
  const std::string token = mat_name_to_token(mat_name);
  const std::string token_l = ascii_lower(token);
  const std::string prefix_l = ascii_lower(prefix);

  // Prefer exact / short matches: Prefix_token.dds then Prefix_token_*.dds
  std::string best;
  int best_score = -1;
  std::error_code ec;
  for (fs::directory_iterator it(texdir, ec); !ec && it != fs::directory_iterator();
       it.increment(ec)) {
    if (!it->is_regular_file(ec)) continue;
    const std::string fname = it->path().filename().string();
    const std::string fl = ascii_lower(fname);
    if (fl.size() < 5 || fl.compare(fl.size() - 4, 4, ".dds") != 0) continue;

    int score = -1;
    if (!prefix_l.empty()) {
      const std::string exact = prefix_l + "_" + token_l + ".dds";
      if (fl == exact) score = 1000;
      else {
        const std::string stem = prefix_l + "_" + token_l + "_";
        if (fl.compare(0, stem.size(), stem) == 0) {
          // Prefer shorter suffix (base variants over CITY etc. not in name).
          score = 800 - static_cast<int>(fl.size());
        }
      }
    }
    if (score < 0) {
      // Fallback: filename contains token (e.g. rearview_mirror).
      if (fl.find(token_l) != std::string::npos)
        score = 400 - static_cast<int>(fl.size());
    }
    if (score > best_score) {
      best_score = score;
      best = it->path().string();
    }
  }
  return best;
}

void* mesh_sub_tex_key(void* mesh_key, size_t index) {
  return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(mesh_key) +
                                 0x10000u + index + 1u);
}

void bind_mesh_textures(void* mesh_key, MeshState& st) {
  if (!mesh_key) return;
  std::filesystem::path scx;
  if (!st.label.empty()) scx = st.label;
  for (size_t i = 0; i < st.subs.size(); ++i) {
    MeshSubmesh& sm = st.subs[i];
    if (sm.name.empty()) continue;
    const std::string path = resolve_mat_texture_path(scx, sm.name);
    if (path.empty()) continue;
    void* tkey = mesh_sub_tex_key(mesh_key, i);
    if (render_d3d9_texture_create_from_file(tkey, path.c_str()) &&
        render_d3d9_texture_ready(tkey)) {
      sm.texture_key = tkey;
      sm.texture_path = path;
    } else {
      render_d3d9_texture_destroy(tkey);
      sm.texture_key = nullptr;
      sm.texture_path.clear();
    }
  }
}

void release_mesh_textures(void* mesh_key, MeshState& st) {
  for (size_t i = 0; i < st.subs.size(); ++i) {
    MeshSubmesh& sm = st.subs[i];
    if (sm.texture_key && mesh_key &&
        sm.texture_key == mesh_sub_tex_key(mesh_key, i))
      render_d3d9_texture_destroy(sm.texture_key);
    sm.texture_key = nullptr;
    sm.texture_path.clear();
  }
}

bool parse_scx_v4(const uint8_t* data, size_t size, MeshState& out) {
  if (!data || size < 24) return false;
  if (std::memcmp(data, "INVO", 4) != 0) return false;
  uint32_t ver = 0, nchunk = 0, rsv = 0;
  if (!read_u32(data, size, 4, &ver) || !read_u32(data, size, 8, &nchunk) ||
      !read_u32(data, size, 12, &rsv))
    return false;
  if (ver != 4 || nchunk == 0 || nchunk > 4096) return false;
  if (16ull + static_cast<uint64_t>(nchunk) * 8ull > size) return false;

  struct Chunk {
    uint32_t off;
    uint32_t typ;
  };
  std::vector<Chunk> chunks(nchunk);
  for (uint32_t i = 0; i < nchunk; ++i) {
    if (!read_u32(data, size, 16 + i * 8, &chunks[i].off) ||
        !read_u32(data, size, 16 + i * 8 + 4, &chunks[i].typ))
      return false;
    if (chunks[i].off >= size) return false;
  }

  auto chunk_end = [&](uint32_t i) -> uint32_t {
    if (i + 1 < nchunk) return chunks[i + 1].off;
    return static_cast<uint32_t>(size);
  };

  out.subs.clear();
  bool bounds_init = false;

  for (uint32_t i = 0; i < nchunk; ++i) {
    if (chunks[i].typ != 1) continue;
    // Find verts(5) and indices(0) after this material.
    int v_i = -1;
    int x_i = -1;
    for (uint32_t j = i + 1; j < nchunk; ++j) {
      if (chunks[j].typ == 1) break;
      if (chunks[j].typ == 5 && v_i < 0) v_i = static_cast<int>(j);
      if (chunks[j].typ == 0 && x_i < 0) x_i = static_cast<int>(j);
    }
    if (v_i < 0 || x_i < 0) continue;

    const uint32_t voff = chunks[v_i].off;
    const uint32_t vend = chunk_end(static_cast<uint32_t>(v_i));
    const uint32_t ioff = chunks[x_i].off;
    const uint32_t iend = chunk_end(static_cast<uint32_t>(x_i));
    if (voff + 16 > vend || ioff + 12 > iend) continue;

    uint32_t vtag = 0, vsize = 0, vcount = 0, vflags = 0;
    if (!read_u32(data, size, voff, &vtag) ||
        !read_u32(data, size, voff + 4, &vsize) ||
        !read_u32(data, size, voff + 8, &vcount) ||
        !read_u32(data, size, voff + 12, &vflags))
      continue;
    (void)vtag;
    (void)vflags;
    if (vcount == 0 || vcount > 500000) continue;
    // Phase 2.44: stride from chunk payload — 0x0241→32 (pos+n+uv),
    // 0x0041→24 (pos+n, glass). Fall back to 32.
    uint32_t stride = 32;
    if (vsize >= 16) {
      const uint32_t payload = vsize - 16;
      if (payload % vcount == 0) {
        const uint32_t s = payload / vcount;
        if (s == 24 || s == 32 || s == 36 || s == 40) stride = s;
      }
    }
    const uint64_t need =
        static_cast<uint64_t>(voff) + 16ull +
        static_cast<uint64_t>(vcount) * static_cast<uint64_t>(stride);
    if (need > vend || need > size) continue;

    uint32_t itag = 0, isize = 0, icount = 0;
    if (!read_u32(data, size, ioff, &itag) ||
        !read_u32(data, size, ioff + 4, &isize) ||
        !read_u32(data, size, ioff + 8, &icount))
      continue;
    (void)itag;
    (void)isize;
    if (icount < 3 || icount > 1500000 || (icount % 3) != 0) continue;
    if (static_cast<uint64_t>(ioff) + 12ull +
            static_cast<uint64_t>(icount) * 2ull >
        iend)
      continue;

    MeshSubmesh sm;
    sm.name = scx_mat_name(data, size, chunks[i].off, chunk_end(i));
    sm.diffuse = scx_mat_diffuse(data, size, chunks[i].off, chunk_end(i));
    sm.verts.resize(vcount);
    const uint8_t* vsrc = data + voff + 16;
    for (uint32_t vi = 0; vi < vcount; ++vi) {
      MeshVertex& v = sm.verts[vi];
      const uint8_t* p = vsrc + static_cast<size_t>(vi) * stride;
      std::memcpy(&v.x, p, 12);
      if (stride >= 24)
        std::memcpy(&v.nx, p + 12, 12);
      else
        v.nx = v.ny = v.nz = 0.f;
      if (stride >= 32)
        std::memcpy(&v.u, p + 24, 8);
      else
        v.u = v.v = 0.f;
      // Drop insane/NaN positions (rare bad verts in light meshes).
      if (!(v.x == v.x) || !(v.y == v.y) || !(v.z == v.z) ||
          std::fabs(v.x) > 1.0e6f || std::fabs(v.y) > 1.0e6f ||
          std::fabs(v.z) > 1.0e6f) {
        v.x = v.y = v.z = 0.f;
        v.nx = 0.f;
        v.ny = 1.f;
        v.nz = 0.f;
      }
    }
    sm.indices.resize(icount);
    std::memcpy(sm.indices.data(), data + ioff + 12, icount * 2);

    uint16_t max_i = 0;
    for (uint16_t idx : sm.indices) {
      if (idx > max_i) max_i = idx;
    }
    if (max_i >= vcount) continue;

    for (const MeshVertex& v : sm.verts) {
      if (!bounds_init) {
        out.bmin[0] = out.bmax[0] = v.x;
        out.bmin[1] = out.bmax[1] = v.y;
        out.bmin[2] = out.bmax[2] = v.z;
        bounds_init = true;
      } else {
        if (v.x < out.bmin[0]) out.bmin[0] = v.x;
        if (v.y < out.bmin[1]) out.bmin[1] = v.y;
        if (v.z < out.bmin[2]) out.bmin[2] = v.z;
        if (v.x > out.bmax[0]) out.bmax[0] = v.x;
        if (v.y > out.bmax[1]) out.bmax[1] = v.y;
        if (v.z > out.bmax[2]) out.bmax[2] = v.z;
      }
    }
    out.subs.push_back(std::move(sm));
  }

  out.ready = !out.subs.empty();
  return out.ready;
}

// Phase 2.51/2.54 — INVO v3 visual mesh (skydome / older parts).
// File: INVO + ver=3 + mat blocks. Each mat block (no magic after the first):
//   +0x00 hdr_size (≥0x80), +0x04 floats, +0x68 name, +0x88 vcount,
//   +0x8C verts stride 64 (pos+n+uv in first 32), then u32 tri_count +
//   tri_count×3 uint32 indices. Phase 2.54: loop subsequent mat blocks.
// Fonts use hdr=0x38 — skip.
bool parse_scx_v3(const uint8_t* data, size_t size, MeshState& out) {
  if (!data || size < 0xA0) return false;
  if (std::memcmp(data, "INVO", 4) != 0) return false;
  uint32_t ver = 0, hdr0 = 0;
  if (!read_u32(data, size, 4, &ver) || !read_u32(data, size, 8, &hdr0))
    return false;
  if (ver != 3 || hdr0 < 0x80u || hdr0 > 0x200u) return false;

  constexpr uint32_t kStride = 64;
  out.subs.clear();
  bool bounds_init = false;
  size_t mat_base = 0x08;  // first block shares file header after ver

  auto chan = [](float c) -> uint32_t {
    if (!(c == c)) c = 0.5f;
    if (c < 0.f) c = 0.f;
    if (c > 1.f) c = 1.f;
    return static_cast<uint32_t>(c * 255.f + 0.5f);
  };

  while (mat_base + 0x8C < size) {
    uint32_t hsz = 0;
    if (!read_u32(data, size, mat_base, &hsz)) break;
    if (hsz < 0x80u || hsz > 0x200u) break;

    uint32_t vcount = 0;
    if (!read_u32(data, size, mat_base + 0x88, &vcount)) break;
    if (vcount == 0 || vcount > 200000 || vcount > 65535u) break;
    const size_t vstart = mat_base + 0x8C;
    const uint64_t vend =
        static_cast<uint64_t>(vstart) +
        static_cast<uint64_t>(vcount) * static_cast<uint64_t>(kStride);
    if (vend + 4ull > size) break;

    uint32_t tri_count = 0;
    if (!read_u32(data, size, static_cast<size_t>(vend), &tri_count)) break;
    if (tri_count == 0 || tri_count > 500000) break;
    const uint64_t icount = static_cast<uint64_t>(tri_count) * 3ull;
    const uint64_t istart = vend + 4ull;
    if (istart + icount * 4ull > size) break;

    // Sanity: a few verts must be finite before committing.
    bool verts_ok = true;
    for (uint32_t vi = 0; vi < vcount && vi < 8u; ++vi) {
      float x = 0, y = 0, z = 0;
      std::memcpy(&x, data + vstart + vi * kStride, 4);
      std::memcpy(&y, data + vstart + vi * kStride + 4, 4);
      std::memcpy(&z, data + vstart + vi * kStride + 8, 4);
      if (!(x == x) || !(y == y) || !(z == z) || std::fabs(x) > 1.0e6f ||
          std::fabs(y) > 1.0e6f || std::fabs(z) > 1.0e6f) {
        verts_ok = false;
        break;
      }
    }
    if (!verts_ok) break;

    MeshSubmesh sm;
    const size_t nstart = mat_base + 0x68;
    const size_t nlim = mat_base + hsz < size ? mat_base + hsz : size;
    size_t nend = nstart;
    while (nend < nlim && data[nend] != 0) ++nend;
    if (nend > nstart)
      sm.name.assign(reinterpret_cast<const char*>(data + nstart), nend - nstart);

    if (mat_base + 0x10 <= size) {
      float r = 0, g = 0, b = 0;
      std::memcpy(&r, data + mat_base + 0x04, 4);
      std::memcpy(&g, data + mat_base + 0x08, 4);
      std::memcpy(&b, data + mat_base + 0x0C, 4);
      sm.diffuse = 0xFF000000u | (chan(r) << 16) | (chan(g) << 8) | chan(b);
    }

    sm.verts.resize(vcount);
    for (uint32_t vi = 0; vi < vcount; ++vi) {
      MeshVertex& v = sm.verts[vi];
      const uint8_t* p = data + vstart + static_cast<size_t>(vi) * kStride;
      std::memcpy(&v.x, p, 12);
      std::memcpy(&v.nx, p + 12, 12);
      std::memcpy(&v.u, p + 24, 8);
      if (!(v.x == v.x) || !(v.y == v.y) || !(v.z == v.z) ||
          std::fabs(v.x) > 1.0e6f || std::fabs(v.y) > 1.0e6f ||
          std::fabs(v.z) > 1.0e6f) {
        v.x = v.y = v.z = 0.f;
        v.nx = 0.f;
        v.ny = 1.f;
        v.nz = 0.f;
      }
      if (!bounds_init) {
        out.bmin[0] = out.bmax[0] = v.x;
        out.bmin[1] = out.bmax[1] = v.y;
        out.bmin[2] = out.bmax[2] = v.z;
        bounds_init = true;
      } else {
        if (v.x < out.bmin[0]) out.bmin[0] = v.x;
        if (v.y < out.bmin[1]) out.bmin[1] = v.y;
        if (v.z < out.bmin[2]) out.bmin[2] = v.z;
        if (v.x > out.bmax[0]) out.bmax[0] = v.x;
        if (v.y > out.bmax[1]) out.bmax[1] = v.y;
        if (v.z > out.bmax[2]) out.bmax[2] = v.z;
      }
    }

    sm.indices.resize(static_cast<size_t>(icount));
    bool idx_ok = true;
    for (uint64_t i = 0; i < icount; ++i) {
      uint32_t idx = 0;
      if (!read_u32(data, size, static_cast<size_t>(istart + i * 4ull), &idx) ||
          idx >= vcount) {
        idx_ok = false;
        break;
      }
      sm.indices[static_cast<size_t>(i)] = static_cast<uint16_t>(idx);
    }
    if (!idx_ok) break;

    out.subs.push_back(std::move(sm));
    mat_base = static_cast<size_t>(istart + icount * 4ull);
    while (mat_base + 4 <= size) {
      uint32_t z = 0;
      if (!read_u32(data, size, mat_base, &z) || z != 0) break;
      mat_base += 4;
    }
  }

  out.ready = !out.subs.empty();
  return out.ready;
}

}  // namespace

bool render_d3d9_mesh_create_from_memory(void* key, const uint8_t* data,
                                         size_t size, const char* label) {
  if (!key || !data || size < 16) return false;
  {
    auto it = g_meshes.find(key);
    if (it != g_meshes.end()) release_mesh_textures(key, it->second);
  }
  MeshState st;
  st.label = label ? label : "";
  if (!parse_scx_v4(data, size, st) && !parse_scx_v3(data, size, st)) {
    g_meshes.erase(key);
    return false;
  }
  bind_mesh_textures(key, st);
  g_meshes[key] = std::move(st);
  return true;
}

bool render_d3d9_mesh_create_from_file(void* key, const char* path) {
  if (!key || !path || !path[0]) return false;
  FILE* f = std::fopen(path, "rb");
  if (!f) {
    std::string resolved = rpak_resolve_path(path);
    if (!resolved.empty()) f = std::fopen(resolved.c_str(), "rb");
  }
  if (!f) return false;
  std::fseek(f, 0, SEEK_END);
  const long sz = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (sz <= 0 || sz > 64 * 1024 * 1024) {
    std::fclose(f);
    return false;
  }
  std::vector<uint8_t> buf(static_cast<size_t>(sz));
  if (std::fread(buf.data(), 1, buf.size(), f) != buf.size()) {
    std::fclose(f);
    return false;
  }
  std::fclose(f);
  // Prefer the path we actually opened so texture-neighbour resolution works.
  std::string label = path;
  std::string resolved = rpak_resolve_path(path);
  if (!resolved.empty()) label = resolved;
  return render_d3d9_mesh_create_from_memory(key, buf.data(), buf.size(),
                                             label.c_str());
}

bool render_d3d9_mesh_create_skydome(void* key, float radius) {
  if (!key || radius <= 0.f) return false;
  {
    auto it = g_meshes.find(key);
    if (it != g_meshes.end()) release_mesh_textures(key, it->second);
  }
  constexpr int kStacks = 16;
  constexpr int kSlices = 32;
  constexpr float kPi = 3.14159265f;
  MeshSubmesh sm;
  sm.name = "skydome";
  sm.diffuse = 0xFF6EC1FFu;  // daytime sky tint until PTX bind
  sm.verts.reserve(static_cast<size_t>((kStacks + 1) * (kSlices + 1)));
  for (int lat = 0; lat <= kStacks; ++lat) {
    const float v = static_cast<float>(lat) / static_cast<float>(kStacks);
    const float phi = v * kPi;  // 0..pi
    const float y = std::cos(phi);
    const float r = std::sin(phi);
    for (int lon = 0; lon <= kSlices; ++lon) {
      const float u = static_cast<float>(lon) / static_cast<float>(kSlices);
      const float theta = u * 2.f * kPi;
      const float x = r * std::cos(theta);
      const float z = r * std::sin(theta);
      MeshVertex vtx;
      vtx.x = x * radius;
      vtx.y = y * radius;
      vtx.z = z * radius;
      // Inward normals (viewed from inside the dome).
      vtx.nx = -x;
      vtx.ny = -y;
      vtx.nz = -z;
      vtx.u = u;
      vtx.v = v;
      sm.verts.push_back(vtx);
    }
  }
  for (int lat = 0; lat < kStacks; ++lat) {
    for (int lon = 0; lon < kSlices; ++lon) {
      const uint16_t i0 =
          static_cast<uint16_t>(lat * (kSlices + 1) + lon);
      const uint16_t i1 = static_cast<uint16_t>(i0 + 1);
      const uint16_t i2 =
          static_cast<uint16_t>((lat + 1) * (kSlices + 1) + lon);
      const uint16_t i3 = static_cast<uint16_t>(i2 + 1);
      sm.indices.push_back(i0);
      sm.indices.push_back(i2);
      sm.indices.push_back(i1);
      sm.indices.push_back(i1);
      sm.indices.push_back(i2);
      sm.indices.push_back(i3);
    }
  }
  MeshState st;
  st.label = "skydome_proc";
  st.bmin[0] = st.bmin[1] = st.bmin[2] = -radius;
  st.bmax[0] = st.bmax[1] = st.bmax[2] = radius;
  st.subs.push_back(std::move(sm));
  st.ready = true;
  g_meshes[key] = std::move(st);
  return true;
}

bool render_d3d9_mesh_scale_vertices(void* key, float sx, float sy, float sz) {
  if (!key) return false;
  auto it = g_meshes.find(key);
  if (it == g_meshes.end() || !it->second.ready) return false;
  MeshState& st = it->second;
  bool any = false;
  float bmin[3] = {1e30f, 1e30f, 1e30f};
  float bmax[3] = {-1e30f, -1e30f, -1e30f};
  for (MeshSubmesh& sm : st.subs) {
    for (MeshVertex& v : sm.verts) {
      v.x *= sx;
      v.y *= sy;
      v.z *= sz;
      if (v.x < bmin[0]) bmin[0] = v.x;
      if (v.y < bmin[1]) bmin[1] = v.y;
      if (v.z < bmin[2]) bmin[2] = v.z;
      if (v.x > bmax[0]) bmax[0] = v.x;
      if (v.y > bmax[1]) bmax[1] = v.y;
      if (v.z > bmax[2]) bmax[2] = v.z;
      any = true;
    }
  }
  if (any) {
    for (int i = 0; i < 3; ++i) {
      st.bmin[i] = bmin[i];
      st.bmax[i] = bmax[i];
    }
  }
  return true;
}

bool render_d3d9_mesh_clone(void* dst, void* src) {
  if (!dst || !src || dst == src) return false;
  auto it = g_meshes.find(src);
  if (it == g_meshes.end() || !it->second.ready) return false;
  {
    auto dit = g_meshes.find(dst);
    if (dit != g_meshes.end()) release_mesh_textures(dst, dit->second);
  }
  MeshState st = it->second;
  for (size_t i = 0; i < st.subs.size(); ++i) {
    if (st.subs[i].texture_key == mesh_sub_tex_key(src, i))
      st.subs[i].texture_key = nullptr;
  }
  bind_mesh_textures(dst, st);
  g_meshes[dst] = std::move(st);
  return true;
}

void render_d3d9_mesh_set_texture_at(void* mesh_key, int32_t submesh,
                                     void* texture_key) {
  if (!mesh_key) return;
  auto it = g_meshes.find(mesh_key);
  if (it == g_meshes.end() || submesh < 0 ||
      static_cast<size_t>(submesh) >= it->second.subs.size())
    return;
  it->second.subs[static_cast<size_t>(submesh)].texture_key = texture_key;
}

void render_d3d9_mesh_set_texture(void* mesh_key, void* texture_key) {
  render_d3d9_mesh_set_texture_at(mesh_key, 0, texture_key);
}

void render_d3d9_mesh_set_color(void* key, int32_t argb) {
  if (!key) return;
  MeshXform& xf = g_mesh_xforms[key];
  xf.color = argb;
  xf.color_set = 1;
}

int32_t render_d3d9_mesh_get_color(void* key) {
  if (!key) return 0;
  auto it = g_mesh_xforms.find(key);
  if (it == g_mesh_xforms.end() || !it->second.color_set) return 0;
  return it->second.color;
}

void render_d3d9_mesh_destroy(void* key) {
  if (!key) return;
  auto it = g_meshes.find(key);
  if (it != g_meshes.end()) {
    release_mesh_textures(key, it->second);
    g_meshes.erase(it);
  }
  g_mesh_xforms.erase(key);
  g_mesh_bones.erase(key);
  g_mesh_bone_alias.erase(key);
  g_mesh_bone_next.erase(key);
  for (size_t i = 0; i < g_mesh_queue.size();) {
    if (g_mesh_queue[i] == key)
      g_mesh_queue.erase(g_mesh_queue.begin() + static_cast<std::ptrdiff_t>(i));
    else
      ++i;
  }
}

bool render_d3d9_mesh_ready(void* key) {
  auto it = g_meshes.find(key);
  return it != g_meshes.end() && it->second.ready;
}

int32_t render_d3d9_mesh_submesh_count(void* key) {
  auto it = g_meshes.find(key);
  if (it == g_meshes.end()) return 0;
  return static_cast<int32_t>(it->second.subs.size());
}

int32_t render_d3d9_mesh_vertex_count(void* key) {
  auto it = g_meshes.find(key);
  if (it == g_meshes.end()) return 0;
  int32_t n = 0;
  for (const auto& sm : it->second.subs)
    n += static_cast<int32_t>(sm.verts.size());
  return n;
}

int32_t render_d3d9_mesh_index_count(void* key) {
  auto it = g_meshes.find(key);
  if (it == g_meshes.end()) return 0;
  int32_t n = 0;
  for (const auto& sm : it->second.subs)
    n += static_cast<int32_t>(sm.indices.size());
  return n;
}

int32_t render_d3d9_mesh_textured_count(void* key) {
  auto it = g_meshes.find(key);
  if (it == g_meshes.end()) return 0;
  int32_t n = 0;
  for (const auto& sm : it->second.subs) {
    if (sm.texture_key && render_d3d9_texture_ready(sm.texture_key)) ++n;
  }
  return n;
}

void* render_d3d9_mesh_get_texture(void* key, int32_t submesh) {
  auto it = g_meshes.find(key);
  if (it == g_meshes.end() || submesh < 0 ||
      static_cast<size_t>(submesh) >= it->second.subs.size())
    return nullptr;
  return it->second.subs[static_cast<size_t>(submesh)].texture_key;
}

bool render_d3d9_mesh_local_bounds(void* key, float bmin[3], float bmax[3]) {
  auto it = g_meshes.find(key);
  if (it == g_meshes.end() || !it->second.ready || !bmin || !bmax) return false;
  for (int i = 0; i < 3; ++i) {
    bmin[i] = it->second.bmin[i];
    bmax[i] = it->second.bmax[i];
  }
  return true;
}

int32_t render_d3d9_mesh_copy_positions(void* key, float* xyz_interleaved,
                                        int32_t max_count) {
  if (!key || !xyz_interleaved || max_count <= 0) return 0;
  auto it = g_meshes.find(key);
  if (it == g_meshes.end() || !it->second.ready) return 0;
  int32_t n = 0;
  // Stride sample for huge meshes.
  int32_t total = 0;
  for (const auto& sm : it->second.subs)
    total += static_cast<int32_t>(sm.verts.size());
  const int32_t step = total > max_count * 4 ? (total / max_count) : 1;
  int32_t seen = 0;
  for (const auto& sm : it->second.subs) {
    for (const MeshVertex& v : sm.verts) {
      if ((seen++ % step) != 0) continue;
      if (n >= max_count) return n;
      xyz_interleaved[n * 3 + 0] = v.x;
      xyz_interleaved[n * 3 + 1] = v.y;
      xyz_interleaved[n * 3 + 2] = v.z;
      ++n;
    }
  }
  return n;
}

void render_d3d9_mesh_set_transform(void* key, float px, float py, float pz,
                                    float yaw, float pitch, float roll,
                                    float sx, float sy, float sz) {
  if (!key) return;
  MeshXform& xf = g_mesh_xforms[key];
  xf.px = px;
  xf.py = py;
  xf.pz = pz;
  xf.oy = yaw;
  xf.op = pitch;
  xf.or_ = roll;
  xf.sx = sx != 0.f ? sx : 1.f;
  xf.sy = sy != 0.f ? sy : 1.f;
  xf.sz = sz != 0.f ? sz : 1.f;
}

void render_d3d9_mesh_set_parent(void* key, void* parent) {
  if (!key) return;
  // Ignore self-parent; break trivial cycles.
  if (parent == key) parent = nullptr;
  MeshXform& xf = g_mesh_xforms[key];
  xf.parent = parent;
  if (!parent) xf.attach_bone = 0;
}

void render_d3d9_mesh_set_attach_bone(void* key, int32_t bone_id) {
  if (!key) return;
  g_mesh_xforms[key].attach_bone = bone_id < 0 ? 0 : bone_id;
}

void* render_d3d9_mesh_get_parent(void* key) {
  if (!key) return nullptr;
  auto it = g_mesh_xforms.find(key);
  return it == g_mesh_xforms.end() ? nullptr : it->second.parent;
}

int32_t render_d3d9_mesh_get_attach_bone(void* key) {
  if (!key) return 0;
  auto it = g_mesh_xforms.find(key);
  return it == g_mesh_xforms.end() ? 0 : it->second.attach_bone;
}

int32_t render_d3d9_mesh_get_bone_id(void* key, const char* alias) {
  // PE getBoneId @ 0x00481020: handle 0 → Mighty ERROR, return 0 (never -1).
  if (!key) return 0;
  std::string name = alias ? alias : "";
  // Stock Camera path attaches under parent + "bone00".
  if (name.empty() || name == "bone00" || name == "root" || name == "Root")
    return 0;
  auto& aliases = g_mesh_bone_alias[key];
  auto it = aliases.find(name);
  if (it != aliases.end()) return it->second;
  int32_t& next = g_mesh_bone_next[key];
  if (next < 1) next = 1;
  const int32_t id = next++;
  aliases[name] = id;
  return id;
}

void render_d3d9_mesh_set_bone_local(void* key, int32_t bone_id, float px,
                                     float py, float pz, float yaw, float pitch,
                                     float roll) {
  if (!key || bone_id < 0) return;
  MeshXform& b = g_mesh_bones[key][bone_id];
  b.px = px;
  b.py = py;
  b.pz = pz;
  b.oy = yaw;
  b.op = pitch;
  b.or_ = roll;
  b.sx = b.sy = b.sz = 1.f;
}

void render_d3d9_mesh_get_transform(void* key, float* px, float* py, float* pz,
                                    float* yaw, float* pitch, float* roll,
                                    float* sx, float* sy, float* sz) {
  MeshXform xf;
  auto it = g_mesh_xforms.find(key);
  if (it != g_mesh_xforms.end()) xf = it->second;
  if (px) *px = xf.px;
  if (py) *py = xf.py;
  if (pz) *pz = xf.pz;
  if (yaw) *yaw = xf.oy;
  if (pitch) *pitch = xf.op;
  if (roll) *roll = xf.or_;
  if (sx) *sx = xf.sx;
  if (sy) *sy = xf.sy;
  if (sz) *sz = xf.sz;
}

void render_d3d9_mesh_world_origin(void* key, float* wx, float* wy, float* wz) {
  float x = 0, y = 0, z = 0;
#ifdef _WIN32
  D3DMATRIX world{};
  resolve_world(key, &world, 0);
  x = world.m[3][0];
  y = world.m[3][1];
  z = world.m[3][2];
#else
  (void)key;
  auto it = g_mesh_xforms.find(key);
  if (it != g_mesh_xforms.end()) {
    x = it->second.px;
    y = it->second.py;
    z = it->second.pz;
  }
#endif
  if (wx) *wx = x;
  if (wy) *wy = y;
  if (wz) *wz = z;
}

void render_d3d9_mesh_queue_clear() { g_mesh_queue.clear(); }

void render_d3d9_mesh_queue_add(void* key) {
  if (!key || !render_d3d9_mesh_ready(key)) return;
  g_mesh_queue.push_back(key);
}

int32_t render_d3d9_mesh_queue_count() {
  return static_cast<int32_t>(g_mesh_queue.size());
}

}  // namespace inv
