// Split from natives_generated_world.cpp — RenderNatives.cpp
#include "natives.hpp"
#include "host_objects.hpp"
#include "runtime.hpp"
#include "render_d3d9.hpp"
#include "tree_interp.hpp"
#include "input_win32.hpp"
#include "video_fmv.hpp"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <array>
#include <string>
#include <unordered_map>
#include <vector>

namespace inv {

void java_render_Camera_create(InvObject* self, InvObject* parent, InvObject* vp, int32_t pri, float aov, float dmin, float dmax, float lodBias, float lodAmp, int32_t oc, int32_t pt) {
  // PE @ 0x004861E0 size 0x243 (579). UnboxArg
  // (Ljava.util.resource.ResourceRef;Ljava.render.Viewport;IFFFFFII)V:
  // this, parent, vp, pri, aov, dmin, dmax, lodBias, lodAmp, oc, pt.
  // Camera.java ctor passes aov*0.5 (half-angle deg) into this native.
  // handle = JVM_vm_get_int_field(this, dword_62E008). No Mighty ERROR.
  // Default bone pos Vector3(0,0,3.0 @ 0x40400000) + Mtx3x4_identity.
  // vpNode = GfxEngine_findByHandle(g_GfxEngine, vp);
  // aspect = vpNode[+0x78]/vpNode[+0x74] (= 1/(w/h) via fdiv+fdivr 1.0).
  // cam = ResourceEngine_type_rendertype_camera(&dword_636450, aspect, aov,
  //   dmin, dmax, lodBias, lodAmp, oc);
  // params = (*(cam->vtbl+0xC))(cam, 1.0f @ 0x3F800000); *(params+0x2C)=pt.
  // Temp list-stub on cam[+0x48/+0x50]; renderinst =
  // ResourceEngine_type_renderinst(parent, stub, "s_renderref_camera", 0);
  // Relink handle[+0/+4/+8/+0xC] onto renderinst list (+0x48/+0x50).
  // RenderRef_bindBone("bone00") + RenderRef_setBoneXform @ 0x0048C0F0
  // (handle, bone, 0, pos, mtx). GfxEngine_hookCameraViewport @ 0x004FCF10
  // (g_GfxEngine, vp, handle); GfxEngine_registerCameraPri @ 0x004FD150
  // (..., pri, -1). Unlink stub. Register Natives_RegisterAll @ 0x488CB6.
  // Host gaps (not mirrored here): ResourceEngine camera factory + aspect,
  // params+0x2C pt write, s_renderref_camera renderinst/list link,
  // bone00@(0,0,3)+Mtx3x4_id, GfxEngine vp hook/pri register.
  // Stand-in: RenderRef_create(parent) + render_d3d9_camera_create store;
  // empty-vp seed is host-only (stock assumes vp dims at +0x74/+0x78).
  if (self) {
    java_util_resource_RenderRef_create(self, parent, nullptr, nullptr);
  }
  render_d3d9_camera_create(self, parent, vp, pri, aov, dmin, dmax, lodBias,
                            lodAmp, oc, pt);
  if (vp) {
    if (render_d3d9_viewport_get_width(vp) <= 0.f &&
        render_d3d9_viewport_get_height(vp) <= 0.f) {
      render_d3d9_viewport_create(vp, 0, 0.f, 0.f, 1.f, 1.f);
    }
  }
}

void java_render_Camera_destroy(InvObject* self) {
  // PE @ 0x004864C0 size 0xae (174). UnboxArg ()V: this only.
  // handle = JVM_vm_get_int_field(this, dword_62E008).
  // handle==0 → Mighty ERROR ("!" + "Mighty ERROR" via CRT_strcat_n_thunk
  // + Engine_ErrorLogPrintf on Engine_ErrorLogBuf @ 0x0062E018); buf[0]=0.
  // Else: node=*(handle+0xC); if node: thiscall sub_419860(node,
  // 0xA0000000, 1.0f @ 0x3F800000, 0.f, 0.f) — same type tag as setFog
  // @ 0x00486570 (vtable+0x14 / sub_5447D0 / vtable+0xC). If result &&
  // (*(result+0x80) & 0xFFFF0000)==0xFFFF0000: thiscall
  // sub_48A8D0(result+0x78). Always thiscall sub_48A8D0(handle) (size 0x1c:
  // *(this+8)&&*(this+0xC) → sub_427620). No D3D in this native. Host:
  // render_d3d9_camera_destroy erases the camera map stand-in.
  render_d3d9_camera_destroy(self);
}

void java_render_Camera_activate(InvObject* self, InvObject* vp, int32_t pri) {
  render_d3d9_camera_activate(self, vp, pri);
}

void java_render_Camera_deactivate(InvObject* self, InvObject* vp) {
  render_d3d9_camera_deactivate(self, vp);
}

void java_render_GfxEngine_forceRendering() {
  // Stock 0x0047C1D0 size 0x43: static ()V, no UnboxArg, no this.
  // esi=7 loop: ResourceEngine sub_5378D0 + sub_537B40; dword_65AECC=0;
  // sub_4FCA30(off_6187B0); ++dword_6200A4. Same inner pump as
  // flush()V @ 0x0047C2D0 (size 0x55) minus tail sub_537ED0(1)+sub_4B7B90.
  // Engine_MainLoop calls the same three callees 1x/frame @ 0x00428CC9.
  // Not a distinct "render-now" flag. Host: one Present (do not 7x).
  render_d3d9_flush();
}

void java_render_GfxEngine_setGlobalEnvmap(InvObject* texture) {
  // PE @ 0x0047C220 size 0xA6 (int_convert 166) static
  // (Ljava.util.resource.ResourceRef;)V. Only callee JVM_UnboxArg @
  // 0x0045D910: dest0=nullptr (no this), dest1=&arg0 in-place. arg0 :=
  // Native.ptr 16-byte box (not jobject). handle := *(box+0xC) engine
  // resource. Node at g_GfxEngine/off_6187B0+0x58 (dwords +0/+4/+8/+0xC
  // = +0x58/+0x5C/+0x60/+0x64). Same handle → no-op. Else: if current
  // (+0x64) nonzero unlink via resource+0x48/+0x50 then zero node; if
  // new nonzero relink (same inline as ResourceRef.set @ 0x0047CFC0);
  // handle 0 clears node. No D3D/SetTexture. Register @ 0x488C1B.
  // Host: InvObject* stand-in for handle; g_envmap store only (no list).
  render_d3d9_set_global_envmap(texture);
}

int32_t java_render_GfxEngine_numDisplayModes() {
  // PE @ 0x0047C440 size 0x5 (int_convert 5) static ()I. No UnboxArg /
  // this, no Mighty ERROR. Bytes: E9 AB DA 03 00 = jmp rel →
  // Gfx_GetNumDisplayModes @ 0x004B9EF0 size 0x6 (int_convert 6). That
  // getter: A1 F4 95 64 00 / C3 = mov eax, dword_6495F4; retn. Sole
  // caller of Gfx_GetNumDisplayModes; do not rename dword_6495F4. Count
  // filled by GfxDevice enum sub_4B86B0 @ 0x004B86B0 size 0x2AA: thiscall
  // on device *(g_GfxEngine+0x204); IDirect3D9 vt+24 GetAdapterModeCount,
  // vt+28 EnumAdapterModes; CheckDeviceFormat vt+36 for D3DFMT 16/24/32;
  // append 20-byte rows (stride 0x14) at dword_6466A4 (+0 w, +4 h, +8
  // depth, +0x10 adapter index); ++dword_6495F4; dedupe scan prior rows
  // on w/h/depth. Orchestrator GfxDevice_EnumDisplayModes sub_4B8970 @
  // 0x004B8970 (GfxDevice ctor sub_4B7D00 @ 0x004B7F2F after Direct3DCreate9).
  // Register Natives_RegisterAll @ 0x488B9F. Host stand-in:
  // render_d3d9_num_display_modes → ensure_modes(); g_modes.size().
  return render_d3d9_num_display_modes();
}

int32_t java_render_GfxEngine_currDisplayMode() {
  // PE @ 0x0047C450 size 0x5 (int_convert 5) static ()I. No UnboxArg /
  // this, no Mighty ERROR. Bytes: E9 4B DA 03 00 = jmp rel →
  // Gfx_GetCurrDisplayMode @ 0x004B9EA0 size 0x4F (int_convert 79). Sole
  // xref of Gfx_GetCurrDisplayMode. If dword_6495F4<=0 → 0; else scan
  // 20-byte rows at dword_6466A4 (stride 0x14): w@+0 h@+4 depth@+8 vs
  // g_GfxDevice (dword_6495DC, set GfxDevice_ctor @ 0x004B7DFA) i16@+0x1BC
  // /+0x1BE/+0x1C0 (same triplet as changeVideoMode early-out; that path
  // loads device via *(g_GfxEngine+0x204)). Match → index; exhausted → 0.
  // Register Natives_RegisterAll @ 0x488BBE. Host stand-in:
  // render_d3d9_curr_display_mode scans g_modes vs g_w/g_h/depth.
  return render_d3d9_curr_display_mode();
}

InvObject* java_render_GfxEngine_displayModeName(int32_t i) {
  char buf[64];
  // PE @ 0x0047C460 size 0x41 (65). Static UnboxArg (I); sub_4B9F00 formats
  // "%d %d %d" from dword_6466A4[20*i] (w/h/depth); OOB → empty string.
  // OptionsDialog.show token(0/1/2) then UI "W X H X D" — not in the native.
  if (i == 0)
    std::snprintf(buf, sizeof(buf), "800 600 32");
  else if (i == 1)
    std::snprintf(buf, sizeof(buf), "1024 768 32");
  else if (i == 2)
    std::snprintf(buf, sizeof(buf), "1280 720 32");
  else if (i == 3)
    std::snprintf(buf, sizeof(buf), "1920 1080 32");
  else if (i < 0)
    buf[0] = '\0';
  else
    std::snprintf(buf, sizeof(buf), "800 600 %d", 32 + i);
  return string_new(buf);
}

void java_render_GfxEngine_changeVideoMode(int32_t width, int32_t height, int32_t depth) {
  // PE @ 0x0047C3F0 size 0x44 (int_convert 68) static (III)V.
  // UnboxArg dest0=nullptr (no this); dest1=width, dest2=height,
  // dest3=&CallInfo arg in-place = depth. ecx = *(g_GfxEngine+0x204)
  // (device from Gfx_InitDisplay); thiscall GfxDevice_vtbl vt+0x14 →
  // GfxDevice_ChangeVideoMode @ 0x004B9810 size 0x223: early-out if
  // i16@+0x1BC/+0x1BE/+0x1C0 match or byte@+0x1C2==1; else CheckDeviceFormat
  // (IDirect3D9 vt+0x28) pick D3DFMT; store dims; GetClientRect if windowed.
  // No ResetDevice in this native. Host: resize/open via render_d3d9.
  render_d3d9_change_video_mode(width, height, depth);
}

void java_render_GfxEngine_printScreen(InvObject* filename) {
  // PE @ 0x0047C3C0 size 0x25 static (Ljava.lang.String;)V. UnboxArg
  // dest0=nullptr (no this), dest1 in-place → char* path. thiscall
  // Engine_GfxPrintScreen @ 0x004FDF60 size 0x13 ecx=g_GfxEngine/off_6187B0.
  // device=*(engine+0x204) via Gfx_InitDisplay/sub_4FE940/sub_4B7D00
  // (off_5F1550); call vt+0x58 = GfxDevice_PrintScreen @ 0x004BC3A0
  // size 0x15b. fopen wb; dword_6495E4 vt+0x20 GetDisplayMode(0);
  // lock vt+0x50; TGA type-2 (dword 0x20000) + u16 w/h + bpp 0x18 +
  // descriptor 0x20; BGRA→BGR rows; unlock vt+0x54; fclose.
  // Not portable 1:1 (D3D lock+TGA). Host stand-in: marker file.
  const char* path = filename ? string_cstr(filename) : nullptr;
  render_d3d9_print_screen(path);
}

void java_render_GfxEngine_flush() {
  // PE @ 0x0047C2D0 size 0x55 static ()V. No UnboxArg. 7× same pump as
  // forceRendering @ 0x0047C1D0 (5378D0 + 537B40 + 4FCA30). Then unique
  // GC: sub_537ED0(g_ResourceEngine, 1) + sub_4B7B90 tex/VB/IB drain.
  // Present if any is inside shared 4FCA30 — not a flush-only Present.
  // Host: one Present. No GPU cache to drain (queue is a no-op here).
  render_d3d9_flush();
}

int32_t java_render_GfxEngine_openVideo(InvObject* filename, int32_t non_exclusive,
                                        int32_t loop) {
  // PE @ 0x0047C330 size 0x47. Unbox filename, non_exclusive I, loop I.
  // Path null → -1. Else FMV_DirectShow_Open @ 0x004F8DD0: success 0.
  // non_exclusive==0 RenderFile+HWND exclusive; !=0 TextureRenderer.
  // loop → dword_64A38C, EC_COMPLETE seek. On success Open writes
  // dword_64A394=1 @ 0x004F92EE (playing flag). Host video_fmv still
  // ignores non_exclusive (always SampleGrabber / RenderFile blit).
  const char* path = filename ? string_cstr(filename) : nullptr;
  return video_fmv_open(path, non_exclusive, loop);
}

void java_render_GfxEngine_closeVideo() {
  // PE @ 0x0047C380 size 0x5 (int_convert 5) static ()V. No UnboxArg /
  // this. jmp FMV_DirectShow_Close @ 0x004F9320: clears dword_64A394=0
  // then tears down DirectShow graph (contrast openVideo body; same
  // thunk shape as isPlayingVideo). Host: video_fmv_close.
  video_fmv_close();
}

int32_t java_render_GfxEngine_isPlayingVideo() {
  // PE @ 0x0047C390 size 0x5 (int_convert 5) static ()I. No UnboxArg /
  // this. jmp sub_4F9750 @ 0x004F9750 size 0x6: mov eax, dword_64A394;
  // retn (raw playing flag). Open writes 1 @ 0x004F92EE; Close writes 0
  // @ 0x004F9337 when byte_64A390!=0. Contrast: openVideo = Unbox+Open
  // body; closeVideo = jmp Close; this = jmp flag read only. Host:
  // video_fmv_is_playing → g_playing ? 1 : 0.
  return video_fmv_is_playing();
}

namespace {
// PE dword_612C64 .data ; u32 init 0xFFFFFFFF (int_convert 4294967295).
int32_t g_text_default_color = static_cast<int32_t>(0xFFFFFFFFu);
}

void java_render_Text_create(InvObject* self, InvObject* parent, InvObject* charset, float x, float z) {
  (void)parent;
  if (charset && !render_d3d9_font_ready(charset)) {
    const int32_t rid = java_util_resource_ResourceRef_id(charset);
    if (!render_d3d9_font_load_from_rid(charset, rid))
      render_d3d9_font_load(charset, "simple20");
  }
  render_d3d9_text_create(self, charset, x, z);
  if (self) {
    tree_field_set_float(self, "_text_px", x);
    tree_field_set_float(self, "_text_py", z);
    tree_field_set_float(self, "_text_pz", 0.f);
    tree_field_set_int(self, "_text_posed", 1);
    render_d3d9_text_set_color(self, static_cast<uint32_t>(g_text_default_color));
  }
}

void java_render_Text_setDefaultColor(int32_t color) {
  // PE @ 0x00487020 size 0x1f (int_convert 31). STATIC (I)V.
  // UnboxArg dest0=0 skip this, dest1=color I. dword_612C64 = edx
  // full DWORD (no AND / 0x00FFFFFF mask). Sibling get @ 0x00487040
  // size 0x6: mov eax, dword_612C64; retn.
  g_text_default_color = color;
}

int32_t java_render_Text_getDefaultColor() {
  // PE @ 0x00487040 size 0x6 (int_convert 6). STATIC ()I.
  // No UnboxArg / this, no Mighty ERROR. Bytes: A1 64 2C 61 00 C3 =
  // mov eax, Text_defaultColor @ dword_612C64; retn. .data init
  // 0xFFFFFFFF (Java Text.DEF_COLOR). Pair with setDefaultColor @
  // 0x00487020 (same global, full DWORD, no 0x00FFFFFF mask). Register
  // Natives_RegisterAll @ 0x488E87. Host: return g_text_default_color.
  return g_text_default_color;
}

void java_render_Text_changeColor(InvObject* self, int32_t color) {
  render_d3d9_text_set_color(self, static_cast<uint32_t>(color));
  render_d3d9_text_update(self);
}

void java_render_Text_changeAlign(InvObject* self, int32_t align) {
  render_d3d9_text_set_align(self, align);
  render_d3d9_text_update(self);
}

void java_render_Text_setPos(InvObject* self, float x, float z) {
  if (self) {
    tree_field_set_float(self, "_text_px", x);
    tree_field_set_float(self, "_text_py", z);
    tree_field_set_float(self, "_text_pz", 0.f);
    tree_field_set_int(self, "_text_posed", 1);
  }
  render_d3d9_text_set_pos(self, x, z);
  render_d3d9_text_update(self);
}

InvObject* java_render_Text_getPos(InvObject* self) {
  // PE @ 0x00487250 size 0xf5 (int_convert 245). Unbox this. renderinst +
  // RenderRef_bindBone("bone00") + sub_48C3D0 (7 xrefs, not renamed):
  // *(handle+8)==0 or bone 0 → nullptr. No Mighty ERROR. Not Vector3(0,0,0).
  // Else alloc Vector3 0x1C (28). qmemcpy 0x40 (64) from bone+0x54 (84);
  // xyz = mtx._41/_42/_43 * flt_5F08E8 (bits 0x3dcccccd, 1036831949).
  // setPos/create write Java (x,z,0) as engine (x,y,z). Host stash is already
  // Java units (no *10 in render_d3d9) — do not fmul 0.1 here.
  // !self / never create: handle+8==0 analogue → nullptr.
  if (!self) return nullptr;
  if (!tree_field_get_int(self, "_text_posed")) return nullptr;
  return vec3_new(tree_field_get_float(self, "_text_px"),
                  tree_field_get_float(self, "_text_py"),
                  tree_field_get_float(self, "_text_pz"));
}

void java_render_Text_update(InvObject* self) {
  if (!self) return;
  if (InvObject* s = tree_field_get_obj(self, "text"))
    render_d3d9_text_set_string(self, string_cstr(s));
  else
    render_d3d9_text_set_string(self, "");
  render_d3d9_text_update(self);
}

float java_render_Text_getWidthPixels(InvObject* str, InvObject* font) {
  // PE @ 0x00487410 size 0x44. Static UnboxArg. cstr==0 → 0.0 (no Mighty).
  // Inner Engine_TextMeasureWidth @ 0x004FDE50 unique; scale 1.0f flags 0.
  // font==0 PE crash [eax+8]; host-safe 0. PE ready-fail → 0.0 (no simple20).
  if (!str) return 0.f;
  if (font && !render_d3d9_font_ready(font)) {
    const int32_t rid = java_util_resource_ResourceRef_id(font);
    if (!render_d3d9_font_load_from_rid(font, rid))
      render_d3d9_font_load(font, "simple20");
  }
  return render_d3d9_font_measure_px(font, string_cstr(str));
}

void java_render_Viewport_create(InvObject* self, int32_t pri, float x, float y, float w, float h) {
  render_d3d9_viewport_create(self, pri, x, y, w, h);
}

void java_render_Viewport_destroy(InvObject* self) {
  // PE @ 0x00481600 size 0x80 (128). UnboxArg ()V: this only.
  // handle = JVM_vm_get_int_field(this, dword_62E008).
  // handle==0 → Mighty ERROR ("!" + "Mighty ERROR" via CRT_strcat_n_thunk
  // + Engine_ErrorLogPrintf on Engine_ErrorLogBuf @ 0x0062E018); buf[0]=0.
  // Else: thiscall Engine_ViewportUnbind (ecx=g_GfxEngine/off_6187B0,
  // arg=handle) — same callee as Viewport.deactivate @ 0x004816C0 (unique
  // xrefs: destroy@0x481631 + deactivate@0x4816ee; type *(node+0x4C)==0x12
  // list unbind) — then always thiscall sub_48A8D0(handle) (size 0x1c:
  // *(this+8)&&*(this+0xC) → sub_427620). Contrast Viewport.create @
  // 0x004814E0 (size 0x11c): ResHandle_Link/sub_538D60 bind, no unbind /
  // no sub_48A8D0. Contrast Camera.destroy @ 0x004864C0: no
  // Engine_ViewportUnbind; optional *(handle+0xC) + sub_419860 tag path
  // before the same sub_48A8D0(handle). No D3D in this native. Host:
  // render_d3d9_viewport_destroy erases the viewport map stand-in.
  render_d3d9_viewport_destroy(self);
}

void java_render_Viewport_activate(InvObject* self, int32_t renderflags) {
  // PE @ 0x00481680 size 0x3e. UnboxArg (I)V: this + renderflags I (DWORD at
  // box+8). handle = JVM_vm_get_int_field(this, dword_62E008).
  // handle==0 → silent return (no Mighty ERROR). Else sub_4FD020(handle, -1, -1)
  // unique callee (stdcall retn 0Ch); ecx=off_6187B0 is dead. sub_4FD020:
  // *(handle+0xC) && *(node+0x4C)==0x12 → sub_4FD280 list bind. Unboxed
  // renderflags never consumed in this 0x3e. Java Viewport.RENDERFLAG_CLEARDEPTH
  // = 0x1, CLEARTARGET = 0x2 (host kViewportClear*). Bind already mirrored.
  if (!self) return;
  render_d3d9_viewport_activate(self, renderflags & kViewportClearMask);
}

void java_render_Viewport_deactivate(InvObject* self) {
  // PE @ 0x004816C0 size 0x34. UnboxArg ()V: this only (no flags).
  // handle = JVM_vm_get_int_field(this, dword_62E008).
  // handle==0 → silent return (no Mighty ERROR). Else thiscall
  // Engine_ViewportUnbind (sub_4FD050) ecx=off_6187B0, arg=handle.
  // Inverse of activate's stdcall sub_4FD020: type *(node+0x4C)==0x12
  // list unbind. off_6187B0 is LIVE here (thiscall), dead on activate.
  if (!self) return;
  render_d3d9_viewport_deactivate(self);
}

float java_render_Viewport_getAspect(InvObject* self) {
  // PE @ 0x004817B0 size 0x37. UnboxArg ()F: this. handle =
  // JVM_vm_get_int_field(this, dword_62E008). FLD flt_5F08F0 (1.0 @
  // 0x005F08F0). handle==0 → RET with 1.0 (no Mighty ERROR). Else FSTP that
  // 1.0 and JMP Viewport_getAspect_inner @ 0x0048CDA0 size 0x82 (ecx=handle).
  // Inner: *(handle+0xC); 0 → 1.0. type *(node+0x4C)==1 skip vtable+0x14(1.0).
  // sub_5447D0(node, 0xA0000001, 0, 10.0); sign → 1.0. Else vtable+0xC(1.0)
  // rect. sub_4FD580 (ecx=off_6187B0) writes two display ints (vtable+0x64 on
  // engine+0x204). FLD [rect+0x18] FIMUL both ints, FDIVP → display_w/display_h
  // (width field cancels; +0x1C height unused). Fail → 1.0.
  return render_d3d9_viewport_get_aspect(self);
}

float java_render_Viewport_getWidth(InvObject* self) {
  // PE @ 0x004817F0 size 0x7a. UnboxArg ()F: this. handle =
  // JVM_vm_get_int_field(this, dword_62E008). handle==0 / inner=*(handle+0xC)==0
  // / vtable+0xC==0 → FLD [ESP+8] (unboxed this bits; no Mighty ERROR). Else
  // same walk as resize @ 0x00481700: type *(node+0x4C)==1 skip
  // vtable+0x14(1.0); sub_5447D0(node, 0x80000001, 0, 10.0); FLD [rect+0x18].
  // Units: Java create/resize (FFFF) stored as-is (no video_x scale) →
  // normalized [0,1]. Host ViewportState.w stands in for rect+0x18.
  return render_d3d9_viewport_get_width(self);
}

float java_render_Viewport_getHeight(InvObject* self) {
  // PE @ 0x00481870 size 0x7a. Same handle/inner/sub_5447D0 walk as getWidth
  // @ 0x004817F0. Success: FLD [rect+0x1C]. Fail: FLD [ESP+8], no Mighty ERROR.
  // Host ViewportState.h stands in for rect+0x1C (normalized [0,1]).
  return render_d3d9_viewport_get_height(self);
}

float java_render_Viewport_getTop(InvObject* self) {
  // PE @ 0x004818F0 size 0x7a. Same walk as getWidth. Success: FLD [rect+0x14].
  // Host ViewportState.y stands in for rect+0x14 (normalized [0,1]).
  return render_d3d9_viewport_get_top(self);
}

float java_render_Viewport_getLeft(InvObject* self) {
  // PE @ 0x00481970 size 0x7a. Same walk as getWidth. Success: FLD [rect+0x10].
  // Host ViewportState.x stands in for rect+0x10 (normalized [0,1]).
  return render_d3d9_viewport_get_left(self);
}

void java_render_Viewport_resize(InvObject* self, float x, float y, float w, float h) {
  // PE @ 0x00481700 size 0xa6. UnboxArg (FFFF)V: this + x,y,w,h (DWORD at
  // box+8 each). handle = JVM_vm_get_int_field(this, dword_62E008).
  // handle==0 / inner=*(handle+0xC)==0 / rect==0 → silent ret (no Mighty
  // ERROR). Else same walk as getWidth @ 0x004817F0: type *(node+0x4C)==1
  // skip vtable+0x14(1.0); sub_5447D0(node, 0x80000001, 0, 10.0); sign →
  // skip. Else vtable+0xC(1.0) rect. FLD unboxed x,y,w,h then FSTP
  // [rect+0x10/14/18/1C] as-is (no video_* scale). Units: Java [0,1]
  // (Viewport.java; Navigator.changeSize 0.02/0.78/0.2/0.18). Host
  // ViewportState.x/y/w/h stand in for those four floats.
  if (!self) return;
  render_d3d9_viewport_resize(self, x, y, w, h);
}

InvObject* java_render_Viewport_unproject(InvObject* self, InvObject* v,
                                          int32_t cameraId) {
  // PE @ 0x00487A60 size 0x176. Unbox this, Vector3, cameraId I.
  // Walk viewport cams; match [cam+0x630]==cameraId (0 is a real id, not
  // "active"). Miss → nullptr. sub_513840 uses xyz then ×0.1 / ×-0.1 / ×0.1.
  // Host: cameraId 0 / miss still falls back to active + vec3(0,0,0) — smoke
  // boot unproject is calibrated on that. Do not retarget without the probe.
  float vx = 0, vy = 0, vz = 0;
  if (v) vec3_get(v, &vx, &vy, &vz);
  void* cam = nullptr;
  if (cameraId != 0) {
    if (InvObject* o = resref_find_by_id(cameraId)) cam = o;
  }
  if (!cam) cam = render_d3d9_camera_active();
  float ox = 0, oy = 0, oz = 0;
  if (!render_d3d9_viewport_unproject(self, cam, vx, vy, &ox, &oy, &oz))
    return vec3_new(0, 0, 0);
  return vec3_new(ox, oy, oz);
}

// Sound / SfxRef audio: natives_resources.cpp + audio_win32.cpp (Phase 2.21)

// VA 0x0047EA10 — bind Animation to RenderRef + clip ResourceRef.

void java_render_Camera_setFog(InvObject* self, int32_t color, float near,
                               float far) {
  // PE @ 0x00486570 size 0x142. UnboxArg (IFF)V: this, color I, near F, far F
  // (DWORD at box+8). handle = JVM_vm_get_int_field(this, dword_62E008).
  // handle==0 → Mighty ERROR. Else walk *(handle+0xC) then *(node+0x84) via
  // vtable+0x14 / sub_5447D0(obj, 0xA0000000, 0.f, 0.f) / vtable+0xC(1.0f).
  // Fog object writes (no color mask, flt_5E7334=10.0 @ 0x005E7334):
  //   +0x24 = 1.0f, +0x28 = color, +0x1C = near*10, +0x20 = far*10.
  // No D3D / SetRenderState in this native. GroundRef.setFog @ 0x00486A20 is
  // a different path (16-byte packet type 0x4A). sub_5447D0 shared — not
  // mirrored (xrefs Config/Chassis/bind). Host: TREE on Camera + D3D stand-in.
  if (!self) return;
  const float n = near * 10.f;
  const float f = far * 10.f;
  tree_field_set_int(self, "fog_on", 1);
  tree_field_set_int(self, "fog_color", color);
  tree_field_set_float(self, "fog_near", n);
  tree_field_set_float(self, "fog_far", f);
  render_d3d9_set_fog(color & 0x00ffffff, n, f);
}

// VA 0x00480440 — GameRef/RenderRef.getDetail()F (LOD bias on resource).

}  // namespace inv
