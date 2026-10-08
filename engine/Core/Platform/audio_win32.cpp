#include "audio_win32.hpp"
#include "rpak.hpp"
#include "render_d3d9.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#define DIRECTSOUND_VERSION 0x0800
#include <windows.h>
#include <mmsystem.h>
#include <dsound.h>
#endif

namespace inv {
namespace {

std::mutex g_mu;
float g_vol[3] = {1.f, 1.f, 1.f};
int32_t g_music_set = -1;  // Sound.MUSIC_SET_NONE
int32_t g_next_voice = 1;
std::string g_last_path;
bool g_ds_tried = false;
bool g_ds_ok = false;
std::vector<std::string> g_music_tracks;  // absolute/resolved paths
int32_t g_music_index = 0;
bool g_music_playing = false;
constexpr const char* kMciAlias = "slrr_bgm";

#ifdef _WIN32
IDirectSound8* g_ds = nullptr;
// Soft PE Sound_primaryBuffer @ 0x7A0440 — InitDS CreateSoundBuffer flags=17.
IDirectSoundBuffer* g_ds_primary = nullptr;
// Soft PE Sound_ds3DListener @ 0x7A0230 — QI IID @ 0x60333C on primary.
IDirectSound3DListener* g_ds3d_listener = nullptr;
#endif
// Soft PE Sound_dsReady @ 0x784CAC (InitDS sets 1 after GetCaps+primary+QI).
bool g_sound_ds_ready = false;
// Soft PE Sound_hw3DCapable @ 0x784CA4 / Sound_hwMixCapable @ 0x784CA8 —
// InitDS: FreeHw*AllBuffers >= 4. Sound.cpp keeps its own budget stand-ins.
uint32_t g_sound_hw3d_capable = 0;
uint32_t g_sound_hwmix_capable = 0;
// Soft PE Sound_dscaps fields @ 0x77C980 (DSCAPS dwSize=96) after GetCaps.
uint32_t g_dscaps_free_hw_mixing_all = 0;     // +0x20 @ 0x77C9A0
uint32_t g_dscaps_free_hw_mixing_static = 0;  // +0x24 @ 0x77C9A4
uint32_t g_dscaps_free_hw3d_all = 0;          // +0x38 @ 0x77C9B8
uint32_t g_dscaps_free_hw3d_static = 0;       // +0x3C @ 0x77C9BC
// Soft PE ApplyListener software mirrors (PE dword_77C960 / 77E078 / …).
float g_ds_listener_pos[4]{};
float g_ds_listener_vel[4]{};
float g_ds_listener_front[4]{};  // pose ux → DS OrientFront
float g_ds_listener_top[4]{};    // pose fx → DS OrientTop
float g_ds_listener_right[3]{};  // front × top (PE flt_77E490)
// Soft PE Sound_dsVoiceBank[0x80*8] @ 0x782C88 — InitDS memset 0x1000; DSAlloc
// fills slots. Soft empty bank until type buffers registered.
#ifdef _WIN32
IDirectSoundBuffer* g_ds_voice_bank[0x80][8]{};
#endif
// Soft PE Sound_dsVoiceTypeMax[0x80] @ 0x77E088 — type max (InitDS 0).
int32_t g_ds_voice_type_max[0x80]{};
// Soft PE dword_783C88[0x80*8] — per-slot flags written by DSAllocVoice.
int32_t g_ds_voice_slot_flags[0x80][8]{};
// Soft PE byte_77DBE0[0x80] — type is-3D gate (InitDS 0).
uint8_t g_ds_voice_type_is3d[0x80]{};
// Soft PE Sfx_listener_* @ 0x768720 — 4×Vector4 (16 floats); pads at
// +0xC/+0x1C/+0x2C/+0x3C. Writer Sfx_ListenerSetPose @ 0x5508F0.
float g_sfx_listener_pose[16]{};
// Soft PE Sfx_dsContext @ 0x768764 — Apply/Commit gate arg (0 = ok).
constexpr int32_t kSfxDsContext = 0;
// Soft PE Sfx_voiceTable[64] @ 0x768A68 stride 23 dwords / Sfx_voiceSortIdx
// @ 0x768968 / Sfx_voiceTypeUseCount[0x80] @ 0x768768 — UpdateVoices.
constexpr int32_t kSfxVoiceSortCount = 64;
constexpr int32_t kSfxVoiceTypeCount = 0x80;
constexpr float kSfxFreePrioritySentinel = 1e10f;  // PE 0x501502F9
constexpr int32_t kSfxHwVoiceBudgetMin = 4;
struct SoftSfxVoiceSlot {
  int32_t state;     // +0x00: 0 free / 1 pending / 2 playing / 3 update
  int32_t type;      // +0x04
  int32_t handle;    // +0x08
  int32_t instance;  // +0x0C
  int32_t ds_voice;  // +0x10 packed (type<<16)|slot
  float priority;    // +0x14 Sfx_voicePriority @ 0x768A7C
  int32_t payload[17];  // +0x18; flags @ [0] pass2 bit0
};
static_assert(sizeof(SoftSfxVoiceSlot) == 92);  // PE stride 23 dwords
static_assert(kSfxVoiceSortCount == 64);
SoftSfxVoiceSlot g_sfx_voice_table[kSfxVoiceSortCount]{};
int32_t g_sfx_voice_sort_idx[kSfxVoiceSortCount]{};
int32_t g_sfx_voice_type_use_count[kSfxVoiceTypeCount]{};
bool g_sfx_voice_table_inited = false;

struct Voice {
  int32_t res_id = 0;
  int32_t flags = 0;
  float pitch = 1.f;
  float volume = 1.f;
  std::string path;
  bool alive = true;
  bool ds = false;
#ifdef _WIN32
  IDirectSoundBuffer* buf = nullptr;
#endif
};

std::unordered_map<int32_t, Voice> g_voices;
// Soft PE AsyncLoad_Type5 → Sound_RegisterVoiceType type id per res.
std::unordered_map<int32_t, int32_t> g_res_to_voice_type;

float clamp01(float v) {
  if (v < 0.f) return 0.f;
  if (v > 1.f) return 1.f;
  return v;
}

std::string parse_sourcefile(const std::vector<uint8_t>& blob) {
  if (blob.size() < 11) return {};
  const char* s = reinterpret_cast<const char*>(blob.data());
  const size_t n = blob.size();
  const char* k = "sourcefile";
  const size_t klen = 10;
  for (size_t i = 0; i + klen < n; ++i) {
    if (std::memcmp(s + i, k, klen) != 0) continue;
    size_t p = i + klen;
    while (p < n && (s[p] == ' ' || s[p] == '\t')) ++p;
    size_t e = p;
    while (e < n && s[e] != '\r' && s[e] != '\n' && s[e] != '\0') ++e;
    if (e > p) {
      std::string path(s + p, s + e);
      for (char& c : path)
        if (c == '\\') c = '/';
      return path;
    }
  }
  return {};
}

bool file_exists(const std::string& path) {
  if (path.empty()) return false;
#ifdef _WIN32
  const DWORD a = GetFileAttributesA(path.c_str());
  return a != INVALID_FILE_ATTRIBUTES &&
         (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
#else
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  std::fclose(f);
  return true;
#endif
}

std::string try_resolve(const std::string& rel) {
  if (rel.empty()) return {};
  std::string p = rpak_resolve_path(rel.c_str());
  if (p.empty()) p = rel;
  if (file_exists(p)) return p;
  return {};
}

bool load_file(const std::string& path, std::vector<uint8_t>* out) {
  out->clear();
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  std::fseek(f, 0, SEEK_END);
  const long sz = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (sz <= 0) {
    std::fclose(f);
    return false;
  }
  out->resize(static_cast<size_t>(sz));
  const size_t n = std::fread(out->data(), 1, out->size(), f);
  std::fclose(f);
  return n == out->size();
}

struct WavInfo {
  WAVEFORMATEX fmt{};
  const uint8_t* data = nullptr;
  uint32_t data_size = 0;
};

bool parse_wav(const std::vector<uint8_t>& file, WavInfo* out) {
  if (file.size() < 44) return false;
  if (std::memcmp(file.data(), "RIFF", 4) != 0) return false;
  if (std::memcmp(file.data() + 8, "WAVE", 4) != 0) return false;
  size_t p = 12;
  const uint8_t* fmt_chunk = nullptr;
  uint32_t fmt_sz = 0;
  const uint8_t* data_chunk = nullptr;
  uint32_t data_sz = 0;
  while (p + 8 <= file.size()) {
    const char* id = reinterpret_cast<const char*>(file.data() + p);
    uint32_t sz = 0;
    std::memcpy(&sz, file.data() + p + 4, 4);
    p += 8;
    if (p + sz > file.size()) return false;
    if (std::memcmp(id, "fmt ", 4) == 0) {
      fmt_chunk = file.data() + p;
      fmt_sz = sz;
    } else if (std::memcmp(id, "data", 4) == 0) {
      data_chunk = file.data() + p;
      data_sz = sz;
    }
    p += sz + (sz & 1u);  // word align
  }
  if (!fmt_chunk || fmt_sz < 16 || !data_chunk || data_sz == 0) return false;
  std::memset(&out->fmt, 0, sizeof(out->fmt));
  uint16_t audio_fmt = 0, channels = 0, bits = 0;
  uint32_t rate = 0, byte_rate = 0;
  uint16_t block_align = 0;
  std::memcpy(&audio_fmt, fmt_chunk + 0, 2);
  std::memcpy(&channels, fmt_chunk + 2, 2);
  std::memcpy(&rate, fmt_chunk + 4, 4);
  std::memcpy(&byte_rate, fmt_chunk + 8, 4);
  std::memcpy(&block_align, fmt_chunk + 12, 2);
  std::memcpy(&bits, fmt_chunk + 14, 2);
  if (audio_fmt != 1) return false;  // PCM only
  out->fmt.wFormatTag = WAVE_FORMAT_PCM;
  out->fmt.nChannels = channels;
  out->fmt.nSamplesPerSec = rate;
  out->fmt.nAvgBytesPerSec = byte_rate;
  out->fmt.nBlockAlign = block_align;
  out->fmt.wBitsPerSample = bits;
  out->fmt.cbSize = 0;
  out->data = data_chunk;
  out->data_size = data_sz;
  return true;
}

#ifdef _WIN32
// Soft PE Sound_InitDirectSound GetCaps residual @ 0x00559934..0x0055994B
// (full InitDS @ 0x005598C0). PE: Sound_dscaps=96; GetCaps; fail→Release/-2.
// hw3D/hwMix = Free*AllBuffers >= 4 @ 0x00559B94..0x00559BAC (after primary).
bool soft_init_ds_getcaps() {
  if (!g_ds) return false;
  DSCAPS caps{};
  caps.dwSize = 96;  // PE Sound_dscaps @ 0x77C980
  const HRESULT hr = g_ds->GetCaps(&caps);
  if (FAILED(hr)) return false;
  g_dscaps_free_hw_mixing_all = caps.dwFreeHwMixingAllBuffers;
  g_dscaps_free_hw_mixing_static = caps.dwFreeHwMixingStaticBuffers;
  g_dscaps_free_hw3d_all = caps.dwFreeHw3DAllBuffers;
  g_dscaps_free_hw3d_static = caps.dwFreeHw3DStaticBuffers;
  return true;
}

void soft_init_ds_clear_voice_banks() {
  // Soft PE InitDS @ 0x559AA2..0x559ADE: memset Sound_dsVoiceBank 0x1000,
  // dword_783C88, byte_77DBE0, Sound_dsVoiceTypeMax, dword_77E288, dword_77DC60.
  std::memset(g_ds_voice_bank, 0, sizeof(g_ds_voice_bank));
  std::memset(g_ds_voice_type_max, 0, sizeof(g_ds_voice_type_max));
  std::memset(g_ds_voice_slot_flags, 0, sizeof(g_ds_voice_slot_flags));
  std::memset(g_ds_voice_type_is3d, 0, sizeof(g_ds_voice_type_is3d));
  g_res_to_voice_type.clear();
}

// Soft PE InitDS primary CTRL3D + QI @ 0x0055999D..0x00559A95.
// DSBUFFERDESC dwSize=36 flags=17 (PRIMARYBUFFER|CTRL3D); SetFormat PCM
// 22050 stereo 16-bit (wfx dword0=0x20001); Play LOOPING; QI IID @ 0x60333C
// → Sound_ds3DListener. Soft: fail leaves listener null, keeps secondary DS
// (PE would Release/-4..-7 — Soft diverge so SFX buffers still boot).
bool soft_init_ds_primary_listener() {
  if (!g_ds) return false;
  if (g_ds_primary) return g_ds3d_listener != nullptr;
  DSBUFFERDESC desc{};
  desc.dwSize = 36;  // PE v3[0]=36
  desc.dwFlags = DSBCAPS_PRIMARYBUFFER | DSBCAPS_CTRL3D;  // 17
  HRESULT hr = g_ds->CreateSoundBuffer(&desc, &g_ds_primary, nullptr);
  if (FAILED(hr) || !g_ds_primary) {
    g_ds_primary = nullptr;
    return false;
  }
  WAVEFORMATEX wfx{};
  wfx.wFormatTag = WAVE_FORMAT_PCM;  // PE *(_DWORD*)v1 = 0x20001
  wfx.nChannels = 2;
  wfx.nSamplesPerSec = 22050;
  wfx.nAvgBytesPerSec = 88200;
  wfx.nBlockAlign = 4;
  wfx.wBitsPerSample = 16;
  hr = g_ds_primary->SetFormat(&wfx);
  if (FAILED(hr)) {
    g_ds_primary->Release();
    g_ds_primary = nullptr;
    return false;
  }
  hr = g_ds_primary->Play(0, 0, DSBPLAY_LOOPING);  // PE a4=1
  if (FAILED(hr)) {
    g_ds_primary->Release();
    g_ds_primary = nullptr;
    return false;
  }
  hr = g_ds_primary->QueryInterface(IID_IDirectSound3DListener,
                                    reinterpret_cast<void**>(&g_ds3d_listener));
  if (FAILED(hr) || !g_ds3d_listener) {
    g_ds3d_listener = nullptr;
    g_ds_primary->Release();
    g_ds_primary = nullptr;
    return false;
  }
  return true;
}

// Soft PE Sfx_ApplyListenerToDS @ 0x0055B260 size 0x1D1.
// PE: (!Sound_dsReady || ctx != 0) → -1. Pose = 16 dwords Vector4 xyz/o/u/f
// (Sfx_listener_* @ 0x768720, pads at +0xC/+0x1C/+0x2C/+0x3C).
// If Sound_ds3DListener && hw3D: SetAllParameters(DS3DLISTENER dwSize=64,
// dist/rolloff/doppler=1.0f, DS3D_DEFERRED=1). Else soft orient cross into
// right vector. Always mirror pos (a2[0..3]) + vel/ox (a2[4..7]).
// Caller PE: Sfx_ListenerSetPose @ 0x0055096A.
int32_t soft_sfx_apply_listener_to_ds(int32_t ctx, const float* pose16) {
  if (!g_sound_ds_ready || ctx != 0 || !pose16) return -1;
  if (g_ds3d_listener != nullptr && g_sound_hw3d_capable != 0) {
    DS3DLISTENER lis{};
    lis.dwSize = sizeof(lis);  // PE v16[0]=64
    lis.vPosition.x = pose16[0];
    lis.vPosition.y = pose16[1];
    lis.vPosition.z = pose16[2];
    lis.vVelocity.x = pose16[4];
    lis.vVelocity.y = pose16[5];
    lis.vVelocity.z = pose16[6];
    lis.vOrientFront.x = pose16[8];
    lis.vOrientFront.y = pose16[9];
    lis.vOrientFront.z = pose16[10];
    lis.vOrientTop.x = pose16[12];
    lis.vOrientTop.y = pose16[13];
    lis.vOrientTop.z = pose16[14];
    lis.flDistanceFactor = 1.f;  // 0x3F800000
    lis.flRolloffFactor = 1.f;
    lis.flDopplerFactor = 1.f;
    g_ds3d_listener->SetAllParameters(&lis, DS3D_DEFERRED);
  } else {
    // Soft PE else @ 0x55B32A: copy ux/fx, right = top × front → 77E490.
    g_ds_listener_front[0] = pose16[8];
    g_ds_listener_front[1] = pose16[9];
    g_ds_listener_front[2] = pose16[10];
    g_ds_listener_front[3] = pose16[11];
    g_ds_listener_top[0] = pose16[12];
    g_ds_listener_top[1] = pose16[13];
    g_ds_listener_top[2] = pose16[14];
    g_ds_listener_top[3] = pose16[15];
    g_ds_listener_right[0] =
        g_ds_listener_top[1] * g_ds_listener_front[2] -
        g_ds_listener_top[2] * g_ds_listener_front[1];
    g_ds_listener_right[1] =
        g_ds_listener_top[2] * g_ds_listener_front[0] -
        g_ds_listener_top[0] * g_ds_listener_front[2];
    g_ds_listener_right[2] =
        g_ds_listener_top[0] * g_ds_listener_front[1] -
        g_ds_listener_top[1] * g_ds_listener_front[0];
  }
  g_ds_listener_pos[0] = pose16[0];
  g_ds_listener_pos[1] = pose16[1];
  g_ds_listener_pos[2] = pose16[2];
  g_ds_listener_pos[3] = pose16[3];
  g_ds_listener_vel[0] = pose16[4];
  g_ds_listener_vel[1] = pose16[5];
  g_ds_listener_vel[2] = pose16[6];
  g_ds_listener_vel[3] = pose16[7];
  return 0;
}

// Soft PE Sfx_ListenerSetPose @ 0x005508F0 size 0x85.
// PE: scatter packed 12 floats from a1 into Sfx_listener_* Vector4 BSS
// (xyz→[0..2], o→[4..6], u→[8..10], f→[12..14]; pads untouched), then
// Sfx_ApplyListenerToDS(Sfx_dsContext, &Sfx_listener_x); return 0.
// Sole xref Engine_MainLoop @ 0x00428CB6.
int32_t soft_sfx_listener_set_pose(const float* pose12) {
  if (!pose12) return -1;
  g_sfx_listener_pose[0] = pose12[0];
  g_sfx_listener_pose[1] = pose12[1];
  g_sfx_listener_pose[2] = pose12[2];
  // [3] pad @ 0x76872C
  g_sfx_listener_pose[4] = pose12[3];
  g_sfx_listener_pose[5] = pose12[4];
  g_sfx_listener_pose[6] = pose12[5];
  // [7] pad @ 0x76873C
  g_sfx_listener_pose[8] = pose12[6];
  g_sfx_listener_pose[9] = pose12[7];
  g_sfx_listener_pose[10] = pose12[8];
  // [11] pad @ 0x76874C
  g_sfx_listener_pose[12] = pose12[9];
  g_sfx_listener_pose[13] = pose12[10];
  g_sfx_listener_pose[14] = pose12[11];
  // [15] pad @ 0x76875C
  soft_sfx_apply_listener_to_ds(kSfxDsContext, g_sfx_listener_pose);
  return 0;
}

// Soft PE Sfx_CommitListenerDS @ 0x0055B440 size 0x50.
// PE: (!Sound_dsReady || ctx != 0) → -1. If Sound_dsThreaded @ 0x784CA0:
// Engine_ReleaseSemaphore — OOS (host threaded=0). Else if listener && hw3D:
// IDirectSound3DListener::CommitDeferredSettings (vtable+0x44).
// Caller PE: Sfx_UpdateVoices @ 0x00550AFD.
int32_t soft_sfx_commit_listener_ds(int32_t ctx) {
  if (!g_sound_ds_ready || ctx != 0) return -1;
  // Soft PE: Sound_dsThreaded stand-in 0 — no Engine_ReleaseSemaphore.
  if (g_ds3d_listener != nullptr && g_sound_hw3d_capable != 0)
    g_ds3d_listener->CommitDeferredSettings();
  return 0;
}

// Soft PE Sfx_HwVoiceBudget @ 0x00559DD0 size 0x4C.
// sum = (hwMix ? FreeHwMixingStatic : 0) + (hw3D ? FreeHw3DStatic : 0);
// return sum <= 4 ? 4 : sum. Sole callee of UpdateVoices head.
int32_t soft_sfx_hw_voice_budget() {
  uint32_t sum = 0;
  if (g_sound_hwmix_capable != 0)
    sum += g_dscaps_free_hw_mixing_static;
  if (g_sound_hw3d_capable != 0) sum += g_dscaps_free_hw3d_static;
  return sum <= static_cast<uint32_t>(kSfxHwVoiceBudgetMin)
             ? kSfxHwVoiceBudgetMin
             : static_cast<int32_t>(sum);
}

// Soft PE Sfx_DSVoiceIsFinished @ 0x0055B950 size 0x50.
// PE name inverted vs English: returns 1 while buffer still PLAYING
// (UpdateVoices keep), 0 → free without Stop. Decode ds_voice =
// (type<<16)|slot; bank Sound_dsVoiceBank[8*type+slot]; GetStatus (vt+0x24);
// SUCCEEDED && (status & 1). Soft bank empty until DSAllocVoice — host
// Voice.buf path uses soft_sfx_ds_buf_is_playing.
int32_t soft_sfx_ds_voice_is_finished(int32_t ds_voice) {
  const int32_t type = ds_voice >> 16;
  const uint32_t slot = static_cast<uint32_t>(ds_voice) & 0xFFFFu;
  if (type < 0 || type >= 0x80) return 0;
  if (slot >= 8u) return 0;
  IDirectSoundBuffer* buf = g_ds_voice_bank[type][slot];
  if (!buf) return 0;
  DWORD status = 0;
  if (FAILED(buf->GetStatus(&status))) return 0;
  return (status & DSBSTATUS_PLAYING) != 0 ? 1 : 0;
}

// Soft PE buffer GetStatus stand-in for host Voice map (no packed handle).
int32_t soft_sfx_ds_buf_is_playing(IDirectSoundBuffer* buf) {
  if (!buf) return 0;
  DWORD status = 0;
  if (FAILED(buf->GetStatus(&status))) return 0;
  return (status & DSBSTATUS_PLAYING) != 0 ? 1 : 0;
}

// Soft PE Sfx_DSStopVoice @ 0x0055C1D0 size 0x41.
// PE: if dsReady && type/slot in range && bank[slot] → Stop (vt+0x48).
void soft_sfx_ds_stop_voice(int32_t ds_voice) {
  if (!g_sound_ds_ready) return;
  const int32_t type = ds_voice >> 16;
  const uint32_t slot = static_cast<uint32_t>(ds_voice) & 0xFFFFu;
  if (type < 0 || type >= 0x80 || slot >= 8u) return;
  IDirectSoundBuffer* buf = g_ds_voice_bank[type][slot];
  if (buf) buf->Stop();
}

// Soft PE Sfx_VoiceTypeMaxCount @ 0x0055B900 / HasBuffers @ 0x0055B920.
int32_t soft_sfx_voice_type_max_count(uint32_t type) {
  if (type >= 0x80u) return 0;
  return g_ds_voice_type_max[type];
}

int32_t soft_sfx_voice_type_has_buffers(uint32_t type) {
  return (type < 0x80u && g_ds_voice_bank[type][0] != nullptr) ? 1 : 0;
}

// Soft PE Sfx_DSApplyVoiceParams @ 0x0055BB20 — 3D pan/volume onto buffer.
// Soft: buffer live → 1 (params OOS); else 0. DSAlloc/Update gate only.
int32_t soft_sfx_ds_apply_voice_params(uint32_t type, uint32_t slot,
                                       const int32_t* /*payload*/) {
  if (type >= 0x80u || slot >= 8u) return 0;
  return g_ds_voice_bank[type][slot] != nullptr ? 1 : 0;
}

// Soft PE Sound_FillSecondaryBuffer @ 0x0055B7F0 — Lock/copy/Unlock.
bool soft_sound_fill_secondary_buffer(IDirectSoundBuffer* buf,
                                      const uint8_t* data, uint32_t size) {
  if (!buf || !data || size == 0) return false;
  void* p1 = nullptr;
  void* p2 = nullptr;
  DWORD s1 = 0, s2 = 0;
  if (FAILED(buf->Lock(0, size, &p1, &s1, &p2, &s2, 0)) || !p1) return false;
  std::memcpy(p1, data, s1);
  if (p2 && s2) std::memcpy(p2, data + s1, s2);
  buf->Unlock(p1, s1, p2, s2);
  return true;
}

// Soft PE Sfx_DSPlayBuffer @ 0x0055BFC0 — Play flag select only.
// PE: (!hwMix || flags&4) → flags&1; else (flags&1)|0x10 (TERMINATEBY_DISTANCE).
// LOSTBUFFER restore / WAV refill path OOS.
HRESULT soft_sfx_ds_play_buffer(IDirectSoundBuffer* buf, int32_t flags) {
  if (!buf) return E_POINTER;
  DWORD play = static_cast<DWORD>(flags & 1);
  if (g_sound_hwmix_capable != 0 && (flags & 4) == 0)
    play |= DSBPLAY_TERMINATEBY_DISTANCE;  // PE | 0x10
  return buf->Play(0, 0, play);
}

// Soft PE Sound_RegisterVoiceType @ 0x0055B490 (AsyncLoad_Type5 @ 0x53BB90).
// PE: free type row; CreateSoundBuffer (PE dwFlags 0xE8/0x400E0/0x200B8/0x600B0);
// FillSecondaryBuffer; QI 3D OOS Soft; DuplicateSoundBuffer for slots 1..max-1;
// typeMax/is3d. Returns type, -1 (full/!dsReady), or -2 (create/fill fail).
int32_t soft_sfx_register_voice_type(const WavInfo& wav, int32_t max_voices,
                                     bool is3d) {
  if (!g_sound_ds_ready || !g_ds || !wav.data || wav.data_size == 0) return -1;
  int32_t type = 0;
  for (; type < 0x80; ++type) {
    if (g_ds_voice_bank[type][0] == nullptr) break;
  }
  if (type >= 0x80) return -1;
  int32_t max_n = max_voices;
  if (max_n > 8) max_n = 8;
  if (max_n <= 0) max_n = 1;

  DWORD caps = 0;
  if (is3d && g_sound_hw3d_capable != 0) {
    // PE @ 0x55B56E: hwMix ? 0x600B0 : 0x200B8
    caps = g_sound_hwmix_capable != 0 ? 0x600B0u : 0x200B8u;
  } else {
    // PE @ 0x55B5C8: hwMix ? 0x400E0 : 0xE8
    caps = g_sound_hwmix_capable != 0 ? 0x400E0u : 0xE8u;
  }
  DSBUFFERDESC desc{};
  desc.dwSize = 36;  // PE v29[0]=36
  desc.dwFlags = caps;
  desc.dwBufferBytes = wav.data_size;
  desc.lpwfxFormat = const_cast<WAVEFORMATEX*>(&wav.fmt);
  IDirectSoundBuffer* buf = nullptr;
  if (FAILED(g_ds->CreateSoundBuffer(&desc, &buf, nullptr)) || !buf) return -2;
  if (!soft_sound_fill_secondary_buffer(buf, wav.data, wav.data_size)) {
    buf->Release();
    return -2;
  }
  g_ds_voice_bank[type][0] = buf;
  // Soft PE QI IDirectSound3DBuffer @ 0x55B66F — OOS (no Soft 3D iface bank).
  g_ds_voice_type_is3d[type] =
      (is3d && g_sound_hw3d_capable != 0) ? static_cast<uint8_t>(1) : 0;
  g_ds_voice_type_max[type] = max_n;
  for (int32_t slot = 1; slot < max_n; ++slot) {
    IDirectSoundBuffer* dup = nullptr;
    if (FAILED(g_ds->DuplicateSoundBuffer(buf, &dup)) || !dup) {
      g_ds_voice_bank[type][slot] = nullptr;
    } else {
      g_ds_voice_bank[type][slot] = dup;
    }
  }
  // Soft PE GetFrequency → dword_77E288[type] — used by ApplyVoiceParams OOS.
  return type;
}

// Soft stand-in: first play of res_id → RegisterVoiceType (max=2 Soft).
int32_t soft_sfx_ensure_voice_type(int32_t res_id, const WavInfo& wav,
                                   bool is3d) {
  auto it = g_res_to_voice_type.find(res_id);
  if (it != g_res_to_voice_type.end()) return it->second;
  const int32_t type = soft_sfx_register_voice_type(wav, 2, is3d);
  if (type >= 0) g_res_to_voice_type[res_id] = type;
  return type;
}

// Soft PE Sfx_DSAllocVoice @ 0x0055B9A0.
// PE: !dsReady|type>=0x80 → -1; scan bank[type][0..max) for free/not-playing
// slot (reuse non-looping playing as last resort); store flags; Stop if
// playing; SetCurrentPosition(0); apply params; Play; return (type<<16)|slot.
// Soft empty bank → -1 (InitDS zeros bank until RegisterVoiceType).
int32_t soft_sfx_ds_alloc_voice(uint32_t type, const int32_t* payload) {
  if (!g_sound_ds_ready || type >= 0x80u || !payload) return -1;
  const int32_t max_n = g_ds_voice_type_max[type];
  if (max_n <= 0) return -1;
  int32_t reuse = -1;
  int32_t slot = 0;
  IDirectSoundBuffer* buf = nullptr;
  for (; slot < max_n && slot < 8; ++slot) {
    buf = g_ds_voice_bank[type][slot];
    if (!buf) return -1;  // PE: null bank ptr → -1 mid-scan
    DWORD status = 0;
    if (FAILED(buf->GetStatus(&status))) continue;
    if ((status & DSBSTATUS_PLAYING) == 0) break;
    // PE: playing + !(slot_flags&2) → candidate reuse
    if ((g_ds_voice_slot_flags[type][slot] & 2) == 0 && reuse < 0)
      reuse = slot;
  }
  if (slot == max_n || slot >= 8) {
    if (reuse < 0) return -1;
    slot = reuse;
    buf = g_ds_voice_bank[type][slot];
  }
  if (!buf) return -1;
  int32_t flags = payload[0];
  if (g_ds_voice_type_is3d[type] == 0) flags |= 4;
  g_ds_voice_slot_flags[type][slot] = flags;
  DWORD status = 0;
  if (SUCCEEDED(buf->GetStatus(&status)) && (status & DSBSTATUS_PLAYING) != 0)
    buf->Stop();
  buf->SetCurrentPosition(0);
  // Soft PE Sfx_DSApplyVoiceParams + Sfx_DSPlayBuffer — pan/vol OOS.
  if (soft_sfx_ds_apply_voice_params(type, static_cast<uint32_t>(slot),
                                     payload) != 0) {
    soft_sfx_ds_play_buffer(buf, flags);
  }
  return (static_cast<int32_t>(type) << 16) | (slot & 0xFFFF);
}

// Soft PE Sfx_DSUpdateVoice @ 0x0055C120.
// PE: decode handle; bank buf; apply params; if apply!=0 && !playing →
// SetCurrentPosition+Play(Sfx_DSPlayBuffer); else Stop. Soft: apply stub.
int32_t soft_sfx_ds_update_voice(int32_t ds_voice, const int32_t* payload) {
  if (!g_sound_ds_ready || !payload) return -1;
  const int32_t type = ds_voice >> 16;
  const uint32_t slot = static_cast<uint32_t>(ds_voice) & 0xFFFFu;
  if (type < 0 || type >= 0x80 || slot >= 8u) return -1;
  IDirectSoundBuffer* buf = g_ds_voice_bank[type][slot];
  if (!buf) return -1;
  const int32_t applied =
      soft_sfx_ds_apply_voice_params(static_cast<uint32_t>(type), slot, payload);
  if (applied != 0) {
    DWORD status = 0;
    if (SUCCEEDED(buf->GetStatus(&status)) &&
        (status & DSBSTATUS_PLAYING) == 0) {
      buf->SetCurrentPosition(0);
      const int32_t flags = g_ds_voice_slot_flags[type][slot];
      soft_sfx_ds_play_buffer(buf, flags);
    }
  } else {
    buf->Stop();
  }
  return ds_voice;
}

// Soft PE Sfx_InitVoiceTable @ 0x00550500 — state/type/priority + sort idx.
void soft_sfx_init_voice_table() {
  for (int32_t i = 0; i < kSfxVoiceSortCount; ++i) {
    SoftSfxVoiceSlot& s = g_sfx_voice_table[i];
    s.state = 0;
    s.type = 0;
    s.handle = 0;
    s.instance = 0;
    s.ds_voice = 0;
    s.priority = kSfxFreePrioritySentinel;
    std::memset(s.payload, 0, sizeof(s.payload));
    g_sfx_voice_sort_idx[i] = i;
  }
  std::memset(g_sfx_voice_type_use_count, 0, sizeof(g_sfx_voice_type_use_count));
  g_sfx_voice_table_inited = true;
}

// Soft PE Sfx_VoiceSortByPriority @ 0x00550B30 — ascending float priority.
int soft_sfx_voice_sort_by_priority(const void* a1, const void* a2) {
  const int32_t ia = *static_cast<const int32_t*>(a1);
  const int32_t ib = *static_cast<const int32_t*>(a2);
  const float d =
      g_sfx_voice_table[ia].priority - g_sfx_voice_table[ib].priority;
  if (d > 0.f) return 1;
  if (d >= 0.f) return 0;
  return -1;
}

void soft_sfx_free_voice_slot(SoftSfxVoiceSlot& s) {
  s.state = 0;
  s.type = 0;
  s.priority = kSfxFreePrioritySentinel;
}

bool soft_sfx_pass1_try_keep(SoftSfxVoiceSlot& s, int32_t budget,
                             int32_t& kept) {
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

// Soft PE UpdateVoices pass1 @ 0x5509B2..0x550A80.
void soft_sfx_update_voices_pass1(int32_t budget) {
  int32_t kept = 0;
  for (int32_t i = 0; i < kSfxVoiceSortCount; ++i) {
    SoftSfxVoiceSlot& s = g_sfx_voice_table[g_sfx_voice_sort_idx[i]];
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
    } else if (st != 3) {
      continue;
    }
    if (soft_sfx_pass1_try_keep(s, budget, kept)) continue;
    soft_sfx_ds_stop_voice(s.ds_voice);
    soft_sfx_free_voice_slot(s);
  }
}

// Soft PE UpdateVoices pass2 @ 0x550A86..0x550AF4.
void soft_sfx_update_voices_pass2() {
  for (int32_t i = 0; i < kSfxVoiceSortCount; ++i) {
    SoftSfxVoiceSlot& s = g_sfx_voice_table[g_sfx_voice_sort_idx[i]];
    if (s.state == 1) {
      const int32_t dv =
          soft_sfx_ds_alloc_voice(static_cast<uint32_t>(s.type), s.payload);
      s.ds_voice = dv;
      if (dv < 0) {
        soft_sfx_free_voice_slot(s);
      } else {
        s.state = 2;
      }
      continue;
    }
    if (s.state == 2) {
      if ((s.payload[0] & 1) == 0) continue;
      soft_sfx_ds_stop_voice(s.ds_voice);
      soft_sfx_free_voice_slot(s);
      continue;
    }
    if (s.state == 3) {
      soft_sfx_ds_update_voice(s.ds_voice, s.payload);
      s.state = 2;
    }
  }
}

// Soft PE Sfx_UpdateVoices @ 0x00550980 size 0x1AC (int_convert 428).
// Sole MainLoop xref @ 0x00428CBE. PE: HwVoiceBudget @ 0x559DD0 +
// CRT_qsort @ 0x5D765D (VoiceSortByPriority @ 0x550B30) + memset typeUse +
// pass1 @ 0x5509B2..0x550A80 + pass2 @ 0x550A86..0x550AF4 +
// CommitListenerDS @ 0x55B440. Free priority sentinel PE 0x501502F9 = 1e10.
int32_t soft_sfx_update_voices() {
  if (!g_sfx_voice_table_inited) soft_sfx_init_voice_table();
  const int32_t budget = soft_sfx_hw_voice_budget();
  std::qsort(g_sfx_voice_sort_idx, static_cast<size_t>(kSfxVoiceSortCount),
             sizeof(int32_t), soft_sfx_voice_sort_by_priority);
  std::memset(g_sfx_voice_type_use_count, 0, sizeof(g_sfx_voice_type_use_count));
  soft_sfx_update_voices_pass1(budget);
  soft_sfx_update_voices_pass2();
  soft_sfx_commit_listener_ds(kSfxDsContext);
  return 0;
}

bool ensure_ds() {
  if (g_ds_tried) return g_ds_ok;
  g_ds_tried = true;
  g_sound_ds_ready = false;
  HRESULT hr = DirectSoundCreate8(nullptr, &g_ds, nullptr);
  if (FAILED(hr) || !g_ds) return false;
  HWND hwnd = reinterpret_cast<HWND>(render_d3d9_hwnd());
  if (!hwnd) hwnd = GetDesktopWindow();
  // PE SetCooperativeLevel(hWnd, 2) = DSSCL_PRIORITY @ 0x00559972.
  hr = g_ds->SetCooperativeLevel(hwnd, DSSCL_PRIORITY);
  if (FAILED(hr)) {
    g_ds->Release();
    g_ds = nullptr;
    return false;
  }
  // Soft PE InitDS GetCaps residual (PE fail path Release + -2).
  if (!soft_init_ds_getcaps()) {
    g_ds->Release();
    g_ds = nullptr;
    return false;
  }
  // Soft PE primary CTRL3D + QI listener (fail → Soft 2D-only path).
  soft_init_ds_primary_listener();
  soft_init_ds_clear_voice_banks();
  // Soft PE hw predicates after primary @ 0x00559B94..0x00559BAC.
  g_sound_hw3d_capable = (g_dscaps_free_hw3d_all >= 4u) ? 1u : 0u;
  g_sound_hwmix_capable = (g_dscaps_free_hw_mixing_all >= 4u) ? 1u : 0u;
  g_sound_ds_ready = true;  // PE Sound_dsReady=1 @ 0x00559BA5
  g_ds_ok = true;
  soft_sfx_init_voice_table();
  // Soft PE: exercise ListenerSetPose + UpdateVoices (MainLoop sites).
  float zero_pose12[12]{};
  soft_sfx_listener_set_pose(zero_pose12);
  soft_sfx_update_voices();
  (void)soft_sfx_ds_voice_is_finished(0);
  soft_sfx_ds_stop_voice(0);
  (void)soft_sfx_voice_type_max_count(0);
  (void)soft_sfx_voice_type_has_buffers(0);
  int32_t payload0[17]{};
  (void)soft_sfx_ds_alloc_voice(0, payload0);
  return true;
}

long volume_to_db(float linear) {
  if (linear <= 0.0001f) return DSBVOLUME_MIN;
  if (linear >= 1.f) return DSBVOLUME_MAX;
  const float db = 20.f * std::log10(linear);
  long v = static_cast<long>(db * 100.f);
  if (v < DSBVOLUME_MIN) v = DSBVOLUME_MIN;
  if (v > DSBVOLUME_MAX) v = DSBVOLUME_MAX;
  return v;
}

IDirectSoundBuffer* ds_create_buffer(const WavInfo& wav, float gain) {
  if (!g_ds || !wav.data || wav.data_size == 0) return nullptr;
  DSBUFFERDESC desc{};
  desc.dwSize = sizeof(desc);
  desc.dwFlags = DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLFREQUENCY | DSBCAPS_GLOBALFOCUS;
  desc.dwBufferBytes = wav.data_size;
  desc.lpwfxFormat = const_cast<WAVEFORMATEX*>(&wav.fmt);
  IDirectSoundBuffer* buf = nullptr;
  HRESULT hr = g_ds->CreateSoundBuffer(&desc, &buf, nullptr);
  if (FAILED(hr) || !buf) return nullptr;
  void* p1 = nullptr;
  void* p2 = nullptr;
  DWORD s1 = 0, s2 = 0;
  hr = buf->Lock(0, wav.data_size, &p1, &s1, &p2, &s2, 0);
  if (FAILED(hr) || !p1) {
    buf->Release();
    return nullptr;
  }
  std::memcpy(p1, wav.data, s1);
  if (p2 && s2) std::memcpy(p2, wav.data + s1, s2);
  buf->Unlock(p1, s1, p2, s2);
  buf->SetVolume(volume_to_db(gain));
  return buf;
}

bool ds_play(IDirectSoundBuffer* buf, bool loop) {
  if (!buf) return false;
  buf->SetCurrentPosition(0);
  const DWORD flags = loop ? DSBPLAY_LOOPING : 0;
  return SUCCEEDED(buf->Play(0, 0, flags));
}

void ds_stop_release(IDirectSoundBuffer*& buf) {
  if (!buf) return;
  buf->Stop();
  buf->Release();
  buf = nullptr;
}
#endif

bool win_play(const std::string& path, bool loop) {
#ifdef _WIN32
  if (path.empty()) return false;
  DWORD flags = SND_ASYNC | SND_FILENAME | SND_NODEFAULT;
  if (loop) flags |= SND_LOOP;
  return PlaySoundA(path.c_str(), nullptr, flags) != FALSE;
#else
  (void)path;
  (void)loop;
  return !path.empty();
#endif
}

void win_stop_all() {
#ifdef _WIN32
  PlaySoundA(nullptr, nullptr, 0);
#endif
}

const char* music_set_folder(int32_t type) {
  // Sound.MUSIC_SET_* → game/music/<dir>
  switch (type) {
    case 0:
      return "music/Garage_Shop";
    case 1:
      return "music/Roam_ride";
    case 2:
      return "music/Race_chase";
    case 3:
      return "music/Main_menu";
    default:
      return nullptr;
  }
}

bool is_music_file(const char* name) {
  if (!name) return false;
  const size_t n = std::strlen(name);
  if (n < 5) return false;
  char ext[5] = {name[n - 4], name[n - 3], name[n - 2], name[n - 1], 0};
  for (int i = 0; i < 4; ++i) {
    if (ext[i] >= 'A' && ext[i] <= 'Z') ext[i] = static_cast<char>(ext[i] - 'A' + 'a');
  }
  return std::strcmp(ext, ".mp3") == 0 || std::strcmp(ext, ".wav") == 0 ||
         std::strcmp(ext, ".ogg") == 0;
}

void music_mci_close() {
#ifdef _WIN32
  mciSendStringA("stop slrr_bgm", nullptr, 0, nullptr);
  mciSendStringA("close slrr_bgm", nullptr, 0, nullptr);
#endif
  g_music_playing = false;
}

bool music_mci_play(const std::string& path) {
  music_mci_close();
  if (path.empty()) return false;
#ifdef _WIN32
  // Escape quotes in path for MCI.
  std::string cmd = "open \"";
  cmd += path;
  cmd += "\" type mpegvideo alias slrr_bgm";
  if (mciSendStringA(cmd.c_str(), nullptr, 0, nullptr) != 0) {
    // Some installs prefer alias without type.
    cmd = "open \"";
    cmd += path;
    cmd += "\" alias slrr_bgm";
    if (mciSendStringA(cmd.c_str(), nullptr, 0, nullptr) != 0) return false;
  }
  // Volume 0..1000 for MCI digital video.
  const int vol = static_cast<int>(clamp01(g_vol[kAudioChannelMusic]) * 1000.f);
  char vcmd[64];
  std::snprintf(vcmd, sizeof(vcmd), "setaudio slrr_bgm volume to %d", vol);
  mciSendStringA(vcmd, nullptr, 0, nullptr);
  if (mciSendStringA("play slrr_bgm", nullptr, 0, nullptr) != 0) {
    music_mci_close();
    return false;
  }
  g_music_playing = true;
  return true;
#else
  (void)path;
  g_music_playing = true;
  return true;
#endif
}

void music_scan_folder(const char* rel_dir, std::vector<std::string>* out) {
  out->clear();
  if (!rel_dir) return;
  std::string dir = rpak_resolve_path(rel_dir);
  if (dir.empty()) dir = rel_dir;
#ifdef _WIN32
  std::string pattern = dir;
  if (!pattern.empty() && pattern.back() != '\\' && pattern.back() != '/')
    pattern += '\\';
  pattern += "*.*";
  WIN32_FIND_DATAA fd{};
  HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return;
  do {
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
    if (!is_music_file(fd.cFileName)) continue;
    std::string full = dir;
    if (!full.empty() && full.back() != '\\' && full.back() != '/') full += '/';
    full += fd.cFileName;
    out->push_back(std::move(full));
  } while (FindNextFileA(h, &fd));
  FindClose(h);
#else
  (void)dir;
#endif
}

const char* basename_of(const std::string& path) {
  size_t s = path.find_last_of("/\\");
  if (s == std::string::npos) return path.c_str();
  return path.c_str() + s + 1;
}

void music_play_current_unlocked() {
  if (g_music_tracks.empty() || g_music_index < 0 ||
      g_music_index >= static_cast<int32_t>(g_music_tracks.size())) {
    music_mci_close();
    return;
  }
  music_mci_play(g_music_tracks[static_cast<size_t>(g_music_index)]);
}

}  // namespace

void audio_set_volume(int32_t channel, float volume) {
  if (channel < 0 || channel > 2) return;
  std::lock_guard<std::mutex> lock(g_mu);
  g_vol[channel] = clamp01(volume);
#ifdef _WIN32
  if (channel == kAudioChannelMusic && g_music_playing) {
    const int vol = static_cast<int>(g_vol[kAudioChannelMusic] * 1000.f);
    char vcmd[64];
    std::snprintf(vcmd, sizeof(vcmd), "setaudio slrr_bgm volume to %d", vol);
    mciSendStringA(vcmd, nullptr, 0, nullptr);
  }
#endif
}

float audio_get_volume(int32_t channel) {
  if (channel < 0 || channel > 2) return 0.f;
  std::lock_guard<std::mutex> lock(g_mu);
  return g_vol[channel];
}

void audio_change_music_set(int32_t type) {
  std::lock_guard<std::mutex> lock(g_mu);
  music_mci_close();
  g_music_set = type;
  g_music_index = 0;
  g_music_tracks.clear();
  if (type < 0) return;
  const char* folder = music_set_folder(type);
  if (!folder) return;
  music_scan_folder(folder, &g_music_tracks);
  if (!g_music_tracks.empty()) music_play_current_unlocked();
}

int32_t audio_music_set() {
  std::lock_guard<std::mutex> lock(g_mu);
  return g_music_set;
}

void audio_music_next_track() {
  std::lock_guard<std::mutex> lock(g_mu);
  if (g_music_tracks.empty()) return;
  g_music_index =
      (g_music_index + 1) % static_cast<int32_t>(g_music_tracks.size());
  music_play_current_unlocked();
}

void audio_music_prev_track() {
  std::lock_guard<std::mutex> lock(g_mu);
  if (g_music_tracks.empty()) return;
  g_music_index =
      (g_music_index - 1 + static_cast<int32_t>(g_music_tracks.size())) %
      static_cast<int32_t>(g_music_tracks.size());
  music_play_current_unlocked();
}

int32_t audio_music_track_count() {
  std::lock_guard<std::mutex> lock(g_mu);
  return static_cast<int32_t>(g_music_tracks.size());
}

int32_t audio_music_track_index() {
  std::lock_guard<std::mutex> lock(g_mu);
  return g_music_index;
}

const char* audio_music_track_name() {
  std::lock_guard<std::mutex> lock(g_mu);
  if (g_music_tracks.empty() || g_music_index < 0 ||
      g_music_index >= static_cast<int32_t>(g_music_tracks.size()))
    return "";
  return basename_of(g_music_tracks[static_cast<size_t>(g_music_index)]);
}

bool audio_music_playing() {
  std::lock_guard<std::mutex> lock(g_mu);
  return g_music_playing;
}

bool audio_ds_ready() {
  std::lock_guard<std::mutex> lock(g_mu);
#ifdef _WIN32
  return ensure_ds();
#else
  return false;
#endif
}

const char* audio_backend() {
  std::lock_guard<std::mutex> lock(g_mu);
#ifdef _WIN32
  if (g_ds_ok) return "dsound";
  if (g_ds_tried) return "winmm";
#endif
  return "none";
}

bool audio_resolve_wav(int32_t res_id, char* out, size_t out_cap) {
  if (!out || out_cap == 0) return false;
  out[0] = '\0';
  if (res_id == 0) return false;

  std::string resolved;
  const RpakEntry* ent = rpak_find_entry(res_id);
  std::vector<uint8_t> blob;
  if (rpak_read_entry(res_id, &blob) && !blob.empty())
    resolved = try_resolve(parse_sourcefile(blob));

  if (resolved.empty() && ent) {
    const std::string& nm = ent->name;
    if (!nm.empty()) {
      resolved = try_resolve(std::string("frontend/sounds/") + nm + ".wav");
      if (resolved.empty())
        resolved = try_resolve(std::string("frontend/sounds/") + nm + ".WAV");
      if (resolved.empty())
        resolved = try_resolve(std::string("sound/wav/") + nm + ".wav");
    }
    if (resolved.empty() && !ent->path.empty()) {
      resolved = try_resolve(ent->path + ".wav");
      if (resolved.empty()) resolved = try_resolve(ent->path);
    }
  }

  if (resolved.empty()) return false;
  if (resolved.size() + 1 > out_cap) return false;
  std::memcpy(out, resolved.c_str(), resolved.size() + 1);
  return true;
}

int32_t audio_sfx_play(int32_t res_id, float pitch, float volume, int32_t flags,
                       int32_t instance) {
  char path_buf[512];
  if (!audio_resolve_wav(res_id, path_buf, sizeof(path_buf))) return 0;

  std::lock_guard<std::mutex> lock(g_mu);
  if (instance != 0) {
    auto it = g_voices.find(instance);
    if (it != g_voices.end() && it->second.alive) return instance;
  }

  const float eff = clamp01(volume) * g_vol[kAudioChannelEffects];
  (void)pitch;
  const bool loop = (flags & kAudioSfxLoop) != 0;

  Voice v;
  v.res_id = res_id;
  v.flags = flags;
  v.pitch = pitch;
  v.volume = volume;
  v.path = path_buf;
  v.alive = true;

#ifdef _WIN32
  if (ensure_ds()) {
    std::vector<uint8_t> file;
    WavInfo wav;
    if (load_file(path_buf, &file) && parse_wav(file, &wav)) {
      // Soft PE AsyncLoad_Type5 @ 0x53BB90 → Sound_RegisterVoiceType
      // @ 0x55B490 — bank fill so UpdateVoices HasBuffers/DSAlloc can run.
      // PE 2D gate flags&4; Soft is3d = !(flags&4).
      (void)soft_sfx_ensure_voice_type(res_id, wav, (flags & 4) == 0);
      IDirectSoundBuffer* buf = ds_create_buffer(wav, eff);
      if (buf && ds_play(buf, loop)) {
        v.ds = true;
        v.buf = buf;
      } else if (buf) {
        ds_stop_release(buf);
      }
    }
  }
  if (!v.ds) {
    // Fallback: single-stream WinMM (stops prior PlaySound voices).
    if (!win_play(path_buf, loop)) return 0;
  }
#else
  if (!win_play(path_buf, loop)) return 0;
#endif

  const int32_t id = (instance != 0) ? instance : g_next_voice++;
  if (id >= g_next_voice) g_next_voice = id + 1;
  g_voices[id] = std::move(v);
  g_last_path = path_buf;
  return id;
}

void audio_sfx_stop(int32_t instance) {
  std::lock_guard<std::mutex> lock(g_mu);
  auto it = g_voices.find(instance);
  if (it == g_voices.end()) return;
#ifdef _WIN32
  if (it->second.ds) {
    ds_stop_release(it->second.buf);
  } else {
    // WinMM can't address a single voice — stop device.
    win_stop_all();
  }
#else
  win_stop_all();
#endif
  g_voices.erase(it);
}

int32_t audio_sfx_active_count() {
  std::lock_guard<std::mutex> lock(g_mu);
#ifdef _WIN32
  // Soft PE UpdateVoices pass1: finished DS buffers free without Stop first.
  for (auto it = g_voices.begin(); it != g_voices.end();) {
    if (it->second.ds && soft_sfx_ds_buf_is_playing(it->second.buf) == 0) {
      ds_stop_release(it->second.buf);
      it = g_voices.erase(it);
    } else {
      ++it;
    }
  }
#endif
  int32_t n = 0;
  for (const auto& kv : g_voices)
    if (kv.second.alive) ++n;
  return n;
}

const char* audio_sfx_last_path() {
  std::lock_guard<std::mutex> lock(g_mu);
  return g_last_path.c_str();
}

bool audio_sfx_voice_alive(int32_t instance) {
  std::lock_guard<std::mutex> lock(g_mu);
  auto it = g_voices.find(instance);
  if (it == g_voices.end() || !it->second.alive) return false;
#ifdef _WIN32
  // Soft PE Sfx_DSVoiceIsFinished GetStatus on host Voice.buf.
  if (it->second.ds && soft_sfx_ds_buf_is_playing(it->second.buf) == 0) {
    ds_stop_release(it->second.buf);
    g_voices.erase(it);
    return false;
  }
#endif
  return true;
}

int32_t audio_sfx_listener_set_pose(const float* pose12) {
  std::lock_guard<std::mutex> lock(g_mu);
  if (!pose12) return -1;
#ifdef _WIN32
  (void)ensure_ds();
  return soft_sfx_listener_set_pose(pose12);
#else
  // Soft non-Win: scatter packed 12 → padded BSS only (no DS Apply).
  g_sfx_listener_pose[0] = pose12[0];
  g_sfx_listener_pose[1] = pose12[1];
  g_sfx_listener_pose[2] = pose12[2];
  g_sfx_listener_pose[4] = pose12[3];
  g_sfx_listener_pose[5] = pose12[4];
  g_sfx_listener_pose[6] = pose12[5];
  g_sfx_listener_pose[8] = pose12[6];
  g_sfx_listener_pose[9] = pose12[7];
  g_sfx_listener_pose[10] = pose12[8];
  g_sfx_listener_pose[12] = pose12[9];
  g_sfx_listener_pose[13] = pose12[10];
  g_sfx_listener_pose[14] = pose12[11];
  return 0;
#endif
}

int32_t audio_sfx_update_voices() {
  std::lock_guard<std::mutex> lock(g_mu);
#ifdef _WIN32
  (void)ensure_ds();
  return soft_sfx_update_voices();
#else
  return 0;
#endif
}

int32_t audio_sfx_hw_voice_budget() {
  std::lock_guard<std::mutex> lock(g_mu);
#ifdef _WIN32
  (void)ensure_ds();
  return soft_sfx_hw_voice_budget();
#else
  return kSfxHwVoiceBudgetMin;
#endif
}

void audio_sfx_listener_get_pos(float* x, float* y, float* z) {
  std::lock_guard<std::mutex> lock(g_mu);
  if (x) *x = g_sfx_listener_pose[0];
  if (y) *y = g_sfx_listener_pose[1];
  if (z) *z = g_sfx_listener_pose[2];
}

}  // namespace inv
