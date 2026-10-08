#include "natives.hpp"
#include "tree_interp.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace inv {
// Forward: Soft PlayWithListenerCull (anon) calls this before its body.
int32_t sfx_3d_listener_cull(float x, float y, float z, float radius);

namespace {
// PE MusicSet layout (MusicSet_init @ 0x00550BE0 size 0x7c):
//   +0x00 float volume = 1.0 (0x3F800000); +0x04 track_idx; +0x08 count;
//   +0x0C playlist char* (MusicSet_scanFolder @ 0x550C60); +0x10 mode = 3.
// BSS: Sound_musicSetGarage @ 0x63C878 / Driving @ 0x640908 /
// Race @ 0x6408A8 / Menu @ 0x640890. Sound_currentMusicSet @ 0x64091C.
// Soft: no CRT_malloc playlist / GetTickCount random start — count=1 pad.
struct SoftMusicSet {
  float volume;
  int32_t track_idx;
  int32_t track_count;
  int32_t playlist_pad;  // Soft: non-zero ↔ PE playlist ptr != 0
  int32_t mode;
};
static_assert(offsetof(SoftMusicSet, mode) == 0x10);
static_assert(sizeof(SoftMusicSet) == 20);

SoftMusicSet g_music_set_garage{};
SoftMusicSet g_music_set_driving{};
SoftMusicSet g_music_set_race{};
SoftMusicSet g_music_set_menu{};
SoftMusicSet* g_sound_current_music_set = nullptr;  // PE @ 0x64091C
bool g_music_sets_inited = false;

// PE Sound_dscaps @ 0x77C980 (DSCAPS dwSize=96). GetCaps OOS this TU.
// PE Sound_dscaps_dwFreeHw3DAllBuffers @ 0x77C9B8 (DSCAPS+0x38) — has3D.
// PE Sound_dscaps_dwFreeHwMixingAllBuffers @ 0x77C9A0 (DSCAPS+0x20) — hasMix.
// Soft stand-in free>=4 (always capable).
uint32_t g_sound_dscaps_dwFreeHw3DAllBuffers = 4;
uint32_t g_sound_dscaps_dwFreeHwMixingAllBuffers = 4;
// PE Sound_dscaps_dwFreeHwMixingStaticBuffers @ 0x77C9A4 (DSCAPS+0x24).
// PE Sound_dscaps_dwFreeHw3DStaticBuffers @ 0x77C9BC (DSCAPS+0x3C).
// Used by Sfx_HwVoiceBudget @ 0x00559DD0 (UpdateVoices head) — Soft PE.
uint32_t g_sound_dscaps_dwFreeHwMixingStaticBuffers = 4;
uint32_t g_sound_dscaps_dwFreeHw3DStaticBuffers = 4;
// PE Sound_hw3DCapable @ 0x784CA4 / Sound_hwMixCapable @ 0x784CA8 —
// InitDS stores has3D/hasMix predicates. Soft stand-in 1 (capable).
uint32_t g_sound_hw3DCapable = 1;
uint32_t g_sound_hwMixCapable = 1;
// PE Sound_volumeEffects/Music/Engine @ 0x612C58/5C/60 — BSS init
// 0x3F800000 (1.0f). Java CHANNEL_EFFECTS/MUSIC/ENGINE. setVolume stores
// raw float (no clamp); getVolume loads same triad / else 0.0.
float g_sound_volumeEffects = 1.f;
float g_sound_volumeMusic = 1.f;
float g_sound_volumeEngine = 1.f;
// PE Sfx_listener layout @ 0x768720 (12 floats / 48B):
//   xyz @ 0x768720/24/28; orient o/u/f @ 0x768730..0x768758
//   (IDA: Sfx_listener_ox..fz). Writer Sfx_ListenerSetPose @ 0x005508F0
//   — sole xref Engine_MainLoop @ 0x00428CB6. Host wire:
//   system_mainloop_sfx_listener_update (System.cpp) ← Input.checkHotkeys
//   passes xyz only; Soft PE keeps orient BSS 0 (full 12-float pose OOS).
float g_sfx_listener_x = 0.f;
float g_sfx_listener_y = 0.f;
float g_sfx_listener_z = 0.f;
float g_sfx_listener_ox = 0.f;  // PE @ 0x768730
float g_sfx_listener_oy = 0.f;  // PE @ 0x768734
float g_sfx_listener_oz = 0.f;  // PE @ 0x768738
float g_sfx_listener_ux = 0.f;  // PE @ 0x768740
float g_sfx_listener_uy = 0.f;  // PE @ 0x768744
float g_sfx_listener_uz = 0.f;  // PE @ 0x768748
float g_sfx_listener_fx = 0.f;  // PE @ 0x768750
float g_sfx_listener_fy = 0.f;  // PE @ 0x768754
float g_sfx_listener_fz = 0.f;  // PE @ 0x768758
// Soft PE last Sfx_HwVoiceBudget @ 0x00559DD0 result (PE local v9).
int32_t g_sfx_voice_hw_budget = 4;
// PE flt_5F3AA0 @ 0x005F3AA0 bytes 00 24 74 47 → 62500.0 (py_eval).
// PE flt_5F0CFC @ 0x005F0CFC bytes 00 00 80 41 → 16.0.
// PE flt_5F0C20 @ 0x005F0C20 bytes 0A D7 23 3C → 0.01 (volume gate).
// PE free-slot priority sentinel @ UpdateVoices [slot+0x14] = 0x501502F9
// → 1e10f (py_eval struct).
constexpr float kSfxCullMaxDist2 = 62500.f;
constexpr float kSfxCullNearDist2 = 16.f;
constexpr float kSfxMinEffVol = 0.01f;  // flt_5F0C20
constexpr float kSfxFreePrioritySentinel = 1e10f;  // 0x501502F9
constexpr int32_t kSfxVoiceSortCount = 64;         // PE qsort n=0x40
constexpr int32_t kSfxVoiceSlotStrideDwords = 23;  // 92B / slot
constexpr int32_t kSfxVoiceTypeCount = 0x80;       // typeUse / MaxCount gate
constexpr int32_t kSfxHwVoiceBudgetMin = 4;        // PE clamp
static_assert(kSfxVoiceSortCount == 64 && kSfxVoiceSlotStrideDwords == 23);
static_assert(kSfxFreePrioritySentinel == 1e10f);
static_assert(sizeof(float) == 4);

// PE Sfx_voiceTable slot @ 0x768A68 stride 23 dwords / 92B (AllocOrUpdateVoice
// + UpdateVoices + InitVoiceTable). Fields used by qsort/pass1:
//   +0x00 state (0 free / 1 pending / 2 playing / 3 update)
//   +0x04 type  (dword_768A6C)
//   +0x08 handle / +0x0C instance
//   +0x10 ds_voice (Soft packed (type<<16)|1; PE bank OOS)
//   +0x14 priority float (Sfx_voicePriority @ 0x768A7C)
//   +0x18 payload[17] (flags @ [0] pass2 bit0)
struct SfxVoiceSlot {
  int32_t state;
  int32_t type;
  int32_t handle;
  int32_t instance;
  int32_t ds_voice;
  float priority;
  int32_t payload[17];
};
static_assert(sizeof(SfxVoiceSlot) == 92);

// PE Sfx_voiceTable[64] @ 0x768A68 / Sfx_voiceSortIdx[64] @ 0x768968 /
// Sfx_voiceTypeUseCount[0x80] @ 0x768768. Soft BSS stand-ins.
SfxVoiceSlot g_sfx_voice_table[kSfxVoiceSortCount]{};
int32_t g_sfx_voice_sort_idx[kSfxVoiceSortCount]{};
int32_t g_sfx_voice_type_use_count[kSfxVoiceTypeCount]{};
bool g_sfx_voice_table_inited = false;

// Soft PE Sfx_HwVoiceBudget @ 0x00559DD0 size 0x4C:
// sum = (hwMix ? FreeHwMixingStatic : 0) + (hw3D ? FreeHw3DStatic : 0);
// return sum <= 4 ? 4 : sum. Sole callee of UpdateVoices head.
int32_t soft_sfx_hw_voice_budget() {
  uint32_t sum = 0;
  if (g_sound_hwMixCapable)
    sum += g_sound_dscaps_dwFreeHwMixingStaticBuffers;
  if (g_sound_hw3DCapable)
    sum += g_sound_dscaps_dwFreeHw3DStaticBuffers;
  return sum <= static_cast<uint32_t>(kSfxHwVoiceBudgetMin)
             ? kSfxHwVoiceBudgetMin
             : static_cast<int32_t>(sum);
}

// Soft PE Sfx_InitVoiceTable @ 0x00550500 size 0x47 (minus DS context):
// for i=0..63: state=0, type=0, priority=1e10, voiceSortIdx[i]=i.
// PE also Sfx_dsContext = sub_55B210() — OOS this TU.
void soft_sfx_init_voice_table() {
  for (int32_t i = 0; i < kSfxVoiceSortCount; ++i) {
    SfxVoiceSlot& s = g_sfx_voice_table[i];
    s.state = 0;
    s.type = 0;
    s.handle = 0;
    s.instance = 0;
    s.ds_voice = 0;
    s.priority = kSfxFreePrioritySentinel;
    std::memset(s.payload, 0, sizeof(s.payload));
    g_sfx_voice_sort_idx[i] = i;
  }
  std::memset(g_sfx_voice_type_use_count, 0,
              sizeof(g_sfx_voice_type_use_count));
  g_sfx_voice_table_inited = true;
}

// Soft PE Sfx_VoiceSortByPriority @ 0x00550B30 size 0x53:
// cmp float priority[23 * *a1] − priority[23 * *a2]; >0 → 1; ==0 → 0; else −1.
int soft_sfx_voice_sort_by_priority(const void* a1, const void* a2) {
  const int32_t ia = *static_cast<const int32_t*>(a1);
  const int32_t ib = *static_cast<const int32_t*>(a2);
  const float pa = g_sfx_voice_table[ia].priority;
  const float pb = g_sfx_voice_table[ib].priority;
  const float d = pa - pb;
  if (d > 0.f) return 1;
  if (d >= 0.f) return 0;
  return -1;
}

// Soft PE Sfx_VoiceTypeMaxCount @ 0x0055B900: type>=0x80 → 0; else
// dword_77E088[type]. Soft stand-in: full table size (no type bank).
int32_t soft_sfx_voice_type_max_count(uint32_t type) {
  if (type >= static_cast<uint32_t>(kSfxVoiceTypeCount)) return 0;
  return kSfxVoiceSortCount;
}

// Soft PE Sfx_VoiceTypeHasBuffers @ 0x0055B920: type<0x80 &&
// dword_782C88[8*type] != 0. Soft stand-in: always capable for valid type.
int32_t soft_sfx_voice_type_has_buffers(uint32_t type) {
  return type < static_cast<uint32_t>(kSfxVoiceTypeCount) ? 1 : 0;
}

// Soft PE Sfx_DSVoiceIsFinished @ 0x55B950 — IDirectSoundBuffer GetStatus
// OOS. Stand-in: non-zero ds_voice ⇒ still active (PE returns 1 when
// buffer live + status bit0); 0 ⇒ treat as finished → free without Stop.
int32_t soft_sfx_ds_voice_is_finished(int32_t ds_voice) {
  return ds_voice != 0 ? 1 : 0;
}

// Soft PE Sfx_DSStopVoice @ 0x0055C1D0 — IDirectSoundBuffer::Stop OOS.
void soft_sfx_ds_stop_voice(int32_t /*ds_voice*/) {}

// Soft PE Sfx_DSAllocVoice @ 0x0055B9A0 size 0x17d (int_convert 381).
// PE: !Sound_dsReady@0x784CAC | type>=0x80 → -1; scan Sound_dsVoiceBank
// @ 0x782C88 / Sound_dsVoiceTypeMax @ 0x77E088; return (type<<16)|slot.
// Soft: no bank/InitDS — packed Soft handle with low16=1 so Soft
// IsFinished(nonzero) keeps state2 (PE type0/slot0 packs to 0 — GAP).
int32_t soft_sfx_ds_alloc_voice(uint32_t type, const int32_t* /*payload*/) {
  if (type >= static_cast<uint32_t>(kSfxVoiceTypeCount)) return -1;
  return (static_cast<int32_t>(type) << 16) | 1;
}

// Soft PE Sfx_DSUpdateVoice @ 0x0055C120 size 0xac (int_convert 172).
// PE: decode handle → bank apply (sub_55BB20) + Play/Stop. Soft: no-op
// return ds_voice (payload already copied by AllocOrUpdate).
int32_t soft_sfx_ds_update_voice(int32_t ds_voice, const int32_t* /*payload*/) {
  return ds_voice;
}

// Soft PE Sfx_CommitListenerDS @ 0x0055B440 size 0x50 (int_convert 80).
// PE: !Sound_dsReady || ctx!=0 → -1; threaded→ReleaseSemaphore else
// IDirectSound3DListener::CommitDeferredSettings. Soft: no DS ctx —
// always -1 (dsReady stand-in 0). Caller UpdateVoices discards ret.
int32_t soft_sfx_commit_listener_ds(int32_t /*ctx*/) { return -1; }

void soft_sfx_free_voice_slot(SfxVoiceSlot& s) {
  s.state = 0;
  s.type = 0;
  s.ds_voice = 0;
  s.priority = kSfxFreePrioritySentinel;
}

// Soft PE UpdateVoices pass1 @ 0x5509B2..0x550A80: walk sort idx, keep
// under budget+type caps or free (state2/3 drop may DSStop — Soft no-op).
bool soft_sfx_pass1_try_keep(SfxVoiceSlot& s, int32_t budget, int32_t& kept) {
  if (kept >= budget) return false;
  const uint32_t ty = static_cast<uint32_t>(s.type);
  if (ty >= static_cast<uint32_t>(kSfxVoiceTypeCount)) return false;
  if (g_sfx_voice_type_use_count[ty] >= soft_sfx_voice_type_max_count(ty))
    return false;
  if (soft_sfx_voice_type_has_buffers(ty) == 0) return false;
  ++g_sfx_voice_type_use_count[ty];
  ++kept;
  return true;
}

void soft_sfx_update_voices_pass1(int32_t budget) {
  int32_t kept = 0;
  for (int32_t i = 0; i < kSfxVoiceSortCount; ++i) {
    SfxVoiceSlot& s = g_sfx_voice_table[g_sfx_voice_sort_idx[i]];
    const int32_t st = s.state;
    if (st == 1) {
      if (!soft_sfx_pass1_try_keep(s, budget, kept)) soft_sfx_free_voice_slot(s);
      continue;
    }
    if (st == 2) {
      if (soft_sfx_ds_voice_is_finished(s.ds_voice) == 0) {
        soft_sfx_free_voice_slot(s);
        continue;
      }
      // fall through — same keep/stop as state 3
    } else if (st != 3) {
      continue;
    }
    // state 2 (still active) or state 3
    if (soft_sfx_pass1_try_keep(s, budget, kept)) continue;
    // Soft PE Sfx_DSStopVoice @ 0x55C1D0
    soft_sfx_ds_stop_voice(s.ds_voice);
    soft_sfx_free_voice_slot(s);
  }
}

// Soft PE UpdateVoices pass2 @ 0x550A86..0x550AF4 (IDA Sfx_UpdateVoices):
// state1 → Sfx_DSAllocVoice @ 0x55B9A0 → state2 (fail → free);
// state2 & payload.flags bit0 → DSStopVoice @ 0x55C1D0 + free;
// state3 → Sfx_DSUpdateVoice @ 0x55C120 → state2.
void soft_sfx_update_voices_pass2() {
  for (int32_t i = 0; i < kSfxVoiceSortCount; ++i) {
    SfxVoiceSlot& s = g_sfx_voice_table[g_sfx_voice_sort_idx[i]];
    if (s.state == 1) {
      const int32_t dv =
          soft_sfx_ds_alloc_voice(static_cast<uint32_t>(s.type), s.payload);
      s.ds_voice = dv;
      if (dv < 0) {
        soft_sfx_free_voice_slot(s);
      } else {
        s.state = 2;  // Soft PE @ 0x550B24
      }
      continue;
    }
    if (s.state == 2) {
      // Soft PE @ 0x550AC9: (payload[0] & 1) == 0 → keep
      if ((s.payload[0] & 1) == 0) continue;
      soft_sfx_ds_stop_voice(s.ds_voice);
      soft_sfx_free_voice_slot(s);
      continue;
    }
    if (s.state == 3) {
      soft_sfx_ds_update_voice(s.ds_voice, s.payload);  // @ 0x55C120
      s.state = 2;  // Soft PE @ 0x550ABE
    }
  }
}

// Soft PE MusicSet_init @ 0x00550BE0 (minus MusicSet_scanFolder / malloc /
// GetTickCount % count). volume=1.0; idx=0; count=1; playlist_pad=1; mode=3.
void soft_music_set_init(SoftMusicSet& s) {
  s.volume = 1.f;
  s.track_idx = 0;
  s.track_count = 1;  // Soft stand-in (PE scan fills count)
  s.playlist_pad = 1;  // Soft stand-in non-null playlist
  s.mode = 3;          // PE *(this+4) = 3 @ 0x550C4E
}

void soft_music_sets_ensure_init() {
  if (g_music_sets_inited) return;
  soft_music_set_init(g_music_set_garage);
  soft_music_set_init(g_music_set_driving);
  soft_music_set_init(g_music_set_race);
  soft_music_set_init(g_music_set_menu);
  g_music_sets_inited = true;
}

SoftMusicSet* soft_music_set_by_type(int32_t type) {
  soft_music_sets_ensure_init();
  switch (type) {
    case 0:
      return &g_music_set_garage;  // Sound_musicSetGarage @ 0x63C878
    case 1:
      return &g_music_set_driving;  // Sound_musicSetDriving @ 0x640908
    case 2:
      return &g_music_set_race;  // Sound_musicSetRace @ 0x6408A8
    case 3:
      return &g_music_set_menu;  // Sound_musicSetMenu @ 0x640890
    default:
      return nullptr;
  }
}

// Soft PE Music_StopPlayback @ 0x0055AF00 — MCI/DS stop OOS.
void soft_music_stop_playback() {}

// Soft PE MusicSet_stop @ 0x00550F30 size 0xd: Music_StopPlayback; return
// [this+4] track_idx.
int32_t soft_music_set_stop(SoftMusicSet& s) {
  soft_music_stop_playback();
  return s.track_idx;
}

// Soft PE MusicSet_play @ 0x00550E50: playlist/count/vol gates; wrap idx;
// walk packed names + MCI (sub_559ED0) + DS vol (sub_55B160) OOS.
int32_t soft_music_set_play(SoftMusicSet& s) {
  if (s.playlist_pad == 0) return -1;
  if (s.track_count <= 0 || s.volume < kSfxMinEffVol) return -1;
  if (s.track_idx < 0) s.track_idx = s.track_count - 1;
  if (s.track_idx >= s.track_count) s.track_idx = 0;
  // Soft: skip packed-string walk + MCI open/play + DS volume apply.
  return s.track_idx;
}

// Soft PE MusicSet_applyVolume @ 0x00550FE0: store *this=a2; <0.01 → 0 +
// stop; else if was silent → play; then DS vol OOS. Returns prior volume
// (fstp discarded by Java setVolume / changeMusicSet).
float soft_music_set_apply_volume(SoftMusicSet& s, float volume) {
  const float prior = s.volume;
  s.volume = volume;
  if (volume < kSfxMinEffVol) {
    s.volume = 0.f;
    soft_music_stop_playback();
    return prior;
  }
  if (prior < kSfxMinEffVol) soft_music_set_play(s);
  // Soft: sub_55B160 DS/MCI volume OOS.
  return prior;
}

// Soft PE MusicSet_nextTrack body @ 0x00550F40: stop; ++[this+4]; play.
int32_t soft_music_set_next_track(SoftMusicSet& s) {
  soft_music_set_stop(s);
  ++s.track_idx;
  soft_music_set_play(s);
  return s.track_idx;
}

// Soft PE MusicSet_prevTrack body @ 0x00550F60: stop; --[this+4]; play.
int32_t soft_music_set_prev_track(SoftMusicSet& s) {
  soft_music_set_stop(s);
  --s.track_idx;
  soft_music_set_play(s);
  return s.track_idx;
}

// PE play-params staging @ dword_640940 size 0x3C (Sfx_PlayWithListenerCull
// writes before AllocOrUpdateVoice). +0x10 unused (Copy skips a1[4]).
// Soft BSS stand-in — no PE absolute VA wire.
struct SoftSfxPlayParams {
  int32_t flags;       // +0x00 dword_640940
  float volume;        // +0x04 flt_640944
  int32_t pad08;       // +0x08 dword_640948 (=0)
  float pitch;         // +0x0C dword_64094C
  int32_t unused10;    // +0x10 skipped by CopyVoicePayload
  float x, y, z;       // +0x14..0x1C dword_640954/958/95C
  float vx, vy, vz;    // +0x20..0x28 dword_640960/964/968 (a8)
  float ox, oy, oz;    // +0x2C..0x34 dword_64096C/970/974 (a9)
  float radius;        // +0x38 dword_640978
};
static_assert(sizeof(SoftSfxPlayParams) == 0x3C);
static_assert(offsetof(SoftSfxPlayParams, radius) == 0x38);
SoftSfxPlayParams g_sfx_play_params{};  // Soft stand-in dword_640940

// Soft PE Sfx_CopyVoicePayload @ 0x00550810 size 0x5B: play-params →
// slot.payload[17] with PE skips (dst[7]/[11]/[15] untouched / src[4]
// unused). IDA: a2[0..3]=a1[0..3]; a2[4..6]=a1[5..7]; a2[8..10]=a1[8..10];
// a2[12..14]=a1[11..13]; a2[16]=a1[14].
void soft_sfx_copy_voice_payload(SfxVoiceSlot& s, const SoftSfxPlayParams& p) {
  const int32_t* a1 = reinterpret_cast<const int32_t*>(&p);
  int32_t* a2 = s.payload;
  a2[0] = a1[0];
  a2[1] = a1[1];
  a2[2] = a1[2];
  a2[3] = a1[3];
  a2[4] = a1[5];
  a2[5] = a1[6];
  a2[6] = a1[7];
  // a2[7] PE skip
  a2[8] = a1[8];
  a2[9] = a1[9];
  a2[10] = a1[10];
  // a2[11] PE skip
  a2[12] = a1[11];
  a2[13] = a1[12];
  a2[14] = a1[13];
  // a2[15] PE skip
  a2[16] = a1[14];
}

// Soft PE Sfx_AllocOrUpdateVoice @ 0x005505E0 size 0x21F. a4=&play_params.
// !(flags&4): vol gate *(a4+4)<=dbl_5F10D0(0.01)→-1; 2nd-pass dist cull
// (same 62500/16 as Sfx_3DListenerCull); priority=metric*(1/(vol*vol)).
// flags&4 (2D): priority=0. instance==0 → first free state1; else match
// handle+instance (state1 refresh / state2|3→state3) or first free.
// PE returns 0/-1; Soft returns slot+1 / -1 (host smoke voice>0) — GAP.
// Soft: pass2 DSAlloc promotes state1→2 (packed Soft handle).
int32_t soft_sfx_alloc_or_update_voice(int32_t type, int32_t instance,
                                      int32_t handle,
                                      const SoftSfxPlayParams& params) {
  if (!g_sfx_voice_table_inited) soft_sfx_init_voice_table();

  float priority = 0.f;
  if ((params.flags & 4) == 0) {
    // PE @ 0x5505FA: fcomp dbl_5F10D0 (0.01) — same gate as flt_5F0C20
    if (params.volume <= kSfxMinEffVol) return -1;
    const float dx = params.x - g_sfx_listener_x;
    const float dy = params.y - g_sfx_listener_y;
    const float dz = params.z - g_sfx_listener_z;
    float metric = dx * dx + dy * dy + dz * dz;
    // PE @ 0x55060E: radius fcomp 0 → plain dist²; else dist²−r²
    if (params.radius > 0.f) {
      metric -= params.radius * params.radius;
      // PE @ 0x55065A: metric < 16 → clamp metric=16 (not early ret 0)
      if (metric < kSfxCullNearDist2) metric = kSfxCullNearDist2;
    }
    if (metric > kSfxCullMaxDist2) return -1;
    // PE @ 0x55066F: priority = metric * (1.0 / (vol*vol))
    const float vol = params.volume;
    priority = metric * (1.f / (vol * vol));
  }

  auto fill_new = [&](SfxVoiceSlot& s) {
    s.state = 1;
    s.type = type;
    s.handle = handle;
    s.instance = instance;
    s.ds_voice = 0;
    s.priority = priority;
    soft_sfx_copy_voice_payload(s, params);
  };

  if (instance != 0) {
    int32_t free_slot = -1;
    for (int32_t i = 0; i < kSfxVoiceSortCount; ++i) {
      SfxVoiceSlot& s = g_sfx_voice_table[i];
      if (s.state == 0) {
        if (free_slot < 0) free_slot = i;
        continue;
      }
      if (s.instance != instance || s.handle != handle) continue;
      if (s.state == 1) {
        s.priority = priority;
        soft_sfx_copy_voice_payload(s, params);
        return i + 1;  // Soft host id; PE Alloc returns 0
      }
      // state 2 or 3 → mark update
      s.state = 3;
      s.priority = priority;
      soft_sfx_copy_voice_payload(s, params);
      return i + 1;
    }
    if (free_slot < 0) return -1;
    fill_new(g_sfx_voice_table[free_slot]);
    return free_slot + 1;
  }

  // instance == 0: first free slot
  for (int32_t i = 0; i < kSfxVoiceSortCount; ++i) {
    SfxVoiceSlot& s = g_sfx_voice_table[i];
    if (s.state != 0) continue;
    fill_new(s);
    s.instance = 0;
    return i + 1;
  }
  return -1;
}

// Soft PE Sfx_stopVoiceInstance @ 0x00550870 size 0x72: walk voiceTable
// match handle+instance state 1..3 → DSStop (Soft no-op) + free slot
// (state=0, type=0, priority=1e10). Neighbor of AllocOrUpdateVoice;
// sole PE caller SfxRef_stopInstance @ 0x48D180 (Resources stop path —
// host still audio_sfx_stop; Soft table helper kept for PE deepen).
[[maybe_unused]] int32_t soft_sfx_stop_voice_instance(int32_t instance,
                                                     int32_t handle) {
  if (!g_sfx_voice_table_inited) return -1;
  for (int32_t i = 0; i < kSfxVoiceSortCount; ++i) {
    SfxVoiceSlot& s = g_sfx_voice_table[i];
    if (s.state <= 0 || s.state > 3) continue;
    if (s.handle != handle || s.instance != instance) continue;
    // Soft PE Sfx_DSStopVoice @ 0x55C1D0
    soft_sfx_ds_stop_voice(s.ds_voice);
    soft_sfx_free_voice_slot(s);
    return 0;
  }
  return -1;
}

// Soft PE Sfx_PlayWithListenerCull @ 0x0048CFB0 size 0x1C0 (thiscall
// handle). a8/a9 always 0 from nplay. Soft: no ResHandle_getPayload /
// inner+0x54 / type@+0x48 — inner_vol stand-in 1.0 when rid loaded.
// Success: PE returns instance (esi); Soft returns Alloc slot+1 — GAP.
int32_t soft_sfx_play_with_listener_cull(int32_t /*type_standin*/,
                                         int32_t handle, float pitch,
                                         float volume, int32_t flags,
                                         const float* pos_xyz, int32_t instance,
                                         float radius) {
  int32_t play_flags = flags;
  float x = 0.f, y = 0.f, z = 0.f;
  if (pos_xyz != nullptr) {
    // PE @ 0x48CFC6: Sfx_3DListenerCull < 0 → -1
    if (sfx_3d_listener_cull(pos_xyz[0], pos_xyz[1], pos_xyz[2], radius) < 0)
      return -1;
    x = pos_xyz[0];
    y = pos_xyz[1];
    z = pos_xyz[2];
  } else {
    // PE @ 0x48D0AA: a5==0 → flags |= 4
    play_flags |= 4;
  }

  // PE @ 0x48D047: inner_vol = *(payload+0x54); null → 0. Soft stand-in 1.0
  constexpr float kInnerVolStandIn = 1.f;
  const float eff = kInnerVolStandIn * volume;
  if (eff <= kSfxMinEffVol) return -1;

  SoftSfxPlayParams& p = g_sfx_play_params;
  p.flags = play_flags;
  p.volume = eff;
  p.pad08 = 0;
  p.pitch = pitch;
  p.unused10 = 0;
  p.x = x;
  p.y = y;
  p.z = z;
  p.vx = p.vy = p.vz = 0.f;  // nplay a8=0
  p.ox = p.oy = p.oz = 0.f;  // nplay a9=0
  p.radius = radius;

  // PE type = *(inner+0x48); Soft stand-in 0 (ResHandle OOS)
  const int32_t voice =
      soft_sfx_alloc_or_update_voice(0, instance, handle, p);
  if (voice < 0) return -1;
  // PE returns instance (a6) incl. 0; Soft returns slot+1
  return voice;
}
}

// PE @ 0x005508F0 size 0x85 (133) — Sfx_ListenerSetPose: copies 12 floats
// (pos + orient o/u/f) into Sfx_listener_* then Sfx_ApplyListenerToDS
// @ 0x0055B260 (a1=Sfx_dsContext @ 0x768764, a2=&Sfx_listener_x).
// Host Soft PE: xyz for Sfx_3DListenerCull; orient BSS stay 0 (no matrix
// axes from System.cpp); ApplyListenerToDS / IDirectSound3DListener OOS.
// Caller: system_mainloop_sfx_listener_update (PE MainLoop @ 0x428CB6).
void sfx_listener_set_pos(float x, float y, float z) {
  g_sfx_listener_x = x;
  g_sfx_listener_y = y;
  g_sfx_listener_z = z;
  // Soft PE: orient BSS (ox..fz) not written — PE MainLoop fills 12 floats
  // from Engine_CameraMatrix before SetPose; host extracts eye xyz only.
}

// PE @ 0x00550980 size 0x1AC (428) — Sfx_UpdateVoices. Sole MainLoop xref
// @ 0x00428CBE immediately after Sfx_ListenerSetPose. Soft PE sound_host:
// budget + qsort + typeUse memset + pass1 keep/drop + pass2 Soft DS*
// stand-ins + CommitListenerDS (-1). Ticket seed VA 0x00480800 was mid
// PhysicsRef_create @ 0x4807F0 — Soft gate retargeted to this MainLoop
// audio stage (java.sound.* already Soft; nplay @ 0x480D40 Soft).
// PE body (IDA renames):
//   1) budget = Sfx_HwVoiceBudget @ 0x550985
//   2) qsort Sfx_voiceSortIdx[64] @ 0x768968 via Sfx_VoiceSortByPriority
//      @ 0x00550B30 (cmp Sfx_voicePriority @ 0x768A7C stride 23)
//   3) memset Sfx_voiceTypeUseCount[0x80] @ 0x768768
//   4) pass1 @ 0x5509B2..0x550A80 over sort idx → Sfx_voiceTable @ 0x768A68
//      state 0/other: skip; state 1 (pending): keep under budget+type caps
//      else free (state=0, type=0, priority=kSfxFreePrioritySentinel);
//      state 2 playing: if Sfx_DSVoiceIsFinished @ 0x55B950 == 0 → free;
//      else / state 3: keep under caps else DSStopVoice @ 0x55C1D0 + free
//   5) pass2 @ 0x550A86..0x550AF4: state1 → Sfx_DSAllocVoice @ 0x55B9A0
//      → state 2 (fail → free); state2 & flags.bit0 → stop+free;
//      state3 → Sfx_DSUpdateVoice @ 0x55C120 → state 2
//   6) Sfx_CommitListenerDS(Sfx_dsContext @ 0x768764) @ 0x55B440; return 0
// Host Soft: voice table BSS + InitVoiceTable; type max/hasBuffers
// stand-in; Soft packed DSAlloc (no bank); Commit → -1.
// Cull still uses listener xyz only. Cluster sfx_3d_cull OK.
void sfx_update_voices() {
  if (!g_sfx_voice_table_inited) soft_sfx_init_voice_table();
  // Soft PE @ 0x550985 — Sfx_HwVoiceBudget
  g_sfx_voice_hw_budget = soft_sfx_hw_voice_budget();
  // Soft PE @ 0x55099C — qsort(voiceSortIdx, 64, 4, VoiceSortByPriority)
  std::qsort(g_sfx_voice_sort_idx, static_cast<size_t>(kSfxVoiceSortCount),
             sizeof(int32_t), soft_sfx_voice_sort_by_priority);
  // Soft PE @ 0x5509A1..0x5509B0 — stosd 0x80 dwords typeUseCount
  std::memset(g_sfx_voice_type_use_count, 0, sizeof(g_sfx_voice_type_use_count));
  // Soft PE @ 0x5509B2..0x550A80 — pass1 keep/drop
  soft_sfx_update_voices_pass1(g_sfx_voice_hw_budget);
  // Soft PE @ 0x550A86..0x550AF4 — pass2 DSAlloc/Update/Stop
  soft_sfx_update_voices_pass2();
  // Soft PE @ 0x550AFD — Sfx_CommitListenerDS(Sfx_dsContext); ret discarded
  soft_sfx_commit_listener_ds(0);  // Soft ctx stand-in 0; dsReady→-1
}
// PE @ 0x00550560 size 0x7a — IDA Sfx_3DListenerCull (renamed from
// sub_550560). Callees: none. Xrefs: Sfx_PlayWithListenerCull @ 0x48CFC6
// (nplay path Soft soft_sfx_play_with_listener_cull), sub_48F0E0,
// sub_4518C0. Returns 0 = play, -1 = culled. Same 62500/16 reused by
// AllocOrUpdateVoice 2nd-pass cull @ 0x5505E0.
int32_t sfx_3d_listener_cull(float x, float y, float z, float radius) {
  const float dx = x - g_sfx_listener_x;
  const float dy = y - g_sfx_listener_y;
  const float dz = z - g_sfx_listener_z;
  const float dist2 = dx * dx + dy * dy + dz * dz;
  float metric;
  if (radius <= 0.f) {
    // PE @ 0x550564 fcomp 0.0 / test ah,41h → loc_5505BE: metric = dist²
    metric = dist2;
  } else {
    // PE @ 0x5505A9: metric = dist² − radius²; < 16.0 → return 0
    metric = dist2 - radius * radius;
    if (metric < kSfxCullNearDist2) return 0;
  }
  // PE @ 0x5505C4 fcomp flt_5F3AA0: metric > 62500 → -1 else 0
  if (metric > kSfxCullMaxDist2) return -1;
  return 0;
}

// PE @ 0x00480D40 size 0x120 — java.util.resource.SfxRef.nplay
// (Ljava.lang.Vector3;FFFII)I. Cluster sfx_3d_cull.
// UnboxArg @ 0x0045D910: this, pos, radius, pitch, volume, flags, instance.
// Native.ptr dword_62E008 → 0: Mighty ERROR + return -1.
// pos==null: skip xyz; neg/sbb → pos_or_0=0 → Sfx_PlayWithListenerCull
// flags|=4 (2D). pos!=null: read Vector3 x/y/z; pos_or_0=&stack_xyz.
// volume *= Sound_volumeEffects @ 0x612C58 (1.0 BSS).
// Type low16 [handle+8] in [0x26,0x2C): reload eax=pos — dead vs pos_or_0
// (IDA @ 0x480DD3; Soft no-op). thiscall Sfx_PlayWithListenerCull @
// 0x0048CFB0: cull → ResHandle payload+0x54 vol gate → AllocOrUpdateVoice;
// return instance (esi). Soft: soft_sfx_play_with_listener_cull (inner+0x54
// stand-in 1.0; return slot+1). Host: !self / id==0 → -1 (no Mighty).
// No audio_win32 / DS mixer. GAPS: Mighty; ResHandle+0x54/+0x48; return
// instance (esi). Cluster sfx_3d_cull OK.
int32_t java_util_resource_SfxRef_nplay(InvObject* self, InvObject* pos,
                                        float radius, float pitch, float volume,
                                        int32_t flags, int32_t instance) {
  // PE @ 0x00480D40 size 0x120 (288) — cluster sfx_3d_cull.
  if (!self) return -1;
  const int32_t rid = java_util_resource_ResourceRef_id(self);
  if (rid == 0) return -1;  // Soft: Native.ptr==0 → Mighty GAP

  float xyz[3] = {};
  const float* pos_or_0 = nullptr;
  if (pos) {
    xyz[0] = tree_field_get_float(pos, "x");
    xyz[1] = tree_field_get_float(pos, "y");
    xyz[2] = tree_field_get_float(pos, "z");
    pos_or_0 = xyz;  // PE neg/sbb/and &stack_xyz @ 0x480DF6
  }
  // PE: volume *= Sound_volumeEffects (flt_612C58)
  const float vol = volume * g_sound_volumeEffects;
  // Soft PE Sfx_PlayWithListenerCull @ 0x48CFB0 — handle=rid stand-in
  return soft_sfx_play_with_listener_cull(0, rid, pitch, vol, flags, pos_or_0,
                                          instance, radius);
}

// PE @ 0x00487460 size 0x81 (int_convert 129). STATIC (I)V. UnboxArg
// dest0=&var_4 dummy this unread, dest1=&arg_0 type (overwrites
// CallInfo). After unbox: cmp eax,3 / ja default (Hex-Rays switch(a1)
// is a lie). case 0..3: esi=&Sound_musicSetGarage @ 0x63C878
// (Music\Garage_Shop) / Sound_musicSetDriving @ 0x640908 (Roam_Ride) /
// Sound_musicSetRace @ 0x6408A8 (Race_Chase) / Sound_musicSetMenu @
// 0x640890 (Main_Menu). default xor esi,esi.
// cmp esi,Sound_currentMusicSet @ 0x64091C / jz ret. Else if ecx!=0:
// thiscall MusicSet_stop @ 0x550F30. mov Sound_currentMusicSet,esi.
// If esi: thiscall MusicSet_play @ 0x550E50; push Sound_volumeMusic @
// 0x612C5C; thiscall MusicSet_applyVolume @ 0x550FE0; fstp st. VOID.
// Soft PE: SoftMusicSet* stand-ins; MCI/DS OOS inside play/applyVolume.
void java_sound_Sound_changeMusicSet(int32_t type) {
  // PE @ 0x00487460
  SoftMusicSet* next = soft_music_set_by_type(type);  // cmp/ja → esi; else 0..3
  if (next == g_sound_current_music_set)  // cmp esi,Sound_currentMusicSet
    return;
  if (g_sound_current_music_set != nullptr)  // test ecx / jz
    soft_music_set_stop(*g_sound_current_music_set);  // MusicSet_stop @ 0x550F30
  g_sound_current_music_set = next;  // mov Sound_currentMusicSet,esi
  if (next != nullptr) {
    soft_music_set_play(*next);  // MusicSet_play @ 0x550E50
    soft_music_set_apply_volume(*next, g_sound_volumeMusic);  // @ 0x550FE0
  }
}
// PE @ 0x00487500 size 0x10 (int_convert 16). STATIC ()V, no UnboxArg,
// no this. Head: mov ecx,Sound_currentMusicSet @ 0x64091C / test /
// jz locret_48750F / jmp MusicSet_nextTrack body @ 0x550F40. Tail: push
// esi; mov esi,ecx; call Music_StopPlayback @ 0x55AF00; mov eax,[esi+4];
// inc eax; mov ecx,esi; mov [esi+4],eax; thiscall MusicSet_play @
// 0x550E50; mov eax,[esi+4]; pop; ret. Wrap [this+4]>=[this+8]→0 is
// inside MusicSet_play, not thunk. VOID Java (eax discarded). Contrast
// prevTrack @ 0x00487510: MusicSet_prevTrack @ 0x550F60, dec not inc.
// Soft PE: no audio_win32.
void java_sound_Sound_nextTrack() {
  // PE @ 0x00487500
  if (g_sound_current_music_set == nullptr) return;
  soft_music_set_next_track(*g_sound_current_music_set);
}
// PE @ 0x00487510 size 0x10 (int_convert 16). STATIC ()V, no UnboxArg.
// ecx=Sound_currentMusicSet @ 0x64091C; jz ret; jmp MusicSet_prevTrack
// @ 0x550F60: Music_StopPlayback, --[esi+4], MusicSet_play @
// 0x550E50 (wrap <0 → count-1 inside play). VOID Java. Contrast
// nextTrack @ 0x00487500: MusicSet_nextTrack + inc; wrap >=count → 0.
// Soft PE: no audio_win32.
void java_sound_Sound_prevTrack() {
  // PE @ 0x00487510
  if (g_sound_current_music_set == nullptr) return;
  soft_music_set_prev_track(*g_sound_current_music_set);
}
// PE @ 0x00487580 size 0x6c (int_convert 108). STATIC (IF)V. UnboxArg
// dest0=&var_4 dummy this (unread). dest1=&var_8 channel. dest2=&arg_0
// volume. Sole callee JVM_UnboxArg @ 0x0045D910 (+ MusicSet_applyVolume
// on ch1). NO 0..1 clamp (no 1.0f / fcomp; Java increase/decreaseVolume
// only). ch 0 store Sound_volumeEffects @ 0x612C58; ch 1 store
// Sound_volumeMusic @ 0x612C5C then if Sound_currentMusicSet @
// 0x64091C!=0 thiscall MusicSet_applyVolume @ 0x550FE0 + fstp; ch 2
// store Sound_volumeEngine @ 0x612C60; else no store. VOID.
// Soft PE: BSS triad unclamped; Soft applyVolume (no MCI/audio_win32).
void java_sound_Sound_setVolume(int32_t channel, float volume) {
  // PE @ 0x00487580
  if (channel == 0) {
    g_sound_volumeEffects = volume;
  } else if (channel == 1) {
    g_sound_volumeMusic = volume;
    if (g_sound_current_music_set != nullptr)
      soft_music_set_apply_volume(*g_sound_current_music_set, volume);
  } else if (channel == 2) {
    g_sound_volumeEngine = volume;
  }
}
// PE @ 0x004875F0 size 0x47 (int_convert 71). STATIC (I)F. UnboxArg
// dest0=&var_4 dummy this unread, dest1=&arg_0 channel (overwrites
// CallInfo). Sole callee JVM_UnboxArg @ 0x0045D910. After unbox:
// mov eax,[esp+arg_0]; sub eax,0 / jz ch0; dec/jz ch1; dec/jz ch2;
// else fld flt_5E73CC (bytes 00 00 00 00 = 0.0). ch0 fld
// Sound_volumeEffects @ 0x612C58; ch1 Sound_volumeMusic @ 0x612C5C;
// ch2 Sound_volumeEngine @ 0x612C60 (Java CHANNEL_EFFECTS/MUSIC/
// ENGINE). Same BSS triad as setVolume @ 0x00487580 (init
// 0x3F800000 = 1.0). NO clamp / no Mighty. Host: g_sound_volume*
// (ch 0/1/2 else 0.f) — not audio_get_volume (clamped backend).
float java_sound_Sound_getVolume(int32_t channel) {
  // PE @ 0x004875F0
  if (channel == 0) return g_sound_volumeEffects;
  if (channel == 1) return g_sound_volumeMusic;
  if (channel == 2) return g_sound_volumeEngine;
  return 0.f;
}
// PE @ 0x00487F00 size 0x17 (int_convert 23). STATIC
// (Ljava.util.resource.ResourceRef;)I. UnboxArg dest0=nullptr (static
// skip this), dest1=&arg_0 (overwrites CallInfo with unboxed ResourceRef
// DWORD). Value unread. xor eax,eax → return 0. Sole callee JVM_UnboxArg
// @ 0x0045D910. 1 xref data: Natives_RegisterAll. Java Sound.init uses
// as bool — stock never enters debug Viewport/Camera path. Host:
// discard display, return 0.
int32_t java_sound_Sound_enableDebugDump(InvObject* display) {
  // PE @ 0x00487F00
  (void)display;
  return 0;
}
// PE @ 0x00487560 size 0x11. STATIC ()I: no UnboxArg, no this, no callees,
// no Mighty ERROR. Bytes: 8B 0D 1C 09 64 00 / 83 C8 FF / 85 C9 / 74 03 /
// 8B 41 10 / C3. ecx=Sound_currentMusicSet @ 0x64091C (BSS 0). or eax,-1
// @ 0x00487566; jz → -1. Else eax=[ecx+0x10] — same DWORD setMode stores
// @ 0x0048754e (signed 0..3 jl/jg). Soft PE: SoftMusicSet::mode (init=3).
int32_t java_sound_Sound_getMode() {
  // PE @ 0x00487560
  if (g_sound_current_music_set == nullptr)  // test ecx / jz
    return -1;                               // or eax,-1 @ 0x487566
  return g_sound_current_music_set->mode;    // mov eax,[ecx+10h] @ 0x48756d
}
// PE @ 0x00487520 size 0x3c (int_convert 60). STATIC (I)I. UnboxArg
// dest0=&var_4 dummy this unread, dest1=&arg_0 mode. Sole callee
// JVM_UnboxArg @ 0x0045D910. eax=Sound_currentMusicSet @ 0x64091C;
// test eax / jz loc_487556 @ 0x48753f → return unboxed arg (no store).
// Else ecx=mode: test ecx / jl loc_487551; cmp ecx,3 / jg loc_487551;
// mov [eax+0x10],ecx @ 0x48754e (signed 0..3; Hex-Rays a1<4 is a lie —
// bytes 0x7C jl / 0x7F jg). loc_487551: return [eax+0x10]. Soft PE:
// SoftMusicSet::mode.
int32_t java_sound_Sound_setMode(int32_t mode) {
  // PE @ 0x00487520
  SoftMusicSet* cur = g_sound_current_music_set;  // mov eax,Sound_currentMusicSet
  if (cur == nullptr)                             // test eax / jz loc_487556
    return mode;                                  // mov eax,[esp+arg_0]
  if (mode >= 0 && mode <= 3)                     // jl/jg skip @ 0x487547/4c
    cur->mode = mode;                             // mov [eax+10h],ecx
  return cur->mode;                               // mov eax,[eax+10h]
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
