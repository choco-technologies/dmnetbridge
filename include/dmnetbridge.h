#ifndef DMNETBRIDGE_H
#define DMNETBRIDGE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "dmod_types.h"
#include "dmnetbridge_defs.h"

/**
 * Public API for the dmnetbridge module.
 *
 * Functions are declared with the dmod_dmnetbridge_api(...) macro - dmod's
 * standard pattern for functions callable from other modules (or from this
 * module's own tests/), resolved dynamically by the loader rather than
 * through normal static linkage. See dm_sw_ring/include/dm_sw_ring.h for a
 * fully worked real-world example of the same shape.
 *
 * Definitions in src/dmnetbridge.c use the matching
 * dmod_dmnetbridge_api_declaration(...) macro - a plain C function
 * definition here will NOT satisfy these declarations at link time.
 *
 * This is an example interface using the usual "opaque handle" pattern -
 * replace the handle, functions, and struct definition in
 * src/dmnetbridge.c with your module's real API.
 */

/* Opaque handle - the real struct is defined in src/dmnetbridge.c */
typedef struct dmnetbridge* dmnetbridge_t;

/**
 * Create a new dmnetbridge instance.
 *
 * @return A valid handle on success, or NULL on allocation failure.
 */
dmod_dmnetbridge_api(1.0, dmnetbridge_t, _create, ( void ));

/**
 * Destroy an instance created by dmnetbridge_create(). Safe to call with
 * NULL.
 */
dmod_dmnetbridge_api(1.0, void, _destroy, ( dmnetbridge_t handle ));

/**
 * Example accessor - replace with your module's real API.
 *
 * @return true if handle is a valid, non-NULL instance.
 */
dmod_dmnetbridge_api(1.0, bool, _is_valid, ( dmnetbridge_t handle ));

#endif // DMNETBRIDGE_H
