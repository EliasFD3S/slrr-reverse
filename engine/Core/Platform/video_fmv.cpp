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

// Stock FMV_DirectShow_Open @ 0x4F921F: IVideoWindow::put_WindowStyle(0x46000000)
// = WS_CHILD|WS_CLIPSIBLINGS|WS_CLIPCHILDREN (int_convert 1174405120).
static constexpr long kFmvExclusiveWindowStyle = 0x46000000L;
// Stock SetNotifyWindow message id @ 0x4F9209 (int_convert 1037 = 0x40D).
static constexpr long kFmvNotifyMsg = 0x40D;
// Soft PE Open/Close fail HRESULT @ 0x4F8DEE / 0x4F909B (int_convert -2147467259).
static constexpr int32_t kFmvEFail = static_cast<int32_t>(0x80004005u);
// Soft PE CreateTextures format @ 0x4F9B6D / 0x4F9C26 (int_convert 22 / 25).
static constexpr D3DFORMAT kFmvTexFmtXrgb = D3DFMT_X8R8G8B8;  // 22
static constexpr D3DFORMAT kFmvTexFmtArgb = D3DFMT_A8R8G8B8;  // 25
#endif

namespace inv {
namespace {

#ifdef _WIN32
IGraphBuilder* g_graph = nullptr;
IMediaControl* g_control = nullptr;
IMediaSeeking* g_seeking = nullptr;
IMediaEvent* g_event = nullptr;
IVideoWindow* g_vwin = nullptr;
IBasicVideo* g_bvideo = nullptr;
ISampleGrabber* g_grabber = nullptr;
IDirect3DTexture9* g_tex = nullptr;
bool g_com = false;
// Soft PE FMV_dsReady @ 0x64A390 — DS subsystem ready (Open/Close gate).
bool g_ds_ready = false;
bool g_playing = false;
bool g_loop = false;
// Stock FMV_nonExclusive @ 0x61852C — !=0 TextureRenderer / ==0 exclusive HWND.
bool g_non_exclusive = false;
int g_w = 0;
int g_h = 0;
// Soft PE FMV_TextureRenderer_CreateTextures @ 0x4F9AA0: tex size may be
// pow2 (else NPOT exact). UpdateQuad UVs = video/tex.
int g_tex_w = 0;
int g_tex_h = 0;
// Soft PE of FMV_pathBuf @ 0x64A160 (stock strcpy in Open).
char g_path[MAX_PATH] = {};
std::vector<uint8_t> g_scratch;

// Soft PE cache of FMV_TextureRenderer_UpdateQuad @ 0x4F8B30 letterbox+UV.
struct FmvUpdateQuad {
  float left = 0.f;
  float top = 0.f;
  float draw_w = 0.f;
  float draw_h = 0.f;
  float u_max = 1.f;
  float v_max = 1.f;
  int screen_w = 0;
  int screen_h = 0;
  bool valid = false;
} g_quad;

void set_notify_window(HWND hwnd, long msg) {
  // Soft PE Open @ 0x4F9209 / Close @ 0x4F9370: IMediaEventEx::SetNotifyWindow.
  if (!g_event) return;
  IMediaEventEx* evx = nullptr;
  if (SUCCEEDED(g_event->QueryInterface(IID_IMediaEventEx, reinterpret_cast<void**>(&evx))) &&
      evx) {
    evx->SetNotifyWindow(reinterpret_cast<OAHWND>(hwnd), msg, 0);
    evx->Release();
  }
}

void release_graph() {
  // Soft PE of FMV_DirectShow_Close @ 0x4F9320:
  // clear FMV_playing → StopWhenReady+GetState drain → SetNotifyWindow(0,0,0)
  // → Release Seeking→Event→Control→Graph→VW→BV (stock; VB/device restore OOS).
  // Soft intro→menu: Boot_PlayPath @ 0x55C60F put_Visible(0) before VW Release
  // (host boot intros reuse Open/Close, not the separate 0x55C470 graph).
  const bool exclusive_hwnd = g_vwin != nullptr && !g_non_exclusive;
  g_playing = false;
  if (g_control) {
    // Soft PE @ 0x4F9347..0x4F935F: StopWhenReady each GetState poll until Stopped.
    OAFilterState st = State_Running;
    do {
      g_control->StopWhenReady();
      if (FAILED(g_control->GetState(0, &st))) break;
    } while (st != State_Stopped);
  }
  set_notify_window(nullptr, 0);
  if (exclusive_hwnd && g_vwin) {
    // Soft PE Boot @ 0x55C60F: IVideoWindow::put_Visible(0) (vt+0x4C).
    g_vwin->put_Visible(OAFALSE);
  }
  // Host TextureRenderer stand-in (stock FMV_videoVB / filter tear OOS).
  if (g_tex) {
    g_tex->Release();
    g_tex = nullptr;
  }
  if (g_grabber) {
    g_grabber->Release();
    g_grabber = nullptr;
  }
  // Soft PE Close release order @ 0x4F939B..0x4F9428 (skip unnamed 64A370/36C).
  if (g_seeking) {
    g_seeking->Release();
    g_seeking = nullptr;
  }
  if (g_event) {
    g_event->Release();
    g_event = nullptr;
  }
  if (g_control) {
    g_control->Release();
    g_control = nullptr;
  }
  if (g_graph) {
    g_graph->Release();
    g_graph = nullptr;
  }
  if (g_vwin) {
    g_vwin->Release();
    g_vwin = nullptr;
  }
  if (g_bvideo) {
    g_bvideo->Release();
    g_bvideo = nullptr;
  }
  g_non_exclusive = false;
  g_w = g_h = 0;
  g_tex_w = g_tex_h = 0;
  g_quad = {};
  g_path[0] = '\0';
  g_scratch.clear();
}

bool ensure_com() {
  // Soft PE FMV_dsReady @ 0x64A390: Open/Close gate after COM+DS available.
  if (g_com && g_ds_ready) return true;
  const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE) {
    g_com = true;
    g_ds_ready = true;
    return true;
  }
  g_ds_ready = false;
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

// Soft PE Open @ 0x4F8FA8: IBaseFilter::FindPin(L"Output", …) before Render.
HRESULT find_pin_named(IBaseFilter* filter, const wchar_t* name, IPin** out) {
  *out = nullptr;
  if (!filter || !name) return E_POINTER;
  const HRESULT hr = filter->FindPin(name, out);
  if (SUCCEEDED(hr) && *out) return hr;
  if (*out) {
    (*out)->Release();
    *out = nullptr;
  }
  // Soft residual: some source filters omit the stock "Output" id.
  return get_pin(filter, PINDIR_OUTPUT, out);
}

// Soft PE Open @ 0x4F9180: QI IMediaControl, IMediaSeeking, IMediaEvent,
// IVideoWindow, IBasicVideo — stock any fail → Close + E_FAIL.
// Soft residual TextureRenderer stand-in (SampleGrabber+Null): NullRenderer
// often lacks IVideoWindow/IBasicVideo — require those only for exclusive.
bool qi_open_interfaces(bool require_video_ifaces) {
  if (!g_graph) return false;
  if (FAILED(g_graph->QueryInterface(IID_IMediaControl,
                                     reinterpret_cast<void**>(&g_control))) ||
      !g_control)
    return false;
  if (FAILED(g_graph->QueryInterface(IID_IMediaSeeking,
                                     reinterpret_cast<void**>(&g_seeking))) ||
      !g_seeking)
    return false;
  if (FAILED(g_graph->QueryInterface(IID_IMediaEvent,
                                     reinterpret_cast<void**>(&g_event))) ||
      !g_event)
    return false;
  g_graph->QueryInterface(IID_IVideoWindow, reinterpret_cast<void**>(&g_vwin));
  g_graph->QueryInterface(IID_IBasicVideo, reinterpret_cast<void**>(&g_bvideo));
  if (require_video_ifaces && (!g_vwin || !g_bvideo)) return false;
  return true;
}

HRESULT set_notify_window_hr(HWND hwnd, long msg) {
  // Soft PE Open @ 0x4F9209: IMediaEventEx::SetNotifyWindow — HRESULT kept.
  if (!g_event) return E_NOINTERFACE;
  IMediaEventEx* evx = nullptr;
  HRESULT hr =
      g_event->QueryInterface(IID_IMediaEventEx, reinterpret_cast<void**>(&evx));
  if (FAILED(hr) || !evx) return FAILED(hr) ? hr : E_NOINTERFACE;
  hr = evx->SetNotifyWindow(reinterpret_cast<OAHWND>(hwnd), msg, 0);
  evx->Release();
  return hr;
}

// Soft PE TextureRenderer branch @ 0x4F8E56 (expects g_graph already created):
// AddFilter(TEXTURERENDERER) → AddSourceFilter(SOURCE) → FindPin(Output) →
// Render. Host stand-in: SampleGrabber RGB32 + NullRenderer sink.
HRESULT build_graph_texture(const wchar_t* wpath) {
  if (!g_graph || !wpath) return E_POINTER;

  IBaseFilter* grab_f = nullptr;
  HRESULT hr = CoCreateInstance(kClsidSampleGrabber, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IBaseFilter, reinterpret_cast<void**>(&grab_f));
  if (FAILED(hr) || !grab_f) {
    return FAILED(hr) ? hr : static_cast<HRESULT>(kFmvEFail);
  }
  // Soft PE @ 0x4F8EB4: AddFilter(…, L"TEXTURERENDERER") before source.
  hr = g_graph->AddFilter(grab_f, L"TEXTURERENDERER");
  if (FAILED(hr)) {
    grab_f->Release();
    return hr;
  }
  grab_f->QueryInterface(kIidISampleGrabber, reinterpret_cast<void**>(&g_grabber));
  if (!g_grabber) {
    grab_f->Release();
    return static_cast<HRESULT>(kFmvEFail);
  }

  AM_MEDIA_TYPE mt{};
  mt.majortype = MEDIATYPE_Video;
  mt.subtype = MEDIASUBTYPE_RGB32;
  mt.formattype = FORMAT_VideoInfo;
  g_grabber->SetMediaType(&mt);
  g_grabber->SetBufferSamples(TRUE);
  g_grabber->SetOneShot(FALSE);

  // Soft sink: stock TextureRenderer is itself a renderer; SampleGrabber needs
  // NullRenderer after it so Render/Connect can complete the chain.
  IBaseFilter* null_f = nullptr;
  hr = CoCreateInstance(kClsidNullRenderer, nullptr, CLSCTX_INPROC_SERVER,
                        IID_IBaseFilter, reinterpret_cast<void**>(&null_f));
  if (FAILED(hr) || !null_f) {
    grab_f->Release();
    return FAILED(hr) ? hr : static_cast<HRESULT>(kFmvEFail);
  }
  hr = g_graph->AddFilter(null_f, L"Null");
  if (FAILED(hr)) {
    null_f->Release();
    grab_f->Release();
    return hr;
  }

  IBaseFilter* src = nullptr;
  // Soft PE @ 0x4F8F44: AddSourceFilter(BSTR path, L"SOURCE", …).
  hr = g_graph->AddSourceFilter(wpath, L"SOURCE", &src);
  if (FAILED(hr) || !src) {
    null_f->Release();
    grab_f->Release();
    return FAILED(hr) ? hr : static_cast<HRESULT>(kFmvEFail);
  }

  IPin* src_out = nullptr;
  hr = find_pin_named(src, L"Output", &src_out);
  if (FAILED(hr) || !src_out) {
    src->Release();
    null_f->Release();
    grab_f->Release();
    return FAILED(hr) ? hr : static_cast<HRESULT>(kFmvEFail);
  }

  // Soft PE @ 0x4F8FEF: IGraphBuilder::Render(output pin). Soft residual:
  // if intelligent connect misses grabber→null, or Render wired another
  // renderer, Disconnect then manual Connect chain.
  hr = g_graph->Render(src_out);
  bool grab_linked = false;
  if (SUCCEEDED(hr) && g_grabber) {
    AM_MEDIA_TYPE probe{};
    if (SUCCEEDED(g_grabber->GetConnectedMediaType(&probe))) {
      grab_linked = true;
      if (probe.pbFormat) CoTaskMemFree(probe.pbFormat);
      if (probe.pUnk) probe.pUnk->Release();
    }
  }
  if (!grab_linked) {
    IPin* peer = nullptr;
    if (SUCCEEDED(src_out->ConnectedTo(&peer)) && peer) {
      g_graph->Disconnect(src_out);
      g_graph->Disconnect(peer);
      peer->Release();
    }
    IPin* grab_in = nullptr;
    IPin* grab_out = nullptr;
    IPin* null_in = nullptr;
    get_pin(grab_f, PINDIR_INPUT, &grab_in);
    get_pin(grab_f, PINDIR_OUTPUT, &grab_out);
    get_pin(null_f, PINDIR_INPUT, &null_in);
    if (grab_in && grab_out && null_in &&
        SUCCEEDED(g_graph->Connect(src_out, grab_in)) &&
        SUCCEEDED(g_graph->Connect(grab_out, null_in))) {
      hr = S_OK;
      grab_linked = true;
    } else if (SUCCEEDED(hr) && !grab_linked) {
      hr = static_cast<HRESULT>(kFmvEFail);
    }
    if (grab_in) grab_in->Release();
    if (grab_out) grab_out->Release();
    if (null_in) null_in->Release();
  }
  src_out->Release();
  null_f->Release();
  grab_f->Release();
  src->Release();
  if (FAILED(hr) || !grab_linked) {
    return FAILED(hr) ? hr : static_cast<HRESULT>(kFmvEFail);
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
  return S_OK;
}

// Soft PE exclusive branch @ 0x4F909E: RenderFile only (HWND wired after QI).
HRESULT build_graph_exclusive(const wchar_t* wpath) {
  if (!g_graph || !wpath) return E_POINTER;
  // Soft PE @ 0x4F90EF: IGraphBuilder::RenderFile(BSTR, nullptr) — fail → E_FAIL.
  const HRESULT hr = g_graph->RenderFile(wpath, nullptr);
  return FAILED(hr) ? static_cast<HRESULT>(kFmvEFail) : S_OK;
}

// Soft PE Open @ 0x4F9220+: exclusive only — put_Owner → put_WindowStyle →
// SetWindowPosition iff IBasicVideo present (else skip position, hr=0).
HRESULT setup_exclusive_hwnd() {
  HWND hwnd = static_cast<HWND>(render_d3d9_hwnd());
  if (!g_vwin || !hwnd) return S_OK;
  HRESULT hr = g_vwin->put_Owner(reinterpret_cast<OAHWND>(hwnd));
  if (FAILED(hr)) return hr;
  hr = g_vwin->put_WindowStyle(kFmvExclusiveWindowStyle);
  if (FAILED(hr)) return hr;
  if (g_bvideo) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    // Soft PE @ 0x4F926x: SetWindowPosition(left, top, right, bottom).
    hr = g_vwin->SetWindowPosition(rc.left, rc.top, rc.right, rc.bottom);
    if (FAILED(hr)) return hr;
    long vw = 0, vh = 0;
    if (SUCCEEDED(g_bvideo->GetVideoSize(&vw, &vh)) && vw > 0 && vh > 0) {
      g_w = static_cast<int>(vw);
      g_h = static_cast<int>(vh);
    }
  }
  return S_OK;
}

void handle_events() {
  if (!g_event) return;
  long code = 0;
  LONG_PTR p1 = 0, p2 = 0;
  while (g_event->GetEvent(&code, &p1, &p2, 0) == S_OK) {
    if (code == EC_COMPLETE) {
      // Stock FMV_loopFlag @ 0x64A38C — EC_COMPLETE seek+Run when loop.
      if (g_loop && g_seeking && g_control) {
        LONGLONG zero = 0;
        g_seeking->SetPositions(&zero, AM_SEEKING_AbsolutePositioning, nullptr,
                                AM_SEEKING_NoPositioning);
        g_control->Run();
      } else {
        // Boot intros / non-loop: stock clears FMV_playing.
        g_playing = false;
      }
    }
    g_event->FreeEventParams(code, p1, p2);
  }
}

// Soft PE of FMV_TextureRenderer_CreateTextures @ 0x4F9AA0 pow2 branch:
// while (p < n) p *= 2; NPOT caps use exact video size.
int pow2_ceil_ge2(int n) {
  int p = 2;
  if (n <= 2) return 2;
  while (p < n) p *= 2;
  return p;
}

// Soft PE of FMV_TextureRenderer_UpdateQuad @ 0x4F8B30:
// GetBackBufferSize → letterbox video aspect with −0.5 half-texel → UV =
// videoW/texW, videoH/texH. Stock fills FMV_videoVB (FVF 0x104); host caches
// the same rect/UV for DrawPrimitiveUP.
void texture_renderer_update_quad() {
  g_quad.valid = false;
  const int sw = render_d3d9_width();
  const int sh = render_d3d9_height();
  if (sw <= 0 || sh <= 0 || g_w <= 0 || g_h <= 0) return;

  const int tex_w = g_tex_w > 0 ? g_tex_w : g_w;
  const int tex_h = g_tex_h > 0 ? g_tex_h : g_h;
  if (tex_w <= 0 || tex_h <= 0) return;

  const float screen_w = static_cast<float>(sw);
  const float screen_h = static_cast<float>(sh);
  const float video_w = static_cast<float>(g_w);
  const float video_h = static_cast<float>(g_h);
  float left = -0.5f;
  float top = -0.5f;
  float draw_w = screen_w;
  float draw_h = screen_h;
  const float video_aspect = video_w / video_h;
  const float screen_aspect = screen_w / screen_h;
  // Stock @ 0x4F8BDC: if videoAspect <= screenAspect → pillarbox width.
  if (video_aspect <= screen_aspect) {
    draw_w = screen_h * video_aspect;
    left = (screen_w - draw_w) * 0.5f - 0.5f;
  } else {
    draw_h = screen_w / video_aspect;
    top = (screen_h - draw_h) * 0.5f - 0.5f;
  }

  g_quad.left = left;
  g_quad.top = top;
  g_quad.draw_w = draw_w;
  g_quad.draw_h = draw_h;
  g_quad.u_max = video_w / static_cast<float>(tex_w);
  g_quad.v_max = video_h / static_cast<float>(tex_h);
  g_quad.screen_w = sw;
  g_quad.screen_h = sh;
  g_quad.valid = true;
}

// Soft PE residual FMV_TextureRenderer_CreateTextures @ 0x4F9AA0:
// CreateTexture format 22 (X8R8G8B8) first, else 25 (A8R8G8B8); NPOT exact
// if possible, else pow2 stand-in.
bool ensure_video_texture(IDirect3DDevice9* dev) {
  if (g_tex) return true;
  if (!dev || g_w <= 0 || g_h <= 0) return false;

  auto try_create = [&](UINT tw, UINT th, D3DFORMAT fmt) -> bool {
    IDirect3DTexture9* tex = nullptr;
    if (FAILED(dev->CreateTexture(tw, th, 1, 0, fmt, D3DPOOL_MANAGED, &tex,
                                  nullptr)) ||
        !tex) {
      return false;
    }
    g_tex = tex;
    g_tex_w = static_cast<int>(tw);
    g_tex_h = static_cast<int>(th);
    D3DSURFACE_DESC desc{};
    if (SUCCEEDED(g_tex->GetLevelDesc(0, &desc))) {
      g_tex_w = static_cast<int>(desc.Width);
      g_tex_h = static_cast<int>(desc.Height);
    }
    return true;
  };

  auto try_sizes = [&](D3DFORMAT fmt) -> bool {
    // Stock NPOT branch @ 0x4F9B18 uses exact video dims when caps allow.
    if (try_create(static_cast<UINT>(g_w), static_cast<UINT>(g_h), fmt))
      return true;
    // Soft PE pow2 residual @ 0x4F9B1A..0x4F9B3A.
    return try_create(static_cast<UINT>(pow2_ceil_ge2(g_w)),
                      static_cast<UINT>(pow2_ceil_ge2(g_h)), fmt);
  };

  // Soft PE @ 0x4F9B6D fmt=22 then accept 22|25 @ 0x4F9C26.
  if (try_sizes(kFmvTexFmtXrgb)) return true;
  return try_sizes(kFmvTexFmtArgb);
}

// Soft PE of FMV_TextureRenderer_CopySample @ 0x4F9C60 (RGB32 stand-in):
// LockRect → copy video rect into top-left; keep bottom-up for UpdateQuad V
// invert (stock RGB24→XRGB; host RGB32 memcpy). Padding rows left untouched.
void copy_sample_to_texture() {
  if (!g_tex || !g_grabber || g_w <= 0 || g_h <= 0) return;
  long sz = 0;
  if (FAILED(g_grabber->GetCurrentBuffer(&sz, nullptr)) || sz <= 0) return;
  if (static_cast<long>(g_scratch.size()) < sz) g_scratch.resize(static_cast<size_t>(sz));
  if (FAILED(g_grabber->GetCurrentBuffer(&sz, reinterpret_cast<long*>(g_scratch.data()))))
    return;

  D3DLOCKED_RECT lr{};
  if (FAILED(g_tex->LockRect(0, &lr, nullptr, 0))) return;
  const int src_pitch = g_w * 4;
  // Soft PE: no host flip — stock CopySample writes bottom-up; UpdateQuad
  // inverts V in the VB @ 0x4F8CAA..
  for (int y = 0; y < g_h; ++y) {
    const uint8_t* src =
        g_scratch.data() + static_cast<size_t>(y) * static_cast<size_t>(src_pitch);
    uint8_t* dst = static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch;
    std::memcpy(dst, src, static_cast<size_t>(src_pitch));
  }
  g_tex->UnlockRect(0);
}

void draw_update_quad(IDirect3DDevice9* dev) {
  if (!dev || !g_tex || !g_quad.valid) return;
  // Stock FVF 0x104 = D3DFVF_XYZRHW|D3DFVF_TEX1 (int_convert 260); 24 B/vert.
  struct FmvVtx {
    float x, y, z, rhw, u, v;
  };
  constexpr DWORD kFvf = D3DFVF_XYZRHW | D3DFVF_TEX1;
  const float L = g_quad.left;
  const float T = g_quad.top;
  const float R = L + g_quad.draw_w;
  const float B = T + g_quad.draw_h;
  const float uw = g_quad.u_max;
  const float vh = g_quad.v_max;
  // Soft PE UpdateQuad VB strip @ 0x4F8C7A..: BL,BR,TL,TR with V inverted.
  const FmvVtx v[4] = {
      {L, B, 0.f, 1.f, 0.f, 0.f},
      {R, B, 0.f, 1.f, uw, 0.f},
      {L, T, 0.f, 1.f, 0.f, vh},
      {R, T, 0.f, 1.f, uw, vh},
  };
  dev->SetFVF(kFvf);
  dev->SetRenderState(D3DRS_ZENABLE, FALSE);
  dev->SetRenderState(D3DRS_LIGHTING, FALSE);
  dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
  dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
  dev->SetTexture(0, g_tex);
  dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
  dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
  dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
  dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
  dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(FmvVtx));
  dev->SetTexture(0, nullptr);
}

void upload_and_draw() {
  // Exclusive HWND path: stock GfxEngine_AltPresentGate @ 0x4F9760 — video
  // window owns pixels; host only pumps EC_COMPLETE (no SampleGrabber blit).
  if (!g_non_exclusive) {
    if (g_playing) handle_events();
    return;
  }
  if (!g_grabber || !g_playing) return;
  handle_events();

  auto* dev = reinterpret_cast<IDirect3DDevice9*>(render_d3d9_device());
  if (!dev || g_w <= 0 || g_h <= 0) return;
  if (!ensure_video_texture(dev)) return;

  // Soft PE FMV_DirectShow_RunUpdateQuad @ 0x4F94E0 / Open @ 0x4F92FA:
  // rebuild letterbox+UV after tex create (pow2) or backbuffer resize.
  if (!g_quad.valid || g_quad.screen_w != render_d3d9_width() ||
      g_quad.screen_h != render_d3d9_height()) {
    texture_renderer_update_quad();
  } else {
    // Refresh UV if CreateTextures fell back to pow2 after first open call.
    const float uw = static_cast<float>(g_w) / static_cast<float>(g_tex_w);
    const float vh = static_cast<float>(g_h) / static_cast<float>(g_tex_h);
    if (g_quad.u_max != uw || g_quad.v_max != vh) texture_renderer_update_quad();
  }

  copy_sample_to_texture();
  if (g_quad.valid) {
    draw_update_quad(dev);
  } else {
    render_d3d9_draw_video_texture(g_tex, g_w, g_h);
  }
}
#endif

}  // namespace

int32_t video_fmv_open(const char* path, int32_t non_exclusive, int32_t loop) {
  // Soft PE of GfxEngine.openVideo @ 0x47C330 → FMV_DirectShow_Open @ 0x4F8DD0.
  // PATH-TO-WORLD: Engine_boot exclusive intros → close → MainMenu
  // openVideo("data\\fmv\\prime.avi", 1, 1) (Java MainMenu.show).
#ifdef _WIN32
  // Soft PE openVideo @ 0x47C357: path null → -1 (empty string enters Open).
  if (!path) return -1;
  // Soft PE Open @ 0x4F8DE0: FMV_dsReady==0 → E_FAIL.
  if (!ensure_com() || !g_ds_ready) return kFmvEFail;
  // Soft PE Open @ 0x4F8DF5: close only when already playing (intro→menu
  // re-open after boot Close; put_Visible(0) on exclusive tear above).
  if (g_playing) video_fmv_close();

  // Soft host resolve (stock strcpy of Unbox path into FMV_pathBuf @ 0x64A160).
  std::string resolved = rpak_resolve_path(path);
  if (resolved.empty()) resolved = path;
  const DWORD attr = GetFileAttributesA(resolved.c_str());
  if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) {
    std::printf("[fmv] missing %s\n", resolved.c_str());
    return kFmvEFail;
  }
  const std::wstring wpath = to_wide(resolved.c_str());
  if (wpath.empty()) return kFmvEFail;

  // Soft PE @ 0x4F8E19 / 0x4F8E3C: strcpy path → FMV_nonExclusive=a2.
  std::strncpy(g_path, resolved.c_str(), sizeof(g_path) - 1);
  g_path[sizeof(g_path) - 1] = '\0';
  g_non_exclusive = non_exclusive != 0;

  // Soft PE @ 0x4F8E42: CoCreateInstance(CLSID_FilterGraph, CLSCTX_INPROC=1).
  HRESULT hr = CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IGraphBuilder, reinterpret_cast<void**>(&g_graph));
  if (FAILED(hr) || !g_graph) {
    g_graph = nullptr;
    return static_cast<int32_t>(FAILED(hr) ? hr : kFmvEFail);
  }

  hr = g_non_exclusive ? build_graph_texture(wpath.c_str())
                       : build_graph_exclusive(wpath.c_str());
  if (FAILED(hr)) {
    std::printf("[fmv] DirectShow graph failed: %s (non_excl=%d hr=0x%08lX)\n",
                g_path, g_non_exclusive ? 1 : 0, static_cast<unsigned long>(hr));
    release_graph();
    return static_cast<int32_t>(hr);
  }

  // Soft PE @ 0x4F9180: QI×5 — exclusive requires VideoWindow+BasicVideo;
  // Soft TextureRenderer stand-in: Control/Seeking/Event hard, VW/BV soft.
  if (!qi_open_interfaces(/*require_video_ifaces=*/!g_non_exclusive)) {
    release_graph();
    return kFmvEFail;
  }

  // Soft PE @ 0x4F9209: SetNotifyWindow(hWnd, 0x40D, 0) both branches.
  hr = set_notify_window_hr(static_cast<HWND>(render_d3d9_hwnd()), kFmvNotifyMsg);
  if (FAILED(hr)) {
    // Stock returns SetNotifyWindow HRESULT without Close on this edge.
    return static_cast<int32_t>(hr);
  }

  // Soft PE @ 0x4F9220+: exclusive HWND Owner/Style/Position.
  // Fail → return HRESULT with graph still live (stock does not Close here).
  if (!g_non_exclusive) {
    hr = setup_exclusive_hwnd();
    if (FAILED(hr)) return static_cast<int32_t>(hr);
  }

  // Soft PE @ 0x4F92AB..0x4F930F (dsReady gate): FMV_loopFlag=loop;
  // if GfxDevice_presentCount%2!=0 → g_GfxDevice vt+0x18 BeginScene
  // (signed-mod and 80000001h @ 0x4F92B2) — host presentCount OOS, skip;
  // Run → playing=1; Run fail → playing=0 then still return 0 (not E_FAIL).
  g_loop = loop != 0;
  if (g_control && SUCCEEDED(g_control->Run())) {
    g_playing = true;
    // Soft PE @ 0x4F92FA: non_exclusive → TextureRenderer_UpdateQuad after Run
    // (menu prime.avi path; exclusive boot skips UpdateQuad).
    if (g_non_exclusive) texture_renderer_update_quad();
  } else {
    g_playing = false;
  }
  std::printf("[fmv] open ok %s non_excl=%d loop=%d playing=%d %dx%d tex=%dx%d\n",
              g_path, g_non_exclusive ? 1 : 0, g_loop ? 1 : 0, g_playing ? 1 : 0,
              g_w, g_h, g_tex_w, g_tex_h);
  return 0;
#else
  (void)path;
  (void)non_exclusive;
  (void)loop;
  return -1;
#endif
}

void video_fmv_close() {
#ifdef _WIN32
  // Soft PE of FMV_DirectShow_Close @ 0x4F9320: if FMV_dsReady==0 ret;
  // else clear FMV_playing then tear graph (see release_graph).
  if (!g_ds_ready) return;
  release_graph();
#endif
}

int32_t video_fmv_is_playing() {
#ifdef _WIN32
  // Soft PE of FMV_DirectShow_IsPlaying @ 0x4F9750: return FMV_playing.
  return g_playing ? 1 : 0;
#else
  return 0;
#endif
}

int32_t video_fmv_is_non_exclusive() {
#ifdef _WIN32
  // Soft PE FMV_nonExclusive @ 0x61852C (Open stores a2).
  return g_non_exclusive ? 1 : 0;
#else
  return 0;
#endif
}

int32_t video_fmv_alt_present_gate() {
  // Soft PE GfxEngine_AltPresentGate @ 0x4F9760 size 0x1b:
  // return FMV_playing != 0 && FMV_nonExclusive == 0.
  // Sole MainLoop xref @ 0x428CE8 → AltPresent else PresentFrame.
  return (video_fmv_is_playing() != 0 && video_fmv_is_non_exclusive() == 0) ? 1
                                                                            : 0;
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
  // Boot uses FMV_Boot_PlayPath_DirectShow @ 0x55C470 (exclusive RenderFile),
  // not GfxEngine.openVideo — host reuses open(..., non_exclusive=0, loop=0).
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
