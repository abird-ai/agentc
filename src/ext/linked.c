/*
 * linked.c — adapter for statically-linked extensions.
 *
 * tools/gen-exts.py emits build/exts.c with a {name, entry} table from
 * extensions/manifest.json. The table is included here (registry.c needs no
 * knowledge of the generated file) and driven by agentc_ext_register_linked().
 */
#include "ext.h"
#include "ext/registry_int.h"

#if defined(__has_include)
#  if __has_include("exts.c")
#    include "exts.c"
#    define AGENTC_EXT_GENERATED 1
#  endif
#endif
#ifndef AGENTC_EXT_GENERATED
#  define AGENTC_EXT_GENERATED 0
#endif

void agentc_ext_register_linked(void) {
#if AGENTC_EXT_GENERATED
    agentc_exts_register_all();
#endif
}
