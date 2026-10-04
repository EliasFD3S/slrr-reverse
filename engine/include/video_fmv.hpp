#pragma once

#include <cstdint>

namespace inv {

// Soft PE GfxEngine.openVideo @ 0x47C330 → FMV_DirectShow_Open @ 0x4F8DD0:
// null path → -1; else dsReady gate → close-if-playing → FilterGraph →
// non_excl TextureRenderer stand-in / else RenderFile+HWND → QI →
// SetNotifyWindow(0x40D) → loop+Run → 0 (Run fail still 0, playing=0).
// Intro→menu: exclusive boot Close (put_Visible 0 @ Boot 0x55C60F soft) then
// MainMenu openVideo(prime.avi,1,1); presentCount BeginScene before Run OOS.
// Soft residual: SampleGrabber+Null (not stock TextureRenderer); VW/BV QI
// hard only for exclusive HWND branch.

int32_t video_fmv_open(const char* path, int32_t non_exclusive, int32_t loop);
void video_fmv_close();
int32_t video_fmv_is_playing();
// Soft PE FMV_nonExclusive @ 0x61852C (!=0 TextureRenderer / ==0 exclusive HWND).
int32_t video_fmv_is_non_exclusive();
// Soft PE GfxEngine_AltPresentGate @ 0x4F9760:
// FMV_playing@64A394 != 0 && FMV_nonExclusive@61852C == 0.
int32_t video_fmv_alt_present_gate();

// Call each frame before OSD (uploads sample + draws aspect-fit quad).
void video_fmv_present();
int32_t video_fmv_width();
int32_t video_fmv_height();

// Engine_boot @ 0x0058C700: Activision → Invictus (StreetLegal.avi = SL1 leftover,
// skipped when missing). Blocking like FMV_Boot_PlayPath_DirectShow @ 0x55C470.
// max_frames_each: 0 = until end/ESC; >0 caps each clip (smoke).
int32_t video_fmv_play_boot_intros(int32_t max_frames_each);

}  // namespace inv
