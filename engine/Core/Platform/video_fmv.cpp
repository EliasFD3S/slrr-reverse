#include "video_fmv.hpp"
#include "render_d3d9.hpp"
#include "rpak.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dshow.h>
#include <d3d9.h>

// qedit.h removed from modern SDKs — local Sample Grabber defs (stock path uses DS).
struct __declspec(uuid("0579154A-2B53-4994-B0D0-E773148EFF85")) ISampleGrabberCB
    : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE SampleCB(double, IMediaSample*) = 0;
  virtual HRESULT STDMETHODCALLTYPE BufferCB(double, BYTE*, long) = 0;
};

struct __declspec(uuid("6B652FFF-11FE-4fce-92AD-0266B5D7C78F")) ISampleGrabber
    : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE SetOneShot(BOOL) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetMediaType(const AM_MEDIA_TYPE*) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetConnectedMediaType(AM_MEDIA_TYPE*) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetBufferSamples(BOOL) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetCurrentBuffer(long*, long*) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetCurrentSample(IMediaSample**) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetCallback(ISampleGrabberCB*, long) = 0;
};

static const GUID kClsidSampleGrabber = {
    0xC1F400A0, 0x3F08, 0x11d3, {0x9F, 0x0B, 0x00, 0x60, 0x08, 0x03, 0x9E, 0x37}};
static const GUID kClsidNullRenderer = {
    0xC1F400A4, 0x3F08, 0x11d3, {0x9F, 0x0B, 0x00, 0x60, 0x08, 0x03, 0x9E, 0x37}};
static const GUID kIidISampleGrabber = {
    0x6B652FFF, 0x11FE, 0x4fce, {0x92, 0xAD, 0x02, 0x66, 0xB5, 0xD7, 0xC7, 0x8F}};
#endif

namespace inv {
namespace {

#ifdef _WIN32
IGraphBuilder* g_graph = nullptr;
IMediaControl* g_control = nullptr;
IMediaSeeking* g_seeking = nullptr;
IMediaEvent* g_event = nullptr;
ISampleGrabber* g_grabber = nullptr;
IDirect3DTexture9* g_tex = nullptr;
bool g_com = false;
bool g_playing = false;
bool g_loop = false;
int g_w = 0;
int g_h = 0;
std::vector<uint8_t> g_scratch;

void release_graph() {
  if (g_control) {
    g_control->Stop();
  }
  if (g_tex) {
    g_tex->Release();
    g_tex = nullptr;
  }
  if (g_grabber) {
    g_grabber->Release();
    g_grabber = nullptr;
  }
  if (g_event) {
    g_event->Release();
    g_event = nullptr;
  }
  if (g_seeking) {
    g_seeking->Release();
    g_seeking = nullptr;
  }
  if (g_control) {
    g_control->Release();
    g_control = nullptr;
  }
  if (g_graph) {
    g_graph->Release();
    g_graph = nullptr;
  }
  g_playing = false;
  g_w = g_h = 0;
  g_scratch.clear();
}

bool ensure_com() {
  if (g_com) return true;
  const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE) {
    g_com = true;
    return true;
  }
  return false;
}

std::wstring to_wide(const char* path) {
  if (!path) return {};
  const int n = MultiByteToWideChar(CP_ACP, 0, path, -1, nullptr, 0);
  if (n <= 0) return {};
  std::wstring out(static_cast<size_t>(n - 1), L'\0');
  MultiByteToWideChar(CP_ACP, 0, path, -1, out.data(), n);
  return out;
}

HRESULT get_pin(IBaseFilter* filter, PIN_DIRECTION dir, IPin** out) {
  *out = nullptr;
  IEnumPins* en = nullptr;
  if (FAILED(filter->EnumPins(&en)) || !en) return E_FAIL;
  IPin* pin = nullptr;
  while (en->Next(1, &pin, nullptr) == S_OK) {
    PIN_DIRECTION d;
    if (SUCCEEDED(pin->QueryDirection(&d)) && d == dir) {
      *out = pin;
      en->Release();
      return S_OK;
    }
    pin->Release();
  }
  en->Release();
  return E_FAIL;
}

bool build_graph(const wchar_t* wpath) {
  HRESULT hr = CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IGraphBuilder, reinterpret_cast<void**>(&g_graph));
  if (FAILED(hr) || !g_graph) return false;

  g_graph->QueryInterface(IID_IMediaControl, reinterpret_cast<void**>(&g_control));
  g_graph->QueryInterface(IID_IMediaSeeking, reinterpret_cast<void**>(&g_seeking));
  g_graph->QueryInterface(IID_IMediaEvent, reinterpret_cast<void**>(&g_event));
  if (!g_control) {
    release_graph();
    return false;
  }

  IBaseFilter* src = nullptr;
  hr = g_graph->AddSourceFilter(wpath, L"Source", &src);
  if (FAILED(hr) || !src) {
    release_graph();
    return false;
  }

  IBaseFilter* grab_f = nullptr;
  hr = CoCreateInstance(kClsidSampleGrabber, nullptr, CLSCTX_INPROC_SERVER,
                        IID_IBaseFilter, reinterpret_cast<void**>(&grab_f));
  if (FAILED(hr) || !grab_f) {
    src->Release();
    release_graph();
    return false;
  }
  g_graph->AddFilter(grab_f, L"Grabber");
  grab_f->QueryInterface(kIidISampleGrabber, reinterpret_cast<void**>(&g_grabber));
  if (!g_grabber) {
    grab_f->Release();
    src->Release();
    release_graph();
    return false;
  }

  AM_MEDIA_TYPE mt{};
  mt.majortype = MEDIATYPE_Video;
  mt.subtype = MEDIASUBTYPE_RGB32;
  mt.formattype = FORMAT_VideoInfo;
  g_grabber->SetMediaType(&mt);
  g_grabber->SetBufferSamples(TRUE);
  g_grabber->SetOneShot(FALSE);

  IBaseFilter* null_f = nullptr;
  hr = CoCreateInstance(kClsidNullRenderer, nullptr, CLSCTX_INPROC_SERVER,
                        IID_IBaseFilter, reinterpret_cast<void**>(&null_f));
  if (FAILED(hr) || !null_f) {
    grab_f->Release();
    src->Release();
    release_graph();
    return false;
  }
  g_graph->AddFilter(null_f, L"Null");

  IPin* src_out = nullptr;
  IPin* grab_in = nullptr;
  IPin* grab_out = nullptr;
  IPin* null_in = nullptr;
  get_pin(src, PINDIR_OUTPUT, &src_out);
  get_pin(grab_f, PINDIR_INPUT, &grab_in);
  get_pin(grab_f, PINDIR_OUTPUT, &grab_out);
  get_pin(null_f, PINDIR_INPUT, &null_in);
  bool ok = src_out && grab_in && grab_out && null_in &&
            SUCCEEDED(g_graph->Connect(src_out, grab_in)) &&
            SUCCEEDED(g_graph->Connect(grab_out, null_in));
  if (src_out) src_out->Release();
  if (grab_in) grab_in->Release();
  if (grab_out) grab_out->Release();
  if (null_in) null_in->Release();
  grab_f->Release();
  null_f->Release();
  src->Release();

  if (!ok) {
    // Fallback: let the graph build freely (may attach default renderer).
    release_graph();
    hr = CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER,
                          IID_IGraphBuilder, reinterpret_cast<void**>(&g_graph));
    if (FAILED(hr) || !g_graph) return false;
    g_graph->QueryInterface(IID_IMediaControl,
                            reinterpret_cast<void**>(&g_control));
    g_graph->QueryInterface(IID_IMediaSeeking,
                            reinterpret_cast<void**>(&g_seeking));
    g_graph->QueryInterface(IID_IMediaEvent, reinterpret_cast<void**>(&g_event));
    if (!g_control || FAILED(g_graph->RenderFile(wpath, nullptr))) {
      release_graph();
      return false;
    }
    // Playing without texture grab still counts as success for stock contract.
    return true;
  }

  AM_MEDIA_TYPE connected{};
  if (SUCCEEDED(g_grabber->GetConnectedMediaType(&connected))) {
    if (connected.formattype == FORMAT_VideoInfo && connected.pbFormat) {
      auto* vih = reinterpret_cast<VIDEOINFOHEADER*>(connected.pbFormat);
      g_w = vih->bmiHeader.biWidth;
      g_h = vih->bmiHeader.biHeight;
      if (g_h < 0) g_h = -g_h;
    }
    if (connected.pbFormat) CoTaskMemFree(connected.pbFormat);
    if (connected.pUnk) connected.pUnk->Release();
  }
  return true;
}

void handle_events() {
  if (!g_event) return;
  long code = 0;
  LONG_PTR p1 = 0, p2 = 0;
  while (g_event->GetEvent(&code, &p1, &p2, 0) == S_OK) {
    if (code == EC_COMPLETE) {
      if (g_loop && g_seeking && g_control) {
        LONGLONG zero = 0;
        g_seeking->SetPositions(&zero, AM_SEEKING_AbsolutePositioning, nullptr,
                                AM_SEEKING_NoPositioning);
        g_control->Run();
      } else {
        // Boot intros (Activision/Invictus): non-loop — stock ends the clip.
        g_playing = false;
      }
    }
    g_event->FreeEventParams(code, p1, p2);
  }
}

void upload_and_draw() {
  if (!g_grabber || !g_playing) return;
  handle_events();

  long sz = 0;
  if (FAILED(g_grabber->GetCurrentBuffer(&sz, nullptr)) || sz <= 0) return;
  if (static_cast<long>(g_scratch.size()) < sz) g_scratch.resize(static_cast<size_t>(sz));
  if (FAILED(g_grabber->GetCurrentBuffer(&sz, reinterpret_cast<long*>(g_scratch.data()))))
    return;

  auto* dev = reinterpret_cast<IDirect3DDevice9*>(render_d3d9_device());
  if (!dev || g_w <= 0 || g_h <= 0) return;

  if (!g_tex) {
    if (FAILED(dev->CreateTexture(static_cast<UINT>(g_w), static_cast<UINT>(g_h), 1,
                                  0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_tex,
                                  nullptr))) {
      return;
    }
  }

  D3DLOCKED_RECT lr{};
  if (FAILED(g_tex->LockRect(0, &lr, nullptr, 0))) return;
  const int src_pitch = g_w * 4;
  // DirectShow RGB32 is typically bottom-up.
  for (int y = 0; y < g_h; ++y) {
    const uint8_t* src =
        g_scratch.data() + static_cast<size_t>(g_h - 1 - y) * static_cast<size_t>(src_pitch);
    uint8_t* dst = static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch;
    std::memcpy(dst, src, static_cast<size_t>(src_pitch));
  }
  g_tex->UnlockRect(0);
  render_d3d9_draw_video_texture(g_tex, g_w, g_h);
}
#endif

}  // namespace

int32_t video_fmv_open(const char* path, int32_t non_exclusive, int32_t loop) {
  (void)non_exclusive;
#ifdef _WIN32
  video_fmv_close();
  if (!path || !path[0]) return -1;
  std::string resolved = rpak_resolve_path(path);
  if (resolved.empty()) resolved = path;
  const DWORD attr = GetFileAttributesA(resolved.c_str());
  if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) {
    std::printf("[fmv] missing %s\n", resolved.c_str());
    return -1;
  }
  if (!ensure_com()) return -1;
  const std::wstring wpath = to_wide(resolved.c_str());
  if (wpath.empty() || !build_graph(wpath.c_str())) {
    std::printf("[fmv] DirectShow open failed: %s\n", resolved.c_str());
    return -1;
  }
  g_loop = loop != 0;
  if (FAILED(g_control->Run())) {
    release_graph();
    return -1;
  }
  g_playing = true;
  std::printf("[fmv] open ok %s loop=%d %dx%d\n", resolved.c_str(), g_loop ? 1 : 0,
              g_w, g_h);
  return 0;
#else
  (void)path;
  (void)loop;
  return -1;
#endif
}

void video_fmv_close() {
#ifdef _WIN32
  release_graph();
#endif
}

int32_t video_fmv_is_playing() {
#ifdef _WIN32
  return g_playing ? 1 : 0;
#else
  return 0;
#endif
}

void video_fmv_present() {
#ifdef _WIN32
  if (g_playing) upload_and_draw();
#endif
}

int32_t video_fmv_width() {
#ifdef _WIN32
  return g_w;
#else
  return 0;
#endif
}

int32_t video_fmv_height() {
#ifdef _WIN32
  return g_h;
#else
  return 0;
#endif
}

int32_t video_fmv_play_boot_intros(int32_t max_frames_each) {
  // Stock Engine_boot @ 0x58C934 table (3 slots). Installs ship Activision +
  // Invictus only — StreetLegal.avi is an SL1 leftover, almost never present.
  static const char* kPaths[] = {
      "Data\\FMV\\Activision.avi",
      "Data\\FMV\\Invictus.avi",
      "Data\\FMV\\StreetLegal.avi",  // SL1 leftover; File_PathExists → skip
  };
  int32_t played = 0;
#ifdef _WIN32
  // Stock FMV_Boot_PlayPath_DirectShow @ 0x55C470: GetAsyncKeyState(VK_ESCAPE).
  auto esc_down = []() -> bool {
    return (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
  };
  if (HWND hwnd = static_cast<HWND>(render_d3d9_hwnd())) {
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);
  }
  for (const char* path : kPaths) {
    if (video_fmv_open(path, /*non_exclusive=*/0, /*loop=*/0) != 0) continue;
    ++played;
    // Stock: if ESC already held at clip start, skip the wait entirely.
    if (esc_down()) {
      std::printf("[fmv] skip ESC (held) %s\n", path);
      video_fmv_close();
      continue;
    }
    int32_t frames = 0;
    while (video_fmv_is_playing()) {
      // Pump then poll ESC — same order as stock PeekMessage + GetAsyncKeyState.
      render_d3d9_flush();
      if (esc_down()) {
        std::printf("[fmv] skip ESC %s\n", path);
        break;
      }
      if (render_d3d9_quit_requested()) break;
      ++frames;
      if (max_frames_each > 0 && frames >= max_frames_each) break;
      Sleep(1);
    }
    video_fmv_close();
    if (render_d3d9_quit_requested()) break;
  }
  // Sticky ESC from FMV skip must not hit --game's AXIS_CANCEL→quit mapping.
  while (esc_down()) {
    render_d3d9_pump(0);
    Sleep(1);
    if (render_d3d9_quit_requested()) break;
  }
#else
  (void)max_frames_each;
#endif
  std::printf("[fmv] boot intros played=%d (Activision+Invictus; StreetLegal=SL1 leftover)\n",
              played);
  return played;
}

}  // namespace inv
