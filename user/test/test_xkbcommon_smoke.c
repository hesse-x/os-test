/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// test_xkbcommon_smoke — target-side smoke test for libxkbcommon, a
// wlroots prerequisite.
//
// Verifies only the pure-logic paths: context creation, compiling a
// self-contained keymap (xkb_keymap_new_from_string, no reliance on on-disk
// XKB rule data — that is WF-4 scope), keysym name<->value conversion, and
// symbol lookup of keys in the keymap. Proves that build/libxkbcommon.so can
// be loaded by the musl loader, that symbols (including secure_getenv) resolve,
// and that the minimal API behaves correctly. Follows the diagnostic style of
// test_pixman_smoke.c.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>

static int fail(const char *what) {
  printf("[FAIL] xkbcommon_smoke: %s\n", what);
  _exit(1);
}

int main(void) {
  // 1. Create the context, disabling the default include path — this test uses
  //    a self-contained keymap and reads no on-disk XKB rule data (only WF-4
  //    wires in XKB_CONFIG_ROOT).
  struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_DEFAULT_INCLUDES);
  if (!ctx)
    fail("xkb_context_new");
  printf("xkbcommon: created context %p\n", (void *)ctx);

  // 2. Compile a self-contained minimal keymap: single key <a> (keycode 24)
  //    mapped to keysym 'a'. Contains no include directives, so no XKB data
  //    directory is needed.
  static const char keymap_str[] = "xkb_keymap {\n"
                                   "  xkb_keycodes { <a> = 24; };\n"
                                   "  xkb_compat {};\n"
                                   "  xkb_types {};\n"
                                   "  xkb_symbols { key <a> { [a] }; };\n"
                                   "};\n";
  struct xkb_keymap *keymap = xkb_keymap_new_from_string(
      ctx, keymap_str, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
  if (!keymap)
    fail("xkb_keymap_new_from_string");
  printf("xkbcommon: compiled keymap %p\n", (void *)keymap);

  // 3. keysym name->value: 'a' maps to XKB_KEY_a (0x61).
  xkb_keysym_t ks = xkb_keysym_from_name("a", XKB_KEYSYM_NO_FLAGS);
  printf("xkbcommon: keysym 'a' = 0x%x\n", ks);
  if (ks != 0x61)
    fail("xkb_keysym_from_name('a') != 0x61");

  // 4. value->name: reverse-resolving 0x61 should return "a".
  char name[64];
  int nlen = xkb_keysym_get_name(ks, name, sizeof(name));
  if (nlen <= 0 || (size_t)nlen >= sizeof(name))
    fail("xkb_keysym_get_name");
  printf("xkbcommon: keysym 0x%x -> \"%s\"\n", ks, name);
  if (strcmp(name, "a") != 0)
    fail("xkb_keysym_get_name did not return 'a'");

  // 5. Symbol lookup of a key in the keymap: keycode 24 (<a>)'s Level0 symbol
  //    should be 'a' (0x61). The layout and level args of
  //    xkb_keymap_key_get_syms_by_level are both 0-based (see xkbcommon.h
  //    comments and upstream test/keymap.c usage); a single-level key's symbol
  //    is at level 0; level 1 exceeds the key's level count -> returns 0, syms
  //    set to NULL.
  const xkb_keysym_t *syms = NULL;
  int nsyms = xkb_keymap_key_get_syms_by_level(keymap, 24, 0, 0, &syms);
  printf("xkbcommon: keycode 24 level0 -> %d syms (first 0x%x)\n", nsyms,
         nsyms > 0 ? syms[0] : 0);
  if (nsyms != 1 || !syms || syms[0] != 0x61)
    fail("xkb_keymap_key_get_syms_by_level(keycode 24) != 'a'");

  // 6. State creation (verifies the keymap can construct a state, covering the
  //    xkb_state_new path).
  struct xkb_state *state = xkb_state_new(keymap);
  if (!state)
    fail("xkb_state_new");
  printf("xkbcommon: created state %p\n", (void *)state);

  xkb_state_unref(state);
  xkb_keymap_unref(keymap);
  xkb_context_unref(ctx);
  printf("[OK] xkbcommon_smoke\n");
  return 0;
}
