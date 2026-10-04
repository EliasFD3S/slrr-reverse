#pragma once

#include <cstddef>
#include <cstdint>

namespace inv {

// Host audio (Phase 2.21+2.26): channel volumes, music-set, SfxRef voices.
// Prefers DirectSound secondary buffers (multi-voice); WinMM PlaySound fallback.

constexpr int32_t kAudioChannelEffects = 0;
constexpr int32_t kAudioChannelMusic = 1;
constexpr int32_t kAudioChannelEngine = 2;
constexpr int32_t kAudioSfxLoop = 0x1;
constexpr int32_t kAudioSfxNoAutoStop = static_cast<int32_t>(0x80000000u);

void audio_set_volume(int32_t channel, float volume);
float audio_get_volume(int32_t channel);
void audio_change_music_set(int32_t type);
int32_t audio_music_set();
void audio_music_next_track();
void audio_music_prev_track();
int32_t audio_music_track_count();
int32_t audio_music_track_index();
const char* audio_music_track_name();  // basename of current, or ""
bool audio_music_playing();

bool audio_ds_ready();
const char* audio_backend();  // "dsound" | "winmm" | "none"

// Soft PE Sfx_ListenerSetPose @ 0x005508F0 — packed 12 floats
// (xyz + ox/oy/oz + ux/uy/uz + fx/fy/fz) → padded Sfx_listener_* →
// Sfx_ApplyListenerToDS. Returns 0.
int32_t audio_sfx_listener_set_pose(const float* pose12);
// Soft PE Sfx_UpdateVoices @ 0x00550980 — HwVoiceBudget + qsort +
// typeUse + pass1/pass2 + CommitListenerDS. Returns 0.
int32_t audio_sfx_update_voices();
// Soft PE Sfx_HwVoiceBudget @ 0x00559DD0 (UpdateVoices head).
int32_t audio_sfx_hw_voice_budget();
// Soft PE Sfx_listener_x/y/z mirrors (cull / tests).
void audio_sfx_listener_get_pos(float* x, float* y, float* z);

// Resolve res_id → on-disk WAV (RPAK blob sourcefile / entry name heuristics).
bool audio_resolve_wav(int32_t res_id, char* out, size_t out_cap);

// Play SFX. If instance!=0 and that voice is live, returns it without restart.
// Returns voice id (>0) or 0 on failure.
int32_t audio_sfx_play(int32_t res_id, float pitch, float volume, int32_t flags,
                       int32_t instance);
void audio_sfx_stop(int32_t instance);
int32_t audio_sfx_active_count();
const char* audio_sfx_last_path();
bool audio_sfx_voice_alive(int32_t instance);

}  // namespace inv
