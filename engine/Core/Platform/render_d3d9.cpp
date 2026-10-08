#include "render_d3d9.hpp"
#include "host_objects.hpp"
#include "Resources.h"
#include "rpak.hpp"
#include "input_win32.hpp"
#include "video_fmv.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstddef>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <wincodec.h>
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#endif

// Body split into fragments to stay under the 128 KB editor buffer
// cap (see native/tools/split_source.py). They are concatenated here,
// so this translation unit is identical to the single-file version.
#include "render_d3d9_part1.inc"
#include "render_d3d9_part2.inc"
#include "render_d3d9_part3.inc"
