cmake_minimum_required(VERSION 3.16)
project(slrr_native LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Unity-like layout:
#   runtime/  — domain natives (System, Cars, Parts, …)
#   core/     — JVM, TREE, script host, platform backends
# Match original shipping binary: 32-bit first. On MSVC, configure with -A Win32.
add_executable(slrr_engine
  # --- core ---
  core/Script/main.cpp
  core/Script/game_boot.cpp
  core/Script/game_script.cpp
  core/Script/tree_fields.cpp
  core/Script/tree_eval.cpp
  core/natives_registry.cpp
  core/natives_stubs.cpp
  core/Jvm/jvm_bridge.cpp
  core/Jvm/jvm.cpp
  core/Jvm/jvm_register.cpp
  core/Jvm/jvm_load.cpp
  core/Jvm/jvm_vmthread.cpp
  core/Jvm/tufa.cpp
  core/Jvm/callinfo.cpp
  core/Jvm/runtime.cpp
  core/Platform/rpak.cpp
  core/Platform/render_d3d9.cpp
  core/Platform/video_fmv.cpp
  core/Platform/input_win32.cpp
  core/Platform/audio_win32.cpp

  # --- runtime / System ---
  runtime/System/System.cpp
  runtime/System/System_natives.cpp
  runtime/System/Lang.cpp
  runtime/System/Math3D.cpp
  runtime/System/GameType.cpp

  # --- runtime / IO ---
  runtime/IO/IO.cpp

  # --- runtime / Resources ---
  runtime/Resources/Resources.cpp
  runtime/Resources/PhysicsRef.cpp
  runtime/Resources/GroundRef_route.cpp
  runtime/Resources/GameRef.cpp
  runtime/Resources/GameRef_core.cpp
  runtime/Resources/GameRef_collision.cpp
  runtime/Resources/GroundRef_traffic.cpp
  runtime/Resources/AnimationParticle.cpp

  # --- runtime / Parts ---
  runtime/Parts/world_state.cpp
  runtime/Parts/Part.cpp
  runtime/Parts/WheelRef.cpp
  runtime/Parts/DynoData.cpp
  runtime/Parts/SfxTable.cpp
  runtime/Parts/Body/Chassis.cpp

  # --- runtime / Cars ---
  runtime/Cars/Cars.cpp

  # --- runtime / Render ---
  runtime/Render/RenderNatives.cpp

  # --- runtime / Audio ---
  runtime/Audio/Sound.cpp
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
