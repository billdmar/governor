/*
 * gov_faults.h (lib/telem) — redirect to the canonical shared definition.
 *
 * At ,  unified the fault-flag bitmask into lib/common/gov_faults.h so
 * lib/safety and lib/telem share ONE source of truth (they must agree on exact
 * bit values). This stub keeps telem's `#include "gov_faults.h"` working; the
 * real definitions live in lib/common. Build include paths add -I../../lib/common.
 */
#ifndef GOV_TELEM_FAULTS_REDIRECT_H
#define GOV_TELEM_FAULTS_REDIRECT_H

#include "../common/gov_faults.h"

#endif /* GOV_TELEM_FAULTS_REDIRECT_H */
