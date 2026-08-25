#pragma once

#include <cstdint>

namespace inv {

// Stock path: GfxEngine.openVideo → DirectShow (CoCreateInstance) + texture blit.
// Host: FilterGraph + SampleGrabber → D3D9 texture under OSD.

int32_t video_fmv_open(const char* path, int32_t non_exclusive, int32_t loop);
void video_fmv_close();
int32_t video_fmv_is_playing();

// Call each frame before OSD (uploads sample + draws aspect-fit quad).
void video_fmv_present();
int32_t video_fmv_width();
int32_t video_fmv_height();

// Engine_boot @ 0x0058C700: Activision → Invictus (StreetLegal.avi = SL1 leftover,
// skipped when missing). Blocking like FMV_Boot_PlayPath_DirectShow @ 0x55C470.
// max_frames_each: 0 = until end/ESC; >0 caps each clip (smoke).
int32_t video_fmv_play_boot_intros(int32_t max_frames_each);

}  // namespace inv
