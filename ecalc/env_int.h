/* env_int.h - the integer environment read, once (was a static copy in comm_xshmem.c, ntt.c, xshmem_boot.c, pe_geo.c).
 * ec_env_int(name, def):    unset -> def; set (also "") -> atoi, so "" reads as 0.   (ntt.c)
 * ec_env_int_ne(name, def): unset or "" -> def; else atoi.                          (xshmem_boot.c, pe_geo.c)
 * The two differ only in the empty string; each call site keeps the rule it had.  tools/switch_registry.py reads both names. */
#ifndef EC_ENV_INT_H
#define EC_ENV_INT_H
#include <stdlib.h>
static inline int ec_env_int(const char *name, int def) { const char *e = getenv(name); return e ? atoi(e) : def; }
static inline int ec_env_int_ne(const char *name, int def) { const char *e = getenv(name); return (e && *e) ? atoi(e) : def; }
#endif
