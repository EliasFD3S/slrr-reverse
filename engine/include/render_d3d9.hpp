#pragma once

#include <cstddef>
#include <cstdint>

namespace inv {

// Phase 2.0 — minimal D3D9 present path (optional window).
// Headless by default; activate with --window or changeVideoMode.

bool render_d3d9_ready();
int32_t render_d3d9_width();
int32_t render_d3d9_height();

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

// Clear backbuffer + Present. No-op if device not ready.
void render_d3d9_flush();

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
// Java Viewport.RENDERFLAG_CLEARDEPTH / CLEARTARGET (resource.h). PE activate
// @ 0x00481680 unboxes the I but does not consume it (bind only).
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
// Phase 2.40 — Invictus .ptx = 36-byte header + JPEG payload.
bool render_d3d9_texture_create_from_ptx(void* key, const uint8_t* data,
                                         size_t size, const char* label);
// RPAK texture entries are usually text (`sourcefile path`) not embedded DDS.
bool render_d3d9_texture_create_from_rpak(void* key, const uint8_t* blob,
                                          size_t blob_size, const char* entry_name,
                                          const char* entry_path);
// Soft circle disc for minimap markers when rtype has no diffuse DDS.
bool render_d3d9_texture_create_solid(void* key, uint32_t argb, int32_t size);
void render_d3d9_texture_destroy(void* key);
bool render_d3d9_texture_ready(void* key);
int32_t render_d3d9_texture_width(void* key);
int32_t render_d3d9_texture_height(void* key);
const char* render_d3d9_texture_label(void* key);
// OSD createBG: DXT1 frontend plates have no alpha — derive A from luminance so
// FMV shows through dark texels (stock RectangleTemplate "solid alpha" material).
bool render_d3d9_texture_apply_luma_alpha(void* key);
// PE @ 0x0047C220 setGlobalEnvmap: store handle, no-op if same, null clears.
// No D3D SetTexture in that native (list bind on the resource engine).
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
