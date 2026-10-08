// Fragment 1/2 of System.cpp — included, not compiled on its own.
// Split only to stay under the 128 KB editor buffer cap; the
// translation unit is unchanged. See native/tools/split_source.py.


namespace inv {
namespace {
// PE Engine_isNight @ 0x006178D0 — written by syncGameTime (not mirrored elsewhere).

// Engine globals written by getConfigOptions (PE Engine_ApplyConfigOptions @ 0x427BF0).

// ---------------------------------------------------------------------------
// FileAsync ring stand-in (PE FileAsync_Ring @ 0x0076AA30)
// PE: 64 slots × 71 dwords (memset 0x4700); end sentinel
// FileAsync_ThreadHandle @ 0x76F130 (was hObject).
// Slot: [0]state [1]buf [2..65]path [66]off [67]size [68]userdata [69]prio
//   [70]cb. States: 0 free, 1 queued, 2 busy, 4 need-alloc→EndFrame.
// W16C: EnqueuePath @ 0x5553B0 / EnqueueRead @ 0x555470 hostable (ring fill
// + WakeSem).
// W17C: WorkerThread @ 0x554EA0 — full PE map (HandleCache 8×66 @ 0x76F138,
//   FilePool_* / PackFile_* id<0x10000 vs +0x10000, stats @ 0x76F978..).
//   Host: pump state machine; open-fail was st=-3 until W20C.
// W18C: AsyncLoad queues + Submit@505960 async stub → EnqueuePathNorm@54D290
//   (OnFileCb@505C00). No type0..5 ctors / sync sub_54C300.
// W19B: PumpOne@505DB0 cursor+vtbl+4 stub (Ready unlink; no FilePool).
// W20C: Worker FilePool disk hop — CreateFileA/Seek/GetSize/Read/Close
//   (PE FilePool_OpenRead@558D80 / Seek@559150 / GetSize@559100 /
//   Read@5591D0 / Close@559090).
// W21B: PackFile before FilePool @ 0x555096 — Open@554480 / Seek@554550 /
//   GetSize@554510 / Read@5545B0 / Close@5544F0 (Lookup@5542F0, Base@76A4A0).
// W35-01 Soft PE: HandleCache 8×66 @ 0x76F138 (W34-20 carte) — path hit →
//   bump LRU+Stamp keep-open; miss → free|min-LRU Close + PackFile|FilePool
//   store; open==-2 → Ring state=1 requeue (no cb). Soft FilePool_Handles[64]
//   @ 0x777558 (pool-full -2). Base unset → PackFile miss → FilePool.
//   Status: 0/-1/-2/-3/-4. FlushHandlesForPath@555550 OOS.
// W22B: PackFile_SetRoot@5542C0 (Base@76A4A0) — 0 PE call/data xrefs;
//   File_UseTree@7686E0 only cleared @ File_InitHandles@54C0D0. Host mount:
//   packfile_set_root (PE) + packfile_mount_path (CreateFileA oneshot →
//   owned blob → SetRoot; same disk stub as FilePool).
// W23B: type0..5 pump shells (Ready unlink → fail@65C1F0 / work@65BD68 /
//   Ready requeue). W24B/W31A Type0_Process (vt+0xF0 create_from_mem a5=0);
//   W25A Type3_Process INVO slice; W26A Type1/2_Process create_dims.
// ---------------------------------------------------------------------------
constexpr int kFileAsyncSlots = 64;
constexpr int kFileAsyncStride = 71;  // dwords; PE +71 / 284 bytes
static_assert(kFileAsyncSlots * kFileAsyncStride * 4 == 0x4700,
              "PE FileAsync_Init memset size");
enum : int32_t {
  kFileAsyncFree = 0,
  kFileAsyncQueued = 1,
  kFileAsyncBusy = 2,
  kFileAsyncNeedAlloc = 4,
};
// PE Worker statuses @ LABEL_62 / open-fail @ 0x555200 / pool-full @ 0x5551AE.
constexpr int32_t kFileAsyncStatusPoolFull = -2;  // FilePool_OpenRead @ 0x558DD5
constexpr int32_t kFileAsyncStatusOpenFail = -3;
constexpr int32_t kFileAsyncStatusMallocFail = -4;
constexpr int32_t kPackFileHandleBias = 0x10000;  // Worker id encoding @ 0x5550A2
struct FileAsyncSlot {
  int32_t state = 0;           // [0]
  void* buf = nullptr;         // [1] — FileAsync_Ring_buf
  char path[256]{};            // [2..65] — FileAsync_Ring_path
  int32_t offset = 0;          // [66] — FileAsync_Ring_offset
  int32_t size = 0;            // [67] — FileAsync_Ring_size
  void* userdata = nullptr;    // [68] — FileAsync_Ring_userdata
  int32_t priority = 0;        // [69] — FileAsync_Ring_priority
  void* callback = nullptr;    // [70] — FileAsync_Ring_callback
};
// Host stand-in (not PE BSS); pointer width may differ from 32-bit PE.
FileAsyncSlot g_fileasync_ring[kFileAsyncSlots];
// PE FileAsync_Active @ 0x76F980 — set by FileAsync_Init @ 0x554E10 after
// CreateThread(Worker). Host: Active without worker/sem table.
int32_t g_fileasync_active = 1;
int32_t g_fileasync_overflow = 0;  // PE FileAsync_OverflowCount @ 0x76F998
// PE Worker RR: last completed slot index (v34 / prior ebx).
int32_t g_fileasync_last_slot = -1;
// PE FileAsync_JobsDone @ 0x76F98C (host counter only).
int32_t g_fileasync_jobs_done = 0;
// PE FileAsync_BytesRead @ 0x76F994.
int32_t g_fileasync_bytes_read = 0;

// PE cb(userdata, status, buf, nbytes) — cdecl @ Worker LABEL_62.
using FileAsyncCb = void (*)(void* userdata, int32_t status, void* buf,
                             uint32_t nbytes);

// PE FileAsync_Malloc @ 0x0054F6E0 → CRT_malloc @ 0x00559500 (size>0).
void* fileasync_malloc(int32_t nbytes) {
  if (nbytes <= 0) return nullptr;
  return std::malloc(static_cast<size_t>(nbytes));
}

// ---------------------------------------------------------------------------
// PackFile_* (PE @ 0x554280..0x55460B) — in-memory VFS tree.
// Entry 44B @ Lookup: +0 off, +4 size, +8 flags (bit0=dir), +12 name[32].
// PackFile_Base @ 0x76A4A0 (SetRoot@5542C0); slots 32×{entry*,cursor}
// @ 0x76A928/92C. Worker prefers PackFile_Open@555096 then FilePool.
// W35-01: Worker keep-open via HandleCache (Close on LRU eviction only).
// ---------------------------------------------------------------------------
constexpr int kPackFileSlots = 32;
constexpr int kPackFileSegMax = 32;
#pragma pack(push, 1)
struct PackFileEntry {
  int32_t data_off;  // +0 — file bytes or child-list off (dir)
  int32_t size;      // +4
  uint8_t flags;     // +8 — bit0 = directory
  uint8_t pad[3];    // +9
  char name[32];     // +12
};
#pragma pack(pop)
static_assert(sizeof(PackFileEntry) == 44, "PE PackFile entry stride");

// PE PackFile_Base @ 0x76A4A0 — null until PackFile_SetRoot@5542C0.
// IDA: 0 call / 0 data refs to SetRoot; only writers are SetRoot + Reset@554280.
const uint8_t* g_packfile_base = nullptr;
// Host-owned blob for packfile_mount_path (PE SetRoot does not allocate).
std::vector<uint8_t> g_packfile_owned;

struct PackFileSlot {
  const PackFileEntry* entry = nullptr;
  int32_t cursor = 0;
};
PackFileSlot g_packfile_slots[kPackFileSlots];

// PE PackFile_SetRoot @ 0x5542C0 — mov PackFile_Base,a1; return 0.
int32_t packfile_set_root(const uint8_t* base) {
  g_packfile_base = base;
  return 0;
}

// Host W22B: path → CreateFileA full read (FilePool oneshot style) → SetRoot.
// No PE path→SetRoot; stock never calls SetRoot / never sets File_UseTree.
// Returns 0 on success, -3 open-fail, -4 malloc/empty (Worker status codes).
int32_t packfile_mount_path(const char* path) {
  if (path == nullptr || path[0] == '\0') return kFileAsyncStatusOpenFail;
#ifdef _WIN32
  HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return kFileAsyncStatusOpenFail;
  const DWORD fsz = GetFileSize(h, nullptr);
  if (fsz == INVALID_FILE_SIZE || fsz == 0) {
    CloseHandle(h);
    return kFileAsyncStatusMallocFail;
  }
  std::vector<uint8_t> blob(static_cast<size_t>(fsz));
  DWORD got = 0;
  const BOOL ok =
      ReadFile(h, blob.data(), fsz, &got, nullptr) && got == fsz;
  CloseHandle(h);
  if (!ok) return kFileAsyncStatusOpenFail;
  g_packfile_owned = std::move(blob);
  return packfile_set_root(g_packfile_owned.data());
#else
  (void)path;
  return kFileAsyncStatusOpenFail;
#endif
}

// PE Path_CompareCstr @ 0x554A10 a3!=0 — case-insensitive; 0 equal.
int packfile_path_compare_ci(const char* a, const char* b) {
  if (a == nullptr || b == nullptr) return (a == b) ? 0 : (a ? 1 : -1);
  for (;;) {
    char ca = *a++;
    char cb = *b++;
    if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca + 32);
    if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb + 32);
    if (ca < cb) return -1;
    if (ca > cb) return 1;
    if (ca == 0) return 0;
  }
}

// PE PackFile_Lookup @ 0x5542F0 — walk \\ segments; want_file → !(flags&1).
const PackFileEntry* packfile_lookup(const uint8_t* base, const char* path,
                                    bool want_file) {
  if (base == nullptr || path == nullptr) return nullptr;
  const char* p = path;
  char c = *p;
  // PE: result starts at base; empty path skips loop → flag-check on root.
  const PackFileEntry* result =
      reinterpret_cast<const PackFileEntry*>(base);
  char seg[kPackFileSegMax];
  while (c != 0) {
    int n = 0;
    while (c != '\\') {
      if (n == kPackFileSegMax) return nullptr;
      seg[n++] = c;
      ++p;
      c = *p;
      if (c == 0) break;
    }
    if (c == '\\') {
      ++p;
      c = *p;
    }
    seg[n] = '\0';
    const PackFileEntry* e = result;
    // PE @ 0x554338: empty level (*off==0) → miss.
    if (e->data_off == 0) return nullptr;
    while (packfile_path_compare_ci(e->name, seg) != 0) {
      // Peek next entry's +0; 0 → end of sibling list (@ 0x554351).
      const int32_t next_off =
          *reinterpret_cast<const int32_t*>(
              reinterpret_cast<const uint8_t*>(e) + 44);
      e = reinterpret_cast<const PackFileEntry*>(
          reinterpret_cast<const uint8_t*>(e) + 44);
      if (next_off == 0) return nullptr;
    }
    result = e;
    if (c == 0) break;
    if ((result->flags & 1) == 0) return nullptr;  // need dir
    // PE for-incr: result = base + entry->data_off (child list).
    result = reinterpret_cast<const PackFileEntry*>(base + result->data_off);
  }
  if (want_file) {
    if ((result->flags & 1) != 0) return nullptr;
    return result;
  }
  if ((result->flags & 1) != 0) return result;
  return nullptr;
}

// PE PackFile_Open @ 0x554480 — slot index or -1.
int32_t packfile_open(const char* path) {
  int slot = 0;
  for (; slot < kPackFileSlots; ++slot) {
    if (g_packfile_slots[slot].entry == nullptr) break;
  }
  if (slot == kPackFileSlots) return -1;
  const PackFileEntry* e = packfile_lookup(g_packfile_base, path, true);
  if (e == nullptr) return -1;
  g_packfile_slots[slot].entry = e;
  g_packfile_slots[slot].cursor = 0;
  return slot;
}

// PE PackFile_Close @ 0x5544F0.
void packfile_close(int32_t slot) {
  if (slot < 0 || slot >= kPackFileSlots) return;
  g_packfile_slots[slot].entry = nullptr;
  g_packfile_slots[slot].cursor = 0;
}

// PE PackFile_GetSize @ 0x554510.
int32_t packfile_get_size(int32_t slot) {
  if (slot < 0 || slot >= kPackFileSlots) return -1;
  const PackFileEntry* e = g_packfile_slots[slot].entry;
  if (e == nullptr) return -1;
  return e->size;
}

// PE PackFile_Seek @ 0x554550 — set cursor; returns prior.
int32_t packfile_seek(int32_t slot, int32_t off) {
  if (slot < 0 || slot >= kPackFileSlots) return 0;
  if (g_packfile_slots[slot].entry == nullptr) return 0;
  const int32_t prev = g_packfile_slots[slot].cursor;
  g_packfile_slots[slot].cursor = off;
  return prev;
}

// PE PackFile_Read @ 0x5545B0 → Engine_MemcpyThunk@559590.
uint32_t packfile_read(int32_t slot, void* dst, uint32_t nbytes) {
  if (slot < 0 || slot >= kPackFileSlots || g_packfile_base == nullptr)
    return static_cast<uint32_t>(-1);
  PackFileSlot& s = g_packfile_slots[slot];
  if (s.entry == nullptr) return static_cast<uint32_t>(-1);
  uint32_t n = nbytes;
  const uint32_t sz = static_cast<uint32_t>(s.entry->size);
  const uint32_t cur = static_cast<uint32_t>(s.cursor);
  if (cur >= sz)
    n = 0;
  else if (cur + n > sz)
    n = sz - cur;
  const uint8_t* src =
      g_packfile_base + static_cast<uint32_t>(s.entry->data_off) + cur;
  if (dst != nullptr && n > 0) std::memcpy(dst, src, n);
  s.cursor = static_cast<int32_t>(cur + n);
  return n;
}

// ---------------------------------------------------------------------------
// PE FilePool_* @ 0x558D80..0x5591D0 — Handles[64] @ 0x777558 / Sizes @
// 0x77755C (stride 2 dwords); LockSem @ 0x61B630 OOS (host single-threaded).
// OpenRead returns slot, -1 CreateFile fail, -2 pool-full (v1==64).
// W35-01 Soft PE: real 64-slot table so Worker HandleCache keep-open + -2
// requeue are VA-faithful.
// ---------------------------------------------------------------------------
constexpr int kFilePoolSlots = 64;
struct FilePoolSlot {
#ifdef _WIN32
  HANDLE handle = nullptr;
#else
  void* handle = nullptr;
#endif
  int32_t size = 0;
};
FilePoolSlot g_filepool_slots[kFilePoolSlots];

// PE FilePool_OpenRead @ 0x558D80.
int32_t filepool_open_read(const char* path) {
  if (path == nullptr || path[0] == '\0') return -1;
  int slot = 0;
  for (; slot < kFilePoolSlots; ++slot) {
    if (g_filepool_slots[slot].handle == nullptr) break;
  }
  if (slot == kFilePoolSlots) return kFileAsyncStatusPoolFull;  // @ 0x558DD5
#ifdef _WIN32
  // PE @ 0x558DF3: GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, NORMAL.
  HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return -1;
  const DWORD fsz = GetFileSize(h, nullptr);
  g_filepool_slots[slot].handle = h;
  g_filepool_slots[slot].size =
      (fsz == INVALID_FILE_SIZE) ? 0 : static_cast<int32_t>(fsz);
  return slot;
#else
  (void)path;
  return -1;
#endif
}

// PE FilePool_Close @ 0x559090.
void filepool_close(int32_t slot) {
  if (slot < 0 || slot >= kFilePoolSlots) return;
#ifdef _WIN32
  if (g_filepool_slots[slot].handle != nullptr) {
    CloseHandle(g_filepool_slots[slot].handle);
  }
#endif
  g_filepool_slots[slot].handle = nullptr;
  g_filepool_slots[slot].size = 0;
}

// PE FilePool_GetSize @ 0x559100 — cached size from Open.
int32_t filepool_get_size(int32_t slot) {
  if (slot < 0 || slot >= kFilePoolSlots) return -1;
  if (g_filepool_slots[slot].handle == nullptr) return -1;
  return g_filepool_slots[slot].size;
}

// PE FilePool_Seek @ 0x559150 — absolute FILE_BEGIN (PE also samples CUR).
int32_t filepool_seek(int32_t slot, int32_t offset) {
  if (slot < 0 || slot >= kFilePoolSlots) return -1;
#ifdef _WIN32
  HANDLE h = g_filepool_slots[slot].handle;
  if (h == nullptr) return -1;
  const DWORD prev =
      SetFilePointer(h, 0, nullptr, FILE_CURRENT);  // PE sample CUR
  if (SetFilePointer(h, offset, nullptr, FILE_BEGIN) ==
      INVALID_SET_FILE_POINTER) {
    return -1;
  }
  return static_cast<int32_t>(prev);
#else
  (void)offset;
  return -1;
#endif
}

// PE FilePool_Read @ 0x5591D0 — ReadFile; FormatMessage path OOS.
int32_t filepool_read(int32_t slot, void* dst, uint32_t nbytes) {
  if (slot < 0 || slot >= kFilePoolSlots || dst == nullptr) return -1;
#ifdef _WIN32
  HANDLE h = g_filepool_slots[slot].handle;
  if (h == nullptr) return -1;
  DWORD got = 0;
  if (!ReadFile(h, dst, nbytes, &got, nullptr)) return -1;
  return static_cast<int32_t>(got);
#else
  (void)nbytes;
  return -1;
#endif
}

// ---------------------------------------------------------------------------
// PE FileAsync_HandleCache @ 0x76F138 — 8 slots × 66 dwords (W34-20 / W35-01).
// Slot: path[256] @ +0, Handle @ +0x100 (dword[64]), Lru @ +0x104 (dword[65]).
// Stamp @ 0x76F134; End @ 0x76FA78. Hit: Path_CompareCstr(ci) → bump Lru+Stamp,
// reuse handle (keep-open). Miss: free slot or min-Lru → Close old →
// PackFile_Open else FilePool_OpenRead → store. Worker open==-2 → requeue.
// FlushHandlesForPath @ 0x555550 OOS (cancel ring + invalidate cache by path).
// ---------------------------------------------------------------------------
constexpr int kHandleCacheSlots = 8;
constexpr int kHandleCacheStrideDwords = 66;  // PE +66 / 264 bytes
static_assert(kHandleCacheSlots * kHandleCacheStrideDwords == 528,
              "PE HandleCache walk bound v14>=528");
struct FileAsyncHandleCacheSlot {
  char path[256]{};
  int32_t handle = -1;  // -1 empty; <0x10000 FilePool; else PackFile+bias
  int32_t lru = 0;
};
FileAsyncHandleCacheSlot g_fileasync_handle_cache[kHandleCacheSlots];
int32_t g_fileasync_handle_cache_stamp = 0;  // PE @ 0x76F134

void fileasync_handlecache_close_id(int32_t id) {
  if (id < 0) return;
  if (id < kPackFileHandleBias)
    filepool_close(id);
  else
    packfile_close(id - kPackFileHandleBias);
}

// Seek/GetSize/Read through a Worker handle id (PackFile bias or FilePool).
void fileasync_handle_seek(int32_t id, int32_t offset) {
  if (id < 0) return;
  if (id < kPackFileHandleBias)
    filepool_seek(id, offset);
  else
    packfile_seek(id - kPackFileHandleBias, offset);
}

int32_t fileasync_handle_get_size(int32_t id) {
  if (id < 0) return -1;
  if (id < kPackFileHandleBias) return filepool_get_size(id);
  return packfile_get_size(id - kPackFileHandleBias);
}

int32_t fileasync_handle_read(int32_t id, void* dst, uint32_t nbytes) {
  if (id < 0) return -1;
  if (id < kPackFileHandleBias) return filepool_read(id, dst, nbytes);
  return static_cast<int32_t>(
      packfile_read(id - kPackFileHandleBias, dst, nbytes));
}

// PE Worker @ 0x554FC1..0x5550EC: lookup / install HandleCache entry.
// Returns handle id (>=0), -1 open-fail, -2 pool-full (FilePool only).
int32_t fileasync_handlecache_open(const char* path) {
  if (path == nullptr || path[0] == '\0') return -1;
  // Hit: handle>=0 && Path_CompareCstr(ci)==0 → bump Lru+Stamp (@ 0x554FC1).
  for (int i = 0; i < kHandleCacheSlots; ++i) {
    FileAsyncHandleCacheSlot& e = g_fileasync_handle_cache[i];
    if (e.handle >= 0 && packfile_path_compare_ci(e.path, path) == 0) {
      const int32_t st = g_fileasync_handle_cache_stamp;
      e.lru = st;
      g_fileasync_handle_cache_stamp = st + 1;
      return e.handle;
    }
  }
  // Miss: first free (handle<0) else min-Lru (@ 0x554FF7..0x555052).
  int victim = 0;
  int32_t victim_lru = g_fileasync_handle_cache[0].lru;
  for (int i = 0; i < kHandleCacheSlots; ++i) {
    if (g_fileasync_handle_cache[i].handle < 0) {
      victim = i;
      break;
    }
    if (g_fileasync_handle_cache[i].lru < victim_lru) {
      victim = i;
      victim_lru = g_fileasync_handle_cache[i].lru;
    }
  }
  FileAsyncHandleCacheSlot& slot = g_fileasync_handle_cache[victim];
  if (slot.handle >= 0) {
    fileasync_handlecache_close_id(slot.handle);
    slot.handle = -1;
  }
  // PackFile_Open then FilePool_OpenRead (@ 0x555096 / 0x5550B9).
  int32_t id = packfile_open(path);
  if (id < 0) {
    id = filepool_open_read(path);  // may be -1 or -2
  } else {
    id = id + kPackFileHandleBias;
  }
  if (id >= 0) {
    const int32_t st = g_fileasync_handle_cache_stamp;
    slot.handle = id;
    slot.lru = st;
    g_fileasync_handle_cache_stamp = st + 1;
    slot.path[0] = '\0';
    std::strncpy(slot.path, path, sizeof(slot.path) - 1);
    slot.path[sizeof(slot.path) - 1] = '\0';
  }
  return id;
}

// Sync oneshot PackFile (ReadEntire / Type3 .bon) — Close after read; no cache.
bool fileasync_packfile_read(const char* path, int32_t offset, void*& buf,
                             uint32_t& nbytes, int32_t& status) {
  const int32_t slot = packfile_open(path);
  if (slot < 0) {
    status = kFileAsyncStatusOpenFail;
    return false;
  }
  packfile_seek(slot, offset);
  uint32_t to_read = nbytes;
  if (to_read == 0) {
    const int32_t fsz = packfile_get_size(slot);
    const int32_t rem = fsz - offset;
    if (fsz < 0 || rem < 0) {
      packfile_close(slot);
      status = -1;
      return true;
    }
    to_read = static_cast<uint32_t>(rem);
  }
  void* dest = buf;
  if (dest == nullptr) {
    dest = fileasync_malloc(static_cast<int32_t>(to_read));
    if (dest == nullptr) {
      packfile_close(slot);
      status = kFileAsyncStatusMallocFail;
      nbytes = to_read;
      return true;
    }
  }
  const uint32_t got = packfile_read(slot, dest, to_read);
  packfile_close(slot);
  buf = dest;
  nbytes = to_read;
  g_fileasync_bytes_read += static_cast<int32_t>(got);
  status = (got == to_read) ? 0 : -1;
  return true;
}

// Sync oneshot FilePool (ReadEntire) — Open/Seek/Read/Close; not HandleCache.
bool fileasync_filepool_read(const char* path, int32_t offset, void*& buf,
                             uint32_t& nbytes, int32_t& status) {
  const int32_t slot = filepool_open_read(path);
  if (slot == kFileAsyncStatusPoolFull) {
    status = kFileAsyncStatusPoolFull;
    return false;
  }
  if (slot < 0) {
    status = kFileAsyncStatusOpenFail;
    return false;
  }
  if (filepool_seek(slot, offset) < 0) {
    filepool_close(slot);
    status = kFileAsyncStatusOpenFail;
    return false;
  }
  uint32_t to_read = nbytes;
  if (to_read == 0) {
    const int32_t fsz = filepool_get_size(slot);
    const int32_t rem = fsz - offset;
    if (fsz < 0 || rem <= 0) {
      filepool_close(slot);
      status = kFileAsyncStatusOpenFail;
      return false;
    }
    to_read = static_cast<uint32_t>(rem);
  }
  void* dest = buf;
  if (dest == nullptr) {
    dest = fileasync_malloc(static_cast<int32_t>(to_read));
    if (dest == nullptr) {
      filepool_close(slot);
      status = kFileAsyncStatusMallocFail;
      nbytes = to_read;
      return false;
    }
  }
  const int32_t got = filepool_read(slot, dest, to_read);
  filepool_close(slot);
  buf = dest;
  nbytes = to_read;
  if (got < 0) {
    status = -1;
    return true;
  }
  g_fileasync_bytes_read += got;
  status = (static_cast<uint32_t>(got) == to_read) ? 0 : -1;
  return true;
}

// PE FilePool_ReadEntireFile @ 0x54C300 — Path_CopyCstr + '/'→'\\' +
// OpenRead/GetSize/malloc/Read/Close. Returns nbytes, -1 open, -2 malloc.
int32_t filepool_read_entire_file(const char* path, void** out_buf) {
  if (!path || !out_buf) return -1;
  *out_buf = nullptr;
  char norm[256];
  norm[0] = '\0';
  std::strncpy(norm, path, sizeof(norm) - 1);
  norm[sizeof(norm) - 1] = '\0';
  for (char* p = norm; *p; ++p) {
    if (*p == '/') *p = '\\';
  }
  void* buf = nullptr;
  uint32_t nbytes = 0;
  int32_t status = 0;
  if (!fileasync_filepool_read(norm, 0, buf, nbytes, status)) {
    return (status == kFileAsyncStatusMallocFail) ? -2 : -1;
  }
  if (status == kFileAsyncStatusMallocFail) return -2;
  if (status < 0) {
    if (buf) std::free(buf);
    return -1;
  }
  *out_buf = buf;
  return static_cast<int32_t>(nbytes);
}

// PE Engine_ReleaseSemaphore @ 0x00559880(FileAsync_WakeSem @ 0x61AC3C).
// Host: no semaphore table / worker thread — stand-in no-op; EndFrame then
// pumps queued slots synchronously (W17C).
void fileasync_release_wake_sem() {
  // PE: ReleaseSemaphore(hSemaphore[2*id], 1) after --count; wake worker.
}

// PE FileAsync_EnqueuePath @ 0x005553B0 size 0xB3.
// Find free slot; fill path/buf/cb/user/prio; off=size=0; state=1; WakeSem.
// Returns slot index, -1 if !Active, -5 if ring full.
// Callers: FileAsync_EnqueuePathNorm @ 0x54D290 (AsyncLoad_Submit/HasWork).
int32_t fileasync_enqueue_path(const char* path, void* buf, void* cb,
                               void* userdata, int32_t prio) {
  if (g_fileasync_active == 0) return -1;
  int slot = 0;
  for (; slot < kFileAsyncSlots; ++slot) {
    if (g_fileasync_ring[slot].state == kFileAsyncFree) break;
  }
  if (slot < kFileAsyncSlots) {
    FileAsyncSlot& s = g_fileasync_ring[slot];
    s.buf = buf;
    s.offset = 0;
    s.size = 0;
    s.userdata = userdata;
    s.priority = prio;
    s.callback = cb;
    s.path[0] = '\0';
    if (path) {
      std::strncpy(s.path, path, sizeof(s.path) - 1);
      s.path[sizeof(s.path) - 1] = '\0';
    }
    s.state = kFileAsyncQueued;
  }
  fileasync_release_wake_sem();
  if (slot != kFileAsyncSlots) return slot;
  ++g_fileasync_overflow;
  return -5;
}

// PE FileAsync_EnqueueRead @ 0x00555470 size 0xE0.
// Same as Path; if buf==0 && size>0 → FileAsync_Malloc(size); returns size
// on success (not slot), -1 !Active, -5 full.
// No host caller yet (PE: EnqueueReadNorm @ 0x54D300).
[[maybe_unused]] int32_t fileasync_enqueue_read(const char* path, void* buf,
                                                int32_t off, int32_t nbytes,
                                                void* cb, void* userdata,
                                                int32_t prio) {
  if (g_fileasync_active == 0) return -1;
  int slot = 0;
  for (; slot < kFileAsyncSlots; ++slot) {
    if (g_fileasync_ring[slot].state == kFileAsyncFree) break;
  }
  if (slot < kFileAsyncSlots) {
    void* alloc = buf;
    if (alloc == nullptr && nbytes > 0) alloc = fileasync_malloc(nbytes);
    FileAsyncSlot& s = g_fileasync_ring[slot];
    s.buf = alloc;
    s.offset = off;
    s.size = nbytes;
    s.userdata = userdata;
    s.priority = prio;
    s.callback = cb;
    s.path[0] = '\0';
    if (path) {
      std::strncpy(s.path, path, sizeof(s.path) - 1);
      s.path[sizeof(s.path) - 1] = '\0';
    }
    s.state = kFileAsyncQueued;
  }
  fileasync_release_wake_sem();
  if (slot != kFileAsyncSlots) return nbytes;
  ++g_fileasync_overflow;
  return -5;
}

// PE FileAsync_WorkerThread @ 0x554EA0 — one job.
// Pick state==1 by highest prio; equal prio → RR after last_slot (PE v34).
// state=2 → HandleCache@76F138 → Seek/GetSize/Read (keep-open) → cb → state=0.
// open==-2 → Ring state=1 requeue (no cb). Returns 1 if progressed, 0 idle.
int fileasync_worker_pump_one() {
  const int32_t last = g_fileasync_last_slot;
  int best = -1;
  int32_t best_prio = 0;
  for (int i = 0; i < kFileAsyncSlots; ++i) {
    if (g_fileasync_ring[i].state != kFileAsyncQueued) continue;
    const int32_t p = g_fileasync_ring[i].priority;
    if (best < 0) {
      best = i;
      best_prio = p;
      continue;
    }
    if (p > best_prio) {
      best = i;
      best_prio = p;
    } else if (p == best_prio) {
      // PE: prefer slot > last when best_so_far <= last (wrap RR).
      if (best <= last && i > last) best = i;
    }
  }
  if (best < 0) return 0;

  FileAsyncSlot& s = g_fileasync_ring[best];
  void* buf = s.buf;
  void* user = s.userdata;
  void* cb = s.callback;
  uint32_t nbytes = static_cast<uint32_t>(s.size);
  const int32_t offset = s.offset;
  s.state = kFileAsyncBusy;  // PE @ 0x554F9B
  ++g_fileasync_jobs_done;  // PE FileAsync_JobsDone @ 0x76F98C

  // PE @ 0x554FC1..0x5550EC HandleCache; @ 0x5551AE pool-full -2 requeue.
  int32_t status = kFileAsyncStatusOpenFail;
  if (s.path[0] != '\0') {
    const int32_t hid = fileasync_handlecache_open(s.path);
    if (hid == kFileAsyncStatusPoolFull) {
      // PE @ 0x5551AE..0x5551F3: LockSem; Ring[slot]=1; no cb / no free.
      s.state = kFileAsyncQueued;
      return 1;
    }
    if (hid < 0) {
      status = kFileAsyncStatusOpenFail;  // PE @ 0x555200 → -3
    } else {
      // Keep-open: Seek/GetSize/Read; Close only on LRU eviction.
      fileasync_handle_seek(hid, offset);
      uint32_t to_read = nbytes;
      if (to_read == 0) {
        // PE @ 0x555122..0x55514B: GetSize − offset (no clamp).
        const int32_t fsz = fileasync_handle_get_size(hid);
        to_read = static_cast<uint32_t>(fsz - offset);
      }
      void* dest = buf;
      if (dest == nullptr) {
        // PE FileAsync_Malloc @ 0x555162; fail → -4 @ 0x5551A4.
        dest = fileasync_malloc(static_cast<int32_t>(to_read));
        if (dest == nullptr) {
          status = kFileAsyncStatusMallocFail;
          nbytes = to_read;
        }
      }
      if (status != kFileAsyncStatusMallocFail) {
        const int32_t got = fileasync_handle_read(hid, dest, to_read);
        buf = dest;
        nbytes = to_read;
        if (got < 0) {
          status = -1;
        } else {
          g_fileasync_bytes_read += got;
          // PE @ 0x55519B: (read == expected) - 1 → 0 or -1.
          status = (static_cast<uint32_t>(got) == to_read) ? 0 : -1;
        }
      }
    }
  }
  s.buf = buf;
  s.size = static_cast<int32_t>(nbytes);

  if (cb) {
    reinterpret_cast<FileAsyncCb>(cb)(user, status, buf, nbytes);
  }

  s.state = kFileAsyncFree;  // PE @ 0x555258 under LockSem
  g_fileasync_last_slot = best;
  return 1;
}

// Drain all queued slots (PE inner while before Wait(WakeSem)).
void fileasync_worker_pump_all() {
  while (fileasync_worker_pump_one()) {
  }
}

// PE Engine_MainLoop_EndFrame @ 0x00554DC0 size 0x44.
// Walk ring; state==4 → buf=FileAsync_Malloc(size), state=1; then WakeSem.
// W17C: after WakeSem, PE worker drains; host pumps sync (no thread).
// ---------------------------------------------------------------------------
// AsyncLoad_* (W18C queues/Submit + W19B PumpOne + W23B type pumps)
//
// PE lists (Init @ 0x505360/460/560): TailSent / Head / SentNext / Tail +
//   counts Overflow@65C200 InFlight@65C204 Ready@65C208.
// WorkList_Init@505660 / FailList_Init@505760 → Work@65BD60/68/70,
//   Fail@65C1E8/F0/F8. DrainWorkFail_GetPackSlot@505EA0 (0 call xrefs) →
//   ResourceEngine_GetPackSlot — OOS (Resources).
// JobBase_ctor @ 0x505860: next/prev/path=0, flags=0, buf/size=0, fa=-1.
// Submit @ 0x505960: type0..5 malloc+ctor vtbls; async(a2!=0): OwnedSem +
//   EnqueuePathNorm → InFlight (ok) / Overflow (-5) / dtor (other fail);
//   sync(a2==0): sub_54C300 + vtbl — OOS.
// OnFileCb @ 0x505C00: InFlight unlink → ReadyTail; flags|=2 ok else
//   |=0x80000004 fail. W20C Worker FilePool read → success stores buf/n.
// HasWork @ 0x505CD0: Overflow→EnqueuePathNorm→InFlight; PumpCursor=
//   ReadyHead; SawType012=0 HoldRepass=1; return cursor!=0.
// PumpOne @ 0x505DB0: save next; SawType012|=(type<=2); clear HoldRepass
//   unless flags&0x10; vtbl+4; free buf if flags&4; copy +0x1C →
//   PumpLastField1C@65C1B4; advance cursor; if exhausted && !HoldRepass &&
//   SawType012 → refill ReadyHead (2nd pass). PE does not dtor here.
// Type pumps (vtbl+4):
//   Type012@507E50 (shared 0/1/2): Ready unlink; flags<0→Fail; else
//     vtbl+16 Process then Ready requeue or Work.
//   Type3@501A70: Ready unlink; flags<0→Fail; flags&2→vtbl+20; requeue/Work.
//   Type4@503A00: Ready unlink; flags<0→Fail; always vtbl+20; &2→Ready else Work.
//   Type5@53BAB0: PerfTimer; flags>=0→vtbl+12; unlink; &2→ReadyTail else orphan.
// Process: Type0@506650 W24B (create+flag paths; D3D callees OOS);
//   Type1@507110 W26A create+clamp+fail/success; Type2@507840 W29A
//   FormatFromFourcc+create+clamp + W28B begin_mip_step/Poll/Finish;
//   Type3@501B90 W25A mesh INVO slice; Type4@503B00 mesh; Type5@53BB90.
// FinishSlice @ 0x505DA0: OwnedSemRelease — host no-op (no sem table).
// ---------------------------------------------------------------------------
struct AsyncLoadJob;
struct AsyncLoadVtbl {
  void (*dtor)(AsyncLoadJob* self, int flags);  // [0] PE thiscall dtor
  void (*pump)(AsyncLoadJob* self);             // [1] +4 — PumpOne calls
};

// PE INVO magic dword @ buf+0 (int_convert 1331056201 → "INVO").
constexpr uint32_t kInvoMagic = 0x4F564E49u;

// W30A — soft PE ParseInvoChunks case1-3 mat (PE malloc 84B).
// BaseInit@505100 clears +8/+C/+10/+14; overlay off_5F2C08 + flags@+18=
// 0x80000001. AddTexId@4FE0F0 → tex_ids; SetMode@4FE130 → mode.
// MatVec_Push@505120 PE targets case0 wrap+4 — W32A branches when wrap set.
// POD (job via malloc/memset); no std::vector on AsyncLoadJob.
struct AsyncLoadMatObj {
  int32_t* tex_ids;   // PE +8 growable
  int32_t tex_count;  // PE +C
  int32_t tex_cap;    // PE +10
  int32_t mode;       // PE +14/+20 SetMode
  uint32_t flags;     // PE +0x18
};

// W32A — soft PE ParseInvoChunks case0 CreateVB wrap (malloc 24).
// wrap+0 = GfxDevice_CreateVB@4BEEF0 (device vt+0xCC); +4/+8/+C = MatVec
// (ptrs/count/cap) targeted by MatVec_Push@505120.
// W33A — soft CreateMatFromStages@4BEFC0 type0 stage blob + ApplyStages@4E5890
//   + GfxMat_GetUvTexInfo@4C68E0 (vt+8 → tex_count / uv slots). Other mat
//   types stub tex_count=1.
// W34-01 — UploadVbFvf@503788 apply uv_slot xform before PackVertex; soft
//   MatObj_PostLoop@4FFFE0 usage stamp (D3D VB/IB create OOS).
// mesh_vb stays case4 only.
struct AsyncLoadGfxStage {
  int32_t d[9];  // PE 36B slot (GfxStage_Parse@4E6C50 / InitTexSlot@4E6BF0)
};

struct AsyncLoadCreateVb {
  int32_t type;       // payload[0] / mat+4
  int32_t flags;      // CreateVB v16 = payload[1]|1|a3; mat+0x10 = v16&~1
  int32_t stage_n;    // payload[2]
  int32_t tex_count;  // GetUvTexInfo a2[1]
  int32_t uv_info0;   // GetUvTexInfo a2[0] (65 / 449)
  int32_t stage_cap;  // PE mat+0x20 (type0 = 6)
  AsyncLoadGfxStage stages[6];  // PE mat+0x1C type0 blob 216B
  int32_t uv_slot[3][5];        // GetUvTexInfo a2[2..] soft xform table
};

struct AsyncLoadCreateVbWrap {
  AsyncLoadCreateVb* cvb;     // +0 PE CreateVB*
  AsyncLoadMatObj** mats;     // +4 MatVec ptrs
  int32_t mat_count;          // +8
  int32_t mat_cap;            // +C
};

struct AsyncLoadJob {
  AsyncLoadVtbl* vtbl = nullptr;  // [0]
  AsyncLoadJob* next = nullptr;   // [1]
  AsyncLoadJob* prev = nullptr;   // [2]
  void* pad3 = nullptr;           // [3]
  char* path = nullptr;           // [4]
  int32_t fa_status = -1;         // [5] slot idx / -1 after cb
  float pe_f18 = 0.f;             // [6] +0x18 — Type2_ctor←65C254; Process EMA
  void* field1c = nullptr;        // [7] +0x1C → PumpLastField1C
  uint32_t flags = 0;             // [8] +0x20
  void* arg_a4 = nullptr;         // [9] +0x24 — Type3 bone-opt dword
  void* buf = nullptr;            // [10] +0x28 — Process create a2
  int32_t nbytes = 0;             // [11] +0x2C — Process create a3
  int32_t field12 = 1;            // [12] JobBase = 1
  int32_t frame_id = 0;           // [13] +0x34 — PE dword_6200A4 stamp
  int32_t mip_weight = 0;         // [14] Type0 success: SumMipBppWeight
  uintptr_t type = 0;             // [15] +0x3C
  void* arg_a5 = nullptr;         // [16]
  void* arg_a6 = nullptr;         // [17]
  float field18 = 1.f;            // [18] +0x48 Type0/1/2_ctor = 1.0
  void* gfx_tex = nullptr;        // [19] +0x4C Type0/1/2_Process create
  void* pad20 = nullptr;          // [20]
  int32_t tex_format = 0;         // [21] dword_6188B8; Type2 may overwrite
  // W29A — soft PE Type2_Process @ 0x507840 state (named; host layout).
  const void* t2_hdr = nullptr;       // PE +0x68
  int32_t t2_orig_levels = 0;         // PE +0x6C
  int32_t t2_full_w = 0;              // PE +0x70 pre-clamp hdr w
  int32_t t2_full_h = 0;              // PE +0x74 pre-clamp hdr h
  int32_t t2_bpp = 0;                 // PE +0x78 BppFromFourcc
  int32_t t2_levels = 0;              // PE +0x7C create gate / clamped
  int32_t t2_done = 0;                // PE +0x80 mips finished
  const uint8_t* t2_payload = nullptr;// PE +0x84 mip payload base
  int32_t t2_begin_armed = 0;         // PE +0x88
  int32_t t2_poll_armed = 0;          // PE +0x8C
  int32_t t2_levels_store = 0;        // PE +0x90 (ctor=1; create←levels)
  uint32_t t2_fourcc = 0;             // host cache desc+8 for begin_mip_step
  // Type3 mesh extras (PE Type34 272B; host packs after Type2 soft):
  int32_t mesh_bon_fa = -1;       // PE +0xF0 FileAsync slot / -1
  int32_t mesh_bon_ready = 0;     // PE +0xF4 BonOnFileCb done
  void* mesh_bon_buf = nullptr;   // PE +0xF8
  int32_t mesh_bon_nbytes = 0;    // PE +0xFC
  int32_t mesh_bon_pending = 0;   // PE +0x100 async .bon in-flight
  // W28A — PE ParseInvoChunks AABB / bsphere (soft named; not PE offsetof).
  float mesh_aabb_min[3] = {};    // PE +0xCC/+0xD0/+0xD4
  float mesh_aabb_max[3] = {};    // PE +0xD8/+0xDC/+0xE0
  int32_t mesh_vert_hi = 0;       // PE +0xE4 max vert count seen
  int32_t mesh_aabb_init = 1;     // PE +0xEC — 1 until first scaled pos
  // PE +0x54..+0x88 bsphere/box after chunk walk (ParseInvoChunks LABEL_40):
  int32_t mesh_bound_kind = 0;    // PE +0x54
  float mesh_origin[3] = {};      // PE +0x58/+0x5C/+0x60
  float mesh_radius[3] = {};      // PE +0x64/+0x68/+0x6C (r*0.1)
  int32_t mesh_bound_flag = 0;    // PE +0x70
  float mesh_center[3] = {};      // PE +0x74/+0x78/+0x7C
  float mesh_half[3] = {};        // PE +0x80/+0x84/+0x88
  // W30A/W32A soft MatVec — PE on case0 wrap+4; job.mesh_mats = no-wrap
  // fallback + ownership mirror for mats pushed before any case0.
  AsyncLoadMatObj* mesh_cur_mat;   // last case1-3 → UploadVbFvf a3
  AsyncLoadMatObj** mesh_mats;     // soft fallback MatVec ptr[]
  int32_t mesh_mat_count;          // fallback MatVec +4
  int32_t mesh_mat_cap;            // fallback MatVec +8
  // W32A soft PtrVec of CreateVB wraps (PE job+0x48/+0x4C via PushGrow).
  AsyncLoadCreateVbWrap** mesh_cvb_wraps;
  int32_t mesh_cvb_wrap_count;
  int32_t mesh_cvb_wrap_cap;
  AsyncLoadCreateVbWrap* mesh_cur_cvb_wrap;  // last case0 = UploadVbFvf a2
};

struct AsyncLoadList {
  AsyncLoadJob* head = nullptr;  // first real node (nullptr = empty)
  AsyncLoadJob* tail = nullptr;  // last real node
  AsyncLoadJob sent{};           // sentinel tip; nodes->next = &sent
  int32_t count = 0;
};

AsyncLoadList g_asyncload_overflow;  // PE Overflow @ 0x65C1C0 / count@65C200
AsyncLoadList g_asyncload_inflight;  // PE InFlight @ 0x65BD88 / count@65C204
AsyncLoadList g_asyncload_ready;     // PE Ready @ 0x65C1A0 / count@65C208
AsyncLoadList g_asyncload_work;      // PE Work @ 0x65BD60 / SentNext@65BD68 / Tail@65BD70
AsyncLoadList g_asyncload_fail;      // PE Fail @ 0x65C1E8 / SentNext@65C1F0 / Tail@65C1F8
AsyncLoadJob* g_asyncload_pump_cursor = nullptr;  // PE @ 0x65C1D8
int32_t g_asyncload_pump_saw_type012 = 0;  // PE AsyncLoad_PumpSawType012 @ 0x65C20C
int32_t g_asyncload_pump_hold_repass = 1;   // PE AsyncLoad_PumpHoldRepass @ 0x618838
void* g_asyncload_pump_last_field1c = nullptr;  // PE @ 0x65C1B4
// Host stand-in for PE dword_6200A4 (frame stamp in Type0/1/2 Process).

// PE AsyncLoad_TexBudget_CountBytes @ 0x65C230 (lo=tex count, hi=weight sum).
int32_t g_asyncload_tex_budget_count = 0;
int32_t g_asyncload_tex_budget_weight = 0;
// PE AsyncLoad_TexLargeAvgEMA @ 0x65C240.
float g_asyncload_tex_large_ema = 0.f;
// PE dword_65C254 — Type2_ctor stamps job+0x18; Process blends *0.5.
float g_asyncload_t2_peer_ema = 0.f;
// PE AsyncLoad_MeshIsFontsPath @ 0x65BD3C — Type3/4 Process path scan.
int32_t g_asyncload_mesh_is_fonts_path = 0;

void asyncload_job_dtor(AsyncLoadJob* self, int /*flags*/) {
  if (!self) return;
  if (self->gfx_tex) {
    render_d3d9_texture_destroy(self->gfx_tex);
    self->gfx_tex = nullptr;
  }
  // W27A — VB/IB keyed by job* (ParseInvoChunks case4/5).
  render_d3d9_mesh_vb_destroy(self);
  render_d3d9_mesh_ib_destroy(self);
  // W32A — soft case0 CreateVB wraps (own MatVec mats when branched).
  if (self->mesh_cvb_wraps) {
    for (int32_t wi = 0; wi < self->mesh_cvb_wrap_count; ++wi) {
      AsyncLoadCreateVbWrap* w = self->mesh_cvb_wraps[wi];
      if (!w) continue;
      if (w->mats) {
        for (int32_t mi = 0; mi < w->mat_count; ++mi) {
          AsyncLoadMatObj* m = w->mats[mi];
          if (!m) continue;
          std::free(m->tex_ids);
          std::free(m);
        }
        std::free(w->mats);
      }
      std::free(w->cvb);
      std::free(w);
    }
    std::free(self->mesh_cvb_wraps);
    self->mesh_cvb_wraps = nullptr;
    self->mesh_cvb_wrap_count = 0;
    self->mesh_cvb_wrap_cap = 0;
    self->mesh_cur_cvb_wrap = nullptr;
  }
  // W30A — soft case1-3 MatObj fallback (no case0 wrap yet).
  if (self->mesh_mats) {
    for (int32_t i = 0; i < self->mesh_mat_count; ++i) {
      AsyncLoadMatObj* m = self->mesh_mats[i];
      if (!m) continue;
      std::free(m->tex_ids);
      std::free(m);
    }
    std::free(self->mesh_mats);
    self->mesh_mats = nullptr;
    self->mesh_mat_count = 0;
    self->mesh_mat_cap = 0;
  }
  self->mesh_cur_mat = nullptr;
  if (self->mesh_bon_buf) {
    std::free(self->mesh_bon_buf);
    self->mesh_bon_buf = nullptr;
  }
  if (self->path) {
    std::free(self->path);
    self->path = nullptr;
  }
  std::free(self);
}

void asyncload_type012_pump(AsyncLoadJob* self);
void asyncload_type3_pump(AsyncLoadJob* self);
void asyncload_type4_pump(AsyncLoadJob* self);
void asyncload_type5_pump(AsyncLoadJob* self);

AsyncLoadVtbl g_asyncload_vtbl_012 = {asyncload_job_dtor, asyncload_type012_pump};
AsyncLoadVtbl g_asyncload_vtbl_3 = {asyncload_job_dtor, asyncload_type3_pump};
AsyncLoadVtbl g_asyncload_vtbl_4 = {asyncload_job_dtor, asyncload_type4_pump};
AsyncLoadVtbl g_asyncload_vtbl_5 = {asyncload_job_dtor, asyncload_type5_pump};

void asyncload_list_push_tail(AsyncLoadList& L, AsyncLoadJob* n) {
  // PE Submit/OnFileCb: *(tail+4)=n; n->prev=old_tail; n->next=&SentNext;
  // Tail=n; ++count. First push overwrites Head via TailSent+4 adjacency.
  n->next = &L.sent;
  n->prev = L.tail;
  if (L.tail) {
    L.tail->next = n;
  } else {
    L.head = n;
  }
  L.tail = n;
  ++L.count;
}

void asyncload_list_push_head(AsyncLoadList& L, AsyncLoadJob* n) {
  // PE Type012/3/4 Ready requeue when prev_ok==0: ReadyHead = n.
  n->prev = nullptr;
  n->next = L.head ? L.head : &L.sent;
  if (L.head) {
    L.head->prev = n;
  } else {
    L.tail = n;
  }
  L.head = n;
  ++L.count;
}

void asyncload_list_insert_after(AsyncLoadList& L, AsyncLoadJob* after,
                                 AsyncLoadJob* n) {
  // PE Type012/3/4: splice after saved prev (v2).
  AsyncLoadJob* nxt = after->next;
  n->next = nxt;
  n->prev = after;
  after->next = n;
  if (nxt && nxt != &L.sent) {
    nxt->prev = n;
  } else {
    L.tail = n;
  }
  ++L.count;
}

void asyncload_list_unlink(AsyncLoadList& L, AsyncLoadJob* n) {
  // PE OnFileCb / HasWork / Type3_Pump@501A70: next->prev=prev; prev->next=next.
  AsyncLoadJob* nxt = n->next;
  AsyncLoadJob* prv = n->prev;
  if (prv) prv->next = nxt;
  if (nxt && nxt != &L.sent) nxt->prev = prv;
  if (L.head == n) L.head = (nxt && nxt != &L.sent) ? nxt : nullptr;
  if (L.tail == n) L.tail = prv;
  n->next = nullptr;
  n->prev = nullptr;
}

bool asyncload_list_contains(const AsyncLoadList& L, const AsyncLoadJob* n) {
  for (AsyncLoadJob* p = L.head; p;) {
    if (p == n) return true;
    AsyncLoadJob* nx = p->next;
    p = (nx && nx != &L.sent) ? nx : nullptr;
  }
  return false;
}

// PE Type0/3/4 success flag side-effect @ Process:
//   flags = flags & 0xFFFFFFE9 | 0x14; [13] = dword_6200A4.
void asyncload_process_flag_014(AsyncLoadJob* self) {
  self->flags = (self->flags & 0xFFFFFFE9u) | 0x14u;
  self->frame_id = g_asyncload_frame;
}

// PE Type5_Process@53BB90: flags = flags & 0xFFFFFFF1 | 0xC (+ sub_55B490 OOS).
void asyncload_process_flag_00c(AsyncLoadJob* self) {
  self->flags = (self->flags & 0xFFFFFFF1u) | 0xCu;
}

// PE AsyncLoad_Type0_Process @ 0x506650 (vtbl+16).
// W31A: call site @ 0x506671 → GfxEngine+0x204 device vt+0xF0
//   GfxDevice_CreateTextureFromMem @ 0x4F8A40 → Body@4F7AE0
//   (buf=[10], n=[11], fmt=[21]/dword_6188B8, a5=0) → [19].
//   a5==0 → MipLevels=1 (Body v6=a5==0 → D3DX a6).
// Host: render_d3d9_texture_create_from_mem(..., tex_format, a5=0)
//   keyed by job* (PE returns GfxTexture wrap).
// Callees: GetLevel0WH@4F6000, SumMipBppWeight@4F6230 (soft mips sum).
void asyncload_type0_process(AsyncLoadJob* self) {
  if (!self) return;
  if (!self->buf || self->nbytes <= 0) {
    self->flags = (self->flags & 0x7FFFFFF9u) | 0x80000004u;
    return;
  }
  // PE create key = GfxTexture wrap; host key = job (stable until dtor).
  void* key = self;
  // PE a4 = *(this+21) = dword_6188B8 (ctor); a5 = 0 (Type0 only).
  const bool uploaded = render_d3d9_texture_create_from_mem(
      key, static_cast<const uint8_t*>(self->buf),
      static_cast<size_t>(self->nbytes), self->tex_format, /*a5=*/0,
      self->path ? self->path : "AsyncLoad0");
  const int32_t w = render_d3d9_texture_width(key);
  const int32_t h = render_d3d9_texture_height(key);
  // PE: *(this+19)=v2; if (!v2) flags|0x80000004.
  if (!uploaded) {
    render_d3d9_texture_destroy(key);
    self->gfx_tex = nullptr;
    self->flags = (self->flags & 0x7FFFFFF9u) | 0x80000004u;
    return;
  }
  self->gfx_tex = key;
  // Soft SumMipBppWeight@4F6230: sum bpp-class per mip (DXT1=0.5→ftol).
  // Host: count mips (PE walks GetLevelCount); exact FOURCC table OOS.
  const int32_t mips = render_d3d9_texture_mips(key);
  const int32_t weight = mips > 0 ? mips : 1;
  self->mip_weight = weight;
  ++g_asyncload_tex_budget_count;
  g_asyncload_tex_budget_weight += weight;
  if (w >= 256 && h >= 256) {
    // PE: (1/area_kb)*0.05 + EMA*0.95 — Hex-Rays showed 0.0/area; use 1/area.
    const float area_kb =
        static_cast<float>(w) * static_cast<float>(h) * 0.0009765625f;
    if (area_kb > 0.f) {
      g_asyncload_tex_large_ema =
          (1.f / area_kb) * 0.05f + g_asyncload_tex_large_ema * 0.95f;
    }
  }
  asyncload_process_flag_014(self);
}

// PE AsyncLoad_Type1_Process @ 0x507110 (vtbl+16).
// Slice W26A: hdr levels/w/h @ buf+4/+8/+12; clamp via dword_6188B4;
//   CreateTexture vt+0xEC → render_d3d9_texture_create_dims(fmt=[21]);
//   fail flags|0x80000004; success budget+SumMipBppWeight+flag|0x14.
// OOS: BeginMipDecode@506AD0 / StepMipUpload@506C70 (JPEG decode +
//   LockRect upload machine).
void asyncload_type1_process(AsyncLoadJob* self) {
  if (!self) return;
  if (!self->buf || self->nbytes < 16) {
    self->flags = (self->flags & 0x7FFFFFF9u) | 0x80000004u;
    return;
  }
  const auto* hdr = static_cast<const int32_t*>(self->buf);
  int32_t levels = hdr[1];
  int32_t w = hdr[2];
  int32_t h = hdr[3];
  if (levels < 1 || w <= 0 || h <= 0) {
    self->flags = (self->flags & 0x7FFFFFF9u) | 0x80000004u;
    return;
  }
  levels = render_d3d9_texture_clamp_async_mips(
      &w, &h, levels, g_engine_texture_size, /*floor4=*/false);
  void* key = self;
  const bool uploaded = render_d3d9_texture_create_dims(
      key, w, h, levels, self->tex_format,
      self->path ? self->path : "AsyncLoad1");
  if (!uploaded) {
    render_d3d9_texture_destroy(key);
    self->gfx_tex = nullptr;
    self->flags = (self->flags & 0x7FFFFFF9u) | 0x80000004u;
    return;
  }
  self->gfx_tex = key;
  const int32_t weight = render_d3d9_texture_sum_mip_bpp_weight(key);
  self->mip_weight = weight > 0 ? weight : 1;
  ++g_asyncload_tex_budget_count;
  g_asyncload_tex_budget_weight += self->mip_weight;
  // PE: mip decode/upload loop then first-mip |0x10 / done &~2|4.
  // Host: skip upload machine → success flag like Type0.
  asyncload_process_flag_014(self);
}

// Soft PE Type2_Process LABEL_35 @ 0x507ADC — bump done; first-mip |0x10;
// all mips → flags&~2|4, clear payload.
void asyncload_type2_advance_mip(AsyncLoadJob* self) {
  if (!self) return;
  const int32_t done = self->t2_done + 1;
  self->t2_done = done;
  if (done == 1) {
    self->flags |= 0x10u;
    self->frame_id = g_asyncload_frame;
  }
  if (self->t2_levels == done) {
    // PE @ 0x507B0B: flags = flags & 0xFFFFFFF9 | 4 (clear bit1=Process).
    self->flags = (self->flags & 0xFFFFFFF9u) | 4u;
    self->t2_payload = nullptr;
  }
}

// PE AsyncLoad_Type2_Process @ 0x507840 (vtbl+16).
// W29A: create gate (+0x7C==0) → FormatFromFourcc + clamp floor4 +
//   create_dims; then W28B type2_begin_mip_step + Poll + Finish loop.
// Helpers: bpp_from_fourcc / rows_budget / payload_offset / level_index /
//   finish_lod / type2_begin_mip_step → Begin@4F6880 Poll@4F68D0
//   Finish@4F5DF0 (in render_d3d9).
// OOS: GetLevel0WH device path (host width/height); dword_65C254 peer
//   fidelity; D3DX LoadSurfaceFromMemory@5912E8 inside Poll.
void asyncload_type2_process(AsyncLoadJob* self) {
  if (!self) return;

  // PE create gate: *(this+0x7C) == 0 @ 0x507850.
  if (self->t2_levels == 0) {
    // PE: fourcc at (hdr+19)+8 → need ≥88 bytes of header/desc.
    if (!self->buf || self->nbytes < 88) {
      self->flags = (self->flags & 0x7FFFFFF9u) | 0x80000004u;
      return;
    }
    const auto* hdr = static_cast<const int32_t*>(self->buf);
    int32_t levels = hdr[7];
    if (levels == 0) levels = 1;
    int32_t w = hdr[4];
    int32_t h = hdr[3];
    if (levels < 1 || w <= 0 || h <= 0) {
      self->flags = (self->flags & 0x7FFFFFF9u) | 0x80000004u;
      return;
    }
    self->t2_hdr = hdr;
    self->t2_done = 0;
    // PE @ 0x50787F: payload = hdr + hdr[1] + 4.
    self->t2_payload =
        reinterpret_cast<const uint8_t*>(hdr) + hdr[1] + 4;
    self->t2_full_w = w;
    self->t2_full_h = h;
    self->t2_orig_levels = levels;

    // PE: desc = hdr+19; Bpp/FormatFromFourcc(*(desc+8)).
    const auto* desc_bytes =
        reinterpret_cast<const uint8_t*>(hdr) + 19 * 4;
    const uint32_t fourcc =
        *reinterpret_cast<const uint32_t*>(desc_bytes + 8);
    self->t2_fourcc = fourcc;
    self->t2_bpp = render_d3d9_texture_bpp_from_fourcc(fourcc);
    const int32_t engine_fmt =
        render_d3d9_texture_format_from_fourcc(fourcc);
    self->tex_format = engine_fmt;

    int32_t create_w = w;
    int32_t create_h = h;
    levels = render_d3d9_texture_clamp_async_mips(
        &create_w, &create_h, levels, g_engine_texture_size,
        /*floor4=*/true);
    // PE writes clamped levels to +0x7C before Create; fail clears it.
    self->t2_levels = levels;
    self->t2_levels_store = levels;  // PE @ 0x5078EA

    void* key = self;
    const bool uploaded = render_d3d9_texture_create_dims(
        key, create_w, create_h, levels, engine_fmt,
        self->path ? self->path : "AsyncLoad2");
    if (!uploaded) {
      render_d3d9_texture_destroy(key);
      self->gfx_tex = nullptr;
      self->t2_payload = nullptr;
      self->t2_levels = 0;
      self->t2_done = 0;
      self->flags = (self->flags & 0x7FFFFFF9u) | 0x80000004u;
      return;
    }
    self->gfx_tex = key;
    const int32_t weight = render_d3d9_texture_sum_mip_bpp_weight(key);
    self->mip_weight = weight > 0 ? weight : 1;
    ++g_asyncload_tex_budget_count;
    g_asyncload_tex_budget_weight += self->mip_weight;
    // PE: fall through to Begin — do not set |0x14 yet.
  }

  void* key = self->gfx_tex ? self->gfx_tex : static_cast<void*>(self);

  // PE Begin gate: *(this+0x88) == 0 @ 0x50797B.
  if (self->t2_begin_armed == 0) {
    const int32_t level0_w = render_d3d9_texture_width(key);
    const int32_t level0_h = render_d3d9_texture_height(key);
    self->t2_begin_armed = 1;
    const bool began =
        self->t2_payload != nullptr &&
        render_d3d9_texture_type2_begin_mip_step(
            key, self->t2_levels, self->t2_done, self->t2_orig_levels,
            level0_w, level0_h, self->t2_full_w, self->t2_full_h,
            self->t2_payload, self->t2_fourcc);
    self->t2_poll_armed = began ? 1 : 0;
    if (!began) {
      // Soft: Begin miss → advance (PE Hex-Rays LABEL_35 if +0x88 cleared).
      self->t2_begin_armed = 0;
      asyncload_type2_advance_mip(self);
      return;
    }
  }

  // PE @ 0x507A8D: +0x8C != 0 && PollAsyncMipUpload == 0 → Finish + advance.
  // Host Poll: true = more rows (PE 1); false = level done (PE 0).
  if (self->t2_poll_armed != 0 &&
      !render_d3d9_texture_poll_async_mip_upload(key)) {
    const int32_t lod = render_d3d9_texture_async_mip_finish_lod(
        self->t2_levels, self->t2_done, self->t2_levels_store);
    render_d3d9_texture_finish_async_mip_upload(key, lod);
    // PE @ 0x507ACA: job+0x18 = (dword_65C254 + job+0x18) * 0.5
    self->pe_f18 = (g_asyncload_t2_peer_ema + self->pe_f18) * 0.5f;
    self->t2_begin_armed = 0;
    asyncload_type2_advance_mip(self);
  }
}

// PE: mesh path → replace last 4 chars with ".bon" (assumes ".xxx").
bool asyncload_mesh_bon_path(char* dst, size_t dst_cap, const char* mesh_path) {
  if (!dst || dst_cap < 5 || !mesh_path || !mesh_path[0]) return false;
  const size_t n = std::strlen(mesh_path);
  if (n < 4 || n >= dst_cap) return false;
  std::memcpy(dst, mesh_path, n + 1);
  dst[n - 4] = '.';
  dst[n - 3] = 'b';
  dst[n - 2] = 'o';
  dst[n - 1] = 'n';
  return true;
}

// ---------------------------------------------------------------------------
// W28A — soft PE GfxVbLayout_FromFvf @ 0x4FE980 field offs +
// GfxVb_PackVertex @ 0x4FF710 + UploadVbFvf@503400 vert walk / AABB.
// Layout lives at PE wrapper+0x1C; PackVertex this = full wrapper
// (count@+4 blob@+8 flags@+12 layout@+0x1C stride@+0x60).
// ---------------------------------------------------------------------------
struct GfxVbLayoutSoft {
  int32_t pos_off = 0;     // layout[0]
  int32_t nrm_off = 0;     // layout[1]
  int32_t dif_off = 0;     // layout[2]
  int32_t spc_off = 0;     // layout[3]
  int32_t uv_off[8] = {};  // layout[4..11]
  int32_t blend_off[7] = {};  // layout[12..] — PE writes (>>8)&7 slots
  int32_t psize_off = 0;   // layout[16]
  int32_t stride = 0;      // layout[17]
};

// Soft PE GfxVbLayout_FromFvf @ 0x4FE980 — full offset table (W26B had
// stride-only in render_d3d9).
GfxVbLayoutSoft gfxvb_layout_from_fvf(uint32_t pe_fvf) {
  GfxVbLayoutSoft L{};
  if ((pe_fvf & 1u) != 0) {
    const int32_t blends = static_cast<int32_t>((pe_fvf >> 8) & 7u);
    L.stride = 12;  // XYZ @ off 0 (layout[0] left 0 after clear)
    for (int32_t i = 0; i < blends && i < 7; ++i) {
      L.blend_off[i] = L.stride;
      L.stride += 4;
    }
  }
  if ((pe_fvf & 0x800u) != 0) {
    L.psize_off = L.stride;
    L.stride += 4;
  }
  if ((pe_fvf & 2u) != 0) {
    L.nrm_off = L.stride;
    L.stride += 12;
  }
  if ((pe_fvf & 4u) != 0) {
    L.dif_off = L.stride;
    L.stride += 4;
  }
  if ((pe_fvf & 8u) != 0) {
    L.spc_off = L.stride;
    L.stride += 4;
  }
  const int32_t tex_n = static_cast<int32_t>((pe_fvf >> 4) & 0xFu);
  int32_t bit = 16;
  for (int32_t t = 0; t < tex_n && t < 8; ++t, bit += 2) {
    L.uv_off[t] = L.stride;
    switch ((pe_fvf >> bit) & 3u) {
      case 0:
        L.stride += 4;
        break;
      case 1:
        L.stride += 8;
        break;
      case 2:
        L.stride += 12;
        break;
      case 3:
        L.stride += 16;
        break;
      default:
        break;
    }
  }
  return L;
}

// Soft PE scratch @ 0x65BC68.. (IDA: GfxVb_Scratch*) — one vert.
struct GfxVbScratch {
  float pos[3] = {};
  float nrm[3] = {};
  uint32_t diffuse = 0;
  uint32_t specular = 0;
  float uv[8][4] = {};  // PackVertex UV cases 0..3 use 1..4 floats/slot
  float blend[7] = {};
  uint32_t psize = 0;
};

// Soft PE GfxVb_PackVertex @ 0x4FF710 — pack scratch into blob[i]*stride.
void gfxvb_pack_vertex(uint8_t* blob, int32_t vert_count, int32_t stride,
                       uint32_t pe_fvf, const GfxVbLayoutSoft& L,
                       unsigned idx, const GfxVbScratch& S) {
  if (!blob || idx >= static_cast<unsigned>(vert_count) || stride <= 0) return;
  uint8_t* dst = blob + static_cast<size_t>(idx) * static_cast<size_t>(stride);
  if ((pe_fvf & 1u) != 0) {
    std::memcpy(dst + L.pos_off, S.pos, 12);
    const int32_t blends = static_cast<int32_t>((pe_fvf >> 8) & 7u);
    for (int32_t i = 0; i < blends && i < 7; ++i) {
      uint32_t bits = 0;
      std::memcpy(&bits, &S.blend[i], 4);
      std::memcpy(dst + L.blend_off[i], &bits, 4);
    }
  }
  if ((pe_fvf & 0x800u) != 0)
    std::memcpy(dst + L.psize_off, &S.psize, 4);
  if ((pe_fvf & 2u) != 0) std::memcpy(dst + L.nrm_off, S.nrm, 12);
  if ((pe_fvf & 4u) != 0) std::memcpy(dst + L.dif_off, &S.diffuse, 4);
  if ((pe_fvf & 8u) != 0) std::memcpy(dst + L.spc_off, &S.specular, 4);
  const int32_t tex_n = static_cast<int32_t>((pe_fvf >> 4) & 0xFu);
  int32_t bit = 16;
  for (int32_t t = 0; t < tex_n && t < 8; ++t, bit += 2) {
    auto* out = reinterpret_cast<uint32_t*>(dst + L.uv_off[t]);
    const float* u = S.uv[t];
    switch ((pe_fvf >> bit) & 3u) {
      case 0:
        std::memcpy(out, u, 4);
        break;
      case 1:
        std::memcpy(out, u, 8);
        break;
      case 2:
        std::memcpy(out, u, 12);
        break;
      case 3:
        std::memcpy(out, u, 16);
        break;
      default:
        break;
    }
  }
}

void asyncload_mesh_aabb_accum(AsyncLoadJob* self, float x, float y, float z) {
  if (!self) return;
  if (self->mesh_aabb_init != 0) {
    self->mesh_aabb_min[0] = self->mesh_aabb_max[0] = x;
    self->mesh_aabb_min[1] = self->mesh_aabb_max[1] = y;
    self->mesh_aabb_min[2] = self->mesh_aabb_max[2] = z;
    self->mesh_aabb_init = 0;
    return;
  }
  if (x < self->mesh_aabb_min[0]) self->mesh_aabb_min[0] = x;
  if (y < self->mesh_aabb_min[1]) self->mesh_aabb_min[1] = y;
  if (z < self->mesh_aabb_min[2]) self->mesh_aabb_min[2] = z;
  if (x > self->mesh_aabb_max[0]) self->mesh_aabb_max[0] = x;
  if (y > self->mesh_aabb_max[1]) self->mesh_aabb_max[1] = y;
  if (z > self->mesh_aabb_max[2]) self->mesh_aabb_max[2] = z;
}

// Soft PE ParseInvoChunks LABEL_40 @ 0x502F.. — radius/center/half from AABB.
void asyncload_mesh_aabb_finalize(AsyncLoadJob* self) {
  if (!self) return;
  if (self->mesh_aabb_init == 1) {
    // PE: no verts → zero bounds, kind/flag = 2.
    self->mesh_origin[0] = self->mesh_origin[1] = self->mesh_origin[2] = 0.f;
    self->mesh_radius[0] = self->mesh_radius[1] = self->mesh_radius[2] = 0.f;
    self->mesh_center[0] = self->mesh_center[1] = self->mesh_center[2] = 0.f;
    self->mesh_half[0] = self->mesh_half[1] = self->mesh_half[2] = 0.f;
    self->mesh_bound_kind = 2;
    self->mesh_bound_flag = 2;
    return;
  }
  const float* mn = self->mesh_aabb_min;
  const float* mx = self->mesh_aabb_max;
  const float min2 = mn[0] * mn[0] + mn[1] * mn[1] + mn[2] * mn[2];
  const float max2 = mx[0] * mx[0] + mx[1] * mx[1] + mx[2] * mx[2];
  const float r =
      (max2 >= min2)
          ? static_cast<float>(std::sqrt(static_cast<double>(max2)))
          : static_cast<float>(std::sqrt(static_cast<double>(min2)));
  const float r01 = r * 0.1f;
  self->mesh_origin[0] = self->mesh_origin[1] = self->mesh_origin[2] = 0.f;
  self->mesh_radius[0] = self->mesh_radius[1] = self->mesh_radius[2] = r01;
  self->mesh_bound_kind = 0;
  // PE: center = (min+max)*0.05 ; half = fabs((max-min)*0.05)
  self->mesh_center[0] = (mn[0] + mx[0]) * 0.05f;
  self->mesh_center[1] = (mn[1] + mx[1]) * 0.05f;
  self->mesh_center[2] = (mn[2] + mx[2]) * 0.05f;
  self->mesh_half[0] =
      static_cast<float>(std::fabs(static_cast<double>((mx[0] - mn[0]) * 0.05f)));
  self->mesh_half[1] =
      static_cast<float>(std::fabs(static_cast<double>((mx[1] - mn[1]) * 0.05f)));
  self->mesh_half[2] =
      static_cast<float>(std::fabs(static_cast<double>((mx[2] - mn[2]) * 0.05f)));
  self->mesh_bound_flag = 1;
}

// Soft PE UploadVbFvf @ 0x503788 — uv_slot xform → ScratchUv before PackVertex.
// Source channel offs from invo_flags bits (0x200<<i); slot[0]==-1 → pos XZ.
void asyncload_mesh_apply_uv_slots(GfxVbScratch& S, const float* uv_base,
                                   const float* uv_end, uint32_t invo_flags,
                                   const AsyncLoadCreateVb* cvb) {
  if (!cvb || !uv_base || !uv_end) return;
  int32_t uv_off[8] = {};
  int32_t acc = 0;
  for (int i = 0; i < 8; ++i) {
    uv_off[i] = acc;
    if ((invo_flags & (512u << i)) != 0) acc += 8;
  }
  int32_t n = cvb->tex_count;
  if (n > 4) n = 4;  // PE min(tex_count, 4)
  if (n > 3) n = 3;  // host GetUvTexInfo slot table
  for (int32_t t = 0; t < n; ++t) {
    const int32_t* slot = cvb->uv_slot[t];
    float v25 = 0.f;
    float v34 = 0.f;
    if (slot[0] == -1) {
      v25 = S.pos[0] - 0.5f;
      v34 = -S.pos[2] - 0.5f;
    } else {
      int32_t ch = slot[0];
      if (ch < 0 || ch > 7) ch = 0;
      const auto* uv = reinterpret_cast<const float*>(
          reinterpret_cast<const uint8_t*>(uv_base) +
          static_cast<size_t>(uv_off[ch]));
      if (uv + 2 > uv_end) break;
      v25 = uv[0] - 0.5f;
      v34 = 0.5f - uv[1];  // (1.0 - v) - 0.5
    }
    const float scaleU = reinterpret_cast<const float&>(slot[1]);
    const float scaleV = reinterpret_cast<const float&>(slot[2]);
    const float offU = reinterpret_cast<const float&>(slot[3]);
    const float offV = reinterpret_cast<const float&>(slot[4]);
    S.uv[t][0] = (v25 - offU) * scaleU + 0.5f;
    S.uv[t][1] = 0.5f - (v34 - offV) * scaleV;
  }
}

// Soft PE AsyncLoad_Mesh_UploadVbFvf @ 0x503400 vert walk (PackVertex +
// AABB). W32A/W33A: tex_count ← CreateVB GetUvTexInfo (soft wrap).
// W34-01: uv_slot xform @ 0x503788 when cvb present; else raw UV copy.
bool asyncload_mesh_upload_vb_pack(AsyncLoadJob* self, void* key,
                                   int32_t vert_count, uint32_t invo_flags,
                                   int32_t tex_count, const uint8_t* verts,
                                   uint32_t vert_bytes,
                                   const AsyncLoadCreateVb* cvb) {
  if (!self || !key || !verts || vert_count <= 0) return false;
  if (self->mesh_vert_hi < vert_count) self->mesh_vert_hi = vert_count;

  const uint32_t pe_fvf =
      render_d3d9_mesh_invo_flags_to_fvf(invo_flags, tex_count);
  const GfxVbLayoutSoft L = gfxvb_layout_from_fvf(pe_fvf);
  if (L.stride <= 0) return false;
  const uint32_t src_stride =
      vert_bytes / static_cast<uint32_t>(vert_count);
  if (src_stride == 0u) return false;

  std::vector<uint8_t> packed(
      static_cast<size_t>(vert_count) * static_cast<size_t>(L.stride), 0);

  for (int32_t vi = 0; vi < vert_count; ++vi) {
    const auto* src = reinterpret_cast<const float*>(
        verts + static_cast<size_t>(vi) * static_cast<size_t>(src_stride));
    const float* end = reinterpret_cast<const float*>(
        verts + static_cast<size_t>(vi) * static_cast<size_t>(src_stride) +
        static_cast<size_t>(src_stride));
    const float* p = src;
    GfxVbScratch S{};
    // PE defaults blend = (1,0,0) @ 0x5036C6 before flag reads.
    S.blend[0] = 1.f;
    S.blend[1] = 0.f;
    S.blend[2] = 0.f;
    uint32_t dif = 0xFFFFFFFFu;
    uint32_t spc = 0xFFFFFFFFu;

    if ((invo_flags & 1u) != 0) {
      if (p + 3 > end) break;
      const float x = p[0] * 0.1f;
      const float y = p[1] * 0.1f;
      const float z = p[2] * 0.1f;
      S.pos[0] = x;
      S.pos[1] = y;
      S.pos[2] = z;
      asyncload_mesh_aabb_accum(self, x, y, z);
      p += 3;
    }
    if ((invo_flags & 4u) != 0) {
      if (p + 1 > end) break;
      S.blend[0] = *p++;
    } else if ((invo_flags & 8u) != 0) {
      if (p + 2 > end) break;
      S.blend[0] = p[0];
      S.blend[1] = p[1];
      p += 2;
    } else if ((invo_flags & 0x10u) != 0) {
      if (p + 3 > end) break;
      S.blend[0] = p[0];
      S.blend[1] = p[1];
      S.blend[2] = p[2];
      p += 3;
    }
    if ((invo_flags & 0x20u) != 0) {
      if (p + 1 > end) break;
      std::memcpy(&S.psize, p, 4);
      ++p;
    }
    if ((invo_flags & 0x40u) != 0) {
      if (p + 3 > end) break;
      S.nrm[0] = p[0];
      S.nrm[1] = p[1];
      S.nrm[2] = p[2];
      p += 3;
    }
    if ((invo_flags & 0x80u) != 0) {
      if (p + 1 > end) break;
      std::memcpy(&dif, p, 4);
      ++p;
    }
    if ((invo_flags & 0x100u) != 0) {
      if (p + 1 > end) break;
      std::memcpy(&spc, p, 4);
      ++p;
    }
    // PE @ 0x503788: uv_slot xform into ScratchUv (65BC90..); else raw 2D.
    if (cvb && cvb->tex_count > 0) {
      asyncload_mesh_apply_uv_slots(S, p, end, invo_flags, cvb);
    } else {
      for (int32_t t = 0; t < tex_count && t < 8; ++t) {
        if (p + 2 > end) break;
        S.uv[t][0] = p[0];
        S.uv[t][1] = p[1];
        p += 2;
      }
    }
    S.diffuse = dif | 0xFF000000u;
    S.specular = spc;
    gfxvb_pack_vertex(packed.data(), vert_count, L.stride, pe_fvf, L,
                      static_cast<unsigned>(vi), S);
  }

  return render_d3d9_mesh_vb_create(key, vert_count, pe_fvf, L.stride,
                                    packed.data(), packed.size());
}

// Soft PE GfxStage_Parse @ 0x4E6C50 — size by tag>>24; fills 9 dwords.
int32_t asyncload_gfx_stage_parse(AsyncLoadGfxStage* out, const uint8_t* src,
                                  uint32_t avail) {
  if (!out || !src || avail < 4u) return 0;
  std::memset(out, 0, sizeof(*out));
  const auto* dw = reinterpret_cast<const int32_t*>(src);
  const int32_t tag = dw[0];
  out->d[0] = tag;
  switch (tag >> 24) {
    case 0: {
      if (avail < 8u) return 0;
      const uint32_t v = static_cast<uint32_t>(dw[1]);
      out->d[1] = dw[1];
      // PE: channels as float * (1/255)
      reinterpret_cast<float&>(out->d[5]) =
          static_cast<float>((v >> 24) & 0xFFu) * (1.f / 255.f);
      reinterpret_cast<float&>(out->d[2]) =
          static_cast<float>((v >> 16) & 0xFFu) * (1.f / 255.f);
      reinterpret_cast<float&>(out->d[3]) =
          static_cast<float>((v >> 8) & 0xFFu) * (1.f / 255.f);
      reinterpret_cast<float&>(out->d[4]) =
          static_cast<float>(v & 0xFFu) * (1.f / 255.f);
      return 8;
    }
    case 1:
      if (avail < 8u) return 0;
      out->d[1] = dw[1];
      return 8;
    case 2:
      if (avail < 12u) return 0;
      out->d[1] = dw[1];
      out->d[2] = dw[2];
      return 12;
    case 3:
      if (avail < 16u) return 0;
      out->d[1] = dw[1];
      out->d[2] = dw[2];
      out->d[3] = dw[3];
      return 16;
    case 4:
      if (avail < 20u) return 0;
      out->d[1] = dw[1];
      out->d[2] = dw[2];
      out->d[3] = dw[3];
      out->d[4] = dw[3];
      return 20;
    case 5:
    case 7:
      if (avail < 8u) return 0;
      out->d[1] = dw[1];
      return 8;
    case 6:
      if (avail < 32u) return 0;
      out->d[3] = dw[1];
      out->d[2] = dw[3];
      out->d[4] = dw[2];
      out->d[5] = dw[4];
      out->d[6] = dw[5];
      out->d[7] = dw[6];
      out->d[8] = dw[7];
      out->d[1] = 0;
      return 32;
    case 8:
      if (avail < 36u) return 0;
      std::memcpy(&out->d[1], src + 4, 31);
      reinterpret_cast<uint8_t*>(&out->d[0])[35] = 0;
      return 36;
    default:
      return 4;
  }
}

// Soft PE GfxStage_InitTexSlot @ 0x4E6BF0.
void asyncload_gfx_stage_init_tex(AsyncLoadGfxStage* s, int32_t tag) {
  if (!s) return;
  std::memset(s, 0, sizeof(*s));
  s->d[0] = tag;
  s->d[2] = 3;
  s->d[3] = -1;
  reinterpret_cast<float&>(s->d[5]) = 1.f;
  reinterpret_cast<float&>(s->d[6]) = 1.f;
}

// Soft PE GfxMat_ApplyStages @ 0x4E5890 — match tag, qmemcpy 36B.
void asyncload_gfx_mat_apply_stages(AsyncLoadCreateVb* mat,
                                    const AsyncLoadGfxStage* src, int32_t nsrc) {
  if (!mat || !src || nsrc <= 0) return;
  for (int32_t si = 0; si < mat->stage_cap; ++si) {
    AsyncLoadGfxStage* dst = &mat->stages[si];
    for (int32_t i = 0; i < nsrc; ++i) {
      if (src[i].d[0] == dst->d[0]) {
        *dst = src[i];
        break;
      }
    }
  }
  // PE: flags & 0x20000000 → mat+0xC from stage tag 0x5000000 dword[1]. OOS.
}

// Soft PE GfxMat_UvSlot_CopyXform @ 0x4E6890.
// PE a2 layout: [0]=flags [1]=tex_count [2..]=slots; host uv_slot = slot body.
void asyncload_gfx_uv_slot_copy(AsyncLoadCreateVb* mat, int slot,
                                const int32_t* stage_d) {
  if (!mat || !stage_d || slot < 0 || slot > 2) return;
  for (int k = 0; k < 5; ++k) mat->uv_slot[slot][k] = stage_d[4 + k];
  mat->uv_info0 |= (512 << slot);  // PE *a2 |= 512<<slot
}

// Soft PE GfxMat_UvSlot_InitIdentity @ 0x4E6930.
void asyncload_gfx_uv_slot_identity(AsyncLoadCreateVb* mat, int slot) {
  if (!mat || slot < 0 || slot > 2) return;
  mat->uv_slot[slot][0] = -1;
  reinterpret_cast<float&>(mat->uv_slot[slot][1]) = 1.f;
  reinterpret_cast<float&>(mat->uv_slot[slot][2]) = 1.f;
  mat->uv_slot[slot][3] = 0;
  mat->uv_slot[slot][4] = 0;
  mat->uv_info0 |= (512 << slot);
}

// Soft PE GfxMat_GetUvTexInfo @ 0x4C68E0 (CreateVB/mat vt+8).
// type0 blob: [0]=0x6000007 [1]=0x6000004 [2]=0x6000005 [3]=0x6000000@+0x6C.
void asyncload_gfx_mat_get_uv_tex_info(AsyncLoadCreateVb* mat) {
  if (!mat) return;
  std::memset(mat->uv_slot, 0, sizeof(mat->uv_slot));
  // PE: *a2 = 65; if (mat+0x10 < 0) *a2 = 449.
  mat->uv_info0 = (mat->flags < 0) ? 449 : 65;
  // First UV xform from stage blob+0x6C (type0 InitTexSlot 0x6000000).
  asyncload_gfx_uv_slot_copy(mat, 0, mat->stages[3].d);
  mat->tex_count = 1;
  const int32_t tex0 = mat->stages[0].d[3];  // blob[3]
  const int32_t tex1 = mat->stages[1].d[3];  // blob[12]
  const int32_t tex2 = mat->stages[2].d[3];  // blob+84
  if (tex0 == -1) {
    if (tex1 != -1) {
      asyncload_gfx_uv_slot_copy(mat, 1, mat->stages[1].d);
      mat->tex_count = 2;
    }
    if (tex2 != -1) {
      if (mat->tex_count == 1) asyncload_gfx_uv_slot_identity(mat, 1);
      asyncload_gfx_uv_slot_copy(mat, 2, mat->stages[2].d);
      mat->tex_count = 3;
    }
  } else {
    asyncload_gfx_uv_slot_copy(mat, 1, mat->stages[0].d);
    mat->tex_count = 2;
  }
}

// Soft PE GfxDevice_CreateMatFromStages @ 0x4BEFC0 — type0 path only.
// PE CreateVB calls device vt+0xC8 after stage walk into unk_649638.
void asyncload_create_mat_from_stages_soft(AsyncLoadCreateVb* mat,
                                           const AsyncLoadGfxStage* src,
                                           int32_t nsrc, int32_t flags_clr) {
  if (!mat) return;
  mat->flags = flags_clr;  // PE mat+0x10
  mat->stage_cap = 0;
  mat->tex_count = 1;
  mat->uv_info0 = (flags_clr < 0) ? 449 : 65;
  std::memset(mat->stages, 0, sizeof(mat->stages));
  std::memset(mat->uv_slot, 0, sizeof(mat->uv_slot));
  // W33A slice: type0 (case CreateMatFromStages@4BF2D1). Other types OOS.
  if (mat->type != 0) return;
  mat->stage_cap = 6;
  // PE init order (not linear): +0x6C, +0x24, +0, +0xB4, +0x48, Apply, +0x90.
  asyncload_gfx_stage_init_tex(&mat->stages[3], 0x6000000);  // +0x6C
  asyncload_gfx_stage_init_tex(&mat->stages[1], 0x6000004);  // +0x24
  asyncload_gfx_stage_init_tex(&mat->stages[0], 0x6000007);  // +0
  // +0xB4 = stages[5]: PE GfxStage via sub_4E6AB0(2) — tag=2, not tex slot.
  std::memset(&mat->stages[5], 0, sizeof(mat->stages[5]));
  mat->stages[5].d[0] = 2;
  reinterpret_cast<float&>(mat->stages[5].d[5]) = 1.f;
  asyncload_gfx_stage_init_tex(&mat->stages[2], 0x6000005);  // +0x48
  asyncload_gfx_mat_apply_stages(mat, src, nsrc);
  // +0x90 = stages[4]: PE sub_4E6A80(0) after Apply.
  std::memset(&mat->stages[4], 0, sizeof(mat->stages[4]));
  mat->stages[4].d[0] = 0;
  mat->stages[4].d[1] = -1;
  reinterpret_cast<float&>(mat->stages[4].d[2]) = 1.f;
  reinterpret_cast<float&>(mat->stages[4].d[3]) = 1.f;
  reinterpret_cast<float&>(mat->stages[4].d[4]) = 1.f;
  reinterpret_cast<float&>(mat->stages[4].d[5]) = 1.f;
  asyncload_gfx_mat_get_uv_tex_info(mat);
}

// Soft PE GfxDevice_CreateVB @ 0x4BEEF0 (device vt+0xCC).
// W33A: GfxStage_Parse walk + 0x6000004→0x6000007 + CreateMatFromStages type0.
AsyncLoadCreateVb* asyncload_create_vb_soft(const uint8_t* payload,
                                            uint32_t payload_len) {
  if (!payload || payload_len < 12u) return nullptr;
  auto* c = static_cast<AsyncLoadCreateVb*>(
      std::malloc(sizeof(AsyncLoadCreateVb)));
  if (!c) return nullptr;
  std::memset(c, 0, sizeof(AsyncLoadCreateVb));
  const auto* pdw = reinterpret_cast<const int32_t*>(payload);
  c->type = pdw[0];
  const int32_t v16 = pdw[1] | 1;  // PE a3=0 on case0
  c->flags = v16;
  c->stage_n = pdw[2];
  c->tex_count = 1;
  c->uv_info0 = 65;

  AsyncLoadGfxStage parsed[16];
  int32_t nparsed = 0;
  const uint8_t* cur = payload + 12;
  uint32_t left = (payload_len > 12u) ? (payload_len - 12u) : 0u;
  const int32_t nstage = c->stage_n;
  if (nstage > 0 && nstage <= 16) {
    for (int32_t i = 0; i < nstage && left >= 4u; ++i) {
      const int32_t adv =
          asyncload_gfx_stage_parse(&parsed[nparsed], cur, left);
      if (adv <= 0) break;
      // PE CreateVB: (tag & 0xFF00FFFF) == 0x6000004 → tag = 0x6000007.
      if ((parsed[nparsed].d[0] & 0xFF00FFFF) == 0x6000004)
        parsed[nparsed].d[0] = 0x6000007;
      cur += static_cast<uint32_t>(adv);
      left -= static_cast<uint32_t>(adv);
      ++nparsed;
    }
  }
  asyncload_create_mat_from_stages_soft(c, parsed, nparsed, v16 & ~1);
  // Keep CreateVB v16 on POD (bit0 set); GetUvTexInfo already used flags_clr.
  c->flags = v16;
  return c;
}