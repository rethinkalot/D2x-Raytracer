/*
 * Shield effects ported from Inferno DMAX (Render.Object.cpp, HUD.cpp,
 * Shield.hlsl). The bubble and the sweep are drawn by the renderer's
 * post-process pass (post_process.hlsl); this file keeps the game-side
 * state and hands it over once per frame. See shieldfx.h.
 *
 * Set D2_SHIELD_EFFECTS=0 in the environment to turn all of it off.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "inferno.h"
#include "3d.h"
#include "object.h"
#include "player.h"
#include "game.h"
#include "gameseq.h"
#include "multi.h"
#include "endlevel.h"
#include "laser.h"
#include "playsave.h"
#include "shieldfx.h"
#ifdef RT_DX12
#include "RTgr.h"
#endif

extern g3s_lrgb compute_light_emission(int objnum); // lighting.c

#define SHIELD_LIFETIME           1.0f   // seconds a hit ripple lasts
#define MAX_SHIELD_IMPACTS        24
#define SHIELD_REFERENCE          100.0f // shields that count as a full bubble
#define SHIELD_RADIUS_SCALE       1.19f
#define FADE_DURATION             0.70f  // invulnerability end / collapse
#define ACTIVATION_DURATION       0.60f
#define ACTIVATION_FADE_DURATION  0.25f
#define SWEEP_DURATION            0.48f
#define SWEEP_ACTIVATION_FADE     0.18f
#define SETTLE_TIME               2.0     // seconds after joining/level start/respawn

typedef struct shield_impact
{
	int    pnum;
	int    signature;
	double started;
	float  dir[3];
	float  strength;
	float  color[3];
	float  shield_fraction;
} shield_impact;

typedef struct shield_player
{
	// From MULTI_DMAX_SHIELDS
	int    known;
	float  shields;
	int    invulnerable;
	double invul_start;
	double invul_end;
	double converting_until;      // energy-to-shield transfer seen until then

	// Smoothed 0..1 strength of the transfer effect
	float  charge;
	double charge_updated;

	// Visual state
	int    signature;
	float  last_shields;          // < 0: not tracked yet
	double shield_restarted;
	double collapse_started;
	int    shield_collapsed;
	int    was_invulnerable;
	double fade_started;
	double last_rendered_invulnerable;
} shield_player;

enum
{
	SWEEP_NONE,
	SWEEP_SHIELD,         // shields collapsed
	SWEEP_SHIELD_ON,      // shields came back
	SWEEP_INVUL,          // invulnerability ended
	SWEEP_INVUL_ON        // invulnerability started
};

typedef struct first_person_state
{
	int    need_rebase;
	double rebased;               // when the state was last rebased
	int    quiet_invulnerability; // began while settling: no sweep, no bubble
	float  prev_shields;
	int    prev_invulnerable;
	double started;
	int    type;
	double invulnerable_since;
	double pending_since;
	int    pending_type;
} first_person_state;

static shield_impact      Impacts[MAX_SHIELD_IMPACTS];
static int                Next_impact;
static shield_player      Shield_players[MAX_PLAYERS];
static first_person_state First_person;
static double             Local_converting_until = -100.0;

static double now_seconds(void)
{
	return (double)GameTime64 / 65536.0;
}

static float saturatef(float v)
{
	return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

static float clampf(float v, float lo, float hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

static int effects_enabled(void)
{
	static int enabled = -1;
	if (enabled < 0)
	{
		const char *value = getenv("D2_SHIELD_EFFECTS");
		enabled = !(value && value[0] == '0');
	}
	return enabled;
}

static void reset_player_visuals(shield_player *sp)
{
	sp->last_shields = -1.0f;
	sp->shield_restarted = -100.0;
	sp->collapse_started = -100.0;
	sp->shield_collapsed = 0;
	sp->was_invulnerable = 0;
	sp->fade_started = -100.0;
	sp->last_rendered_invulnerable = -100.0;
	sp->charge = 0.0f;
}

void shieldfx_reset(void)
{
	int i;
	memset(Impacts, 0, sizeof(Impacts));
	Next_impact = 0;
	memset(Shield_players, 0, sizeof(Shield_players));
	for (i = 0; i < MAX_PLAYERS; i++)
	{
		Shield_players[i].signature = -1;
		reset_player_visuals(&Shield_players[i]);
	}
	memset(&First_person, 0, sizeof(First_person));
	First_person.need_rebase = 1;
	Local_converting_until = -100.0;
}

void shieldfx_local_converting(void)
{
	// Called every frame the transfer runs; bridges the odd frame where
	// nothing moved (rounding) without lingering after the key is let go.
	Local_converting_until = now_seconds() + 0.15;
}

int shieldfx_is_local_converting(void)
{
	return now_seconds() < Local_converting_until;
}

void shieldfx_remote_state(int pnum, fix shields, int invulnerable, int converting, fix invul_elapsed, fix invul_remaining)
{
	shield_player *sp;
	double now;

	if (pnum < 0 || pnum >= MAX_PLAYERS || pnum == Player_num)
		return;

	sp = &Shield_players[pnum];
	now = now_seconds();
	sp->known = 1;
	sp->shields = f2fl(shields);
	sp->invulnerable = invulnerable;
	// Updates arrive about ten times a second while transferring.
	sp->converting_until = converting ? now + 0.35 : -100.0;
	if (invulnerable)
	{
		// The sender's clock differs from ours, so it sends durations.
		sp->invul_start = now - f2fl(invul_elapsed);
		sp->invul_end = invul_remaining < 0 ? 1.0e30 : now + f2fl(invul_remaining);
	}
}

void shieldfx_player_hit(object *playerobj, object *weapon, vms_vector *hit_point, fix damage)
{
	shield_impact *hit;
	shield_player *sp;
	vms_vector dir;
	float length, r = 1.0f, g = 0.82f, b = 0.48f, brightest, known_shields;
	int pnum;

	if (!playerobj || playerobj->type != OBJ_PLAYER || !hit_point)
		return;
	pnum = playerobj->id;
	// Our own hits are kept too: in VR you see your own bubble.
	if (pnum < 0 || pnum >= MAX_PLAYERS)
		return;

	sp = &Shield_players[pnum];
	hit = &Impacts[Next_impact++ % MAX_SHIELD_IMPACTS];
	hit->pnum = pnum;
	hit->signature = playerobj->signature;
	hit->started = now_seconds();

	vm_vec_sub(&dir, hit_point, &playerobj->pos);
	length = f2fl(vm_vec_mag(&dir));
	if (length < 0.001f)
		dir = playerobj->orient.fvec;
	else
		vm_vec_normalize(&dir);
	hit->dir[0] = f2fl(dir.x);
	hit->dir[1] = f2fl(dir.y);
	hit->dir[2] = f2fl(dir.z);

	hit->strength = clampf(0.55f + f2fl(damage) / 35.0f, 0.65f, 1.8f);
	known_shields = pnum == Player_num ? f2fl(Players[Player_num].shields) + f2fl(damage) :
		(sp->known ? sp->shields : SHIELD_REFERENCE);
	hit->shield_fraction = saturatef((known_shields - f2fl(damage)) / SHIELD_REFERENCE);

	// Tint the ripple with the weapon's light colour. Omega is skipped: its
	// light code draws from the game's random number generator.
	if (weapon && weapon->type == OBJ_WEAPON && weapon->id != OMEGA_ID)
	{
		const g3s_lrgb light = compute_light_emission((int)(weapon - Objects));
		const float lr = f2fl(light.r), lg = f2fl(light.g), lb = f2fl(light.b);
		if (lr + lg + lb > 0.001f)
		{
			r = lr; g = lg; b = lb;
		}
	}
	brightest = r > g ? r : g;
	brightest = brightest > b ? brightest : b;
	if (brightest < 0.001f)
		brightest = 0.001f;
	hit->color[0] = r * (2.9f / brightest);
	hit->color[1] = g * (2.9f / brightest);
	hit->color[2] = b * (2.9f / brightest);
}

#ifdef RT_DX12
static RT_Vec3 shield_color(float fraction)
{
	const float t = saturatef(fraction);
	return RT_Vec3Make(0.16f + (0.42f - 0.16f) * t,
	                   0.22f + (0.52f - 0.22f) * t,
	                   1.55f + (2.85f - 1.55f) * t);
}

static void build_player_bubble(RT_ShieldEffects *fx, int pnum, double now)
{
	shield_player *sp = &Shield_players[pnum];
	const shield_impact *active = NULL;
	object *obj;
	float hp, critical, invul_fade, activation_fade, restart_age, restart_fade, collapse_age, collapse_fade;
	float age, impact_fade, fraction, expiry = 0.0f, radius;
	int invulnerable, invul_ending, destabilizing, restarting, collapsing, i;
	RT_ShieldBubble *bubble;

	// (Single player does not mark the local pilot as connected.)
	if ((pnum != Player_num && Players[pnum].connected != CONNECT_PLAYING) || Players[pnum].objnum < 0)
	{
		sp->signature = -1;
		reset_player_visuals(sp);
		return;
	}
	obj = &Objects[Players[pnum].objnum];
	if (obj->type != OBJ_PLAYER)
	{
		// Dead (ghost) or gone: start fresh when the ship reappears.
		sp->signature = -1;
		reset_player_visuals(sp);
		return;
	}
	if (obj->signature != sp->signature)
	{
		sp->signature = obj->signature;
		reset_player_visuals(sp);
	}
	if (Players[pnum].flags & PLAYER_FLAGS_CLOAKED)
		return;

	if (pnum == Player_num)
	{
		// Your own ship (VR): read the local state directly.
		if (Player_is_dead)
		{
			reset_player_visuals(sp);
			return;
		}
		sp->known = 1;
		sp->shields = f2fl(Players[Player_num].shields);
		sp->invulnerable = (Players[Player_num].flags & PLAYER_FLAGS_INVULNERABLE) &&
			!First_person.quiet_invulnerability;
		sp->invul_start = (double)Players[Player_num].invulnerable_time / 65536.0;
		sp->invul_end = sp->invul_start + f2fl(INVULNERABLE_TIME_MAX);
		sp->converting_until = Local_converting_until;
	}

	{
		// Ease the transfer effect in and out.
		const float dt = clampf((float)(now - sp->charge_updated), 0.0f, 0.1f);
		const int converting = now < sp->converting_until;
		sp->charge_updated = now;
		sp->charge = converting ? sp->charge + dt / 0.25f : sp->charge - dt / 0.45f;
		sp->charge = saturatef(sp->charge);
	}

	hp = sp->known ? sp->shields : SHIELD_REFERENCE;
	invulnerable = sp->known && sp->invulnerable && now < sp->invul_end + 0.25;

	if (sp->last_shields < 0.0f)
	{
		sp->last_shields = hp;
		sp->shield_collapsed = hp < 1.0f;
	}
	else
	{
		// The gauge truncates fractional shields to zero, so crossing one
		// point is where the field goes offline or comes back online.
		if (sp->last_shields < 1.0f && hp >= 1.0f)
		{
			sp->shield_restarted = now;
			sp->shield_collapsed = 0;
		}
		else if (hp < 1.0f && !sp->shield_collapsed)
		{
			sp->shield_collapsed = 1;
			if (hp >= 0.0f) // alive at zero; a lethal hit gets no shutdown sweep
				sp->collapse_started = now;
		}
		sp->last_shields = hp;
	}

	if (invulnerable)
	{
		sp->was_invulnerable = 1;
		sp->fade_started = -100.0;
		sp->last_rendered_invulnerable = now;
	}
	else if (sp->was_invulnerable)
	{
		sp->was_invulnerable = 0;
		// Do not replay a stale shutdown for a player who comes into view
		// after their invulnerability already ran out elsewhere.
		sp->fade_started = now - sp->last_rendered_invulnerable <= 0.25 ? now : -100.0;
	}

	invul_fade = invulnerable ? 0.0f : saturatef((float)(now - sp->fade_started) / FADE_DURATION);
	invul_ending = !invulnerable && invul_fade < 1.0f;
	critical = SHIELD_REFERENCE * 0.05f;
	destabilizing = !invulnerable && !sp->shield_collapsed && hp > 0.0f && hp <= critical;
	activation_fade = invulnerable ? saturatef((float)(now - sp->invul_start) / ACTIVATION_DURATION) : 1.0f;
	restart_age = (float)(now - sp->shield_restarted);
	restarting = restart_age >= 0.0f && restart_age < ACTIVATION_DURATION + ACTIVATION_FADE_DURATION;
	restart_fade = saturatef(restart_age / ACTIVATION_DURATION);
	collapse_age = (float)(now - sp->collapse_started);
	collapsing = !restarting && collapse_age >= 0.0f && collapse_age < FADE_DURATION;
	collapse_fade = collapsing ? saturatef(collapse_age / FADE_DURATION) : 0.0f;

	for (i = 0; i < MAX_SHIELD_IMPACTS; i++)
	{
		const shield_impact *hit = &Impacts[i];
		const double hit_age = now - hit->started;
		if (hit->pnum == pnum && hit->signature == obj->signature && hit->started > 0.0 &&
			hit_age >= 0.0 && hit_age < SHIELD_LIFETIME && (!active || hit->started > active->started))
			active = hit;
	}

	if (!active && !invulnerable && !invul_ending && !destabilizing && !restarting && !collapsing && sp->charge <= 0.0f)
		return;
	if (fx->bubble_count >= RT_MAX_SHIELD_BUBBLES)
		return;

	age = active ? (float)(now - active->started) : SHIELD_LIFETIME;
	impact_fade = active ? powf(1.0f - saturatef(age / SHIELD_LIFETIME), 1.25f) * saturatef(age / 0.04f) : 0.0f;
	fraction = active && !restarting ? active->shield_fraction : saturatef(hp / SHIELD_REFERENCE);
	if (invulnerable && sp->invul_end < 1.0e29)
		expiry = saturatef((4.0f - (float)(sp->invul_end - now)) / 4.0f);

	radius = f2fl(obj->size) * SHIELD_RADIUS_SCALE * RT_VRObjectScale();

	bubble = &fx->bubbles[fx->bubble_count++];
	memset(bubble, 0, sizeof(*bubble));
	bubble->center = RT_Vec3Fromvms_vector(&obj->pos);
	bubble->radius = radius;
	if (active)
		bubble->impact_direction = RT_Vec3Make(active->dir[0], active->dir[1], active->dir[2]);
	else
		bubble->impact_direction = RT_Vec3Fromvms_vector(&obj->orient.fvec);
	bubble->impact_strength = active ? active->strength : 0.0f;
	bubble->impact_fade = restarting ? 0.0f : impact_fade;
	bubble->damage = 1.0f - fraction;
	bubble->shutdown_fade = invul_ending ? invul_fade : collapse_fade;
	bubble->self_bubble = pnum == Player_num ? 1.0f : 0.0f;
	bubble->charging = sp->charge;

	if (invulnerable || invul_ending)
	{
		// Gold containment field with an energy pulse.
		bubble->shield_color = RT_Vec3Make(2.55f, 0.88f, 0.14f);
		bubble->impact_color = RT_Vec3Make(3.5f, 2.25f, 0.64f);
		bubble->visibility = 0.70f;
		bubble->invulnerable = 1.0f;
		bubble->expiry_warning = expiry;
		bubble->activation_fade = activation_fade;
		bubble->destabilizing = 0.0f;
	}
	else
	{
		const float critical_visibility = 0.40f + 0.10f * sinf((float)now * 13.0f + sinf((float)now * 4.7f));
		const float restart_visibility = restart_age <= ACTIVATION_DURATION ? 0.70f :
			0.70f * (1.0f - saturatef((restart_age - ACTIVATION_DURATION) / ACTIVATION_FADE_DURATION));
		bubble->shield_color = shield_color(fraction);
		bubble->impact_color = active ? RT_Vec3Make(active->color[0], active->color[1], active->color[2])
		                              : RT_Vec3Make(0.72f, 0.92f, 2.8f);
		bubble->visibility = collapsing ? 0.72f : (restarting ? restart_visibility :
			(destabilizing ? critical_visibility : impact_fade));
		// Keep the bubble on screen while energy is being pumped into it.
		if (bubble->visibility < 0.60f * sp->charge)
			bubble->visibility = 0.60f * sp->charge;
		bubble->invulnerable = 0.0f;
		bubble->expiry_warning = 0.0f;
		bubble->activation_fade = restart_fade;
		bubble->destabilizing = destabilizing ? 1.0f : 0.0f;
	}
}

static void build_first_person_sweep(RT_ShieldEffects *fx, double now)
{
	first_person_state *fp = &First_person;
	const float shields = f2fl(Players[Player_num].shields);
	const int invulnerable = (Players[Player_num].flags & PLAYER_FLAGS_INVULNERABLE) != 0;
	int activating;
	float duration, elapsed, linear, progress, completion_fade;

	// Dying and respawning are not a shield shutdown or power-up.
	if (Player_is_dead)
	{
		fp->need_rebase = 1;
		return;
	}
	if (fp->need_rebase)
	{
		fp->need_rebase = 0;
		fp->rebased = now;
		fp->quiet_invulnerability = invulnerable;
		fp->prev_shields = shields;
		fp->prev_invulnerable = invulnerable;
		fp->started = -100.0;
		fp->type = SWEEP_NONE;
		fp->invulnerable_since = invulnerable ? now : -100.0;
		fp->pending_since = -100.0;
		fp->pending_type = SWEEP_NONE;
		return;
	}

	// Joining a game, starting a level or respawning can briefly report
	// shield or invulnerability changes that are not real; absorb them.
	if (now - fp->rebased < SETTLE_TIME)
	{
		if (invulnerable && !fp->prev_invulnerable)
			fp->quiet_invulnerability = 1;
		if (!invulnerable)
			fp->quiet_invulnerability = 0;
		fp->prev_shields = shields;
		fp->prev_invulnerable = invulnerable;
		fp->invulnerable_since = invulnerable ? now : -100.0;
		fp->pending_since = -100.0;
		fp->pending_type = SWEEP_NONE;
		return;
	}
	if (fp->quiet_invulnerability)
	{
		// Leftover from the settle window: let it run out silently too.
		if (!invulnerable)
			fp->quiet_invulnerability = 0;
		fp->prev_invulnerable = invulnerable;
	}

	if (invulnerable && !fp->prev_invulnerable)
	{
		fp->invulnerable_since = now;
		fp->started = now;
		fp->type = SWEEP_INVUL_ON;
		fp->pending_since = -100.0;
		fp->pending_type = SWEEP_NONE;
	}

	// The gauge floors fractional shields: this is where it first reads zero
	// while the ship is still alive.
	if (fp->prev_shields >= 1.0f && shields > 0.0f && shields < 1.0f)
	{
		fp->pending_since = now;
		fp->pending_type = SWEEP_SHIELD;
	}
	else if (fp->prev_shields < 1.0f && shields >= 1.0f)
	{
		fp->started = now;
		fp->type = SWEEP_SHIELD_ON;
		fp->pending_since = -100.0;
		fp->pending_type = SWEEP_NONE;
	}
	if (fp->prev_invulnerable && !invulnerable && now - fp->invulnerable_since >= 0.25)
	{
		fp->pending_since = now;
		fp->pending_type = SWEEP_INVUL;
	}

	// Require a shutdown to hold for a moment, and cancel it if undone.
	if ((fp->pending_type == SWEEP_SHIELD && !(shields > 0.0f && shields < 1.0f)) ||
		(fp->pending_type == SWEEP_INVUL && invulnerable))
	{
		fp->pending_since = -100.0;
		fp->pending_type = SWEEP_NONE;
	}
	if (fp->pending_type != SWEEP_NONE && now - fp->pending_since >= 0.12)
	{
		fp->started = now;
		fp->type = fp->pending_type;
		fp->pending_since = -100.0;
		fp->pending_type = SWEEP_NONE;
	}
	fp->prev_shields = shields;
	fp->prev_invulnerable = invulnerable;

	activating = fp->type == SWEEP_SHIELD_ON || fp->type == SWEEP_INVUL_ON;
	duration = activating ? SWEEP_DURATION + SWEEP_ACTIVATION_FADE : SWEEP_DURATION;
	elapsed = (float)(now - fp->started);
	if (fp->type == SWEEP_NONE || elapsed < 0.0f || elapsed >= duration)
		return;

	linear = saturatef(elapsed / SWEEP_DURATION);
	progress = linear * linear * (3.0f - 2.0f * linear);
	completion_fade = 1.0f;
	if (activating && elapsed > SWEEP_DURATION)
	{
		const float t = saturatef((elapsed - SWEEP_DURATION) / SWEEP_ACTIVATION_FADE);
		completion_fade = 1.0f - t * t;
	}

	fx->sweep_progress = progress;
	fx->sweep_fade = completion_fade;
	fx->sweep_invulnerable = (fp->type == SWEEP_INVUL || fp->type == SWEEP_INVUL_ON) ? 1.0f : 0.0f;
	fx->sweep_activating = activating ? 1.0f : 0.0f;
	fx->sweep_elapsed = elapsed;
}
#endif

void shieldfx_frame(void)
{
#ifdef RT_DX12
	static int last_level = -1;
	static fix64 last_time = 0;
	RT_ShieldEffects fx;
	double now;
	int pnum;

	if (Current_level_num != last_level || GameTime64 < last_time)
	{
		shieldfx_reset();
		last_level = Current_level_num;
	}
	last_time = GameTime64;

	memset(&fx, 0, sizeof(fx));
	fx.sweep_progress = -1.0f;
	now = now_seconds();
	fx.time = (float)fmod(now, 2048.0);

	if (effects_enabled() && PlayerCfg.ShieldEffects && !Endlevel_sequence)
	{
		// Options > Graphics > Shield Effects
		if (Game_mode & GM_MULTI)
			for (pnum = 0; pnum < N_players && pnum < MAX_PLAYERS; pnum++)
				if (pnum != Player_num)
					build_player_bubble(&fx, pnum, now);
		// Always tracked (it also decides which invulnerability is real).
		build_first_person_sweep(&fx, now);
		// Your own bubble shows the same events as the flat screen sweep
		// (kept in the code, not used), seen from inside on desktop and in VR:
		// hits, invulnerability, collapse, power-up and energy transfer.
		fx.sweep_progress = -1.0f;
		if (Player_num >= 0 && Player_num < MAX_PLAYERS)
			build_player_bubble(&fx, Player_num, now);
	}

	RT_SetShieldEffects(&fx);
#endif
}

float shieldfx_gauge_pulse(void)
{
	float shields, fraction, severity, critical_speed, wave, pulse;
	const float now = (float)fmod(now_seconds(), 2048.0);

	if (!effects_enabled() || !PlayerCfg.ShieldEffects || Player_is_dead)
		return 0.0f;
	if (Players[Player_num].flags & (PLAYER_FLAGS_CLOAKED | PLAYER_FLAGS_INVULNERABLE))
		return 0.0f;
	shields = f2fl(Players[Player_num].shields);
	fraction = saturatef(shields / SHIELD_REFERENCE);
	if (shields <= 0.0f || fraction > 0.20f)
		return 0.0f;

	severity = 1.0f - saturatef(fraction / 0.20f);
	critical_speed = fraction <= 0.10f ? 3.5f : 0.0f;
	wave = 0.5f + 0.5f * sinf(now * (5.5f + severity * 2.5f + critical_speed));
	pulse = wave * wave;
	// Kept well below full strength so the gauge artwork stays readable.
	return saturatef(0.06f + pulse * (0.20f + severity * 0.10f));
}
