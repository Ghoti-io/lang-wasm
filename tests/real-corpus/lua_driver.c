/* SPDX-License-Identifier: LGPL-3.0-only */
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "corpus_result.h"

/* The harness needs only Lua's core allocation contract. Avoid luaL_newstate:
 * it installs host-oriented panic/warning handlers and seeds from the clock,
 * which add unrelated WASI imports to this deterministic reactor workload. */
static void *corpus_lua_allocator(void *userdata, void *pointer,
    size_t old_size, size_t new_size) {
  (void)userdata;
  (void)old_size;
  if (new_size == 0) {
    free(pointer);
    return NULL;
  }
  return realloc(pointer, new_size);
}

uint64_t corpus_run(void) {
  corpus_result_reset();
  static const char script[] =
      "local total=0; local out={}; "
      "for i=1,32 do local v=(i*17)%23-11; total=total+v; "
      "out[i]=string.format('%d:%d',i,v) end; "
      "return total, table.concat(out, ',')";
  lua_State *state = lua_newstate(corpus_lua_allocator, NULL,
      UINT32_C(0x4c554135));
  if (!state) return 0;
  (void)luaopen_string(state);
  lua_setglobal(state, LUA_STRLIBNAME);
  (void)luaopen_table(state);
  lua_setglobal(state, LUA_TABLIBNAME);
  if (luaL_loadbuffer(state, script, sizeof(script) - 1, "corpus.lua") != LUA_OK ||
      lua_pcall(state, 0, 2, 0) != LUA_OK || !lua_isinteger(state, -2) ||
      lua_type(state, -1) != LUA_TSTRING) {
    lua_close(state);
    return 0;
  }
  size_t length = 0;
  const char *bytes = lua_tolstring(state, -1, &length);
  const int64_t total = (int64_t)lua_tointeger(state, -2);
  const int ok = bytes && length <= UINT32_MAX &&
      corpus_result_append_u64((uint64_t)total) &&
      corpus_result_append_u32((uint32_t)length) &&
      corpus_result_append(bytes, (uint32_t)length);
  lua_close(state);
  if (!ok) {
    corpus_result_reset();
    return 0;
  }
  corpus_result_valid = 1;
  return 1;
}
