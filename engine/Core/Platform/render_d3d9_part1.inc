// Fragment 1/3 of render_d3d9.cpp — included, not compiled on its own.
// Split only to stay under the 128 KB editor buffer cap; the
// translation unit is unchanged. See native/tools/split_source.py.


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
// Soft PE GfxDevice_presentCount @ 0x006495FC — ++ in GfxDevice_Present
// @ 0x004BB423 after D3D Present(0,0,0,0). FMV_DirectShow_Open @ 0x4F92AD
// and RunUpdateQuad @ 0x4F94E9 read it (parity / odd BeginScene); Soft
// count here matches Present path; FMV %2 BeginScene consumer OOS until
// video_fmv reads this (header export not in this ticket).
int32_t g_present_count = 0;

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
  // PE GfxTexture+16 / CreateEmpty a5 — engine fmt index (Config.texture_format
  // or FormatFromFourcc). Used by soft SumMipBppWeight@4F6230.
  int32_t engine_fmt = 3;
  bool luma_alpha = false;
  // W27B/W28B — soft PE GfxTexture async mip (+0x1C level, +0x20 rows_done,
  // +0x24 surface, +0x28 flag, +0x2C src, +0x30 rows_step, +0x34 ddspf).
  // W28B adds Type2 rows_budget / payload_offset / level_index / finish_lod.
  int32_t mip_up_level = 0;
  int32_t mip_up_rows_done = 0;
  int32_t mip_up_rows_step = 0;
  int32_t mip_up_flag = 0;
  const uint8_t* mip_up_src = nullptr;
  uint32_t mip_up_fourcc = 0;
  bool mip_up_active = false;
  bool mip_level_ready = false;  // soft PE GfxTexture+13 byte after Poll done
#ifdef _WIN32
  IDirect3DTexture9* tex = nullptr;
  // W31B — soft PE GfxCubeTexture (+4) from CreateEmpty@4F6ED0.
  IDirect3DCubeTexture9* cube = nullptr;
  IDirect3DSurface9* mip_up_surf = nullptr;
#endif
};

#ifdef _WIN32
void texture_release_gpu(TextureState& st) {
  if (st.tex) {
    st.tex->Release();
    st.tex = nullptr;
  }
  if (st.cube) {
    st.cube->Release();
    st.cube = nullptr;
  }
}

// Soft PE: cube preferred over 2D for SetTexture (IDirect3DBaseTexture9*).
IDirect3DBaseTexture9* texture_d3d_base(const TextureState& st) {
  if (st.cube) return st.cube;
  return st.tex;
}

#ifdef _WIN32
// Soft PE envmap stage1 bind (setGlobalEnvmap@47C220 = store only).
// Present: COLOROP ADD @ PE material path 4CE53A (TSS stage1 op=7);
// TEXCOORDINDEX = D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR (0x30000) @
// PE 4E7718 / 4D9126 (SetTextureStageState vt+0x10C).
void apply_envmap_stage1(IDirect3DBaseTexture9* env_tex) {
  if (!g_dev) return;
  if (env_tex) {
    g_dev->SetTexture(1, env_tex);
    g_dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_ADD);
    g_dev->SetTextureStageState(1, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    g_dev->SetTextureStageState(1, D3DTSS_COLORARG2, D3DTA_CURRENT);
    g_dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    g_dev->SetTextureStageState(1, D3DTSS_TEXCOORDINDEX,
                                D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR);
  } else {
    g_dev->SetTexture(1, nullptr);
    g_dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    g_dev->SetTextureStageState(1, D3DTSS_TEXCOORDINDEX, 0);
  }
}
#endif

// W35-02 — soft freelists for GfxDevice_DrainGpuCaches @ 0x4B7B90.
// PE managers live at g_GfxDevice+0x33C (tex) / +0x320 (VB) / +0x324 (IB);
// destroy enqueues COM ptrs; Drain*(force=1) Release+clear. Live-key
// replace paths keep immediate texture_release_gpu / VB|IB->Release.
std::vector<IDirect3DBaseTexture9*> g_tex_drain_list;
std::vector<IDirect3DVertexBuffer9*> g_vb_drain_list;
std::vector<IDirect3DIndexBuffer9*> g_ib_drain_list;

void texture_enqueue_drain(TextureState& st) {
  if (st.tex) {
    g_tex_drain_list.push_back(st.tex);
    st.tex = nullptr;
  }
  if (st.cube) {
    g_tex_drain_list.push_back(st.cube);
    st.cube = nullptr;
  }
}

void drain_texture_cache_force() {
  // Soft PE GfxDevice_DrainTextureCache @ 0x4F8240 with a2=1 (no budget).
  for (IDirect3DBaseTexture9* p : g_tex_drain_list) {
    if (p) p->Release();
  }
  g_tex_drain_list.clear();
}

void drain_vb_slots_force() {
  // Soft PE GfxDevice_DrainVertexBufferSlots @ 0x4ECDB0 with a2=1.
  for (IDirect3DVertexBuffer9* p : g_vb_drain_list) {
    if (p) p->Release();
  }
  g_vb_drain_list.clear();
}

void drain_ib_slots_force() {
  // Soft PE GfxDevice_DrainIndexBufferSlots @ 0x4EBDF0 with a2=1.
  for (IDirect3DIndexBuffer9* p : g_ib_drain_list) {
    if (p) p->Release();
  }
  g_ib_drain_list.clear();
}
#endif
std::unordered_map<void*, TextureState> g_textures;
// Host analogue of g_GfxEngine/off_6187B0+0x64 (PE @ 0x0047C220).
// setGlobalEnvmap does not SetTexture; store only — do not bind here.
void* g_envmap = nullptr;

// W27B — release GetSurfaceLevel hold (PE Begin@4F6880 Release prior +0x24).
void texture_clear_async_mip(TextureState& st) {
#ifdef _WIN32
  if (st.mip_up_surf) {
    st.mip_up_surf->Release();
    st.mip_up_surf = nullptr;
  }
#endif
  st.mip_up_level = 0;
  st.mip_up_rows_done = 0;
  st.mip_up_rows_step = 0;
  st.mip_up_flag = 0;
  st.mip_up_src = nullptr;
  st.mip_up_fourcc = 0;
  st.mip_up_active = false;
}

// PE Poll@4F691C: DXT1 fourcc → 2 else 4 bytes/texel for src pitch.
int32_t async_mip_src_bpp(uint32_t fourcc, int32_t engine_fmt) {
  if (fourcc == 0x31545844u) return 2;  // 'DXT1'
  if (fourcc != 0) return 4;
  if (engine_fmt == 6) return 2;
  return 4;
}

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

// W26B — soft PE AsyncLoad_Mesh_UploadVbFvf @ 0x503400 wrapper (malloc 0x68):
//   +0x04 vert_count, +0x08 CPU blob, +0x0C pe_fvf, +0x1C layout
//   (GfxVbLayout_FromFvf@4FE980), stride @ +0x60.
struct MeshVbState {
  int32_t vert_count = 0;
  int32_t stride = 0;
  uint32_t pe_fvf = 0;
  std::vector<uint8_t> cpu;
  bool ready = false;
#ifdef _WIN32
  IDirect3DVertexBuffer9* vb = nullptr;
#endif
};
std::unordered_map<void*, MeshVbState> g_mesh_vbs;

// W26B — soft PE AsyncLoad_Mesh_UploadIbTris @ 0x5038E0 wrapper (malloc 20):
//   [0]=count [1]=blob [2]=type=2 [3]=stride=2; u16 triangle list.
// Present DrawIndexed @ 4ED080: PrimCount = count/3 (soft); BaseVertex/
// StartIndex = 0 (PE args a5/a6 when non-zero — host default 0).
struct MeshIbState {
  int32_t index_count = 0;
  std::vector<uint16_t> cpu;
  bool ready = false;
#ifdef _WIN32
  IDirect3DIndexBuffer9* ib = nullptr;
#endif
};
std::unordered_map<void*, MeshIbState> g_mesh_ibs;

// PE ResPayload_createBoneParentHook @ 0x0053F7E0 — Engine_malloc(0x1C).
// Layout (asm 0x53f81a..0x53f8a3):
//   +0x00 vtbl (off_5F355C; dtor BoneParentHook_dtor @ 0x45F8B0)
//   +0x04/+0x08 payload+0x78 dllist (fwd→sentinel, back→old tail)
//   +0x0C..+0x18 ResHandle_Link slice (a2 = hook+0x0C → node+0x44)
struct BoneParentHook {
  void* vtbl = nullptr;
  void* list_fwd = nullptr;   // +0x04
  void* list_back = nullptr;  // +0x08
  void* link0 = nullptr;      // +0x0C
  void* link1 = nullptr;      // +0x10
  int32_t node_field50 = 0;   // +0x14 ← *(node+0x50)
  void* owner_node = nullptr; // +0x18
};
static_assert(sizeof(BoneParentHook) == 0x1C, "PE hook size 0x1C");

// PE bone node (LinkOrUnlinkBone a3 / RenderPayload_findBoneById). Raw blob so
// +0x0C/+0x10 dllist, +0x44 ResHandle, +0x4C (=RH+8), +0xAC hook match PE.
struct PeBoneNode {
  uint8_t raw[0xB0]{};
};
static_assert(sizeof(PeBoneNode) == 0xB0, "bone through +0xAC");

// PE payload own-list arena: sentinel @ +0x168, head @ +0x178 (head ==
// sentinel+0x10). HostMidPayload stops ~0x88 — side map, same offsets.
struct PeBoneOwnList {
  uint8_t raw[0x17C]{};
  PeBoneOwnList() {
    void* sent = raw + 0x168;
    *reinterpret_cast<void**>(raw + 0x178) = sent;
    *reinterpret_cast<void**>(reinterpret_cast<char*>(sent) + 0x0C) = sent;
  }
};

struct MeshXform {
  float px = 0, py = 0, pz = 0;
  float oy = 0, op = 0, or_ = 0;
  float sx = 1, sy = 1, sz = 1;
  void* parent = nullptr;
  // When parent is set: Local * BoneLocal(parent, attach_bone) * ParentWorld.
  int32_t attach_bone = 0;
  // PE LinkOrUnlinkBone bone[+0xAC] = ResPayload_createBoneParentHook result.
  BoneParentHook* bone_parent_hook = nullptr;
  // Host PeBone for a4==0 / Rebind / indexed-slot splice (not InvObject*).
  PeBoneNode* pe_bone = nullptr;
  // PE setColor @ 0x00480310 → slot+0xCC DWORD as-is (no 1/256).
  int32_t color = 0;
  int32_t color_set = 0;
};
std::unordered_map<void*, PeBoneOwnList> g_bone_own_lists;
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
  // Soft: new IDirect3DDevice9 → presentCount restart (PE GfxDevice ctor
  // path zeros GfxDevice_presentCount@6495FC with device rebuild).
  g_present_count = 0;
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
int32_t render_d3d9_present_count() { return g_present_count; }

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
  // W35-02: soft +0x33C/+0x320/+0x324 freelists while device still alive.
  drain_texture_cache_force();
  drain_vb_slots_force();
  drain_ib_slots_force();
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
    texture_clear_async_mip(kv.second);
#ifdef _WIN32
    texture_release_gpu(kv.second);
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
  for (auto& kv : g_mesh_vbs) {
#ifdef _WIN32
    if (kv.second.vb) kv.second.vb->Release();
#endif
  }
  g_mesh_vbs.clear();
  for (auto& kv : g_mesh_ibs) {
#ifdef _WIN32
    if (kv.second.ib) kv.second.ib->Release();
#endif
  }
  g_mesh_ibs.clear();
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
  // Soft full-RT rect; PE GfxDevice_SetViewportRect @ 0x4BD500 packs
  // D3DVIEWPORT9 MinZ=0 / MaxZ=0x3F800000 (1.0f) then device vt+0xBC.
  D3DVIEWPORT9 dvp{};
  dvp.X = 0;
  dvp.Y = 0;
  dvp.Width = static_cast<DWORD>(g_w);
  dvp.Height = static_cast<DWORD>(g_h);
  dvp.MinZ = 0.f;
  {
    const uint32_t maxz_bits = 0x3F800000u;
    std::memcpy(&dvp.MaxZ, &maxz_bits, sizeof(dvp.MaxZ));
  }
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
  // Soft PE GfxCamera_ApplyViewport @ 0x516660 → GfxEngine_SetViewport @
  // 0x4FD560 → GfxDevice_SetViewportRect @ 0x4BD500 (D3D SetViewport vt+0xBC).
  // PE: display_w/h from GfxDevice_GetDisplaySize @ 0x4BD5B0 (device+0x1DC/1E0);
  // pixel = norm_rect × display; MinZ=0 MaxZ=0x3F800000. Host clamps to RT.
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
  {
    const uint32_t maxz_bits = 0x3F800000u;
    std::memcpy(&dvp.MaxZ, &maxz_bits, sizeof(dvp.MaxZ));
  }
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

  // Soft PE PresentFrame draw path stand-in (GfxEngine_PresentFrame@4FCA30
  // → device vt+40 Present after draw). Default FVF = XYZ|NORMAL|TEX1
  // (0x112) — stock UploadVbFvf@503475..5034EE builds pe_fvf per mesh;
  // GfxDevice_DrawIndexedTris@4ED080 does not SetFVF (assumes prior bind).
  const DWORD fvf_default = D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1;
  g_dev->SetFVF(fvf_default);
  g_dev->SetRenderState(D3DRS_ZENABLE, TRUE);
  g_dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
  g_dev->SetRenderState(D3DRS_LIGHTING, TRUE);
  g_dev->SetRenderState(D3DRS_NORMALIZENORMALS, TRUE);
  g_dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
  g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
  g_dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
  g_dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
  g_dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
  g_dev->SetSamplerState(1, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
  g_dev->SetSamplerState(1, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);

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

  // Soft material: global envmap → stage1 (setGlobalEnvmap@47C220 store;
  // D3D bind via apply_envmap_stage1 — ADD + TCI reflection).
  IDirect3DBaseTexture9* env_tex = nullptr;
  if (g_envmap) {
    auto eit = g_textures.find(g_envmap);
    if (eit != g_textures.end()) env_tex = texture_d3d_base(eit->second);
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

    // Soft PE mesh upload Present bind: UploadVbFvf@503400 + UploadIbTris
    // @5038E0 → GfxDevice_DrawIndexedTris@4ED080 (SetStreamSource vt+400 /
    // SetIndices vt+416 / DrawIndexedPrimitive vt+328 TRIANGLELIST=4).
    const MeshVbState* uploaded_vb = nullptr;
    const MeshIbState* uploaded_ib = nullptr;
    {
      auto vit = g_mesh_vbs.find(key);
      auto iit = g_mesh_ibs.find(key);
      if (vit != g_mesh_vbs.end() && vit->second.ready && vit->second.vb &&
          iit != g_mesh_ibs.end() && iit->second.ready && iit->second.ib &&
          iit->second.index_count >= 3) {
        uploaded_vb = &vit->second;
        uploaded_ib = &iit->second;
      }
    }

    if (uploaded_vb && uploaded_ib) {
      // Soft materials (CMaterial_Tex stand-in): stage0 modulate tex×diffuse.
      IDirect3DBaseTexture9* tex0 = nullptr;
      int32_t tex_fmt = 3;
      if (!it->second.subs.empty() && it->second.subs[0].texture_key) {
        auto tit = g_textures.find(it->second.subs[0].texture_key);
        if (tit != g_textures.end()) {
          tex0 = texture_d3d_base(tit->second);
          tex_fmt = tit->second.engine_fmt;
        }
      }
      g_dev->SetTexture(0, tex0);
      if (tex0) {
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
      // DXT3/DXT5 (engine_fmt 13/7 via FormatFromFourcc@4F8990) → alpha blend.
      const bool alpha_tex = (tex_fmt == 7 || tex_fmt == 13);
      g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, alpha_tex ? TRUE : FALSE);
      if (alpha_tex) {
        g_dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        g_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
      }
      apply_envmap_stage1(env_tex);

      D3DMATERIAL9 mat{};
      const uint32_t dcol =
          (xf && xf->color_set) ? static_cast<uint32_t>(xf->color)
                                : (!it->second.subs.empty()
                                       ? it->second.subs[0].diffuse
                                       : 0xFF808080u);
      const float r = static_cast<float>((dcol >> 16) & 0xff) / 255.f;
      const float g = static_cast<float>((dcol >> 8) & 0xff) / 255.f;
      const float b = static_cast<float>(dcol & 0xff) / 255.f;
      mat.Diffuse.r = mat.Ambient.r = r;
      mat.Diffuse.g = mat.Ambient.g = g;
      mat.Diffuse.b = mat.Ambient.b = b;
      mat.Diffuse.a = mat.Ambient.a = 1.f;
      g_dev->SetMaterial(&mat);

      // Soft SetFVF(pe_fvf) — CreateVertexBuffer FVF; PE@4ED080 omits SetFVF.
      const DWORD pe_fvf =
          uploaded_vb->pe_fvf != 0 ? uploaded_vb->pe_fvf : fvf_default;
      g_dev->SetFVF(pe_fvf);
      // PE@4ED080: SetStreamSource(0, *(vb+0x18), 0, *(vb+0x0C)=stride);
      // SetIndices(*ib); DrawIndexed(4, baseVert, 0, nVert, startIdx, nPrim).
      // Soft hosts baseVert=0 / startIdx=0; nPrim = index_count/3 (IB tris).
      g_dev->SetStreamSource(0, uploaded_vb->vb, 0,
                             static_cast<UINT>(uploaded_vb->stride));
      g_dev->SetIndices(uploaded_ib->ib);
      const UINT nvert = static_cast<UINT>(uploaded_vb->vert_count);
      const UINT ntri =
          static_cast<UINT>(uploaded_ib->index_count / 3);
      if (nvert > 0 && ntri > 0)
        g_dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, nvert, 0, ntri);
      g_dev->SetStreamSource(0, nullptr, 0, 0);
      g_dev->SetIndices(nullptr);
      g_dev->SetFVF(fvf_default);
      apply_envmap_stage1(nullptr);
      g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
      continue;
    }

    for (const MeshSubmesh& sm : it->second.subs) {
      if (sm.verts.empty() || sm.indices.size() < 3) continue;

      IDirect3DBaseTexture9* tex = nullptr;
      int32_t tex_fmt = 3;
      if (sm.texture_key) {
        auto tit = g_textures.find(sm.texture_key);
        if (tit != g_textures.end()) {
          tex = texture_d3d_base(tit->second);
          tex_fmt = tit->second.engine_fmt;
        }
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
      // Soft materials: DXT3/5 alpha (FormatFromFourcc → fmt 13/7).
      const bool alpha_tex = (tex_fmt == 7 || tex_fmt == 13);
      g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, alpha_tex ? TRUE : FALSE);
      if (alpha_tex) {
        g_dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        g_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
      }
      apply_envmap_stage1(env_tex);

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
  apply_envmap_stage1(nullptr);
  g_dev->LightEnable(0, FALSE);
  g_dev->SetRenderState(D3DRS_LIGHTING, FALSE);
  g_dev->SetRenderState(D3DRS_NORMALIZENORMALS, FALSE);
  g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
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
    // Soft PE GfxEngine_PresentFrame @ 0x004FCA30 (forceRendering@47C1FC /
    // flush@47C2FC / MainLoop Present slot). Stock order:
    //   FlushDeferred@52CD40 → SetPendingClearObj(0) device vt+0x128
    //   → optional SetPendingClearObj(cam+0x4C) → if GfxEngine_skipPresent
    //   @65AECC==0: GfxDevice_Present @ 0x004BB410 (GfxDevice vt+0x18 →
    //   IDirect3DDevice9 vt+0x44 Present(0,0,0,0); ++presentCount@6495FC)
    //   → skipPresent=0 → BeginFrame device vt+0x24 (BeginScene)
    //   → DrawCameraPass@516A90 → ApplyViewport@516660 + DrawScene@50EBA0
    //     → ClearTargetZ @ 0x004BA980 (device vt+0x1C / D3D Clear @ vt+0xAC)
    //   → EndScene device vt+0x28. Soft host: Clear/BeginScene/draw/EndScene
    //   then Present (PE flips previous frame early when skipPresent==0).
    //
    // World Clear: PE ClearTargetZ @ 0x4BA980 — Count=0 pRects=null, Z=
    // 0x3F7FFF58, flags=(z?ZBUFFER:0)|(target?TARGET:0), color from
    // DrawScene 0xFF0000FF. Soft pending_clear (Java RENDERFLAG via
    // Viewport.activate) mirrors flags after apply_active_viewport, before
    // draw_meshes. Boot/splash (no pending): full TARGET|Z before BeginScene.
    D3DCOLOR kClear = D3DCOLOR_XRGB(18, 24, 38);
    if (g_fog.enabled) {
      kClear = D3DCOLOR_XRGB((g_fog.color >> 16) & 0xff, (g_fog.color >> 8) & 0xff,
                             g_fog.color & 0xff);
    }
    float kPeClearZ = 0.f;
    {
      // PE ClearTargetZ a6 immediate @ 0x4BA980 — int_convert 1065353048.
      const uint32_t zbits = 0x3F7FFF58u;
      std::memcpy(&kPeClearZ, &zbits, sizeof(kPeClearZ));
    }
    DWORD world_clear_flags = 0;
    bool have_world_clear = false;
    if (g_active_vp) {
      auto it = g_viewports.find(g_active_vp);
      if (it != g_viewports.end() && it->second.pending_clear != 0) {
        have_world_clear = true;
        // Java CLEARTARGET=0x2 → D3DCLEAR_TARGET; CLEARDEPTH=0x1 → ZBUFFER.
        // PE DrawScene@50EBE7: ClearTargetZ(flags&~1, flags&1, 0xFF0000FF).
        if (it->second.pending_clear & kViewportClearTarget)
          world_clear_flags |= D3DCLEAR_TARGET;
        if (it->second.pending_clear & kViewportClearDepth)
          world_clear_flags |= D3DCLEAR_ZBUFFER;
        it->second.pending_clear = 0;
      }
    }
    if (!have_world_clear)
      g_dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, kClear,
                   kPeClearZ, 0);
    if (SUCCEEDED(g_dev->BeginScene())) {
      // FMV + OSD use XYZRHW in full RT space — don't clip to a 3D viewport.
      set_fullscreen_viewport();
      video_fmv_present();
      apply_active_viewport();
      if (have_world_clear && world_clear_flags != 0) {
        // Soft ClearTargetZ@4BA980 stand-in (PE color 0xFF0000FF; host
        // fog/slate so sky/meshes read without a blue flash).
        g_dev->Clear(0, nullptr, world_clear_flags, kClear, kPeClearZ, 0);
      }
      draw_meshes();
      queue_flare_sprites();
      set_fullscreen_viewport();
      draw_osd_rects();
      draw_osd_texts();
      g_dev->EndScene();
    }
    // Soft PE GfxDevice_Present @ 0x004BB410: Present(0,0,0,0) + ++count.
    // Host after EndScene; PE PresentFrame calls it before BeginFrame.
    g_dev->Present(nullptr, nullptr, nullptr, nullptr);
    ++g_present_count;
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

void render_d3d9_drain_gpu_caches() {
  // Soft PE GfxDevice_DrainGpuCaches @ 0x4B7B90 size 0x40 (int_convert 64).
  //   mov esi, g_GfxDevice@6495DC; test esi → ret
  //   [esi+0x33C] → DrainTextureCache(1) @ 0x4F8240
  //   [esi+0x320] → DrainVertexBufferSlots(1) @ 0x4ECDB0
  //   [esi+0x324] → DrainIndexBufferSlots(1) @ 0x4EBDF0
  // Host: g_dev ≡ g_GfxDevice; freelist vectors ≡ manager objects (always
  // "present" when device open). a2=1 force — no budget early-out.
#ifdef _WIN32
  if (!g_dev) return;
  drain_texture_cache_force();
  drain_vb_slots_force();
  drain_ib_slots_force();
#endif
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
  // unused — only GfxEngine_ViewportBind@4FD020 (→ sub_4FD280 list bind with
  // a2=a3=-1). Soft: stash Java RENDERFLAG as pending_clear for flush world
  // Clear (stand-in for DrawScene→ClearTargetZ@4BA980). apply_active_viewport
  // is soft ApplyViewport@516660 / SetViewportRect@4BD500 (PE binds list only
  // here; pixel SetViewport runs at camera-pass draw).
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