/*
 * Shield effects ported from Inferno DMAX:
 *  - shield bubbles around other players (hit ripples, low-shield
 *    flicker, collapse and restart), gold when they are invulnerable;
 *  - the first-person sweep when your own shields collapse or come back
 *    and when invulnerability starts or ends;
 *  - the red low-shield pulse on the cockpit ship gauge.
 * Other players' shield level and invulnerability come from the
 * MULTI_DMAX_SHIELDS message (multi.c).
 */

#ifndef _SHIELDFX_H
#define _SHIELDFX_H

#include "object.h"

// New level, new game or leaving the game: forget all shield state.
void shieldfx_reset(void);

// A weapon hit a player (any player, local or remote). damage is in fix
// shield units after all multipliers.
void shieldfx_player_hit(object *playerobj, object *weapon, vms_vector *hit_point, fix damage);

// Another player's shield state from the network.
void shieldfx_remote_state(int pnum, fix shields, int invulnerable, int converting, fix invul_elapsed, fix invul_remaining);

// The local pilot is transferring energy to shields this frame (gamecntl.c).
void shieldfx_local_converting(void);
// Whether the local pilot transferred energy to shields very recently.
int shieldfx_is_local_converting(void);

// Once per frame, before the main view renders: hands the bubbles and the
// first-person sweep to the renderer.
void shieldfx_frame(void);

// Alpha (0..1) of the red pulse over the cockpit ship gauge; 0 = none.
float shieldfx_gauge_pulse(void);

#endif
