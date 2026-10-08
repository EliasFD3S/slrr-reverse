#pragma once

#include <cstddef>
#include <cstdint>

namespace inv {

// Phase 2.0 — minimal D3D9 present path (optional window).
// Headless by default; activate with --window or changeVideoMode.

bool render_d3d9_ready();
int32_t render_d3d9_width();
int32_t render_d3d9_height();
// Soft PE GfxDevice_presentCount @ 0x006495FC — frames Present'd this device.
int32_t render_d3d9_present_count();

// Create a visible Win32 window + D3D9 device. Returns false on failure.
bool render_d3d9_open(int32_t width, int32_t height, const char* title);

// Phase 2.123 — stock exe icon (.ico) + Win32 cursors (.cur) from assets/.
// Cursor ids match RT_CURSOR resource names (2..11); 0 = default arrow (2).
bool render_d3d9_assets_ready();
bool render_d3d9_set_stock_cursor(int32_t cursor_id);
int32_t render_d3d9_stock_cursor();
bool render_d3d9_stock_icon_loaded();
void render_d3d9_set_cursor_visible(int32_t visible);

void render_d3d9_close();

// HWND of the render window (nullptr if headless / not open).
void* render_d3d9_hwnd();

// Soft PE GfxEngine_PresentFrame @ 0x4FCA30 (stock order differs — see cpp):
//   SetPendingClearObj(0) device vt+0x128 → optional cam+0x4C →
//   Present vt+0x18 (unless GfxEngine_skipPresent) → BeginFrame vt+0x24 →
//   DrawCameraPass (ApplyViewport@516660 + DrawScene ClearTargetZ@4BA980) →
//   EndScene vt+0x28. Host: Clear/BeginScene/draw/EndScene/Present stand-in.
// No-op if device not ready.
void render_d3d9_flush();

// W35-02 — soft PE GfxDevice_DrainGpuCaches @ 0x4B7B90 size 0x40.
// Stock: if g_GfxDevice@6495DC; +0x33C DrainTextureCache(1)@4F8240;
// +0x320 DrainVBSlots(1)@4ECDB0; +0x324 DrainIBSlots(1)@4EBDF0.
// Host: COM freelists filled by texture/VB/IB destroy; force-drain Release.
// Caller: GfxEngine.flush @ 0x47C323 (after GCSweep). OOS: PE slot-row
// managers, budget a2=0 paths (dword_61832C / 617FBC / 617FC0), multi-
// buffer VB/IB ReleaseLast stacks.
void render_d3d9_drain_gpu_caches();

// Pump Win32 messages for up to `ms` milliseconds (0 = one peek).
void render_d3d9_pump(int32_t ms);

// Phase 2.126 — interactive loop: true after WM_QUIT / window destroy.
bool render_d3d9_quit_requested();
void render_d3d9_clear_quit();
void render_d3d9_request_quit();

int32_t render_d3d9_num_display_modes();
int32_t render_d3d9_curr_display_mode();
bool render_d3d9_change_video_mode(int32_t width, int32_t height, int32_t depth);

// Viewport registry (normalized [0,1] rects, matching java.render.Viewport).
// `key` is typically the InvObject* self pointer.
// Java Viewport.RENDERFLAG_CLEARDEPTH=0x1 / CLEARTARGET=0x2 (Viewport.java).
// PE activate @ 0x00481680 unboxes the I but does not consume it (bind only →
// GfxEngine_ViewportBind@4FD020 → list reorder @4FD280). Soft: pending_clear.
// World clear is PE GfxCamera_DrawScene @ 0x50EBA0 → GfxDevice_ClearTargetZ
// @ 0x4BA980 (device vt+0x1C), NOT PresentFrame: flags=(cam_mask&pass_flags),
// ClearTargetZ(flags&~1 /*target*/, flags&1 /*z*/, color=0xFF0000FF);
// D3D: (z?ZBUFFER=2:0)|(target?TARGET=1:0) — Java bits swapped vs D3D.
// Soft flush: pending_clear → Clear just before draw_meshes (after ApplyViewport).
constexpr int32_t kViewportClearDepth = 0x1;
constexpr int32_t kViewportClearTarget = 0x2;
constexpr int32_t kViewportClearMask = kViewportClearDepth | kViewportClearTarget;

void render_d3d9_viewport_create(void* key, int32_t pri, float x, float y,
                                 float w, float h);
void render_d3d9_viewport_destroy(void* key);
void render_d3d9_viewport_activate(void* key, int32_t renderflags);
// PE deactivate @ 0x004816C0: handle 0 silent; thiscall unbind off_6187B0.
void render_d3d9_viewport_deactivate(void* key);
// PE resize @ 0x00481700: FSTP [0,1] into rect+0x10/14/18/1C; handle 0 silent.
void render_d3d9_viewport_resize(void* key, float x, float y, float w, float h);
// PE getters: rect+0x10/14/18/1C = left/top/width/height (normalized).
// getAspect @ 0x004817B0: handle 0 → 1.0; else display_w/display_h.
float render_d3d9_viewport_get_aspect(void* key);
float render_d3d9_viewport_get_width(void* key);
float render_d3d9_viewport_get_height(void* key);
float render_d3d9_viewport_get_top(void* key);
float render_d3d9_viewport_get_left(void* key);
void* render_d3d9_viewport_active();

// Camera registry (java.render.Camera). `aov` is half-angle degrees as passed
// by Java (`create(..., aov*0.5, ...)`). Stock impl VA create=0x004861E0.
void render_d3d9_camera_create(void* key, void* parent, void* viewport,
                               int32_t pri, float half_aov_deg, float dmin,
                               float dmax, float lod_bias, float lod_amp,
                               int32_t oc, int32_t pt);
void render_d3d9_camera_destroy(void* key);
void render_d3d9_camera_activate(void* key, void* viewport, int32_t pri);
void render_d3d9_camera_deactivate(void* key, void* viewport);
void* render_d3d9_camera_active();
float render_d3d9_camera_half_aov(void* key);
float render_d3d9_camera_dmin(void* key);
float render_d3d9_camera_dmax(void* key);

// Phase 2.35 — look-at / chase view for the camera.
void render_d3d9_camera_lookat(void* key, float eye_x, float eye_y, float eye_z,
                               float at_x, float at_y, float at_z);
// Chase behind target facing yaw (forward = sin/cos); dist behind, height up.
void render_d3d9_camera_chase(void* key, float px, float py, float pz, float yaw,
                              float dist, float height, float look_height);
bool render_d3d9_camera_get_lookat(void* key, float* eye_x, float* eye_y,
                                   float* eye_z, float* at_x, float* at_y,
                                   float* at_z);

// Phase 2.86 — screen/NDC → world (ground plane at camera look-at Y).
// vx,vy in [-1,1] viewport NDC; vz unused (depth via plane hit).
bool render_d3d9_viewport_unproject(void* vp, void* cam, float vx, float vy,
                                    float* out_x, float* out_y, float* out_z);

// Phase 2.86 — screenshot stub (writes tiny marker file + counter).
bool render_d3d9_print_screen(const char* path);
int32_t render_d3d9_print_screen_count();
const char* render_d3d9_print_screen_last();

// Global fog. PE GroundRef.setFog @ 0x00486A20 (packet type 0x4A). Camera.setFog
// @ 0x00486570 writes a nested fog object (*10 via flt_5E7334); host uses this
// D3D path as stand-in until sub_5447D0 is mirrored. Applied on flush.
void render_d3d9_set_fog(int32_t color_rgb, float near_z, float far_z);
void render_d3d9_clear_fog();
bool render_d3d9_fog_enabled();
int32_t render_d3d9_fog_color();
float render_d3d9_fog_near();
float render_d3d9_fog_far();

// Phase 2.105 / race70 — RenderRef.setLight @ 0x00486AB0 → D3D dir+ambient
// (PE RGB * 1/256 via RenderRef_applyLight @ 0x0048C9D0).
void render_d3d9_set_light(int32_t diffuse_rgb, int32_t ambient_rgb,
                           int32_t specular_rgb);
void render_d3d9_clear_light();
bool render_d3d9_light_enabled();
int32_t render_d3d9_light_diffuse();
int32_t render_d3d9_light_ambient();
int32_t render_d3d9_light_specular();

// Phase 2.108 / race71 — RenderRef.setFlare @ 0x00486B20 → OSD glow sprites
// (PE RenderRef_applyFlare @ 0x0048CB40 stores min/max/color/count/rays as-is).
void render_d3d9_set_flare(void* key, void* glow_tex, int32_t glow_color,
                           float glow_min, float glow_max, int32_t flare_count,
                           int32_t ray_count);
void render_d3d9_set_flare_world(void* key, float wx, float wy, float wz);
void render_d3d9_clear_flare(void* key);
int32_t render_d3d9_flare_sources();
int32_t render_d3d9_flare_sprites_last();
void render_d3d9_set_flares_enabled(bool on);
bool render_d3d9_flares_enabled();
bool render_d3d9_flare_screen_pos(void* key, float* sx, float* sy);
// Phase 2.109 — world → OSD NDC [-1,1] via active camera (false if behind).
bool render_d3d9_project(float wx, float wy, float wz, float* ndc_x,
                         float* ndc_y);

// Textures (ResourceRef.makeTexture / RPAK→D3D later). Key = InvObject*.
// Supports DDS DXT1/DXT3/DXT5 (+ A8R8G8B8). No-op / false if device missing.
bool render_d3d9_texture_create_from_file(void* key, const char* path);
bool render_d3d9_texture_create_from_memory(void* key, const uint8_t* data,
                                            size_t size, const char* label);
// W29B — PE GfxDevice vt+0xF0 CreateTextureFromMem @ 0x4F8A40 /
// Body@4F7AE0(buf,n,engine_fmt,a5). Stock: D3DX_CreateTextureFromFileInMemoryEx
// @5924FF → Body@591D85 (device CreateTexture vt+92 + D3DX_LoadSurfaceFromMemory
// @5912E8 per level). a5==0 → MipLevels=1 (Type0@506671); fmt 9/11 →
// Usage=RENDERTARGET Pool=DEFAULT else MANAGED. Host: soft GetProcAddress
// d3dx9_*.dll (no link); else CreateTexture+LockRect DDS/PTX fallback.
bool render_d3d9_texture_create_from_mem(void* key, const uint8_t* data,
                                         size_t size, int32_t engine_fmt,
                                         int32_t a5_mip_flag,
                                         const char* label);
// Soft PE GfxTexture_UploadLevelFromMem_A8R8G8B8 @ 0x4F6710:
// GetSurfaceLevel(level) + LoadSurfaceFromMemory(A8R8G8B8) or LockRect.
// Fills an existing create_dims texture (dims path leftover).
bool render_d3d9_texture_upload_level_argb(void* key, int32_t level,
                                           const void* src_argb, int32_t src_w,
                                           int32_t src_h);
// W31B — soft PE GfxCubeTexture_CreateEmpty @ 0x4F6ED0 (CreateCubeTexture
// vt+100) + UploadCubeFaceFromMem A8@4F7260 / X8@4F7300 (GetCubeMapSurface
// + D3DX_LoadSurfaceFromMemory@5912E8 Filter=1, or LockRect). face 0..5.
bool render_d3d9_texture_create_cube_dims(void* key, int32_t edge,
                                          int32_t levels, int32_t engine_fmt,
                                          const char* label);
bool render_d3d9_texture_upload_cube_face(void* key, int32_t face,
                                          int32_t level, const void* src_argb,
                                          int32_t src_w, int32_t src_h,
                                          bool x8r8g8b8);
// W33B / W34-02 — soft PE GfxDevice_CreateCubeTextureFromFile @ 0x4F8AA0 /
// Body@4F7FE0(path,engine_fmt,a4). Stock maps path via
// D3DX_CreateCubeTextureFromFile@5925DC (MappedFile_Open CreateFile+MapView)
// then D3DX_CreateCubeTextureFromFileInMemoryEx@59253C → Body@591D85 a16=5
// (cube). Host: GetProcAddress D3DXCreateCubeTextureFromFileInMemoryEx
// (fallback non-Ex); else CreateCubeTexture + LockRect DDS face unpack
// (POSX..NEGZ, caps2 CUBEMAP|all faces). a5_mip_flag==0 → MipLevels=1;
// fmt 9/11 → RT+DEFAULT else MANAGED (d3dx path only). Does not replace
// create_cube_dims / upload_cube_face / CopyAllFaces / Poll.
bool render_d3d9_texture_create_cube_from_mem(void* key, const uint8_t* data,
                                              size_t size, int32_t engine_fmt,
                                              int32_t a5_mip_flag,
                                              const char* label);
// W32B — soft PE GfxCubeTexture_CopyAllFacesFrom @ 0x4F7010: min(GetLevelCount)
// mips × 6 faces; GetCubeMapSurface both; D3DX_LoadSurfaceFromSurface@591807
// Filter=1 ColorKey=0 (GetProcAddress) or LockRect; set +13 ready.
bool render_d3d9_texture_copy_all_faces_from(void* dst_key, void* src_key);
// Soft IDirect3DDevice9::SetTexture(stage, cube|tex|null). Cube keys bind
// IDirect3DCubeTexture9 (IDirect3DBaseTexture9); key=nullptr clears stage.
bool render_d3d9_texture_set(int32_t stage, void* key);
// Soft PE content contract CRT_TEXTURE_PTX @ 0x618868 (CRT table @ 0x618840
// → DX/PTX/DDS/MESH stringptrs). File hdr 36B: +0 reserved0, +4 ver
// (reject 0; soft accepts 1..8), +8/+C w/h, +10 payload_a (=c+d),
// +14 jpeg0_c, +18 jpeg1_d (0=single), +1C pad, +20 float, +24 JPEG(s)
// of `a` bytes (FFD8). Further levels: 20B mip hdr + `a` JPEG until EOF.
// Host uploads last level's jpeg0 (largest / best quality). d>0 = dual
// stream (color+alpha stand-in OOS — color only).
bool render_d3d9_texture_create_from_ptx(void* key, const uint8_t* data,
                                         size_t size, const char* label);
// Soft PE content contract CRT_TEXTURE_DDS @ 0x618878: 'DDS ' + DDS_HEADER
// size 124 / pf.size 32; FOURCC DXT1/3/5 (FormatFromFourcc@4F8990 →
// engine 6/13/7) or RGB masks (A8R8G8B8 / R8G8B8 / R5G6B5).
// RPAK texture entries are usually text (`sourcefile path`) not embedded DDS.
bool render_d3d9_texture_create_from_rpak(void* key, const uint8_t* blob,
                                          size_t blob_size, const char* entry_name,
                                          const char* entry_path);
// Soft circle disc for minimap markers when rtype has no diffuse DDS.
bool render_d3d9_texture_create_solid(void* key, uint32_t argb, int32_t size);
// W25B — PE GfxDevice vt+0xEC CreateTexture @ 0x4F8A70 / body @ 0x4F7740 /
// GfxTexture_CreateEmpty @ 0x4F5E70. Type1_Process@50717a / Type2@50791d.
// engine_fmt = Config.texture_format (default 3 → A8R8G8B8) or Type2
// FormatFromFourcc@4F8990 (DXT1→6 DXT3→13 DXT5→7 else→3). Does not replace
// Type0 CreateTextureFromMem path (vt+0xF0 / texture_create_from_mem).
bool render_d3d9_texture_create_dims(void* key, int32_t w, int32_t h,
                                     int32_t levels, int32_t engine_fmt,
                                     const char* label);
// PE Type1/2 mip LOD clamp via dword_6188B4 (texture_size): levels' =
// max(1, levels - lod); if shrunk, w/h >>= (levels-levels'). Type2 also
// floors w/h to 4 after shrink. Returns effective levels.
int32_t render_d3d9_texture_clamp_async_mips(int32_t* w, int32_t* h,
                                             int32_t levels, int32_t lod_bias,
                                             bool floor4);
// Soft PE GfxTexture_SumMipBppWeight @ 0x4F6230 (DXT1=0.5→ftol 0; DXT2-5=1;
// RGB565-class=2; R8G8B8=3; else=4 per level). Host walks registered mips.
int32_t render_d3d9_texture_sum_mip_bpp_weight(void* key);
// PE GfxDevice_FormatFromFourcc @ 0x4F8990 — FOURCC at desc+8 → engine_fmt.
int32_t render_d3d9_texture_format_from_fourcc(uint32_t fourcc);
// PE GfxDevice_BppFromFourcc @ 0x4F89D0 — FOURCC at desc+8 → bytes/block
// (DXT1→8, else→16). Used by Type2_Process mip payload offset sum.
int32_t render_d3d9_texture_bpp_from_fourcc(uint32_t fourcc);
// W27B — soft PE Type2 async mip family (chosen over Type1 JPEG decode):
//   GfxTexture_BeginAsyncMipUpload @ 0x4F6880
//   GfxTexture_PollAsyncMipUpload  @ 0x4F68D0
//   GfxTexture_FinishAsyncMipUpload @ 0x4F5DF0
// Begin: GetSurfaceLevel(level) + bookkeeping (src / rows_per_step / fourcc).
// W30B Poll: soft D3DX_LoadSurfaceFromMemory@5912E8 (GetDesc strip RECT,
// dest=src RECT, Filter=1) via W29B GetProcAddress; LockRect fallback if
// d3dx9_*.dll absent. true = more rows (PE 1), false = done (PE 0).
// Finish: SetLOD(lod) + PreLoad when engine_fmt not 9/10/11/12.
// fourcc_or_0 = DDS_PIXELFORMAT.dwFourCC (PE a5+8); 0 → bpp from engine_fmt.
// W28B — soft Type2_Process orchestration helpers (no System job*):
//   rows_budget @ 0x5079E6: Engine_ftol(16384.0 / mip_w) → rows_per_step
//   payload_offset @ 0x507A01..507A57: sum (bw*bh*bpp) prior DXT blocks
//   level_index @ 0x50798A: (levels - done) - 1 (smallest mip first)
//   finish_lod @ 0x507AA3..507ABD: levels - min(done, levels_store) - 1
//   type2_begin_mip_step: compose Begin args from job counters + payload
// OOS: Type1 BeginMipDecode@506AD0 / StepMipUpload JPEG, System Type2_Process
// job wiring / EMA float@+0x18, non-DDS formats when d3dx9_*.dll absent,
// CreateCubeFromFile path-map wrapper (PE@4F7FE0→5925DC CreateFile/MapView;
// host already receives buffered bytes into create_cube_from_mem).
int32_t render_d3d9_texture_async_mip_rows_budget(int32_t mip_w);
int32_t render_d3d9_texture_async_mip_payload_offset(int32_t full_w,
                                                     int32_t full_h,
                                                     int32_t orig_levels,
                                                     int32_t done,
                                                     int32_t bpp_per_block);
int32_t render_d3d9_texture_async_mip_level_index(int32_t levels,
                                                  int32_t done);
int32_t render_d3d9_texture_async_mip_finish_lod(int32_t levels, int32_t done,
                                                 int32_t levels_store);
// Soft PE Type2_Process Begin setup @ 0x50797B..507A78.
// level0_w/h = GetLevel0WH stand-in (clamped create_dims size); full_w/h =
// job+112/+116 (pre-clamp hdr); orig_levels = job+108; levels/done =
// job+124/+128; payload_base = job+132.
bool render_d3d9_texture_type2_begin_mip_step(
    void* key, int32_t levels, int32_t done, int32_t orig_levels,
    int32_t level0_w, int32_t level0_h, int32_t full_w, int32_t full_h,
    const void* payload_base, uint32_t fourcc);
bool render_d3d9_texture_begin_async_mip_upload(void* key, int32_t level,
                                                const void* src,
                                                int32_t rows_per_step,
                                                uint32_t fourcc_or_0);
bool render_d3d9_texture_poll_async_mip_upload(void* key);
void render_d3d9_texture_finish_async_mip_upload(void* key, int32_t lod);
bool render_d3d9_texture_async_mip_active(void* key);
void render_d3d9_texture_destroy(void* key);
bool render_d3d9_texture_ready(void* key);
int32_t render_d3d9_texture_width(void* key);
int32_t render_d3d9_texture_height(void* key);
int32_t render_d3d9_texture_mips(void* key);
const char* render_d3d9_texture_label(void* key);
// OSD createBG: DXT1 frontend plates have no alpha — derive A from luminance so
// FMV shows through dark texels (stock RectangleTemplate "solid alpha" material).
bool render_d3d9_texture_apply_luma_alpha(void* key);
// PE @ 0x0047C220 setGlobalEnvmap: store handle @ g_GfxEngine+0x64, no-op
// if same, null clears list node (+0x58..+0x64). No D3D SetTexture there.
// Soft Present binds g_envmap → stage1 (COLOROP ADD @ PE 4CE53A;
// TEXCOORDINDEX=TCI_CAMERASPACEREFLECTIONVECTOR=0x30000 @ PE 4E7718).
void render_d3d9_set_global_envmap(void* key);
void* render_d3d9_global_envmap();

// OSD 2D blit queue (host stand-in for Rectangle RenderRef instances).
// Coords match Osd.createRectangle: center (x,y), size (w,h) in ~[-1,1] space
// where (0,0,2,2) is fullscreen.
void render_d3d9_osd_clear();
void render_d3d9_osd_add_rect(float x, float y, float w, float h, void* texture,
                              int32_t pri);
// Keyed upsert (replaces prior rect with same key). key=nullptr → add only.
void render_d3d9_osd_set_rect(void* key, float x, float y, float w, float h,
                              void* texture, int32_t pri);
void render_d3d9_osd_set_rect_color(void* key, float x, float y, float w,
                                    float h, void* texture, uint32_t argb,
                                    int32_t pri);
void render_d3d9_osd_remove_rect(void* key);
void render_d3d9_osd_set_rect_visible(void* key, int32_t visible);
int32_t render_d3d9_osd_count();

// OSD bitmap fonts (INVO v3 glyph mesh + greyscale TGA atlas + font.dat).
// Key = typically charset ResourceRef*. Align: 0=right 1=center 2=left (Text.java).
bool render_d3d9_font_load(void* key, const char* name);
// Resolve frontend:0xNN charset RID → RPAK entry name (simple20, slii24, …).
bool render_d3d9_font_load_from_rid(void* key, int32_t res_id);
bool render_d3d9_font_ready(void* key);
const char* render_d3d9_font_name(void* key);
int32_t render_d3d9_font_glyph_count(void* key);
int32_t render_d3d9_font_px_height(void* key);
float render_d3d9_font_measure_px(void* key, const char* text);
void render_d3d9_font_destroy(void* key);

// Text instances (java.render.Text). update() rebuilds an OSD text entry.
void render_d3d9_text_create(void* key, void* font, float x, float y);
void render_d3d9_text_destroy(void* key);
void render_d3d9_text_set_color(void* key, uint32_t argb);
void render_d3d9_text_set_align(void* key, int32_t align);
void render_d3d9_text_set_pos(void* key, float x, float y);
void render_d3d9_text_set_string(void* key, const char* utf8);
void render_d3d9_text_set_visible(void* key, int32_t visible);
const char* render_d3d9_text_get_string(void* key);
void render_d3d9_text_update(void* key);
int32_t render_d3d9_osd_text_count();

// W26B/W36 soft — AsyncLoad_Mesh_UploadVbFvf @ 0x503400 /
// UploadIbTris @ 0x5038E0 (ParseInvoChunks case4/5). Key = host handle for
// later System attach (mat vtbl+16/+20 OOS). Bookkeeping always; D3D
// CreateVertexBuffer(FVF=pe_fvf) / CreateIndexBuffer when device ready.
// Present draw (soft ≈ GfxDevice_DrawIndexedTris @ 0x4ED080):
//   SetStreamSource(0, vb+0x18, 0, stride@+0x0C) → SetIndices →
//   DrawIndexedPrimitive(TRIANGLELIST=4, baseVert, MinIndex=0, nVert,
//   startIndex, primCount). Soft: base/start=0, primCount=index_count/3;
//   SetFVF(pe_fvf) before draw (CreateVB FVF bind; PE wrapper omits SetFVF).
// Soft materials: TSS0 modulate; DXT3/5 (fmt 13/7) alpha; envmap stage1
// ADD + TCI_CAMERASPACEREFLECTIONVECTOR when set.
// OOS: full INVO chunk parse, GfxVb_PackVertex@4FF710 walk, AABB@job+204,
// mat UV query vtbl+8, CreateVB case0 device vt+0xCC, GfxMat_ApplyStages
// full shader paths, AltPresent exclusive FMV TSS/VB draw.
// Soft PE GfxVbLayout_FromFvf @ 0x4FE980 — stride only (layout table OOS).
int32_t render_d3d9_mesh_vb_stride_from_fvf(uint32_t pe_fvf);
// Soft INVO chunk flags → PE FVF (UploadVbFvf@503475..5034EE). tex_count
// is mat vtbl+8 UV slot count (pass explicitly; query OOS).
uint32_t render_d3d9_mesh_invo_flags_to_fvf(uint32_t invo_flags,
                                            int32_t tex_count);
bool render_d3d9_mesh_vb_create(void* key, int32_t vert_count, uint32_t pe_fvf,
                                int32_t stride, const void* bytes,
                                size_t bytes_len);
void render_d3d9_mesh_vb_destroy(void* key);
bool render_d3d9_mesh_vb_ready(void* key);
int32_t render_d3d9_mesh_vb_vert_count(void* key);
int32_t render_d3d9_mesh_vb_stride(void* key);
uint32_t render_d3d9_mesh_vb_fvf(void* key);
void* render_d3d9_mesh_vb_d3d(void* key);  // IDirect3DVertexBuffer9* or null
bool render_d3d9_mesh_ib_create(void* key, int32_t index_count,
                                const uint16_t* indices);
void render_d3d9_mesh_ib_destroy(void* key);
bool render_d3d9_mesh_ib_ready(void* key);
int32_t render_d3d9_mesh_ib_index_count(void* key);
void* render_d3d9_mesh_ib_d3d(void* key);  // IDirect3DIndexBuffer9* or null

// SCX / INVO meshes (ResourceRef render objects). Key = InvObject*.
// Format: magic "INVO", version 4, directory of (offset,type) chunks.
// Submesh pattern: mat(1) → meta(4) → verts(5) → indices(0).
// Vertex = pos3 + normal3 + uv2 (32 bytes). Indices = uint16 triangles.
bool render_d3d9_mesh_create_from_file(void* key, const char* path);
bool render_d3d9_mesh_create_from_memory(void* key, const uint8_t* data,
                                         size_t size, const char* label);
// Phase 2.39 — procedural sphere (INVO v3 skydome.SCX not yet parsed).
bool render_d3d9_mesh_create_skydome(void* key, float radius);
// PE Resource_cloneNative @ 0x00545230 — independent mesh (changeResource unique).
bool render_d3d9_mesh_clone(void* dst, void* src);
// PE ResourceRef.scaleMesh @ 0x00480390 → ResourceRef_applyScaleMesh @ 0x0048E7F0:
// bake vertex positions (and AABB). Clone copies verts so RectangleTemplate
// changeResource keeps the scale. Does not write instance MeshXform.sx.
bool render_d3d9_mesh_scale_vertices(void* key, float sx, float sy, float sz);
// Bind an already-loaded texture to a submesh (0 = default / skydome).
void render_d3d9_mesh_set_texture_at(void* mesh_key, int32_t submesh,
                                     void* texture_key);
void render_d3d9_mesh_set_texture(void* mesh_key, void* texture_key);
// PE RenderRef.setColor @ 0x00480310 → slot+0xCC packed DWORD as-is.
void render_d3d9_mesh_set_color(void* key, int32_t argb);
int32_t render_d3d9_mesh_get_color(void* key);
void render_d3d9_mesh_destroy(void* key);
bool render_d3d9_mesh_ready(void* key);
int32_t render_d3d9_mesh_submesh_count(void* key);
int32_t render_d3d9_mesh_vertex_count(void* key);
int32_t render_d3d9_mesh_index_count(void* key);
int32_t render_d3d9_mesh_textured_count(void* key);
void* render_d3d9_mesh_get_texture(void* key, int32_t submesh);
// Local AABB from parsed verts (false if mesh missing).
bool render_d3d9_mesh_local_bounds(void* key, float bmin[3], float bmax[3]);
// Copy up to max_count unique-ish positions (x,y,z interleaved). Returns count.
int32_t render_d3d9_mesh_copy_positions(void* key, float* xyz_interleaved,
                                        int32_t max_count);
// Local→world: scale * Ry(yaw)*Rx(pitch)*Rz(roll) * translate (row-vector D3D).
// Angles in radians (stock Ypr). Parent chain: World = Local * BoneLocal * ParentWorld.
void render_d3d9_mesh_set_transform(void* key, float px, float py, float pz,
                                    float yaw, float pitch, float roll,
                                    float sx, float sy, float sz);
void render_d3d9_mesh_set_parent(void* key, void* parent);
void render_d3d9_mesh_set_attach_bone(void* key, int32_t bone_id);
void* render_d3d9_mesh_get_parent(void* key);
int32_t render_d3d9_mesh_get_attach_bone(void* key);
int32_t render_d3d9_mesh_get_bone_id(void* key, const char* alias);
void render_d3d9_mesh_set_bone_local(void* key, int32_t bone_id, float px,
                                     float py, float pz, float yaw, float pitch,
                                     float roll);
void render_d3d9_mesh_get_transform(void* key, float* px, float* py, float* pz,
                                    float* yaw, float* pitch, float* roll,
                                    float* sx, float* sy, float* sz);
// Transform local origin through the full parent/bone chain into world space.
void render_d3d9_mesh_world_origin(void* key, float* wx, float* wy, float* wz);
// Queue for flush (drawn before OSD, after clear).
void render_d3d9_mesh_queue_clear();
void render_d3d9_mesh_queue_add(void* key);
int32_t render_d3d9_mesh_queue_count();

// Phase 2.159 — FMV: D3D device pointer + aspect-fit blit of an IDirect3DTexture9*.
void* render_d3d9_device();
void render_d3d9_draw_fullscreen_texture(void* d3d_texture);
// Letterbox/pillarbox into the backbuffer (stock TextureRenderer fit).
void render_d3d9_draw_video_texture(void* d3d_texture, int32_t src_w,
                                    int32_t src_h);

}  // namespace inv
