#include "audio_win32.hpp"
#include "rpak.hpp"
#include "render_d3d9.hpp"

#include <cmath>
#include <cstdio>
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
#endif

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
bool ensure_ds() {
  if (g_ds_tried) return g_ds_ok;
  g_ds_tried = true;
  HRESULT hr = DirectSoundCreate8(nullptr, &g_ds, nullptr);
  if (FAILED(hr) || !g_ds) return false;
  HWND hwnd = reinterpret_cast<HWND>(render_d3d9_hwnd());
  if (!hwnd) hwnd = GetDesktopWindow();
  hr = g_ds->SetCooperativeLevel(hwnd, DSSCL_PRIORITY);
  if (FAILED(hr)) {
    g_ds->Release();
    g_ds = nullptr;
    return false;
  }
  g_ds_ok = true;
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
  return it != g_voices.end() && it->second.alive;
}

}  // namespace inv
