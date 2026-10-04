// W10A — PE *[phys+0xDC] part dllist export (cmdAddPart @ 0x447B82 →
// GameRef_physDcList_insertTail @ 0x45FAF0). Chassis forceUpdate_apply @
// 0x4485C9 / setBuck @ 0x43E1C0 consume this list — include this header
// only (no GameRef.cpp guts).
#pragma once

#include <cstdint>

namespace inv {

struct InvObject;

// Opaque PePhysDcNode* (host). PE walk idiom @ 0x4485C9:
//   head = *[phys+0xDC]; start = *(head+4)!=0 ? head : 0;
//   next = *(node+4); stop when next==0 || *(next+4)==0;
//   part id @ +0x14; child instance @ +0x18.
// Host: head = first live node (sentinel skipped); next stops at sentinel.
void* gameref_phys_dc_head(InvObject* gameref);
void* gameref_phys_dc_next(void* node);
int32_t gameref_phys_dc_part_id(void* node);   // PE +0x14 match_key
InvObject* gameref_phys_dc_child(void* node);  // host InvObject* (PE +0x18)
int32_t gameref_phys_dc_count(InvObject* gameref);

// W12A/W13A — PE GameRef_findCamIndexByMatchId @ 0x448D10.
// Walk cam[4] stride 0x4A8; match @ blob+0x128 (=cam_entry+0xC = ctrl
// after activate Bind cam+0x4). osd cmd findCam(ctrl) then writes osd.id
// @ cam+0x5C via Bind cam+0x54 — not into +0x128.
// count = chassis_cam_count (+0x177C). Returns idx or -1.
int32_t gameref_find_cam_index_by_match_id(InvObject* gameref, int32_t match_id,
                                           int32_t start_idx, int32_t step);

}  // namespace inv
