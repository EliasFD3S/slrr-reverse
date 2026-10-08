// Fragment 3/3 of render_d3d9.cpp — included, not compiled on its own.
// Split only to stay under the 128 KB editor buffer cap; the
// translation unit is unchanged. See native/tools/split_source.py.


bool render_d3d9_texture_apply_luma_alpha(void* key) {
#ifdef _WIN32
  if (!g_dev || !key) return false;
  auto it = g_textures.find(key);
  if (it == g_textures.end() || !it->second.tex) return false;
  TextureState& st = it->second;
  if (st.luma_alpha) return true;

  D3DSURFACE_DESC desc{};
  if (FAILED(st.tex->GetLevelDesc(0, &desc)) || desc.Width == 0 ||
      desc.Height == 0)
    return false;

  IDirect3DTexture9* out = nullptr;
  if (FAILED(g_dev->CreateTexture(desc.Width, desc.Height, 1, 0,
                                  D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &out,
                                  nullptr)) ||
      !out)
    return false;

  D3DLOCKED_RECT src_lr{};
  D3DLOCKED_RECT dst_lr{};
  if (FAILED(st.tex->LockRect(0, &src_lr, nullptr, D3DLOCK_READONLY)) ||
      FAILED(out->LockRect(0, &dst_lr, nullptr, 0))) {
    if (src_lr.pBits) st.tex->UnlockRect(0);
    out->Release();
    return false;
  }

  auto* dst_base = static_cast<uint8_t*>(dst_lr.pBits);
  const int w = static_cast<int>(desc.Width);
  const int h = static_cast<int>(desc.Height);

  if (desc.Format == D3DFMT_DXT1) {
    const auto* src = static_cast<const uint8_t*>(src_lr.pBits);
    const int bw = (w + 3) / 4;
    const int bh = (h + 3) / 4;
    for (int by = 0; by < bh; ++by) {
      for (int bx = 0; bx < bw; ++bx) {
        uint8_t px[16][4];
        decode_dxt1_block(src + (by * bw + bx) * 8, px);
        for (int k = 0; k < 16; ++k) {
          const int x = bx * 4 + (k % 4);
          const int y = by * 4 + (k / 4);
          if (x >= w || y >= h) continue;
          const uint8_t bb = px[k][0], gg = px[k][1], rr = px[k][2];
          uint8_t* d = dst_base + y * dst_lr.Pitch + x * 4;
          d[0] = bb;
          d[1] = gg;
          d[2] = rr;
          d[3] = luma_to_osd_bg_alpha(rr, gg, bb);
        }
      }
    }
  } else {
    // Already uncompressed — rewrite alpha from luma, keep RGB.
    for (int y = 0; y < h; ++y) {
      const auto* s =
          static_cast<const uint8_t*>(src_lr.pBits) + y * src_lr.Pitch;
      auto* d = dst_base + y * dst_lr.Pitch;
      for (int x = 0; x < w; ++x) {
        uint8_t bb = 0, gg = 0, rr = 0;
        if (desc.Format == D3DFMT_A8R8G8B8 || desc.Format == D3DFMT_X8R8G8B8) {
          bb = s[x * 4 + 0];
          gg = s[x * 4 + 1];
          rr = s[x * 4 + 2];
        } else if (desc.Format == D3DFMT_R5G6B5) {
          const uint16_t c = static_cast<uint16_t>(s[x * 2] | (s[x * 2 + 1] << 8));
          rgb565_to_rgb(c, &rr, &gg, &bb);
        } else {
          out->UnlockRect(0);
          st.tex->UnlockRect(0);
          out->Release();
          return false;
        }
        d[x * 4 + 0] = bb;
        d[x * 4 + 1] = gg;
        d[x * 4 + 2] = rr;
        d[x * 4 + 3] = luma_to_osd_bg_alpha(rr, gg, bb);
      }
    }
  }

  out->UnlockRect(0);
  st.tex->UnlockRect(0);
  st.tex->Release();
  st.tex = out;
  st.mips = 1;
  st.luma_alpha = true;
  return true;
#else
  (void)key;
  return false;
#endif
}

int32_t render_d3d9_texture_width(void* key) {
  auto it = g_textures.find(key);
  return it == g_textures.end() ? 0 : it->second.w;
}

int32_t render_d3d9_texture_height(void* key) {
  auto it = g_textures.find(key);
  return it == g_textures.end() ? 0 : it->second.h;
}

int32_t render_d3d9_texture_mips(void* key) {
  auto it = g_textures.find(key);
  if (it == g_textures.end()) return 0;
  return it->second.mips > 0 ? it->second.mips : 1;
}

const char* render_d3d9_texture_label(void* key) {
  auto it = g_textures.find(key);
  return it == g_textures.end() ? "" : it->second.label.c_str();
}

void render_d3d9_set_global_envmap(void* key) {
  // PE @ 0x0047C220: cmp current(+0x64) vs handle @ 0x47C24A → jz no-op.
  // handle 0 @ loc_47C2B8 zeros node (+0x58..+0x64). Else unlink/relink
  // list; host has no resource+0x48 list — assign g_envmap only.
  // Present binds via apply_envmap_stage1 (ADD + TCI_CAMERASPACEREFLECTION).
  if (g_envmap == key) return;
  g_envmap = key;
}

void* render_d3d9_global_envmap() { return g_envmap; }

void render_d3d9_osd_clear() {
  g_osd.clear();
  g_osd_text.clear();
}

void render_d3d9_osd_add_rect(float x, float y, float w, float h, void* texture,
                              int32_t pri) {
  OsdRect r;
  r.x = x;
  r.y = y;
  r.w = w;
  r.h = h;
  r.texture = texture;
  r.pri = pri;
  g_osd.push_back(r);
}

void render_d3d9_osd_set_rect(void* key, float x, float y, float w, float h,
                              void* texture, int32_t pri) {
  render_d3d9_osd_set_rect_color(key, x, y, w, h, texture, 0xFFFFFFFFu, pri);
}

void render_d3d9_osd_set_rect_color(void* key, float x, float y, float w,
                                    float h, void* texture, uint32_t argb,
                                    int32_t pri) {
  if (key) {
    for (auto& r : g_osd) {
      if (r.key == key) {
        r.x = x;
        r.y = y;
        r.w = w;
        r.h = h;
        r.texture = texture;
        r.pri = pri;
        r.color = argb ? argb : 0xFFFFFFFFu;
        return;
      }
    }
  }
  OsdRect r;
  r.key = key;
  r.x = x;
  r.y = y;
  r.w = w;
  r.h = h;
  r.texture = texture;
  r.pri = pri;
  r.color = argb ? argb : 0xFFFFFFFFu;
  g_osd.push_back(r);
}

void render_d3d9_osd_set_rect_visible(void* key, int32_t visible) {
  if (!key) return;
  for (auto& r : g_osd) {
    if (r.key == key) {
      r.visible = visible != 0;
      return;
    }
  }
}

void render_d3d9_osd_remove_rect(void* key) {
  if (!key) return;
  for (size_t i = 0; i < g_osd.size();) {
    if (g_osd[i].key == key)
      g_osd.erase(g_osd.begin() + static_cast<std::ptrdiff_t>(i));
    else
      ++i;
  }
}

int32_t render_d3d9_osd_count() {
  return static_cast<int32_t>(g_osd.size());
}

int32_t render_d3d9_osd_text_count() {
  return static_cast<int32_t>(g_osd_text.size());
}

namespace {

int32_t font_px_height_from_name(const std::string& name) {
  std::string n = name;
  for (char& c : n) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  // Mirror Text.getFontSize RID table.
  if (n.find("simple40") != std::string::npos) return 40;
  if (n.find("slii24") != std::string::npos) return 24;
  if (n.find("simple20") != std::string::npos) return 20;
  if (n.find("slii17") != std::string::npos) return 15;
  if (n.find("slii11") != std::string::npos) return 10;
  if (n.find("console10") != std::string::npos) return 10;
  if (n.find("console5") != std::string::npos) return 7;
  if (n.find("sl28") != std::string::npos) return 28;
  if (n.find("sl14") != std::string::npos) return 14;
  return 20;
}

bool load_file_bytes(const std::string& path, std::vector<uint8_t>* out) {
  if (!out) return false;
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) {
    std::string resolved = rpak_resolve_path(path.c_str());
    if (!resolved.empty()) f = std::fopen(resolved.c_str(), "rb");
  }
  if (!f) return false;
  std::fseek(f, 0, SEEK_END);
  const long sz = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (sz <= 0 || sz > 32 * 1024 * 1024) {
    std::fclose(f);
    return false;
  }
  out->resize(static_cast<size_t>(sz));
  const bool ok = std::fread(out->data(), 1, out->size(), f) == out->size();
  std::fclose(f);
  return ok;
}

// Decode PNG/JPEG/etc via WIC into a font atlas.
// SLII*.png are LA: white fill + black outline with coverage alpha — preserve
// BGRA. Greyscale coverage atlases (no dark-with-alpha outline) still use
// white RGB + luma→alpha so Text.changeColor can tint.
bool upload_wic_font_atlas(void* key, const uint8_t* data, size_t size,
                           const char* label) {
#ifdef _WIN32
  if (!g_dev || !key || !data || size < 8) return false;

  static bool com_inited = false;
  if (!com_inited) {
    const HRESULT chr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(chr) || chr == RPC_E_CHANGED_MODE) com_inited = true;
  }

  IWICImagingFactory* factory = nullptr;
  HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
  if (FAILED(hr) || !factory) return false;

  IWICStream* stream = nullptr;
  hr = factory->CreateStream(&stream);
  if (FAILED(hr) || !stream) {
    factory->Release();
    return false;
  }
  hr = stream->InitializeFromMemory(const_cast<BYTE*>(data),
                                    static_cast<DWORD>(size));
  if (FAILED(hr)) {
    stream->Release();
    factory->Release();
    return false;
  }

  IWICBitmapDecoder* decoder = nullptr;
  hr = factory->CreateDecoderFromStream(stream, nullptr,
                                        WICDecodeMetadataCacheOnLoad, &decoder);
  stream->Release();
  if (FAILED(hr) || !decoder) {
    factory->Release();
    return false;
  }

  IWICBitmapFrameDecode* frame = nullptr;
  hr = decoder->GetFrame(0, &frame);
  decoder->Release();
  if (FAILED(hr) || !frame) {
    factory->Release();
    return false;
  }

  IWICFormatConverter* conv = nullptr;
  hr = factory->CreateFormatConverter(&conv);
  if (FAILED(hr) || !conv) {
    frame->Release();
    factory->Release();
    return false;
  }
  hr = conv->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
                        nullptr, 0.0, WICBitmapPaletteTypeCustom);
  frame->Release();
  if (FAILED(hr)) {
    conv->Release();
    factory->Release();
    return false;
  }

  UINT w = 0, h = 0;
  conv->GetSize(&w, &h);
  if (w == 0 || h == 0 || w > 8192 || h > 8192) {
    conv->Release();
    factory->Release();
    return false;
  }
  const UINT stride = w * 4;
  const UINT bytes = stride * h;
  std::vector<uint8_t> pixels(bytes);
  hr = conv->CopyPixels(nullptr, stride, bytes, pixels.data());
  conv->Release();
  factory->Release();
  if (FAILED(hr)) return false;

  // Detect baked outline (black RGB + non-zero alpha), e.g. SLII LA PNGs.
  bool outline_atlas = false;
  for (UINT i = 0; i < w * h; ++i) {
    const uint8_t* p = pixels.data() + i * 4;
    const int luma =
        (static_cast<int>(p[0]) + static_cast<int>(p[1]) + static_cast<int>(p[2])) /
        3;
    if (luma < 40 && p[3] > 16) {
      outline_atlas = true;
      break;
    }
  }
  if (!outline_atlas) {
    // Coverage mask: opaque RGB → alpha; RGB white for diffuse tint.
    for (UINT i = 0; i < w * h; ++i) {
      uint8_t* p = pixels.data() + i * 4;
      const int a = (static_cast<int>(p[0]) + p[1] + p[2]) / 3;
      p[0] = 0xff;
      p[1] = 0xff;
      p[2] = 0xff;
      p[3] = static_cast<uint8_t>(a);
    }
  }

  TextureState& st = g_textures[key];
  st.label = label ? label : "";
  if (st.tex) {
    st.tex->Release();
    st.tex = nullptr;
  }
  if (FAILED(g_dev->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
                                  &st.tex, nullptr)) ||
      !st.tex)
    return false;
  D3DLOCKED_RECT lr{};
  if (FAILED(st.tex->LockRect(0, &lr, nullptr, 0))) {
    st.tex->Release();
    st.tex = nullptr;
    return false;
  }
  if (lr.Pitch == static_cast<INT>(stride)) {
    std::memcpy(lr.pBits, pixels.data(), bytes);
  } else {
    const uint8_t* src = pixels.data();
    uint8_t* dst = static_cast<uint8_t*>(lr.pBits);
    for (UINT y = 0; y < h; ++y) {
      std::memcpy(dst, src, stride);
      src += stride;
      dst += lr.Pitch;
    }
  }
  st.tex->UnlockRect(0);
  st.w = static_cast<int32_t>(w);
  st.h = static_cast<int32_t>(h);
  st.mips = 1;
  return true;
#else
  (void)key;
  (void)data;
  (void)size;
  (void)label;
  return false;
#endif
}

bool upload_tga_font_atlas(void* key, const uint8_t* data, size_t size,
                           const char* label) {
#ifdef _WIN32
  if (!g_dev || !key || !data || size < 18) return false;
  const uint8_t id_len = data[0];
  const uint8_t cmap_type = data[1];
  const uint8_t img_type = data[2];
  const uint16_t cmap_first =
      static_cast<uint16_t>(data[3] | (data[4] << 8));
  const uint16_t cmap_len =
      static_cast<uint16_t>(data[5] | (data[6] << 8));
  const uint8_t cmap_entry_bits = data[7];
  const uint16_t w = static_cast<uint16_t>(data[12] | (data[13] << 8));
  const uint16_t h = static_cast<uint16_t>(data[14] | (data[15] << 8));
  const uint8_t bpp = data[16];
  const uint8_t desc = data[17];
  if (w == 0 || h == 0 || bpp != 8) return false;
  // Type 3 = greyscale; type 1 = indexed (SLII*_detect.tga).
  if (img_type != 3 && img_type != 1) return false;
  if (img_type == 1 && (cmap_type != 1 || cmap_len == 0 || cmap_entry_bits < 24))
    return false;

  const size_t cmap_bytes =
      img_type == 1
          ? static_cast<size_t>(cmap_len) * (cmap_entry_bits / 8)
          : 0;
  const size_t header = 18u + id_len + cmap_bytes;
  const size_t need = header + static_cast<size_t>(w) * static_cast<size_t>(h);
  if (size < need) return false;

  // Build 256 greyscale alpha samples from palette (or identity for type 3).
  uint8_t alpha_lut[256];
  for (int i = 0; i < 256; ++i) alpha_lut[i] = static_cast<uint8_t>(i);
  if (img_type == 1) {
    std::memset(alpha_lut, 0, sizeof(alpha_lut));
    const uint8_t* pal = data + 18u + id_len;
    const int epb = cmap_entry_bits / 8;
    for (uint16_t i = 0; i < cmap_len && i < 256; ++i) {
      const uint8_t* e = pal + static_cast<size_t>(i) * epb;
      // TGA palette is BGR(A).
      const uint8_t b = e[0], g = e[1], r = e[2];
      const int a = (static_cast<int>(r) + g + b) / 3;
      const int idx = static_cast<int>(cmap_first) + i;
      if (idx >= 0 && idx < 256) alpha_lut[idx] = static_cast<uint8_t>(a);
    }
  }

  TextureState& st = g_textures[key];
  st.label = label ? label : "";
  if (st.tex) {
    st.tex->Release();
    st.tex = nullptr;
  }
  if (FAILED(g_dev->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
                                  &st.tex, nullptr)) ||
      !st.tex)
    return false;

  D3DLOCKED_RECT lr{};
  if (FAILED(st.tex->LockRect(0, &lr, nullptr, 0))) {
    st.tex->Release();
    st.tex = nullptr;
    return false;
  }
  const bool top_left = (desc & 0x20) != 0;
  for (uint16_t y = 0; y < h; ++y) {
    const uint16_t src_y = top_left ? y : static_cast<uint16_t>(h - 1 - y);
    const uint8_t* src = data + header + static_cast<size_t>(src_y) * w;
    uint8_t* dst = static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch;
    for (uint16_t x = 0; x < w; ++x) {
      const uint8_t a = alpha_lut[src[x]];
      dst[x * 4 + 0] = 0xff;
      dst[x * 4 + 1] = 0xff;
      dst[x * 4 + 2] = 0xff;
      dst[x * 4 + 3] = a;
    }
  }
  st.tex->UnlockRect(0);
  st.w = w;
  st.h = h;
  st.mips = 1;
  return true;
#else
  (void)key;
  (void)data;
  (void)size;
  (void)label;
  return false;
#endif
}

// Invictus font SCX (INVO v3, hdr=0x38): vcount@0x40, verts@0x44, stride 44,
// uv at +24. Glyphs are indexed by ASCII code (z ≈ -100*ascii), 4 verts each.
bool parse_font_glyphs(const uint8_t* scx, size_t scx_size,
                       std::vector<FontGlyph>* out) {
  if (!scx || !out || scx_size < 0x80) return false;
  out->clear();
  if (std::memcmp(scx, "INVO", 4) != 0) return false;
  uint32_t ver = 0, vcount = 0;
  std::memcpy(&ver, scx + 4, 4);
  std::memcpy(&vcount, scx + 0x40, 4);
  if (ver != 3 || vcount < 4 || (vcount % 4) != 0 || vcount > 4096) return false;
  constexpr size_t kStride = 44;
  constexpr size_t kVStart = 0x44;
  const uint64_t need =
      static_cast<uint64_t>(kVStart) +
      static_cast<uint64_t>(vcount) * static_cast<uint64_t>(kStride);
  if (need > scx_size) return false;

  const size_t nglyphs = static_cast<size_t>(vcount / 4);
  out->reserve(nglyphs);
  int ok = 0;
  for (size_t g = 0; g < nglyphs; ++g) {
    float umin = 2.f, umax = -1.f, vmin = 2.f, vmax = -1.f;
    for (int i = 0; i < 4; ++i) {
      const size_t off =
          kVStart + (g * 4 + static_cast<size_t>(i)) * kStride;
      float u = 0, v = 0;
      std::memcpy(&u, scx + off + 24, 4);
      std::memcpy(&v, scx + off + 28, 4);
      if (!(u >= 0.f && u <= 1.01f && v >= 0.f && v <= 1.01f)) {
        umin = umax = vmin = vmax = 0.f;
        break;
      }
      if (u < umin) umin = u;
      if (u > umax) umax = u;
      if (v < vmin) vmin = v;
      if (v > vmax) vmax = v;
    }
    FontGlyph glyph;
    glyph.u0 = umin;
    glyph.v0 = vmin;
    glyph.u1 = umax;
    glyph.v1 = vmax;
    out->push_back(glyph);
    if ((umax - umin) > 0.002f && (vmax - vmin) > 0.015f) ++ok;
  }
  return ok >= 32;
}

}  // namespace

bool render_d3d9_font_load(void* key, const char* name) {
  if (!key || !name || !name[0]) return false;
  std::string n = name;
  // Strip path / extension.
  {
    const size_t slash = n.find_last_of("/\\");
    if (slash != std::string::npos) n = n.substr(slash + 1);
    const size_t dot = n.find_last_of('.');
    if (dot != std::string::npos) n = n.substr(0, dot);
  }

  std::string up = n;
  for (char& c : up) {
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
  }

  std::string scx = "frontend/meshes/Fonts/" + n + ".scx";
  std::string charset_path = "frontend/font.dat";

  std::vector<uint8_t> scx_bytes, atlas_bytes, cs_bytes;
  if (!load_file_bytes(scx, &scx_bytes)) {
    scx = "frontend/meshes/Fonts/" + n + ".scx";
    if (!load_file_bytes(scx, &scx_bytes)) return false;
  }
  if (!load_file_bytes(charset_path, &cs_bytes) || cs_bytes.empty()) return false;

  // Prefer real atlases (TGA then PNG). Never prefer *_detect.tga — those are
  // debug maps and produce garbage glyphs (SLII* ships as SLII24.png).
  enum class AtlasKind { Tga, Wic };
  AtlasKind kind = AtlasKind::Tga;
  std::string atlas_path;
  auto try_atlas = [&](const std::string& path, AtlasKind k) -> bool {
    if (!load_file_bytes(path, &atlas_bytes) || atlas_bytes.empty()) return false;
    atlas_path = path;
    kind = k;
    return true;
  };
  if (!try_atlas("frontend/textures/Fonts/" + n + ".tga", AtlasKind::Tga) &&
      !try_atlas("frontend/textures/Fonts/" + up + ".tga", AtlasKind::Tga) &&
      !try_atlas("frontend/textures/Fonts/" + n + ".png", AtlasKind::Wic) &&
      !try_atlas("frontend/textures/Fonts/" + up + ".png", AtlasKind::Wic) &&
      !try_atlas("frontend/textures/Fonts/" + up + "_detect.tga", AtlasKind::Tga) &&
      !try_atlas("frontend/textures/Fonts/" + n + "_detect.tga", AtlasKind::Tga)) {
    return false;
  }

  void* atlas_key =
      reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(key) + 0x20000u);
  const bool uploaded =
      (kind == AtlasKind::Tga)
          ? upload_tga_font_atlas(atlas_key, atlas_bytes.data(),
                                  atlas_bytes.size(), atlas_path.c_str())
          : upload_wic_font_atlas(atlas_key, atlas_bytes.data(),
                                  atlas_bytes.size(), atlas_path.c_str());
  if (!uploaded) {
    // No device / WIC fail: keep dims so font_ready + Text metrics still work
    // (Valocity HUD smoke without --window; mirrors PTX skydome path).
    int32_t pw = 0, ph = 0;
    bool dims_ok = false;
    if (kind == AtlasKind::Tga && atlas_bytes.size() >= 18) {
      pw = static_cast<int32_t>(
          atlas_bytes[12] | (static_cast<uint16_t>(atlas_bytes[13]) << 8));
      ph = static_cast<int32_t>(
          atlas_bytes[14] | (static_cast<uint16_t>(atlas_bytes[15]) << 8));
      dims_ok = pw > 0 && ph > 0;
    } else if (atlas_bytes.size() >= 24 && atlas_bytes[0] == 0x89 &&
               atlas_bytes[1] == 'P' && atlas_bytes[2] == 'N' &&
               atlas_bytes[3] == 'G') {
      const uint32_t w = (static_cast<uint32_t>(atlas_bytes[16]) << 24) |
                         (static_cast<uint32_t>(atlas_bytes[17]) << 16) |
                         (static_cast<uint32_t>(atlas_bytes[18]) << 8) |
                         static_cast<uint32_t>(atlas_bytes[19]);
      const uint32_t h = (static_cast<uint32_t>(atlas_bytes[20]) << 24) |
                         (static_cast<uint32_t>(atlas_bytes[21]) << 16) |
                         (static_cast<uint32_t>(atlas_bytes[22]) << 8) |
                         static_cast<uint32_t>(atlas_bytes[23]);
      if (w > 0 && h > 0 && w <= 8192 && h <= 8192) {
        pw = static_cast<int32_t>(w);
        ph = static_cast<int32_t>(h);
        dims_ok = true;
      }
    }
    if (!dims_ok) {
      render_d3d9_texture_destroy(atlas_key);
      return false;
    }
    TextureState& tst = g_textures[atlas_key];
    tst.label = atlas_path;
    tst.w = pw;
    tst.h = ph;
    tst.mips = 1;
  }
  std::printf("[font] %s atlas=%s\n", n.c_str(), atlas_path.c_str());

  FontState st;
  st.name = n;
  st.atlas_key = atlas_key;
  st.atlas_w = render_d3d9_texture_width(atlas_key);
  st.atlas_h = render_d3d9_texture_height(atlas_key);
  st.px_height = font_px_height_from_name(n);
  st.charset.assign(reinterpret_cast<const char*>(cs_bytes.data()),
                    cs_bytes.size());
  if (!parse_font_glyphs(scx_bytes.data(), scx_bytes.size(), &st.glyphs)) {
    render_d3d9_texture_destroy(atlas_key);
    return false;
  }
  for (FontGlyph& g : st.glyphs) {
    g.adv_px = (g.u1 - g.u0) * static_cast<float>(st.atlas_w);
    if (g.adv_px < 1.f) g.adv_px = static_cast<float>(st.px_height) * 0.35f;
  }
  st.ready = !st.glyphs.empty();
  {
    auto it = g_fonts.find(key);
    if (it != g_fonts.end() && it->second.atlas_key &&
        it->second.atlas_key != atlas_key)
      render_d3d9_texture_destroy(it->second.atlas_key);
  }
  g_fonts[key] = std::move(st);
  return g_fonts[key].ready;
}

bool render_d3d9_font_load_from_rid(void* key, int32_t res_id) {
  if (!key || res_id == 0) return false;
  const RpakEntry* ent = rpak_find_entry(res_id);
  if (!ent || ent->name.empty()) return false;
  return render_d3d9_font_load(key, ent->name.c_str());
}

bool render_d3d9_font_ready(void* key) {
  auto it = g_fonts.find(key);
  return it != g_fonts.end() && it->second.ready;
}

const char* render_d3d9_font_name(void* key) {
  auto it = g_fonts.find(key);
  if (it == g_fonts.end() || !it->second.ready) return "";
  return it->second.name.c_str();
}

int32_t render_d3d9_font_glyph_count(void* key) {
  auto it = g_fonts.find(key);
  if (it == g_fonts.end()) return 0;
  return static_cast<int32_t>(it->second.glyphs.size());
}

int32_t render_d3d9_font_px_height(void* key) {
  auto it = g_fonts.find(key);
  if (it == g_fonts.end() || !it->second.ready) return 0;
  return it->second.px_height;
}

float render_d3d9_font_measure_px(void* key, const char* text) {
  auto it = g_fonts.find(key);
  if (it == g_fonts.end() || !it->second.ready || !text) return 0.f;
  const FontState& font = it->second;
  float w = 0.f;
  for (const char* p = text; *p; ++p) {
    const unsigned char ch = static_cast<unsigned char>(*p);
    if (ch == ' ') {
      w += static_cast<float>(font.px_height) * 0.35f;
      continue;
    }
    const size_t gi = static_cast<size_t>(ch);
    if (gi >= font.glyphs.size()) continue;
    w += font.glyphs[gi].adv_px;
  }
  return w;
}

void render_d3d9_font_destroy(void* key) {
  auto it = g_fonts.find(key);
  if (it == g_fonts.end()) return;
  if (it->second.atlas_key) render_d3d9_texture_destroy(it->second.atlas_key);
  g_fonts.erase(it);
}

void render_d3d9_text_create(void* key, void* font, float x, float y) {
  if (!key) return;
  TextState& t = g_texts[key];
  t.font = font;
  t.x = x;
  t.y = y;
  t.color = 0xFFFFFFFFu;
  t.align = 2;
  t.text.clear();
  t.alive = true;
}

void render_d3d9_text_destroy(void* key) {
  if (!key) return;
  for (size_t i = 0; i < g_osd_text.size();) {
    if (g_osd_text[i].text_key == key)
      g_osd_text.erase(g_osd_text.begin() + static_cast<std::ptrdiff_t>(i));
    else
      ++i;
  }
  g_texts.erase(key);
}

void render_d3d9_text_set_color(void* key, uint32_t argb) {
  auto it = g_texts.find(key);
  if (it == g_texts.end()) return;
  it->second.color = argb;
  for (auto& ot : g_osd_text) {
    if (ot.text_key == key) ot.color = argb;
  }
}

void render_d3d9_text_set_align(void* key, int32_t align) {
  auto it = g_texts.find(key);
  if (it == g_texts.end()) return;
  it->second.align = align;
}

void render_d3d9_text_set_pos(void* key, float x, float y) {
  auto it = g_texts.find(key);
  if (it == g_texts.end()) return;
  it->second.x = x;
  it->second.y = y;
}

void render_d3d9_text_set_string(void* key, const char* utf8) {
  auto it = g_texts.find(key);
  if (it == g_texts.end()) return;
  it->second.text = utf8 ? utf8 : "";
}

void render_d3d9_text_set_visible(void* key, int32_t visible) {
  auto it = g_texts.find(key);
  if (it == g_texts.end()) return;
  it->second.visible = visible != 0;
  for (auto& ot : g_osd_text) {
    if (ot.text_key == key) ot.visible = it->second.visible;
  }
}

const char* render_d3d9_text_get_string(void* key) {
  auto it = g_texts.find(key);
  if (it == g_texts.end() || !it->second.alive) return "";
  return it->second.text.c_str();
}

void render_d3d9_text_update(void* key) {
  auto it = g_texts.find(key);
  if (it == g_texts.end() || !it->second.alive) return;
  const TextState& t = it->second;
  // Replace existing OSD entry for this text instance.
  for (size_t i = 0; i < g_osd_text.size();) {
    if (g_osd_text[i].text_key == key)
      g_osd_text.erase(g_osd_text.begin() + static_cast<std::ptrdiff_t>(i));
    else
      ++i;
  }
  if (t.text.empty() || !t.font) return;
  OsdText ot;
  ot.text_key = key;
  ot.font = t.font;
  ot.x = t.x;
  ot.y = t.y;
  ot.color = t.color;
  ot.align = t.align;
  ot.text = t.text;
  ot.pri = 0;
  ot.visible = t.visible;
  g_osd_text.push_back(std::move(ot));
}

namespace {

bool read_u32(const uint8_t* data, size_t size, size_t off, uint32_t* out) {
  if (off + 4 > size || !out) return false;
  std::memcpy(out, data + off, 4);
  return true;
}

std::string scx_mat_name(const uint8_t* data, size_t size, uint32_t off,
                         uint32_t end) {
  if (off + 24 > end || end > size) return {};
  // Observed at +0x14: big-endian length (or 3 zero pad + len byte), then ASCII.
  // e.g. 00 00 00 08 "F grill\0" / 00 00 00 08 "hood\0..."
  const uint32_t p = off + 0x14;
  if (p + 4 >= end) return {};
  uint32_t n = (static_cast<uint32_t>(data[p]) << 24) |
               (static_cast<uint32_t>(data[p + 1]) << 16) |
               (static_cast<uint32_t>(data[p + 2]) << 8) |
               static_cast<uint32_t>(data[p + 3]);
  if (n == 0 || n > 64 || p + 4 + n > end) return {};
  uint32_t len = n;
  while (len > 0 && data[p + 4 + len - 1] == 0) --len;
  if (len == 0) return {};
  for (uint32_t i = 0; i < len; ++i) {
    const uint8_t c = data[p + 4 + i];
    if (c < 32 || c > 126) return {};
  }
  return std::string(reinterpret_cast<const char*>(data + p + 4), len);
}

uint32_t scx_mat_diffuse(const uint8_t* data, size_t size, uint32_t off,
                         uint32_t end) {
  if (off + 64 > end || end > size) return 0xFF808080u;
  // First 0xAARRGGBB-ish dword with alpha 0xFF after the name field.
  for (uint32_t p = off + 0x30; p + 4 <= end; p += 4) {
    uint32_t c = 0;
    if (!read_u32(data, size, p, &c)) break;
    if ((c >> 24) == 0xffu) return c;
  }
  return 0xFF808080u;
}

std::string ascii_lower(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return s;
}

std::string mat_name_to_token(const std::string& name) {
  std::string out;
  out.reserve(name.size());
  for (char c : name) {
    if (c == ' ' || c == '-' || c == '/')
      out.push_back('_');
    else
      out.push_back(c);
  }
  return out;
}

std::string guess_car_prefix(const std::filesystem::path& scx_path) {
  // .../Baiern_data/meshes/hood.scx → Baiern
  std::filesystem::path cur = scx_path;
  for (int i = 0; i < 4 && !cur.empty(); ++i) {
    std::string stem = cur.stem().string();
    if (stem.size() > 5) {
      const std::string low = ascii_lower(stem);
      if (low.size() >= 5 && low.compare(low.size() - 5, 5, "_data") == 0)
        return stem.substr(0, stem.size() - 5);
    }
    if (ascii_lower(cur.filename().string()) == "meshes") {
      std::string parent = cur.parent_path().stem().string();
      const std::string low = ascii_lower(parent);
      if (low.size() >= 5 && low.compare(low.size() - 5, 5, "_data") == 0)
        return parent.substr(0, parent.size() - 5);
      if (!parent.empty()) return parent;
    }
    cur = cur.parent_path();
  }
  return {};
}

std::filesystem::path guess_textures_dir(const std::filesystem::path& scx_path) {
  namespace fs = std::filesystem;
  const fs::path parent = scx_path.parent_path();
  fs::path cand = parent / "textures";
  if (fs::is_directory(cand)) return cand;
  cand = parent.parent_path() / "textures";
  if (fs::is_directory(cand)) return cand;
  return parent.parent_path() / "textures";
}

std::string resolve_mat_texture_path(const std::filesystem::path& scx_path,
                                     const std::string& mat_name) {
  namespace fs = std::filesystem;
  if (mat_name.empty()) return {};
  const fs::path texdir = guess_textures_dir(scx_path);
  if (!fs::is_directory(texdir)) return {};

  const std::string prefix = guess_car_prefix(scx_path);
  const std::string token = mat_name_to_token(mat_name);
  const std::string token_l = ascii_lower(token);
  const std::string prefix_l = ascii_lower(prefix);

  // Prefer exact / short matches: Prefix_token.dds then Prefix_token_*.dds
  std::string best;
  int best_score = -1;
  std::error_code ec;
  for (fs::directory_iterator it(texdir, ec); !ec && it != fs::directory_iterator();
       it.increment(ec)) {
    if (!it->is_regular_file(ec)) continue;
    const std::string fname = it->path().filename().string();
    const std::string fl = ascii_lower(fname);
    if (fl.size() < 5 || fl.compare(fl.size() - 4, 4, ".dds") != 0) continue;

    int score = -1;
    if (!prefix_l.empty()) {
      const std::string exact = prefix_l + "_" + token_l + ".dds";
      if (fl == exact) score = 1000;
      else {
        const std::string stem = prefix_l + "_" + token_l + "_";
        if (fl.compare(0, stem.size(), stem) == 0) {
          // Prefer shorter suffix (base variants over CITY etc. not in name).
          score = 800 - static_cast<int>(fl.size());
        }
      }
    }
    if (score < 0) {
      // Fallback: filename contains token (e.g. rearview_mirror).
      if (fl.find(token_l) != std::string::npos)
        score = 400 - static_cast<int>(fl.size());
    }
    if (score > best_score) {
      best_score = score;
      best = it->path().string();
    }
  }
  return best;
}

void* mesh_sub_tex_key(void* mesh_key, size_t index) {
  return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(mesh_key) +
                                 0x10000u + index + 1u);
}

void bind_mesh_textures(void* mesh_key, MeshState& st) {
  if (!mesh_key) return;
  std::filesystem::path scx;
  if (!st.label.empty()) scx = st.label;
  for (size_t i = 0; i < st.subs.size(); ++i) {
    MeshSubmesh& sm = st.subs[i];
    if (sm.name.empty()) continue;
    const std::string path = resolve_mat_texture_path(scx, sm.name);
    if (path.empty()) continue;
    void* tkey = mesh_sub_tex_key(mesh_key, i);
    if (render_d3d9_texture_create_from_file(tkey, path.c_str()) &&
        render_d3d9_texture_ready(tkey)) {
      sm.texture_key = tkey;
      sm.texture_path = path;
    } else {
      render_d3d9_texture_destroy(tkey);
      sm.texture_key = nullptr;
      sm.texture_path.clear();
    }
  }
}

void release_mesh_textures(void* mesh_key, MeshState& st) {
  for (size_t i = 0; i < st.subs.size(); ++i) {
    MeshSubmesh& sm = st.subs[i];
    if (sm.texture_key && mesh_key &&
        sm.texture_key == mesh_sub_tex_key(mesh_key, i))
      render_d3d9_texture_destroy(sm.texture_key);
    sm.texture_key = nullptr;
    sm.texture_path.clear();
  }
}

bool parse_scx_v4(const uint8_t* data, size_t size, MeshState& out) {
  if (!data || size < 24) return false;
  if (std::memcmp(data, "INVO", 4) != 0) return false;
  uint32_t ver = 0, nchunk = 0, rsv = 0;
  if (!read_u32(data, size, 4, &ver) || !read_u32(data, size, 8, &nchunk) ||
      !read_u32(data, size, 12, &rsv))
    return false;
  if (ver != 4 || nchunk == 0 || nchunk > 4096) return false;
  if (16ull + static_cast<uint64_t>(nchunk) * 8ull > size) return false;

  struct Chunk {
    uint32_t off;
    uint32_t typ;
  };
  std::vector<Chunk> chunks(nchunk);
  for (uint32_t i = 0; i < nchunk; ++i) {
    if (!read_u32(data, size, 16 + i * 8, &chunks[i].off) ||
        !read_u32(data, size, 16 + i * 8 + 4, &chunks[i].typ))
      return false;
    if (chunks[i].off >= size) return false;
  }

  auto chunk_end = [&](uint32_t i) -> uint32_t {
    if (i + 1 < nchunk) return chunks[i + 1].off;
    return static_cast<uint32_t>(size);
  };

  out.subs.clear();
  bool bounds_init = false;

  for (uint32_t i = 0; i < nchunk; ++i) {
    if (chunks[i].typ != 1) continue;
    // Find verts(5) and indices(0) after this material.
    int v_i = -1;
    int x_i = -1;
    for (uint32_t j = i + 1; j < nchunk; ++j) {
      if (chunks[j].typ == 1) break;
      if (chunks[j].typ == 5 && v_i < 0) v_i = static_cast<int>(j);
      if (chunks[j].typ == 0 && x_i < 0) x_i = static_cast<int>(j);
    }
    if (v_i < 0 || x_i < 0) continue;

    const uint32_t voff = chunks[v_i].off;
    const uint32_t vend = chunk_end(static_cast<uint32_t>(v_i));
    const uint32_t ioff = chunks[x_i].off;
    const uint32_t iend = chunk_end(static_cast<uint32_t>(x_i));
    if (voff + 16 > vend || ioff + 12 > iend) continue;

    uint32_t vtag = 0, vsize = 0, vcount = 0, vflags = 0;
    if (!read_u32(data, size, voff, &vtag) ||
        !read_u32(data, size, voff + 4, &vsize) ||
        !read_u32(data, size, voff + 8, &vcount) ||
        !read_u32(data, size, voff + 12, &vflags))
      continue;
    (void)vtag;
    (void)vflags;
    if (vcount == 0 || vcount > 500000) continue;
    // Phase 2.44: stride from chunk payload — 0x0241→32 (pos+n+uv),
    // 0x0041→24 (pos+n, glass). Fall back to 32.
    uint32_t stride = 32;
    if (vsize >= 16) {
      const uint32_t payload = vsize - 16;
      if (payload % vcount == 0) {
        const uint32_t s = payload / vcount;
        if (s == 24 || s == 32 || s == 36 || s == 40) stride = s;
      }
    }
    const uint64_t need =
        static_cast<uint64_t>(voff) + 16ull +
        static_cast<uint64_t>(vcount) * static_cast<uint64_t>(stride);
    if (need > vend || need > size) continue;

    uint32_t itag = 0, isize = 0, icount = 0;
    if (!read_u32(data, size, ioff, &itag) ||
        !read_u32(data, size, ioff + 4, &isize) ||
        !read_u32(data, size, ioff + 8, &icount))
      continue;
    (void)itag;
    (void)isize;
    if (icount < 3 || icount > 1500000 || (icount % 3) != 0) continue;
    if (static_cast<uint64_t>(ioff) + 12ull +
            static_cast<uint64_t>(icount) * 2ull >
        iend)
      continue;

    MeshSubmesh sm;
    sm.name = scx_mat_name(data, size, chunks[i].off, chunk_end(i));
    sm.diffuse = scx_mat_diffuse(data, size, chunks[i].off, chunk_end(i));
    sm.verts.resize(vcount);
    const uint8_t* vsrc = data + voff + 16;
    for (uint32_t vi = 0; vi < vcount; ++vi) {
      MeshVertex& v = sm.verts[vi];
      const uint8_t* p = vsrc + static_cast<size_t>(vi) * stride;
      std::memcpy(&v.x, p, 12);
      if (stride >= 24)
        std::memcpy(&v.nx, p + 12, 12);
      else
        v.nx = v.ny = v.nz = 0.f;
      if (stride >= 32)
        std::memcpy(&v.u, p + 24, 8);
      else
        v.u = v.v = 0.f;
      // Drop insane/NaN positions (rare bad verts in light meshes).
      if (!(v.x == v.x) || !(v.y == v.y) || !(v.z == v.z) ||
          std::fabs(v.x) > 1.0e6f || std::fabs(v.y) > 1.0e6f ||
          std::fabs(v.z) > 1.0e6f) {
        v.x = v.y = v.z = 0.f;
        v.nx = 0.f;
        v.ny = 1.f;
        v.nz = 0.f;
      }
    }
    sm.indices.resize(icount);
    std::memcpy(sm.indices.data(), data + ioff + 12, icount * 2);

    uint16_t max_i = 0;
    for (uint16_t idx : sm.indices) {
      if (idx > max_i) max_i = idx;
    }
    if (max_i >= vcount) continue;

    for (const MeshVertex& v : sm.verts) {
      if (!bounds_init) {
        out.bmin[0] = out.bmax[0] = v.x;
        out.bmin[1] = out.bmax[1] = v.y;
        out.bmin[2] = out.bmax[2] = v.z;
        bounds_init = true;
      } else {
        if (v.x < out.bmin[0]) out.bmin[0] = v.x;
        if (v.y < out.bmin[1]) out.bmin[1] = v.y;
        if (v.z < out.bmin[2]) out.bmin[2] = v.z;
        if (v.x > out.bmax[0]) out.bmax[0] = v.x;
        if (v.y > out.bmax[1]) out.bmax[1] = v.y;
        if (v.z > out.bmax[2]) out.bmax[2] = v.z;
      }
    }
    out.subs.push_back(std::move(sm));
  }

  out.ready = !out.subs.empty();
  return out.ready;
}

// Phase 2.51/2.54 — INVO v3 visual mesh (skydome / older parts).
// File: INVO + ver=3 + mat blocks. Each mat block (no magic after the first):
//   +0x00 hdr_size (≥0x80), +0x04 floats, +0x68 name, +0x88 vcount,
//   +0x8C verts stride 64 (pos+n+uv in first 32), then u32 tri_count +
//   tri_count×3 uint32 indices. Phase 2.54: loop subsequent mat blocks.
// Fonts use hdr=0x38 — skip.
bool parse_scx_v3(const uint8_t* data, size_t size, MeshState& out) {
  if (!data || size < 0xA0) return false;
  if (std::memcmp(data, "INVO", 4) != 0) return false;
  uint32_t ver = 0, hdr0 = 0;
  if (!read_u32(data, size, 4, &ver) || !read_u32(data, size, 8, &hdr0))
    return false;
  if (ver != 3 || hdr0 < 0x80u || hdr0 > 0x200u) return false;

  constexpr uint32_t kStride = 64;
  out.subs.clear();
  bool bounds_init = false;
  size_t mat_base = 0x08;  // first block shares file header after ver

  auto chan = [](float c) -> uint32_t {
    if (!(c == c)) c = 0.5f;
    if (c < 0.f) c = 0.f;
    if (c > 1.f) c = 1.f;
    return static_cast<uint32_t>(c * 255.f + 0.5f);
  };

  while (mat_base + 0x8C < size) {
    uint32_t hsz = 0;
    if (!read_u32(data, size, mat_base, &hsz)) break;
    if (hsz < 0x80u || hsz > 0x200u) break;

    uint32_t vcount = 0;
    if (!read_u32(data, size, mat_base + 0x88, &vcount)) break;
    if (vcount == 0 || vcount > 200000 || vcount > 65535u) break;
    const size_t vstart = mat_base + 0x8C;
    const uint64_t vend =
        static_cast<uint64_t>(vstart) +
        static_cast<uint64_t>(vcount) * static_cast<uint64_t>(kStride);
    if (vend + 4ull > size) break;

    uint32_t tri_count = 0;
    if (!read_u32(data, size, static_cast<size_t>(vend), &tri_count)) break;
    if (tri_count == 0 || tri_count > 500000) break;
    const uint64_t icount = static_cast<uint64_t>(tri_count) * 3ull;
    const uint64_t istart = vend + 4ull;
    if (istart + icount * 4ull > size) break;

    // Sanity: a few verts must be finite before committing.
    bool verts_ok = true;
    for (uint32_t vi = 0; vi < vcount && vi < 8u; ++vi) {
      float x = 0, y = 0, z = 0;
      std::memcpy(&x, data + vstart + vi * kStride, 4);
      std::memcpy(&y, data + vstart + vi * kStride + 4, 4);
      std::memcpy(&z, data + vstart + vi * kStride + 8, 4);
      if (!(x == x) || !(y == y) || !(z == z) || std::fabs(x) > 1.0e6f ||
          std::fabs(y) > 1.0e6f || std::fabs(z) > 1.0e6f) {
        verts_ok = false;
        break;
      }
    }
    if (!verts_ok) break;

    MeshSubmesh sm;
    const size_t nstart = mat_base + 0x68;
    const size_t nlim = mat_base + hsz < size ? mat_base + hsz : size;
    size_t nend = nstart;
    while (nend < nlim && data[nend] != 0) ++nend;
    if (nend > nstart)
      sm.name.assign(reinterpret_cast<const char*>(data + nstart), nend - nstart);

    if (mat_base + 0x10 <= size) {
      float r = 0, g = 0, b = 0;
      std::memcpy(&r, data + mat_base + 0x04, 4);
      std::memcpy(&g, data + mat_base + 0x08, 4);
      std::memcpy(&b, data + mat_base + 0x0C, 4);
      sm.diffuse = 0xFF000000u | (chan(r) << 16) | (chan(g) << 8) | chan(b);
    }

    sm.verts.resize(vcount);
    for (uint32_t vi = 0; vi < vcount; ++vi) {
      MeshVertex& v = sm.verts[vi];
      const uint8_t* p = data + vstart + static_cast<size_t>(vi) * kStride;
      std::memcpy(&v.x, p, 12);
      std::memcpy(&v.nx, p + 12, 12);
      std::memcpy(&v.u, p + 24, 8);
      if (!(v.x == v.x) || !(v.y == v.y) || !(v.z == v.z) ||
          std::fabs(v.x) > 1.0e6f || std::fabs(v.y) > 1.0e6f ||
          std::fabs(v.z) > 1.0e6f) {
        v.x = v.y = v.z = 0.f;
        v.nx = 0.f;
        v.ny = 1.f;
        v.nz = 0.f;
      }
      if (!bounds_init) {
        out.bmin[0] = out.bmax[0] = v.x;
        out.bmin[1] = out.bmax[1] = v.y;
        out.bmin[2] = out.bmax[2] = v.z;
        bounds_init = true;
      } else {
        if (v.x < out.bmin[0]) out.bmin[0] = v.x;
        if (v.y < out.bmin[1]) out.bmin[1] = v.y;
        if (v.z < out.bmin[2]) out.bmin[2] = v.z;
        if (v.x > out.bmax[0]) out.bmax[0] = v.x;
        if (v.y > out.bmax[1]) out.bmax[1] = v.y;
        if (v.z > out.bmax[2]) out.bmax[2] = v.z;
      }
    }

    sm.indices.resize(static_cast<size_t>(icount));
    bool idx_ok = true;
    for (uint64_t i = 0; i < icount; ++i) {
      uint32_t idx = 0;
      if (!read_u32(data, size, static_cast<size_t>(istart + i * 4ull), &idx) ||
          idx >= vcount) {
        idx_ok = false;
        break;
      }
      sm.indices[static_cast<size_t>(i)] = static_cast<uint16_t>(idx);
    }
    if (!idx_ok) break;

    out.subs.push_back(std::move(sm));
    mat_base = static_cast<size_t>(istart + icount * 4ull);
    while (mat_base + 4 <= size) {
      uint32_t z = 0;
      if (!read_u32(data, size, mat_base, &z) || z != 0) break;
      mat_base += 4;
    }
  }

  out.ready = !out.subs.empty();
  return out.ready;
}

}  // namespace

bool render_d3d9_mesh_create_from_memory(void* key, const uint8_t* data,
                                         size_t size, const char* label) {
  if (!key || !data || size < 16) return false;
  {
    auto it = g_meshes.find(key);
    if (it != g_meshes.end()) release_mesh_textures(key, it->second);
  }
  MeshState st;
  st.label = label ? label : "";
  if (!parse_scx_v4(data, size, st) && !parse_scx_v3(data, size, st)) {
    g_meshes.erase(key);
    return false;
  }
  bind_mesh_textures(key, st);
  g_meshes[key] = std::move(st);
  return true;
}

bool render_d3d9_mesh_create_from_file(void* key, const char* path) {
  if (!key || !path || !path[0]) return false;
  FILE* f = std::fopen(path, "rb");
  if (!f) {
    std::string resolved = rpak_resolve_path(path);
    if (!resolved.empty()) f = std::fopen(resolved.c_str(), "rb");
  }
  if (!f) return false;
  std::fseek(f, 0, SEEK_END);
  const long sz = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (sz <= 0 || sz > 64 * 1024 * 1024) {
    std::fclose(f);
    return false;
  }
  std::vector<uint8_t> buf(static_cast<size_t>(sz));
  if (std::fread(buf.data(), 1, buf.size(), f) != buf.size()) {
    std::fclose(f);
    return false;
  }
  std::fclose(f);
  // Prefer the path we actually opened so texture-neighbour resolution works.
  std::string label = path;
  std::string resolved = rpak_resolve_path(path);
  if (!resolved.empty()) label = resolved;
  return render_d3d9_mesh_create_from_memory(key, buf.data(), buf.size(),
                                             label.c_str());
}

bool render_d3d9_mesh_create_skydome(void* key, float radius) {
  if (!key || radius <= 0.f) return false;
  {
    auto it = g_meshes.find(key);
    if (it != g_meshes.end()) release_mesh_textures(key, it->second);
  }
  constexpr int kStacks = 16;
  constexpr int kSlices = 32;
  constexpr float kPi = 3.14159265f;
  MeshSubmesh sm;
  sm.name = "skydome";
  sm.diffuse = 0xFF6EC1FFu;  // daytime sky tint until PTX bind
  sm.verts.reserve(static_cast<size_t>((kStacks + 1) * (kSlices + 1)));
  for (int lat = 0; lat <= kStacks; ++lat) {
    const float v = static_cast<float>(lat) / static_cast<float>(kStacks);
    const float phi = v * kPi;  // 0..pi
    const float y = std::cos(phi);
    const float r = std::sin(phi);
    for (int lon = 0; lon <= kSlices; ++lon) {
      const float u = static_cast<float>(lon) / static_cast<float>(kSlices);
      const float theta = u * 2.f * kPi;
      const float x = r * std::cos(theta);
      const float z = r * std::sin(theta);
      MeshVertex vtx;
      vtx.x = x * radius;
      vtx.y = y * radius;
      vtx.z = z * radius;
      // Inward normals (viewed from inside the dome).
      vtx.nx = -x;
      vtx.ny = -y;
      vtx.nz = -z;
      vtx.u = u;
      vtx.v = v;
      sm.verts.push_back(vtx);
    }
  }
  for (int lat = 0; lat < kStacks; ++lat) {
    for (int lon = 0; lon < kSlices; ++lon) {
      const uint16_t i0 =
          static_cast<uint16_t>(lat * (kSlices + 1) + lon);
      const uint16_t i1 = static_cast<uint16_t>(i0 + 1);
      const uint16_t i2 =
          static_cast<uint16_t>((lat + 1) * (kSlices + 1) + lon);
      const uint16_t i3 = static_cast<uint16_t>(i2 + 1);
      sm.indices.push_back(i0);
      sm.indices.push_back(i2);
      sm.indices.push_back(i1);
      sm.indices.push_back(i1);
      sm.indices.push_back(i2);
      sm.indices.push_back(i3);
    }
  }
  MeshState st;
  st.label = "skydome_proc";
  st.bmin[0] = st.bmin[1] = st.bmin[2] = -radius;
  st.bmax[0] = st.bmax[1] = st.bmax[2] = radius;
  st.subs.push_back(std::move(sm));
  st.ready = true;
  g_meshes[key] = std::move(st);
  return true;
}

bool render_d3d9_mesh_scale_vertices(void* key, float sx, float sy, float sz) {
  if (!key) return false;
  auto it = g_meshes.find(key);
  if (it == g_meshes.end() || !it->second.ready) return false;
  MeshState& st = it->second;
  bool any = false;
  float bmin[3] = {1e30f, 1e30f, 1e30f};
  float bmax[3] = {-1e30f, -1e30f, -1e30f};
  for (MeshSubmesh& sm : st.subs) {
    for (MeshVertex& v : sm.verts) {
      v.x *= sx;
      v.y *= sy;
      v.z *= sz;
      if (v.x < bmin[0]) bmin[0] = v.x;
      if (v.y < bmin[1]) bmin[1] = v.y;
      if (v.z < bmin[2]) bmin[2] = v.z;
      if (v.x > bmax[0]) bmax[0] = v.x;
      if (v.y > bmax[1]) bmax[1] = v.y;
      if (v.z > bmax[2]) bmax[2] = v.z;
      any = true;
    }
  }
  if (any) {
    for (int i = 0; i < 3; ++i) {
      st.bmin[i] = bmin[i];
      st.bmax[i] = bmax[i];
    }
  }
  return true;
}

bool render_d3d9_mesh_clone(void* dst, void* src) {
  if (!dst || !src || dst == src) return false;
  auto it = g_meshes.find(src);
  if (it == g_meshes.end() || !it->second.ready) return false;
  {
    auto dit = g_meshes.find(dst);
    if (dit != g_meshes.end()) release_mesh_textures(dst, dit->second);
  }
  MeshState st = it->second;
  for (size_t i = 0; i < st.subs.size(); ++i) {
    if (st.subs[i].texture_key == mesh_sub_tex_key(src, i))
      st.subs[i].texture_key = nullptr;
  }
  bind_mesh_textures(dst, st);
  g_meshes[dst] = std::move(st);
  return true;
}

void render_d3d9_mesh_set_texture_at(void* mesh_key, int32_t submesh,
                                     void* texture_key) {
  if (!mesh_key) return;
  auto it = g_meshes.find(mesh_key);
  if (it == g_meshes.end() || submesh < 0 ||
      static_cast<size_t>(submesh) >= it->second.subs.size())
    return;
  it->second.subs[static_cast<size_t>(submesh)].texture_key = texture_key;
}

void render_d3d9_mesh_set_texture(void* mesh_key, void* texture_key) {
  render_d3d9_mesh_set_texture_at(mesh_key, 0, texture_key);
}

void render_d3d9_mesh_set_color(void* key, int32_t argb) {
  if (!key) return;
  MeshXform& xf = g_mesh_xforms[key];
  xf.color = argb;
  xf.color_set = 1;
}

int32_t render_d3d9_mesh_get_color(void* key) {
  if (!key) return 0;
  auto it = g_mesh_xforms.find(key);
  if (it == g_mesh_xforms.end() || !it->second.color_set) return 0;
  return it->second.color;
}

void render_d3d9_mesh_destroy(void* key) {
  if (!key) return;
  auto it = g_meshes.find(key);
  if (it != g_meshes.end()) {
    release_mesh_textures(key, it->second);
    g_meshes.erase(it);
  }
  g_mesh_xforms.erase(key);
  g_mesh_bones.erase(key);
  g_mesh_bone_alias.erase(key);
  g_mesh_bone_next.erase(key);
  for (size_t i = 0; i < g_mesh_queue.size();) {
    if (g_mesh_queue[i] == key)
      g_mesh_queue.erase(g_mesh_queue.begin() + static_cast<std::ptrdiff_t>(i));
    else
      ++i;
  }
}

bool render_d3d9_mesh_ready(void* key) {
  auto it = g_meshes.find(key);
  return it != g_meshes.end() && it->second.ready;
}

int32_t render_d3d9_mesh_submesh_count(void* key) {
  auto it = g_meshes.find(key);
  if (it == g_meshes.end()) return 0;
  return static_cast<int32_t>(it->second.subs.size());
}

int32_t render_d3d9_mesh_vertex_count(void* key) {
  auto it = g_meshes.find(key);
  if (it == g_meshes.end()) return 0;
  int32_t n = 0;
  for (const auto& sm : it->second.subs)
    n += static_cast<int32_t>(sm.verts.size());
  return n;
}

int32_t render_d3d9_mesh_index_count(void* key) {
  auto it = g_meshes.find(key);
  if (it == g_meshes.end()) return 0;
  int32_t n = 0;
  for (const auto& sm : it->second.subs)
    n += static_cast<int32_t>(sm.indices.size());
  return n;
}

int32_t render_d3d9_mesh_textured_count(void* key) {
  auto it = g_meshes.find(key);
  if (it == g_meshes.end()) return 0;
  int32_t n = 0;
  for (const auto& sm : it->second.subs) {
    if (sm.texture_key && render_d3d9_texture_ready(sm.texture_key)) ++n;
  }
  return n;
}

void* render_d3d9_mesh_get_texture(void* key, int32_t submesh) {
  auto it = g_meshes.find(key);
  if (it == g_meshes.end() || submesh < 0 ||
      static_cast<size_t>(submesh) >= it->second.subs.size())
    return nullptr;
  return it->second.subs[static_cast<size_t>(submesh)].texture_key;
}

bool render_d3d9_mesh_local_bounds(void* key, float bmin[3], float bmax[3]) {
  auto it = g_meshes.find(key);
  if (it == g_meshes.end() || !it->second.ready || !bmin || !bmax) return false;
  for (int i = 0; i < 3; ++i) {
    bmin[i] = it->second.bmin[i];
    bmax[i] = it->second.bmax[i];
  }
  return true;
}

int32_t render_d3d9_mesh_copy_positions(void* key, float* xyz_interleaved,
                                        int32_t max_count) {
  if (!key || !xyz_interleaved || max_count <= 0) return 0;
  auto it = g_meshes.find(key);
  if (it == g_meshes.end() || !it->second.ready) return 0;
  int32_t n = 0;
  // Stride sample for huge meshes.
  int32_t total = 0;
  for (const auto& sm : it->second.subs)
    total += static_cast<int32_t>(sm.verts.size());
  const int32_t step = total > max_count * 4 ? (total / max_count) : 1;
  int32_t seen = 0;
  for (const auto& sm : it->second.subs) {
    for (const MeshVertex& v : sm.verts) {
      if ((seen++ % step) != 0) continue;
      if (n >= max_count) return n;
      xyz_interleaved[n * 3 + 0] = v.x;
      xyz_interleaved[n * 3 + 1] = v.y;
      xyz_interleaved[n * 3 + 2] = v.z;
      ++n;
    }
  }
  return n;
}

void render_d3d9_mesh_set_transform(void* key, float px, float py, float pz,
                                    float yaw, float pitch, float roll,
                                    float sx, float sy, float sz) {
  if (!key) return;
  MeshXform& xf = g_mesh_xforms[key];
  xf.px = px;
  xf.py = py;
  xf.pz = pz;
  xf.oy = yaw;
  xf.op = pitch;
  xf.or_ = roll;
  xf.sx = sx != 0.f ? sx : 1.f;
  xf.sy = sy != 0.f ? sy : 1.f;
  xf.sz = sz != 0.f ? sz : 1.f;
}

// PE ResHandle_Link @ 0x004290F0 (this = node+0x44, a2 = hook+0x0C).
static void res_handle_link_node44(void* node, BoneParentHook* hook) {
  if (!node || !hook) return;
  auto* head = reinterpret_cast<void**>(reinterpret_cast<char*>(node) + 0x44);
  void* cur = head[1];  // node+0x48
  if (cur) *reinterpret_cast<void**>(cur) = &hook->link0;
  hook->link0 = nullptr;
  hook->link1 = cur;
  head[1] = &hook->link0;
  hook->owner_node = node;
}

// PE ResHandle_Rebind @ 0x00429060 (this = bone+0x44|light+0xC, a2 = owner_node|0).
// Exported for voidEvent add_light owner path (RenderNatives / Resources.h).
void res_handle_rebind(void* rh /*bone+0x44*/, void* owner_node) {
  if (!rh) return;
  auto* t = reinterpret_cast<void**>(rh);
  void* old_owner = t[3];  // rh+0x0C
  if (old_owner == owner_node) return;
  if (old_owner) {
    // Unlink from old owner+0x48 list (same shape as hook dtor slice).
    if (t[0]) {
      *reinterpret_cast<void**>(reinterpret_cast<char*>(t[0]) + 4) = t[1];
    } else {
      *reinterpret_cast<void**>(reinterpret_cast<char*>(old_owner) + 0x48) =
          t[1];
    }
    if (t[1]) *reinterpret_cast<void**>(t[1]) = t[0];
    t[3] = nullptr;
    t[2] = nullptr;
    t[0] = nullptr;
    t[1] = nullptr;
  }
  if (owner_node) {
    auto* head =
        reinterpret_cast<void**>(reinterpret_cast<char*>(owner_node) + 0x44);
    void* cur = head[1];  // owner+0x48
    if (cur) *reinterpret_cast<void**>(cur) = rh;
    t[0] = nullptr;
    t[1] = cur;
    head[1] = rh;
    t[3] = owner_node;
    t[2] = *reinterpret_cast<void**>(reinterpret_cast<char*>(owner_node) +
                                     0x50);  // rh+8 ← *(owner+0x50)
  } else {
    t[3] = nullptr;
    t[2] = nullptr;
    t[0] = nullptr;
    t[1] = nullptr;
  }
}

// PE ResHandle_unlinkIndexedSlot @ 0x004293B0 (idx=1 → bone+0x0C/+0x10).
static void res_handle_unlink_indexed_slot1(PeBoneNode* bone) {
  if (!bone) return;
  auto* b = bone->raw;
  void* next = *reinterpret_cast<void**>(b + 0x0C);
  void* prev = *reinterpret_cast<void**>(b + 0x10);
  if (next && prev) {
    *reinterpret_cast<void**>(reinterpret_cast<char*>(next) + 0x10) = prev;
    *reinterpret_cast<void**>(reinterpret_cast<char*>(prev) + 0x0C) = next;
  }
  *reinterpret_cast<void**>(b + 0x0C) = nullptr;
  *reinterpret_cast<void**>(b + 0x10) = nullptr;
}

// PE ResHandle_insertIndexedSlot @ 0x0048F470 with this=payload+0x150, idx=1
// ≡ HEAD insert at payload+0x168 sentinel / +0x178 head (a4==0 manual path).
static void pe_bone_own_list_insert(PeBoneOwnList& own, PeBoneNode* bone) {
  if (!bone) return;
  auto* base = own.raw;
  void* sentinel = base + 0x168;
  void** head_slot = reinterpret_cast<void**>(base + 0x178);
  // Ensure empty-list init (map default-ctor already did; defend OOB reuse).
  if (*head_slot == nullptr) {
    *head_slot = sentinel;
    *reinterpret_cast<void**>(reinterpret_cast<char*>(sentinel) + 0x0C) =
        sentinel;
  }
  void* old_head = *head_slot;
  *reinterpret_cast<void**>(reinterpret_cast<char*>(old_head) + 0x0C) = bone;
  *reinterpret_cast<void**>(bone->raw + 0x0C) = sentinel;
  *reinterpret_cast<void**>(bone->raw + 0x10) = old_head;
  *head_slot = bone;
}

static PeBoneNode* pe_bone_ensure(MeshXform& xf) {
  if (!xf.pe_bone) xf.pe_bone = new PeBoneNode{};
  return xf.pe_bone;
}

static int32_t pe_bone_type_field(PeBoneNode* bone) {
  if (!bone) return 0;
  // bone[+0x4C] = ResHandle at +0x44 field+8 (a3[19] gate / type-match).
  return *reinterpret_cast<int32_t*>(bone->raw + 0x4C);
}

static void pe_bone_store_hook(PeBoneNode* bone, BoneParentHook* hook) {
  if (!bone) return;
  *reinterpret_cast<BoneParentHook**>(bone->raw + 0xAC) = hook;
}

// PE ResHandle_maybeRelinkLod @ 0x00425150 — gate then RelinkLodSlot
// @ 0x00537790 (Resources: tryRelinkLod + freelist skip). Called from
// both a4!=0 arms @ 0x48BEA9 / 0x48BEC0.
static void res_handle_maybe_relink_lod(void* parent_key) {
  if (!parent_key) return;
  void* inner = native_ptr_node(reinterpret_cast<InvObject*>(parent_key));
  res_handle_maybe_relink_lod_node(inner);
}

// PE BoneParentHook_dtor @ 0x0045F8B0 (a2&1 → free): unlink ResHandle slice
// then payload+0x78 dllist (same splice as ResListNode_dtor @ 0x00429150).
static void bone_parent_hook_destroy(BoneParentHook* hook, int32_t* node_flags) {
  constexpr int32_t kHookFlag = 0x4000;
  if (!hook) return;
  // Unlink from node+0x44 list (hook+0x0C slice).
  if (hook->owner_node) {
    void* link = &hook->link0;
    auto* l0 = reinterpret_cast<void**>(link);
    void* owner = hook->owner_node;
    if (l0[0]) {
      *reinterpret_cast<void**>(reinterpret_cast<char*>(l0[0]) + 4) = l0[1];
    } else {
      *reinterpret_cast<void**>(reinterpret_cast<char*>(owner) + 0x48) = l0[1];
    }
    if (l0[1]) *reinterpret_cast<void**>(l0[1]) = l0[0];
    hook->owner_node = nullptr;
    hook->node_field50 = 0;
    hook->link0 = nullptr;
    hook->link1 = nullptr;
  }
  // Unlink from payload+0x78 dllist (+4/+8).
  if (hook->list_back && hook->list_fwd) {
    *reinterpret_cast<void**>(reinterpret_cast<char*>(hook->list_fwd) + 8) =
        hook->list_back;
    *reinterpret_cast<void**>(reinterpret_cast<char*>(hook->list_back) + 4) =
        hook->list_fwd;
  }
  hook->list_fwd = nullptr;
  hook->list_back = nullptr;
  if (node_flags) *node_flags &= ~kHookFlag;
  delete hook;
}

// PE ResPayload_createBoneParentHook @ 0x0053F7E0.
// this=getPayload(parent); arg=render handle → child_node=*(handle+0xC).
static BoneParentHook* res_payload_create_bone_parent_hook(void* payload,
                                                          void* child_node) {
  if (!payload || !child_node) return nullptr;
  auto* flags =
      reinterpret_cast<int32_t*>(reinterpret_cast<char*>(child_node) + 0x54);
  // PE: flags!=0 && !(flags&0x4000). HostResNode has no PE flag word at
  // +0x54 (mid starts there); stock live nodes carry bits (e.g. |=0x200 @
  // 0x539988). Seed 0x200 when zero so the gate can pass on host nodes.
  if (*flags == 0) *flags = 0x200;
  if ((*flags & 0x4000) != 0) return nullptr;

  auto* hook = new BoneParentHook{};
  // ResListNode_ctor zeros +4/+8 then create zeros +0x0C..+0x18; vtbl set.
  // Host: vtbl unused (no PE virtual call); fields match asm stores.
  if (hook->owner_node != child_node) {
    res_handle_link_node44(child_node, hook);
    hook->node_field50 =
        *reinterpret_cast<int32_t*>(reinterpret_cast<char*>(child_node) + 0x50);
  }
  *flags |= 0x4000;

  // payload+0x78 sentinel / +0x80 tail (asm 0x53f88b..0x53f89d).
  auto* p = reinterpret_cast<char*>(payload);
  void* sentinel = p + 0x78;
  auto* tail_slot = reinterpret_cast<void**>(p + 0x80);
  // PE ctor leaves tail → sentinel; host mid pad is zero — init empty list.
  if (*tail_slot == nullptr) {
    *tail_slot = sentinel;
    *reinterpret_cast<void**>(p + 0x7C) = sentinel;
  }
  void* old_tail = *tail_slot;
  *reinterpret_cast<void**>(reinterpret_cast<char*>(old_tail) + 4) = hook;
  hook->list_back = old_tail;
  hook->list_fwd = sentinel;
  *tail_slot = hook;
  return hook;
}

void render_d3d9_mesh_set_parent(void* key, void* parent) {
  if (!key) return;
  // Ignore self-parent; break trivial cycles.
  if (parent == key) parent = nullptr;
  MeshXform& xf = g_mesh_xforms[key];

  // PE RenderRef_LinkOrUnlinkBone @ 0x0048BE10 (always from
  // SetBoneMatrixParentLink @ 0x0048BF50 after HEAD|+0x1800).
  // a1=render handle, a2=THIS payload, a3=bone*, a4=parent_ref|0.
  //   a4==0 && bone[+0x4C]!=0: teardown hook; ~0x4000; unlink +0xC/+0x10;
  //     ResHandle_Rebind(bone+0x44, 0); own-list insert +0x168/+0x178.
  //   a4!=0 && bone[+0x4C]==*(a4+8): ResHandle_maybeRelinkLod(a4) only.
  //   a4!=0 mismatch: unlinkIndexedSlot(1); Rebind(bone+0x44, *(a4+0xC));
  //     insertIndexedSlot(payload+0x150,1,bone); getPayload(A0)+hook→+0xAC;
  //     then maybeRelinkLod(a4) @ 0x48BEA9.
  // Host: PeBoneNode + PeBoneOwnList side map; RelinkLod gate @ 0x425150.
  constexpr int32_t kTagA0 = static_cast<int32_t>(0xA0000000u);

  auto node_flags_ptr = [](InvObject* o) -> int32_t* {
    void* node = native_ptr_node(o);
    if (!node) return nullptr;
    return reinterpret_cast<int32_t*>(reinterpret_cast<char*>(node) + 0x54);
  };
  auto teardown_hook = [&](InvObject* child) {
    if (!xf.bone_parent_hook) return;
    BoneParentHook* hook = xf.bone_parent_hook;
    xf.bone_parent_hook = nullptr;
    if (xf.pe_bone) pe_bone_store_hook(xf.pe_bone, nullptr);
    // PE: (**hook)(hook, 1) → BoneParentHook_dtor; then &= ~0x4000 on
    // *(handle+0xC)+0x54 (also cleared inside dtor path after vtbl call).
    bone_parent_hook_destroy(hook, node_flags_ptr(child));
  };

  auto* child = reinterpret_cast<InvObject*>(key);
  PeBoneNode* bone = pe_bone_ensure(xf);
  PeBoneOwnList& own = g_bone_own_lists[key];

  if (!parent) {
    // a4==0: only when bone[+0x4C]!=0 (PE gate @ 0x48BED0).
    teardown_hook(child);
    if (pe_bone_type_field(bone) != 0) {
      res_handle_unlink_indexed_slot1(bone);
      res_handle_rebind(bone->raw + 0x44, nullptr);
      pe_bone_own_list_insert(own, bone);
    }
    xf.parent = nullptr;
    xf.attach_bone = 0;
    return;
  }

  // Type-match stand-in: bone[+0x4C]==*(a4+8) → RelinkLod only @ 0x48BEC0.
  // Host handle+8 is alive (not PE type); InvObject* equality is the stand-in.
  if (xf.parent == parent) {
    res_handle_maybe_relink_lod(parent);
    return;
  }

  // mismatch path @ 0x48BE2F.. then maybeRelinkLod @ 0x48BEA9.
  teardown_hook(child);
  res_handle_unlink_indexed_slot1(bone);
  native_ptr_ensure(child);
  native_ptr_ensure(reinterpret_cast<InvObject*>(parent));
  void* child_inner = native_ptr_node(child);
  void* parent_inner = native_ptr_node(reinterpret_cast<InvObject*>(parent));
  // Seed owner+0x50 so Rebind writes non-zero bone[+0x4C] (a4==0 gate later).
  if (parent_inner) {
    auto* p50 =
        reinterpret_cast<int32_t*>(reinterpret_cast<char*>(parent_inner) + 0x50);
    if (*p50 == 0) *p50 = 1;
  }
  res_handle_rebind(bone->raw + 0x44, parent_inner);
  pe_bone_own_list_insert(own, bone);
  void* payload =
      parent_inner ? res_handle_get_payload(parent_inner, kTagA0) : nullptr;
  if (payload && child_inner) {
    xf.bone_parent_hook =
        res_payload_create_bone_parent_hook(payload, child_inner);
    pe_bone_store_hook(bone, xf.bone_parent_hook);
  } else {
    pe_bone_store_hook(bone, nullptr);
  }
  xf.parent = parent;
  res_handle_maybe_relink_lod(parent);
}

void render_d3d9_mesh_set_attach_bone(void* key, int32_t bone_id) {
  if (!key) return;
  g_mesh_xforms[key].attach_bone = bone_id < 0 ? 0 : bone_id;
}

void* render_d3d9_mesh_get_parent(void* key) {
  if (!key) return nullptr;
  auto it = g_mesh_xforms.find(key);
  return it == g_mesh_xforms.end() ? nullptr : it->second.parent;
}

int32_t render_d3d9_mesh_get_attach_bone(void* key) {
  if (!key) return 0;
  auto it = g_mesh_xforms.find(key);
  return it == g_mesh_xforms.end() ? 0 : it->second.attach_bone;
}

int32_t render_d3d9_mesh_get_bone_id(void* key, const char* alias) {
  // PE getBoneId @ 0x00481020: handle 0 → Mighty ERROR, return 0 (never -1).
  if (!key) return 0;
  std::string name = alias ? alias : "";
  // Stock Camera path attaches under parent + "bone00".
  if (name.empty() || name == "bone00" || name == "root" || name == "Root")
    return 0;
  auto& aliases = g_mesh_bone_alias[key];
  auto it = aliases.find(name);
  if (it != aliases.end()) return it->second;
  int32_t& next = g_mesh_bone_next[key];
  if (next < 1) next = 1;
  const int32_t id = next++;
  aliases[name] = id;
  return id;
}

void render_d3d9_mesh_set_bone_local(void* key, int32_t bone_id, float px,
                                     float py, float pz, float yaw, float pitch,
                                     float roll) {
  if (!key || bone_id < 0) return;
  MeshXform& b = g_mesh_bones[key][bone_id];
  b.px = px;
  b.py = py;
  b.pz = pz;
  b.oy = yaw;
  b.op = pitch;
  b.or_ = roll;
  b.sx = b.sy = b.sz = 1.f;
}

void render_d3d9_mesh_get_transform(void* key, float* px, float* py, float* pz,
                                    float* yaw, float* pitch, float* roll,
                                    float* sx, float* sy, float* sz) {
  MeshXform xf;
  auto it = g_mesh_xforms.find(key);
  if (it != g_mesh_xforms.end()) xf = it->second;
  if (px) *px = xf.px;
  if (py) *py = xf.py;
  if (pz) *pz = xf.pz;
  if (yaw) *yaw = xf.oy;
  if (pitch) *pitch = xf.op;
  if (roll) *roll = xf.or_;
  if (sx) *sx = xf.sx;
  if (sy) *sy = xf.sy;
  if (sz) *sz = xf.sz;
}

void render_d3d9_mesh_world_origin(void* key, float* wx, float* wy, float* wz) {
  float x = 0, y = 0, z = 0;
#ifdef _WIN32
  D3DMATRIX world{};
  resolve_world(key, &world, 0);
  x = world.m[3][0];
  y = world.m[3][1];
  z = world.m[3][2];
#else
  (void)key;
  auto it = g_mesh_xforms.find(key);
  if (it != g_mesh_xforms.end()) {
    x = it->second.px;
    y = it->second.py;
    z = it->second.pz;
  }
#endif
  if (wx) *wx = x;
  if (wy) *wy = y;
  if (wz) *wz = z;
}

int32_t render_d3d9_mesh_vb_stride_from_fvf(uint32_t pe_fvf) {
  // Soft PE GfxVbLayout_FromFvf @ 0x4FE980 — running offset at layout[17]
  // (= wrapper+0x60). Field offset table OOS; stride only.
  int32_t off = 0;
  if ((pe_fvf & 1u) != 0) {
    const int32_t blends = static_cast<int32_t>((pe_fvf >> 8) & 7u);
    off = 12;
    for (int32_t i = 0; i < blends; ++i) off += 4;
  }
  if ((pe_fvf & 0x800u) != 0) off += 4;
  if ((pe_fvf & 2u) != 0) off += 12;
  if ((pe_fvf & 4u) != 0) off += 4;
  if ((pe_fvf & 8u) != 0) off += 4;
  const int32_t tex_n = static_cast<int32_t>((pe_fvf >> 4) & 0xFu);
  int32_t bit = 16;
  for (int32_t t = 0; t < tex_n; ++t) {
    switch ((pe_fvf >> bit) & 3u) {
      case 0:
        off += 4;
        break;
      case 1:
        off += 8;
        break;
      case 2:
        off += 12;
        break;
      case 3:
        off += 16;
        break;
      default:
        break;
    }
    bit += 2;
  }
  return off;
}

uint32_t render_d3d9_mesh_invo_flags_to_fvf(uint32_t invo_flags,
                                            int32_t tex_count) {
  // Soft PE UploadVbFvf @ 0x503475..0x5034EE (before GfxVbLayout_FromFvf).
  uint32_t fvf = (invo_flags & 1u) != 0 ? 1u : 0u;
  if ((invo_flags & 0x40u) != 0) fvf |= 2u;
  if ((invo_flags & 0x80u) != 0) fvf |= 4u;
  if ((invo_flags & 0x100u) != 0) fvf |= 8u;
  if ((invo_flags & 4u) != 0 || (invo_flags & 0x18u) != 0) fvf |= 0x300u;
  if ((invo_flags & 0x20u) != 0) fvf |= 0x800u;
  if (tex_count < 0) tex_count = 0;
  if (tex_count > 8) tex_count = 8;
  fvf |= static_cast<uint32_t>(tex_count) << 4;
  // PE: for each UV slot, OR (1 << (16 + 2*i)) — texcoord format case0.
  for (int32_t i = 0, bit = 16; i < tex_count; ++i, bit += 2)
    fvf |= 1u << bit;
  return fvf;
}

bool render_d3d9_mesh_vb_create(void* key, int32_t vert_count, uint32_t pe_fvf,
                                int32_t stride, const void* bytes,
                                size_t bytes_len) {
  // Soft PE AsyncLoad_Mesh_UploadVbFvf @ 0x503400 — alloc wrapper + blob;
  // CreateVertexBuffer with FVF=pe_fvf (Present SetFVF bind). Attach / pack /
  // AABB OOS (ParseInvo later).
  if (!key || vert_count <= 0) return false;
  int32_t stride_use = stride;
  if (stride_use <= 0)
    stride_use = render_d3d9_mesh_vb_stride_from_fvf(pe_fvf);
  if (stride_use <= 0) return false;
  const size_t need =
      static_cast<size_t>(vert_count) * static_cast<size_t>(stride_use);

  MeshVbState& st = g_mesh_vbs[key];
#ifdef _WIN32
  if (st.vb) {
    st.vb->Release();
    st.vb = nullptr;
  }
#endif
  st.vert_count = vert_count;
  st.stride = stride_use;
  st.pe_fvf = pe_fvf;
  st.cpu.assign(need, 0);
  if (bytes && bytes_len > 0) {
    const size_t n = bytes_len < need ? bytes_len : need;
    std::memcpy(st.cpu.data(), bytes, n);
  }
  st.ready = true;

#ifdef _WIN32
  if (!g_dev) return true;  // bookkeeping without GPU (headless), like dims
  // PE wrapper+0x0C pe_fvf → CreateVertexBuffer FVF (host soft Present bind).
  if (FAILED(g_dev->CreateVertexBuffer(static_cast<UINT>(need), 0, pe_fvf,
                                       D3DPOOL_MANAGED, &st.vb, nullptr)) ||
      !st.vb) {
    st.vb = nullptr;
    return false;
  }
  if (bytes && bytes_len > 0) {
    void* locked = nullptr;
    if (SUCCEEDED(st.vb->Lock(0, static_cast<UINT>(need), &locked, 0)) &&
        locked) {
      const size_t n = bytes_len < need ? bytes_len : need;
      std::memcpy(locked, bytes, n);
      st.vb->Unlock();
    }
  }
  return true;
#else
  (void)bytes_len;
  return st.ready;
#endif
}

void render_d3d9_mesh_vb_destroy(void* key) {
  if (!key) return;
  auto it = g_mesh_vbs.find(key);
  if (it == g_mesh_vbs.end()) return;
#ifdef _WIN32
  // W35-02: soft PE VBSlots freelist (device+0x320) — enqueue; DrainVBSlots.
  if (it->second.vb) {
    g_vb_drain_list.push_back(it->second.vb);
    it->second.vb = nullptr;
  }
#endif
  g_mesh_vbs.erase(it);
}

bool render_d3d9_mesh_vb_ready(void* key) {
  if (!key) return false;
  auto it = g_mesh_vbs.find(key);
  return it != g_mesh_vbs.end() && it->second.ready;
}

int32_t render_d3d9_mesh_vb_vert_count(void* key) {
  if (!key) return 0;
  auto it = g_mesh_vbs.find(key);
  return it == g_mesh_vbs.end() ? 0 : it->second.vert_count;
}

int32_t render_d3d9_mesh_vb_stride(void* key) {
  if (!key) return 0;
  auto it = g_mesh_vbs.find(key);
  return it == g_mesh_vbs.end() ? 0 : it->second.stride;
}

uint32_t render_d3d9_mesh_vb_fvf(void* key) {
  if (!key) return 0;
  auto it = g_mesh_vbs.find(key);
  return it == g_mesh_vbs.end() ? 0u : it->second.pe_fvf;
}

void* render_d3d9_mesh_vb_d3d(void* key) {
  if (!key) return nullptr;
  auto it = g_mesh_vbs.find(key);
  if (it == g_mesh_vbs.end()) return nullptr;
#ifdef _WIN32
  return it->second.vb;
#else
  return nullptr;
#endif
}

bool render_d3d9_mesh_ib_create(void* key, int32_t index_count,
                                const uint16_t* indices) {
  // Soft PE AsyncLoad_Mesh_UploadIbTris @ 0x5038E0 — malloc 20B wrapper
  // (count / blob / type=2 / stride=2) + 2*count bytes; vtbl+20 attach OOS.
  if (!key || index_count <= 0) return false;
  MeshIbState& st = g_mesh_ibs[key];
#ifdef _WIN32
  if (st.ib) {
    st.ib->Release();
    st.ib = nullptr;
  }
#endif
  st.index_count = index_count;
  st.cpu.assign(static_cast<size_t>(index_count), 0);
  if (indices) {
    std::memcpy(st.cpu.data(), indices,
                static_cast<size_t>(index_count) * sizeof(uint16_t));
  }
  st.ready = true;

#ifdef _WIN32
  if (!g_dev) return true;
  const UINT bytes =
      static_cast<UINT>(index_count) * static_cast<UINT>(sizeof(uint16_t));
  if (FAILED(g_dev->CreateIndexBuffer(bytes, 0, D3DFMT_INDEX16, D3DPOOL_MANAGED,
                                      &st.ib, nullptr)) ||
      !st.ib) {
    st.ib = nullptr;
    return false;
  }
  if (indices) {
    void* locked = nullptr;
    if (SUCCEEDED(st.ib->Lock(0, bytes, &locked, 0)) && locked) {
      std::memcpy(locked, indices, bytes);
      st.ib->Unlock();
    }
  }
  return true;
#else
  return st.ready;
#endif
}

void render_d3d9_mesh_ib_destroy(void* key) {
  if (!key) return;
  auto it = g_mesh_ibs.find(key);
  if (it == g_mesh_ibs.end()) return;
#ifdef _WIN32
  // W35-02: soft PE IBSlots freelist (device+0x324) — enqueue; DrainIBSlots.
  if (it->second.ib) {
    g_ib_drain_list.push_back(it->second.ib);
    it->second.ib = nullptr;
  }
#endif
  g_mesh_ibs.erase(it);
}

bool render_d3d9_mesh_ib_ready(void* key) {
  if (!key) return false;
  auto it = g_mesh_ibs.find(key);
  return it != g_mesh_ibs.end() && it->second.ready;
}

int32_t render_d3d9_mesh_ib_index_count(void* key) {
  if (!key) return 0;
  auto it = g_mesh_ibs.find(key);
  return it == g_mesh_ibs.end() ? 0 : it->second.index_count;
}

void* render_d3d9_mesh_ib_d3d(void* key) {
  if (!key) return nullptr;
  auto it = g_mesh_ibs.find(key);
  if (it == g_mesh_ibs.end()) return nullptr;
#ifdef _WIN32
  return it->second.ib;
#else
  return nullptr;
#endif
}

void render_d3d9_mesh_queue_clear() { g_mesh_queue.clear(); }

void render_d3d9_mesh_queue_add(void* key) {
  if (!key || !render_d3d9_mesh_ready(key)) return;
  g_mesh_queue.push_back(key);
}

int32_t render_d3d9_mesh_queue_count() {
  return static_cast<int32_t>(g_mesh_queue.size());
}

}  // namespace inv
