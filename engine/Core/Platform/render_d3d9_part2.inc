// Fragment 2/3 of render_d3d9.cpp — included, not compiled on its own.
// Split only to stay under the 128 KB editor buffer cap; the
// translation unit is unchanged. See native/tools/split_source.py.

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
// DDS_HEADER.dwCaps2 — Body@591D85 a16=5 requires exactly 6 faces.
constexpr uint32_t kDdscaps2Cubemap = 0x200u;
constexpr uint32_t kDdscaps2CubemapFaceMask = 0xFC00u;  // ±X ±Y ±Z

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

#ifdef _WIN32
// Soft PE D3DX_CreateTextureFromFileInMemoryEx @ 0x5924FF / Body@591D85 —
// no d3dx9 import; resolve at runtime when the DLL is present.
using PFN_D3DXCreateTextureFromFileInMemoryEx = HRESULT(WINAPI*)(
    IDirect3DDevice9*, LPCVOID, UINT, UINT, UINT, UINT, DWORD, D3DFORMAT,
    D3DPOOL, DWORD, DWORD, D3DCOLOR, void*, PALETTEENTRY*,
    IDirect3DTexture9**);
// Soft PE D3DX_CreateCubeTextureFromFileInMemoryEx @ 0x59253C → Body@591D85
// a16=5 (cube). W33B CreateCubeFromFile_Body@4F7FE0 maps path then this.
using PFN_D3DXCreateCubeTextureFromFileInMemoryEx = HRESULT(WINAPI*)(
    IDirect3DDevice9*, LPCVOID, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
    DWORD, DWORD, D3DCOLOR, void*, PALETTEENTRY*, IDirect3DCubeTexture9**);
using PFN_D3DXCreateCubeTextureFromFileInMemory = HRESULT(WINAPI*)(
    IDirect3DDevice9*, LPCVOID, UINT, IDirect3DCubeTexture9**);
using PFN_D3DXLoadSurfaceFromMemory = HRESULT(WINAPI*)(
    IDirect3DSurface9*, const PALETTEENTRY*, const RECT*, LPCVOID, D3DFORMAT,
    UINT, const PALETTEENTRY*, const RECT*, DWORD, D3DCOLOR);
using PFN_D3DXLoadSurfaceFromSurface = HRESULT(WINAPI*)(
    IDirect3DSurface9*, const PALETTEENTRY*, const RECT*, IDirect3DSurface9*,
    const PALETTEENTRY*, const RECT*, DWORD, D3DCOLOR);

HMODULE d3dx9_module() {
  static HMODULE mod = nullptr;
  static bool tried = false;
  if (tried) return mod;
  tried = true;
  // Prefer newer redistributables; stock Invictus ships an in-PE D3DX copy.
  static const char* kNames[] = {"d3dx9_43.dll", "d3dx9_42.dll", "d3dx9_41.dll",
                                 "d3dx9_40.dll", "d3dx9_39.dll", "d3dx9_38.dll",
                                 "d3dx9_37.dll", "d3dx9_36.dll", "d3dx9_35.dll",
                                 "d3dx9_34.dll", "d3dx9_33.dll", "d3dx9_32.dll",
                                 "d3dx9_31.dll", "d3dx9_30.dll", "d3dx9_29.dll",
                                 "d3dx9_28.dll", "d3dx9_27.dll", "d3dx9_26.dll",
                                 "d3dx9_25.dll", "d3dx9_24.dll", "d3dx9.dll"};
  for (const char* n : kNames) {
    mod = LoadLibraryA(n);
    if (mod) return mod;
  }
  return nullptr;
}

PFN_D3DXCreateTextureFromFileInMemoryEx d3dx_create_from_mem_ex() {
  static PFN_D3DXCreateTextureFromFileInMemoryEx pfn = nullptr;
  static bool resolved = false;
  if (resolved) return pfn;
  resolved = true;
  HMODULE m = d3dx9_module();
  if (!m) return nullptr;
  pfn = reinterpret_cast<PFN_D3DXCreateTextureFromFileInMemoryEx>(
      GetProcAddress(m, "D3DXCreateTextureFromFileInMemoryEx"));
  return pfn;
}

PFN_D3DXCreateCubeTextureFromFileInMemoryEx d3dx_create_cube_from_mem_ex() {
  static PFN_D3DXCreateCubeTextureFromFileInMemoryEx pfn = nullptr;
  static bool resolved = false;
  if (resolved) return pfn;
  resolved = true;
  HMODULE m = d3dx9_module();
  if (!m) return nullptr;
  pfn = reinterpret_cast<PFN_D3DXCreateCubeTextureFromFileInMemoryEx>(
      GetProcAddress(m, "D3DXCreateCubeTextureFromFileInMemoryEx"));
  return pfn;
}

PFN_D3DXCreateCubeTextureFromFileInMemory d3dx_create_cube_from_mem() {
  static PFN_D3DXCreateCubeTextureFromFileInMemory pfn = nullptr;
  static bool resolved = false;
  if (resolved) return pfn;
  resolved = true;
  HMODULE m = d3dx9_module();
  if (!m) return nullptr;
  pfn = reinterpret_cast<PFN_D3DXCreateCubeTextureFromFileInMemory>(
      GetProcAddress(m, "D3DXCreateCubeTextureFromFileInMemory"));
  return pfn;
}

PFN_D3DXLoadSurfaceFromMemory d3dx_load_surface_from_memory() {
  static PFN_D3DXLoadSurfaceFromMemory pfn = nullptr;
  static bool resolved = false;
  if (resolved) return pfn;
  resolved = true;
  HMODULE m = d3dx9_module();
  if (!m) return nullptr;
  pfn = reinterpret_cast<PFN_D3DXLoadSurfaceFromMemory>(
      GetProcAddress(m, "D3DXLoadSurfaceFromMemory"));
  return pfn;
}

PFN_D3DXLoadSurfaceFromSurface d3dx_load_surface_from_surface() {
  static PFN_D3DXLoadSurfaceFromSurface pfn = nullptr;
  static bool resolved = false;
  if (resolved) return pfn;
  resolved = true;
  HMODULE m = d3dx9_module();
  if (!m) return nullptr;
  pfn = reinterpret_cast<PFN_D3DXLoadSurfaceFromSurface>(
      GetProcAddress(m, "D3DXLoadSurfaceFromSurface"));
  return pfn;
}

// Soft PE Gfx_EngineFmtToD3D @ 0x618330 (post-probe host table).
D3DFORMAT engine_fmt_to_d3d_early(int32_t engine_fmt) {
  switch (engine_fmt) {
    case 0:
      return D3DFMT_R5G6B5;
    case 1:
      return D3DFMT_A1R5G5B5;
    case 2:
      return D3DFMT_X8R8G8B8;
    case 6:
      return D3DFMT_DXT1;
    case 7:
      return D3DFMT_DXT5;
    case 8:
      return D3DFMT_A8L8;
    case 13:
      return D3DFMT_DXT3;
    case 3:
    case 4:
    case 5:
    case 9:
    default:
      return D3DFMT_A8R8G8B8;
  }
}

bool adopt_d3d_texture(TextureState& st, IDirect3DTexture9* tex,
                       int32_t engine_fmt) {
  if (!tex) return false;
  D3DSURFACE_DESC desc{};
  if (FAILED(tex->GetLevelDesc(0, &desc)) || desc.Width == 0 ||
      desc.Height == 0) {
    tex->Release();
    return false;
  }
  texture_clear_async_mip(st);
  texture_release_gpu(st);
  st.tex = tex;
  st.w = static_cast<int32_t>(desc.Width);
  st.h = static_cast<int32_t>(desc.Height);
  st.mips = static_cast<int32_t>(tex->GetLevelCount());
  if (st.mips < 1) st.mips = 1;
  st.engine_fmt = engine_fmt;
  st.luma_alpha = false;
  st.mip_level_ready = true;  // soft PE GfxTexture+13 after create-from-mem
  return true;
}

bool adopt_d3d_cube(TextureState& st, IDirect3DCubeTexture9* cube,
                    int32_t engine_fmt) {
  if (!cube) return false;
  D3DSURFACE_DESC desc{};
  if (FAILED(cube->GetLevelDesc(0, &desc)) || desc.Width == 0) {
    cube->Release();
    return false;
  }
  texture_clear_async_mip(st);
  texture_release_gpu(st);
  st.cube = cube;
  st.w = static_cast<int32_t>(desc.Width);
  st.h = static_cast<int32_t>(desc.Width);  // cube edge
  st.mips = static_cast<int32_t>(cube->GetLevelCount());
  if (st.mips < 1) st.mips = 1;
  st.engine_fmt = engine_fmt;
  st.luma_alpha = false;
  st.mip_level_ready = true;  // soft PE GfxCubeTexture+13 after create
  return true;
}

// Soft PE CreateTextureFromMem_Body path when d3dx9_*.dll is present.
bool try_d3dx_create_from_mem(TextureState& st, const uint8_t* data, size_t size,
                              int32_t engine_fmt, int32_t a5_mip_flag) {
  auto* pfn = d3dx_create_from_mem_ex();
  if (!pfn || !g_dev || !data || size == 0) return false;

  // PE Body@4F7AFB: v6 = (a5 == 0); remapped to MipLevels via @5924FF → 1 or 0.
  // MipLevels=1 → single level (Type0); 0 → D3DX_DEFAULT full chain.
  const UINT mip_levels = (a5_mip_flag == 0) ? 1u : 0u;
  DWORD usage = 0;
  D3DPOOL pool = D3DPOOL_MANAGED;
  if (engine_fmt == 9 || engine_fmt == 11) {
    usage = D3DUSAGE_RENDERTARGET;
    pool = D3DPOOL_DEFAULT;
  }
  const D3DFORMAT fmt = engine_fmt_to_d3d_early(engine_fmt);
  // PE filter 524292 (0x80004) when -1; mip filter -1 → 5 in Body@591D85.
  constexpr DWORD kFilter = 0x80004u;
  constexpr DWORD kMipFilter = 5u;
  IDirect3DTexture9* tex = nullptr;
  const HRESULT hr = pfn(g_dev, data, static_cast<UINT>(size), 0, 0, mip_levels,
                         usage, fmt, pool, kFilter, kMipFilter, 0, nullptr,
                         nullptr, &tex);
  if (FAILED(hr) || !tex) return false;
  return adopt_d3d_texture(st, tex, engine_fmt);
}

// Soft PE CreateCubeFromFile_Body@4F7FE0 InMemoryEx path (a16=5) when d3dx
// present. PE maps a path first (@5925DC MappedFile); host soft takes
// already-buffered bytes. W34-02 LockRect DDS cube is the no-d3dx fallback.
bool try_d3dx_create_cube_from_mem(TextureState& st, const uint8_t* data,
                                   size_t size, int32_t engine_fmt,
                                   int32_t a5_mip_flag) {
  if (!g_dev || !data || size == 0) return false;

  // PE Body@4F7FFB: v5 = (a4 == 0) → MipLevels 1 or 0 (D3DX_DEFAULT).
  const UINT mip_levels = (a5_mip_flag == 0) ? 1u : 0u;
  DWORD usage = 0;
  D3DPOOL pool = D3DPOOL_MANAGED;
  if (engine_fmt == 9 || engine_fmt == 11) {
    usage = D3DUSAGE_RENDERTARGET;
    pool = D3DPOOL_DEFAULT;
  }
  const D3DFORMAT fmt = engine_fmt_to_d3d_early(engine_fmt);
  constexpr DWORD kFilter = 0x80004u;  // PE 524292
  constexpr DWORD kMipFilter = 5u;     // Body@591D85 remaps mip filter -1 → 5
  IDirect3DCubeTexture9* cube = nullptr;

  if (auto* pfn_ex = d3dx_create_cube_from_mem_ex()) {
    // Soft D3DXCreateCubeTextureFromFileInMemoryEx — Size=0 (from file),
    // ColorKey=0 (host; PE pushes -1, in-PE Body remaps).
    const HRESULT hr =
        pfn_ex(g_dev, data, static_cast<UINT>(size), 0, mip_levels, usage, fmt,
               pool, kFilter, kMipFilter, 0, nullptr, nullptr, &cube);
    if (SUCCEEDED(hr) && cube) return adopt_d3d_cube(st, cube, engine_fmt);
    if (cube) {
      cube->Release();
      cube = nullptr;
    }
  }
  // Fallback: non-Ex ignores Usage/Format/Pool/MipLevels (D3DX defaults).
  if (auto* pfn = d3dx_create_cube_from_mem()) {
    const HRESULT hr =
        pfn(g_dev, data, static_cast<UINT>(size), &cube);
    if (SUCCEEDED(hr) && cube) return adopt_d3d_cube(st, cube, engine_fmt);
    if (cube) cube->Release();
  }
  return false;
}

// Soft PE D3DX_LoadSurfaceFromMemory @ 0x5912E8 — GetProcAddress or LockRect.
bool soft_load_surface_from_memory(IDirect3DSurface9* surf, const void* src,
                                   D3DFORMAT src_fmt, UINT src_pitch,
                                   int32_t src_w, int32_t src_h) {
  if (!surf || !src || src_w <= 0 || src_h <= 0) return false;
  RECT rc{};
  rc.left = 0;
  rc.top = 0;
  rc.right = src_w;
  rc.bottom = src_h;
  if (auto* pfn = d3dx_load_surface_from_memory()) {
    // PE UploadLevel@4F6783: Filter=1 (D3DX_FILTER_NONE).
    const HRESULT hr =
        pfn(surf, nullptr, nullptr, src, src_fmt, src_pitch, nullptr, &rc, 1,
            0);
    return SUCCEEDED(hr);
  }
  // Fallback LockRect row copy (upload_level / Poll when d3dx absent).
  D3DLOCKED_RECT lr{};
  if (FAILED(surf->LockRect(&lr, &rc, 0)) || !lr.pBits) return false;
  const auto* row = static_cast<const uint8_t*>(src);
  auto* dst = static_cast<uint8_t*>(lr.pBits);
  const size_t row_bytes = static_cast<size_t>(src_pitch);
  for (int32_t y = 0; y < src_h; ++y) {
    std::memcpy(dst, row, row_bytes);
    row += src_pitch;
    dst += lr.Pitch;
  }
  surf->UnlockRect();
  return true;
}

// Soft PE D3DX_LoadSurfaceFromSurface @ 0x591807 — GetProcAddress or LockRect.
// CopyAllFacesFrom@4F70AA: Filter=1 (D3DX_FILTER_NONE), ColorKey=0, null RECTs.
bool soft_load_surface_from_surface(IDirect3DSurface9* dst,
                                    IDirect3DSurface9* src) {
  if (!dst || !src) return false;
  if (auto* pfn = d3dx_load_surface_from_surface()) {
    const HRESULT hr =
        pfn(dst, nullptr, nullptr, src, nullptr, nullptr, 1, 0);
    return SUCCEEDED(hr);
  }
  D3DSURFACE_DESC dd{};
  D3DSURFACE_DESC sd{};
  if (FAILED(dst->GetDesc(&dd)) || FAILED(src->GetDesc(&sd))) return false;
  D3DLOCKED_RECT slr{};
  D3DLOCKED_RECT dlr{};
  if (FAILED(src->LockRect(&slr, nullptr, D3DLOCK_READONLY)) || !slr.pBits)
    return false;
  if (FAILED(dst->LockRect(&dlr, nullptr, 0)) || !dlr.pBits) {
    src->UnlockRect();
    return false;
  }
  const UINT rows = dd.Height < sd.Height ? dd.Height : sd.Height;
  const size_t copy =
      static_cast<size_t>(slr.Pitch < dlr.Pitch ? slr.Pitch : dlr.Pitch);
  auto* srow = static_cast<const uint8_t*>(slr.pBits);
  auto* drow = static_cast<uint8_t*>(dlr.pBits);
  for (UINT y = 0; y < rows; ++y) {
    std::memcpy(drow, srow, copy);
    srow += slr.Pitch;
    drow += dlr.Pitch;
  }
  dst->UnlockRect();
  src->UnlockRect();
  return true;
}
#endif  // _WIN32

// max_mips: 0 = all DDS levels; >0 caps CreateTexture Levels (PE a5==0 → 1).
// Soft PE CRT_TEXTURE_DDS @ 0x618878 — magic + header.size 124 / pf.size 32;
// FOURCC DXT1/3/5 (FormatFromFourcc@4F8990 → engine 6/13/7) or RGB masks.
bool upload_dds(TextureState& st, const uint8_t* data, size_t size,
                int32_t max_mips) {
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
  if (max_mips > 0 && mips > max_mips) mips = max_mips;

  texture_clear_async_mip(st);
  texture_release_gpu(st);
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
  st.mip_level_ready = true;
  if (compressed) {
    if (dxt1)
      st.engine_fmt = 6;
    else if (fmt == D3DFMT_DXT3)
      st.engine_fmt = 13;
    else
      st.engine_fmt = 7;  // DXT5
  } else if (fmt == D3DFMT_R5G6B5) {
    st.engine_fmt = 0;
  } else if (fmt == D3DFMT_R8G8B8) {
    st.engine_fmt = 2;
  } else {
    st.engine_fmt = 3;  // A8R8G8B8 default
  }
  return true;
#else
  (void)st;
  (void)data;
  (void)size;
  (void)max_mips;
  return false;
#endif
}

bool upload_dds(TextureState& st, const uint8_t* data, size_t size) {
  return upload_dds(st, data, size, 0);
}

// W34-02 — soft PE Body@591D85 a16=5 cube path without d3dx9_*.dll:
// CreateCubeTexture vt+100 + LockRect per face/mip (DDS face order POSX..NEGZ).
// Stock prefers LoadSurfaceFromMemory after MapView@5925DC; host already has
// buffered bytes (create_cube_from_mem). Does not touch create_cube_dims /
// upload_cube_face / CopyAllFaces / Poll.
bool upload_dds_cube(TextureState& st, const uint8_t* data, size_t size,
                     int32_t max_mips) {
#ifdef _WIN32
  if (!g_dev || !data || size < 4 + sizeof(DdsHeader)) return false;
  uint32_t magic = 0;
  std::memcpy(&magic, data, 4);
  if (magic != kDdsMagic) return false;
  DdsHeader hdr{};
  std::memcpy(&hdr, data + 4, sizeof(hdr));
  if (hdr.size != 124 || hdr.pf.size != 32) return false;
  if (hdr.width == 0 || hdr.height == 0) return false;
  // Cubes are square edge; Body CreateCubeTexture Size=i (==j for Ex thunk).
  if (hdr.width != hdr.height) return false;
  if ((hdr.caps2 & kDdscaps2Cubemap) == 0) return false;
  // Require all six face bits (0xFC00), matching Body v62==6.
  if ((hdr.caps2 & kDdscaps2CubemapFaceMask) != kDdscaps2CubemapFaceMask)
    return false;

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
  if (max_mips > 0 && mips > max_mips) mips = max_mips;

  texture_clear_async_mip(st);
  texture_release_gpu(st);
  st.luma_alpha = false;
  HRESULT hr = g_dev->CreateCubeTexture(
      hdr.width, static_cast<UINT>(mips), 0, fmt, D3DPOOL_MANAGED, &st.cube,
      nullptr);
  if (FAILED(hr) || !st.cube) {
    st.cube = nullptr;
    return false;
  }

  const uint8_t* src = data + 4 + sizeof(DdsHeader);
  size_t remain = size - (4 + sizeof(DdsHeader));
  // DDS cubemap layout: face major (POSX..NEGZ), full mip chain per face.
  for (int face = 0; face < 6; ++face) {
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
        st.cube->Release();
        st.cube = nullptr;
        return false;
      }
      D3DLOCKED_RECT lr{};
      if (FAILED(st.cube->LockRect(static_cast<D3DCUBEMAP_FACES>(face),
                                   static_cast<UINT>(level), &lr, nullptr,
                                   0)) ||
          !lr.pBits) {
        st.cube->Release();
        st.cube = nullptr;
        return false;
      }
      if (compressed || lr.Pitch == static_cast<INT>(mw * bpp)) {
        std::memcpy(lr.pBits, src, level_bytes);
      } else {
        const uint8_t* row = src;
        auto* dst = static_cast<uint8_t*>(lr.pBits);
        const size_t row_bytes = static_cast<size_t>(mw) * bpp;
        for (uint32_t y = 0; y < mh; ++y) {
          std::memcpy(dst, row, row_bytes);
          row += row_bytes;
          dst += lr.Pitch;
        }
      }
      st.cube->UnlockRect(static_cast<D3DCUBEMAP_FACES>(face),
                          static_cast<UINT>(level));
      src += level_bytes;
      remain -= level_bytes;
      if (mw > 1) mw /= 2;
      if (mh > 1) mh /= 2;
    }
  }

  st.w = static_cast<int32_t>(hdr.width);
  st.h = static_cast<int32_t>(hdr.width);
  st.mips = mips;
  st.luma_alpha = false;
  st.mip_level_ready = true;
  if (compressed) {
    if (dxt1)
      st.engine_fmt = 6;
    else if (fmt == D3DFMT_DXT3)
      st.engine_fmt = 13;
    else
      st.engine_fmt = 7;
  } else if (fmt == D3DFMT_R5G6B5) {
    st.engine_fmt = 0;
  } else if (fmt == D3DFMT_R8G8B8) {
    st.engine_fmt = 2;
  } else {
    st.engine_fmt = 3;
  }
  return true;
#else
  (void)st;
  (void)data;
  (void)size;
  (void)max_mips;
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

  texture_release_gpu(st);
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
  st.engine_fmt = 3;  // A8R8G8B8
  return true;
#else
  (void)st;
  (void)jpeg;
  (void)jpeg_size;
  return false;
#endif
}

// Soft PE CRT_TEXTURE_PTX content contract (samples + table @ 0x618840):
#pragma pack(push, 1)
struct PtxFileHeader {
  uint32_t reserved;     // +0  (0)
  uint32_t version;      // +4  (1..8 observed; NOT restricted to 1)
  uint32_t width;        // +8
  uint32_t height;       // +12
  uint32_t payload_a;    // +16 (= jpeg0_c + jpeg1_d)
  uint32_t jpeg0_c;      // +20 primary JPEG bytes
  uint32_t jpeg1_d;      // +24 secondary (0 = single stream)
  uint32_t pad;          // +28
  float inv_version;     // +32 (≈ 1/version)
};
struct PtxMipHeader {
  uint32_t payload_a;  // +0
  uint32_t jpeg0_c;    // +4
  uint32_t jpeg1_d;    // +8
  uint32_t pad;        // +12
  float inv_version;   // +16
};
#pragma pack(pop)
static_assert(sizeof(PtxFileHeader) == 36, "PTX file hdr 36");
static_assert(sizeof(PtxMipHeader) == 20, "PTX mip hdr 20");

bool ptx_level_jpeg0(const uint8_t* level_data, size_t level_bytes,
                     uint32_t jpeg0_c, uint32_t jpeg1_d, uint32_t payload_a,
                     const uint8_t** jpeg_out, size_t* jpeg_size_out) {
  if (!level_data || !jpeg_out || !jpeg_size_out) return false;
  if (payload_a < 4 || payload_a > level_bytes) return false;
  if (level_data[0] != 0xff || level_data[1] != 0xd8) return false;
  uint32_t j0 = jpeg0_c;
  if (j0 == 0 || j0 > payload_a) j0 = payload_a;
  // Dual stream (c+d==a): color JPEG is first `c` bytes.
  if (jpeg1_d > 0 && jpeg0_c > 0 &&
      static_cast<uint64_t>(jpeg0_c) + jpeg1_d == payload_a)
    j0 = jpeg0_c;
  if (j0 < 4 || j0 > payload_a) return false;
  *jpeg_out = level_data;
  *jpeg_size_out = j0;
  return true;
}

// Walk PTX levels; prefer last valid jpeg0 (highest quality / largest).
bool parse_ptx_jpeg(const uint8_t* data, size_t size, const uint8_t** jpeg_out,
                    size_t* jpeg_size_out, int32_t* w_out, int32_t* h_out) {
  if (!data || size < sizeof(PtxFileHeader) + 4 || !jpeg_out || !jpeg_size_out)
    return false;
  PtxFileHeader hdr{};
  std::memcpy(&hdr, data, sizeof(hdr));
  // Soft PE CRT_TEXTURE_PTX: reject ver==0; accept 1..8 (observed range).
  if (hdr.version == 0 || hdr.version > 8 || hdr.width == 0 ||
      hdr.height == 0)
    return false;
  if (hdr.payload_a < 4) return false;

  const uint8_t* best_jpeg = nullptr;
  size_t best_sz = 0;
  size_t off = sizeof(PtxFileHeader);
  uint32_t a = hdr.payload_a;
  uint32_t c = hdr.jpeg0_c;
  uint32_t d = hdr.jpeg1_d;

  for (int level = 0; level < 32; ++level) {
    if (off + static_cast<size_t>(a) > size) break;
    const uint8_t* level_data = data + off;
    const uint8_t* j = nullptr;
    size_t jsz = 0;
    if (ptx_level_jpeg0(level_data, a, c, d, a, &j, &jsz)) {
      best_jpeg = j;
      best_sz = jsz;
    }
    off += a;
    if (off + sizeof(PtxMipHeader) > size) break;
    PtxMipHeader mip{};
    std::memcpy(&mip, data + off, sizeof(mip));
    if (mip.payload_a < 4 ||
        off + sizeof(PtxMipHeader) + static_cast<size_t>(mip.payload_a) > size)
      break;
    if (mip.jpeg0_c > mip.payload_a) break;
    off += sizeof(PtxMipHeader);
    a = mip.payload_a;
    c = mip.jpeg0_c;
    d = mip.jpeg1_d;
  }

  if (!best_jpeg || best_sz < 4) return false;
  *jpeg_out = best_jpeg;
  *jpeg_size_out = best_sz;
  if (w_out) *w_out = static_cast<int32_t>(hdr.width);
  if (h_out) *h_out = static_cast<int32_t>(hdr.height);
  return true;
}

bool looks_like_ptx(const uint8_t* data, size_t size) {
  const uint8_t* j = nullptr;
  size_t jsz = 0;
  return parse_ptx_jpeg(data, size, &j, &jsz, nullptr, nullptr);
}

}  // namespace

bool render_d3d9_texture_create_from_ptx(void* key, const uint8_t* data,
                                         size_t size, const char* label) {
  // Soft PE CRT_TEXTURE_PTX: walk quality pyramid, upload last jpeg0 via WIC.
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
  // Convenience path: full DDS mip chain (Resources / RPAK). Type0 PE uses
  // create_from_mem(…, fmt, a5=0) → single mip via D3DX@5924FF.
  return render_d3d9_texture_create_from_mem(key, data, size, 3, 1, label);
}

bool render_d3d9_texture_create_from_mem(void* key, const uint8_t* data,
                                         size_t size, int32_t engine_fmt,
                                         int32_t a5_mip_flag,
                                         const char* label) {
  // Soft PE GfxDevice_CreateTextureFromMem @ 0x4F8A40 / Body@4F7AE0.
  if (!key || !data || size == 0) return false;
  // Soft PE CRT_TEXTURE_PTX (skydome / chassis / flares) — not D3DX in-mem.
  if (looks_like_ptx(data, size))
    return render_d3d9_texture_create_from_ptx(key, data, size, label);
  TextureState& st = g_textures[key];
  st.label = label ? label : "";

#ifdef _WIN32
  // Stock: D3DX_CreateTextureFromFileInMemoryEx@5924FF (soft GetProcAddress).
  if (try_d3dx_create_from_mem(st, data, size, engine_fmt, a5_mip_flag))
    return true;
#endif

  // Fallback: device CreateTexture + LockRect levels (DXT/A8R8G8B8 DDS).
  // PE a5==0 → MipLevels=1; a5!=0 → full DDS chain (host stand-in for 0/DEFAULT).
  const int32_t max_mips = (a5_mip_flag == 0) ? 1 : 0;
  if (!upload_dds(st, data, size, max_mips)) {
    // Keep registry entry for type bookkeeping even if upload failed / no device.
    if (size >= 128 && data[0] == 'D' && data[1] == 'D' && data[2] == 'S') {
      DdsHeader hdr{};
      std::memcpy(&hdr, data + 4, sizeof(hdr));
      st.w = static_cast<int32_t>(hdr.width);
      st.h = static_cast<int32_t>(hdr.height);
      int32_t m = static_cast<int32_t>(hdr.mipmap_count > 0 ? hdr.mipmap_count : 1);
      if (max_mips > 0 && m > max_mips) m = max_mips;
      st.mips = m;
    }
    st.engine_fmt = engine_fmt;
#ifdef _WIN32
    return st.tex != nullptr;
#else
    return st.w > 0;
#endif
  }
  // Prefer caller engine_fmt when PE forces a non-default Config index (Type0 a4).
  if (engine_fmt != 3) st.engine_fmt = engine_fmt;
  return true;
}

bool render_d3d9_texture_upload_level_argb(void* key, int32_t level,
                                           const void* src_argb, int32_t src_w,
                                           int32_t src_h) {
  // Soft PE GfxTexture_UploadLevelFromMem_A8R8G8B8 @ 0x4F6710:
  // GetSurfaceLevel(level) + D3DX_LoadSurfaceFromMemory@5912E8 (fmt=21) or
  // LockRect. Fills create_dims textures (dims leftover upload).
  if (!key || !src_argb || level < 0 || src_w <= 0 || src_h <= 0) return false;
  auto it = g_textures.find(key);
  if (it == g_textures.end()) return false;
  TextureState& st = it->second;
#ifdef _WIN32
  if (!st.tex) return false;
  if (st.mips > 0 && level >= st.mips) return false;
  IDirect3DSurface9* surf = nullptr;
  if (FAILED(st.tex->GetSurfaceLevel(static_cast<UINT>(level), &surf)) ||
      !surf)
    return false;
  const UINT pitch = static_cast<UINT>(src_w) * 4u;
  const bool ok = soft_load_surface_from_memory(surf, src_argb, D3DFMT_A8R8G8B8,
                                                pitch, src_w, src_h);
  surf->Release();
  if (ok) st.mip_level_ready = true;
  return ok;
#else
  (void)level;
  st.mip_level_ready = true;
  return true;
#endif
}

bool render_d3d9_texture_upload_cube_face(void* key, int32_t face,
                                          int32_t level, const void* src_argb,
                                          int32_t src_w, int32_t src_h,
                                          bool x8r8g8b8) {
  // Soft PE GfxTexture_UploadCubeFaceFromMem_A8R8G8B8 @ 0x4F7260 /
  // _X8R8G8B8 @ 0x4F7300: GetCubeMapSurface(face,level) vt+72 → GetDesc →
  // D3DX_LoadSurfaceFromMemory@5912E8 (fmt 21/22, pitch=4*Width, Filter=1)
  // or LockRect; set GfxTexture+13 ready. face = D3DCUBEMAP_FACES 0..5.
  if (!key || !src_argb || face < 0 || face > 5 || level < 0 || src_w <= 0 ||
      src_h <= 0)
    return false;
  auto it = g_textures.find(key);
  if (it == g_textures.end()) return false;
  TextureState& st = it->second;
#ifdef _WIN32
  if (!st.cube) return false;
  if (st.mips > 0 && level >= st.mips) return false;
  IDirect3DSurface9* surf = nullptr;
  if (FAILED(st.cube->GetCubeMapSurface(static_cast<D3DCUBEMAP_FACES>(face),
                                        static_cast<UINT>(level), &surf)) ||
      !surf)
    return false;
  D3DSURFACE_DESC desc{};
  if (FAILED(surf->GetDesc(&desc)) || desc.Width == 0 || desc.Height == 0) {
    surf->Release();
    return false;
  }
  // PE builds src RECT from GetDesc Width/Height (not caller args).
  const int32_t dw = static_cast<int32_t>(desc.Width);
  const int32_t dh = static_cast<int32_t>(desc.Height);
  (void)src_w;
  (void)src_h;
  const D3DFORMAT src_fmt =
      x8r8g8b8 ? D3DFMT_X8R8G8B8 : D3DFMT_A8R8G8B8;
  const UINT pitch = static_cast<UINT>(dw) * 4u;
  const bool ok =
      soft_load_surface_from_memory(surf, src_argb, src_fmt, pitch, dw, dh);
  surf->Release();
  if (ok) st.mip_level_ready = true;
  return ok;
#else
  (void)face;
  (void)level;
  (void)x8r8g8b8;
  st.mip_level_ready = true;
  return true;
#endif
}

bool render_d3d9_texture_copy_all_faces_from(void* dst_key, void* src_key) {
  // Soft PE GfxCubeTexture_CopyAllFacesFrom @ 0x4F7010:
  // this[+4]/src[+4] cubes; min(GetLevelCount vt+52); for level, for face
  // 0..5: GetCubeMapSurface(face,level) both (vt+72); D3DX_LoadSurfaceFromSurface
  // @591807 Filter=1 ColorKey=0; Release; on success set +13 ready, return 1.
  if (!dst_key || !src_key) return false;
  auto dit = g_textures.find(dst_key);
  auto sit = g_textures.find(src_key);
  if (dit == g_textures.end() || sit == g_textures.end()) return false;
  TextureState& dst = dit->second;
  TextureState& src = sit->second;
#ifdef _WIN32
  if (!dst.cube || !src.cube) return false;
  const UINT dst_lvls = dst.cube->GetLevelCount();
  const UINT src_lvls = src.cube->GetLevelCount();
  UINT levels = dst_lvls < src_lvls ? dst_lvls : src_lvls;
  if (levels == 0) {
    dst.mip_level_ready = true;
    return true;
  }
  HRESULT last = S_OK;
  for (UINT level = 0; level < levels; ++level) {
    for (int face = 0; face < 6; ++face) {
      IDirect3DSurface9* dsurf = nullptr;
      IDirect3DSurface9* ssurf = nullptr;
      last = dst.cube->GetCubeMapSurface(static_cast<D3DCUBEMAP_FACES>(face),
                                         level, &dsurf);
      if (SUCCEEDED(last) && dsurf) {
        last = src.cube->GetCubeMapSurface(static_cast<D3DCUBEMAP_FACES>(face),
                                           level, &ssurf);
        if (SUCCEEDED(last) && ssurf) {
          last = soft_load_surface_from_surface(dsurf, ssurf) ? S_OK : E_FAIL;
          ssurf->Release();
        }
        dsurf->Release();
      }
    }
    if (FAILED(last)) return false;
  }
  dst.mip_level_ready = true;
  return true;
#else
  (void)src;
  dst.mip_level_ready = true;
  return true;
#endif
}

bool render_d3d9_texture_set(int32_t stage, void* key) {
  // Soft IDirect3DDevice9::SetTexture — cube keys bind IDirect3DCubeTexture9
  // (IDirect3DBaseTexture9*); 2D tex otherwise; key=nullptr clears stage.
  if (stage < 0) return false;
#ifdef _WIN32
  if (!g_dev) return false;
  if (!key) {
    g_dev->SetTexture(static_cast<DWORD>(stage), nullptr);
    return true;
  }
  auto it = g_textures.find(key);
  if (it == g_textures.end()) return false;
  IDirect3DBaseTexture9* base = texture_d3d_base(it->second);
  if (!base) return false;
  return SUCCEEDED(
      g_dev->SetTexture(static_cast<DWORD>(stage), base));
#else
  (void)key;
  return true;
#endif
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
  st.engine_fmt = 3;
#ifdef _WIN32
  if (!g_dev) return true;
  texture_release_gpu(st);
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
  // Soft PE CRT_TEXTURE_DDS @ 0x618878: magic 'DDS ' + DDS_HEADER.size==124 /
  // pf.size==32; require FOURCC or RGB (upload_dds FormatFromFourcc path).
  if (!data || size < 4 + sizeof(DdsHeader)) return false;
  uint32_t magic = 0;
  std::memcpy(&magic, data, 4);
  if (magic != kDdsMagic) return false;
  DdsHeader hdr{};
  std::memcpy(&hdr, data + 4, sizeof(hdr));
  if (hdr.size != 124 || hdr.pf.size != 32 || hdr.width == 0 || hdr.height == 0)
    return false;
  if (hdr.pf.flags & kDdpfFourcc) {
    const uint32_t fc = hdr.pf.fourcc;
    return fc == fourcc_u32("DXT1") || fc == fourcc_u32("DXT3") ||
           fc == fourcc_u32("DXT5");
  }
  if (hdr.pf.flags & kDdpfRgb) {
    const uint32_t bpp = hdr.pf.rgb_bit_count / 8;
    return bpp == 2 || bpp == 3 || bpp == 4;
  }
  return false;
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

// PE Gfx_EngineFmtToD3D @ 0x618330 after probe @ 0x4B8A20 (DXT rows runtime).
#ifdef _WIN32
D3DFORMAT engine_fmt_to_d3d(int32_t engine_fmt) {
  switch (engine_fmt) {
    case 0:
      return D3DFMT_R5G6B5;
    case 1:
      return D3DFMT_A1R5G5B5;
    case 2:
      return D3DFMT_X8R8G8B8;
    case 6:
      return D3DFMT_DXT1;  // runtime dword_618348 = 'DXT1'
    case 7:
      return D3DFMT_DXT5;  // runtime dword_61834C = 'DXT5'
    case 8:
      return D3DFMT_A8L8;  // image 0x3C
    case 13:
      return D3DFMT_DXT3;  // runtime dword_618364 = 'DXT3'
    case 3:
    case 4:
    case 5:
    case 9:
    default:
      return D3DFMT_A8R8G8B8;  // Config.texture_format default 3
  }
}
#endif

// Soft PE SumMipBppWeight@4F6230 bpp-class from engine_fmt (no GetLevelDesc).
int32_t engine_fmt_mip_weight(int32_t engine_fmt) {
  switch (engine_fmt) {
    case 6:   // DXT1 — PE 0.5 → Engine_ftol → 0
      return 0;
    case 7:   // DXT5
    case 13:  // DXT3
      return 1;
    case 0:
    case 1:
      return 2;
    case 2:
      return 3;
    default:
      return 4;
  }
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

bool render_d3d9_texture_create_dims(void* key, int32_t w, int32_t h,
                                     int32_t levels, int32_t engine_fmt,
                                     const char* label) {
  // PE GfxDevice_CreateTexture @ 0x4F8A70 → Body@4F7740 →
  // GfxTexture_CreateEmpty@4F5E70(w,h,levels,fmt). levels==0 → auto-count
  // by shifting w/h until zero (Body@4F7759). Type1@50717a / Type2@50791d.
  if (!key || w <= 0 || h <= 0) return false;
  int32_t mips = levels;
  if (mips <= 0) {
    mips = 0;
    for (int32_t tw = w, th = h; tw != 0 && th != 0; tw >>= 1, th >>= 1) ++mips;
    if (mips < 1) mips = 1;
  }
  TextureState& st = g_textures[key];
  texture_clear_async_mip(st);
  st.label = label ? label : "";
  st.w = w;
  st.h = h;
  st.mips = mips;
  st.engine_fmt = engine_fmt;
  st.luma_alpha = false;
  st.mip_level_ready = false;
#ifdef _WIN32
  if (!g_dev) return true;  // dims bookkeeping without GPU (headless)
  texture_release_gpu(st);
  const D3DFORMAT fmt = engine_fmt_to_d3d(engine_fmt);
  // PE CreateEmpty: fmt 9/11 → Usage=RENDERTARGET Pool=DEFAULT; else Usage=0
  // Pool=MANAGED. Host: managed for Type1/2 load-path (depth/RT extras OOS).
  DWORD usage = 0;
  D3DPOOL pool = D3DPOOL_MANAGED;
  if (engine_fmt == 9 || engine_fmt == 11) {
    usage = D3DUSAGE_RENDERTARGET;
    pool = D3DPOOL_DEFAULT;
  }
  if (FAILED(g_dev->CreateTexture(static_cast<UINT>(w), static_cast<UINT>(h),
                                  static_cast<UINT>(mips), usage, fmt, pool,
                                  &st.tex, nullptr)) ||
      !st.tex) {
    st.tex = nullptr;
    return false;
  }
  return true;
#else
  (void)engine_fmt;
  return st.w > 0 && st.h > 0;
#endif
}

bool render_d3d9_texture_create_cube_dims(void* key, int32_t edge,
                                          int32_t levels, int32_t engine_fmt,
                                          const char* label) {
  // Soft PE GfxCubeTexture_CreateEmpty @ 0x4F6ED0 — device CreateCubeTexture
  // vt+100 (EdgeLength, Levels, Usage, Format, Pool). vftable off_5F2990.
  // fmt 9/11 → Usage=1 (RT); 10/12 → Usage=2; those use Pool=DEFAULT else
  // MANAGED. levels passed through (no auto-count unlike CreateTexture Body).
  if (!key || edge <= 0) return false;
  int32_t mips = levels;
  if (mips < 1) mips = 1;
  TextureState& st = g_textures[key];
  texture_clear_async_mip(st);
  st.label = label ? label : "";
  st.w = edge;
  st.h = edge;
  st.mips = mips;
  st.engine_fmt = engine_fmt;
  st.luma_alpha = false;
  st.mip_level_ready = false;
#ifdef _WIN32
  if (!g_dev) return true;
  texture_release_gpu(st);
  const D3DFORMAT fmt = engine_fmt_to_d3d(engine_fmt);
  DWORD usage = 0;
  D3DPOOL pool = D3DPOOL_MANAGED;
  if (engine_fmt == 9 || engine_fmt == 11) {
    usage = D3DUSAGE_RENDERTARGET;
    pool = D3DPOOL_DEFAULT;
  } else if (engine_fmt == 10 || engine_fmt == 12) {
    usage = D3DUSAGE_DEPTHSTENCIL;
    pool = D3DPOOL_DEFAULT;
  }
  if (FAILED(g_dev->CreateCubeTexture(static_cast<UINT>(edge),
                                      static_cast<UINT>(mips), usage, fmt, pool,
                                      &st.cube, nullptr)) ||
      !st.cube) {
    st.cube = nullptr;
    return false;
  }
  return true;
#else
  (void)engine_fmt;
  return st.w > 0;
#endif
}

bool render_d3d9_texture_create_cube_from_mem(void* key, const uint8_t* data,
                                              size_t size, int32_t engine_fmt,
                                              int32_t a5_mip_flag,
                                              const char* label) {
  // Soft PE GfxDevice_CreateCubeTextureFromFile @ 0x4F8AA0 / Body@4F7FE0.
  // Stock: path → D3DX_CreateCubeTextureFromFile@5925DC (MappedFile_Open) →
  // D3DX_CreateCubeTextureFromFileInMemoryEx@59253C (Body@591D85 a16=5).
  // Host: buffered bytes + GetProcAddress InMemoryEx; else DDS LockRect cube
  // (W34-02). Does not replace create_cube_dims / upload_cube_face /
  // CopyAllFaces / Poll.
  if (!key || !data || size == 0) return false;
  TextureState& st = g_textures[key];
  st.label = label ? label : "";
#ifdef _WIN32
  if (try_d3dx_create_cube_from_mem(st, data, size, engine_fmt, a5_mip_flag))
    return true;
  // W34-02: CreateCubeTexture + LockRect face/mip unpack (mirror upload_dds).
  // a5==0 → MipLevels=1 (PE Body@4F7FFB); else all DDS mips.
  const int32_t max_mips = (a5_mip_flag == 0) ? 1 : 0;
  if (upload_dds_cube(st, data, size, max_mips)) {
    // Body stores caller engine_fmt on GfxCubeTexture+16 (a3), not DDS-derived.
    st.engine_fmt = engine_fmt;
    return true;
  }
  st.engine_fmt = engine_fmt;
  return false;
#else
  (void)engine_fmt;
  (void)a5_mip_flag;
  return false;
#endif
}

int32_t render_d3d9_texture_clamp_async_mips(int32_t* w, int32_t* h,
                                             int32_t levels, int32_t lod_bias,
                                             bool floor4) {
  // PE Type1@507143 / Type2@5078ba: out = levels - dword_6188B4; if <1 → 1;
  // if out < levels: w/h >>= (levels-out). Type2 floors to 4 after.
  if (!w || !h || levels < 1) return 0;
  int32_t out = levels - lod_bias;
  if (out < 1) out = 1;
  if (out < levels) {
    const int32_t shift = levels - out;
    *w >>= shift;
    *h >>= shift;
  }
  if (floor4) {
    if (*w < 4) *w = 4;
    if (*h < 4) *h = 4;
  }
  if (*w < 1) *w = 1;
  if (*h < 1) *h = 1;
  return out;
}

int32_t render_d3d9_texture_sum_mip_bpp_weight(void* key) {
  // Soft PE GfxTexture_SumMipBppWeight @ 0x4F6230; cube variant
  // GfxCubeTexture_SumMipBppWeight @ 0x4F7120 multiplies each level by 6 faces.
  if (!key) return 0;
  auto it = g_textures.find(key);
  if (it == g_textures.end()) return 0;
  const TextureState& st = it->second;
  const int32_t mips = st.mips > 0 ? st.mips : 1;
  const int32_t per = mips * engine_fmt_mip_weight(st.engine_fmt);
#ifdef _WIN32
  if (st.cube) return per * 6;
#endif
  return per;
}

int32_t render_d3d9_texture_format_from_fourcc(uint32_t fourcc) {
  // PE GfxDevice_FormatFromFourcc @ 0x4F8990 (desc+8).
  if (fourcc == 0x31545844u) return 6;   // DXT1
  if (fourcc == 0x33545844u) return 13;  // DXT3
  if (fourcc == 0x35545844u) return 7;   // DXT5
  return 3;
}

int32_t render_d3d9_texture_bpp_from_fourcc(uint32_t fourcc) {
  // Soft PE GfxDevice_BppFromFourcc @ 0x4F89D0 (desc+8).
  // DXT1 ('DXT1'/827611204) → 8 bytes/block; else → 16 (DXT3/DXT5/…).
  return fourcc == 0x31545844u ? 8 : 16;
}

int32_t render_d3d9_texture_async_mip_rows_budget(int32_t mip_w) {
  // Soft PE Type2_Process @ 0x5079E6: Engine_ftol(16384.0 / mip_w)
  // (dbl_5F2D60 / fdivr). Used as BeginAsyncMipUpload rows_per_step.
  if (mip_w <= 0) return 1;
  const int32_t rows =
      static_cast<int32_t>(16384.0 / static_cast<double>(mip_w));
  return rows > 0 ? rows : 1;
}

int32_t render_d3d9_texture_async_mip_payload_offset(int32_t full_w,
                                                     int32_t full_h,
                                                     int32_t orig_levels,
                                                     int32_t done,
                                                     int32_t bpp_per_block) {
  // Soft PE Type2_Process @ 0x507A01..507A57 — byte offset into DXT mip
  // payload for the mip about to Begin. Iterations =
  // orig_levels - done - 1; each step adds (bw*bh*bpp) then w/h >>= 1.
  // bw = (w&~3)>=4 ? w>>2 : 1 (same for h). bpp = BppFromFourcc.
  if (bpp_per_block <= 0) return 0;
  int32_t iters = orig_levels - done - 1;
  if (iters <= 0) return 0;
  int32_t w = full_w;
  int32_t h = full_h;
  int32_t sum = 0;
  for (int32_t i = 0; i < iters; ++i) {
    const int32_t bw = ((w & ~3) >= 4) ? (w >> 2) : 1;
    const int32_t bh = ((h & ~3) >= 4) ? (h >> 2) : 1;
    sum += bw * bh * bpp_per_block;
    w >>= 1;
    h >>= 1;
  }
  return sum;
}

int32_t render_d3d9_texture_async_mip_level_index(int32_t levels,
                                                  int32_t done) {
  // Soft PE Type2_Process @ 0x50798A..5079AE: level = (levels - done) - 1.
  // Uploads coarsest remaining mip first (levels-1, then levels-2, …).
  return levels - done - 1;
}

int32_t render_d3d9_texture_async_mip_finish_lod(int32_t levels, int32_t done,
                                                 int32_t levels_store) {
  // Soft PE Type2_Process @ 0x507AA3..507ABD after Poll returns 0:
  // Finish(levels - min(done, levels_store) - 1). levels_store = job+144.
  int32_t v = levels_store;
  if (done < v) v = done;
  return levels - v - 1;
}

bool render_d3d9_texture_type2_begin_mip_step(
    void* key, int32_t levels, int32_t done, int32_t orig_levels,
    int32_t level0_w, int32_t level0_h, int32_t full_w, int32_t full_h,
    const void* payload_base, uint32_t fourcc) {
  // Soft PE Type2_Process Begin setup @ 0x50797B..507A78.
  // GetLevel0WH → shift by level_index → rows_budget; payload += offset;
  // Begin(level, src, rows, ddspf/fourcc). level0_* = clamped tex size;
  // full_* = pre-clamp hdr dims (job+112/+116).
  (void)level0_h;
  if (!key || !payload_base || levels < 1 || done < 0 || done >= levels)
    return false;
  const int32_t level =
      render_d3d9_texture_async_mip_level_index(levels, done);
  if (level < 0) return false;

  int32_t mip_w = level0_w >> level;
  if (mip_w < 1) mip_w = 1;
  const int32_t rows = render_d3d9_texture_async_mip_rows_budget(mip_w);
  const int32_t bpp = render_d3d9_texture_bpp_from_fourcc(fourcc);
  const int32_t off = render_d3d9_texture_async_mip_payload_offset(
      full_w, full_h, orig_levels, done, bpp);
  const auto* src = static_cast<const uint8_t*>(payload_base) + off;
  return render_d3d9_texture_begin_async_mip_upload(key, level, src, rows,
                                                    fourcc);
}

bool render_d3d9_texture_begin_async_mip_upload(void* key, int32_t level,
                                                const void* src,
                                                int32_t rows_per_step,
                                                uint32_t fourcc_or_0) {
  // Soft PE GfxTexture_BeginAsyncMipUpload @ 0x4F6880:
  // Release prior surface; GetSurfaceLevel(level); store src/step/ddspf.
  // Variant Flag@4F6830 (Type1) sets +0x28=flag, +0x34=0 — OOS here.
  if (!key || !src || level < 0) return false;
  auto it = g_textures.find(key);
  if (it == g_textures.end()) return false;
  TextureState& st = it->second;
  if (st.mips > 0 && level >= st.mips) return false;

  texture_clear_async_mip(st);
  st.mip_up_level = level;
  st.mip_up_rows_done = 0;
  st.mip_up_rows_step = rows_per_step > 0 ? rows_per_step : 1;
  st.mip_up_flag = 0;  // Begin Type2 path
  st.mip_up_src = static_cast<const uint8_t*>(src);
  st.mip_up_fourcc = fourcc_or_0;
  st.mip_level_ready = false;

#ifdef _WIN32
  if (st.tex) {
    if (FAILED(st.tex->GetSurfaceLevel(static_cast<UINT>(level),
                                       &st.mip_up_surf)) ||
        !st.mip_up_surf) {
      st.mip_up_surf = nullptr;
      return false;
    }
  }
#endif
  st.mip_up_active = true;
  return true;
}

bool render_d3d9_texture_poll_async_mip_upload(void* key) {
  // Soft PE GfxTexture_PollAsyncMipUpload @ 0x4F68D0.
  // Returns true while more rows remain (PE 1); false when level done (PE 0).
  // Stock: GetDesc → strip RECT + D3DX_LoadSurfaceFromMemory@5912E8
  // (destRECT=srcRECT, src=level base, Filter=1). Host: soft GetProcAddress
  // (W29B); LockRect strip fallback when d3dx9_*.dll absent.
  if (!key) return false;
  auto it = g_textures.find(key);
  if (it == g_textures.end()) return false;
  TextureState& st = it->second;
  if (!st.mip_up_active || !st.mip_up_src) return false;

  const int32_t level = st.mip_up_level;
  int32_t lw = st.w >> level;
  int32_t lh = st.h >> level;
  if (lw < 1) lw = 1;
  if (lh < 1) lh = 1;

#ifdef _WIN32
  // PE Poll@4F68EA: surface GetDesc → Width/Height for strip + pitch.
  if (st.mip_up_surf) {
    D3DSURFACE_DESC desc{};
    if (SUCCEEDED(st.mip_up_surf->GetDesc(&desc)) && desc.Width > 0 &&
        desc.Height > 0) {
      lw = static_cast<int32_t>(desc.Width);
      lh = static_cast<int32_t>(desc.Height);
    }
  }
#endif

  const int32_t bpp = async_mip_src_bpp(st.mip_up_fourcc, st.engine_fmt);
  const int32_t src_pitch = lw * bpp;
  int32_t y0 = st.mip_up_rows_done;
  int32_t y1 = y0 + st.mip_up_rows_step;
  if (y1 > lh) y1 = lh;
  st.mip_up_rows_done = y1;

#ifdef _WIN32
  if (st.tex && y1 > y0) {
    RECT rc{};
    rc.left = 0;
    rc.top = y0;
    rc.right = lw;
    rc.bottom = y1;

    // PE@4F6942..4F6950: fourcc at ddspf+8 as D3DFORMAT, else
    // 22 - (flag!=0) → X8R8G8B8 / A8R8G8B8.
    D3DFORMAT src_fmt = D3DFMT_X8R8G8B8;
    if (st.mip_up_fourcc != 0)
      src_fmt = static_cast<D3DFORMAT>(st.mip_up_fourcc);
    else if (st.mip_up_flag != 0)
      src_fmt = D3DFMT_A8R8G8B8;

    bool uploaded = false;
    if (st.mip_up_surf) {
      if (auto* pfn = d3dx_load_surface_from_memory()) {
        // PE@4F697A: a3=a8=&RECT, a4=src base (not y0-offset), a9=1.
        const HRESULT hr =
            pfn(st.mip_up_surf, nullptr, &rc, st.mip_up_src, src_fmt,
                static_cast<UINT>(src_pitch), nullptr, &rc, 1, 0);
        if (FAILED(hr)) {
          // PE@4F697C: D3DX fail → return 0 (leave surface; Begin releases).
          return false;
        }
        uploaded = true;
      }
    }

    // Fallback LockRect strip when D3DX absent (or no GetSurfaceLevel hold).
    if (!uploaded) {
      D3DLOCKED_RECT lr{};
      RECT lock_rc = rc;
      // DXT LockRect wants 4-aligned boxes — expand strip when compressed.
      const bool dxt = (st.mip_up_fourcc == 0x31545844u ||
                        st.mip_up_fourcc == 0x33545844u ||
                        st.mip_up_fourcc == 0x35545844u || st.engine_fmt == 6 ||
                        st.engine_fmt == 7 || st.engine_fmt == 13);
      if (dxt) {
        lock_rc.top = static_cast<LONG>(y0) & ~3L;
        lock_rc.bottom = static_cast<LONG>((y1 + 3) & ~3);
        if (lock_rc.bottom < lock_rc.top + 4) lock_rc.bottom = lock_rc.top + 4;
        const LONG lim = static_cast<LONG>((lh + 3) & ~3);
        if (lock_rc.bottom > lim) lock_rc.bottom = lim;
      }
      HRESULT hr = E_FAIL;
      if (st.mip_up_surf)
        hr = st.mip_up_surf->LockRect(&lr, &lock_rc, 0);
      else
        hr = st.tex->LockRect(static_cast<UINT>(level), &lr, &lock_rc, 0);
      if (SUCCEEDED(hr) && lr.pBits) {
        const uint8_t* src =
            st.mip_up_src +
            static_cast<size_t>(y0) * static_cast<size_t>(src_pitch);
        auto* dst = static_cast<uint8_t*>(lr.pBits);
        // When DXT top was aligned down, skip leading pad rows in dst.
        if (dxt && lock_rc.top < y0 && lr.Pitch > 0) {
          dst += static_cast<size_t>(y0 - static_cast<int32_t>(lock_rc.top)) *
                 static_cast<size_t>(lr.Pitch);
        }
        for (int32_t y = y0; y < y1; ++y) {
          const size_t n = static_cast<size_t>(src_pitch);
          std::memcpy(dst, src, n);
          src += n;
          dst += lr.Pitch;
        }
        if (st.mip_up_surf)
          st.mip_up_surf->UnlockRect();
        else
          st.tex->UnlockRect(static_cast<UINT>(level));
      }
    }
  }
#else
  (void)src_pitch;
#endif

  if (static_cast<unsigned>(st.mip_up_rows_done) < static_cast<unsigned>(lh))
    return true;  // PE: height > rows_done → 1

  // Level complete: Release surface, set +13 ready byte (PE).
#ifdef _WIN32
  if (st.mip_up_surf) {
    st.mip_up_surf->Release();
    st.mip_up_surf = nullptr;
  }
#endif
  st.mip_up_active = false;
  st.mip_level_ready = true;
  return false;
}

void render_d3d9_texture_finish_async_mip_upload(void* key, int32_t lod) {
  // Soft PE GfxTexture_FinishAsyncMipUpload @ 0x4F5DF0:
  // tex->SetLOD(lod); if fmt ∉ {9,10,11,12} → PreLoad.
  if (!key) return;
  auto it = g_textures.find(key);
  if (it == g_textures.end()) return;
  TextureState& st = it->second;
#ifdef _WIN32
  if (st.tex) {
    st.tex->SetLOD(lod < 0 ? 0u : static_cast<DWORD>(lod));
    const int32_t fmt = st.engine_fmt;
    if (fmt != 9 && fmt != 10 && fmt != 11 && fmt != 12) st.tex->PreLoad();
  }
#else
  (void)lod;
  (void)st;
#endif
}

bool render_d3d9_texture_async_mip_active(void* key) {
  if (!key) return false;
  auto it = g_textures.find(key);
  return it != g_textures.end() && it->second.mip_up_active;
}

void render_d3d9_texture_destroy(void* key) {
  if (!key) return;
  auto it = g_textures.find(key);
  if (it == g_textures.end()) return;
  texture_clear_async_mip(it->second);
#ifdef _WIN32
  // W35-02: soft PE TextureCache freelist (device+0x33C) — enqueue COM;
  // GfxDevice_DrainGpuCaches / drain_texture_cache_force Release.
  texture_enqueue_drain(it->second);
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
  if (it->second.tex != nullptr || it->second.cube != nullptr) return true;
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