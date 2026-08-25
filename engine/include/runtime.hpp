#pragma once

#include "natives.hpp"

#include <cstdint>

namespace inv {

// Minimal string object for the rewrite host (not stock layout yet).
struct InvString {
  const char* utf8;
};

InvObject* string_new(const char* utf8);
const char* string_cstr(InvObject* obj);

// Registry-only extras not always present in Java inventory.
int32_t java_lang_Object_hashCode(InvObject* self);

void time_init();
float time_current();
float time_sim();
float time_warp(float m);
void time_sync_game(float t);
int32_t time_game_day_seconds();
bool exit_requested();
void request_exit();

}  // namespace inv
