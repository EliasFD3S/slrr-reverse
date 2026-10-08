// Fragment 2/2 of System.cpp — included, not compiled on its own.
// Split only to stay under the 128 KB editor buffer cap; the
// translation unit is unchanged. See native/tools/split_source.py.


// Soft PE AsyncLoad_Mesh_CreateVbWrap_Init @ 0x501880 / case0 @ 0x502E6B.
AsyncLoadCreateVbWrap* asyncload_cvb_wrap_create(const uint8_t* payload,
                                                 uint32_t payload_len) {
  auto* w = static_cast<AsyncLoadCreateVbWrap*>(
      std::malloc(sizeof(AsyncLoadCreateVbWrap)));
  if (!w) return nullptr;
  std::memset(w, 0, sizeof(AsyncLoadCreateVbWrap));
  w->cvb = asyncload_create_vb_soft(payload, payload_len);
  return w;
}

// Soft PE PtrVec_PushGrow @ 0x5052A0 on job CreateVB wrap list (PE +0x48).
void asyncload_cvb_wrap_vec_push(AsyncLoadJob* self, AsyncLoadCreateVbWrap* w) {
  if (!self || !w) return;
  if (self->mesh_cvb_wrap_count == self->mesh_cvb_wrap_cap) {
    const int32_t ncap = self->mesh_cvb_wrap_cap + 16;  // PE grow +16
    auto* narr = static_cast<AsyncLoadCreateVbWrap**>(std::realloc(
        self->mesh_cvb_wraps,
        static_cast<size_t>(ncap) * sizeof(AsyncLoadCreateVbWrap*)));
    if (!narr) return;
    self->mesh_cvb_wraps = narr;
    self->mesh_cvb_wrap_cap = ncap;
  }
  self->mesh_cvb_wraps[self->mesh_cvb_wrap_count++] = w;
  self->mesh_cur_cvb_wrap = w;
}

// Soft PE AsyncLoad_Mesh_MatObj_BaseInit @ 0x505100 + overlay off_5F2C08.
AsyncLoadMatObj* asyncload_mat_create() {
  auto* m = static_cast<AsyncLoadMatObj*>(std::malloc(sizeof(AsyncLoadMatObj)));
  if (!m) return nullptr;
  std::memset(m, 0, sizeof(AsyncLoadMatObj));
  m->flags = 0x80000001u;  // PE +0x18 after case1-3 overlay
  return m;
}

// Soft PE AsyncLoad_Mesh_MatObj_AddTexId @ 0x4FE0F0 (vtbl+0x3C).
void asyncload_mat_add_tex(AsyncLoadMatObj* m, int32_t tex_id) {
  if (!m) return;
  if (m->tex_count == m->tex_cap) {
    const int32_t ncap = m->tex_cap + 2;  // PE grow +2
    auto* nids = static_cast<int32_t*>(
        std::realloc(m->tex_ids, static_cast<size_t>(ncap) * sizeof(int32_t)));
    if (!nids) return;
    m->tex_ids = nids;
    m->tex_cap = ncap;
  }
  m->tex_ids[m->tex_count++] = tex_id;
}

// Soft PE AsyncLoad_Mesh_MatObj_SetMode @ 0x4FE130 (vtbl+0x40).
void asyncload_mat_set_mode(AsyncLoadMatObj* m, int32_t mode) {
  if (!m) return;
  m->mode = mode;
}

// Soft PE AsyncLoad_Mesh_MatObj_PostLoop @ 0x4FFFE0 (vtbl+0x24).
// Slice: D3DUSAGE from mode + MeshIsFontsPath (PE @ 0x500096..0x5000B8).
// OOS: +20 helper malloc/sub_498010; GfxVb/Ib_DtorRelease; device
//   vt+164/168 VB/IB create; GC liveType5; vtbl+32 bind.
int32_t asyncload_mat_post_loop_soft(AsyncLoadMatObj* m) {
  if (!m) return 0;
  int32_t usage = 1;
  if (g_asyncload_mesh_is_fonts_path != 0) usage = 4;
  if (m->mode == 1)
    usage = 16;
  else if (m->mode > 1 && m->mode <= 5)
    usage = 32;
  return usage;
}

// Soft PE AsyncLoad_Mesh_MatVec_Push @ 0x505120 — PE this=case0 wrap+4.
// W32A: branch onto mesh_cur_cvb_wrap MatVec when set; else job.mesh_mats.
void asyncload_mat_vec_push(AsyncLoadJob* self, AsyncLoadMatObj* m) {
  if (!self || !m) return;
  AsyncLoadCreateVbWrap* w = self->mesh_cur_cvb_wrap;
  if (w) {
    if (w->mat_count == w->mat_cap) {
      const int32_t ncap = w->mat_cap + 16;  // PE MatVec grow +16
      auto* narr = static_cast<AsyncLoadMatObj**>(std::realloc(
          w->mats, static_cast<size_t>(ncap) * sizeof(AsyncLoadMatObj*)));
      if (!narr) return;
      w->mats = narr;
      w->mat_cap = ncap;
    }
    w->mats[w->mat_count++] = m;
  } else {
    if (self->mesh_mat_count == self->mesh_mat_cap) {
      const int32_t ncap = self->mesh_mat_cap + 16;
      auto* narr = static_cast<AsyncLoadMatObj**>(std::realloc(
          self->mesh_mats,
          static_cast<size_t>(ncap) * sizeof(AsyncLoadMatObj*)));
      if (!narr) return;
      self->mesh_mats = narr;
      self->mesh_mat_cap = ncap;
    }
    self->mesh_mats[self->mesh_mat_count++] = m;
  }
  self->mesh_cur_mat = m;
}

// PE AsyncLoad_Mesh_ParseInvoVle3 @ 0x502070 (size 0xd6e).
// Soft: host INVO ver<=3 mesh walk (fonts path) using PE layout + Max
//   SLR_SCX_readV3 cross-check (scripts_3dmax_2026). PE callees MatHdr
//   Init/ClearMaps/CopyV3 @ 0x5080F0/100/160; VB FVF imm 0x15003F;
//   PackVertex@4FF710; ends flags|0x14. OOS: Gfx CreateVB vt+0xD0,
//   soft-VB side path, full MatHdr PE overlay / PostLoop D3D bind.
void asyncload_mesh_parse_invo_vle3(AsyncLoadJob* self) {
  if (!self) return;
  // PE @ VB malloc path: FVF immediate 1376319 (0x15003F) → FromFvf.
  constexpr uint32_t kVle3Fvf = 0x15003Fu;
  (void)gfxvb_layout_from_fvf(kVle3Fvf);
  (void)render_d3d9_mesh_vb_stride_from_fvf(kVle3Fvf);

  if (!self->buf || self->nbytes < 8) {
    asyncload_process_flag_014(self);
    return;
  }

  const auto* base = static_cast<const uint8_t*>(self->buf);
  const size_t nbytes = static_cast<size_t>(self->nbytes);
  const auto* hdr = reinterpret_cast<const uint32_t*>(base);
  const uint32_t ver = hdr[1];

  // Soft PE: ver==3 uses MatHdr_CopyV3@508160 + oneVertexSize mesh loop.
  // ver 1/2 legacy hdr sizes (56/52) remain OOS — still flag_014.
  if (ver != 3u) {
    asyncload_process_flag_014(self);
    return;
  }

  // PE ParseInvoVle3 prologue: clear AABB, mesh_aabb_init(+0xEC)=1.
  self->mesh_aabb_min[0] = self->mesh_aabb_min[1] = self->mesh_aabb_min[2] = 0.f;
  self->mesh_aabb_max[0] = self->mesh_aabb_max[1] = self->mesh_aabb_max[2] = 0.f;
  self->mesh_aabb_init = 1;
  self->mesh_vert_hi = 0;
  self->mesh_cur_mat = nullptr;
  self->mesh_cur_cvb_wrap = nullptr;

  // PE cursor starts at buf+8 (after magic+ver). Max SLR_SCX_readV3 same.
  size_t cur = 8u;
  void* const key = self;
  std::vector<uint8_t> staged;
  std::vector<uint16_t> indices16;

  while (cur + 4u <= nbytes) {
    // PE/Max: matSize dword; stop if not a material (Max: <68 or >1024).
    const uint32_t mat_size =
        *reinterpret_cast<const uint32_t*>(base + cur);
    if (mat_size < 68u || mat_size > 1024u || cur + mat_size > nbytes) break;
    // Max: oneVertexSize @ matStart+60 (after size+diff/spec/flags/maps).
    if (cur + 64u > nbytes) break;
    const uint32_t one_vert =
        *reinterpret_cast<const uint32_t*>(base + cur + 60u);
    const size_t mat_end = cur + static_cast<size_t>(mat_size);
    cur = mat_end;

    if (one_vert < 12u || one_vert > 256u) break;
    if (cur + 4u > nbytes) break;
    const int32_t nv =
        static_cast<int32_t>(*reinterpret_cast<const uint32_t*>(base + cur));
    cur += 4u;

    if (nv == 0) {
      // Max: sector marker — faces count always 0 too.
      if (cur + 4u > nbytes) break;
      cur += 4u;
      continue;
    }
    if (nv < 0 || nv > 500000) break;
    const size_t vert_bytes =
        static_cast<size_t>(nv) * static_cast<size_t>(one_vert);
    if (cur + vert_bytes > nbytes) break;

    // Soft PE UploadVbFvf flag set from Max field presence by stride.
    // pos always; nrm@>=24; uv1@>=32; uv2@>=40; color@>=44; uv3@>=60.
    uint32_t invo_flags = 1u;
    if (one_vert >= 24u) invo_flags |= 0x40u;
    if (one_vert >= 44u) invo_flags |= 0x80u;
    int32_t tex_count = 0;
    if (one_vert >= 32u) tex_count = 1;
    if (one_vert >= 40u) tex_count = 2;
    if (one_vert >= 60u) tex_count = 3;

    // Stage Soft order for asyncload_mesh_upload_vb_pack: pos, nrm, dif, uvs.
    // Max file order: pos, nrm, uv1, uv2, color BGRA, (pad8+uv3).
    size_t stage_stride = 12u;  // pos
    if ((invo_flags & 0x40u) != 0) stage_stride += 12u;
    if ((invo_flags & 0x80u) != 0) stage_stride += 4u;
    stage_stride += static_cast<size_t>(tex_count) * 8u;
    staged.assign(static_cast<size_t>(nv) * stage_stride, 0);

    for (int32_t vi = 0; vi < nv; ++vi) {
      // Max SLR_SCX_readV3 field order by oneVertexSize:
      //   pos@0, nrm@12, uv1@24, uv2@32, color@40, pad8+uv3@44/52.
      // Soft UploadVbFvf order: pos, nrm, dif, uv*.
      const uint8_t* src =
          base + cur + static_cast<size_t>(vi) * static_cast<size_t>(one_vert);
      uint8_t* dst = staged.data() + static_cast<size_t>(vi) * stage_stride;
      size_t d = 0;
      std::memcpy(dst + d, src + 0, 12);
      d += 12;
      if ((invo_flags & 0x40u) != 0) {
        std::memcpy(dst + d, src + 12, 12);
        d += 12;
      }
      if ((invo_flags & 0x80u) != 0) {
        std::memcpy(dst + d, src + 40, 4);
        d += 4;
      }
      if (tex_count >= 1) {
        std::memcpy(dst + d, src + 24, 8);
        d += 8;
      }
      if (tex_count >= 2) {
        std::memcpy(dst + d, src + 32, 8);
        d += 8;
      }
      if (tex_count >= 3) {
        std::memcpy(dst + d, src + 52, 8);
        d += 8;
      }
      (void)d;
    }

    AsyncLoadMatObj* m = asyncload_mat_create();
    if (m) {
      asyncload_mat_set_mode(m, 0);
      asyncload_mat_vec_push(self, m);
    }

    asyncload_mesh_upload_vb_pack(self, key, nv, invo_flags, tex_count,
                                  staged.data(),
                                  static_cast<uint32_t>(staged.size()),
                                  nullptr);
    if (self->mesh_cur_mat)
      (void)asyncload_mat_post_loop_soft(self->mesh_cur_mat);

    cur += vert_bytes;
    if (cur + 4u > nbytes) break;
    const int32_t nf =
        static_cast<int32_t>(*reinterpret_cast<const uint32_t*>(base + cur));
    cur += 4u;
    if (nf < 0 || nf > 500000) break;
    // Max/PE v3 faces: nf × 3 × u32 indices.
    const size_t face_bytes =
        static_cast<size_t>(nf) * 12u;
    if (cur + face_bytes > nbytes) break;
    indices16.clear();
    indices16.reserve(static_cast<size_t>(nf) * 3u);
    const auto* faces =
        reinterpret_cast<const uint32_t*>(base + cur);
    for (int32_t fi = 0; fi < nf * 3; ++fi) {
      const uint32_t idx = faces[fi];
      indices16.push_back(static_cast<uint16_t>(idx > 0xFFFFu ? 0u : idx));
    }
    if (!indices16.empty())
      render_d3d9_mesh_ib_create(
          key, static_cast<int32_t>(indices16.size()), indices16.data());
    cur += face_bytes;
  }

  asyncload_mesh_aabb_finalize(self);
  asyncload_process_flag_014(self);
}

// PE AsyncLoad_Mesh_ParseInvoChunks @ 0x502DE0.
// W27A: case4/5 → mesh_vb/ib_create. W28A: case4 soft UploadVbFvf walk
//   (PackVertex@4FF710 + AABB@+0xCC + radius finalize). Key = job*.
// W30A: case1-3 soft MatObj_BaseInit@505100 / AddTexId@4FE0F0 /
//   SetMode@4FE130 / MatVec_Push@505120.
// W32A: case0 soft CreateVB wrap (GfxDevice_CreateVB@4BEEF0 vt+0xCC) +
//   MatVec on wrap+4; case4 tex_count ← soft GetUvTexInfo (cvb.tex_count).
// W33A: CreateVB stage walk GfxStage_Parse@4E6C50 + CreateMatFromStages@4BEFC0
//   type0 blob/ApplyStages@4E5890 + GetUvTexInfo@4C68E0 uv_slot/tex_count.
// W34-01: case4 uv_slot xform @503788; soft PostLoop@4FFFE0 usage stamp.
// OOS: CreateMatFromStages non-type0; PostLoop D3D VB/IB create/bind.
void asyncload_mesh_parse_invo_chunks(AsyncLoadJob* self) {
  if (!self) return;
  if (!self->buf || self->nbytes < 12) {
    asyncload_process_flag_014(self);
    return;
  }

  const auto* base = static_cast<const uint8_t*>(self->buf);
  const size_t nbytes = static_cast<size_t>(self->nbytes);
  const auto* w32 = reinterpret_cast<const uint32_t*>(base);
  // PE: buf+0 magic, +4 ver, +8 nchunk; dir = nchunk×(off,typ) @ +12;
  // body cursor = buf+8 + 4*(2*nchunk+1) = +12 + 8*nchunk.
  const uint32_t nchunk = w32[2];
  if (nchunk == 0u || nchunk > 4096u) {
    asyncload_process_flag_014(self);
    return;
  }
  const size_t dir_bytes = static_cast<size_t>(nchunk) * 8u;
  if (12u + dir_bytes > nbytes) {
    asyncload_process_flag_014(self);
    return;
  }

  // PE ParseInvoChunks prologue: clear AABB @ +0xCC.., init flag@+0xEC=1.
  self->mesh_aabb_min[0] = self->mesh_aabb_min[1] = self->mesh_aabb_min[2] = 0.f;
  self->mesh_aabb_max[0] = self->mesh_aabb_max[1] = self->mesh_aabb_max[2] = 0.f;
  self->mesh_aabb_init = 1;
  self->mesh_vert_hi = 0;
  self->mesh_cur_mat = nullptr;
  self->mesh_cur_cvb_wrap = nullptr;

  size_t off = 12u + dir_bytes;
  void* const key = self;

  for (uint32_t ci = 0; ci < nchunk; ++ci) {
    if (off + 8u > nbytes) break;
    const uint32_t ctype =
        *reinterpret_cast<const uint32_t*>(base + off);
    const uint32_t csize =
        *reinterpret_cast<const uint32_t*>(base + off + 4);
    if (csize < 8u || off + static_cast<size_t>(csize) > nbytes) break;
    const uint8_t* payload = base + off + 8;
    const uint32_t payload_len = csize - 8u;
    const auto* pdw = reinterpret_cast<const int32_t*>(payload);

    switch (ctype) {
      case 0: {
        // PE @ 0x502E6B: malloc 24 wrap; MatVec zero @+4/+8/+C;
        // GfxEngine+0x204 device vt+0xCC CreateVB(payload, 0); PtrVec@+0x48.
        AsyncLoadCreateVbWrap* w =
            asyncload_cvb_wrap_create(payload, payload_len);
        if (!w) break;
        asyncload_cvb_wrap_vec_push(self, w);
        break;
      }
      case 1: {
        // PE @ 0x502EC9: malloc 84 → BaseInit → overlay → SetMode(0) → Push.
        AsyncLoadMatObj* m = asyncload_mat_create();
        if (!m) break;
        asyncload_mat_set_mode(m, 0);
        asyncload_mat_vec_push(self, m);
        break;
      }
      case 2: {
        // PE @ 0x502F2E: AddTex(payload[1]), AddTex(payload[2]), SetMode(1).
        if (payload_len < 12u) break;
        AsyncLoadMatObj* m = asyncload_mat_create();
        if (!m) break;
        asyncload_mat_add_tex(m, pdw[1]);
        asyncload_mat_add_tex(m, pdw[2]);
        asyncload_mat_set_mode(m, 1);
        asyncload_mat_vec_push(self, m);
        break;
      }
      case 3: {
        // PE @ 0x502FAC: count=payload[1]; ids @ payload+8; SetMode(-1).
        if (payload_len < 8u) break;
        const int32_t ntex = pdw[1];
        if (ntex < 0 || ntex > 64) break;
        if (8u + static_cast<uint32_t>(ntex) * 4u > payload_len) break;
        AsyncLoadMatObj* m = asyncload_mat_create();
        if (!m) break;
        for (int32_t ti = 0; ti < ntex; ++ti)
          asyncload_mat_add_tex(m, pdw[2 + ti]);
        asyncload_mat_set_mode(m, -1);
        asyncload_mat_vec_push(self, m);
        break;
      }
      case 4: {
        // PE UploadVbFvf@503400: *payload=vert_count, [1]=invo_flags,
        // verts@+8; src_stride=(payload_len-8)/count → pack walk.
        if (payload_len < 8u) break;
        const int32_t vert_count =
            static_cast<int32_t>(*reinterpret_cast<const uint32_t*>(payload));
        const uint32_t invo_flags =
            *reinterpret_cast<const uint32_t*>(payload + 4);
        if (vert_count <= 0 || vert_count > 500000) break;
        const uint32_t vert_bytes = payload_len - 8u;
        if (vert_bytes < static_cast<uint32_t>(vert_count)) break;
        const int32_t src_stride = static_cast<int32_t>(
            vert_bytes / static_cast<uint32_t>(vert_count));
        // PE: CreateVB vt+8 → v44 tex_count (UploadVbFvf a2=wrap).
        // Soft: cvb.tex_count; else mat AddTex; else W28A stride heuristic.
        int32_t tex_count = 0;
        const AsyncLoadCreateVb* cvb = nullptr;
        if (self->mesh_cur_cvb_wrap && self->mesh_cur_cvb_wrap->cvb &&
            self->mesh_cur_cvb_wrap->cvb->tex_count > 0) {
          cvb = self->mesh_cur_cvb_wrap->cvb;
          tex_count = cvb->tex_count;
        } else if (self->mesh_cur_mat && self->mesh_cur_mat->tex_count > 0)
          tex_count = self->mesh_cur_mat->tex_count;
        else
          tex_count = (src_stride >= 32) ? 1 : 0;
        asyncload_mesh_upload_vb_pack(self, key, vert_count, invo_flags,
                                      tex_count, payload + 8, vert_bytes, cvb);
        // W34-01: soft PostLoop usage (D3D create OOS) on current MatObj.
        if (self->mesh_cur_mat)
          (void)asyncload_mat_post_loop_soft(self->mesh_cur_mat);
        break;
      }
      case 5: {
        // PE UploadIbTris@5038E0: *payload=index_count, u16[] @ +4.
        if (payload_len < 4u) break;
        const int32_t index_count =
            static_cast<int32_t>(*reinterpret_cast<const uint32_t*>(payload));
        if (index_count <= 0 || index_count > 1500000) break;
        if (4u + static_cast<uint32_t>(index_count) * 2u > payload_len) break;
        const auto* indices =
            reinterpret_cast<const uint16_t*>(payload + 4);
        render_d3d9_mesh_ib_create(key, index_count, indices);
        break;
      }
      default:
        break;
    }
    off += csize;
  }

  asyncload_mesh_aabb_finalize(self);
  asyncload_process_flag_014(self);
}

// PE AsyncLoad_Type3_Process @ 0x501B90 (vtbl+20).
// Slice W25A: fonts path flag; optional sync .bon via FilePool_ReadEntire;
//   magic INVO; v<=3 → ParseInvoVle3 else ParseInvoChunks (W27A case4/5
//   + W28A PackVertex/AABB).
void asyncload_type3_process(AsyncLoadJob* self) {
  if (!self) return;
  // PE +0x100: async .bon in-flight → return 1 (keep flags&2, Ready requeue).
  if (self->mesh_bon_pending != 0) return;

  g_asyncload_mesh_is_fonts_path = 0;
  if (self->path) {
    // PE Util_strncpy_n → MeshPathLowerScratch@65AF2C; Util_str_tolower;
    // Util_strstr(..., "fonts") → MeshIsFontsPath@65BD3C.
    char lower[256];
    std::strncpy(lower, self->path, sizeof(lower) - 1);
    lower[sizeof(lower) - 1] = '\0';
    for (char* p = lower; *p; ++p)
      *p = static_cast<char>(std::tolower(static_cast<unsigned char>(*p)));
    if (std::strstr(lower, "fonts") != nullptr)
      g_asyncload_mesh_is_fonts_path = 1;
  }

  if (!self->buf || self->nbytes < 8) {
    self->flags = (self->flags & 0x7FFFFFF9u) | 0x80000004u;
    return;
  }

  const auto* hdr = static_cast<const uint32_t*>(self->buf);
  const uint32_t ver = hdr[1];

  // PE: ver<=3 skips .bon block → LABEL_39 magic+parse.
  if (ver > 3u) {
    const uint32_t bone_opt =
        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self->arg_a4));
    if ((self->flags & 0x20000000u) != 0) {
      // Sync Submit path: FilePool_ReadEntireFile(.bon) @ 0x54C300.
      if ((bone_opt & 1u) != 0) {
        char bon[256];
        if (asyncload_mesh_bon_path(bon, sizeof(bon), self->path)) {
          void* bon_buf = nullptr;
          const int32_t n = filepool_read_entire_file(bon, &bon_buf);
          self->mesh_bon_buf = bon_buf;
          self->mesh_bon_nbytes = n;
          // PE: n>=0 → ReadLine/sscanf bone table (+0x104/+0x108) then free.
          // Host: detect+sync read only; bone-table parse OOS — free blob.
          if (bon_buf) {
            std::free(bon_buf);
            self->mesh_bon_buf = nullptr;
            self->mesh_bon_nbytes = 0;
          }
        }
      }
    } else {
      // PE async: EnqueuePathNorm(.bon, BonOnFileCb@502000) + pending@+0x100.
      // Host: FileAsync .bon sidecar OOS (keep Process progressing).
      (void)bone_opt;
    }
  }

  // PE: *buf != INVO → Engine_Logf("Invalid mesh file! -> %s"); still parse.
  const bool invo_ok = (hdr[0] == kInvoMagic);
  (void)invo_ok;

  if (ver > 3u)
    asyncload_mesh_parse_invo_chunks(self);
  else
    asyncload_mesh_parse_invo_vle3(self);
}

void asyncload_ready_reinsert(AsyncLoadJob* self, AsyncLoadJob* prev_ok) {
  if (prev_ok && asyncload_list_contains(g_asyncload_ready, prev_ok)) {
    asyncload_list_insert_after(g_asyncload_ready, prev_ok, self);
  } else {
    asyncload_list_push_head(g_asyncload_ready, self);
  }
}

// PE AsyncLoad_Type012_Pump @ 0x507E50 (vtbl+4 shared type0/1/2).
void asyncload_type012_pump(AsyncLoadJob* self) {
  if (!self) return;
  AsyncLoadJob* prev = self->prev;
  AsyncLoadJob* prev_ok =
      (prev != nullptr && prev->prev != nullptr) ? prev : nullptr;
  asyncload_list_unlink(g_asyncload_ready, self);
  if (g_asyncload_ready.count > 0) --g_asyncload_ready.count;

  if (static_cast<int32_t>(self->flags) < 0) {
    asyncload_list_push_tail(g_asyncload_fail, self);
    return;
  }
  if ((self->flags & 2u) != 0) {
    // PE (*vtbl+16) — Type0@506650 / Type1@507110 / Type2@507840.
    if (self->type == 0)
      asyncload_type0_process(self);
    else if (self->type == 1)
      asyncload_type1_process(self);
    else if (self->type == 2)
      asyncload_type2_process(self);
    else
      asyncload_process_flag_014(self);
  }
  if ((self->flags & 2u) == 0 && (self->flags & 0x20000000u) != 0) {
    self->flags = (self->flags & 0xFFFFFFE7u) | 8u;
  }
  if ((self->flags & 2u) != 0 ||
      ((self->flags & 0x10u) != 0 && self->frame_id == g_asyncload_frame)) {
    asyncload_ready_reinsert(self, prev_ok);
  } else {
    self->flags = (self->flags & 0xFFFFFFE7u) | 8u;
    asyncload_list_push_tail(g_asyncload_work, self);
  }
}

// PE AsyncLoad_Type3_Pump @ 0x501A70.
void asyncload_type3_pump(AsyncLoadJob* self) {
  if (!self) return;
  AsyncLoadJob* prev = self->prev;
  AsyncLoadJob* prev_ok =
      (prev != nullptr && prev->prev != nullptr) ? prev : nullptr;
  asyncload_list_unlink(g_asyncload_ready, self);
  if (g_asyncload_ready.count > 0) --g_asyncload_ready.count;

  if (static_cast<int32_t>(self->flags) < 0) {
    asyncload_list_push_tail(g_asyncload_fail, self);
    self->field1c = nullptr;
    return;
  }
  if ((self->flags & 2u) != 0) {
    // PE (*vtbl+20) Type3_Process@501B90 — W25A INVO slice hosted.
    asyncload_type3_process(self);
  }
  if ((self->flags & 2u) == 0 && (self->flags & 0x20000000u) != 0) {
    self->flags = (self->flags & 0xFFFFFFE7u) | 8u;
  }
  if ((self->flags & 2u) != 0 ||
      ((self->flags & 0x10u) != 0 && self->frame_id == g_asyncload_frame)) {
    asyncload_ready_reinsert(self, prev_ok);
    self->field1c = nullptr;
  } else {
    self->flags = (self->flags & 0xFFFFFFE7u) | 8u;
    asyncload_list_push_tail(g_asyncload_work, self);
    self->field1c = nullptr;
  }
}

// PE AsyncLoad_Type4_Pump @ 0x503A00.
void asyncload_type4_pump(AsyncLoadJob* self) {
  if (!self) return;
  AsyncLoadJob* prev = self->prev;
  AsyncLoadJob* prev_ok =
      (prev != nullptr && prev->prev != nullptr) ? prev : nullptr;
  asyncload_list_unlink(g_asyncload_ready, self);
  if (g_asyncload_ready.count > 0) --g_asyncload_ready.count;

  if (static_cast<int32_t>(self->flags) < 0) {
    asyncload_list_push_tail(g_asyncload_fail, self);
    self->field1c = nullptr;
    return;
  }
  // PE always (*vtbl+20) Type4_Process@503B00 mesh — OOS; flag mask hosted.
  asyncload_process_flag_014(self);
  if ((self->flags & 2u) != 0) {
    asyncload_ready_reinsert(self, prev_ok);
    self->field1c = nullptr;
  } else {
    self->flags = (self->flags & 0xFFFFFFE7u) | 8u;
    asyncload_list_push_tail(g_asyncload_work, self);
    self->field1c = nullptr;
  }
}

// PE AsyncLoad_Type5_Pump @ 0x53BAB0.
void asyncload_type5_pump(AsyncLoadJob* self) {
  if (!self) return;
  // PE Engine_PerfTimerBegin/End(11) — host skip; field1c left 0.
  if (static_cast<int32_t>(self->flags) >= 0) {
    // PE (*vtbl+12) Type5_Process@53BB90 — OOS; flag mask hosted.
    asyncload_process_flag_00c(self);
  }
  asyncload_list_unlink(g_asyncload_ready, self);
  if (g_asyncload_ready.count > 0) --g_asyncload_ready.count;
  if (static_cast<int32_t>(self->flags) >= 0 && (self->flags & 2u) != 0) {
    asyncload_list_push_tail(g_asyncload_ready, self);  // PE ReadyTail push
  }
  self->field1c = nullptr;
}

// PE FileAsync_EnqueuePathNorm @ 0x54D290: Path_CopyCstr + '/'→'\\' +
// EnqueuePath.
int32_t fileasync_enqueue_path_norm(const char* path, void* buf, void* cb,
                                    void* userdata, int32_t prio) {
  char norm[256];
  norm[0] = '\0';
  if (path) {
    std::strncpy(norm, path, sizeof(norm) - 1);
    norm[sizeof(norm) - 1] = '\0';
    for (char* p = norm; *p; ++p) {
      if (*p == '/') *p = '\\';
    }
  }
  return fileasync_enqueue_path(norm, buf, cb, userdata, prio);
}

// PE AsyncLoad_OnFileCb @ 0x505C00 — FileAsync worker cdecl cb.
void asyncload_on_file_cb(void* userdata, int32_t status, void* buf,
                          uint32_t nbytes) {
  auto* job = static_cast<AsyncLoadJob*>(userdata);
  // PE: Engine_OwnedSemaphoreWait(AsyncLoad_OwnedSem) — host no-op.
  if (job) {
    --g_asyncload_inflight.count;  // PE InFlightCount @ 0x65C204
    asyncload_list_unlink(g_asyncload_inflight, job);
    asyncload_list_push_tail(g_asyncload_ready, job);
    job->flags = (job->flags & 0xFFFFFFFCu) | 2u;  // PE: &~3 | 2
    job->fa_status = -1;
    if (status >= 0 && buf != nullptr && static_cast<int32_t>(nbytes) > 0) {
      job->buf = buf;
      job->nbytes = static_cast<int32_t>(nbytes);
    } else {
      // PE fail: flags = flags & 0x7FFFFFFA | 0x80000004
      job->flags = (job->flags & 0x7FFFFFFAu) | 0x80000004u;
      job->fa_status = -1;
    }
  }
  // PE: Engine_OwnedSemaphoreRelease — host no-op.
}

// PE AsyncLoad_Submit @ 0x505960 — async hop only (a2!=0).
// Types 0..5 PE malloc sizes/ctors skipped; host JobBase-sized + type pump vtbl.
// Sync path (a2==0 / sub_54C300) returns nullptr (OOS).
// PE AsyncLoad_PumpOne @ 0x505DB0 — cursor + vtbl+4 type pumps.
// ---------------------------------------------------------------------------
// EngineState GII_CONTROL sim dllist (PE g_EngineState @ 0x636338, this+8).
//
// Layout — Engine_SimObjectList_ctor @ 0x429350 (7 dwords):
//   [0] outer vtbl  [1] tail-sentinel vtbl  [2] head_ptr  [3] 0
//   [4..6] head sentinel (vtbl / next / prev); [6] also = list.tail
// Empty — Engine_SimObjectListEmpty @ 0x429390: *(*(this+2)+4)==0.
//
// Nodes enter via Engine_registerGameInstanceCallback @ 0x427370 mode 8
// (GII_CONTROL): malloc(0x1C), Engine_SimCallbackNode_ctor @ 0x429130,
// ResHandle slot @ node+12 (clear @ 0x428FC0 + ResHandle_Rebind),
// tail-insert eng+0x10/+0x18, ++eng+0xC4. First insert overwrites eng[2]
// (via *(tail_sent+4)=node) so head_ptr becomes the first real node;
// node.next = &eng[4] (head sentinel). DllistNext @ 0x40CFC0 returns 0
// when next is the sentinel (sentinel.next==0).
//
// SimulateFrame walk @ 0x4284AF..0x4284F7: for each node, if *(node+20)
// TickSimObject(dt, node+12) @ 0x4291E0 else vtbl dtor + --count.
//
// Producer: GameType_registerCallback mode8 → engine_gii_control_register
// (System.h). Register: ResHandle_Rebind(node+12, *(handle+12)) into
// owner+0x48; unregister: ResHandle_Unlink(owner+0x44); dead walk:
// Engine_SimCallbackNode_dtor @ 0x45F8B0. TickSimObject: stack RH Rebind +
// flag 0x100 clear + ResHandle_getPayload Soft + vtbl control /
// CallNamedMethod ("control") — no bytecode VM.
// ---------------------------------------------------------------------------
struct EngSimNode {
  EngSimNode* vtbl_or_self;  // [0] PE vtbl; host unused for tick
  EngSimNode* next;          // [1] +4
  EngSimNode* prev;          // [2] +8
  void* rh0;                 // [3] ResHandle slot +0x0C  [0]
  void* rh1;                 // [4]                      [1]
  void* alive;               // [5] +0x14 — PE SimulateFrame *(node+20) / RH[2]
  void* payload;             // [6] +0x18 — TickSimObject *(a2+12) / RH[3]
  // Host-only (PE malloc 0x1C stops at payload). Optional GameType* for
  // CallNamedMethod when PE child+0x50 is not seeded (register 2nd arg).
  InvObject* script;
};
static_assert(offsetof(EngSimNode, payload) + sizeof(void*) == 0x1C,
              "PE callback node head 0x1C");

// eng[0..6] CONTROL list object; count stands in for eng+0xC4.
static std::uintptr_t g_eng_ctrl[7] = {};
static int32_t g_eng_ctrl_count = 0;
static bool g_eng_ctrl_ready = false;

static void eng_ctrl_list_ensure() {
  if (g_eng_ctrl_ready) return;
  // Engine_SimObjectList_ctor @ 0x429350 (pointer-sized host Win32).
  g_eng_ctrl[1] = 0;  // tail-sentinel "vtbl" unused
  g_eng_ctrl[3] = 0;
  g_eng_ctrl[4] = 0;  // head-sentinel vtbl unused
  g_eng_ctrl[5] = 0;  // head-sentinel next
  g_eng_ctrl[0] = 0;
  g_eng_ctrl[2] = reinterpret_cast<std::uintptr_t>(&g_eng_ctrl[4]);
  g_eng_ctrl[6] = reinterpret_cast<std::uintptr_t>(&g_eng_ctrl[1]);
  g_eng_ctrl_count = 0;
  g_eng_ctrl_ready = true;
}

static bool eng_sim_list_empty() {
  // Engine_SimObjectListEmpty @ 0x429390
  eng_ctrl_list_ensure();
  auto* head = reinterpret_cast<EngSimNode*>(g_eng_ctrl[2]);
  if (!head) return true;
  return head->next == nullptr;
}

static EngSimNode* eng_dllist_next(EngSimNode* node) {
  // Engine_DllistNext @ 0x40CFC0
  if (!node) return nullptr;
  EngSimNode* n = node->next;
  if (!n || n->next == nullptr) return nullptr;
  return n;
}

// PE ResHandle_Rebind @ 0x429060 (this = 4 dwords, a2 = owner|0).
// Null-safe: rh==nullptr no-op; a2==nullptr clears slot (PE else branch).
static void resh_rebind(void** rh, void* owner) {
  if (!rh) return;
  void* old = rh[3];
  if (old == owner) return;
  if (old) {
    if (rh[0]) {
      *reinterpret_cast<void**>(reinterpret_cast<char*>(rh[0]) + 4) = rh[1];
    } else {
      *reinterpret_cast<void**>(reinterpret_cast<char*>(old) + 0x48) = rh[1];
    }
    if (rh[1]) *reinterpret_cast<void**>(rh[1]) = rh[0];
    rh[3] = nullptr;
    rh[2] = nullptr;
    rh[0] = nullptr;
    rh[1] = nullptr;
  }
  if (owner) {
    void* cur =
        *reinterpret_cast<void**>(reinterpret_cast<char*>(owner) + 0x48);
    if (cur) *reinterpret_cast<void**>(cur) = rh;
    rh[0] = nullptr;
    rh[1] = cur;
    *reinterpret_cast<void**>(reinterpret_cast<char*>(owner) + 0x48) = rh;
    rh[3] = owner;
    rh[2] = *reinterpret_cast<void**>(reinterpret_cast<char*>(owner) + 0x50);
  } else {
    rh[3] = nullptr;
    rh[2] = nullptr;
    rh[0] = nullptr;
    rh[1] = nullptr;
  }
}

// PE TickSimObject epilogue @ 0x4292FF..0x429325 — unlink stack RH from
// owner+0x48 list (inline; not Rebind(null)). Does NOT clear rh[0..3].
static void resh_unlink_temp(void** rh) {
  if (!rh || !rh[3]) return;
  void* owner = rh[3];
  if (rh[0]) {
    *reinterpret_cast<void**>(reinterpret_cast<char*>(rh[0]) + 4) = rh[1];
  } else {
    *reinterpret_cast<void**>(reinterpret_cast<char*>(owner) + 0x48) = rh[1];
  }
  if (rh[1]) *reinterpret_cast<void**>(rh[1]) = rh[0];
}

// PE ResHandle_Unlink @ 0x429010 (thiscall this=owner+0x44, a2=RH embed,
// retn 4). Splice from owner+0x48 list then zero all 4 RH dwords.
// unregister mode8 @ 0x4275FA: Unlink(payload+0x44, node+12).
static void resh_unlink(void* owner_plus_0x44, void** rh) {
  if (!rh) return;
  if (rh[0]) {
    *reinterpret_cast<void**>(reinterpret_cast<char*>(rh[0]) + 4) = rh[1];
  } else if (owner_plus_0x44) {
    *reinterpret_cast<void**>(reinterpret_cast<char*>(owner_plus_0x44) + 4) =
        rh[1];
  }
  if (rh[1]) *reinterpret_cast<void**>(rh[1]) = rh[0];
  rh[3] = nullptr;
  rh[2] = nullptr;
  rh[0] = nullptr;
  rh[1] = nullptr;
}

// PE ResHandle_orPayloadFlags @ 0x48D1F0: *(owner+0x54) |= flags; null RH→0.
static int32_t resh_or_payload_flags(void** rh, int32_t flags) {
  if (!rh || !rh[3]) return 0;
  auto* p = reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(rh[3]) + 0x54);
  const uint32_t v = static_cast<uint32_t>(flags) | *p;
  *p = v;
  return static_cast<int32_t>(v);
}

// PE ResHandle_andNotPayloadFlags @ 0x48D210: *(owner+0x54) &= ~flags.
static int32_t resh_and_not_payload_flags(void** rh, int32_t flags) {
  if (!rh || !rh[3]) return 0;
  auto* p = reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(rh[3]) + 0x54);
  const uint32_t v = ~static_cast<uint32_t>(flags) & *p;
  *p = v;
  return static_cast<int32_t>(v);
}

// PE ResHandle_PrepareLod @ 0x5447D0 — host null-safe readiness gate only.
// PE returns 0 when (*(obj+0x64)&1) else 0x80000001 (−2147483647);
// LOD apply callees (sub_537240 / ResNode_relinkLodSlot / Touch) OOS.
// TickSim sites: owner flags 0xA0000000 (bit31|bit29 — RelinkLod skipped),
// ctrl flags 0x80000000 (bit31); a3=a4=0. Soft: bit0 at +0x64 only.
static int32_t resh_prepare_lod_nullsafe(void* obj, int32_t /*flags*/) {
  if (!obj) return static_cast<int32_t>(0x80000001u);
  const uint32_t st =
      *reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(obj) + 0x64);
  if ((st & 1u) == 0u) return static_cast<int32_t>(0x80000001u);
  return 0;
}

// PE TickSimObject @ 0x429250 / 0x429299 (and CallNamedMethod_va @ 0x425639):
//   test eax, 80000000h / jnz fail — NOT signed >=0 (same for 0 / 0x80000001).
static bool resh_prepare_lod_ok(int32_t ret) {
  return (static_cast<uint32_t>(ret) & 0x80000000u) == 0u;
}

// PE float 1.0f pushed as imm 0x3F800000 on vtbl+0xC (int_convert).
constexpr int32_t kVtblPri1f = 0x3F800000;

static void** inv_obj_vtbl(void* obj) {
  if (!obj) return nullptr;
  return *reinterpret_cast<void***>(obj);
}

// PE thiscall vtbl+off(obj, 0x3F800000) — getPayload success @ 0x419894.
static void* inv_vtbl_call_pri1f(void* obj, std::size_t slot_off) {
  void** vtbl = inv_obj_vtbl(obj);
  if (!vtbl) return nullptr;
  using Fn = void*(__thiscall*)(void*, int32_t);
  auto* fn = reinterpret_cast<Fn>(vtbl[slot_off / sizeof(void*)]);
  if (!fn) return nullptr;
  return fn(obj, kVtblPri1f);
}

// PE dword field at obj+idx*4 (TickSimObject v4[19]/v4[20]/v4[17]).
static std::uintptr_t inv_obj_dword(void* obj, std::size_t dword_idx) {
  if (!obj) return 0;
  return reinterpret_cast<std::uintptr_t*>(obj)[dword_idx];
}

// PE ResHandle_getPayload @ 0x00419860 size 0x48 (thiscall, retn 10h):
//   cmp [this+0x4C],1 / jz; else (*(vtbl+0x14))(this, a3);
//   PrepareLod(this, tag, a4, a5); test eax,80000000h / jnz → 0;
//   else return (*(vtbl+0xC))(this, 0x3F800000).
// Soft: PrepareLod +0x64 bit0 only (a4/a5 Relink/Touch/scale OOS).
// Contrast Resources res_handle_get_payload: mid-layout stand-in, not this CF.
static void* resh_get_payload(void* obj, int32_t tag, int32_t a3, int32_t /*a4*/,
                              int32_t /*a5*/) {
  if (!obj) return nullptr;
  // PE @ 0x419863: cmp dword ptr [esi+4Ch], 1
  if (inv_obj_dword(obj, 19) != 1u) {
    // PE @ 0x419869..0x419872: push a3; call [eax+14h]
    void** vtbl = inv_obj_vtbl(obj);
    if (vtbl) {
      using Fn = void(__thiscall*)(void*, int32_t);
      auto* fn = reinterpret_cast<Fn>(vtbl[0x14 / sizeof(void*)]);
      if (fn) fn(obj, a3);
    }
  }
  // PE @ 0x419886..0x419890: call PrepareLod; test eax,80000000h / jnz→0
  if (!resh_prepare_lod_ok(resh_prepare_lod_nullsafe(obj, tag)))
    return nullptr;
  // PE @ 0x419894..0x41989b: push 3F800000h; call [edx+0Ch]
  return inv_vtbl_call_pri1f(obj, 0xC);
}

static InvObject* inv_as_script(void* p) {
  auto* o = reinterpret_cast<InvObject*>(p);
  if (!o) return nullptr;
  const char* cn = tree_host_class(o);
  if (!cn || !cn[0]) return nullptr;
  return o;
}

// PE Engine_CallNamedMethod_packArgs @ 0x425240 type tags (int_convert):
//   2 = int (box dword_62E00C), 3 = float (box dword_62DEEC from qword),
//   4 = object, sentinel 79 = 0x4F 'O'. Ends in VMThread_pushCallFrame @
//   0x41F9F0.
constexpr int kPackArgFloat = 3;
constexpr int kPackArgSentinelO = 79;  // 'O'

// PE CallNamedMethod_va @ 0x4255F8..0x42563C gate on *(a1+0x44):
// ResHandle_getPayload(rh, 0x80000000, 1.0f, 0, 0) ≠ 0.
// Soft: no bytecode VM beyond existing TREE invoke path.
static bool engine_call_named_method_va_rh_gate(void* child) {
  if (!child) return false;
  void* rh = reinterpret_cast<void*>(inv_obj_dword(child, 17));  // +0x44
  if (!rh) return false;
  return resh_get_payload(rh, static_cast<int32_t>(0x80000000u), kVtblPri1f, 0,
                          0) != nullptr;
}

// PE Engine_getResNameCstr @ 0x48ADB0: thiscall ecx=GI+0x38 (embedded RH);
// return *( *(this+0xC) + 0x60 ). this+0xC ≡ GI+0x44 (same as RH gate ptr).
static const char* engine_get_res_name_cstr(void* child) {
  if (!child) return nullptr;
  void* node = reinterpret_cast<void*>(inv_obj_dword(child, 17));  // +0x44
  if (!node) return nullptr;
  return reinterpret_cast<const char*>(inv_obj_dword(node, 24));  // +0x60
}

// PE Engine_CallNamedMethod @ 0x425A90 → Engine_CallNamedMethod_va @ 0x4255E0
// → Engine_CallNamedMethod_packArgs @ 0x425240 → Object_callMethod @ 0x408A30
//   → Thread_callMethod @ 0x4207C0 (vmthread, *(script+0xC)=clazz, script, name)
//   → VMThread_invokeMethod @ 0x41FBC0.
// TickSimObject site @ 0x4292E3: (child, "control", sync=0, tag=3, dt, 79).
// CallNamedMethod_va PE slice for control:
//   require *(a1+0x50) script; RH gate on +0x44; build name
//   "THRD-RUNVMI "+Engine_getResNameCstr(a1+0x38)+"."+"control" (aThrdRunvmiDot);
//   malloc(56) + VMThread_init @ 0x41F340 (JVM, prio=10, sync=0, name);
//   packArgs(sync, a1+0x38, thread, 3, dt-as-qword, 79);
//   Object_callMethod(*(a1+0x50), thread, "control");
//   if invoke ret==0 → VMThread_run(0.0) @ 0x420FF0;
//   if sync!=0 || ret!=0 → popOperand + requestStop + vtbl dtor;
//   sync==0 && ret==0 → leave green-thread for Jvm_PumpFrame drain.
// W13C: getResNameCstr name; sleep +0x30; DONE→STOP dtor; E4≡g_JVM gate.
// W14D: opcode switch inventory only — residual @ 0x4210D4 size 0x2979;
//   host still TREE drain ≡ op16/42/43; no invented bytecode VM.
// Gap residual: full interpreter (see jvm.cpp vmthread_run W14D map).
static void engine_call_named_method_control(void* child, float dt,
                                             InvObject* script_fallback) {
  InvObject* script = nullptr;
  if (child) {
    // PE @ 0x4255EE: require *(a1+0x50) != 0 (dword[20]).
    script = inv_as_script(reinterpret_cast<void*>(inv_obj_dword(child, 20)));
    if (script) {
      // PE @ 0x4255F8..0x42563C: +0x44 ResHandle_getPayload gate.
      if (!engine_call_named_method_va_rh_gate(child)) return;
    }
  }
  // Host extension when child+0x50 unset (register script) — not a PE path.
  if (!script) script = inv_as_script(script_fallback);
  if (!script) return;
  Jvm* j = jvm_active();
  const char* cn = tree_host_class(script);
  if (!j || !cn || !cn[0]) return;
  // PE site @ 0x4292E3: CallNamedMethod(child,"control",0,tag3,dt,79).
  static_assert(kPackArgFloat == 3 && kPackArgSentinelO == 79,
                "PE packArgs control site tags");
  static_assert(kVmThreadBlobSize == 56, "PE malloc(56) VMThread");
  static_assert(kCallFrameBlobSize == 64, "PE CallFrame pool slot");
  static_assert(kVmFrameListBlobSize == 28, "PE FrameList malloc(28)");
  // PE @ 0x425665..0x4256BB: "THRD-RUNVMI " + getResNameCstr(a1+0x38) + "."
  // + method. aThrdRunvmiDot @ 0x60C978.
  const char* res_name = engine_get_res_name_cstr(child);
  char thr_name[64];
  std::snprintf(thr_name, sizeof(thr_name), "THRD-RUNVMI %s.control",
                res_name ? res_name : "");
  // PE @ 0x4256CC..0x4256E4: malloc(56) + VMThread_init(JVM, 10, sync=0, name).
  constexpr int32_t kSync = 0;  // TickSimObject control site
  VmThread* thr = vmthread_init(j, /*priority=*/10, /*sync_flags=*/kSync,
                                thr_name);
  if (!thr) return;
  // PE packArgs tag3 + sentinel 79 → pushCallFrame @ 0x41F9F0.
  vmthread_pack_arg_float(thr, dt);
  vmthread_push_call_frame(thr);
  // PE Object_callMethod @ 0x408A30 → Thread_callMethod @ 0x4207C0 → invokeMethod.
  const int inv = vmthread_thread_call_method(thr, script, cn, "control");
  // PE @ 0x42570F: if invoke ret==0 (Java queued) → VMThread_run(0.0).
  if (inv == 0) vmthread_run(thr, /*budget_ms=*/0.f);
  // PE @ 0x42571B: if sync!=0 || ret!=0 → requestStop + dtor.
  // sync==0 && ret==0: leave DONE'd green-thread for PumpFrame STOP→dtor.
  if (kSync != 0 || inv != 0) {
    vmthread_request_stop(thr);
    vmthread_destroy(thr);
  }
}

// PE native control tick @ 0x4292C4: (*(ctrl.vtbl+0x2C))(ctrl, child[19], dt).
static void engine_vtbl_control_tick(void* ctrl, std::uintptr_t child_field19,
                                     float dt) {
  void** vtbl = inv_obj_vtbl(ctrl);
  if (!vtbl) return;
  using Fn = void(__thiscall*)(void*, std::uintptr_t, float);
  auto* fn = reinterpret_cast<Fn>(vtbl[0x2C / sizeof(void*)]);
  if (!fn) return;
  fn(ctrl, child_field19, dt);
}

// PE Engine_TickSimObject @ 0x4291E0 (stdcall; SimulateFrame also loads
// ecx=EngineState but body unused). a2 = node+12 ResHandle slot.
// Stock: stack RH Rebind → save/clear flag 0x100 →
//   ResHandle_getPayload(v3, 0xA0000000, 1.0f, 0, 0) →
//   (vtbl+0x2C | CallNamedMethod "control") → restore flags + unlink temp RH.
//   Child null @ 0x42926B → epilogue.
// Host: Rebind/flags + Soft getPayload + PE vtbl slices when present;
//   CallNamedMethod → jvm invoke control(F)V. No bytecode VM invented.
static void engine_tick_sim_object(float dt, EngSimNode* node) {
  if (!node) return;
  // PE a2 = node+12 — RH embed [rh0,rh1,alive,payload].
  void** a2 = reinterpret_cast<void**>(&node->rh0);
  // PE @ 0x4291EF: v10 = *(a2+12) [= payload / RH[3]]
  void* owner = a2[3];
  // PE @ 0x4291F4..0x429204: stack ResHandle + ResHandle_Rebind(&tmp, v10)
  void* tmp_rh[4] = {};
  resh_rebind(tmp_rh, owner);
  // PE @ 0x42921C..0x429220: orFlags(0) save, andNot(0x100) clear bit8
  const int32_t saved_flags = resh_or_payload_flags(tmp_rh, 0);
  resh_and_not_payload_flags(tmp_rh, 256);
  // PE @ 0x429225: v3 = *(a2+12) — re-read after Rebind (same as owner)
  void* v3 = a2[3];
  if (v3) {
    // PE @ 0x429230..0x429264 ≡ ResHandle_getPayload @ 0x00419860
    //   (tag 0xA0000000, a3=1.0f, a4=a5=0) — Soft resh_get_payload.
    void* v4 = resh_get_payload(v3, static_cast<int32_t>(0xA0000000u),
                                kVtblPri1f, 0, 0);
    if (!v4) {
      // PE @ 0x42926B: jz epilogue — no CallNamedMethod on null child.
      // Host extension only when owner has no PE vtbl at all (TREE GI).
      if (!inv_obj_vtbl(v3))
        engine_call_named_method_control(v3, dt, node->script);
    } else if (inv_obj_dword(v4, 19) != 0) {
      // PE @ 0x42926D..0x4292C4: native controller path
      //   v6 = [edi+44h]; getPayload(v6, 0x80000000, 1.0f, 0, 0);
      //   ctrl = *(eax+0xC); (*(ctrl.vtbl+0x2C))(ctrl, [edi+4Ch], dt)
      void* v6 = reinterpret_cast<void*>(inv_obj_dword(v4, 17));
      if (v6) {
        void* v7 = resh_get_payload(v6, static_cast<int32_t>(0x80000000u),
                                    kVtblPri1f, 0, 0);
        if (v7) {
          void* ctrl = *reinterpret_cast<void**>(
              reinterpret_cast<char*>(v7) + 12);
          if (ctrl)
            engine_vtbl_control_tick(ctrl, inv_obj_dword(v4, 19), dt);
        }
      }
    } else if (inv_obj_dword(v4, 20) != 0) {
      // PE @ 0x4292C9..0x4292E3: [edi+50h]!=0 → CallNamedMethod
      //   (v4, "control", sync=0, tag=3, dt, 79='O')
      engine_call_named_method_control(v4, dt, node->script);
    }
  }
  // PE @ 0x4292F4: orPayloadFlags(tmp, saved) restores cleared bits
  resh_or_payload_flags(tmp_rh, saved_flags);
  // PE @ 0x4292FF: unlink temp RH if owner was bound (tmp_rh[3]/var_4)
  resh_unlink_temp(tmp_rh);
}

// PE Engine_registerGameInstanceCallback @ 0x427370 mode 8 only.
// PE Engine_unregisterGameInstanceCallback @ 0x4274E0 mode 8 only.
// Soft PE Engine_SimCallbackNode_dtor @ 0x45F8B0 (off_5F09B4[0], a2=1).
// RH unlink from owner+0x48 → dllist splice (both links) → Engine_free.
// Overlapping sentinels restore eng[2]/eng[6] via splice — no special head/
// tail rewrite (PE does not touch counts here; caller --count).
static void eng_sim_node_dtor_free(EngSimNode* node) {
  if (!node) return;
  void** rh = reinterpret_cast<void**>(&node->rh0);
  void* payload = rh[3];
  if (payload) {
    if (rh[0]) {
      *reinterpret_cast<void**>(reinterpret_cast<char*>(rh[0]) + 4) = rh[1];
    } else {
      *reinterpret_cast<void**>(reinterpret_cast<char*>(payload) + 0x48) =
          rh[1];
    }
    if (rh[1]) *reinterpret_cast<void**>(rh[1]) = rh[0];
    rh[3] = nullptr;
    rh[2] = nullptr;
    rh[0] = nullptr;
    rh[1] = nullptr;
  } else {
    rh[2] = nullptr;
  }
  EngSimNode* prev = node->prev;
  EngSimNode* next = node->next;
  // PE @ 0x45F8FD..0x45F90E: splice only when both prev and next non-null.
  if (prev && next) {
    next->prev = prev;
    prev->next = next;
  }
  node->next = nullptr;
  node->prev = nullptr;
  std::free(node);
}

static void engine_tick_sim_objects(float dt) {
  // PE @ 0x004284A1..0x004284FB (SimulateFrame sim-list slice).
  // Caller passes Engine_simDtAccum (not frame_dt) when accum > 1e-4.
  eng_ctrl_list_ensure();
  if (eng_sim_list_empty()) return;
  EngSimNode* cur = reinterpret_cast<EngSimNode*>(g_eng_ctrl[2]);
  if (!cur) return;
  do {
    EngSimNode* nxt = eng_dllist_next(cur);
    if (cur->alive != nullptr) {
      // PE @ 0x4284DE..0x4284EE: TickSimObject(dt, node+12).
      engine_tick_sim_object(dt, cur);
    } else {
      // PE @ 0x4284CA..0x4284D4: (**vtbl)(node,1) + --*(eng+0xC4).
      eng_sim_node_dtor_free(cur);
      if (g_eng_ctrl_count > 0) --g_eng_ctrl_count;
    }
    cur = nxt;
  } while (cur != nullptr);
}

// Soft PE dllist7 (Engine_SimObjectList_ctor @ 0x429350 layout) — empty
// sentinels for Drain/Timers residual. Producers OOS on host.
static void eng_dllist7_ensure(std::uintptr_t* L, bool& ready) {
  if (ready) return;
  L[1] = 0;
  L[3] = 0;
  L[4] = 0;
  L[5] = 0;
  L[0] = 0;
  L[2] = reinterpret_cast<std::uintptr_t>(&L[4]);
  L[6] = reinterpret_cast<std::uintptr_t>(&L[1]);
  ready = true;
}

static bool eng_dllist7_head_empty(std::uintptr_t* L) {
  // PE empty: !head || *(head+4)==0 (Drain @ 0x42780D / TickTimers @ 0x427177).
  auto* head = reinterpret_cast<EngSimNode*>(L[2]);
  return !head || head->next == nullptr;
}

// Soft PE EngineState timer dllist7 — object base EngState+0x8C:
//   head_ptr @ +0x94 (EngState_timerHeadPtr), head sentinel @ +0x9C
//   (EngState_timerHeadSent), tail_ptr @ +0xA4 (EngState_timerTailPtr),
//   count @ +0xD8. addTimer @ 0x48B750 malloc(0x30) tail-inserts into
//   HeadSent/TailPtr. Host GameType.addTimer uses Java stand-in (pollTimers)
//   — native list stays empty Soft.
static std::uintptr_t g_eng_timers[7] = {};
static int32_t g_eng_timer_count = 0;
static bool g_eng_timers_ready = false;

// Soft PE physWorld stand-in (Engine_physWorld @ 0x617650 → BSS):
//   +0 clock (Physics_Step @ 0x4A548E += v22; TickTimers fcomp)
//   +4 residue (Physics_Step gate @ 0x4A5197..0x4A51FD)
//   +8 max substep (SimulateFrame @ 0x42851A) — Soft kPhysMaxSubstepSoft
static float g_phys_world_clock = 0.f;
static float g_phys_world_residue = 0.f;

// PE Engine_addTimer node @ 0x48B750 malloc(0x30) — SimCallbackNode head +
// fire_at/+0x1C period/+0x20 type/+0x24 timerid/+0x28 msg/+0x2C.
struct EngTimerNode {
  EngTimerNode* vtbl_or_self;  // [0]
  EngTimerNode* next;          // [1] +4
  EngTimerNode* prev;          // [2] +8
  void* rh0;                   // [3] +0x0C
  void* rh1;                   // [4]
  void* alive;                 // [5] +0x14
  void* payload;               // [6] +0x18
  float fire_at;               // [7] +0x1C
  float period;                // [8] +0x20
  int32_t type;                // [9] +0x24
  int32_t timerid;             // [10] +0x28
  char* msg;                   // [11] +0x2C
};
static_assert(sizeof(EngTimerNode) == 0x30, "PE addTimer malloc(0x30)");

static EngTimerNode* eng_timer_dllist_next(EngTimerNode* node) {
  // PE TickTimers inline @ 0x42717E..0x427190 (same as DllistNext @ 0x40CFC0).
  if (!node) return nullptr;
  EngTimerNode* n = node->next;
  if (!n || n->next == nullptr) return nullptr;
  return n;
}

// Soft PE SimCallbackNode_dtor @ 0x45F8B0 for timer nodes (+ free msg @ +0x2C).
static void eng_timer_node_dtor_free(EngTimerNode* node) {
  if (!node) return;
  void** rh = reinterpret_cast<void**>(&node->rh0);
  void* payload = rh[3];
  if (payload) {
    if (rh[0]) {
      *reinterpret_cast<void**>(reinterpret_cast<char*>(rh[0]) + 4) = rh[1];
    } else {
      *reinterpret_cast<void**>(reinterpret_cast<char*>(payload) + 0x48) =
          rh[1];
    }
    if (rh[1]) *reinterpret_cast<void**>(rh[1]) = rh[0];
    rh[3] = nullptr;
    rh[2] = nullptr;
    rh[0] = nullptr;
    rh[1] = nullptr;
  } else {
    rh[2] = nullptr;
  }
  EngTimerNode* prev = node->prev;
  EngTimerNode* next = node->next;
  if (prev && next) {
    next->prev = prev;
    prev->next = next;
  }
  node->next = nullptr;
  node->prev = nullptr;
  if (node->msg) {
    std::free(node->msg);
    node->msg = nullptr;
  }
  std::free(node);
}

// Soft PE Engine_queueEvent_dispatch @ 0x4265C0 (size 0x238) — timer site
// args: (node+12, 0, type&0x0FFFFFFF, timerid, msg).
// Stock: stack RH Rebind(*(a1+0xC)) → PrepareLod owner → mask test GI+0x70 →
// script/vtbl+0x38 → ALWAYS watcher walk GI+0x5C (even on mask miss).
// Soft: RH Rebind entry only. Watcher fan-out / DrainEvent dllist producers
// (EngState+0x5C/+0x78/+0xB0) OOS — do NOT invent enqueue into Soft Drain
// sentinels. Script EVENT_TIME path remains java_lang_GameType_pollTimers.
static void engine_queue_event_dispatch_soft(void** rh_embed, int32_t /*related*/,
                                             int32_t /*type_masked*/,
                                             int32_t /*timerid*/,
                                             char* /*msg*/) {
  if (!rh_embed) return;
  // PE @ 0x4265C8..0x4265F0: temp RH Rebind(*(a1+0xC)) [=payload owner].
  void* owner = rh_embed[3];
  void* tmp_rh[4] = {};
  resh_rebind(tmp_rh, owner);
  // Residual OOS: PrepareLod / mask / dispatchScriptEvent / watcher walk /
  // DrainEvent dllist push. Unlink temp to avoid dangling owner+0x48 link.
  resh_unlink_temp(tmp_rh);
}

// Soft PE Engine_TickTimers @ 0x427160 — walk EngState+0x94 vs soft
// *(float*)physWorld clock; due → queueEvent_dispatch Soft; type&0x80000000
// oneshot dtor else fire_at += period. Native producers OOS (list empty);
// host always runs pollTimers (wall time_current) as script-timer stand-in.
static void engine_tick_timers() {
  eng_dllist7_ensure(g_eng_timers, g_eng_timers_ready);
  EngTimerNode* cur = reinterpret_cast<EngTimerNode*>(g_eng_timers[2]);
  if (cur && cur->next != nullptr) {
    do {
      EngTimerNode* nxt = eng_timer_dllist_next(cur);
      if (cur->alive == nullptr) {
        // PE @ 0x427199..0x4271A5: vtbl dtor + --count @ +0xD8.
        eng_timer_node_dtor_free(cur);
        if (g_eng_timer_count > 0) --g_eng_timer_count;
      } else {
        // PE @ 0x4271AD..0x4271BD: due iff fire_at <= *physWorld (fcomp; ah&41h).
        if (cur->fire_at <= g_phys_world_clock) {
          void** rh = reinterpret_cast<void**>(&cur->rh0);
          engine_queue_event_dispatch_soft(
              rh, /*related=*/0,
              cur->type & 0x0FFFFFFF, cur->timerid, cur->msg);
          // PE @ 0x4271E9: test type,80000000h → oneshot dtor.
          if ((static_cast<uint32_t>(cur->type) & 0x80000000u) != 0u) {
            eng_timer_node_dtor_free(cur);
            if (g_eng_timer_count > 0) --g_eng_timer_count;
          } else {
            // PE @ 0x4271F2..0x4271F8: fire_at += period.
            cur->fire_at += cur->period;
          }
        }
      }
      cur = nxt;
    } while (cur != nullptr);
  }
  java_lang_GameType_pollTimers();
}

// Soft PE Drain queues: EngState+0x5C / +0x78 / +0xB0 head_ptrs (this[23]/
// [30]/[44]); counts +0xD0/+0xD4 (this[52]/[53]); aux gate +0xE4 (this[57]).
static std::uintptr_t g_eng_q_a[7] = {};  // +0x5C family
static std::uintptr_t g_eng_q_b[7] = {};  // +0x78 family
static std::uintptr_t g_eng_q_c[7] = {};  // +0xB0 family
static int32_t g_eng_q_a_count = 0;
static int32_t g_eng_q_b_count = 0;
static void* g_eng_drain_aux = nullptr;  // this[57] / +0xE4
static bool g_eng_q_ready = false;

static void eng_drain_lists_ensure() {
  if (g_eng_q_ready) return;
  bool a = false, b = false, c = false;
  eng_dllist7_ensure(g_eng_q_a, a);
  eng_dllist7_ensure(g_eng_q_b, b);
  eng_dllist7_ensure(g_eng_q_c, c);
  g_eng_q_a_count = 0;
  g_eng_q_b_count = 0;
  g_eng_drain_aux = nullptr;
  g_eng_q_ready = true;
}

// PE Engine_DrainEventQueues @ 0x004277F0 — outer do-while until i==0 (no
// work any queue). Soft: empty sentinels → one empty pass. PATH-TO-WORLD:
// DrainEvent dllist producers (queueEvent watcher → +0x5C/+0x78/+0xB0) OOS —
// do NOT invent node walks / dispatch enqueue when Soft lists stay empty.
// DrainAux when this+0xE4≠0 OOS (aux≡null).
static void engine_drain_event_queues() {
  eng_drain_lists_ensure();
  int i;
  do {
    i = 0;
    // Queue A @ this[23]/+0x5C: PE for-loop pops while head non-empty.
    // Soft empty → break (producers OOS — no invented pop/dispatch).
    if (!eng_dllist7_head_empty(g_eng_q_a)) {
      (void)g_eng_q_a_count;
      break;
    }
    // Queue B @ this[30]/+0x78: similar + inline RH unlink + dual vtbl dtor.
    if (!eng_dllist7_head_empty(g_eng_q_b)) {
      (void)g_eng_q_b_count;
      break;
    }
    // Queue C @ this[44]/+0xB0: simple dllist pop + vtbl dtor.
    if (!eng_dllist7_head_empty(g_eng_q_c)) {
      break;
    }
  } while (i != 0);
  // PE @ 0x427970: if this[57] → Engine_DrainAuxObject @ 0x418BC0.
  if (g_eng_drain_aux) {
    // Soft: aux object list drain OOS (host aux≡null).
  }
}

// PE cam matrix copy @ 0x0042863C..0x00428771: if Engine_hasCameraMatrix
// @ 0x63C550 > 0, per-slot Veh_ensureSceneBound @ 0x48AEA0 → copy tx/ty/tz,
// look, 0x30 orient; inactive → ResHandle_Relink. Host count≡0 → no-op;
// listener later uses render_d3d9_camera_get_lookat (active cam).
static void engine_copy_camera_matrices() {}

// PE Physics_MarkStepReset @ 0x4A5180 — sets Physics_stepResetPending @
// 0x6439D8 = 1. Physics_Step @ 0x4A51BF consumes it (clears body scratch at
// world+280 then pending=0). Host physics_integrate has no body-list wipe —
// soft flag only (ordering parity); consumption OOS.
static int32_t g_physics_step_reset_pending = 0;

static void physics_mark_step_reset() {
  g_physics_step_reset_pending = 1;  // PE @ 0x4A5180
}

// Soft stand-in for *(float*)(Engine_physWorld+8) — max phys substep used by
// SimulateFrame @ 0x42851A and sub_43A3B0 @ 0x43A429. Static IDB BSS at
// unk_6409C4+8 is 0; runtime init writer not recovered (soft PE).
// 0x3D4CCCCD = 0.05f (py_eval/int_convert) — common rdata imm; NOT proven
// as the stock seed. Live read of Engine_physWorld@0x617650→+8 OOS on host.
constexpr float kPhysMaxSubstepSoft = 0.05f;

// PE Engine_SimulateFrame @ 0x00428450 (MainLoop body — NOT EndFrame).
// Stock order (IDA decompile + disasm @ 0x428450..0x42877A):
//   v23 = Engine_frameDt@0x63C534 * *(EngineState+0x110), clamp
//     Engine_dtClamp@0x60C8CC (=0x3DCCCCCD / 0.1f);
//   Engine_simDtAccum@0x63C538 += v23;
//   if accum > flt_5F09D4 (0x38D1B717 / 1e-4) &&
//     !Engine_SimObjectListEmpty@0x429390:
//     walk this+8 via Engine_DllistNext@0x40CFC0 → Engine_TickSimObject
//     @0x4291E0(accum, node+12) or vtbl dtor + --count@+0xC4;
//     then accum = 0;   // TickSim uses ACCUM, physics uses v23
//   Engine_PerfTimerBegin(0);  // OOS host
//   v19 = *(float*)(Engine_physWorld+8);  // max read BEFORE quit (@0x42851A)
//   if !Engine_quitRequested@0x63C520:
//     Physics_MarkStepReset@0x4A5180;
//     while v23>0 && !quit:
//       substep pick vs v19; Physics_Step@0x4A5190 (gate step+world+4,
//         clock += v22 @0x4A548E) → TickTimers@0x427160 → Drain@0x4277F0;
//       Engine_simStepScratch@0x62F270 = 0;  // OOS
//   if stepped <= 0: DrainEventQueues (fallback @0x4285EF);
//   ++Engine_simFrameCounter@0x63C54C (wrap 1000);
//   cam matrix copy via Veh_ensureSceneBound@0x48AEA0 when
//     Engine_hasCameraMatrix@0x63C550 > 0.
// PATH-TO-WORLD Soft deepen (TickSim+CONTROL+timers):
//   soft physWorld clock/residue; TickTimers PE walk EngTimerNode 0x30 vs
//   clock; queueEvent Soft Rebind-only (DrainEvent dllist OOS — documented);
//   pollTimers stand-in; CONTROL TickSim already VA-backed.
// Host Soft: wall GetTickCount × time_warp(-1) ≡ frameDt*EngineState+0x110
//   (query path; m<0 leaves g_warp). Input_tick@0x54DDA0 and
//   Jvm_PumpFrame@0x418D10 are MainLoop neighbors — outside this body.
// Physics_Step body OOS (solver lists @+132/+156/+184, sub_426F60 animate
//   walk, Chassis aero) — call site ≡ physics_integrate(step) only.
//   Soft residual: host physics_integrate still does internal kSub=4
//   (Resources.cpp) — not stock one-solver-pass-per-call.
// Drain Soft PE empty sentinels +0x5C/+0x78/+0xB0 outer do-while
//   (producers OOS — no invented walk); cam count≡0.
}  // namespace
float g_measure = 1000.f;
float g_measure_div3600 = 1000.f / 3600.f;
float g_ten_div_measure = 10.f / 1000.f;
int32_t g_load_depth = 0;
int32_t g_load_peak = 0;
int32_t g_load_opens = 0;
int32_t g_ld_priority = 0;
float g_ld_work_scale = 0.1f;  // PE Engine_ldWorkScale @ 0x0060C8C4
int32_t g_ld_high = 0;         // PE System_ldHigh @ 0x00640920
int32_t g_config_apply_count = 0;
[[maybe_unused]] int32_t g_engine_is_night = 0;
InvObject* g_config_host = nullptr;
int32_t g_engine_headlight_rays = 0;       // dword_6187BC
int32_t g_engine_flares = 0;               // dword_6187C0
int32_t g_engine_shadow_size = 0;          // dword_6187B8
int32_t g_engine_shadows = 0;              // dword_6187B4
float g_engine_shadow_detail = 0.f;        // flt_65AED0
int32_t g_engine_texture_size = 0;         // dword_6188B4
float g_engine_object_detail = 0.f;        // flt_65C424
float g_engine_object_detail_dup = 0.f;    // flt_60C8C8 (stock reads object_detail twice)
float g_engine_object_detail_amp = 0.f;    // flt_6188C4
int32_t g_engine_texture_format = 0;       // dword_6188B8
float g_engine_video_gamma_inv = 1.f;      // flt_617728 = 1/video_gamma when set
float g_engine_particle_density = 0.f;     // flt_618948
int32_t g_engine_skidmark_max = 0;         // dword_618BF4
int32_t g_engine_texture_save_q = 0;       // dword_6188B0 = ftol(texture_save_quality*100)
float g_engine_external_damage = 0.f;      // flt_611358
float g_engine_internal_damage = 0.f;      // flt_61135C
float g_engine_deformation = 0.f;          // flt_611360
int32_t g_engine_mem_vertex_max = 0;       // dword_618D4C
int32_t g_engine_mem_vertex_min = 0;       // dword_618D50
int32_t g_engine_mem_texture_max = 0;      // dword_618D54
int32_t g_engine_mem_texture_min = 0;      // dword_618D58
int32_t g_engine_mem_instance_max = 0;     // dword_618D5C
int32_t g_engine_mem_instance_min = 0;     // dword_618D60
int32_t g_engine_mem_sound_max = 0;        // dword_618D64
int32_t g_engine_mem_sound_min = 0;        // off_618D68
int32_t g_engine_resource_loadrate = 0;    // dword_618D6C
int32_t g_engine_force_feedback = 0;       // dword_777448
float g_engine_ffb_strength = 0.f;         // flt_61AC40 = FFB_strength*10000
float g_engine_ffb_strength_emu = 0.f;     // flt_61AB14 = FFB_strength_emulated*1e-6
float g_engine_engine_inertia = 0.f;       // flt_60E028
float g_engine_wheel_gnd_feedback = 0.f;   // flt_60E020
float g_engine_wheel_brake_factor = 0.f;   // flt_60E024
int32_t g_engine_mouse_help = 0;           // dword_63C83C
float g_engine_steerhelp_turn = 0.f;       // flt_63C7A8
float g_engine_head_move_steer = 0.f;      // flt_60E014
float g_engine_head_move_vel = 0.f;        // flt_60E018
float g_engine_head_move_acc = 0.f;        // flt_60E01C
int32_t g_asyncload_frame = 0;

void engine_mainloop_endframe() {
  for (int i = 0; i < kFileAsyncSlots; ++i) {
    FileAsyncSlot& s = g_fileasync_ring[i];
    if (s.state == kFileAsyncNeedAlloc) {
      s.buf = fileasync_malloc(s.size);  // PE: v0[1]=FileAsync_Malloc(v0[67])
      s.state = kFileAsyncQueued;         // PE: *v0 = 1
    }
  }
  fileasync_release_wake_sem();  // PE: Engine_ReleaseSemaphore(FileAsync_WakeSem)
  fileasync_worker_pump_all();   // W17C stand-in for WorkerThread wake+drain
}


void asyncload_finish_slice() {
  // PE: Engine_OwnedSemaphoreRelease(AsyncLoad_OwnedSem @ 0x65C1D4).
}


void engine_simulate_frame() {
  static unsigned s_last_ms = 0;
  static float s_sim_dt_accum = 0.f;  // PE Engine_simDtAccum @ 0x63C538
  static int32_t s_sim_frame_counter = 0;  // PE Engine_simFrameCounter @ 0x63C54C
  float wall_dt = 0.f;
#ifdef _WIN32
  const unsigned now = GetTickCount();
  if (s_last_ms != 0) {
    wall_dt = static_cast<float>(now - s_last_ms) * 0.001f;
  }
  s_last_ms = now;
#endif
  // PE @ 0x42845F: fmul [ebx+110h] — Engine_timeWarp (InitState=1.0f).
  // Host: time_warp(-1) returns g_warp without apply (Java query path).
  float frame_dt = wall_dt * time_warp(-1.f);
  // PE Engine_dtClamp @ 0x60C8CC = 0x3DCCCCCD (py_eval → 0.1f).
  constexpr float kDtClamp = 0.1f;
  if (frame_dt > kDtClamp) frame_dt = kDtClamp;
  // PE flt_5F09D4 @ 0x5F09D4 = 0x38D1B717 → ~1e-4 (py_eval).
  constexpr float kAccumEps = 0.0001f;

  // PE @ 0x00428485..0x004284FB: accum += clamped scaled dt; TickSim(accum).
  // CONTROL: !SimObjectListEmpty → walk this+8 TickSimObject / dtor; then
  // accum=0 even if empty (PE @ 0x4284FB).
  s_sim_dt_accum += frame_dt;
  if (s_sim_dt_accum > kAccumEps) {
    engine_tick_sim_objects(s_sim_dt_accum);
    s_sim_dt_accum = 0.f;
  }

  // PE @ 0x0042850A PerfTimerBegin(0) OOS.
  // PE @ 0x0042851A: max = *(physWorld+8) BEFORE quit test.
  const float max_step = kPhysMaxSubstepSoft;
  float remaining = frame_dt;
  float stepped = 0.f;
  // PE @ 0x0042852C: quit → LABEL_27 fallback Drain only.
  if (!exit_requested()) {
    // PE @ 0x00428532
    physics_mark_step_reset();
    // Soft: host integrate does not wipe body scratch (PE @ 0x4A51BF OOS).
    // Pending flag left set until Soft consume below (ordering note only).
    (void)g_physics_step_reset_pending;
    g_physics_step_reset_pending = 0;

    // PE @ 0x00428545: if v23<=0 → LABEL_27.
    while (remaining > 0.f && !exit_requested()) {
      float step;
      // PE @ 0x00428557..0x0042858D substep pick (disasm-confirmed).
      if (remaining < max_step) {
        step = remaining;
      } else if (remaining - max_step <= max_step) {
        step = remaining * 0.5f;  // flt_5F09D0 @ 0x5F09D0 = 0.5f
      } else {
        step = max_step;
      }
      // Soft PE Physics_Step gate @ 0x4A5197..0x4A51AE: residue+=step;
      // if sum < 1e-4 return 0 (leave residue); else residue-=sum (=0).
      g_phys_world_residue += step;
      if (g_phys_world_residue < kAccumEps) break;
      g_phys_world_residue = 0.f;  // PE @ 0x4A51FD: +4 -= v22 (sum)
      physics_integrate(step);
      // Soft PE @ 0x4A548E: *(physWorld+0) += v22 — TickTimers clock.
      g_phys_world_clock += step;
      remaining -= step;
      stepped += step;
      // PE @ 0x004285B7 / 0x004285BE — per successful substep.
      engine_tick_timers();
      engine_drain_event_queues();
      // PE Engine_simStepScratch@0x62F270=0 — OOS (no scratch mirror).
    }
  }
  // PE @ 0x004285EB..0x004285EF: if stepped<=0 (or quit before loop) Drain.
  if (stepped <= 0.f) {
    engine_drain_event_queues();
  }
  // PE @ 0x004285F5..0x00428606: ++simFrameCounter; if >=1000 → 0.
  if (++s_sim_frame_counter >= 1000) s_sim_frame_counter = 0;
  // PE @ 0x0042863C.. cam matrix table (hasCameraMatrix≡0 → skip).
  engine_copy_camera_matrices();
}


void eng_ctrl_register_gi(int32_t gi_handle, InvObject* script) {
  eng_ctrl_list_ensure();
  // PE @ 0x427466: Engine_malloc(0x1C). Host: skip insert on OOM (PE
  // assumes success and would deref null at node.prev=). Host node is
  // larger (script tail) — PE head 0x1C still used as list payload.
  auto* node = static_cast<EngSimNode*>(std::malloc(sizeof(EngSimNode)));
  if (!node) return;
  // Engine_SimCallbackNode_ctor @ 0x429130: next/prev=0, vtbl off_5F09BC.
  node->next = nullptr;
  node->prev = nullptr;
  node->vtbl_or_self = nullptr;  // PE vtbls 5F09B8→clear→5F09B4; host unused
  // Engine_ResHandleSlot_clear @ 0x428FC0 (this = node+12).
  node->rh0 = nullptr;
  node->rh1 = nullptr;
  node->alive = nullptr;
  node->payload = nullptr;
  node->script = inv_as_script(script);
  // PE @ 0x42749B: ResHandle_Rebind(node+12, *(handle+12)).
  // createNativeInstance @ 0x481A70: handle+8 = inst+0x50 → unregister match
  // node[5]==*(handle+8). Rebind sets RH[2]=*(owner+0x50), RH[3]=owner and
  // head-inserts into owner+0x48 (dtor / unregister Unlink splice it out).
  void* owner = nullptr;
  if (gi_handle != 0) {
    auto* hp = reinterpret_cast<void**>(
        static_cast<std::uintptr_t>(static_cast<uint32_t>(gi_handle)));
    owner = hp[3];  // *(handle+12)
  }
  resh_rebind(reinterpret_cast<void**>(&node->rh0), owner);
  if (!node->script && owner) {
    void* key = *reinterpret_cast<void**>(reinterpret_cast<char*>(owner) + 0x50);
    node->script = inv_as_script(key);
  }
  // Tail-insert PE @ 0x4274A4..0x4274BD: v13=*(eng+0x18)=eng[6];
  // *(v13+4)=node; node.prev=v13; node.next=eng+0x10 (&eng[4]); eng[6]=node;
  // ++eng+0xC4. First insert overwrites eng[2] via tail-sentinel next.
  auto* tail = reinterpret_cast<EngSimNode*>(g_eng_ctrl[6]);
  if (tail) tail->next = node;
  node->prev = tail;
  node->next = reinterpret_cast<EngSimNode*>(&g_eng_ctrl[4]);
  g_eng_ctrl[6] = reinterpret_cast<std::uintptr_t>(node);
  ++g_eng_ctrl_count;
}


void eng_ctrl_unregister_gi(int32_t gi_handle) {
  eng_ctrl_list_ensure();
  // PE reads *(handle+8); host guard (stock GI blob always valid here).
  if (gi_handle == 0) return;
  auto* hp = reinterpret_cast<void**>(
      static_cast<std::uintptr_t>(static_cast<uint32_t>(gi_handle)));
  void* key = hp[2];  // *(handle+8)
  // PE @ 0x4275C8: result = *(eng+8) = eng[2] head_ptr.
  EngSimNode* cur = reinterpret_cast<EngSimNode*>(g_eng_ctrl[2]);
  if (!cur || cur->next == nullptr) return;  // empty (head sentinel)
  // PE @ 0x4275E2: while node[5] != *(handle+8), DllistNext-style walk.
  while (cur->alive != key) {
    cur = eng_dllist_next(cur);
    if (!cur) return;
  }
  // PE @ 0x4275FA..0x427611: if payload → ResHandle_Unlink(owner+0x44,
  // node+12); else zero slot+8 (alive). Node stays in list; SimulateFrame
  // walk dtor frees when !alive.
  void** rh = reinterpret_cast<void**>(&cur->rh0);
  void* payload = rh[3];
  if (payload) {
    resh_unlink(reinterpret_cast<char*>(payload) + 0x44, rh);
  } else {
    rh[2] = nullptr;  // alive only
  }
  cur->script = nullptr;  // host-only tail
}


void loading_enter() {
  ++g_load_depth;
  if (g_load_depth > g_load_peak) g_load_peak = g_load_depth;
}


void loading_leave() {
  if (g_load_depth > 0) --g_load_depth;
}


void* asyncload_submit_impl(std::uintptr_t type, int32_t async, const char* path,
                            void* a4, void* a5, void* a6) {
  if (type > 5) return nullptr;
  if (async == 0) return nullptr;  // sync + vtbl process OOS

  auto* job = static_cast<AsyncLoadJob*>(std::malloc(sizeof(AsyncLoadJob)));
  if (!job) return nullptr;
  std::memset(job, 0, sizeof(AsyncLoadJob));
  // PE: Type0_ctor@5065F0 / Type1@506980 / Type2@507530 → vtbl Type012_Pump;
  //   Type34_ctor@4FE210 + off_5F2D00/CE8; Type5_ctor@53BC00.
  // Host: type pump vtbl; Type0 stamps [18]/[21] for Process.
  if (type <= 2)
    job->vtbl = &g_asyncload_vtbl_012;
  else if (type == 3)
    job->vtbl = &g_asyncload_vtbl_3;
  else if (type == 4)
    job->vtbl = &g_asyncload_vtbl_4;
  else
    job->vtbl = &g_asyncload_vtbl_5;
  job->fa_status = -1;
  job->field12 = 1;
  job->arg_a4 = a4;
  job->arg_a5 = a5;
  job->arg_a6 = a6;
  job->type = type;
  if (type <= 2) {
    // PE Type0_ctor@5065F0 / Type1@506980 / Type2@507530:
    //   [18]=1.0; [21]=dword_6188B8 (texture_format). Type2_Process may
    //   overwrite [21] via FormatFromFourcc.
    job->field18 = 1.f;
    job->tex_format = g_engine_texture_format;
  }
  if (type == 2) {
    // PE Type2_ctor@507530: +0x90=1; +0x18=dword_65C254.
    job->t2_levels_store = 1;
    job->pe_f18 = g_asyncload_t2_peer_ema;
  }

  if (path && path[0]) {
    // PE: sprintf scratch → malloc(strlen+1) → Util_strncpy_n(..., 256).
    const size_t n = std::strlen(path);
    job->path = static_cast<char*>(std::malloc(n + 1));
    if (job->path) {
      const size_t copy = n < 255 ? n : 255;
      std::memcpy(job->path, path, copy);
      job->path[copy] = '\0';
    }
  }

  // PE: OwnedSemaphoreWait — host no-op.
  job->flags |= 1u;
  const int32_t rc = fileasync_enqueue_path_norm(
      path, nullptr, reinterpret_cast<void*>(&asyncload_on_file_cb), job, 0);
  job->fa_status = rc;
  if (rc < 0) {
    if (rc == -5) {
      asyncload_list_push_tail(g_asyncload_overflow, job);
      return job;
    }
    if (job->vtbl && job->vtbl->dtor) job->vtbl->dtor(job, 1);
    return nullptr;
  }
  asyncload_list_push_tail(g_asyncload_inflight, job);
  return job;
}


int asyncload_has_work() {
  // PE: OwnedSemaphoreWait(AsyncLoad_OwnedSem) — host no-op.
  // Drain Overflow → EnqueuePathNorm → InFlight (stop on first fail).
  AsyncLoadJob* cur = g_asyncload_overflow.head;
  while (cur) {
    AsyncLoadJob* node = cur;
    const int32_t rc = fileasync_enqueue_path_norm(
        node->path, nullptr, reinterpret_cast<void*>(&asyncload_on_file_cb),
        node, 0);
    node->fa_status = rc;
    if (rc < 0) break;
    AsyncLoadJob* nxt = node->next;
    if (nxt && nxt != &g_asyncload_overflow.sent && nxt->next)
      cur = nxt;
    else
      cur = nullptr;
    asyncload_list_unlink(g_asyncload_overflow, node);
    --g_asyncload_overflow.count;  // host coherence (PE leaves count)
    asyncload_list_push_tail(g_asyncload_inflight, node);
  }
  // PE: PumpCursor = ReadyHead if *(ReadyHead+4)!=0. No frame stamp here
  // (MainLoop ++dword_6200A4 between PumpOne on LD_HIGH @ 0x428E3x).
  g_asyncload_pump_cursor = g_asyncload_ready.head;
  g_asyncload_pump_saw_type012 = 0;  // PE AsyncLoad_PumpSawType012 @ 0x65C20C
  g_asyncload_pump_hold_repass = 1;   // PE AsyncLoad_PumpHoldRepass @ 0x618838
  return g_asyncload_pump_cursor != nullptr ? 1 : 0;
}


int asyncload_pump_one() {
  AsyncLoadJob* cur = g_asyncload_pump_cursor;
  if (!cur) return 0;

  // PE: next = cursor->next if next && next->next; else 0 (sentinel tip).
  AsyncLoadJob* nxt_raw = cur->next;
  AsyncLoadJob* nxt = nullptr;
  if (nxt_raw && nxt_raw->next) nxt = nxt_raw;

  if (g_asyncload_pump_saw_type012 == 0) {
    if (cur->type <= 2) g_asyncload_pump_saw_type012 = 1;
  }
  if ((cur->flags & 0x10u) == 0) g_asyncload_pump_hold_repass = 0;

  if (cur->vtbl && cur->vtbl->pump) cur->vtbl->pump(cur);

  if ((cur->flags & 4u) != 0 && cur->buf != nullptr) {
    std::free(cur->buf);  // PE Engine_free
    cur->buf = nullptr;
    cur->nbytes = 0;
  }
  g_asyncload_pump_last_field1c = cur->field1c;  // PE @ 0x65C1B4
  g_asyncload_pump_cursor = nxt;

  // PE PumpOne does not dtor. Fail/Work stay for
  // DrainWorkFail_GetPackSlot@505EA0 → ResourceEngine_GetPackSlot (OOS).
  // W24: do not free Work/Fail here (W23B reclaim UAF risk).

  if (nxt != nullptr) return 1;

  if (g_asyncload_pump_hold_repass != 0) {
    g_asyncload_pump_saw_type012 = 0;
    return 0;
  }
  if (g_asyncload_pump_saw_type012 == 0) return 0;

  g_asyncload_pump_cursor = g_asyncload_ready.head;
  g_asyncload_pump_saw_type012 = 0;
  g_asyncload_pump_hold_repass = 1;
  return g_asyncload_pump_cursor != nullptr ? 1 : 0;
}



// System natives / mainloop hooks -> System_natives.cpp
}  // namespace inv
