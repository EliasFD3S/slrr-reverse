#include "natives.hpp"
#include "runtime.hpp"
#include "rpak.hpp"
#include "render_d3d9.hpp"
#include "tree_interp.hpp"
#include "host_objects.hpp"
#include "jvm.hpp"
#include "Resources.h"
#include "System_internal.hpp"

#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

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

// PE AsyncLoad_Mesh_ParseInvoVle3 @ 0x502070.
// Soft W27A: live W26B FVF/stride helpers (PE VB alloc uses imm 0x15003F).
// OOS: sub_5080F0/100/160 mat hdr, Gfx CreateVB (device vt+0xD0 / +208),
//   PtrVec, PackVertex walk (vle3 path — separate from chunks W28A),
//   full AABB wire into job mesh_* until vle3 hosted. Ends flags|0x14.
void asyncload_mesh_parse_invo_vle3(AsyncLoadJob* self) {
  if (!self) return;
  // PE @ VB malloc path: FVF immediate 1376319 (0x15003F) → FromFvf.
  constexpr uint32_t kVle3Fvf = 0x15003Fu;
  (void)gfxvb_layout_from_fvf(kVle3Fvf);
  (void)render_d3d9_mesh_vb_stride_from_fvf(kVle3Fvf);
  (void)render_d3d9_mesh_invo_flags_to_fvf(0u, 0);
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
// flag 0x100 clear + PrepareLod gate + vtbl control / CallNamedMethod
// ("control") — no bytecode VM.
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

// PE float 1.0f pushed as imm 0x3F800000 on vtbl+0xC / +0x14 (int_convert).
constexpr int32_t kVtblPri1f = 0x3F800000;

static void** inv_obj_vtbl(void* obj) {
  if (!obj) return nullptr;
  return *reinterpret_cast<void***>(obj);
}

// PE thiscall vtbl+off(obj, 0x3F800000) — TickSimObject @ 0x42923F / 0x429264.
static void* inv_vtbl_call_pri1f(void* obj, std::size_t slot_off) {
  void** vtbl = inv_obj_vtbl(obj);
  if (!vtbl) return nullptr;
  using Fn = void*(__thiscall*)(void*, int32_t);
  auto* fn = reinterpret_cast<Fn>(vtbl[slot_off / sizeof(void*)]);
  if (!fn) return nullptr;
  return fn(obj, kVtblPri1f);
}

static void inv_vtbl_void_pri1f(void* obj, std::size_t slot_off) {
  void** vtbl = inv_obj_vtbl(obj);
  if (!vtbl) return;
  using Fn = void(__thiscall*)(void*, int32_t);
  auto* fn = reinterpret_cast<Fn>(vtbl[slot_off / sizeof(void*)]);
  if (!fn) return;
  fn(obj, kVtblPri1f);
}

// PE dword field at obj+idx*4 (TickSimObject v4[19]/v4[20]/v4[17]).
static std::uintptr_t inv_obj_dword(void* obj, std::size_t dword_idx) {
  if (!obj) return 0;
  return reinterpret_cast<std::uintptr_t*>(obj)[dword_idx];
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

// PE CallNamedMethod_va @ 0x4255F8..0x42563C gate on *(a1+0x44) ResHandle:
// vtbl+0x14 if dword[19]!=1 → PrepareLod(0x80000000) bit31-clear →
// vtbl+0xC ≠ 0. Soft: no bytecode VM beyond existing TREE invoke path.
static bool engine_call_named_method_va_rh_gate(void* child) {
  if (!child) return false;
  void* rh = reinterpret_cast<void*>(inv_obj_dword(child, 17));  // +0x44
  if (!rh) return false;
  if (inv_obj_dword(rh, 19) != 1u) inv_vtbl_void_pri1f(rh, 0x14);
  if (!resh_prepare_lod_ok(
          resh_prepare_lod_nullsafe(rh, static_cast<int32_t>(0x80000000u))))
    return false;
  return inv_vtbl_call_pri1f(rh, 0xC) != nullptr;
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
      // PE @ 0x4255F8..0x42563C: +0x44 PrepareLod / vtbl+0xC gate.
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
// Stock: stack RH Rebind → save/clear flag 0x100 → PrepareLod(0xA0000000)
//   bit31-clear → vtbl+0xC child → (vtbl+0x2C | CallNamedMethod "control")
//   → restore flags + unlink temp RH. Child null @ 0x42926B → epilogue.
// Host: Rebind/flags/PrepareLod bit31 gate + PE vtbl slices when present;
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
    // PE @ 0x429230: cmp [esi+4Ch],1 — dword[19] at +0x4C
    if (inv_obj_dword(v3, 19) != 1u) inv_vtbl_void_pri1f(v3, 0x14);
    // PE @ 0x429244..0x429255: PrepareLod(v3, 0xA0000000, 0, 0); test bit31
    if (resh_prepare_lod_ok(
            resh_prepare_lod_nullsafe(v3, static_cast<int32_t>(0xA0000000u)))) {
      // PE @ 0x429264: v4 = (*(vtbl+0xC))(v3, 1.0f) → control child
      void* v4 = inv_vtbl_call_pri1f(v3, 0xC);
      if (!v4) {
        // PE @ 0x42926B: jz epilogue — no CallNamedMethod on null child.
        // Host extension only when owner has no PE vtbl at all (TREE GI).
        if (!inv_obj_vtbl(v3))
          engine_call_named_method_control(v3, dt, node->script);
      } else if (inv_obj_dword(v4, 19) != 0) {
        // PE @ 0x42926D..0x4292C4: native controller path
        //   v6 = [edi+44h]; PrepareLod(0x80000000); vtbl+0xC;
        //   ctrl = *(eax+0xC); (*(ctrl.vtbl+0x2C))(ctrl, [edi+4Ch], dt)
        void* v6 = reinterpret_cast<void*>(inv_obj_dword(v4, 17));
        if (v6) {
          if (inv_obj_dword(v6, 19) != 1u) inv_vtbl_void_pri1f(v6, 0x14);
          if (resh_prepare_lod_ok(resh_prepare_lod_nullsafe(
                  v6, static_cast<int32_t>(0x80000000u)))) {
            void* v7 = inv_vtbl_call_pri1f(v6, 0xC);
            if (v7) {
              void* ctrl = *reinterpret_cast<void**>(
                  reinterpret_cast<char*>(v7) + 12);
              if (ctrl)
                engine_vtbl_control_tick(ctrl, inv_obj_dword(v4, 19), dt);
            }
          }
        }
      } else if (inv_obj_dword(v4, 20) != 0) {
        // PE @ 0x4292C9..0x4292E3: [edi+50h]!=0 → CallNamedMethod
        //   (v4, "control", sync=0, tag=3, dt, 79='O')
        engine_call_named_method_control(v4, dt, node->script);
      }
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
