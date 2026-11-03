/* env_int.h - ec_env_int_ne(name, def): unset or "" -> def; else atoi. */
#ifndef EC_ENV_INT_H
#define EC_ENV_INT_H
#include <stdlib.h>
static inline int ec_env_int_ne(const char *name, int def) { const char *e = getenv(name); return (e && *e) ? atoi(e) : def; }
#endif
