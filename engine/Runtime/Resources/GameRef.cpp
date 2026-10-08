#include "host_objects.hpp"
#include "natives.hpp"
#include "runtime.hpp"
#include "rpak.hpp"
#include "jvm.hpp"
#include "tree_interp.hpp"
#include "render_d3d9.hpp"
#include "input_win32.hpp"
#include "video_fmv.hpp"
#include "Resources.h"
#include "System.h"
#include "GameRef.h"
#include "GameRef_internal.hpp"
#include "../Parts/Body/Chassis.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// Body split into fragments to stay under the 128 KB editor buffer
// cap (see native/tools/split_source.py). They are concatenated here,
// so this translation unit is identical to the single-file version.
#include "GameRef_part1.inc"
#include "GameRef_part2.inc"
#include "GameRef_part3.inc"
#include "GameRef_part4.inc"
#include "GameRef_part5.inc"
#include "GameRef_part6.inc"
