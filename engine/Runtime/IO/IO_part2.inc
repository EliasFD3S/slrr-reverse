// Fragment 2/2 of IO.cpp — included, not compiled on its own.
// Split only to stay under the 128 KB editor buffer cap; the
// translation unit is unchanged. See native/tools/split_source.py.


const char* kb_axis_name(int32_t axis) {
  if (axis < 0 || axis >= 256) return "";
  if (axis < kKbAxisLo) return kKbAxisName[axis];
  switch (axis) {
    case 0x9C: return "NumEnter";
    case 0x9D: return "RCtrl";
    case 0xB5: return "Num/";
    case 0xB7: return "SysRq";
    case 0xB8: return "RAlt";
    case 0xC5: return "Pause";
    case 0xC7: return "Home";
    case 0xC8: return "Up";
    case 0xC9: return "PageUp";
    case 0xCB: return "Left";
    case 0xCD: return "Right";
    case 0xCF: return "End";
    case 0xD0: return "Down";
    case 0xD1: return "PageDown";
    case 0xD2: return "Insert";
    case 0xD3: return "Delete";
    case 0xDB: return "LWin";
    case 0xDC: return "RWin";
    case 0xDD: return "AppMenu";
    default: return "?";
  }
}

const char* kMouseAxisName[] = {
    "Mouse X",    "Mouse Y",    "Mouse Z",    "M.Button 1", "M.Button 2",
    "M.Button 3", "M.Button 4", "Mouse X",    "Mouse Y",    "Mouse Z",
};
constexpr int32_t kMouseAxisCount =
    static_cast<int32_t>(sizeof(kMouseAxisName) / sizeof(kMouseAxisName[0]));
// PE Input_dev0_axisCount @ 0x76F9E8 = 256.
constexpr int32_t kKbPhysAxisCount = 256;

float java_io_Input_getAxis(int32_t device, int32_t axis) {
  // PE @ 0x0047C990 size 0x2a (42). Static getAxis(II)F: JVM_UnboxArg
  // dest0=nullptr → sole callee Input_readPhysicalAxis @ 0x00557430
  // (device, axis). Physical only — NOT Input_readLogicalAxis @ 0x54D830
  // (Controller.user_GetAxisVal / mapAxis remap + smooth).
  // Device table Input_diDevices @ 0x76F9B0 stride 490 dwords; type at
  // dword[+12] (1=key 2=mouse 3=joy); naxes at dword[+14].
  // No Input_pollDevices / Cursor_tick inside getAxis — MainLoop
  // Input_tick @ 0x54DDA0 samples devices. Host MUST NOT call
  // input_live_poll() here: that re-enters tickSysCursor (historical race
  // from physics_drive / Valocity simulate → abort).
  int32_t type = 0;
  int32_t naxes = 0;
  if (device == 0) {
    type = 1;
    naxes = kKbPhysAxisCount;
  } else if (device == 1) {
    type = 2;
    naxes = kMouseAxisCount;
  }
  // Type 3 (joystick) Soft OOS — PE returns 0 if type not 1/2/3.
  if (type == 0) return 0.f;

  std::lock_guard<std::recursive_mutex> lock(g_mu);

  // Mouse branch @ 0x55776E: (unsigned)axis > 0xFFFFFF9A (-102).
  // Input_mouseSensScale @ 0x777438 = (-1-axis)*0.01; zero float
  // accum at slot+17 for naxes; return 0.0. Keyboard type1 never
  // takes this path (negative axis fails a2>=0 → 0.0).
  if (type == 2 && static_cast<uint32_t>(axis) > 0xFFFFFF9Au) {
    g_mouse_sens_scale = static_cast<float>(-1 - axis) * 0.01f;
    auto dit = g_axes.find(device);
    if (dit != g_axes.end()) {
      for (auto& kv : dit->second) kv.second = 0.f;
    }
    // Soft bridge: mirror onto Input.cursor MouseCursor for host probes
    // (PE global only; Java MouseCursor.config queues "sens").
    if (g_input_cursor) {
      tree_field_set_float(g_input_cursor, "mouse_sensitivity",
                           g_mouse_sens_scale);
    }
    return 0.f;
  }

  if (axis < 0 || axis >= naxes) return 0.f;
  float v = 0.f;
  auto dit = g_axes.find(device);
  if (dit != g_axes.end()) {
    auto ait = dit->second.find(axis);
    if (ait != dit->second.end()) v = ait->second;
  }
  if (type == 1) {
    // Keyboard: DI buffer signed char < 0 → 1.0 else 0.0.
    return v > 0.f ? 1.f : 0.f;
  }
  // Mouse: button phys ids (initDevices 3..6) digital; analog clamp [-1,1].
  if (axis >= 3 && axis <= 6) return v > 0.f ? 1.f : 0.f;
  if (v > 1.f) return 1.f;
  if (v < -1.f) return -1.f;
  return v;
}

InvObject* java_io_Input_getDeviceName(int32_t i) {
  // PE @ 0x0047C9C0 size 0x29 (41, int_convert). Static native:
  // JVM_UnboxArg @ 0x0045D910 (dest0=nullptr) unboxes int i.
  // Input_getDeviceName @ 0x0054DE20 → Input_DiDevice_getName @ 0x00558100:
  // if i<0 || i>=Input_diDeviceCount @ 0x777434 return nullptr; else
  // &Input_diDeviceName @ 0x770104 + 1960*i (0x7A8; device stride 490
  // dwords; name at dword+469 / +1876, 64 bytes via sub_5D6600).
  // initDevices @ 0x00556150 sets "SysKeyboard"/"SysMouse" for 0/1.
  // JVM_String_from_cstr @ 0x004174A0: null cstr → null String; empty
  // buffer → empty String (not null). Soft: g_input_di_device_count.
  if (i < 0 || i >= g_input_di_device_count) return nullptr;
  const char* cstr = kInputDeviceName[i];
  if (!cstr) return nullptr;
  return string_new(cstr);
}

void java_io_Input_mapAxis(InvObject* inst, int32_t l_axis, int32_t device,
                           int32_t i_axis, float i_from, float i_to, float l_from,
                           float l_to) {
  // PE @ 0x0047C9F0 size 0x7B. Static native: JVM_UnboxArg @ 0x0045D910
  // (dest0=nullptr) unboxes GameRef inst + l_axis, device, i_axis, i_from,
  // i_to, l_from, l_to. thiscall sub_426470(g_EngineState @ 0x00636338,
  // channel 27, 0) resolves Controller Input* map table from GameRef inst
  // (same tail as setAxisSmooth @ 0x0047CA70). Contrast Controller.user_Add
  // @ 0x00477740: Unbox this → Native.ptr (dword_62E008) → *(h+0xC) gate
  // (v2[19]!=1 → vtbl+0x14(1.0f); sub_5447D0>=0 → vtbl+0xC(1.0f)) then
  // Input_mapAxis_add on *(result+76)+0x1C. Both call Input_mapAxis_add
  // @ 0x0054D650 (first free of 152; analogFlag=Input_physAxisIsAnalog).
  // Return void — slot code from add is discarded.
  input_map_add(inst, l_axis, device, i_axis, i_from, i_to, l_from, l_to);
}
void java_io_Input_setAxisSmooth(InvObject* inst, int32_t l_axis, float a,
                                 float b, float c, float d) {
  // PE @ 0x0047CA70 size 0x6c (108). STATIC (GameRef;IFFFF)V.
  // JVM_UnboxArg dest0=nullptr; dest1=GameRef; dest2..5=l_axis+a,b,c,d
  // (center_range, factor_center, factor_opposite, factor_same).
  // Engine_queryGameRefChannel @ 0x00426470(g_EngineState @ 0x00636338,
  // GameRef, channel 27, 0) → Input* map.
  // thiscall Input_setLogicalAxisSmooth @ 0x0054D7B0
  // (map, l_axis, a,b,c,d, power=1.0f=0x3F800000):
  //   slot = map + 48*l_axis + 4; store +3..+6,+8 (VirtualAxisSmooth).
  // Contrast Controller.user_SetAxisSmooth @ 0x004778B0 (power=e).
  // VOID — EAX discarded. No poll / no Cursor_tick.
  // Host: input_map_set_smooth(..., power=1.f).
  input_map_set_smooth(inst, l_axis, a, b, c, d, 1.f);
}

// ---- Controller (physical→logical maps; same table as Input.mapAxis) ----
// PE gate (Add/Del/Reset/SetAxisForce @ 0x00477740..0x00477B20): UnboxArg →
// Native.ptr (dword_62E008) → *(h+0xC); if v2[19]!=1 vtbl+0x14(1.0f);
// sub_5447D0(v2, 0xA0000000, 0, 0)>=0 → vtbl+0xC(1.0f) → Input*+0x1C.

int32_t java_io_Controller_user_Add(InvObject* self, int32_t vaxis,
                                    int32_t device, int32_t paxis, float a,
                                    float b, float c, float d) {
  // PE @ 0x00477740: UnboxArg(this,vaxis,device,paxis,a,b,c,d) →
  // Native.ptr (dword_62E008) → *(h+0xC); if v2[0x4C]!=1 vtbl+0x14(1.0f);
  // sub_5447D0(v2,0xA0000000,0,0) fail / vtbl+0xC(1.0f)==0 → ret 0;
  // else Input_mapAxis_add(*(res+0x4C)+0x1C, ...) @ 0x0054D650 (−1/0/slot+1).
  return input_map_add_pe(self, vaxis, device, paxis, a, b, c, d);
}

int32_t java_io_Controller_user_Del(InvObject* self, int32_t vaxis,
                                    int32_t device, int32_t paxis) {
  // PE @ 0x00477810 size 0x9A (154). Unbox this+I×3 (JVM_UnboxArg @ 0x0045D910).
  // handle = JVM_vm_get_int_field(this, dword_62E008) — Native.ptr.
  // handle==0 || inner=*(handle+0xC)==0 → loc_4778A4 xor eax,eax.
  // [inner+0x4C]!=1 (INSTANCE_GAME) → vtbl+0x14(1.0f=0x3F800000).
  // sub_5447D0(inner, 0xA0000000, 0.0, 0.0); eax&0x80000000 → skip del.
  // mid = vtbl+0xC(inner, 1.0f); mid==0 → skip del.
  // Input_mapAxis_del(*(mid+0x4C)+0x1C, vaxis, device, paxis) @ 0x0054D700:
  // first matching slot only (152 cap, stride 8 dwords, zero vaxis, --count).
  // Always return 0 (xor eax,eax) — contrast user_Add @ 0x00477740 (slot code).
  // Host stand-in: self as Controller inst (no Native.ptr / sub_5447D0 / vtbl
  // gate — same gap as user_Add). input_map_del_pe mirrors first-match del.
  // race123–125 deepen: Input_mapAxis_del; always ret 0; first-match only.
  if (!self) return 0;
  (void)input_map_del_pe(self, vaxis, device, paxis);
  return 0;
}

int32_t java_io_Controller_user_SetAxisSmooth(InvObject* self, int32_t vaxis,
                                              float a, float b, float c,
                                              float d, float e) {
  // PE @ 0x004778B0 size 0xbc (188). Unbox this+I+FFFFF. Same gate as
  // user_Add @ 0x00477740: Native.ptr → *(h+0xC); [inner+0x4C]!=1 →
  // vtbl+0x14(1.0f); sub_5447D0(0xA0000000) fail / vtbl+0xC==0 → skip.
  // Else thiscall Input_setLogicalAxisSmooth @ 0x0054D7B0
  // (*(mid+0x4C)+0x1C, vaxis, a,b,c,d,e) — slot+3..+6,+8 (power=e).
  // Always return 1. Contrast Input.setAxisSmooth @ 0x0047CA70 (power=1).
  // Host stand-in: self as Controller (no Native.ptr gate).
  // race123–125 deepen: Input_setLogicalAxisSmooth; always 1; power=e.
  input_map_set_smooth(self, vaxis, a, b, c, d, e);
  return 1;
}

void java_io_Controller_user_SetAxisSpeed(InvObject* self, int32_t vaxis,
                                          float a) {
  // PE @ 0x00477970 size 0x8f (143). Unbox this+I+F
  // (JVM_UnboxArg @ 0x0045D910). handle=JVM_vm_get_int_field(this,
  // dword_62E008); handle==0 || inner=*(handle+0xC)==0 → ret.
  // [inner+0x4C]!=1 → vtbl+0x14(1.0f). sub_5447D0(inner,
  // 0xA0000000,0,0) eax&0x80000000 → ret. mid=vtbl+0xC(inner,1.0f);
  // mid==0 → ret. thiscall Input_setLogicalAxisSpeed @ 0x0054D810
  // (*(mid+0x4C)+0x1C, vaxis, speed): store float at map+48*vaxis+0x20
  // — no clamp. Same gate family as user_Add @ 0x00477740 /
  // user_GetAxisVal @ 0x00477A80. Host stand-in: self as Controller
  // (no Native.ptr / sub_5447D0 / vtbl gate); input_map_set_speed.
  // race123–125 deepen: store @ map+48*vaxis+0x20 raw; void discard EAX.
  if (!self) return;
  input_map_set_speed(self, vaxis, a);
}

float java_io_Controller_user_GetAxisVal(InvObject* self, int32_t vaxis) {
  // PE @ 0x00477A80 size 0x95 (149). Unbox this+I; stack float zeroed.
  // handle=JVM_vm_get_int_field(this, dword_62E008);
  // handle==0 || inner=*(handle+0xC)==0 → fld 0. [inner+0x4C]!=1 →
  // vtbl+0x14(1.0f). sub_5447D0(inner,0xA0000000,0,0) eax&0x80000000
  // → fld 0. mid=vtbl+0xC(inner,1.0f); mid==0 → fld 0. Else thiscall
  // Input_readLogicalAxis @ 0x0054D830(*(mid+0x4C)+0x1C, vaxis) —
  // 152-map remap + smooth (does NOT pump Cursor_tick). Same gate as
  // user_SetAxisSpeed @ 0x00477970 / user_Add. Host stand-in: self as
  // Controller; input_map_get_logical (no live_poll — re-entrancy).
  // race123–125 deepen: gate fail → 0.0; no Cursor_tick; host logical.
  if (!self) return 0.f;
  return input_map_get_logical(self, vaxis);
}

void java_io_Controller_user_SetAxisForce(InvObject* self, int32_t vaxis,
                                          float c, float f) {
  // PE @ 0x00477B20 size 0x9e (158). Unbox this+I+FF. Same gate family
  // as user_Add @ 0x00477740 / user_Reset @ 0x00477A00. Then thiscall
  // Input_setLogicalAxisForce @ 0x0054D7F0(*(mid+0x4C)+0x1C, vaxis, c, f):
  // store c @ map+48*vaxis+4+36, f @ +40 (raw floats, no epsilon clear).
  // VOID (EAX=1 discarded). Host stand-in: f overrides logical; |f|<ε
  // clears (smoke SetAxisForce(...,0) — PE keeps zeros in slots).
  // race123–125 deepen: PE stores c@+36 f@+40 raw; host ε-clear for smoke.
  (void)c;
  if (!self) return;
  if (f > -1e-6f && f < 1e-6f)
    input_map_clear_force(self, vaxis);
  else
    input_map_set_force(self, vaxis, f);
}

void java_io_Controller_user_Reset(InvObject* self) {
  // PE @ 0x00477A00 size 0x7a (122). Unbox this. Same Native.ptr /
  // INSTANCE_GAME / sub_5447D0 / vtbl+0xC gate as user_Add @ 0x00477740.
  // Then Input_mapAxis_reset @ 0x0054D750(*(mid+0x4C)+0x1C) — full wipe
  // of 152 maps + 76 vaxis props (not a Del-all loop). EAX=1 discarded
  // (Java void). Contrast Add→mapAxis_add / Del→mapAxis_del.
  // Host stand-in: input_map_reset(self).
  // race123–125 deepen: Input_mapAxis_reset (152×8-dword + 76 props);
  // EAX=1 discarded; host input_map_reset.
  input_map_reset(self);
}

int32_t java_io_Input_activeAxis(int32_t device) {
  // PE @ 0x0047CAE0 size 0x1f (31). STATIC (I)I. UnboxArg dest0=nullptr
  // → device (overwrites CallInfo). Sole callee Input_activeAxis @
  // 0x00557AA0 (size 0x565). device∉[0,Input_deviceCount@0x777434) →
  // -1. Walk axes of device slot dword_76F9B0+490*device; skip if
  // type dword[+0x144+4*i] bit15 set. Type 1/2/3 sample → float;
  // hit bands (get_bytes): flt_5F0C80@0x5F0C80=0.25, flt_5F3AD0@
  // 0x5F3AD0=-0.25, flt_5F0C60@0x5F0C60=-0.75 — v>=0.25 OR
  // v∈[-0.75,-0.25] OR (v<=-0.75 && prev[+0x44+4*i]>=-0.25); store
  // prev; count hits; return index iff count==1 else -1.
  // No Input_pollDevices inside activeAxis (same as getAxis) — do NOT
  // call input_live_poll() (tickSysCursor re-entrancy race).
  if (device < 0 || device >= g_input_di_device_count) return -1;
  const int32_t naxes = (device == 0) ? 256 : kMouseAxisCount;
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  auto& cur = g_axes[device];
  auto& prev = g_axis_prev[device];
  int32_t found = -1;
  int32_t n = 0;
  for (int32_t i = 0; i < naxes; ++i) {
    float v = 0.f;
    auto it = cur.find(i);
    if (it != cur.end()) v = it->second;
    float old = 0.f;
    auto pit = prev.find(i);
    if (pit != prev.end()) old = pit->second;
    bool hit = false;
    if (v >= 0.25f)
      hit = true;
    else if (v <= -0.25f && v >= -0.75f)
      hit = true;
    else if (v <= -0.75f && old >= -0.25f)
      hit = true;
    if (hit) {
      found = i;
      ++n;
    }
    prev[i] = v;
  }
  return n == 1 ? found : -1;
}

InvObject* java_io_Input_axisName(int32_t device, int32_t axis) {
  // PE @ 0x0047CB00 size 0x4b (75). Static axisName(II)String: UnboxArg
  // dest0=nullptr. Input_physAxisName @ 0x005579F0(device,axis,buf256):
  // device∉[0,Input_deviceCount) or axis∉[0,naxes) → buf[0]=0, ret -1;
  // type1 → strcpy(off_61AC44[axis]); type2/3 → device name table
  // +4*axis+145. Then JVM_String_from_cstr(buf) — empty buf → empty
  // String (not null; null only if cstr ptr null).
  // PE @ 0x0047CB00
  const char* s = "";
  if (device == 0)
    s = kb_axis_name(axis);
  else if (device == 1 && axis >= 0 && axis < kMouseAxisCount)
    s = kMouseAxisName[axis];
  return string_new(s);
}

int32_t java_io_Input_lastKey() {
  // PE @ 0x0047CC10 → Input_lastKeyEvent @ 0x00556E00: scan | (ascii<<16).
  // If (result & 0xFF0000): BYTE2 → 16-byte Input_cheatRing (wrap @ end).
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  int32_t result = input_last_key_event();
  if (result == 0 && !g_key_queue.empty()) {
    // Host smoke injection (input_set_last_key edges); not stock.
    const int32_t dik = g_key_queue.front() & 0xFF;
    g_key_queue.pop_front();
    const char ascii = dik_to_lower(dik);
    result = dik | (static_cast<int32_t>(static_cast<unsigned char>(ascii)) << 16);
  }
  if ((result & 0xFF0000) != 0)
    cheat_append_ascii(static_cast<char>((result >> 16) & 0xFF));
  return result;
}

namespace {

constexpr int32_t kHkVirtual = 1;
constexpr int32_t kHkKey = 2;
constexpr int32_t kEventKeyPress = 0x1;
constexpr int32_t kEventKeyRelease = 0x2;
// PE Input_hotkeyTable @ 0x0063C890: 512 slots × 32 bytes (Input_hotkeyTableClear).
constexpr int32_t kHotkeyCap = 512;
// PE checkHotkeys @ 0x0047CD40: down if LogicalAxis > 0.2 (dbl_5F0900).
constexpr float kHotkeyDown = 0.2f;

struct HotkeyReg {
  InvObject* hk = nullptr;
  int32_t axis = 0;
  int32_t flags = 0;
  int32_t event_filter = kEventKeyPress;
  InvObject* handler = nullptr;
  InvObject* owner = nullptr;
  int32_t prev_down = 0;
  int32_t armed = 0;  // PE +28; 0 after create/flush → sample only
};

std::vector<HotkeyReg> g_hotkeys;

// PE Engine_queueEvent @ 0x00426800 → Engine_queueEvent_dispatch @ 0x004265C0
// (eventMask +0x70 & type) → Engine_dispatchScriptEvent @ 0x00425C60.
// EVENT_HOTKEY 0x00100000 → handleEvent(Hotkey), not osdCommand.
constexpr int32_t kEventHotkey = 0x00100000;

bool class_has_handleEvent_hotkey(Jvm* j, const char* cn) {
  if (!j || !cn || !cn[0]) return false;
  const JvmClass* cls = j->find_class(cn);
  if (!cls) return false;
  for (const JvmMethod& m : cls->methods) {
    if (m.name == "handleEvent" &&
        m.signature.find("Hotkey") != std::string::npos)
      return true;
  }
  return false;
}

void hotkey_fire(InvObject* hk, InvObject* native_handler, int32_t cmd) {
  if (!native_handler) return;
  // PE: skip if !(eventMask & EVENT_HOTKEY).
  const int32_t mask = tree_field_get_int(native_handler, "event_mask");
  if ((mask & kEventHotkey) == 0) return;
  tree_field_set_int(native_handler, "last_event", kEventHotkey);

  // Osd.handleEvent else-branch telemetry: hk.handler.osdCommand(cmd).
  InvObject* dest = hk ? tree_field_get_obj(hk, "handler") : nullptr;
  if (!dest) dest = native_handler;
  tree_field_set_int(dest, "last_osd_cmd", cmd);
  tree_field_set_int(dest, "osd_cmd_count",
                     tree_field_get_int(dest, "osd_cmd_count") + 1);

  Jvm* j = jvm_active();
  const char* cn = tree_host_class(native_handler);
  if (j && cn && cn[0] && class_has_handleEvent_hotkey(j, cn)) {
    std::vector<JvmValue> args = {JvmValue::make_obj(native_handler),
                                  JvmValue::make_obj(hk)};
    j->invoke(cn, "handleEvent", "(Ljava.io.Hotkey;)V", args, false);
  }
}

float hotkey_axis_value(InvObject* inst, int32_t axis, int32_t flags) {
  if (flags & kHkVirtual) {
    return inst ? input_map_get_logical(inst, axis) : 0.f;
  }
  if (flags & kHkKey) {
    return java_io_Input_getAxis(0, axis);
  }
  // Default: treat as virtual logical axis.
  return inst ? input_map_get_logical(inst, axis) : 0.f;
}

}  // namespace

void java_io_Input_createHotkey(int32_t axis, int32_t flags, InvObject* hk,
                                InvObject* handler, InvObject* owner,
                                int32_t eventFilter) {
  // PE @ 0x0047CC50 size 0xa3 (163). STATIC
  // (IILjava.io.Hotkey;Ljava.util.resource.GameRef;Ljava.render.Osd;I)V.
  // UnboxArg dest0=nullptr; dest1=&var_14 axis; dest2=&arg_0 flags;
  // dest3=&var_10 hk; dest4=&var_C handler; dest5=&var_8 owner;
  // dest6=&var_4 eventFilter. Walk Input_hotkeyTable @ 0x63C890
  // (512×0x20) to first slot[+0]==0; full → ret. Else fill:
  // +0 flags (occupied), +4 axis, +8 Hotkey*, +12 owner Osd,
  // +16 prev_down=0, +20 handler GameRef, +24 eventFilter, +28 armed=0.
  // Java Input.VIRTUAL=1 / KEY bit2. No Hotkey field writes in PE.
  // Host: g_hotkeys side table + mirror key/flags/osd on hk for smoke.
  // PE @ 0x0047CC50
  if (!hk) return;
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  tree_field_set_int(hk, "key", axis);
  tree_field_set_int(hk, "flags", flags);
  if (eventFilter != 0) tree_field_set_int(hk, "eventFilter", eventFilter);
  if (owner) tree_field_set_obj(hk, "osd", owner);
  tree_field_set_int(hk, "active", 1);
  tree_field_set_int(hk, "state", 0);

  for (HotkeyReg& r : g_hotkeys) {
    if (r.hk == hk) {
      r.axis = axis;
      r.flags = flags;
      r.event_filter = eventFilter ? eventFilter : kEventKeyPress;
      r.handler = handler;
      r.owner = owner;
      r.prev_down = 0;
      r.armed = 0;
      return;
    }
  }
  if (static_cast<int32_t>(g_hotkeys.size()) >= kHotkeyCap) return;
  HotkeyReg r;
  r.hk = hk;
  r.axis = axis;
  r.flags = flags;
  r.event_filter = eventFilter ? eventFilter : kEventKeyPress;
  r.handler = handler;
  r.owner = owner;
  r.prev_down = 0;
  r.armed = 0;
  g_hotkeys.push_back(r);
}

void java_io_Input_deleteHotkey(InvObject* hk) {
  // PE @ 0x0047CD00 size 0x3d (61). Static deleteHotkey(Hotkey)V:
  // JVM_UnboxArg dest0=nullptr. Walk Input_hotkeyTable @ 0x0063C890
  // (512×32 B) matching Hotkey* at slot+8 (dword_63C898); on hit zero
  // slot[0] (dword_63C890[8*i]=0) — no Hotkey field writes. Miss → ret.
  // Host: erase g_hotkeys entry; active=0 for smoke.
  // PE @ 0x0047CD00
  if (!hk) return;
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  g_hotkeys.erase(
      std::remove_if(g_hotkeys.begin(), g_hotkeys.end(),
                     [hk](const HotkeyReg& r) { return r.hk == hk; }),
      g_hotkeys.end());
  tree_field_set_int(hk, "active", 0);
}

void java_io_Input_checkHotkeys(InvObject* inst, InvObject* infocus) {
  // PE @ 0x0047CD40 size 0x13e (318). STATIC (GameRef,Osd)V. UnboxArg
  // dest0=nullptr → inst, infocus. Engine_queryGameRefChannel(inst,27,0)
  // → Controller mid for Input_readLogicalAxis. Walk 512 slots @
  // 0x63C890: skip +0==0; owner(+12)==0 || owner==infocus; sample:
  // flags&1 → readLogicalAxis(mid, axis@+4); flags&2 → key sample
  // sub_556EC0(0,0,axis); down = sample > 0.2 (dbl_5F0900). armed@+28
  //==0 → set 1 (arm only); else edge vs prev@+16 & eventFilter@+24
  // bits F_KEY_PRESS/RELEASE → count; first edge index kept. Always
  // store prev@+16; JVM setIntField(hk,"state",down). Tail: if any
  // edge → Engine_queueEvent(handler@+20, 0, EVENT_HOTKEY 0x100000,
  // hk@+8, 0). Host: g_hotkeys + hotkey_fire stand-in.
  // race119–121: Frontend polls checkHotkeys each frame — stand-in for
  // Engine_MainLoop Input_tick adjacency before Sfx_ListenerSetPose
  // @ 0x00428CB6 + Sfx_UpdateVoices @ 0x00428CBE + PollWindowQuit
  // @ 0x00428F5F (system_mainloop_sfx_listener_update).
  // PE @ 0x0047CD40
  system_mainloop_sfx_listener_update();
  struct PendingFire {
    InvObject* hk = nullptr;
    InvObject* handler = nullptr;
    int32_t cmd = 0;
  };
  PendingFire fire{};
  {
    std::lock_guard<std::recursive_mutex> lock(g_mu);
    bool fired = false;
    for (HotkeyReg& r : g_hotkeys) {
      if (!r.hk) continue;
      if (tree_field_get_int(r.hk, "active") == 0) continue;
      // PE: owner==0 (global) || owner==infocus. Null infocus skips OSD keys.
      if (r.owner && r.owner != infocus) continue;

      const int32_t axis = tree_field_get_int(r.hk, "key");
      const int32_t flags = tree_field_get_int(r.hk, "flags");
      int32_t ef = tree_field_get_int(r.hk, "eventFilter");
      if (ef == 0) ef = r.event_filter;
      const float v = hotkey_axis_value(inst, axis ? axis : r.axis,
                                        flags ? flags : r.flags);
      const int32_t down = (v > kHotkeyDown) ? 1 : 0;
      if (r.armed == 0) {
        r.armed = 1;
      } else if (down != r.prev_down && !fired) {
        const int32_t want = down ? kEventKeyPress : kEventKeyRelease;
        if (ef & want) {
          fire.hk = r.hk;
          fire.handler = r.handler;
          fire.cmd = tree_field_get_int(r.hk, "command");
          fired = true;
        }
      }
      r.prev_down = down;
      tree_field_set_int(r.hk, "state", down);
    }
  }
  if (fire.hk || fire.handler) hotkey_fire(fire.hk, fire.handler, fire.cmd);
}

void java_io_Input_flushHotkeys() {
  // PE @ 0x0047CE80 size 0x1d (29). Static ()V: no JVM_UnboxArg; no callees.
  // Walk Input_hotkeyArmed[0..512) @ +28, stride 0x20: if slot.flags(+0)!=0 → armed=0.
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  for (HotkeyReg& r : g_hotkeys) {
    if (r.flags != 0) r.armed = 0;
  }
}

InvObject* java_io_Input_cursor() {
  // Soft stand-in for Java Input.cursor() accessor (not a PE native —
  // registry has MouseCursor.getPos/getPickedPos only @ 0x487970/0x487870).
  // Returns the singleton MouseCursor GameType; ctor allocates inner
  // GameRef RID_CURSOR. tickSysCursor / enable queueEvent dest = inner.
  if (!g_input_cursor) {
    g_input_cursor = tree_host_new("java.io.MouseCursor");
    tree_field_set_int(g_input_cursor, "visible", 0);
    tree_field_set_int(g_input_cursor, "enabled", 0);
    tree_field_set_float(g_input_cursor, "mouse_sensitivity",
                         g_mouse_sens_scale);
  }
  // Java MouseCursor ctor: cursor = new GameRef(..., RID_CURSOR, ...).
  // tickSysCursor / addHandler dest is this inner GameRef.
  if (!tree_field_get_obj(g_input_cursor, "cursor")) {
    InvObject* inner = gameref_new();
    tree_field_set_obj(g_input_cursor, "cursor", inner);
    tree_field_set_obj(inner, "cursor_owner", g_input_cursor);
  }
  return g_input_cursor;
}

int32_t java_io_MouseCursor_enable(InvObject* self, int32_t state) {
  // Soft stand-in for Java MouseCursor.enable(I)I (sources MouseCursor.java
  // — NOT a PE native). Stock: prev=visible; if (visible^state) then
  // queueEvent EVENT_COMMAND "enable"/"disable" on inner cursor GameRef,
  // vp activate/deactivate, visible=state; return prev.
  // Host: xor toggle + Win32 ShowCursor; soft-queue enable/disable on
  // inner GameRef (vp/camera OOS). getPos/getPickedPos live in Cars.cpp.
  if (!self) return 0;
  const int32_t prev = tree_field_get_int(self, "visible");
  const int32_t on = state ? 1 : 0;
  if ((prev ^ on) != 0) {
    tree_field_set_int(self, "visible", on);
    tree_field_set_int(self, "enabled", on);
    InvObject* inner = tree_field_get_obj(self, "cursor");
    if (inner) {
      // EVENT_COMMAND = 0x10 — Java enable() path.
      java_util_resource_GameRef_queueEvent(
          inner, nullptr, 0x10,
          string_new(on ? "enable" : "disable"));
    }
    render_d3d9_set_cursor_visible(on);
  }
  return prev;
}

// ---- Thread ----

void java_lang_Thread_init(InvObject* self, InvObject* name) {
  // PE @ 0x0047C510 size 0x59 (89). Unbox this+String (JVM_UnboxArg @
  // 0x0045D910): dest0=this, dest1=name cstr. Engine_malloc(56 / 0x38) →
  // VMThread_init @ 0x0041F340 (thiscall on blob):
  //   JVM*=*CallInfo, priority=0 (NORM), flags=2 (arm +0x2C bit1), name.
  // *(handle+0x18)=this Java back-ref; JVM_vm_set_int_field(this,
  // dword_62E008, handle) stores Native.ptr. VMThread_init links handle into
  // cooperative scheduler dllist (host: vm_sched_link) — no CreateThread.
  // start @ 0x0047C570 clears bit1 then queues target.run (race109).
  // malloc-fail path zeros EAX then still writes [eax+18h] (latent PE crash;
  // not mirrored). xref: Natives_RegisterAll @ 0x004883F8.
  // PATH-TO-WORLD Soft: vmthread_init(prio=0, flags=2) + TREE ptr tag;
  // Runnable.run still OS-detaches on start (blocking LoadingScreen).
  if (!self) return;
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  ThreadState& st = g_threads[self];
  thread_drop_vm(&st, self);
  st.name = string_cstr(name);
  st.priority = 0;  // PE VMThread_init a3=0 — NORM_PRIORITY
  st.daemon = 0;
  st.alive = false;
  st.suspended = false;
  VmThread* thr =
      vmthread_init(jvm_active(), /*priority=*/0, /*sync_flags=*/2,
                    st.name.empty() ? nullptr : st.name.c_str());
  if (thr) {
    thr->pe.java_backref =
        static_cast<uint32_t>(reinterpret_cast<std::uintptr_t>(self));
    st.thr = thr;
    tree_field_set_int(self, "ptr", thread_ptr_tag(thr));
  }
}

void java_lang_Thread_start(InvObject* self) {
  // PE @ 0x0047C570 size 0x90 — Thread.start()V.
  // Unbox this; Native.ptr via dword_62E008. ptr==0 → ret.
  // Bytes @ 0x47C59A: mov eax,[esi+2Ch]; or ecx,2; jz (dead — or|2 never
  // ZF); and eax,~2; store — clear armed bit1 from VMThread_init a4=2.
  // push 0; thiscall VMThread_pushCallFrame(ecx=handle, a2=null) @ 0x41F9F0.
  // get_instance_field("target","java.lang.Runnable");
  // Object_callMethod(ecx=target, push handle, push "run" @ off_612D18) →
  // Thread_callMethod @ 0x4207C0. Nonzero → Thread_requestStop +
  // [handle+0x18]=0 + set_int_field(ptr,0). Cooperative — NO CreateThread.
  // PATH-TO-WORLD Soft: ensure VmThread + clear bit1 + pushCallFrame(empty);
  // do NOT green-queue run() (pending+PumpFrame would block MainLoop on
  // Object.wait). Detached std::thread invokes target.run()V — LoadingScreen /
  // HotkeyWatcher / GameType.enterAsyncMode_Script path.
  if (!self) return;
  InvObject* target = tree_field_get_obj(self, "target");
  // Soft: TREE may omit Thread(Runnable) target=this; PE ctor always sets it.
  if (!target) target = self;

  {
    std::lock_guard<std::recursive_mutex> lock(g_mu);
    ThreadState& st = g_threads[self];
    if (st.alive) return;
    VmThread* thr = thread_ensure_vm(&st, self);
    if (thr) {
      // PE @ 0x47C5A4: flags &= ~2 (armed clear).
      thr->pe.flags &= ~2;
      thr->pack_vec.clear();
      // PE push 0 → a2=null → pushCallFrame still boxes count=0 onto operand.
      vmthread_push_call_frame(thr);
    }
    st.alive = true;
    st.suspended = false;
    if (thr) thr->pe.flags &= ~0x20;  // Soft: clear suspend bit on (re)start
  }

  // Detached OS thread — stock stop() is cooperative via STOP bit / alive.
  std::thread([self, target]() {
    tree_field_set_int(self, "engine_run_entered", 1);
    Jvm* j = jvm_active();
    if (j && target) {
      const char* hc = tree_host_class(target);
      if (!hc || !hc[0]) hc = "java.lang.Thread";
      for (;;) {
        bool sus = false;
        bool alive = false;
        {
          std::lock_guard<std::recursive_mutex> lock(g_mu);
          auto it = g_threads.find(self);
          if (it == g_threads.end()) break;
          alive = it->second.alive;
          sus = it->second.suspended;
          if (it->second.thr &&
              (it->second.thr->pe.flags & 0x20) != 0) {
            sus = true;
          }
        }
        if (!alive) break;
        if (!sus) break;
        java_lang_Thread_sleep(10.f);
      }
      {
        std::lock_guard<std::recursive_mutex> lock(g_mu);
        auto it = g_threads.find(self);
        if (it == g_threads.end() || !it->second.alive) {
          tree_field_set_int(self, "engine_run_done", 1);
          return;
        }
      }
      // Soft ≡ Object_callMethod(target, handle, "run") → TREE run()V.
      j->invoke(hc, "run", "()V", {JvmValue::make_obj(target)}, false);
    }
    tree_field_set_int(self, "engine_run_done", 1);
    std::lock_guard<std::recursive_mutex> lock(g_mu);
    auto it = g_threads.find(self);
    if (it != g_threads.end()) {
      it->second.alive = false;
      // Soft: mark DONE on VM blob (PE op16 |=0x40); leave for stop/dtor.
      if (it->second.thr) it->second.thr->pe.flags |= kVmThreadFlagDone;
    }
  }).detach();
}

void java_lang_Thread_stop(InvObject* self) {
  // PE @ 0x0047C600 size 0x50 (80). Unbox this. handle=JVM_vm_get_int_field
  // (this, dword_62E008); handle==0 → ret. Else Thread_requestStop
  // @ 0x0041F780: if flags+0x2C & 0x10 → Object_MonitorDequeue; then
  // flags |= 0x80 (STOP). Clear *(handle+0x18)=0; JVM_vm_set_int_field
  // (this, dword_62E008, 0) — destroys Native.ptr. Contrast suspend
  // @ 0x0047C680 (bit 0x20 + suspend queue, keeps handle).
  // PATH-TO-WORLD Soft: alive=false + vmthread_request_stop + destroy +
  // TREE ptr=0 (PE destroys handle; host unlinks green-list).
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  auto it = g_threads.find(self);
  if (it == g_threads.end()) return;
  it->second.alive = false;
  it->second.suspended = false;
  thread_drop_vm(&it->second, self);
}

int32_t java_lang_Thread_isAlive(InvObject* self) {
  // PE @ 0x0047C730 size 0x26 (38). Unbox this (JVM_UnboxArg @ 0x0045D910).
  // JVM_vm_get_int_field(this, dword_62E008) — Native.ptr; EAX discarded.
  // Then xor eax,eax / retn — stock ALWAYS returns 0 (no alive bit read).
  // Host stand-in: g_threads.alive set by start/cleared by stop+run end
  // (Phase 2.117 LoadingScreen / HotkeyWatcher waits need a real flag).
  // race123–125 deepen: PE always 0; host keeps alive≠PE (smoke waits).
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  auto it = g_threads.find(self);
  return (it != g_threads.end() && it->second.alive) ? 1 : 0;
}

int32_t java_lang_Thread_getPriority(InvObject* self) {
  // PE @ 0x0047C760 size 0x2e (46). Unbox this (JVM_UnboxArg @ 0x0045D910).
  // handle = JVM_vm_get_int_field(this, dword_62E008) — Native.ptr.
  // handle==0 → xor eax,eax return 0. Else return *(DWORD*)(handle+0x28).
  // Contrast Thread.init @ 0x0047C510: Engine_malloc(56) → VMThread_init
  // @ 0x0041F340 seeds *(handle+0x28)=0 NORM. setPriority @ 0x0047C790 stores.
  // PATH-TO-WORLD Soft: thr→pe.priority (+0x28); missing thr → 0.
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  auto it = g_threads.find(self);
  if (it == g_threads.end()) return 0;
  if (it->second.thr) return it->second.thr->pe.priority;
  return it->second.priority;
}

void java_lang_Thread_setPriority(InvObject* self, int32_t newPriority) {
  // PE @ 0x0047C790 size 0x35 (53). Unbox this+I (JVM_UnboxArg @ 0x0045D910):
  // dest0=this, dest1=newPriority (var_4). handle = JVM_vm_get_int_field(this,
  // dword_62E008). handle==0 → no store. Else *(DWORD*)(handle+0x28)=newPriority.
  // Contrast getPriority @ 0x0047C760: same Unbox+dword_62E008+0x28 slot, but load.
  // PATH-TO-WORLD Soft: thr→pe.priority; null thr no-op (PE polarity).
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  auto it = g_threads.find(self);
  if (it == g_threads.end()) return;
  it->second.priority = newPriority;
  if (it->second.thr) it->second.thr->pe.priority = newPriority;
}

void java_lang_Thread_setDaemon(InvObject* self, int32_t daemon) {
  // PE @ 0x0047C6E0 size 0x4d (77). Unbox this+I
  // (JVM_UnboxArg @ 0x0045D910): dest0=this, dest1=daemon (var_4).
  // handle=JVM_vm_get_int_field(this, dword_62E008); handle==0 → no
  // store. Else flags=*(handle+0x2C): daemon!=0 → OR 0x100; else AND
  // 0xFFFFFEFF. Contrast setPriority @ 0x0047C790 (store at handle+0x28).
  // PATH-TO-WORLD Soft: ThreadState.daemon + thr flags bit 0x100.
  if (!self) return;
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  ThreadState& st = g_threads[self];
  st.daemon = daemon;
  if (!st.thr) return;
  if (daemon)
    st.thr->pe.flags |= 0x100;
  else
    st.thr->pe.flags &= ~0x100;
}

void java_lang_Thread_sleep(float millisec) {
  // PE @ 0x0047C650 size 0x29 (41). Unbox F (JVM_UnboxArg @ 0x0045D910):
  // dest0=&var_4 (static: no this write), dest1=&arg_0 := millisec float.
  // ECX = *(CallInfo+4) — current VM thread ctx. thiscall
  // Thread_setSleepDeadline @ 0x0041F630 (race125 rename, size 0x1C):
  // now=Engine_GetTimeMs @ 0x005516C0; *(float*)(this+0x30)=now+ms;
  // *(DWORD*)(this+0x2C)|=8 (sleep bit). Scheduler @ 0x00416940: skip run
  // while bit8; clear when Engine_GetTimeMs() > *(float*)(this+0x30).
  // Stock is cooperative, not OS Sleep. PATH-TO-WORLD Soft: Win32 Sleep /
  // usleep — LoadingScreen / SoftTimer need a real block on OS thread.
  // (No CallInfo+4 current-VM ctx on host OS workers.)
#ifdef _WIN32
  Sleep(static_cast<DWORD>(millisec < 0 ? 0 : millisec));
#else
  usleep(static_cast<useconds_t>(millisec * 1000.f));
#endif
}

void java_lang_Thread_suspend(InvObject* self) {
  // PE @ 0x0047C680 size 0x2f (47). Unbox this. handle=JVM_vm_get_int_field
  // (this, dword_62E008). handle==0 → ret. Else JMP loc_41F570 (ECX=handle):
  // flags=*(DWORD*)(handle+0x2C); if (flags&0x20) already suspended → ret.
  // Else flags|=0x20; store +0x2C. if (flags&0x10)!=0 → skip queue relink
  // (WAITING). Else unlink +4/+8, push onto suspend queue via
  // *(*(handle+0x10)+0x1C) head at +0x1C. Contrast resume @ 0x0047C6B0
  // (clear bit20, run queue +0x18). PATH-TO-WORLD Soft: suspended=true +
  // thr flags |= 0x20 (start() polls before run).
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  ThreadState& st = g_threads[self];
  st.suspended = true;
  if (st.thr && (st.thr->pe.flags & 0x20) == 0) st.thr->pe.flags |= 0x20;
}

void java_lang_Thread_resume(InvObject* self) {
  // PE @ 0x0047C6B0 size 0x2f (47). Unbox this (JVM_UnboxArg @ 0x0045D910).
  // handle = JVM_vm_get_int_field(this, dword_62E008). handle==0 → ret.
  // Else JMP loc_41F5D0 (ECX=handle): flags=*(DWORD*)(handle+0x2C);
  // if (flags&0x20)==0 → already runnable, ret. Else flags&=~0x20; store +0x2C.
  // if (flags&0x10)!=0 → skip queue relink. Else unlink +4/+8 list, then
  // push onto run queue via *( *(handle+0x10) +0x18 ) head at +0x18.
  // Contrast suspend @ 0x0047C680: JMP loc_41F570 sets bit 0x20.
  // Contrast stop @ 0x0047C600: requestStop + clear +0x18 + destroy ptr.
  // PATH-TO-WORLD Soft: suspended=false + thr flags &= ~0x20.
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  ThreadState& st = g_threads[self];
  st.suspended = false;
  if (st.thr) st.thr->pe.flags &= ~0x20;
}

// ---- Object ----

namespace {

struct ObjMonitor {
  std::mutex mu;
  std::condition_variable cv;
  int pending = 0;  // notify tokens (sticky until consumed by wait)
  int waiters = 0;
  int wake_count = 0;
  int notify_count = 0;
  int gc_disabled = 0;
};

std::mutex g_mon_map_mu;
std::unordered_map<InvObject*, std::shared_ptr<ObjMonitor>> g_monitors;

std::shared_ptr<ObjMonitor> monitor_of(InvObject* self) {
  std::lock_guard<std::mutex> lock(g_mon_map_mu);
  auto& p = g_monitors[self];
  if (!p) p = std::make_shared<ObjMonitor>();
  return p;
}

}  // namespace

void java_lang_Object_wait(InvObject* self) {
  // PE @ 0x0047C920 size 0x22 (34). Unbox this (JVM_UnboxArg @ 0x0045D910).
  // Object_MonitorEnqueue @ 0x00408B20(monitor, CallInfo+4 Thread*):
  //   Thread_markWaiting @ 0x0041F650 — OR flags+0x2C bit0x10 (WAITING),
  //   store monitor @ thread+0x34, unlink run-queue DLL (+4/+8), link wait-
  //   queue; then push Thread* onto T_Container @ InvObject+0x14 (lazy
  //   Engine_malloc 12 B: capacity/data/count). Native returns immediately —
  //   stock does NOT OS-block; cooperative JVM yields until notify pops LIFO
  //   and Thread_notify @ 0x0041F6F0 clears WAITING / re-queues (+0x18) unless
  //   SUSPENDED bit0x20. No sticky tokens; empty notify-before-wait is no-op.
  // xref: Natives_RegisterAll @ 0x00488534. Pair: notify @ 0x0047C950 /
  // notifyAll @ 0x0047C970 (_ida_race107_object_wait / _ida_race108_object_notify).
  // Host stand-in: ObjMonitor side-table CV (not InvObject+0x14). Blocks the
  // calling OS thread until sticky pending>0 — usable for smoke; full PE port
  // needs VM scheduler (Thread_markWaiting / Thread_notify / T_Container), out
  // of scope here (no invented green-thread APIs). LoadingScreen render waits
  // still bypassed via polling (D3D9 thread safety).
  if (!self) return;
  auto m = monitor_of(self);
  std::unique_lock<std::mutex> lk(m->mu);
  ++m->waiters;
  tree_field_set_int(self, "waiting", 1);
  m->cv.wait(lk, [&] { return m->pending > 0; });
  --m->pending;
  --m->waiters;
  ++m->wake_count;
  tree_field_set_int(self, "waiting", 0);
  tree_field_set_int(self, "wake_count", m->wake_count);
}

void java_lang_Object_notify(InvObject* self) {
  // PE @ 0x0047C950 size 0x1b (27). Unbox this → Object_MonitorNotify
  // body @ 0x00408BC0: T_Container @ this+0x14; if count==0 → no-op
  // (no sticky token). Else loop count times: pop LIFO waiter, clear
  // slot, Thread_notify @ 0x0041F6F0 (first non-null stops — notify
  // wakes ONE). Underflow → Engine_ErrorLogMsgBox "T_Container:
  // underflow". Contrast notifyAll @ 0x0047C970 (jmp 0x00408CE0, wake
  // all). xref Natives_RegisterAll @ 0x00488553.
  // Host stand-in: sticky pending+1 + cv.notify_one (pairs with wait
  // @ 0x0047C920 host CV — needed for smoke; full PE needs VM scheduler).
  // PE @ 0x0047C950
  if (!self) return;
  auto m = monitor_of(self);
  std::lock_guard<std::mutex> lk(m->mu);
  ++m->pending;
  ++m->notify_count;
  tree_field_set_int(self, "notify_count", m->notify_count);
  m->cv.notify_one();
}

void java_lang_Object_notifyAll(InvObject* self) {
  // PE @ 0x0047C970 size 0x1b (27). Unbox this (JVM_UnboxArg @ 0x0045D910).
  // jmp Object_MonitorNotifyAll @ 0x00408CE0 — no inline body (contrast notify
  // @ 0x0047C950: same Unbox then monitor pop loop inlined ending @ 0x00408C3E).
  // Object_MonitorNotifyAll: T_Container @ this+0x14; loop initial count times:
  // pop waiter stack (index count-1), clear slot, Thread_notify @ 0x0041F6F0
  // each non-null; T_Container underflow → Engine_ErrorLogPrintf. Count==0 →
  // no-op (no sticky token for future waiters). xref: Natives_RegisterAll
  // @ 0x00488572.
  // Host stand-in: ObjMonitor CV — one pending token per current waiter, then
  // cv.notify_all() (LoadingScreen.termSig / Signal.notifyAll).
  if (!self) return;
  auto m = monitor_of(self);
  std::lock_guard<std::mutex> lk(m->mu);
  if (m->waiters <= 0) return;
  m->pending += m->waiters;
  ++m->notify_count;
  tree_field_set_int(self, "notify_count", m->notify_count);
  m->cv.notify_all();
}

void java_lang_Object_enableGC(InvObject* self) {
  // PE @ 0x0047C8E0 size 0x1b (27). Unbox this → Object_GC_enable
  // @ 0x00408780 size 0x58: if (flags+0x18 & 4)==0 → already enabled,
  // ret. Else unlink DLL +4/+8, link onto GC list via class+0xC → +0x10
  // head, flags &= ~4. Idempotent bit clear (not a nest counter).
  // Host stand-in: gc_disabled bit → 0 (no real GC lists).
  // PE @ 0x0047C8E0 — race123 deepen: IDA Object_GC_enable confirmed.
  // race124/125 deepen: Object_GC_enable @ 0x00408780 — bit4 clear +
  // link GC list via class+0xC→+0x10; idempotent; host gc_disabled=0.
  if (!self) return;
  auto m = monitor_of(self);
  std::lock_guard<std::mutex> lk(m->mu);
  m->gc_disabled = 0;
  tree_field_set_int(self, "gc_disabled", 0);
}

void java_lang_Object_disableGC(InvObject* self) {
  // PE @ 0x0047C8C0 size 0x1b (27). Unbox this → Object_GC_disable
  // @ 0x00408720 size 0x58: if (flags+0x18 & 4)!=0 → already pinned,
  // ret. Else unlink DLL, link onto no-GC list via class+0xC → +0x14
  // head, flags |= 4. Idempotent bit set (nested disable is no-op).
  // Host stand-in: gc_disabled=1 (no real GC lists).
  // PE @ 0x0047C8C0 — race123 deepen: IDA Object_GC_disable confirmed.
  // race124/125 deepen: Object_GC_disable @ 0x00408720 — bit4 set +
  // link no-GC list via class+0xC→+0x14; idempotent; host gc_disabled=1.
  if (!self) return;
  auto m = monitor_of(self);
  std::lock_guard<std::mutex> lk(m->mu);
  m->gc_disabled = 1;
  tree_field_set_int(self, "gc_disabled", 1);
}

int32_t java_lang_Object_hashCode(InvObject* self) {
  // PE @ 0x0047C900 size 0x17 (int_convert 23). Unbox this
  // (JVM_UnboxArg @ 0x0045D910): dest0=&arg_0 overwrites CallInfo with
  // object ptr. mov eax,[esp+arg_0]; ret — identity pointer as int,
  // NOT JDK scramble / System.identityHashCode mix. Sole callee
  // JVM_UnboxArg. xref: Natives_RegisterAll @ 0x004885EE. Host: InvObject*
  // is the unboxed handle → same cast.
  // PE @ 0x0047C900
  return static_cast<int32_t>(reinterpret_cast<uintptr_t>(self));
}

InvObject* java_lang_Object_toString(InvObject* self) {
  // PE @ 0x0047C870 size 0x50 (80). Unbox this (JVM_UnboxArg @ 0x0045D910).
  // Class_getNameCstr @ 0x00404EA0(*(this+0xC)) — Class* FQN C string.
  // Util_Sprintf(dst256, "%s<%0x>" @ 0x00612D1C, cn, thisObj) →
  // JVM_String_from_cstr @ 0x004174A0. xref: Natives_RegisterAll @ 0x00488591.
  char buf[256];
  const char* cn = tree_host_class(self);
  if (!cn || !cn[0]) cn = "java.lang.Object";
  std::snprintf(buf, sizeof(buf), "%s<%0x>", cn,
                static_cast<unsigned>(reinterpret_cast<uintptr_t>(self)));
  return string_new(buf);
}

// ---- Input controllers (GameLogic boot) ----

namespace {
constexpr int kMaxPlayers = 1;
InvObject* g_input_controllers[kMaxPlayers] = {};
}  // namespace

InvObject* input_init_controllers() {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  // Mirror Input.initControllers (old_controls=0): one Controller with id!=0.
  // Controller.RESOURCEID = system:0x32 — bind a non-zero ResourceRef id.
  if (!g_input_controllers[0]) {
    InvObject* c = gameref_new();
    java_util_resource_ResourceRef_set(c, 0x32);
    InvObject* css = tree_host_new("java.io.ControlSetState");
    for (int i = 0; i < 5; ++i) {
      char key[8];
      std::snprintf(key, sizeof(key), "g%d", i);
      tree_field_set_int(css, key, 0);
    }
    tree_field_set_obj(c, "css", css);
    g_input_controllers[0] = c;
  }
  return g_input_controllers[0];
}

int32_t input_is_player_active(int32_t n) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  if (n < 0 || n >= kMaxPlayers || !g_input_controllers[n]) return 0;
  return java_util_resource_ResourceRef_id(g_input_controllers[n]) != 0 ? 1
                                                                        : 0;
}

InvObject* input_get_controller(int32_t n) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  if (n < 0 || n >= kMaxPlayers) return nullptr;
  return g_input_controllers[n];
}

namespace {
constexpr int32_t kDefaultSet = 0;
InvObject* css_of(InvObject* ctrl) {
  if (!ctrl) return nullptr;
  InvObject* css = tree_field_get_obj(ctrl, "css");
  if (css) return css;
  css = tree_host_new("java.io.ControlSetState");
  for (int i = 0; i < 5; ++i) {
    char key[8];
    std::snprintf(key, sizeof(key), "g%d", i);
    tree_field_set_int(css, key, 0);
  }
  tree_field_set_obj(ctrl, "css", css);
  return css;
}
}  // namespace

void controller_activate_state(InvObject* ctrl, int32_t group,
                               int32_t new_state) {
  InvObject* css = css_of(ctrl);
  if (!css || group < 0 || group >= 5) return;
  char key[8];
  std::snprintf(key, sizeof(key), "g%d", group);
  if (tree_field_get_int(css, key) == new_state) return;
  tree_field_set_int(css, key, new_state);
  // ControlSet.load / user_Add skipped until control-file IO is wired.
}

InvObject* controller_reset(InvObject* ctrl) {
  InvObject* css = css_of(ctrl);
  if (!css) return nullptr;
  for (int i = 0; i < 5; ++i) {
    char key[8];
    std::snprintf(key, sizeof(key), "g%d", i);
    tree_field_set_int(css, key, 0);
  }
  // Mirror Controller.reset(null) → activateState(DEFAULTSET).
  controller_activate_state(ctrl, kDefaultSet, 1);
  return css;
}

int32_t controller_css_get(InvObject* ctrl, int32_t group) {
  InvObject* css = css_of(ctrl);
  if (!css || group < 0 || group >= 5) return 0;
  char key[8];
  std::snprintf(key, sizeof(key), "g%d", group);
  return tree_field_get_int(css, key);
}

// ---- ControlSet (CTRL binary) ----

namespace {
constexpr int32_t kCtrlFileId = 0x4c525443;
constexpr int32_t kCtrlFileVersion = 16;
constexpr int kNControls = 58 + 8;

struct CtrlSetData {
  int nDevices = 0;
  std::vector<std::string> deviceName;
  int group[kNControls]{};
  int vaxisID[kNControls]{};
  int deviceID[kNControls]{};
  int axisID[kNControls]{};
  float from_min[kNControls]{};
  float from_max[kNControls]{};
  float to_min[kNControls]{};
  float to_max[kNControls]{};
  float dead_zone[kNControls]{};
  int nControls = kNControls;
  bool loaded = false;
};

std::unordered_map<InvObject*, CtrlSetData> g_control_sets;

CtrlSetData& ctrl_data(InvObject* cs) { return g_control_sets[cs]; }
}  // namespace

InvObject* control_set_new() {
  InvObject* cs = tree_host_new("java.io.ControlSet");
  CtrlSetData& d = ctrl_data(cs);
  // Mirror ControlSet ctor (sources ControlSet.java): while
  // (Input.getDeviceName(nDevices) != null) nDevices++ — null only
  // (PE @ 0x0047C9C0 / DiDevice_getName OOB); empty name still counts.
  d.nDevices = 0;
  for (int i = 0; i < kInputDeviceCap; ++i) {
    InvObject* name = java_io_Input_getDeviceName(i);
    if (!name) break;
    const char* s = string_cstr(name);
    d.deviceName.emplace_back(s ? s : "");
    ++d.nDevices;
  }
  d.nControls = kNControls;
  tree_field_set_int(cs, "nDevices", d.nDevices);
  tree_field_set_obj(cs, "vasp", tree_vector_new());
  return cs;
}

int32_t control_set_file_check(const char* path) {
  if (!path || !path[0]) return 0;
  InvObject* f = file_new(path);
  if (!java_io_File_open(f, 0)) return 0;
  const int32_t magic = java_io_File_readInt(f);
  const int32_t ver = java_io_File_readInt(f);
  java_io_File_close(f);
  return (magic == kCtrlFileId && ver == kCtrlFileVersion) ? 1 : 0;
}

int32_t control_set_load(InvObject* cs, const char* path) {
  if (!cs || !path || !path[0]) return 0;
  InvObject* f = file_new(path);
  if (!java_io_File_open(f, 0)) return 0;
  const int32_t magic = java_io_File_readInt(f);
  const int32_t ver = java_io_File_readInt(f);
  if (magic != kCtrlFileId || ver != kCtrlFileVersion) {
    java_io_File_close(f);
    return 0;
  }

  CtrlSetData& d = ctrl_data(cs);
  const int32_t nDev = java_io_File_readInt(f);
  d.deviceName.clear();
  if (nDev > 0) d.deviceName.reserve(static_cast<size_t>(nDev));
  for (int i = 0; i < nDev; ++i) {
    InvObject* sn = java_io_File_readString(f);
    const char* s = string_cstr(sn);
    d.deviceName.emplace_back(s ? s : "");
  }
  d.nDevices = nDev;

  const int32_t n = java_io_File_readInt(f);
  for (int i = 0; i < n; ++i) {
    const int32_t group = java_io_File_readInt(f);
    const int32_t vaxis = java_io_File_readInt(f);
    int32_t id = java_io_File_readInt(f);
    const int32_t axis = java_io_File_readInt(f);
    const float fmin = java_io_File_readFloat(f);
    const float fmax = java_io_File_readFloat(f);
    const float tmin = java_io_File_readFloat(f);
    const float tmax = java_io_File_readFloat(f);
    const float dz = java_io_File_readFloat(f);
    if (i < kNControls) {
      if (id < 0) id = -id - 1;
      d.group[i] = group;
      d.vaxisID[i] = vaxis;
      d.deviceID[i] = id;
      d.axisID[i] = axis;
      d.from_min[i] = fmin;
      d.from_max[i] = fmax;
      d.to_min[i] = tmin;
      d.to_max[i] = tmax;
      d.dead_zone[i] = dz;
    }
  }
  d.nControls = n < kNControls ? n : kNControls;

  const int32_t nVasp = java_io_File_readInt(f);
  InvObject* vasp = tree_vector_new();
  for (int i = 0; i < nVasp; ++i) {
    // VirtualAxisSmoothProperties(ctrlFile) — skip 6 floats if present later.
    (void)i;
  }
  tree_field_set_obj(cs, "vasp", vasp);
  tree_field_set_int(cs, "nDevices", d.nDevices);
  d.loaded = true;
  java_io_File_close(f);
  return 1;
}

int32_t control_set_nitems(InvObject* cs) {
  if (!cs) return 0;
  auto it = g_control_sets.find(cs);
  if (it == g_control_sets.end()) return kNControls;
  return it->second.nControls;
}

int32_t control_set_ndevices(InvObject* cs) {
  if (!cs) return 0;
  auto it = g_control_sets.find(cs);
  return it == g_control_sets.end() ? 0 : it->second.nDevices;
}

int32_t control_set_count_group(InvObject* cs, int32_t group) {
  if (!cs) return 0;
  auto it = g_control_sets.find(cs);
  if (it == g_control_sets.end()) return 0;
  int n = 0;
  for (int i = 0; i < it->second.nControls; ++i) {
    if (it->second.group[i] == group) ++n;
  }
  return n;
}

}  // namespace inv
