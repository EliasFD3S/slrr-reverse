#include "natives.hpp"
#include "audio_win32.hpp"

#include <cstdint>

namespace inv {
namespace {
int32_t g_sound_mode = 0;  // PE *(Sound_currentMusicSet+0x10); BSS 0.
// PE Sound_dscaps_dwFreeHw3DAllBuffers @ 0x77C9B8 (DSCAPS dword_77C980+0x38).
// PE Sound_dscaps_dwFreeHwMixingAllBuffers @ 0x77C9A0 (DSCAPS+0x20).
// Host: no GetCaps fill in this TU; stand-in free>=4 (always capable).
uint32_t g_sound_dscaps_dwFreeHw3DAllBuffers = 4;
uint32_t g_sound_dscaps_dwFreeHwMixingAllBuffers = 4;
}

// PE @ 0x00487460 size 0x81 (int_convert 129). STATIC (I)V. UnboxArg
// dest0=&var_4 dummy this unread, dest1=&arg_0 type (overwrites
// CallInfo). After unbox: cmp eax,3 / ja default (Hex-Rays switch(a1)
// is a lie). case 0..3: esi=&Sound_musicSetGarage @ 0x63C878
// (Music\Garage_Shop) / Sound_musicSetDriving @ 0x640908 (Roam_Ride) /
// Sound_musicSetRace @ 0x6408A8 (Race_Chase) / Input_hotkeyTableEnd @
// 0x640890 MENU (Main_Menu; dual hotkey-table end). default xor esi,esi.
// cmp esi,Sound_currentMusicSet @ 0x64091C / jz ret. Else if ecx!=0:
// thiscall MusicSet_stop @ 0x550F30. mov Sound_currentMusicSet,esi.
// If esi: thiscall MusicSet_play @ 0x550E50; push Sound_volumeMusic @
// 0x612C5C; thiscall MusicSet_applyVolume @ 0x550FE0; fstp st. VOID.
// Host: int set id ↔ BSS ptr; -1 ↔ esi=0. Stand-in audio_change_music_set
// (stop/store/scan+play+volume); same-id early exit = PE same-ptr.
void java_sound_Sound_changeMusicSet(int32_t type) {
  // PE @ 0x00487460
  const int32_t next = (type >= 0 && type <= 3) ? type : -1;  // cmp/ja → esi; else 0..3
  const int32_t current = audio_music_set();  // mov ecx,Sound_currentMusicSet @ 0x64091C
  if (next == current)                        // cmp esi,ecx / jz loc_4874DE
    return;
  audio_change_music_set(next);  // stop if current; store; play+applyVolume if esi
}
// PE @ 0x00487500 size 0x10 (int_convert 16). STATIC ()V, no UnboxArg,
// no this. Head: mov ecx,Sound_currentMusicSet @ 0x64091C / test /
// jz locret_48750F / jmp loc_550F40. Tail (IDA same fn): push esi;
// mov esi,ecx; call sub_55AF00 (not renamed); mov eax,[esi+4]; inc eax;
// mov ecx,esi; mov [esi+4],eax; thiscall MusicSet_play @ 0x550E50;
// mov eax,[esi+4]; pop; ret. Wrap [this+4]>=[this+8]→0 is inside
// MusicSet_play, not thunk. VOID Java (eax discarded). Contrast
// prevTrack @ 0x00487510 size 0x10: same head/tail shape, jmp
// loc_550F60, dec not inc; wrap [this+4]<0→count-1 also in
// MusicSet_play. Host: audio_music_next_track.
void java_sound_Sound_nextTrack() { audio_music_next_track(); }
// PE @ 0x00487510 size 0x10. STATIC ()V, no UnboxArg. ecx=
// Sound_currentMusicSet @ 0x64091C; jz ret; jmp loc_550F60: sub_55AF00
// (not renamed), --[esi+4], MusicSet_play @ 0x550E50 (wrap <0 →
// count-1 inside play). VOID Java. Contrast nextTrack @ 0x00487500:
// loc_550F40 + inc; wrap >=count → 0. Host: audio_music_prev_track.
void java_sound_Sound_prevTrack() { audio_music_prev_track(); }
// PE @ 0x00487580 size 0x6c. STATIC. UnboxArg dest0=&var_4 dummy this
// (unread). dest1=&var_8 channel. dest2=&arg_0 volume. NO 0..1 clamp
// (no 1.0f / fcomp; Java increase/decreaseVolume only). ch 0 store
// Sound_volumeEffects @ 0x612C58; ch 1 store Sound_volumeMusic @
// 0x612C5C then if Sound_currentMusicSet @ 0x64091C!=0 call
// MusicSet_applyVolume @ 0x550FE0; ch 2 store Sound_volumeEngine @
// 0x612C60; else no store. VOID. Host: audio_set_volume (ch 0/1/2).
void java_sound_Sound_setVolume(int32_t channel, float volume) {
  audio_set_volume(channel, volume);
}
// PE @ 0x004875F0 size 0x47 (int_convert 71). STATIC (I)F. UnboxArg
// dest0=&var_4 dummy this unread, dest1=&arg_0 channel (overwrites
// CallInfo). Sole callee JVM_UnboxArg @ 0x0045D910. After unbox:
// mov eax,[esp+arg_0]; sub eax,0 / jz ch0; dec/jz ch1; dec/jz ch2;
// else fld flt_5E73CC (bytes 00 00 00 00 = 0.0). ch0 fld
// Sound_volumeEffects @ 0x612C58; ch1 Sound_volumeMusic @ 0x612C5C;
// ch2 Sound_volumeEngine @ 0x612C60 (Java CHANNEL_EFFECTS/MUSIC/
// ENGINE). Same BSS triad as setVolume @ 0x00487580 (init
// 0x3F800000 = 1.0). NO clamp / no Mighty. Host: audio_get_volume
// (ch 0/1/2 else 0.f).
float java_sound_Sound_getVolume(int32_t channel) {
  // PE @ 0x004875F0
  return audio_get_volume(channel);
}
// PE @ 0x00487F00 size 0x17. STATIC (Ljava.util.resource.ResourceRef;)I.
// UnboxArg dest0=nullptr (static skip this), dest1=&arg_0 (overwrites
// CallInfo with unboxed ResourceRef DWORD). Value unread. xor eax,eax
// → return 0. Sole callee JVM_UnboxArg @ 0x0045D910. 1 xref data:
// Natives_RegisterAll. Java Sound.init uses as bool — stock never
// enters debug Viewport/Camera path. Host: discard display, return 0.
int32_t java_sound_Sound_enableDebugDump(InvObject* display) {
  (void)display;
  return 0;
}
// PE @ 0x00487560 size 0x11. STATIC ()I: no UnboxArg, no this, no callees,
// no Mighty ERROR. Bytes: 8B 0D 1C 09 64 00 / 83 C8 FF / 85 C9 / 74 03 /
// 8B 41 10 / C3. ecx=Sound_currentMusicSet @ 0x64091C (BSS 0). or eax,-1
// @ 0x00487566; jz → -1. Else eax=[ecx+0x10] (int_convert 16) — same
// DWORD setMode stores @ 0x0048754e (signed 0..3 jl/jg). Four BSS
// music-set objs share the current ptr; host: g_sound_mode = that slot;
// audio_music_set() not in 0..3 stands in for ptr==0.
int32_t java_sound_Sound_getMode() {
  // PE @ 0x00487560
  const int32_t current = audio_music_set();  // mov ecx,Sound_currentMusicSet @ 0x64091C
  if (current < 0 || current > 3)             // test ecx,ecx / jz locret_487570; host ↔ ptr==0
    return -1;                                // or eax,-1 @ 0x487566
  return g_sound_mode;                        // mov eax,[ecx+10h] @ 0x48756d
}
// PE @ 0x00487520 size 0x3c (int_convert 60). STATIC (I)I. UnboxArg
// dest0=&var_4 dummy this unread, dest1=&arg_0 mode. Sole callee
// JVM_UnboxArg @ 0x0045D910. eax=Sound_currentMusicSet @ 0x64091C;
// test eax / jz loc_487556 @ 0x48753f → return unboxed arg (no store).
// Else ecx=mode: test ecx / jl loc_487551; cmp ecx,3 / jg loc_487551;
// mov [eax+0x10],ecx @ 0x48754e (signed 0..3; Hex-Rays a1<4 is a lie —
// bytes 0x7C jl / 0x7F jg). loc_487551: return [eax+0x10]. Same DWORD
// as getMode @ 0x48756d. No Mighty. Host: g_sound_mode = [ptr+0x10];
// audio_music_set() not in 0..3 stands in for ptr==0 (same as getMode).
int32_t java_sound_Sound_setMode(int32_t mode) {
  // PE @ 0x00487520
  const int32_t current = audio_music_set();  // mov eax,Sound_currentMusicSet @ 0x64091C
  if (current < 0 || current > 3)             // test eax / jz loc_487556 @ 0x48753f
    return mode;                              // mov eax,[esp+arg_0] @ 0x487556
  if (mode >= 0 && mode <= 3)                 // jl/jg skip @ 0x487547/4c
    g_sound_mode = mode;                      // mov [eax+10h],ecx @ 0x48754e
  return g_sound_mode;                        // mov eax,[eax+10h] @ 0x487551
}
// PE @ 0x00487640 size 0x5 (int_convert 5). STATIC ()I: no UnboxArg,
// no this. Bytes: E9 DB 27 0D 00 — jmp Sound_has3DHardware @
// 0x00559E20 size 0xb (int_convert 11): cmp
// Sound_dscaps_dwFreeHw3DAllBuffers,4 / sbb eax,eax / inc eax / ret
// → (unsigned) >= 4. Field @ 0x77C9B8 = DSCAPS+0x38 (int_convert 56)
// dwFreeHw3DAllBuffers (GetCaps into dword_77C980, dwSize=96 @
// Sound_InitDirectSound). Same predicate InitDS stores to dword_784CA4.
// Engine_boot also calls when Sound_3D_HW==2. No Mighty. Host: BSS
// stand-in g_sound_dscaps_dwFreeHw3DAllBuffers (no GetCaps here).
int32_t java_sound_Sound_has3DHardware() {
  // PE @ 0x00487640
  return static_cast<int32_t>(g_sound_dscaps_dwFreeHw3DAllBuffers >= 4u);
}
// PE @ 0x00487650 size 0x5. STATIC ()I: no UnboxArg, no this. Bytes:
// E9 DB 27 0D 00 — jmp Sound_hasMixHardware @ 0x00559E30 size 0xb:
// cmp Sound_dscaps_dwFreeHwMixingAllBuffers,4 / sbb eax,eax / inc eax /
// ret → (unsigned) >= 4. Contrast has3DHardware @ 0x00487640: same
// thunk+body shape (identical E9 DB 27 0D 00 rel), different DSCAPS
// field — mix @ 0x77C9A0 = DSCAPS+0x20 dwFreeHwMixingAllBuffers; 3D
// uses +0x38 Sound_dscaps_dwFreeHw3DAllBuffers @ 0x77C9B8. Same
// GetCaps buffer dword_77C980. InitDS stores this predicate to
// dword_784CA8 (3D → dword_784CA4). Engine_boot also calls when
// Sound_Mix_HW==2. No Mighty. Host: BSS stand-in
// g_sound_dscaps_dwFreeHwMixingAllBuffers (no GetCaps here).
int32_t java_sound_Sound_hasMixHardware() {
  // PE @ 0x00487650
  return static_cast<int32_t>(g_sound_dscaps_dwFreeHwMixingAllBuffers >= 4u);
}

}  // namespace inv
