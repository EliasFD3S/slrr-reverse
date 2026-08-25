cmake_minimum_required(VERSION 3.16)
project(slrr_native LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Unity-like layout:
#   Runtime/  — domain natives (System, Cars, Parts, …)
#   Core/     — JVM, TREE, script host, platform backends
# Match original shipping binary: 32-bit first. On MSVC, configure with -A Win32.
add_executable(slrr_engine
  # --- Core ---
  Core/Script/main.cpp
  Core/Script/game_boot.cpp
  Core/Script/game_script.cpp
  Core/Script/tree_interp.cpp
  Core/natives_registry.cpp
  Core/natives_stubs.cpp
  Core/Jvm/jvm_bridge.cpp
  Core/Jvm/jvm.cpp
  Core/Jvm/tufa.cpp
  Core/Jvm/callinfo.cpp
  Core/Jvm/runtime.cpp
  Core/Platform/rpak.cpp
  Core/Platform/render_d3d9.cpp
  Core/Platform/video_fmv.cpp
  Core/Platform/input_win32.cpp
  Core/Platform/audio_win32.cpp

  # --- Runtime / System ---
  Runtime/System/System.cpp
  Runtime/System/Lang.cpp
  Runtime/System/Math3D.cpp
  Runtime/System/GameType.cpp

  # --- Runtime / IO ---
  Runtime/IO/IO.cpp

  # --- Runtime / Resources ---
  Runtime/Resources/Resources.cpp
  Runtime/Resources/GameRef.cpp
  Runtime/Resources/AnimationParticle.cpp

  # --- Runtime / Parts ---
  Runtime/Parts/world_state.cpp
  Runtime/Parts/Part.cpp
  Runtime/Parts/WheelRef.cpp
  Runtime/Parts/DynoData.cpp
  Runtime/Parts/SfxTable.cpp
  Runtime/Parts/Body/Chassis.cpp

  # --- Runtime / Cars ---
  Runtime/Cars/Cars.cpp

  # --- Runtime / Render ---
  Runtime/Render/RenderNatives.cpp

  # --- Runtime / Audio ---
  Runtime/Audio/Sound.cpp
)

# Runtime data for host JVM smoke (class index + sample TUFA).
add_custom_command(TARGET slrr_engine POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:slrr_engine>/data"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/data/class_index.txt"
    "$<TARGET_FILE_DIR:slrr_engine>/data/class_index.txt"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/data/System.class"
    "$<TARGET_FILE_DIR:slrr_engine>/data/System.class"
  COMMAND ${CMAKE_COMMAND} -E remove_directory
    "$<TARGET_FILE_DIR:slrr_engine>/assets"
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_CURRENT_SOURCE_DIR}/assets"
    "$<TARGET_FILE_DIR:slrr_engine>/assets"
  COMMENT "Copy JVM data + stock assets next to slrr_engine"
)

target_include_directories(slrr_engine PRIVATE include)

if(MSVC)
  target_compile_options(slrr_engine PRIVATE /W3 /utf-8)
  target_compile_definitions(slrr_engine PRIVATE _CRT_SECURE_NO_WARNINGS)
  target_link_libraries(slrr_engine PRIVATE d3d9 dinput8 dxguid dsound winmm
                        user32 gdi32 windowscodecs ole32 oleaut32 strmiids)
else()
  target_compile_options(slrr_engine PRIVATE -Wall -Wextra)
endif()

install(TARGETS slrr_engine RUNTIME DESTINATION bin)
