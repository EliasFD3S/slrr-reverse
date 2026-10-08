// Split from natives_generated_world.cpp — Chassis.cpp
#include "natives.hpp"
#include "host_objects.hpp"
#include "runtime.hpp"
#include "render_d3d9.hpp"
#include "tree_interp.hpp"
#include "input_win32.hpp"
#include "video_fmv.hpp"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <array>
#include <string>
#include <unordered_map>
#include <vector>

#include "../world_state.hpp"
#include "Chassis.h"
#include "GameRef.h"
#include "Resources.h"

// Body split into fragments to stay under the 128 KB editor buffer
// cap (see native/tools/split_source.py). They are concatenated here,
// so this translation unit is identical to the single-file version.
#include "Chassis_part1.inc"
#include "Chassis_part2.inc"
