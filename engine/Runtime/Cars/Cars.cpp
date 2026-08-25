// Split from natives_generated_world.cpp — Cars.cpp
#include "natives.hpp"
#include "host_objects.hpp"
#include "runtime.hpp"
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

#include "../Parts/world_state.hpp"

namespace inv {

int32_t java_game_GameLogic_kismajomCheck(InvObject* kismajomArray) {
  // PE @ 0x0047CB50 size 0xb1 (177). Sig ([Ljava.lang.String;)I static.
  // JVM_UnboxArg(CallInfo, dest0=nullptr) @ 0x0045D910 — static, array
  // stays in arg0. length = JVM_vm_get_int_field_by_name(arr, "length"
  // @ 0x00612D24) @ 0x0042A430. length==0 → -1 (or eax).
  // Walk i=length-1..0: elem = JVM_Array_getElement(arr,i) @ 0x0042B000;
  // cstr = JVM_vm_get_int_field(elem, dword_62E008) @ 0x0042AB50
  // (runtime Field*; BSS 0 at idle — String chars slot, not renamed here).
  // Inline strlen (repne scasb): empty → ecx=-1 → jl match (no ring probe).
  // Else movsx(encoded[c])-1 vs movsx(ring): walk ptr backward from
  // Input_cheatRingPtr @ 0x00612C68, wrap +16 if < Input_cheatRing
  // @ 0x00640924. Full match (ecx<0): * (ptr-1) = 0 (wrap); return i.
  // Exhaust → -1. No other side effects / no API beyond ring+JVM.
  // Host gaps: tree_vector_size/element_at + string_cstr stand in for
  // length/getElement/dword_62E008; match+zero via
  // input_cheat_try_match_encoded (g_cheat_ring in IO.cpp, not PE BSS).
  // null elem/cstr → miss (PE would deref); unsigned vs PE movsx on
  // high-bit bytes (cheats are ASCII+1). Empty "" matches like PE.
  const int32_t n = kismajomArray ? tree_vector_size(kismajomArray) : 0;
  if (n <= 0) return -1;
  for (int32_t i = n - 1; i >= 0; --i) {
    InvObject* enc = tree_vector_element_at(kismajomArray, i);
    const char* es = enc ? string_cstr(enc) : nullptr;
    if (input_cheat_try_match_encoded(es)) return i;
  }
  return -1;
}

// updateNavigator: natives_gameref.cpp

void java_game_Painter_doPaint(InvObject* self, InvObject* cursor, int32_t color,
                               int32_t brush, int32_t temp, float rot, float size,
                               int32_t flip) {
  // PE @ 0x00482960 Painter_doPaint size 0x293 (659). Sig (GameRef,IIIFFI)V.
  // JVM_UnboxArg dests (cdecl, I/F = box+8): this@var_120 unused — no
  // Mighty ERROR; cursor@var_13C; color@var_154; brush@var_150;
  // temp@var_140; rot@var_144; size@var_14C; flip@var_148.
  // Zero brush ResHandle@var_16C then thiscall sub_545FC0(&brushH, brush)
  // (30 xrefs — not renamed): stores RID at handle+8 (=var_164) via
  // sub_536820; sprintf second %d reads that slot (same as paintBrush.id()).
  // thiscall sub_426470(g_EngineState, cursor, GII_POS=2, &xyz[3]@var_118).
  // Cursor handle+8==0 → getInfo 0 → pick fails → no event. No CRT_rand
  // (contrast paintPart @ 0x004827D0). Engine_pickAt @ 0x0048D450(xyz,
  // &pick@var_17C, list@var_15C, scratch12, 0, scratch4, 0, hit@var_138,
  // nrm@var_12C, 0) — hit/nrm non-null (unlike paintPart nullptr outs).
  // getInfo(pick, GII_CATEGORY=7, dest0): EAX must be GIR_CAT_PART=9 or
  // GIR_CAT_VEHICLE=5 else thiscall sub_45FB80(list) + optional
  // ResHandle_Unlink(inst+0x44, &brushH|/&pick) → return 0.
  // Util_Sprintf(dst256, "bigdecal %d,%d %.3f,%.3f,%.3f %.3f,%.3f,%.3f "
  // "%.2f %.2f %d %d", color, brushH+8, hit, nrm, size, rot, temp, flip)
  // @ 0x00551220 (Invictus, not CRT). Engine_queueEvent(g_EngineState,
  // &pick, 0, EVENT_COMMAND=0x10, cmd, 0) trampoline @ 0x00426800 →
  // dispatch @ 0x004265C0. Epilogue always splices pick list + relinks
  // brushH/pick if owner!=0. Java: MODE_PAINTCOLOR / MODE_PAINTDECAL
  // (paintBrush.id(), temp 0|1); PE re-checks pick category.
  if (!self) return;
  PaintStroke s;
  s.cursor = cursor;
  s.color = color;
  s.brush = brush;
  s.temp = temp;
  s.rot = rot;
  s.size = size;
  s.flip = flip;
  g_painter[self].push_back(s);
  tree_field_set_int(self, "paint_count",
                     static_cast<int32_t>(g_painter[self].size()));
  tree_field_set_int(self, "paint_last_color", color);
  tree_field_set_int(self, "paint_last_brush", brush);
  tree_field_set_float(self, "paint_last_size", size);

  // PE dest is the picked handle (&var_17C), not Painter / not cursor.
  // Host has no Engine_pickAt world pick (nor bigdecal decode in
  // GameRef.queueEvent). Queue on cursor when Native.ptr analog != 0;
  // hit = cursor getPos; normals 0; brush = raw unbox (PE brushH+8
  // after wrap). paintPart/xPaint are separate PE bodies
  // (0x004827D0 / 0x00482C00) — not this helper.
  if (!cursor || java_util_resource_ResourceRef_id(cursor) == 0) return;
  float hx = 0.f, hy = 0.f, hz = 0.f;
  vec3_get(java_util_resource_GameRef_getPos(cursor), &hx, &hy, &hz);
  char buf[256];
  std::snprintf(buf, sizeof(buf),
                "bigdecal %d,%d %.3f,%.3f,%.3f %.3f,%.3f,%.3f %.2f %.2f %d %d",
                color, brush, hx, hy, hz, 0.f, 0.f, 0.f, size, rot, temp, flip);
  java_util_resource_GameRef_queueEvent(cursor, nullptr, 0x10, string_new(buf));
}

void java_game_Painter_paintPart(InvObject* self, InvObject* cursor,
                                 int32_t color) {
  // PE @ 0x004827D0 Painter_paintPart size 0x189 (393). Sig (GameRef,I)V.
  // JVM_UnboxArg cdecl dests (I = box+8): this@var_50 unused — no Mighty
  // ERROR; cursor@var_70; color@var_74. Bytes @0x4827FA: lea ecx,&xyz;
  // push ecx; push 2; push cursor; thiscall sub_426470(g_EngineState,…).
  // CRT_rand() — EAX discarded (paintPart-only; doPaint @ 0x00482960 /
  // xPaint @ 0x00482C00 have none). Copy xyz→ray; Engine_pickAt @
  // 0x0048D450(ray,&pick@var_8C,list@var_7C,scratch12,0,scratch4,0,
  // hit=0,nrm=0,0) — hit/nrm nullptr (unlike doPaint bigdecal outs).
  // getInfo(pick,GII_CATEGORY=7,dest0): EAX must be GIR_CAT_PART=9 or
  // GIR_CAT_VEHICLE=5 else thiscall sub_45FB80(list) + optional
  // ResHandle_Unlink(owner+0x44,&pick) → return 0.
  // Util_Sprintf(dst64,"paint 0 0 %d 0 -1",color@var_74) @ 0x00551220
  // (Invictus; asm mov edx,[var_74] — not Hex-Rays v9[2]).
  // Engine_queueEvent(g_EngineState,&pick,0,EVENT_COMMAND=0x10,cmd,0)
  // trampoline @ 0x00426800. Epilogue always splices list + relink if
  // owner!=0. Contrast xPaint (literal bigdecal, no pick) / doPaint
  // (sprintf bigdecal). Java MODE_PAINTPART pre-filters cat then calls
  // with paintColor|0xFF000000; PE re-checks pick category.
  if (!self) return;
  PaintStroke s;
  s.cursor = cursor;
  s.color = color;
  s.part_fill = true;
  g_painter[self].push_back(s);
  tree_field_set_int(self, "paint_count",
                     static_cast<int32_t>(g_painter[self].size()));
  tree_field_set_int(self, "paint_last_color", color);
  tree_field_set_int(self, "paint_part_fills",
                     tree_field_get_int(self, "paint_part_fills") + 1);
  // Colorize cursor target part texture id if present.
  if (cursor) tree_field_set_int(cursor, "part_texture", color);

  // PE dest is the picked handle, not Painter / not cursor. Host has no
  // Engine_pickAt world pick (nor "paint" decode in GameRef.queueEvent).
  // Queue on cursor when id!=0; soft cat gate when host classifies
  // (0 = unknown → allow; PE only queues after pick cat∈{5,9}).
  if (!cursor || java_util_resource_ResourceRef_id(cursor) == 0) return;
  constexpr int32_t kGiiCategory = 7;
  constexpr int32_t kGirCatVehicle = 5;
  constexpr int32_t kGirCatPart = 9;
  const int32_t cat =
      java_util_resource_GameRef_getInfo(cursor, kGiiCategory, 0);
  if (cat != 0 && cat != kGirCatPart && cat != kGirCatVehicle) return;
  char buf[64];
  std::snprintf(buf, sizeof(buf), "paint 0 0 %d 0 -1", color);
  java_util_resource_GameRef_queueEvent(cursor, nullptr, 0x10, string_new(buf));
}

void java_game_Painter_xPaint(InvObject* self, InvObject* part) {
  // PE @ 0x00482C00 Painter_xPaint size 0x34 (52). Sig (GameRef)V.
  // Single BB. Callees: JVM_UnboxArg @ 0x0045D910, Engine_queueEvent
  // trampoline @ 0x00426800 → Engine_queueEvent_dispatch @ 0x004265C0.
  // Prologue push ecx = var_4. UnboxArg cdecl dests: dest0=&var_4 (this,
  // unused — no Mighty ERROR), dest1=&arg_0 (part ResHandle* overwrites
  // CallInfo). No getInfo / Engine_pickAt / Util_Sprintf / CRT_rand.
  // Contrast paintPart @ 0x004827D0 (pick + "paint 0 0 %d 0 -1", queue
  // &pick) and doPaint @ 0x00482960 (pick + sprintf bigdecal).
  // Stack: push part (bare DWORD), 0, EVENT_COMMAND=0x10, aBigdecal000000
  // @ 0x006134D8 "bigdecal 0,0 0,0,0 0,0,0 0 0 1 0", 0; mov ecx,
  // g_EngineState @ 0x00636338; call trampoline (retn 14h). Dispatch
  // treats arg0 as ResHandle* ([ebx+0xC]) — no null check. Epilogue
  // pop ecx; retn — no pick-list splice (unlike paintPart).
  // Java MODE_PAINTDECAL: flush texture via xPaint(player.car).
  // Host: xpaint_* bookkeeping; queue always (PE ignores this).
  if (self) {
    tree_field_set_obj(self, "xpaint_part", part);
    tree_field_set_int(self, "xpaint_count",
                       tree_field_get_int(self, "xpaint_count") + 1);
  }
  java_util_resource_GameRef_queueEvent(
      part, nullptr, 0x10,
      string_new("bigdecal 0,0 0,0,0 0,0,0 0 0 1 0"));
}

InvObject* java_io_MouseCursor_getPos(InvObject* self) {
  // PE @ 0x00487970 size 0xeb (235). Sig ()Ljava.lang.Vector3;.
  // UnboxArg this. JVM_getFieldResourceNative(this, "cursor").
  // Handle==0: CRT_strcat_n_thunk(Engine_ErrorLogBuf, "!", 256) +
  // "Mighty Mouse ERROR 2" + Engine_ErrorLogPrintf + clear scratch + null.
  // Else thiscall Engine_queryGameRefChannel(g_EngineState @ 0x00636338,
  //   handle, GII_POS=2, &xyz). Return ignored. RESTYPE_GAME=8 →
  //   vtbl+0x3C Cursor_getInfo @ 0x004625F0 (ecx=inst, 0, 2, dest):
  //   dest!=0: dest[0]=inst[41]=+0xA4, dest[1]=inst[42]=+0xA8,
  //   dest[2]=inst[43] then overwrite dest[2]=inst[92]=+0x170.
  //   return 1. Cursor_tick / EVENT_COMMAND "move" write NDC at +0xA4/+0xA8.
  // Engine_malloc(0x1C=28) Vector3, JVM_getClass, JVM_Object_vtbl,
  // JVM_Instance_initialize(class, 0), set float x,y only — z left at
  // ctor 0 (channel fills z but JNI never sets "z").
  // Contrast getPickedPos @ 0x00487870: query 59, sets x,y,z; ERROR
  // without " 2". Host: cursor_x/y (+ cursor_set) stand-in for +0xA4/+0xA8;
  // else GameRef_getPos xy (z discarded). Always vec3 z=0. No Mighty log.
  InvObject* inner = self ? tree_field_get_obj(self, "cursor") : nullptr;
  if (!inner) return nullptr;
  float cx = 0.f, cy = 0.f, cz = 0.f;
  if (tree_field_get_int(inner, "cursor_set") != 0) {
    cx = tree_field_get_float(inner, "cursor_x");
    cy = tree_field_get_float(inner, "cursor_y");
  } else {
    vec3_get(java_util_resource_GameRef_getPos(inner), &cx, &cy, &cz);
    (void)cz;
  }
  return vec3_new(cx, cy, 0.f);
}

InvObject* java_io_MouseCursor_getPickedPos(InvObject* self) {
  // PE @ 0x00487870 size 0xff (255). Sig ()Ljava.lang.Vector3;.
  // UnboxArg this. JVM_getFieldResourceNative(this, "cursor").
  // Handle==0: CRT_strcat_n_thunk(Engine_ErrorLogBuf, "!", 256) +
  // "Mighty Mouse ERROR" + Engine_ErrorLogPrintf + clear scratch + null.
  // Else thiscall Engine_queryGameRefChannel(g_EngineState @ 0x00636338,
  //   handle, 59=0x3B, &xyz). Return ignored. RESTYPE_GAME=8 →
  //   vtbl+0x3C Cursor_getInfo @ 0x004625F0 (ecx=inst, 0, 59, dest):
  //   dest!=0: dest[0..2] = inst[+0x164/+0x168/+0x16C] (dword[89,90,91]).
  //   eax = inst[+0x144] (dword[81] pick handle). dest==0: skip copy.
  //   Cursor_ctor @ 0x0045FDF0 zeros [89,90,91]. Cursor_tick @ 0x00460140
  //   writes them from Engine_pickAt @ 0x0048D450 when (inst+0x50)&1.
  // Engine_malloc(0x1C=28) Vector3, JVM_getClass, JVM_Object_vtbl,
  // JVM_Instance_initialize(class, 0), set float x,y,z (all three).
  // Contrast getPos @ 0x00487970: query GII_POS=2, Cursor_getInfo copies
  // [41,42]=+0xA4/+0xA8 then z=[92]=+0x170; JNI sets x,y only.
  // Query 59 unnamed in GameType.java (gap 57..70). Case 44 is
  // GII_PICKEDINSTANCE (commented). Unique GII-style push 3Bh.
  // Host: GameRef_getInfo is int ABI — not this path. No Engine_pickAt.
  // cursor missing → null. Else Vector3 from inner pick_x/y/z
  // (tick analog of +0x164) else ctor 0,0,0. Always alloc. No Mighty log.
  InvObject* inner = self ? tree_field_get_obj(self, "cursor") : nullptr;
  if (!inner) return nullptr;
  const float wx = tree_field_get_float(inner, "pick_x");
  const float wy = tree_field_get_float(inner, "pick_y");
  const float wz = tree_field_get_float(inner, "pick_z");
  if (self) {
    tree_field_set_float(self, "pick_x", wx);
    tree_field_set_float(self, "pick_y", wy);
    tree_field_set_float(self, "pick_z", wz);
    tree_field_set_int(self, "pick_ok", 1);
  }
  return vec3_new(wx, wy, wz);
}

void java_io_MouseCursor_tickSysCursor() {
  // PE Cursor_tick @ 0x00460140: SysCursor NDC copy then EVENT_CURSOR edges.
  // HOVER SfxRef.play / Present pump must not re-enter (nested LCLICK).
  static bool in_tick = false;
  if (in_tick) return;
  in_tick = true;
  struct Clear {
    bool* f;
    ~Clear() { *f = false; }
  } clear{&in_tick};

  InvObject* mc = java_io_Input_cursor();
  InvObject* inner = mc ? tree_field_get_obj(mc, "cursor") : nullptr;
  if (!inner) return;

  InvObject* cfg = system_config_host();
  const int32_t sys = cfg ? tree_field_get_int(cfg, "SysCursor") : 1;
  float x = 0.f, y = 0.f;
  const bool have_ndc = input_syscursor_ndc(&x, &y);
  if (sys != 0 && have_ndc) {
    if (x > 1.f) x = 1.f;
    if (x < -1.f) x = -1.f;
    if (y > 1.f) y = 1.f;
    if (y < -1.f) y = -1.f;
    java_util_resource_GameRef_setPos(inner, vec3_new(x, y, 0.f));
    tree_field_set_float(inner, "cursor_x", x);
    tree_field_set_float(inner, "cursor_y", y);
    tree_field_set_int(inner, "cursor_set", 1);
    tree_field_set_float(mc, "cursor_x", x);
    tree_field_set_float(mc, "cursor_y", y);
    tree_field_set_int(mc, "cursor_set", 1);
  } else {
    float z = 0.f;
    vec3_get(java_util_resource_GameRef_getPos(inner), &x, &y, &z);
    (void)z;
  }

  // PE 0x0046024A: dx²+dy² vs flt_5F0ED4 (~1e-6) → moved (var_24C).
  static float prev_ndc_x = 0.f, prev_ndc_y = 0.f;
  static bool have_prev_ndc = false;
  int32_t moved = 0;
  if (have_prev_ndc) {
    const float dx = x - prev_ndc_x;
    const float dy = y - prev_ndc_y;
    if (dx * dx + dy * dy > 1e-6f) moved = 1;
  }
  prev_ndc_x = x;
  prev_ndc_y = y;
  have_prev_ndc = true;

  const int32_t cursor_id = java_util_resource_ResourceRef_id(inner);
  InvObject* phy = nullptr;
  InvObject* group = nullptr;
  float px = 0.f, py = 0.f, pz = 0.f;
  const bool picked = physics_pick_osd_gadget(x, y, &phy, &group, &px, &py, &pz);
  const int32_t phy_id =
      (picked && phy) ? java_util_resource_ResourceRef_id(phy) : 0;

  // EC_LEAVE=16 / EC_HOVER=15: "%d %d %d %d %d" @ 0x0046043F / 0x00460509.
  // token3 = phy id (+0x14C / +0x50). token4 = moved. Dest +0x13C then cursor.
  // Group.handleEvent only hilites HOVER when moved != 0.
  static InvObject* hover_phy = nullptr;
  static InvObject* hover_group = nullptr;
  static int32_t hover_phy_id = 0;
  static InvObject* hover_state = nullptr;
  InvObject* cur_state = game_logic_actual_state();
  if (cur_state != hover_state) {
    hover_phy = nullptr;
    hover_group = nullptr;
    hover_phy_id = 0;
    hover_state = cur_state;
  }
  auto osd_click_lock = [](InvObject* grp) -> bool {
    InvObject* osd = grp ? tree_field_get_obj(grp, "osd") : nullptr;
    return osd && tree_field_get_int(osd, "clickLock") != 0;
  };
  auto fire_hl = [&](int32_t code, InvObject* dest, InvObject* obj, int32_t id) {
    if (!dest || osd_click_lock(dest)) return;
    char buf[80];
    std::snprintf(buf, sizeof(buf), "%d %d %d %d %d", code, cursor_id, id, id,
                  moved);
    java_lang_GameType_dispatchCursorTo(dest, obj, string_new(buf));
  };
  if (phy != hover_phy) {
    if (hover_group && hover_phy_id) fire_hl(16, hover_group, hover_phy, hover_phy_id);
    // Java Group.handleEvent EC_HOVER: only hilite if token4 (moved) != 0.
    if (moved && picked && group && phy_id) fire_hl(15, group, phy, phy_id);
    hover_phy = picked ? phy : nullptr;
    hover_group = picked ? group : nullptr;
    hover_phy_id = phy_id;
  }

  // LDOWN(1) on MK_LBUTTON edge; LUP(2)+LCLICK(5) on release.
  // LDRAGBEGIN(9)/END(10)/LDROP(11) on L hold. R* on MK_RBUTTON.
  static uint32_t prev_mk = 0;
  static InvObject* ldown_phy = nullptr;
  static InvObject* ldown_group = nullptr;
  static int32_t ldown_phy_id = 0;
  static float ldown_px = 0.f, ldown_py = 0.f, ldown_pz = 0.f;
  static float ldown_x = 0.f, ldown_y = 0.f;
  static bool ldrag = false;
  static float rdown_x = 0.f, rdown_y = 0.f;
  static bool rdrag = false;
  static InvObject* rdrag_state = nullptr;
  const uint32_t mk = input_syscursor_buttons();
  constexpr uint32_t kMkLbutton = 1u;
  constexpr uint32_t kMkRbutton = 2u;
  const bool ldown = (mk & kMkLbutton) != 0;
  const bool lwas = (prev_mk & kMkLbutton) != 0;
  const bool rdown = (mk & kMkRbutton) != 0;
  const bool rwas = (prev_mk & kMkRbutton) != 0;
  prev_mk = mk;

  auto fire_long = [&](int32_t code) {
    char buf[160];
    // PE 0x00461239 / 0x004619F0: Mechanic/Garage addHandler dest=cursor
    std::snprintf(buf, sizeof(buf),
                  "%d %d %d %.3f %.3f %.3f %.3f %.3f %.3f", code, cursor_id, 0,
                  0.f, 0.f, 0.f, x, y, 0.f);
    java_lang_GameType_dispatchCursor(inner, string_new(buf));
  };
  auto fire_r4 = [&](int32_t code) {
    char buf[80];
    // PE 0x004615F3 / 0x00461779: "%d %d %d %d" → cursor (esi)
    std::snprintf(buf, sizeof(buf), "%d %d %d %d", code, cursor_id, 0, 0);
    java_lang_GameType_dispatchCursor(inner, string_new(buf));
  };
  auto fire_r3 = [&](int32_t code, int32_t tok2 = 0) {
    char buf[80];
    // PE 0x0046105C / 0x004612BB / 0x004616D5 / 0x00461A36: "%d %d %d"
    std::snprintf(buf, sizeof(buf), "%d %d %d", code, cursor_id, tok2);
    java_lang_GameType_dispatchCursor(inner, string_new(buf));
  };
  auto fire_r3_group = [&](int32_t code, int32_t tok2) {
    char buf[80];
    std::snprintf(buf, sizeof(buf), "%d %d %d", code, cursor_id, tok2);
    InvObject* p = string_new(buf);
    java_lang_GameType_dispatchCursor(inner, p);
    // PE +0xF4: also queue to +0xEC (Group parent).
    if (picked && group) java_lang_GameType_dispatchCursorTo(group, phy, p);
  };

  // Don't leave Garage look-axes mapped after CAS (Valocity / MainMenu).
  if (cur_state != rdrag_state) {
    if (rdrag) fire_r3(13);
    if (ldrag) fire_r3(10, ldown_phy_id);
    rdrag = false;
    ldrag = false;
    rdrag_state = cur_state;
  }

  if (rdown && !rwas) {
    rdown_x = x;
    rdown_y = y;
    rdrag = false;
    fire_r4(3);
  } else if (rdown && rwas && !rdrag) {
    // PE 0x0046165A: hypot(press+0xBC − pos+0xA4) vs [this+0x20].
    // Cursor factory ctor @ 0x00429926 zeros +0x20 → any pixel move starts drag.
    const float dx = x - rdown_x;
    const float dy = y - rdown_y;
    if (dx * dx + dy * dy > 1e-6f) {
      rdrag = true;
      fire_r3(12);
    }
  } else if (!rdown && rwas) {
    fire_r4(4);
    if (rdrag) {
      // PE 0x00461A36: RDRAGEND=13; skips RCLICK.
      fire_r3(13);
      rdrag = false;
    }
    // PE 0x004617DD: RCLICK only if +0x104 pick && !rdrag. No 3D pick yet.
  }

  if (ldown && !lwas) {
    ldown_phy = picked ? phy : nullptr;
    ldown_group = picked ? group : nullptr;
    ldown_phy_id = phy_id;
    ldown_px = px;
    ldown_py = py;
    ldown_pz = pz;
    ldown_x = x;
    ldown_y = y;
    ldrag = false;
    if (picked && group) {
      char buf[160];
      // PE 0x00460F73: "%d %d %d %d %.3f %.3f %.3f"
      // Group.handleEvent EC_LDOWN: physicsId = token(3)
      std::snprintf(buf, sizeof(buf), "%d %d %d %d %.3f %.3f %.3f", 1, cursor_id,
                    phy_id, phy_id, px, py, pz);
      java_lang_GameType_dispatchCursorTo(group, phy, string_new(buf));
    }
    fire_long(1);
    return;
  }

  if (ldown && lwas) {
    // PE loc_460FD3: hold; skip if +0xE0; hypot(press+0xB0 − pos+0xA4) vs +0x20.
    // Factory ctor zeros +0x20 → any pixel move starts drag (host 1e-6 NDC).
    if (!ldrag) {
      const float dx = x - ldown_x;
      const float dy = y - ldown_y;
      if (dx * dx + dy * dy > 1e-6f) {
        ldrag = true;
        // PE 0x0046105C EC_LDRAGBEGIN=9; dest cursor then +0xEC if +0xF4.
        fire_r3_group(9, ldown_phy_id);
      }
    }
    return;
  }

  if (!ldown && !lwas) return;

  // LUP: PE 0x004610F0 "%d %d %d %d" token3 = +0x10C (press object) id
  if (ldown_group && ldown_phy_id) {
    char buf[80];
    std::snprintf(buf, sizeof(buf), "%d %d %d %d", 2, cursor_id, ldown_phy_id,
                  ldown_phy_id);
    java_lang_GameType_dispatchCursorTo(ldown_group, ldown_phy, string_new(buf));
  }
  fire_long(2);

  if (ldrag) {
    // PE loc_4612A0: skip LCLICK; LDRAGEND=10 then LDROP=11 if +0xF4.
    fire_r3_group(10, ldown_phy_id);
    if (picked && group) {
      char sbuf[80];
      // PE 0x00461358: "%d %d %d %d %d" token2=hover phy token4=press phy.
      std::snprintf(sbuf, sizeof(sbuf), "%d %d %d %d %d", 11, cursor_id, phy_id,
                    ldown_phy_id, ldown_phy_id);
      java_lang_GameType_dispatchCursorTo(group, phy, string_new(sbuf));
      char lbuf[160];
      // PE 0x004613DC: "%d %d %d %d %.3f %.3f %.3f %d" → cursor (Garage).
      std::snprintf(lbuf, sizeof(lbuf), "%d %d %d %d %.3f %.3f %.3f %d", 11,
                    cursor_id, phy_id, ldown_phy_id, px, py, pz, phy_id);
      java_lang_GameType_dispatchCursor(inner, string_new(lbuf));
    }
    ldrag = false;
    ldown_phy = nullptr;
    ldown_group = nullptr;
    ldown_phy_id = 0;
    return;
  }

  // LCLICK short only if current pick nonzero (PE [esi+0xF4]) and not drag.
  // token(2) = +0x10C id (press target). Group.handleEvent EC_LCLICK.
  if (picked && ldown_group && ldown_phy_id) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%d %d %d %.3f %.3f %.3f", 5, cursor_id,
                  ldown_phy_id, ldown_px, ldown_py, ldown_pz);
    java_lang_GameType_dispatchCursorTo(ldown_group, ldown_phy, string_new(buf));
  }
  fire_long(5);
  ldown_phy = nullptr;
  ldown_group = nullptr;
  ldown_phy_id = 0;
}

// GameType event/timer/createNativeInstance: natives_gametype.cpp

}  // namespace inv
