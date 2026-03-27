/**
 * Debug output macros for frank-386
 *
 * Use DBG_PRINT for informational debug messages that should only
 * appear when DEBUG_ENABLED=1 is set at compile time.
 *
 * Use printf directly for critical errors that should always be shown.
 */

#ifndef DEBUG_H
#define DEBUG_H

#include <stdio.h>

#ifdef DEBUG_ENABLED
#define DBG_PRINT(...) printf(__VA_ARGS__)
#else
#define DBG_PRINT(...) ((void)0)
#endif

#endif /* DEBUG_H */
