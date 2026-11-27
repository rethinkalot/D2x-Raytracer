/*
THE COMPUTER CODE CONTAINED HEREIN IS THE SOLE PROPERTY OF PARALLAX
SOFTWARE CORPORATION ("PARALLAX").  PARALLAX, IN DISTRIBUTING THE CODE TO
END-USERS, AND SUBJECT TO ALL OF THE TERMS AND CONDITIONS HEREIN, GRANTS A
ROYALTY-FREE, PERPETUAL LICENSE TO SUCH END-USERS FOR USE BY SUCH END-USERS
IN USING, DISPLAYING,  AND CREATING DERIVATIVE WORKS THEREOF, SO LONG AS
SUCH USE, DISPLAY OR CREATION IS FOR NON-COMMERCIAL, ROYALTY OR REVENUE
FREE PURPOSES.  IN NO EVENT SHALL THE END-USER USE THE COMPUTER CODE
CONTAINED HEREIN FOR REVENUE-BEARING PURPOSES.  THE END-USER UNDERSTANDS
AND AGREES TO THE TERMS HEREIN AND ACCEPTS THE SAME BY USE OF THIS FILE.
COPYRIGHT 1993-1999 PARALLAX SOFTWARE CORPORATION.  ALL RIGHTS RESERVED.
*/

/*
 *
 * Inferno main menu.
 *
 */

#include <stdio.h>
#include <string.h>

#include "menu.h"
#include "inferno.h"
#include "game.h"
#include "gr.h"
#include "xmodel.h"
#include "key.h"
#include "mouse.h"
#include "iff.h"
#include "u_mem.h"
#include "dxxerror.h"
#include "bm.h"
#include "screens.h"
#include "joy.h"
#include "vecmat.h"
#include "effects.h"
#include "slew.h"
#include "gamemine.h"
#include "gamesave.h"
#include "palette.h"
#include "args.h"
#include "newdemo.h"
#include "timer.h"
#include "sounds.h"
#include "gameseq.h"
#include "text.h"
#include "gamefont.h"
#include "newmenu.h"
#include "scores.h"
#include "playsave.h"
#include "kconfig.h"
#include "titles.h"
#include "credits.h"
#include "texmap.h"
#include "polyobj.h"
#include "state.h"
#include "mission.h"
#include "songs.h"
#ifdef USE_SDLMIXER
#include "jukebox.h" // for jukebox_exts
#endif
#include "config.h"
#include "movie.h"
#include "gamepal.h"
#include "gauges.h"
#include "powerup.h"
#include "strutil.h"
#include "multi.h"
#include "vers_id.h"
#ifdef USE_UDP
#include "net_udp.h"
#endif
#ifdef EDITOR
#include "editor/editor.h"
#include "editor/kdefs.h"
#endif
#ifdef OGL
#include "ogl_init.h"
#endif
#ifdef RT_DX12
#define RT_RENDER_SETTINGS_CONFIG_FILE "render_settings.vars"
#include "Core/String.h"
#include "Core/Config.h"
#include "Game/Lights.h"
#include "RTmaterials.h"
#include "RTgr.h"
#define GR_MAX_TEXTURE_PACKS 16 // pack folders shown in Graphics > Use Custom Textures
#endif


// Menu IDs...
enum MENUS
{
    MENU_NEW_GAME = 0,
    MENU_GAME,
    MENU_EDITOR,
    MENU_VIEW_SCORES,
    MENU_QUIT,
    MENU_LOAD_GAME,
    MENU_SAVE_GAME,
    MENU_DEMO_PLAY,
    MENU_CONFIG,
    MENU_REJOIN_NETGAME,
    MENU_DIFFICULTY,
    MENU_HELP,
    MENU_NEW_PLAYER,
    #if defined(USE_UDP)
        MENU_MULTIPLAYER,
    #endif

    MENU_SHOW_CREDITS,
    MENU_ORDER_INFO,

    #ifdef USE_UDP
    MENU_START_UDP_NETGAME,
    MENU_JOIN_MANUAL_UDP_NETGAME,
    MENU_JOIN_LIST_UDP_NETGAME,
    #endif
    #ifndef RELEASE
    MENU_SANDBOX
    #endif
};

//ADD_ITEM("Start netgame...", MENU_START_NETGAME, -1 );
//ADD_ITEM("Send net message...", MENU_SEND_NET_MESSAGE, -1 );

#define ADD_ITEM(t,value,key)  do { m[num_options].type=NM_TYPE_MENU; m[num_options].text=t; menu_choice[num_options]=value;num_options++; } while (0)

static window *menus[16] = { NULL };

// Function Prototypes added after LINTING
int do_option(int select);
void do_multi_player_menu();
#ifndef RELEASE
void do_sandbox_menu();
#endif
extern void newmenu_free_background();
extern void ReorderPrimary();
extern void ReorderSecondary();

// Hide all menus
int hide_menus(void)
{
	window *wind;
	int i;

	if (menus[0])
		return 0;		// there are already hidden menus

	for (i = 0; (i < 15) && (wind = window_get_front()); i++)
	{
		menus[i] = wind;
		window_set_visible(wind, 0);
	}

	Assert(window_get_front() == NULL);
	menus[i] = NULL;

	return 1;
}

// Show all menus, with the front one shown first
// This makes sure EVENT_WINDOW_ACTIVATED is only sent to that window
void show_menus(void)
{
	int i;

	for (i = 0; (i < 16) && menus[i]; i++)
		if (window_exists(menus[i]))
			window_set_visible(menus[i], 1);

	menus[0] = NULL;
}

//pairs of chars describing ranges
char playername_allowed_chars[] = "azAZ09__--";

int MakeNewPlayerFile(int allow_abort)
{
	int x;
	char filename[PATH_MAX];
	newmenu_item m;
	char text[CALLSIGN_LEN+9]="";

	strncpy(text, Players[Player_num].callsign,CALLSIGN_LEN);

try_again:
	m.type=NM_TYPE_INPUT; m.text_len = CALLSIGN_LEN; m.text = text;

	Newmenu_allowed_chars = playername_allowed_chars;
	x = newmenu_do( NULL, TXT_ENTER_PILOT_NAME, 1, &m, NULL, NULL );
	Newmenu_allowed_chars = NULL;

	if ( x < 0 ) {
		if ( allow_abort ) return 0;
		goto try_again;
	}

	if (text[0]==0)	//null string
		goto try_again;

	d_strlwr(text);

	memset(filename, '\0', PATH_MAX);
	snprintf( filename, PATH_MAX, GameArg.SysUsePlayersDir? "Players/%s.plr" : "%s.plr", text );

	if (PHYSFSX_exists(filename,0))
	{
		nm_messagebox(NULL, 1, TXT_OK, "%s '%s' %s", TXT_PLAYER, text, TXT_ALREADY_EXISTS );
		goto try_again;
	}

	if ( !new_player_config() )
		goto try_again;			// They hit Esc during New player config

	strncpy(Players[Player_num].callsign, text, CALLSIGN_LEN);
	d_strlwr(Players[Player_num].callsign);

	write_player_file();

	return 1;
}

void delete_player_saved_games(char * name);

int player_menu_keycommand( listbox *lb, d_event *event )
{
	char **items = listbox_get_items(lb);
	int citem = listbox_get_citem(lb);

	switch (event_key_get(event))
	{
		case KEY_CTRLED+KEY_D:
			if (citem > 0)
			{
				int x = 1;
				x = nm_messagebox( NULL, 2, TXT_YES, TXT_NO, "%s %s?", TXT_DELETE_PILOT, items[citem]+((items[citem][0]=='$')?1:0) );
				if (x==0)	{
					char * p;
					char plxfile[PATH_MAX], efffile[PATH_MAX], ngpfile[PATH_MAX];
					int ret;
					char name[PATH_MAX];

					p = items[citem] + strlen(items[citem]);
					*p = '.';

					strcpy(name, GameArg.SysUsePlayersDir ? "Players/" : "");
					strcat(name, items[citem]);

					ret = !PHYSFS_delete(name);
					*p = 0;

					if (!ret)
					{
						delete_player_saved_games( items[citem] );
						// delete PLX file
						sprintf(plxfile, GameArg.SysUsePlayersDir? "Players/%.8s.plx" : "%.8s.plx", items[citem]);
						if (PHYSFSX_exists(plxfile,0))
							PHYSFS_delete(plxfile);
						// delete EFF file
						sprintf(efffile, GameArg.SysUsePlayersDir? "Players/%.8s.eff" : "%.8s.eff", items[citem]);
						if (PHYSFSX_exists(efffile,0))
							PHYSFS_delete(efffile);
						// delete NGP file
						sprintf(ngpfile, GameArg.SysUsePlayersDir? "Players/%.8s.ngp" : "%.8s.ngp", items[citem]);
						if (PHYSFSX_exists(ngpfile,0))
							PHYSFS_delete(ngpfile);
					}

					if (ret)
						nm_messagebox( NULL, 1, TXT_OK, "%s %s %s", TXT_COULDNT, TXT_DELETE_PILOT, items[citem]+((items[citem][0]=='$')?1:0) );
					else
						listbox_delete_item(lb, citem);
				}

				return 1;
			}
			break;
	}

	return 0;
}

int player_menu_handler( listbox *lb, d_event *event, char **list )
{
	char **items = listbox_get_items(lb);
	int citem = listbox_get_citem(lb);

	switch (event->type)
	{
		case EVENT_KEY_COMMAND:
			return player_menu_keycommand(lb, event);
			break;

		case EVENT_NEWMENU_SELECTED:
			if (citem < 0)
				return 0;		// shouldn't happen
			else if (citem == 0)
			{
				// They selected 'create new pilot'
				return !MakeNewPlayerFile(1);
			}
			else
			{
				strncpy(Players[Player_num].callsign,items[citem] + ((items[citem][0]=='$')?1:0), CALLSIGN_LEN);
				d_strlwr(Players[Player_num].callsign);
			}
			break;

		case EVENT_WINDOW_CLOSE:
			if (read_player_file() != EZERO)
				return 1;		// abort close!

			WriteConfigFile();		// Update lastplr

			PHYSFS_freeList(list);
			d_free(items);
			break;

		default:
			break;
	}

	return 0;
}

//Inputs the player's name, without putting up the background screen
int RegisterPlayer()
{
	char **m;
	char **f;
	char **list;
	static const char *const types[] = { ".plr", NULL };
	int i = 0, NumItems;
	int citem = 0;
	int allow_abort_flag = 1;

	if ( Players[Player_num].callsign[0] == 0 )
	{
		if (GameCfg.LastPlayer[0]==0)
		{
			strncpy( Players[Player_num].callsign, "player", CALLSIGN_LEN );
			allow_abort_flag = 0;
		}
		else
		{
			// Read the last player's name from config file, not lastplr.txt
			strncpy( Players[Player_num].callsign, GameCfg.LastPlayer, CALLSIGN_LEN );
		}
	}

	list = PHYSFSX_findFiles(GameArg.SysUsePlayersDir ? "Players/" : "", types);
	if (!list)
		return 0;	// memory error
	if (!*list)
	{
		MakeNewPlayerFile(0);	// make a new player without showing listbox
		PHYSFS_freeList(list);
		return 0;
	}


	for (NumItems = 0; list[NumItems] != NULL; NumItems++) {}
	NumItems++;		// for TXT_CREATE_NEW

	MALLOC(m, char *, NumItems);
	if (m == NULL)
	{
		PHYSFS_freeList(list);
		return 0;
	}

	m[i++] = TXT_CREATE_NEW;

	for (f = list; *f != NULL; f++)
	{
		char *p;

		if (strlen(*f) > FILENAME_LEN-1 || strlen(*f) < 5) // sorry guys, can only have up to eight chars for the player name
		{
			NumItems--;
			continue;
		}
		m[i++] = *f;
		p = strchr(*f, '.');
		if (p)
			*p = '\0';		// chop the .plr
	}

	if (NumItems <= 1) // so it seems all plr files we found were too long. funny. let's make a real player
	{
		MakeNewPlayerFile(0);	// make a new player without showing listbox
		PHYSFS_freeList(list);
		return 0;
	}

	// Sort by name, except the <Create New Player> string
	qsort(&m[1], NumItems - 1, sizeof(char *), (int (*)( const void *, const void * ))string_array_sort_func);

	for ( i=0; i<NumItems; i++ )
		if (!d_stricmp(Players[Player_num].callsign, m[i]) )
			citem = i;

	newmenu_listbox1(TXT_SELECT_PILOT, NumItems, m, allow_abort_flag, citem, (int (*)(listbox *, d_event *, void *))player_menu_handler, list);

	return 1;
}

// Draw Copyright and Version strings
void draw_copyright()
{
	gr_set_current_canvas(NULL);
	gr_set_curfont(GAME_FONT);
	gr_set_fontcolor(BM_XRGB(6,6,6),-1);
	gr_string(0x8000,SHEIGHT-LINE_SPACING,TXT_COPYRIGHT);
	{
		// The title palette has no true red (any red lands on orange). OGL reads the text colour from
		// gr_current_pal at draw time, so set one entry to exact red (DEMO RECORDING's 27,0,0) just for this line.
		int c = BM_XRGB(27,0,0);
		ubyte saved[3];
		memcpy(saved, &gr_current_pal[c*3], 3);
		gr_current_pal[c*3] = 54; gr_current_pal[c*3+1] = 0; gr_current_pal[c*3+2] = 0;
		gr_set_fontcolor(c, -1);
		gr_string(0x8000,SHEIGHT-(LINE_SPACING*2),GameArg.SysOpenVR ? DESCENT_VERSION VERSION_EXTRA_MAIN_SCREEN_VR : DESCENT_VERSION VERSION_EXTRA_MAIN_SCREEN);
		memcpy(&gr_current_pal[c*3], saved, 3);
	}
}

//returns the number of demo files on the disk
int newdemo_count_demos();

// ------------------------------------------------------------------------
int main_menu_handler(newmenu *menu, d_event *event, int *menu_choice )
{
	newmenu_item *items = newmenu_get_items(menu);

	switch (event->type)
	{
		case EVENT_WINDOW_ACTIVATED:
			load_palette(MENU_PALETTE,0,1);		//get correct palette

			if ( Players[Player_num].callsign[0]==0 )
				RegisterPlayer();
			else
				keyd_time_when_last_pressed = timer_query();		// .. 20 seconds from now!
			break;

		case EVENT_KEY_COMMAND:
			// Don't allow them to hit ESC in the main menu.
			if (event_key_get(event)==KEY_ESC)
				return 1;
			break;

		case EVENT_MOUSE_BUTTON_DOWN:
		case EVENT_MOUSE_BUTTON_UP:
			// Don't allow mousebutton-closing in main menu.
			if (event_mouse_get_button(event) == MBTN_RIGHT)
				return 1;
			break;

		case EVENT_IDLE:
			if ( /*keyd_time_when_last_pressed+i2f(25) < timer_query() || */ GameArg.SysAutoDemo  )
			{
				int n_demos;
				n_demos = newdemo_count_demos();
				keyd_time_when_last_pressed = timer_query();			// Reset timer so that disk won't thrash if no demos.

				if (((d_rand() % (n_demos+1)) == 0) && !GameArg.SysAutoDemo)
				{
#ifdef OGL
					Screen_mode = -1;
#endif
					init_subtitles("intro.tex");
					PlayMovie("intro.mve",0);
					close_subtitles();
					songs_play_song(SONG_TITLE,1);
					set_screen_mode(SCREEN_MENU);
				}
				else
				{
					newdemo_start_playback(NULL);		// Randomly pick a file, assume native endian (crashes if not)
					if (Newdemo_state == ND_STATE_PLAYBACK)
						return 0;
				}
			}
			break;

		case EVENT_NEWMENU_DRAW:
			draw_copyright();
			break;

		case EVENT_NEWMENU_SELECTED:
			return do_option(menu_choice[newmenu_get_citem(menu)]);
			break;

		case EVENT_WINDOW_CLOSE:
			d_free(menu_choice);
			d_free(items);
			break;

		default:
			break;
	}

	return 0;
}

//	-----------------------------------------------------------------------------
//	Create the main menu.
void create_main_menu(newmenu_item *m, int *menu_choice, int *callers_num_options)
{
	int	num_options;

	#ifndef DEMO_ONLY
	num_options = 0;

	ADD_ITEM(TXT_NEW_GAME,MENU_NEW_GAME,KEY_N);

	ADD_ITEM(TXT_LOAD_GAME,MENU_LOAD_GAME,KEY_L);
#if defined(USE_UDP)
	ADD_ITEM(TXT_MULTIPLAYER_,MENU_MULTIPLAYER,-1);
#endif

	ADD_ITEM(TXT_OPTIONS_, MENU_CONFIG, -1 );
	ADD_ITEM(TXT_CHANGE_PILOTS,MENU_NEW_PLAYER,unused);
	ADD_ITEM(TXT_VIEW_DEMO,MENU_DEMO_PLAY,0);
	ADD_ITEM(TXT_VIEW_SCORES,MENU_VIEW_SCORES,KEY_V);
	if (PHYSFSX_exists("orderd2.pcx",1)) /* SHAREWARE */
		ADD_ITEM(TXT_ORDERING_INFO,MENU_ORDER_INFO,-1);
	ADD_ITEM(TXT_CREDITS,MENU_SHOW_CREDITS,-1);
	#endif
	ADD_ITEM(TXT_QUIT,MENU_QUIT,KEY_Q);

	#ifndef RELEASE
	if (!(Game_mode & GM_MULTI ))	{
		//m[num_options].type=NM_TYPE_TEXT;
		//m[num_options++].text=" Debug options:";

		#ifdef EDITOR
		ADD_ITEM("  Editor", MENU_EDITOR, KEY_E);
		#endif
	}
	/* The sandbox viewers are developer tools, so only offer them when the
	 * game was started with -debug. */
	if (GameArg.DbgVerbose >= CON_DEBUG)
		ADD_ITEM("  SANDBOX", MENU_SANDBOX, -1);
	#endif

	*callers_num_options = num_options;
}

//returns number of item chosen
int DoMenu()
{
	int *menu_choice;
	newmenu_item *m;
	int num_options = 0;

	MALLOC(menu_choice, int, 25);
	if (!menu_choice)
		return -1;
	MALLOC(m, newmenu_item, 25);
	if (!m)
	{
		d_free(menu_choice);
		return -1;
	}

	memset(menu_choice, 0, sizeof(int)*25);
	memset(m, 0, sizeof(newmenu_item)*25);

	create_main_menu(m, menu_choice, &num_options); // may have to change, eg, maybe selected pilot and no save games.

	// Nudge the main menu slightly right of centre. Authored in pixels against the
	// background art; scaled to the canvas width at runtime.
	newmenu_set_x_nudge(15);

	newmenu_do3( "", NULL, num_options, m, (int (*)(newmenu *, d_event *, void *))main_menu_handler, menu_choice, 0, Menu_pcx_name);

	return 0;
}

extern void show_order_form(void);	// John didn't want this in inferno.h so I just externed it.

//returns flag, true means quit menu
int do_option ( int select)
{
	switch (select) {
		case MENU_NEW_GAME:
			select_mission(0, "New Game\n\nSelect mission", do_new_game_menu);
			break;
		case MENU_GAME:
			break;
		case MENU_DEMO_PLAY:
			select_demo();
			break;
		case MENU_LOAD_GAME:
			state_restore_all(0, 0, NULL);
			break;
		#ifdef EDITOR
		case MENU_EDITOR:
			if (!Current_mission)
			{
				create_new_mine();
				SetPlayerFromCurseg();
			}

			hide_menus();
			init_editor();
			break;
		#endif
		case MENU_VIEW_SCORES:
			scores_view(NULL, -1);
			break;
#if 1 //def SHAREWARE
		case MENU_ORDER_INFO:
			show_order_form();
			break;
#endif
		case MENU_QUIT:
			#ifdef EDITOR
			if (! SafetyCheck()) break;
			#endif
			return 0;

		case MENU_NEW_PLAYER:
			RegisterPlayer();
			break;

#ifdef USE_UDP
		case MENU_START_UDP_NETGAME:
			multi_protocol = MULTI_PROTO_UDP;
			select_mission(1, TXT_MULTI_MISSION, net_udp_setup_game);
			break;
		case MENU_JOIN_MANUAL_UDP_NETGAME:
			multi_protocol = MULTI_PROTO_UDP;
			net_udp_manual_join_game();
			break;
		case MENU_JOIN_LIST_UDP_NETGAME:
			multi_protocol = MULTI_PROTO_UDP;
			net_udp_list_join_game();
			break;
#endif
#if defined(USE_UDP)
		case MENU_MULTIPLAYER:
			do_multi_player_menu();
			break;
#endif
		case MENU_CONFIG:
			do_options_menu();
			break;
		case MENU_SHOW_CREDITS:
			credits_show(NULL);
			break;
#ifndef RELEASE
		case MENU_SANDBOX:
			do_sandbox_menu();
			break;
#endif
		default:
			Error("Unknown option %d in do_option",select);
			break;
	}

	return 1;		// stay in main menu unless quitting
}

void delete_player_saved_games(char * name)
{
	int i;
	char filename[PATH_MAX];

	for (i=0;i<10; i++)
	{
		snprintf( filename, PATH_MAX, GameArg.SysUsePlayersDir? "Players/%s.sg%x" : "%s.sg%x", name, i );
		PHYSFS_delete(filename);
		snprintf( filename, PATH_MAX, GameArg.SysUsePlayersDir? "Players/%s.mg%x" : "%s.mg%x", name, i );
		PHYSFS_delete(filename);
	}
}

int demo_menu_keycommand( listbox *lb, d_event *event )
{
	char **items = listbox_get_items(lb);
	int citem = listbox_get_citem(lb);

	switch (event_key_get(event))
	{
		case KEY_CTRLED+KEY_D:
			if (citem >= 0)
			{
				int x = 1;
				x = nm_messagebox( NULL, 2, TXT_YES, TXT_NO, "%s %s?", TXT_DELETE_DEMO, items[citem]+((items[citem][0]=='$')?1:0) );
				if (x==0)
				{
					int ret;
					char name[PATH_MAX];

					strcpy(name, DEMO_DIR);
					strcat(name,items[citem]);

					ret = !PHYSFS_delete(name);

					if (ret)
						nm_messagebox( NULL, 1, TXT_OK, "%s %s %s", TXT_COULDNT, TXT_DELETE_DEMO, items[citem]+((items[citem][0]=='$')?1:0) );
					else
						listbox_delete_item(lb, citem);
				}

				return 1;
			}
			break;

		case KEY_CTRLED+KEY_C:
			{
				int x = 1;
				char bakname[PATH_MAX];

				// Get backup name
				change_filename_extension(bakname, items[citem]+((items[citem][0]=='$')?1:0), DEMO_BACKUP_EXT);
				x = nm_messagebox( NULL, 2, TXT_YES, TXT_NO,	"Are you sure you want to\n"
								  "swap the endianness of\n"
								  "%s? If the file is\n"
								  "already endian native, D1X\n"
								  "will likely crash. A backup\n"
								  "%s will be created", items[citem]+((items[citem][0]=='$')?1:0), bakname );
				if (!x)
					newdemo_swap_endian(items[citem]);

				return 1;
			}
			break;
	}

	return 0;
}

int demo_menu_handler( listbox *lb, d_event *event, void *userdata )
{
	char **items = listbox_get_items(lb);
	int citem = listbox_get_citem(lb);

	userdata = userdata;

	switch (event->type)
	{
		case EVENT_KEY_COMMAND:
			return demo_menu_keycommand(lb, event);
			break;

		case EVENT_NEWMENU_SELECTED:
			if (citem < 0)
				return 0;		// shouldn't happen

			newdemo_start_playback(items[citem]);
			return 1;		// stay in demo selector

		case EVENT_WINDOW_CLOSE:
			PHYSFS_freeList(items);
			break;

		default:
			break;
	}

	return 0;
}

int select_demo(void)
{
	char **list;
	static const char *const types[] = { DEMO_EXT, NULL };
	int NumItems;

	list = PHYSFSX_findFiles(DEMO_DIR, types);
	if (!list)
		return 0;	// memory error
	if ( !*list )
	{
		nm_messagebox( NULL, 1, TXT_OK, "%s %s\n%s", TXT_NO_DEMO_FILES, TXT_USE_F5, TXT_TO_CREATE_ONE);
		PHYSFS_freeList(list);
		return 0;
	}

	for (NumItems = 0; list[NumItems] != NULL; NumItems++) {}

	// Sort by name
	qsort(list, NumItems, sizeof(char *), (int (*)( const void *, const void * ))string_array_sort_func);

	newmenu_listbox1(TXT_SELECT_DEMO, NumItems, list, 1, 0, demo_menu_handler, NULL);

	return 1;
}

int do_difficulty_menu()
{
	int s;
	newmenu_item m[5];

	m[0].type=NM_TYPE_MENU; m[0].text=MENU_DIFFICULTY_TEXT(0);
	m[1].type=NM_TYPE_MENU; m[1].text=MENU_DIFFICULTY_TEXT(1);
	m[2].type=NM_TYPE_MENU; m[2].text=MENU_DIFFICULTY_TEXT(2);
	m[3].type=NM_TYPE_MENU; m[3].text=MENU_DIFFICULTY_TEXT(3);
	m[4].type=NM_TYPE_MENU; m[4].text=MENU_DIFFICULTY_TEXT(4);

	s = newmenu_do1( NULL, TXT_DIFFICULTY_LEVEL, NDL, m, NULL, NULL, Difficulty_level);

	if (s > -1 )	{
		if (s != Difficulty_level)
		{
			PlayerCfg.DefaultDifficulty = s;
			write_player_file();
		}
		Difficulty_level = s;
		return 1;
	}
	return 0;
}

int do_new_game_menu()
{
	int new_level_num,player_highest_level;

	new_level_num = 1;
#ifdef SHIPPING_BUILD
	player_highest_level = get_highest_level();

	if (player_highest_level > Last_level)
#endif
		player_highest_level = Last_level;
	if (player_highest_level > 1) {
		newmenu_item m[4];
		char info_text[80];
		char num_text[10];
		int choice;
		int n_items;
		int valid = 0;

		while (!valid)
		{
			sprintf(info_text,"%s %d",TXT_START_ANY_LEVEL, player_highest_level);

			m[0].type=NM_TYPE_TEXT; m[0].text = info_text;
			m[1].type=NM_TYPE_INPUT; m[1].text_len = 10; m[1].text = num_text;
			n_items = 2;

			strcpy(num_text,"1");

			choice = newmenu_do( NULL, TXT_SELECT_START_LEV, n_items, m, NULL, NULL );

			if (choice==-1 || m[1].text[0]==0)
				return 0;

			new_level_num = atoi(m[1].text);

			if (!(new_level_num>0 && new_level_num<=player_highest_level)) {
				m[0].text = TXT_ENTER_TO_CONT;
				nm_messagebox( NULL, 1, TXT_OK, TXT_INVALID_LEVEL);
				valid = 0;
			}
			else
				valid = 1;
		}
	}

	Difficulty_level = PlayerCfg.DefaultDifficulty;

	if (!do_difficulty_menu())
		return 0;

	StartNewGame(new_level_num);

	return 1;	// exit mission listbox
}

void do_sound_menu();
void input_config();
void change_res();
void graphics_config();
void do_misc_menu();
void do_obs_menu();

int options_menuset(newmenu *menu, d_event *event, void *userdata)
{
	switch (event->type)
	{
		case EVENT_NEWMENU_CHANGED:
			break;

		case EVENT_NEWMENU_SELECTED:
			switch(newmenu_get_citem(menu))
			{
				case  0: do_sound_menu();		break;
				case  2: input_config();		break;
				case  4: change_res();			break;
				case  5: graphics_config();		break;
				case  7: ReorderPrimary();		break;
				case  8: ReorderSecondary();		break;
				case  9: do_misc_menu();		break;
				case 10: do_obs_menu();         break;
			}
			return 1;	// stay in menu until escape
			break;

		case EVENT_WINDOW_CLOSE:
		{
			newmenu_item *items = newmenu_get_items(menu);
			d_free(items);
			write_player_file();
			break;
		}

		default:
			break;
	}

	userdata = userdata;		//kill warning

	return 0;
}

int gcd(int a, int b)
{
	if (!b)
		return a;

	return gcd(b, a%b);
}

// Screen Resolution menu. "use custom values" is a checkbox: while it is ticked the aspect field is a
// live filter on the list, custom resolutions autofill to that shape and up to 2 of them stay in the
// list. Unticking it clears all that and puts the menu back to default. Saved in descent.cfg.
#define RES_REBUILD_TOGGLE 1 // the checkbox changed
#define RES_REBUILD_ASPECT 2 // Enter on the aspect field
static int res_opt_custom, res_opt_res, res_opt_aspect, res_num_list, res_opt_fullscr, res_opt_borderless, res_apply_window;
static int res_rebuild, res_focus, res_apply_custom, res_enter_item;
static int res_click_ticked = -1; // the resolution that was ticked when the mouse button went down
// The highlighted item, kept across rebuilds: a list line by its index, an item below the list by
// its place after the list, so the cursor stays on the same field when the list grows or shrinks
#define RES_FOCUS(citem) ((citem) < res_num_list ? (citem) : 1000 + (citem) - res_num_list)

// Reads "16x9", "16:9" or "1600x900" into a and b; 0 if it isn't a pair of positive numbers
static int res_parse_pair(const char *s, int *a, int *b)
{
	const char *sep = strpbrk(s, "xX:");
	if (!sep)
		return 0;
	*a = atoi(s);
	*b = atoi(sep + 1);
	return *a > 0 && *b > 0;
}

// Whether a mode has the filter's shape, within 3% (1360x768 counts as 16:9, 2560x1080 as 21:9)
static int res_in_filter(u_int32_t mode)
{
	float want, got;
	if (!GameCfg.ResCustom || !GameCfg.ResFilterW || !GameCfg.ResFilterH)
		return 1;
	want = (float)GameCfg.ResFilterW / GameCfg.ResFilterH;
	got = (float)SM_W(mode) / SM_H(mode);
	return got >= want * 0.97f && got <= want * 1.03f;
}

// The shapes the arrow keys step through in the aspect field; 0x0 = every shape
static const char *const res_aspect_presets[] = { "0x0", "4x3", "5x4", "16x10", "16x9", "21x9" };
#define RES_NUM_ASPECT_PRESETS (sizeof(res_aspect_presets) / sizeof(res_aspect_presets[0]))

// Common 16:9 and 16:10 sizes always offered in the list (monitors rarely list them; they work windowed)
static const u_int32_t res_small_wide[] = { SM(960, 540), SM(854, 480),
	SM(1024, 640), SM(1152, 720), SM(1280, 800), SM(1440, 900), SM(1680, 1050) };
#define RES_NUM_SMALL_WIDE (sizeof(res_small_wide) / sizeof(res_small_wide[0]))

// The modes the monitor lists, while the menu is open (the arrows in the resolution field step through them)
static const u_int32_t *res_all_modes;
static int res_all_n;

// 1 while the aspect field is 0x0: the aspect then follows the typed resolution instead of filling it
static int res_aspect_auto;


// Fills the other side of the resolution for the filter's shape, from what has been typed so far
// (the text before the edit position):
// - a width: 555 -> 555x312, edit position stays after the width
// - past the x (typed x, or the right arrow), a height: x312 -> 555x312, edit position stays at the end
// so typing and backspace carry on from there.
static void res_autofill(newmenu_item *it, int aw, int ah)
{
	char digits[6], *x;
	int i, n = 0, len = (it->value < 0) ? (int)strlen(it->text) : it->value;

	it->text[len] = 0;
	x = strpbrk(it->text, "xX");
	for (i = x ? (int)(x - it->text) + 1 : 0; it->text[i] && n < 5; i++)
		if (it->text[i] >= '0' && it->text[i] <= '9')
			digits[n++] = it->text[i];
	digits[n] = 0;

	if (x) // the height was typed: fill in the width
	{
		if (n)
			snprintf(it->text, it->text_len + 1, "%ix%s", (atoi(digits) * aw + ah / 2) / ah, digits);
		else
			x[1] = 0; // just the x so far: keep the width, wait for the height
		it->value = strlen(it->text);
	}
	else // the width was typed: fill in the height
	{
		if (n)
			snprintf(it->text, it->text_len + 1, "%sx%i", digits, (atoi(digits) * ah + aw / 2) / aw);
		else
			it->text[0] = 0;
		it->value = n;
	}
}

// The shape in the aspect field, if it holds one and isn't following the typed resolution
static int res_field_aspect(newmenu_item *items, int *aw, int *ah)
{
	return !res_aspect_auto && res_parse_pair(items[res_opt_aspect].text, aw, ah);
}

// Re-fills the resolution field from its width after the aspect changed
static void res_refill(newmenu_item *items)
{
	newmenu_item *it = &items[res_opt_res];
	char *x = strpbrk(it->text, "xX");
	int aw, ah;

	if (!res_field_aspect(items, &aw, &ah))
		return;
	it->value = x ? (int)(x - it->text) : (int)strlen(it->text);
	res_autofill(it, aw, ah);
	it->value = -1; // the next key typed starts over, as on entering the field
}

// The next bigger (up) or smaller known size of the aspect field's shape (any shape at 0x0), from
// the monitor's modes, the common sizes and your custom ones, put in the resolution field
static void res_step(newmenu_item *items, int up)
{
	newmenu_item *it = &items[res_opt_res];
	char *x = strpbrk(it->text, "xX");
	int cw = atoi(it->text), ch = x ? atoi(x + 1) : 0, aw, ah, shape, k, n;
	long cur, a, best_a = 0;
	u_int32_t cand, best = 0;

	shape = res_field_aspect(items, &aw, &ah);
	if (!ch && shape)
		ch = cw * ah / aw;
	cur = (long)cw * ch;
	n = res_all_n + RES_NUM_SMALL_WIDE + 2;
	for (k = 0; k < n; k++)
	{
		cand = (k < res_all_n) ? res_all_modes[k] : (k < res_all_n + (int)RES_NUM_SMALL_WIDE) ? res_small_wide[k - res_all_n] :
			(GameCfg.ResCustom ? GameCfg.ResExtra[k - res_all_n - RES_NUM_SMALL_WIDE] : 0);
		if (!cand)
			continue;
		if (shape) // within 3% of the shape, like the list filter
		{
			float want = (float)aw / ah, got = (float)SM_W(cand) / SM_H(cand);
			if (got < want * 0.97f || got > want * 1.03f)
				continue;
		}
		a = (long)SM_W(cand) * SM_H(cand);
		if (up ? (a > cur && (!best || a < best_a)) : (a < cur && (!best || a > best_a)))
		{
			best = cand;
			best_a = a;
		}
	}
	if (!best)
		return;
	snprintf(it->text, it->text_len + 1, "%ix%i", SM_W(best), SM_H(best));
	it->value = up ? (int)strlen(it->text) : 0; // the cursor stays at the end it stepped from
	if (res_aspect_auto) // 0x0: the aspect follows the resolution
		snprintf(items[res_opt_aspect].text, items[res_opt_aspect].text_len + 1, "%ix%i", SM_W(best) / gcd(SM_W(best), SM_H(best)), SM_H(best) / gcd(SM_W(best), SM_H(best)));
}

// The resolution field edits like a normal text box: the arrows, Home and End move the cursor, typing
// inserts at it, Backspace and Delete remove. Right at the end steps to the next bigger size of the
// aspect's shape, left at the start to the next smaller. Editing the width fills in the height from
// the aspect as you type digits; deleting never refills. A height you type yourself is kept. Returns 0 for keys the menu handles (Enter, Esc, up, down).
static int res_field_key(newmenu_item *items, int key)
{
	newmenu_item *it = &items[res_opt_res];
	int len = strlen(it->text), cur = (it->value < 0 || it->value > len) ? len : it->value, ascii, aw, ah, typed = 0;
	char *x;

	if (key == (KEY_DELETE | KEY_DEBUGGED)) // the key handler marks keys pressed while Delete is down, Delete itself too
		key = KEY_DELETE;
	switch (key)
	{
		case KEY_LEFT: case KEY_PAD4:
			if (cur > 0)
				it->value = cur - 1;
			else
				res_step(items, 0);
			return 1;
		case KEY_RIGHT: case KEY_PAD6:
			if (cur < len)
				it->value = cur + 1;
			else
				res_step(items, 1);
			return 1;
		case KEY_HOME:
			it->value = 0;
			return 1;
		case KEY_END:
			it->value = len;
			return 1;
		case KEY_BACKSP:
			if (cur == 0)
				return 1;
			memmove(it->text + cur - 1, it->text + cur, len - cur + 1);
			cur--;
			break;
		case KEY_DELETE:
			if (cur == len)
				return 1;
			memmove(it->text + cur, it->text + cur + 1, len - cur);
			break;
		case KEY_ENTER: case KEY_PADENTER: case KEY_ESC: case KEY_UP: case KEY_DOWN: case KEY_PAD8: case KEY_PAD2:
		case KEY_TAB: case KEY_PAGEUP: case KEY_PAGEDOWN:
			return 0;
		default:
			ascii = key_ascii();
			if (ascii == 255)
				return 0;
			if (ascii == 'x' || ascii == 'X')
			{
				if ((x = strpbrk(it->text, "xX"))) // already has its x: jump past it
				{
					it->value = (int)(x - it->text) + 1;
					return 1;
				}
				ascii = 'x';
			}
			else if (ascii < '0' || ascii > '9')
				return 1; // digits and x only
			if (len >= it->text_len)
				return 1;
			memmove(it->text + cur + 1, it->text + cur, len - cur + 1);
			it->text[cur++] = ascii;
			typed = (ascii != 'x');
			break;
	}
	it->value = cur;
	x = strpbrk(it->text, "xX");
	if (res_field_aspect(items, &aw, &ah))
	{
		if (typed && (!x || cur <= (int)(x - it->text)) && atoi(it->text) > 0) // a digit typed in the width: the height follows the shape (deleting just deletes)
			snprintf(it->text, it->text_len + 1, "%ix%i", atoi(it->text), (atoi(it->text) * ah + aw / 2) / aw);
	}
	else if (res_aspect_auto && res_parse_pair(it->text, &aw, &ah)) // 0x0: the aspect follows the resolution
		snprintf(items[res_opt_aspect].text, items[res_opt_aspect].text_len + 1, "%ix%i", aw / gcd(aw, ah), ah / gcd(aw, ah));
	return 1;
}

static int change_res_handler(newmenu *menu, d_event *event, void *userdata)
{
	newmenu_item *items = newmenu_get_items(menu);
	int citem = newmenu_get_citem(menu), i, aw, ah, key;

	userdata = userdata;

	switch (event->type)
	{
		// Mouse: a click ticks a resolution; a click on the one already ticked applies it, like Enter
		case EVENT_MOUSE_BUTTON_DOWN:
			if (event_mouse_get_button(event) == MBTN_LEFT)
				for (res_click_ticked = -1, i = 0; i < res_num_list; i++)
					if (items[i].value)
						res_click_ticked = i;
			break;

		case EVENT_MOUSE_BUTTON_UP:
			if (event_mouse_get_button(event) == MBTN_LEFT && citem < res_num_list && citem == res_click_ticked && items[citem].value)
			{
				res_focus = RES_FOCUS(citem);
				res_enter_item = citem;
				window_close(newmenu_get_window(menu));
				return 1;
			}
			break;

		case EVENT_KEY_COMMAND:
			key = event_key_get(event);
			if (citem == res_opt_aspect && (key == KEY_LEFT || key == KEY_PAD4 || key == KEY_RIGHT || key == KEY_PAD6))
			{
				// arrows step through the preset shapes
				int idx = -1, n = RES_NUM_ASPECT_PRESETS, w = 0, h = 0, pw, ph;
				res_parse_pair(items[citem].text, &w, &h);
				for (i = 0; i < n; i++)
				{
					if (!res_parse_pair(res_aspect_presets[i], &pw, &ph))
						pw = ph = 0;
					if ((w == 0 && pw == 0) || (w && pw && w * ph == h * pw))
						idx = i;
				}
				if (key == KEY_RIGHT || key == KEY_PAD6)
					idx = (idx + 1) % n;
				else
					idx = (idx < 0) ? n - 1 : (idx + n - 1) % n;
				strcpy(items[citem].text, res_aspect_presets[idx]);
				items[citem].value = -1;
				res_aspect_auto = (idx == 0);
				res_refill(items);
				return 1;
			}
			if (citem == res_opt_res)
				return res_field_key(items, key);
			break;

		case EVENT_NEWMENU_CHANGED:
			if (citem == res_opt_custom) // ticked or unticked: rebuild the menu for it
			{
				res_rebuild = RES_REBUILD_TOGGLE;
				res_focus = RES_FOCUS(citem);
				window_close(newmenu_get_window(menu));
				return 1;
			}
			if (citem == res_opt_aspect) // typed a shape: the resolution follows it; typed 0x0: the other way round
			{
				if (res_parse_pair(items[citem].text, &aw, &ah))
				{
					res_aspect_auto = 0;
					res_refill(items);
				}
				else if (strpbrk(items[citem].text, "xX:") && atoi(items[citem].text) == 0)
					res_aspect_auto = 1;
			}
			break;

		case EVENT_NEWMENU_SELECTED: // Enter. Only the ticked, highlighted resolution changes the screen.
			res_focus = RES_FOCUS(citem);
			if (citem == res_opt_custom)
			{
				items[citem].value = !items[citem].value;
				res_rebuild = RES_REBUILD_TOGGLE;
			}
			else if (citem == res_opt_aspect)
				res_rebuild = RES_REBUILD_ASPECT;
			else if (citem == res_opt_res)
			{
				if (items[res_opt_custom].value)
					res_apply_custom = 1; // box ticked: apply the typed resolution
				else
				{
					items[res_opt_custom].value = 1; // box unticked: tick it first
					res_rebuild = RES_REBUILD_TOGGLE;
				}
			}
			else if (citem < res_num_list)
			{
				if (items[citem].value)
					res_enter_item = citem; // already ticked: apply it
				else
				{
					for (i = 0; i < res_num_list; i++) // not ticked yet: tick it and stay
						items[i].value = (i == citem);
					return 1;
				}
			}
			else if (citem == res_opt_fullscr || citem == res_opt_borderless)
			{
				// Space ticks and unticks. Enter on a ticked box applies it. Enter on an empty box does
				// nothing, but two quick Enters tick it and apply it, like a double click.
				static fix64 last_enter;
				static int last_item = -1;
				fix64 now = timer_query();

				if (!items[citem].value)
				{
					if (last_item != citem || now - last_enter > F1_0 / 2)
					{
						last_item = citem;
						last_enter = now;
						return 1;
					}
					items[citem].value = 1;
				}
				last_item = -1;
				res_apply_window = 1;
			}
			else
				return 1; // anything else: stay in the menu
			break;

		default:
			break;
	}
	return 0;
}

// Applies a screen mode plus the fullscreen and borderless settings
static void res_apply(u_int32_t new_mode, int fullscr, int borderless)
{
	int cur_borderless = GameCfg.BorderlessWindow;

	GameCfg.BorderlessWindow = borderless;

	// now check for fullscreen toggle and apply if necessary
	if (fullscr != gr_check_fullscreen())
		gr_toggle_fullscreen();

	// clean up and apply everything
	newmenu_free_background();
	set_screen_mode(SCREEN_MENU);
	if (new_mode != Game_screen_mode || cur_borderless != borderless)
	{
		gr_set_mode(new_mode);
		Game_screen_mode = new_mode;
		if (Game_wind) // shortly activate Game_wind so it's canvas will align to new resolution. really minor glitch but whatever
		{
			d_event event;
			WINDOW_SEND_EVENT(Game_wind, EVENT_WINDOW_ACTIVATED);
			WINDOW_SEND_EVENT(Game_wind, EVENT_WINDOW_DEACTIVATED);
		}
	}
	game_init_render_buffers(SM_W(Game_screen_mode), SM_H(Game_screen_mode));
}

// The resolution list as shown in the menu, for counting steps above the monitor (NULL outside the menu)
static const u_int32_t *res_steps;
static int res_steps_n;

// Whether a mode is too far above the monitor's own size to open without asking. Going up the list
// from the monitor's size, the next two sizes open straight away and the third asks first (a typed
// size counts by where it would sit in the list). Anything more than 1.5x the monitor on a side asks too.
// The monitor's size is the screen the window is on; before there is a window, the largest listed mode.
static int res_too_big(u_int32_t mode, const u_int32_t *modes, int n, int *maxw, int *maxh)
{
	const u_int32_t *list = res_steps ? res_steps : modes;
	int i, nlist = res_steps ? res_steps_n : n, steps = 0;
	long area = (long)SM_W(mode) * SM_H(mode), native;

	if (!gr_monitor_size(maxw, maxh))
	{
		*maxw = *maxh = 0;
		for (i = 0; i < n; i++)
		{
			if (SM_W(modes[i]) > *maxw)
				*maxw = SM_W(modes[i]);
			if (SM_H(modes[i]) > *maxh)
				*maxh = SM_H(modes[i]);
		}
		if (!*maxw)
			return 0;
	}
	native = (long)*maxw * *maxh;
	if (area <= native)
		return 0;
	for (i = 0; i < nlist; i++) // listed sizes between the monitor's and this one
	{
		long a = (long)SM_W(list[i]) * SM_H(list[i]);
		if (a > native && a < area)
			steps++;
	}
	return steps >= 2 || SM_W(mode) * 2 > *maxw * 3 || SM_H(mode) * 2 > *maxh * 3;
}

// Startup: a saved resolution far bigger than the monitor isn't opened straight away. The game
// starts at the default 960x540 and asks, once menus can show, whether to go back to it.
static u_int32_t res_startup_big;

void res_startup_check(void) // before the first gr_set_mode
{
	u_int32_t modes[50];
	int maxw, maxh;

	res_startup_big = 0;
	if (res_too_big(Game_screen_mode, modes, gr_list_modes(modes), &maxw, &maxh))
	{
		res_startup_big = Game_screen_mode;
		Game_screen_mode = SM(960, 540);
	}
}

void res_startup_ask(void) // once menus can show
{
	if (!res_startup_big)
		return;
	if (nm_messagebox(NULL, 2, "Yes", "Default", "Your last game reported a larger than\nmonitor resolution selected.\n\nContinue with %ix%i?",
		SM_W(res_startup_big), SM_H(res_startup_big)) == 0)
	{
		gr_set_mode(res_startup_big);
		Game_screen_mode = res_startup_big;
		set_screen_mode(SCREEN_MENU);
	}
	else // Default (or Esc): stay at 960x540, 16:9
	{
		GameCfg.AspectY = 16;
		GameCfg.AspectX = 9;
	}
	res_startup_big = 0;
}

// "Keep this resolution?" countdown: Revert is highlighted, Keep must be picked on purpose,
// and Esc or running out of time reverts
#define RES_KEEP_SECONDS 15
static fix64 res_keep_start;
static int res_keep_expired;
static char res_keep_text[40];

static int res_keep_handler(newmenu *menu, d_event *event, void *userdata)
{
	userdata = userdata;
	if (event->type == EVENT_IDLE)
	{
		int left = RES_KEEP_SECONDS - (int)((timer_query() - res_keep_start) / F1_0);
		if (left <= 0)
		{
			res_keep_expired = 1;
			window_close(newmenu_get_window(menu));
			return 1;
		}
		snprintf(res_keep_text, sizeof(res_keep_text), "Reverting in %i seconds", left);
	}
	return 0;
}

static int res_keep_countdown(void)
{
	newmenu_item m[3];
	int choice;

	res_keep_start = timer_query();
	res_keep_expired = 0;
	snprintf(res_keep_text, sizeof(res_keep_text), "Reverting in %i seconds", RES_KEEP_SECONDS);
	m[0].type = NM_TYPE_TEXT; m[0].text = res_keep_text;
	m[1].type = NM_TYPE_MENU; m[1].text = "Revert";
	m[2].type = NM_TYPE_MENU; m[2].text = "Keep";
	choice = newmenu_do1(NULL, "Keep this resolution?", 3, m, res_keep_handler, NULL, 1);
	return !res_keep_expired && choice == 2;
}

// Applies a mode. One bigger than the monitor needs "Are you sure?" first (No or Esc cancels), then
// Keep within the countdown, else it switches back. Returns 1 if the new mode is in use.
static int res_apply_checked(u_int32_t new_mode, int fullscr, int borderless, const u_int32_t *modes, int n)
{
	u_int32_t old_mode = Game_screen_mode;
	int maxw, maxh, i;

	if (fullscr && !borderless) // fullscreen exclusive: only the modes the monitor lists
	{
		for (i = 0; i < n && modes[i] != new_mode; i++)
			;
		if (n > 0 && i == n)
		{
			nm_messagebox("Screen Resolution", 1, "OK", "Fullscreen only runs at resolutions\nyour monitor lists.\n\nTick Borderless window, or untick\nFullscreen, for other sizes.");
			return 0;
		}
		res_apply(new_mode, fullscr, borderless);
		return 1;
	}
	if (fullscr) // fullscreen borderless: no bigger than the monitor it runs on
	{
		if (gr_monitor_size(&maxw, &maxh) && (SM_W(new_mode) > maxw || SM_H(new_mode) > maxh))
		{
			nm_messagebox("Screen Resolution", 1, "OK", "Fullscreen Borderless can't be bigger\nthan the monitor it runs on (%ix%i).", maxw, maxh);
			return 0;
		}
		res_apply(new_mode, fullscr, borderless);
		return 1;
	}
	if (!res_too_big(new_mode, modes, n, &maxw, &maxh)) // windowed: anything, far bigger than the monitor asks first
	{
		res_apply(new_mode, fullscr, borderless);
		return 1;
	}
	if (nm_messagebox("Screen Resolution", 2, "Yes", "No", "%ix%i is larger than\nyour monitor (%ix%i).\n\nAre you sure?",
		SM_W(new_mode), SM_H(new_mode), maxw, maxh) != 0)
		return 0;
	res_apply(new_mode, fullscr, borderless);
	if (res_keep_countdown())
		return 1;
	res_apply(old_mode, fullscr, borderless);
	return 0;
}

// Sets the render aspect for a mode: the filter's shape while the box is ticked, else the mode's own
static void res_set_aspect(u_int32_t casp)
{
	if (GameCfg.ResCustom && GameCfg.ResFilterW && res_in_filter(casp)) // a size you typed with your own height keeps its own shape
		casp = SM(GameCfg.ResFilterW, GameCfg.ResFilterH);
	GameCfg.AspectY = SM_W(casp)/gcd(SM_W(casp),SM_H(casp));
	GameCfg.AspectX = SM_H(casp)/gcd(SM_W(casp),SM_H(casp));
}

// Applies Fullscreen / Borderless, moving to a size the new mode allows if needed
static void res_apply_window_mode(int fullscr, int borderless, u_int32_t *modes, int num_presets)
{
	u_int32_t new_mode;
	int i;

	new_mode = Game_screen_mode;
	if (fullscr && !borderless && num_presets > 0) // fullscreen exclusive: a listed mode, else the monitor's largest
	{
		for (i = 0; i < num_presets && modes[i] != new_mode; i++)
			;
		if (i == num_presets)
			for (new_mode = modes[0], i = 1; i < num_presets; i++)
				if (SM_W(modes[i]) * SM_H(modes[i]) > SM_W(new_mode) * SM_H(new_mode))
					new_mode = modes[i];
	}
	else if (fullscr) // fullscreen borderless: no bigger than the monitor
	{
		int monw, monh;
		if (gr_monitor_size(&monw, &monh) && (SM_W(new_mode) > monw || SM_H(new_mode) > monh))
			new_mode = SM(monw, monh);
	}
	if (new_mode != Game_screen_mode)
	{
		GameCfg.AspectY = SM_W(new_mode)/gcd(SM_W(new_mode),SM_H(new_mode));
		GameCfg.AspectX = SM_H(new_mode)/gcd(SM_W(new_mode),SM_H(new_mode));
	}
	res_apply(new_mode, fullscr, borderless);
}

void change_res()
{
	u_int32_t modes[50], list[60], new_mode;
	int i, k, mc, num_presets, nlist, citem, opt_fullscr, opt_borderless, fullscr, borderless, ax, ay;
	newmenu_item m[60+10];
	char restext[60][12], crestext[12], casptext[12];

	num_presets = gr_list_modes( modes );
	res_all_modes = modes;
	res_all_n = num_presets;
	fullscr = gr_check_fullscreen();
	borderless = GameCfg.BorderlessWindow;
	res_focus = 0;
	snprintf(crestext, sizeof(crestext), "%ix%i", SM_W(Game_screen_mode), SM_H(Game_screen_mode));

	// The menu stays open: Enter ticks, rebuilds or applies, and the menu is built again each time.
	// Esc leaves it.
	for (;;)
	{
		// The list: the modes of the filter's shape (or every mode), then the saved custom resolutions
		nlist = 0;
		for (i = 0; i < num_presets; i++)
			if (res_in_filter(modes[i]))
				list[nlist++] = modes[i];
		for (k = 0; k < RES_NUM_SMALL_WIDE; k++) // common sizes the monitor does not list, for windowed play
		{
			for (i = 0; i < nlist && list[i] != res_small_wide[k]; i++)
				;
			if (i == nlist && res_in_filter(res_small_wide[k]))
				list[nlist++] = res_small_wide[k];
		}
		if (GameCfg.ResCustom)
			for (k = 0; k < 2; k++)
			{
				for (i = 0; i < nlist && list[i] != GameCfg.ResExtra[k]; i++)
					;
				if (GameCfg.ResExtra[k] && i == nlist)
					list[nlist++] = GameCfg.ResExtra[k];
			}
		for (i = 1; i < nlist; i++) // in order, biggest first like the monitor's own list: by width, then height
		{
			u_int32_t v = list[i];
			for (k = i; k > 0 && list[k - 1] < v; k--)
				list[k] = list[k - 1];
			list[k] = v;
		}

		mc = 0;
		citem = -1;
		for (i = 0; i < nlist; i++)
		{
			snprintf(restext[mc], sizeof(restext[mc]), "%ix%i", SM_W(list[i]), SM_H(list[i]));
			m[mc].type = NM_TYPE_RADIO;
			m[mc].text = restext[mc];
			m[mc].value = (citem == -1 && Game_screen_mode == list[i]);
			m[mc].group = 0;
			if (m[mc].value)
				citem = mc;
			mc++;
		}
		res_num_list = nlist;
		res_steps = list;
		res_steps_n = nlist;

		m[mc].type = NM_TYPE_TEXT; m[mc].text = ""; mc++; // little space for overview
		// the custom checkbox and its resolution and aspect fields
		res_opt_custom = mc;
		m[mc].type = NM_TYPE_CHECK; m[mc].text = "use custom values"; m[mc].value = GameCfg.ResCustom; mc++;
		m[mc].type = NM_TYPE_TEXT; m[mc].text = "resolution:"; mc++;
		res_opt_res = mc;
		m[mc].type = NM_TYPE_INPUT; m[mc].text = crestext; m[mc].text_len = 11; mc++;
		m[mc].type = NM_TYPE_TEXT; m[mc].text = "aspect:"; mc++;
		res_opt_aspect = mc;
		if (GameCfg.ResCustom && GameCfg.ResFilterW)
			snprintf(casptext, sizeof(casptext), "%ix%i", GameCfg.ResFilterW, GameCfg.ResFilterH); // the live filter
		else if (GameCfg.ResCustom)
			snprintf(casptext, sizeof(casptext), "0x0"); // every shape: the aspect follows the typed resolution
		else
			snprintf(casptext, sizeof(casptext), "%ix%i", GameCfg.AspectY, GameCfg.AspectX); // placeholder: the current aspect
		res_aspect_auto = (GameCfg.ResCustom && !GameCfg.ResFilterW);
		m[mc].type = NM_TYPE_INPUT; m[mc].text = casptext; m[mc].text_len = 11; mc++;
		m[mc].type = NM_TYPE_TEXT; m[mc].text = ""; mc++; // little space for overview
		// fullscreen
		opt_fullscr = res_opt_fullscr = mc;
		m[mc].type = NM_TYPE_CHECK; m[mc].text = "Fullscreen"; m[mc].value = fullscr; mc++;

		opt_borderless = res_opt_borderless = mc;
		m[mc].type = NM_TYPE_CHECK; m[mc].text = "Borderless window"; m[mc].value = borderless; mc++;

		Assert(mc <= SDL_arraysize(m));

		// create the menu
		res_rebuild = res_apply_custom = res_apply_window = 0;
		res_enter_item = -1;
		newmenu_do1(NULL, "Screen Resolution", mc, m, change_res_handler, NULL,
			(res_focus >= 1000) ? nlist + res_focus - 1000 : min(res_focus, max(nlist - 1, 0)));

		fullscr = m[opt_fullscr].value;
		borderless = m[opt_borderless].value;

		if (res_rebuild == RES_REBUILD_ASPECT) // Enter on the aspect field ticks the box; the typed shape becomes the filter
		{
			int w, h;
			GameCfg.ResCustom = 1;
			if (res_parse_pair(casptext, &w, &h))
			{
				GameCfg.ResFilterW = w / gcd(w, h);
				GameCfg.ResFilterH = h / gcd(w, h);
			}
			else if (strtol(casptext, NULL, 10) == 0) // 0x0: every shape again
				GameCfg.ResFilterW = GameCfg.ResFilterH = 0;
			continue;
		}
		if (res_rebuild == RES_REBUILD_TOGGLE)
		{
			GameCfg.ResCustom = m[res_opt_custom].value;
			if (!GameCfg.ResCustom) // unticked: no filter, no custom entries, default menu
			{
				GameCfg.ResFilterW = GameCfg.ResFilterH = 0;
				GameCfg.ResExtra[0] = GameCfg.ResExtra[1] = 0;
			}
			continue;
		}
		if (res_apply_custom) // Enter on a typed resolution with the box ticked
		{
			int w, h;
			if (!res_parse_pair(crestext, &w, &h))
				continue;
			if (w < 320 || h < 200) // oh oh - the resolution is too small. Revert!
			{
				nm_messagebox( TXT_WARNING, 1, "OK", "Entered resolution is too small.\nReverting ..." );
				snprintf(crestext, sizeof(crestext), "%ix%i", SM_W(Game_screen_mode), SM_H(Game_screen_mode));
				continue;
			}
			new_mode = SM(w, h);
			ax = GameCfg.AspectX;
			ay = GameCfg.AspectY;
			if (GameCfg.ResFilterW || !res_parse_pair(casptext, &w, &h)) // the filter's shape, or the mode's own
				res_set_aspect(new_mode);
			else // we even have a custom aspect set up
			{
				GameCfg.AspectY = w / gcd(w, h);
				GameCfg.AspectX = h / gcd(w, h);
			}
			if (!res_apply_checked(new_mode, fullscr, borderless, modes, num_presets))
			{
				GameCfg.AspectX = ax; // cancelled or reverted
				GameCfg.AspectY = ay;
				continue;
			}
			if (new_mode != GameCfg.ResExtra[0]) // keep it in the list, newest first, at most 2
			{
				GameCfg.ResExtra[1] = GameCfg.ResExtra[0];
				GameCfg.ResExtra[0] = new_mode;
			}
			continue;
		}
		if (res_enter_item >= 0) // Enter on the ticked, highlighted resolution
		{
			new_mode = list[res_enter_item];
			ax = GameCfg.AspectX;
			ay = GameCfg.AspectY;
			GameCfg.AspectY = SM_W(new_mode)/gcd(SM_W(new_mode),SM_H(new_mode)); // a listed mode keeps its true shape
			GameCfg.AspectX = SM_H(new_mode)/gcd(SM_W(new_mode),SM_H(new_mode));
			if (!res_apply_checked(new_mode, fullscr, borderless, modes, num_presets))
			{
				GameCfg.AspectX = ax; // cancelled or reverted
				GameCfg.AspectY = ay;
			}
			snprintf(crestext, sizeof(crestext), "%ix%i", SM_W(Game_screen_mode), SM_H(Game_screen_mode));
			continue;
		}
		if (res_apply_window) // Enter on Fullscreen or Borderless: apply it and stay in the menu
		{
			res_apply_window_mode(fullscr, borderless, modes, num_presets);
			fullscr = gr_check_fullscreen();
			borderless = GameCfg.BorderlessWindow;
			snprintf(crestext, sizeof(crestext), "%ix%i", SM_W(Game_screen_mode), SM_H(Game_screen_mode));
			continue;
		}
		break; // Esc: leave, keeping the resolution
	}

	// Leaving: apply Fullscreen / Borderless changes
	res_apply_window_mode(fullscr, borderless, modes, num_presets);
	res_steps = NULL; // the lists live on this function's stack
	res_all_modes = NULL;
	res_all_n = 0;
}

//	Sensitivity & Deadzone menu.
//
//	Built conditionally from the currently enabled control devices, so the
//	player only sees sliders they can actually use. Order, top to bottom, is
//	always: Mouse (if enabled), then Keyboard (always, it is the fallback
//	scheme), then Joystick (if enabled).
void input_config_sensitivity()
{
	newmenu_item m[64];
	int i, nitems = 0;
	int keysens = -1, joysens = -1, joydead = -1, joyunder = -1;
	int mousesens = -1, mouseoverrun = -1, mousefsdead = -1;
	int use_mouse, use_joy, old_school;
	int mouseimpulse = -1; /* Old school mouse */ 
	int mouseturnlimitx = -1, mouseturnlimity = -1, joyturnlimitx = -1, joyturnlimity = -1;

	use_mouse = (PlayerCfg.ControlType & CONTROL_USING_MOUSE) ? 1 : 0;
	use_joy   = (PlayerCfg.ControlType & CONTROL_USING_JOYSTICK) ? 1 : 0;

	// Old school is one of the three radio styles, and the only one that uses a
	// single Base Sensitivity slider instead of the per-axis set.
	old_school = (PlayerCfg.MouseControlStyle == MOUSE_CONTROL_OLDSCHOOL) ? 1 : 0;

	// The Oversteer Buffer is hidden from the UI; everything underneath defaults
	// to zero so it cannot affect the mouse response.
	for (i = 0; i < 6; i++)
		PlayerCfg.MouseOverrun[i] = 0;

	// ---- Mouse block: first, and only when the mouse is enabled ----
	if (use_mouse)
	{
		m[nitems].type = NM_TYPE_TEXT; m[nitems].text = "Mouse Sensitivity:"; nitems++;

		if (old_school)
		{
			mouseimpulse = nitems;
			m[nitems].type = NM_TYPE_SLIDER; m[nitems].text = "Base Sensitivity:"; m[nitems].value = PlayerCfg.MouseImpulse; m[nitems].min_value = 0; m[nitems].max_value = 16; nitems++;
		}
		else
		{
			mousesens = nitems;
			m[nitems].type = NM_TYPE_SLIDER; m[nitems].text = TXT_TURN_LR; m[nitems].value = PlayerCfg.MouseSens[0]; m[nitems].min_value = 0; m[nitems].max_value = 16; nitems++;
			m[nitems].type = NM_TYPE_SLIDER; m[nitems].text = TXT_PITCH_UD; m[nitems].value = PlayerCfg.MouseSens[1]; m[nitems].min_value = 0; m[nitems].max_value = 16; nitems++;
			m[nitems].type = NM_TYPE_SLIDER; m[nitems].text = TXT_SLIDE_LR; m[nitems].value = PlayerCfg.MouseSens[2]; m[nitems].min_value = 0; m[nitems].max_value = 16; nitems++;
			m[nitems].type = NM_TYPE_SLIDER; m[nitems].text = TXT_SLIDE_UD; m[nitems].value = PlayerCfg.MouseSens[3]; m[nitems].min_value = 0; m[nitems].max_value = 16; nitems++;
			m[nitems].type = NM_TYPE_SLIDER; m[nitems].text = TXT_BANK_LR; m[nitems].value = PlayerCfg.MouseSens[4]; m[nitems].min_value = 0; m[nitems].max_value = 16; nitems++;


			if (PlayerCfg.MouseControlStyle == MOUSE_CONTROL_FLIGHT_SIM)
			{
				m[nitems].type = NM_TYPE_TEXT; m[nitems].text = ""; nitems++;
				m[nitems].type = NM_TYPE_TEXT; m[nitems].text = "Mouse FlightSim Deadzone:"; nitems++;
				mousefsdead = nitems;
				m[nitems].type = NM_TYPE_SLIDER; m[nitems].text = "X/Y"; m[nitems].value = PlayerCfg.MouseFSDead; m[nitems].min_value = 0; m[nitems].max_value = 16; nitems++;
			}

			if (PlayerCfg.MouseControlStyle == MOUSE_CONTROL_ENHANCED)
			{
				m[nitems].type = NM_TYPE_TEXT; m[nitems].text = ""; nitems++;
				m[nitems].type = NM_TYPE_TEXT; m[nitems].text = "Enhanced Turn Limits:"; nitems++;
				m[nitems].type = NM_TYPE_TEXT; m[nitems].text = "0=Classic, 15=400%, 16=Uncapped"; nitems++;
				mouseturnlimitx = nitems; m[nitems].type = NM_TYPE_SLIDER; m[nitems].text = "X Axis"; m[nitems].value = PlayerCfg.MouseTurnLimit[0]; m[nitems].min_value = 0; m[nitems].max_value = 16; nitems++;
				mouseturnlimity = nitems; m[nitems].type = NM_TYPE_SLIDER; m[nitems].text = "Y Axis"; m[nitems].value = PlayerCfg.MouseTurnLimit[1]; m[nitems].min_value = 0; m[nitems].max_value = 16; nitems++;
			}
		}

		m[nitems].type = NM_TYPE_TEXT; m[nitems].text = ""; nitems++;
	}

	// ---- Keyboard block: always present ----
	m[nitems].type = NM_TYPE_TEXT; m[nitems].text = "Keyboard Sensitivity:"; nitems++;
	keysens = nitems;
	m[nitems].type = NM_TYPE_SLIDER; m[nitems].text = TXT_TURN_LR; m[nitems].value = PlayerCfg.KeyboardSens[0]; m[nitems].min_value = 0; m[nitems].max_value = 16; nitems++;
	m[nitems].type = NM_TYPE_SLIDER; m[nitems].text = TXT_PITCH_UD; m[nitems].value = PlayerCfg.KeyboardSens[1]; m[nitems].min_value = 0; m[nitems].max_value = 16; nitems++;
	m[nitems].type = NM_TYPE_SLIDER; m[nitems].text = TXT_SLIDE_LR; m[nitems].value = PlayerCfg.KeyboardSens[2]; m[nitems].min_value = 0; m[nitems].max_value = 16; nitems++;
	m[nitems].type = NM_TYPE_SLIDER; m[nitems].text = TXT_SLIDE_UD; m[nitems].value = PlayerCfg.KeyboardSens[3]; m[nitems].min_value = 0; m[nitems].max_value = 16; nitems++;
	m[nitems].type = NM_TYPE_SLIDER; m[nitems].text = TXT_BANK_LR; m[nitems].value = PlayerCfg.KeyboardSens[4]; m[nitems].min_value = 0; m[nitems].max_value = 16; nitems++;
	m[nitems].type = NM_TYPE_TEXT; m[nitems].text = ""; nitems++;
	// ---- Joystick block: last, and only when a joystick is enabled ----
	if (use_joy)
	{
	m[nitems].type = NM_TYPE_TEXT; m[nitems].text = "Joystick Sensitivity:"; nitems++;
	joysens = nitems;
	m[nitems].type = NM_TYPE_SLIDER; m[nitems].text = TXT_TURN_LR; m[nitems].value = PlayerCfg.JoystickSens[0]; m[nitems].min_value = 0; m[nitems].max_value = 16; nitems++;
	m[nitems].type = NM_TYPE_SLIDER; m[nitems].text = TXT_PITCH_UD; m[nitems].value = PlayerCfg.JoystickSens[1]; m[nitems].min_value = 0; m[nitems].max_value = 16; nitems++;
	m[nitems].type = NM_TYPE_SLIDER; m[nitems].text = TXT_SLIDE_LR; m[nitems].value = PlayerCfg.JoystickSens[2]; m[nitems].min_value = 0; m[nitems].max_value = 16; nitems++;
	m[nitems].type = NM_TYPE_SLIDER; m[nitems].text = TXT_SLIDE_UD; m[nitems].value = PlayerCfg.JoystickSens[3]; m[nitems].min_value = 0; m[nitems].max_value = 16; nitems++;
	m[nitems].type = NM_TYPE_SLIDER; m[nitems].text = TXT_BANK_LR; m[nitems].value = PlayerCfg.JoystickSens[4]; m[nitems].min_value = 0; m[nitems].max_value = 16; nitems++;
	m[nitems].type = NM_TYPE_SLIDER; m[nitems].text = TXT_THROTTLE; m[nitems].value = PlayerCfg.JoystickSens[5]; m[nitems].min_value = 0; m[nitems].max_value = 16; nitems++;
	m[nitems].type = NM_TYPE_TEXT; m[nitems].text = ""; nitems++;
	m[nitems].type = NM_TYPE_TEXT; m[nitems].text = "Joystick Turn Cap (0=Classic, 16=Uncapped):"; nitems++;
	joyturnlimitx = nitems; m[nitems].type = NM_TYPE_SLIDER; m[nitems].text = "X Axis"; m[nitems].value = PlayerCfg.JoystickTurnLimit[0]; m[nitems].min_value = 0; m[nitems].max_value = 16; nitems++;
	joyturnlimity = nitems; m[nitems].type = NM_TYPE_SLIDER; m[nitems].text = "Y Axis"; m[nitems].value = PlayerCfg.JoystickTurnLimit[1]; m[nitems].min_value = 0; m[nitems].max_value = 16; nitems++;
	m[nitems].type = NM_TYPE_TEXT; m[nitems].text = ""; nitems++;
	m[nitems].type = NM_TYPE_TEXT; m[nitems].text = "Joystick Deadzone:"; nitems++;
	joydead = nitems;
	m[nitems].type = NM_TYPE_SLIDER; m[nitems].text = TXT_TURN_LR; m[nitems].value = PlayerCfg.JoystickDead[0]; m[nitems].min_value = 0; m[nitems].max_value = 16; nitems++;
	m[nitems].type = NM_TYPE_SLIDER; m[nitems].text = TXT_PITCH_UD; m[nitems].value = PlayerCfg.JoystickDead[1]; m[nitems].min_value = 0; m[nitems].max_value = 16; nitems++;
	m[nitems].type = NM_TYPE_SLIDER; m[nitems].text = TXT_SLIDE_LR; m[nitems].value = PlayerCfg.JoystickDead[2]; m[nitems].min_value = 0; m[nitems].max_value = 16; nitems++;
	m[nitems].type = NM_TYPE_SLIDER; m[nitems].text = TXT_SLIDE_UD; m[nitems].value = PlayerCfg.JoystickDead[3]; m[nitems].min_value = 0; m[nitems].max_value = 16; nitems++;
	m[nitems].type = NM_TYPE_SLIDER; m[nitems].text = TXT_BANK_LR; m[nitems].value = PlayerCfg.JoystickDead[4]; m[nitems].min_value = 0; m[nitems].max_value = 16; nitems++;
	m[nitems].type = NM_TYPE_SLIDER; m[nitems].text = TXT_THROTTLE; m[nitems].value = PlayerCfg.JoystickDead[5]; m[nitems].min_value = 0; m[nitems].max_value = 16; nitems++;
	m[nitems].type = NM_TYPE_TEXT; m[nitems].text = ""; nitems++;
	m[nitems].type = NM_TYPE_TEXT; m[nitems].text = "Joystick Undercalibration:"; nitems++;
	joyunder = nitems;
	m[nitems].type = NM_TYPE_SLIDER; m[nitems].text = TXT_TURN_LR; m[nitems].value = PlayerCfg.JoystickUndercalibrate[0]; m[nitems].min_value = 0; m[nitems].max_value = 16; nitems++;
	m[nitems].type = NM_TYPE_SLIDER; m[nitems].text = TXT_PITCH_UD; m[nitems].value = PlayerCfg.JoystickUndercalibrate[1]; m[nitems].min_value = 0; m[nitems].max_value = 16; nitems++;
	m[nitems].type = NM_TYPE_SLIDER; m[nitems].text = TXT_SLIDE_LR; m[nitems].value = PlayerCfg.JoystickUndercalibrate[2]; m[nitems].min_value = 0; m[nitems].max_value = 16; nitems++;
	m[nitems].type = NM_TYPE_SLIDER; m[nitems].text = TXT_SLIDE_UD; m[nitems].value = PlayerCfg.JoystickUndercalibrate[3]; m[nitems].min_value = 0; m[nitems].max_value = 16; nitems++;
	m[nitems].type = NM_TYPE_SLIDER; m[nitems].text = TXT_BANK_LR; m[nitems].value = PlayerCfg.JoystickUndercalibrate[4]; m[nitems].min_value = 0; m[nitems].max_value = 16; nitems++;
	m[nitems].type = NM_TYPE_SLIDER; m[nitems].text = TXT_THROTTLE; m[nitems].value = PlayerCfg.JoystickUndercalibrate[5]; m[nitems].min_value = 0; m[nitems].max_value = 16; nitems++;
	} // end joystick block

	newmenu_do1(NULL, "SENSITIVITY / ADVANCED TURNING", nitems, m, NULL, NULL, 1);

	// Read back only the groups that were actually built. Every index starts at
	// -1, so an absent group is skipped rather than indexed out of bounds.
	for (i = 0; i < 5; i++)
		PlayerCfg.KeyboardSens[i] = m[keysens + i].value;

	if (mousesens >= 0)
		for (i = 0; i < 5; i++)
			PlayerCfg.MouseSens[i] = m[mousesens + i].value;

	if (mouseoverrun >= 0)
		for (i = 0; i < 6; i++)
			PlayerCfg.MouseOverrun[i] = m[mouseoverrun + i].value;

	if (mouseimpulse >= 0)
		PlayerCfg.MouseImpulse = m[mouseimpulse].value;

	if (mousefsdead >= 0)
		PlayerCfg.MouseFSDead = m[mousefsdead].value;

	if (mouseturnlimitx >= 0)
		PlayerCfg.MouseTurnLimit[0] = m[mouseturnlimitx].value;
	if (mouseturnlimity >= 0)
		PlayerCfg.MouseTurnLimit[1] = m[mouseturnlimity].value;
	if (joyturnlimitx >= 0)
		PlayerCfg.JoystickTurnLimit[0] = m[joyturnlimitx].value;
	if (joyturnlimity >= 0)
		PlayerCfg.JoystickTurnLimit[1] = m[joyturnlimity].value;

	if (joysens >= 0)
		for (i = 0; i < 6; i++)
		{
			PlayerCfg.JoystickSens[i] = m[joysens + i].value;
			PlayerCfg.JoystickDead[i] = m[joydead + i].value;
			PlayerCfg.JoystickUndercalibrate[i] = m[joyunder + i].value;
		}
}

static int opt_ic_usejoy = 0, opt_ic_usemouse = 0, opt_ic_confkey = 0, opt_ic_confjoy = 0, opt_ic_confmouse = 0, opt_ic_confweap = 0, opt_ic_confobs = 0, opt_ic_mouseflightsim = 0, opt_ic_joymousesens = 0, opt_ic_grabinput = 0, opt_ic_mousefsgauge = 0, opt_ic_stickyrear = 0, opt_ic_lastinput = 0, opt_ic_help0 = 0, opt_ic_help1 = 0, opt_ic_help2 = 0;
int input_config_menuset(newmenu *menu, d_event *event, void *userdata)
{
	newmenu_item *items = newmenu_get_items(menu);
	int citem = newmenu_get_citem(menu);

	userdata = userdata;

	switch (event->type)
	{
		case EVENT_NEWMENU_CHANGED:
			if (citem == opt_ic_usejoy)
				(items[citem].value)?(PlayerCfg.ControlType|=CONTROL_USING_JOYSTICK):(PlayerCfg.ControlType&=~CONTROL_USING_JOYSTICK);
			if (citem == opt_ic_usemouse)
				(items[citem].value)?(PlayerCfg.ControlType|=CONTROL_USING_MOUSE):(PlayerCfg.ControlType&=~CONTROL_USING_MOUSE);
			/* Old School Mouse */
			if (citem == opt_ic_mouseflightsim)
				PlayerCfg.MouseControlStyle = MOUSE_CONTROL_REBIRTH;
			if (citem == opt_ic_mouseflightsim+1)
				PlayerCfg.MouseControlStyle = MOUSE_CONTROL_FLIGHT_SIM;
			if (citem == opt_ic_mouseflightsim+2)
				PlayerCfg.MouseControlStyle = MOUSE_CONTROL_OLDSCHOOL;
			if (citem == opt_ic_mouseflightsim+3)
				PlayerCfg.MouseControlStyle = MOUSE_CONTROL_ENHANCED;
			if (citem == opt_ic_grabinput)
				GameCfg.Grabinput = items[citem].value;
			if (citem == opt_ic_mousefsgauge)
				PlayerCfg.MouseFSIndicator = items[citem].value;
			if (citem == opt_ic_stickyrear)			
				PlayerCfg.StickyRearview = items[citem].value;
			if (citem == opt_ic_lastinput)
				PlayerCfg.LastInputWins = items[citem].value;
			break;

		case EVENT_NEWMENU_SELECTED:
			if (citem == opt_ic_confkey)
				kconfig(0, "KEYBOARD");
			if (citem == opt_ic_confjoy)
				kconfig(1, "JOYSTICK");
			if (citem == opt_ic_confmouse)
				kconfig(2, "MOUSE");
			if (citem == opt_ic_confweap)
				kconfig(3, "WEAPON KEYS");
			if (citem == opt_ic_confobs)
				kconfig(4, "OBSERVER CONTROLS");
			if (citem == opt_ic_joymousesens)
				input_config_sensitivity();
			if (citem == opt_ic_help0)
				show_help();
			if (citem == opt_ic_help1)
				show_netgame_help();
			if (citem == opt_ic_help2)
				show_newdemo_help();
			return 1;		// stay in menu
			break;

		default:
			break;
	}

	return 0;
}

void input_config()
{
	newmenu_item m[28];
	int nitems = 0;

	opt_ic_usejoy = nitems;
	m[nitems].type = NM_TYPE_CHECK; m[nitems].text = "USE JOYSTICK"; m[nitems].value = (PlayerCfg.ControlType&CONTROL_USING_JOYSTICK); nitems++;
	opt_ic_usemouse = nitems;
	m[nitems].type = NM_TYPE_CHECK; m[nitems].text = "USE MOUSE"; m[nitems].value = (PlayerCfg.ControlType&CONTROL_USING_MOUSE); nitems++;
	m[nitems].type = NM_TYPE_TEXT; m[nitems].text = ""; nitems++;
	opt_ic_confkey = nitems;
	m[nitems].type = NM_TYPE_MENU; m[nitems].text = "CUSTOMIZE KEYBOARD"; nitems++;
	opt_ic_confjoy = nitems;
	m[nitems].type = NM_TYPE_MENU; m[nitems].text = "CUSTOMIZE JOYSTICK"; nitems++;
	opt_ic_confmouse = nitems;
	m[nitems].type = NM_TYPE_MENU; m[nitems].text = "CUSTOMIZE MOUSE"; nitems++;
	opt_ic_confweap = nitems;
	m[nitems].type = NM_TYPE_MENU; m[nitems].text = "CUSTOMIZE WEAPON KEYS"; nitems++;
	opt_ic_confobs = nitems;
	m[nitems].type = NM_TYPE_MENU; m[nitems].text = "CUSTOMIZE OBSERVER CONTROLS"; nitems++;
	m[nitems].type = NM_TYPE_TEXT; m[nitems].text = ""; nitems++;
	m[nitems].type = NM_TYPE_TEXT; m[nitems].text = "MOUSE CONTROL TYPE:"; nitems++;
	opt_ic_mouseflightsim = nitems;
	/* Old School Mouse */
	m[nitems].type = NM_TYPE_RADIO; m[nitems].text = "Rebirth"; m[nitems].value = PlayerCfg.MouseControlStyle == MOUSE_CONTROL_REBIRTH; m[nitems].group = 0; nitems++;
	m[nitems].type = NM_TYPE_RADIO; m[nitems].text = "FlightSim"; m[nitems].value = PlayerCfg.MouseControlStyle == MOUSE_CONTROL_FLIGHT_SIM; m[nitems].group = 0; nitems++;
	m[nitems].type = NM_TYPE_RADIO; m[nitems].text = "Old school"; m[nitems].value = PlayerCfg.MouseControlStyle == MOUSE_CONTROL_OLDSCHOOL; m[nitems].group = 0; nitems++;
	m[nitems].type = NM_TYPE_RADIO; m[nitems].text = "Enhanced Turning"; m[nitems].value = PlayerCfg.MouseControlStyle == MOUSE_CONTROL_ENHANCED; m[nitems].group = 0; nitems++;
	m[nitems].type = NM_TYPE_TEXT; m[nitems].text = ""; nitems++;opt_ic_joymousesens = nitems;
	m[nitems].type = NM_TYPE_MENU; m[nitems].text = "SENSITIVITY / ADVANCED TURNING"; nitems++;
	m[nitems].type = NM_TYPE_TEXT; m[nitems].text = ""; nitems++;
	opt_ic_grabinput = nitems;
	m[nitems].type = NM_TYPE_CHECK; m[nitems].text= "Keep Keyboard/Mouse focus"; m[nitems].value = GameCfg.Grabinput; nitems++;
	opt_ic_mousefsgauge = nitems;
	m[nitems].type = NM_TYPE_CHECK; m[nitems].text= "Mouse FlightSim Indicator"; m[nitems].value = PlayerCfg.MouseFSIndicator; nitems++;
	opt_ic_stickyrear = nitems;
	m[nitems].type = NM_TYPE_CHECK; m[nitems].text= "Sticky Rearview"; m[nitems].value = PlayerCfg.StickyRearview; nitems++;
	opt_ic_lastinput = nitems;
	m[nitems].type = NM_TYPE_CHECK; m[nitems].text= "Last Input Wins (Slide/Bank)"; m[nitems].value = PlayerCfg.LastInputWins; nitems++;
	m[nitems].type = NM_TYPE_TEXT; m[nitems].text = ""; nitems++;
	opt_ic_help0 = nitems;
	m[nitems].type = NM_TYPE_MENU; m[nitems].text = "GAME SYSTEM KEYS"; nitems++;
	opt_ic_help1 = nitems;
	m[nitems].type = NM_TYPE_MENU; m[nitems].text = "NETGAME SYSTEM KEYS"; nitems++;
	opt_ic_help2 = nitems;
	m[nitems].type = NM_TYPE_MENU; m[nitems].text = "DEMO SYSTEM KEYS"; nitems++;

	newmenu_do1(NULL, TXT_CONTROLS, nitems, m, input_config_menuset, NULL, 3);
}

void reticle_config()
{
#ifdef OGL
	newmenu_item m[18];
#else
	newmenu_item m[17];
#endif
	int nitems = 0, i, opt_ret_type, opt_ret_rgba, opt_ret_size;
	
	m[nitems].type = NM_TYPE_TEXT; m[nitems].text = "Reticle Type:"; nitems++;
	opt_ret_type = nitems;
	m[nitems].type = NM_TYPE_RADIO; m[nitems].text = "Classic"; m[nitems].value = 0; m[nitems].group = 0; nitems++;
#ifdef OGL
	m[nitems].type = NM_TYPE_RADIO; m[nitems].text = "Classic Reboot"; m[nitems].value = 0; m[nitems].group = 0; nitems++;
#endif
	m[nitems].type = NM_TYPE_RADIO; m[nitems].text = "None"; m[nitems].value = 0; m[nitems].group = 0; nitems++;
	m[nitems].type = NM_TYPE_RADIO; m[nitems].text = "X"; m[nitems].value = 0; m[nitems].group = 0; nitems++;
	m[nitems].type = NM_TYPE_RADIO; m[nitems].text = "Dot"; m[nitems].value = 0; m[nitems].group = 0; nitems++;
	m[nitems].type = NM_TYPE_RADIO; m[nitems].text = "Circle"; m[nitems].value = 0; m[nitems].group = 0; nitems++;
	m[nitems].type = NM_TYPE_RADIO; m[nitems].text = "Cross V1"; m[nitems].value = 0; m[nitems].group = 0; nitems++;
	m[nitems].type = NM_TYPE_RADIO; m[nitems].text = "Cross V2"; m[nitems].value = 0; m[nitems].group = 0; nitems++;
	m[nitems].type = NM_TYPE_RADIO; m[nitems].text = "Angle"; m[nitems].value = 0; m[nitems].group = 0; nitems++;
	m[nitems].type = NM_TYPE_TEXT; m[nitems].text = ""; nitems++;
	m[nitems].type = NM_TYPE_TEXT; m[nitems].text = "Reticle Color:"; nitems++;
	opt_ret_rgba = nitems;
	m[nitems].type = NM_TYPE_SLIDER; m[nitems].text = "Red"; m[nitems].value = (PlayerCfg.ReticleRGBA[0]/2); m[nitems].min_value = 0; m[nitems].max_value = 16; nitems++;
	m[nitems].type = NM_TYPE_SLIDER; m[nitems].text = "Green"; m[nitems].value = (PlayerCfg.ReticleRGBA[1]/2); m[nitems].min_value = 0; m[nitems].max_value = 16; nitems++;
	m[nitems].type = NM_TYPE_SLIDER; m[nitems].text = "Blue"; m[nitems].value = (PlayerCfg.ReticleRGBA[2]/2); m[nitems].min_value = 0; m[nitems].max_value = 16; nitems++;
	m[nitems].type = NM_TYPE_SLIDER; m[nitems].text = "Alpha"; m[nitems].value = (PlayerCfg.ReticleRGBA[3]/2); m[nitems].min_value = 0; m[nitems].max_value = 16; nitems++;
	m[nitems].type = NM_TYPE_TEXT; m[nitems].text = ""; nitems++;
	opt_ret_size = nitems;
	m[nitems].type = NM_TYPE_SLIDER; m[nitems].text = "Reticle Size:"; m[nitems].value = PlayerCfg.ReticleSize; m[nitems].min_value = 0; m[nitems].max_value = 4; nitems++;

	i = PlayerCfg.ReticleType;
#ifndef OGL
	if (i > 1) i--;
#endif
	m[opt_ret_type+i].value=1;

	newmenu_do1( NULL, "Reticle Options", nitems, m, NULL, NULL, 1 );

#ifdef OGL
	for (i = 0; i < 9; i++)
		if (m[i+opt_ret_type].value)
			PlayerCfg.ReticleType = i;
#else
	for (i = 0; i < 8; i++)
		if (m[i+opt_ret_type].value)
			PlayerCfg.ReticleType = i;
	if (PlayerCfg.ReticleType > 1) PlayerCfg.ReticleType++;
#endif
	for (i = 0; i < 4; i++)
		PlayerCfg.ReticleRGBA[i] = (m[i+opt_ret_rgba].value*2);
	PlayerCfg.ReticleSize = m[opt_ret_size].value;
}

int opt_gr_briefingrobots3d;
int opt_gr_texfilt, opt_gr_movietexfilt, opt_gr_brightness, opt_gr_reticlemenu, opt_gr_alphafx, opt_gr_dynlightcolor, opt_gr_vsync, opt_gr_multisample, opt_gr_fpsindi, opt_gr_disablecockpit;
int opt_gr_classicdepth;
int opt_gr_custommodels, opt_gr_customtextures, opt_gr_reducedflash;
#ifdef RT_DX12

int RT_GetIntFromConfig(RT_Config* config, RT_String key) 
{
	int temp = 0;
	ALWAYS(RT_ConfigReadInt(config, key, &temp));
	return temp;
}

float RT_GetFloatFromConfig(RT_Config* config, RT_String key)
{
	float temp = 0.0;
	ALWAYS(RT_ConfigReadFloat(config, key, &temp));
	return temp;
}

RT_Vec3 RT_GetVec3FromConfig(RT_Config* config, RT_String key)
{
	RT_Vec3 temp = RT_Vec3FromScalar(0.f);
	ALWAYS(RT_ConfigReadVec3(config, key, &temp));
	return temp;
}

// ------------------------------------------------------------------
// Render profiles: the .vars files in assets/render_presets, shared by every pilot and
// every texture pack. The active one is GameCfg.RenderProfile (descent.cfg), and each
// pack remembers the profile last used with it in render_profiles.cfg ("pack=profile",
// "Original" when no pack is on), so any profile can go with any pack.

#define GR_MAX_RENDER_PROFILES 24
#define RENDER_PROFILE_MAP_FILE "render_profiles.cfg"
#define GR_NAME_MAX 64

static int gr_compare_names(const void *a, const void *b)
{
	return d_stricmp((const char *)a, (const char *)b);
}

// Profile name as shown in menus: the file name without ".vars".
static void gr_profile_label(char *out, size_t out_size, const char *file)
{
	size_t len;
	snprintf(out, out_size, "%s", file);
	len = strlen(out);
	if (len > 5 && !d_stricmp(out + len - 5, ".vars"))
		out[len - 5] = 0;
}

static int gr_list_render_profiles(char names[][GR_NAME_MAX], int max)
{
	char **rc = PHYSFS_enumerateFiles("assets/render_presets/");
	char **it;
	int n = 0;
	for (it = rc; *it; it++)
	{
		size_t len = strlen(*it);
		if (len > 5 && len < GR_NAME_MAX && !d_stricmp(*it + len - 5, ".vars") && n < max)
			snprintf(names[n++], GR_NAME_MAX, "%s", *it);
	}
	PHYSFS_freeList(rc);
	qsort(names, n, GR_NAME_MAX, gr_compare_names);
	return n;
}

static const char *gr_texture_pack_key(void)
{
	return (GameCfg.UseCustomTextures && GameCfg.TexturePack[0]) ? GameCfg.TexturePack : "Original";
}

// Applies a profile like Raytracing Options > Change preset does, and saves it as the active one.
// Light levels (Raytracing Options: explosion, weapon, headlight, game lights) ride along in the
// render settings as game_* keys, so a profile or a pilot carries them. RT_StartImGuiFrame keeps
// the keys current every frame.
void RT_ApplyGameLightKeys(RT_Config *config)
{
	float v;
	if (RT_ConfigReadFloat(config, RT_StringLiteral("game_explosion_brightness"), &v)) g_rt_dynamic_light_info.explosionBrightMod = v;
	if (RT_ConfigReadFloat(config, RT_StringLiteral("game_weapon_brightness"), &v))    g_rt_dynamic_light_info.weaponBrightMod = v;
	if (RT_ConfigReadFloat(config, RT_StringLiteral("game_headlight_brightness"), &v)) g_headlights.brightness = v;
	if (RT_ConfigReadFloat(config, RT_StringLiteral("game_light_brightness"), &v))
	{
		g_light_multiplier_default = g_light_multiplier = v;
		g_pending_light_update = true;
	}
}

void RT_StoreGameLightKeys(RT_Config *config)
{
	RT_ConfigWriteFloat(config, RT_StringLiteral("game_explosion_brightness"), g_rt_dynamic_light_info.explosionBrightMod);
	RT_ConfigWriteFloat(config, RT_StringLiteral("game_weapon_brightness"), g_rt_dynamic_light_info.weaponBrightMod);
	RT_ConfigWriteFloat(config, RT_StringLiteral("game_headlight_brightness"), g_headlights.brightness);
	RT_ConfigWriteFloat(config, RT_StringLiteral("game_light_brightness"), g_light_multiplier_default);
}

static void gr_apply_render_profile(const char *name)
{
	char path[128];
	RT_Config *config = RT_GetRendererIO()->config;
	int smooth = 1;
	RT_ConfigReadInt(config, RT_StringLiteral("smooth_transparency"), &smooth);
	snprintf(path, sizeof(path), "assets/render_presets/%s", name);
	if (!RT_DeserializeConfigFromFile(config, path))
		return;
	RT_ConfigWriteInt(config, RT_StringLiteral("smooth_transparency"), smooth); // pilot setting, not the profile's
	config->last_modified_time = RT_GetHighResTime().value;
	RT_SerializeConfigToFile(config, RT_RENDER_SETTINGS_CONFIG_FILE);
	snprintf(GameCfg.RenderProfile, sizeof(GameCfg.RenderProfile), "%s", name);
	RT_SetLightsProfile(name); // that profile's lights (lights/<profile>/, else shared)
	RT_ApplyGameLightKeys(config); // light levels saved in the profile itself win
	snprintf(RT_GetRendererIO()->active_profile, sizeof(RT_GetRendererIO()->active_profile), "%s", name);
	con_printf(CON_NORMAL, "Render profile: %s\n", name);
}

// render_profiles.cfg: one "pack=profile" line per pack.
static int gr_read_profile_map(char packs[][GR_NAME_MAX], char profiles[][GR_NAME_MAX], int max)
{
	char line[2 * GR_NAME_MAX + 4];
	int n = 0;
	FILE *f = fopen(RENDER_PROFILE_MAP_FILE, "r");
	if (!f)
		return 0;
	while (n < max && fgets(line, sizeof(line), f))
	{
		char *eq = strchr(line, '=');
		size_t len;
		if (!eq)
			continue;
		*eq = 0;
		len = strlen(eq + 1);
		while (len && isspace((unsigned char)eq[len]))
			eq[len--] = 0;
		snprintf(packs[n], GR_NAME_MAX, "%s", line);
		snprintf(profiles[n], GR_NAME_MAX, "%s", eq + 1);
		n++;
	}
	fclose(f);
	return n;
}

// The profile each shipped pack starts with until you pick another one for it.
static const char *const gr_default_pack_profiles[][2] =
{
	{ "Rethink", "RTXRETHINK.vars" },
	{ "DMAX",    "RTXDMAX.vars" },
};

static int gr_profile_exists(const char *profile)
{
	char path[128];
	snprintf(path, sizeof(path), "assets/render_presets/%s", profile);
	return PHYSFSX_exists(path, 0);
}

static int gr_profile_for_pack(const char *pack, char *out, size_t out_size)
{
	char packs[GR_MAX_TEXTURE_PACKS + 1][GR_NAME_MAX], profiles[GR_MAX_TEXTURE_PACKS + 1][GR_NAME_MAX];
	int i, n = gr_read_profile_map(packs, profiles, GR_MAX_TEXTURE_PACKS + 1);
	for (i = 0; i < n; i++)
		if (!d_stricmp(packs[i], pack) && gr_profile_exists(profiles[i]))
		{
			snprintf(out, out_size, "%s", profiles[i]);
			return 1;
		}
	for (i = 0; i < (int)(sizeof(gr_default_pack_profiles) / sizeof(gr_default_pack_profiles[0])); i++)
		if (!d_stricmp(gr_default_pack_profiles[i][0], pack) && gr_profile_exists(gr_default_pack_profiles[i][1]))
		{
			snprintf(out, out_size, "%s", gr_default_pack_profiles[i][1]);
			return 1;
		}
	return 0;
}

// One frame with "Applying <pack> Textures and Shaders..." in a menu box, shown before the
// (slow) pack/profile switch so the game doesn't look frozen.
static void gr_show_applying(const char *pack)
{
	char msg[128];
	snprintf(msg, sizeof(msg), "Applying %s Textures and Shaders...", pack);
	RT_BeginFrame();
	RT_StartImGuiFrame();
	show_boxed_message(msg, 1);
	RT_EndImguiFrame();
	RT_EndFrame();
	gr_flip();
}

static void gr_remember_profile_for_pack(const char *pack, const char *profile);

// Graphics menu pack/profile items (built in graphics_config, used by its callback too).
static char gr_texture_packs[GR_MAX_TEXTURE_PACKS][RT_TEXTURE_PACK_NAME_MAX]; // the menu keeps pointers to these
static char gr_render_profiles[GR_MAX_RENDER_PROFILES][GR_NAME_MAX];
static char gr_render_profile_labels[GR_MAX_RENDER_PROFILES][GR_NAME_MAX]; // shown without ".vars"
static int  gr_texpack_count, gr_opt_texpack = -1, gr_opt_profilemenu = -1;
static char gr_pending_profile[GR_NAME_MAX];        // profile picked in the menu, applied by gr_menu_apply
static char gr_profile_item_text[GR_NAME_MAX + 32]; // "Render Profile: X" line in the Graphics menu

static void gr_set_pending_profile(const char *file)
{
	char label[GR_NAME_MAX];
	snprintf(gr_pending_profile, sizeof(gr_pending_profile), "%s", file);
	gr_profile_label(label, sizeof(label), file[0] ? file : "(none)");
	snprintf(gr_profile_item_text, sizeof(gr_profile_item_text), "Render Profile: %s", label);
}

// Graphics > Render Profile: the profiles open in their own window, so the Graphics menu stays short.
// Enter picks the highlighted profile, Esc keeps the ticked one (Space ticks).
static void gr_profile_menu(void)
{
	newmenu_item m[GR_MAX_RENDER_PROFILES];
	int n = gr_list_render_profiles(gr_render_profiles, GR_MAX_RENDER_PROFILES), k, cur = 0, r;
	if (!n)
	{
		nm_messagebox(NULL, 1, TXT_OK, "No profiles in assets\\render_presets");
		return;
	}
	for (k = 0; k < n; k++)
	{
		gr_profile_label(gr_render_profile_labels[k], GR_NAME_MAX, gr_render_profiles[k]);
		m[k].type = NM_TYPE_RADIO; m[k].text = gr_render_profile_labels[k];
		m[k].value = !d_stricmp(gr_render_profiles[k], gr_pending_profile); m[k].group = 0;
		if (m[k].value)
			cur = k;
	}
	r = newmenu_do1(NULL, "Render Profile", n, m, NULL, NULL, cur);
	if (r >= 0 && r < n)
	{
		gr_set_pending_profile(gr_render_profiles[r]);
		return;
	}
	for (k = 0; k < n; k++)
		if (m[k].value)
			gr_set_pending_profile(gr_render_profiles[k]);
}
static char gr_applied_pack_key[GR_NAME_MAX]; // pack in effect, to tell whether a switch is needed
extern int opt_gr_customtextures;

// The pack the menu items currently point at ("Original" when Use Custom Textures is off).
static const char *gr_menu_pack_key(newmenu_item *m)
{
	int k;
	if (!m[opt_gr_customtextures].value)
		return "Original";
	for (k = 0; k < gr_texpack_count; k++)
		if (m[gr_opt_texpack + k].value)
			return gr_texture_packs[k];
	return "Original";
}

// Picking a pack (Space) moves the Render Profile line to that pack's profile, so you see it.
static void gr_menu_sync_profile(newmenu_item *m)
{
	char profile[GR_NAME_MAX];
	if (gr_profile_for_pack(gr_menu_pack_key(m), profile, sizeof(profile)))
		gr_set_pending_profile(profile);
}

// Applies the ticked pack and profile (Enter keeps the menu open, Esc applies on the way out).
static void gr_menu_apply(newmenu_item *m)
{
	const char *chosen = NULL;
	int k, pack_changed, profile_changed;

	GameCfg.UseCustomTextures = m[opt_gr_customtextures].value;
	for (k = 0; k < gr_texpack_count; k++)
		if (m[gr_opt_texpack + k].value)
			snprintf(GameCfg.TexturePack, sizeof(GameCfg.TexturePack), "%s", gr_texture_packs[k]);
	if (gr_pending_profile[0])
		chosen = gr_pending_profile;

	pack_changed = d_stricmp(gr_applied_pack_key, gr_texture_pack_key()) != 0;
	profile_changed = chosen && d_stricmp(chosen, GameCfg.RenderProfile);
	if (pack_changed || profile_changed)
		gr_show_applying(gr_texture_pack_key());

	// Switches right away (reloads what is on screen); Original only when unchecked.
	RT_SetTexturePack(GameCfg.UseCustomTextures ? GameCfg.TexturePack : "");
	if (profile_changed)
		gr_apply_render_profile(chosen);
	if (GameCfg.RenderProfile[0])
		gr_remember_profile_for_pack(gr_texture_pack_key(), GameCfg.RenderProfile);
	snprintf(gr_applied_pack_key, sizeof(gr_applied_pack_key), "%s", gr_texture_pack_key());
}

// Pilot memory: each pilot keeps the texture pack, render profile and Smooth Transparency it last
// used. Synced into PlayerCfg whenever the pilot file is written, applied when a pilot is selected.
// Players/<pilot>.rtvars: the full render settings this pilot last played with (its profile's
// values plus whatever was changed in game). The profile file itself only changes from the renderer.
static void gr_pilot_settings_path(char *out, size_t out_size)
{
	snprintf(out, out_size, GameArg.SysUsePlayersDir ? "Players/%.8s.rtvars" : "%.8s.rtvars", Players[Player_num].callsign);
}

void RT_SyncPilotRenderSettings(void)
{
	RT_RendererIO *io = RT_GetRendererIO();
	int smooth;
	char path[64];
	PlayerCfg.RTCustomTextures = GameCfg.UseCustomTextures ? 1 : 0;
	snprintf(PlayerCfg.RTTexturePack, sizeof(PlayerCfg.RTTexturePack), "%s", GameCfg.TexturePack);
	snprintf(PlayerCfg.RTRenderProfile, sizeof(PlayerCfg.RTRenderProfile), "%s", GameCfg.RenderProfile);
	if (io && io->config && RT_ConfigReadInt(io->config, RT_StringLiteral("smooth_transparency"), &smooth))
		PlayerCfg.SmoothTransparency = smooth ? 1 : 0;
	if (io && io->config && Players[Player_num].callsign[0])
	{
		RT_StoreGameLightKeys(io->config); // light levels ride along in the pilot's settings file
		gr_pilot_settings_path(path, sizeof(path));
		RT_SerializeConfigToFile(io->config, path);
	}
}

void RT_ApplyPilotRenderSettings(void)
{
	RT_RendererIO *io = RT_GetRendererIO();
	char old_pack[GR_NAME_MAX];
	int pack_changed, profile_changed;

	snprintf(old_pack, sizeof(old_pack), "%s", gr_texture_pack_key());
	if (PlayerCfg.RTCustomTextures != 255)
	{
		GameCfg.UseCustomTextures = PlayerCfg.RTCustomTextures;
		if (PlayerCfg.RTTexturePack[0])
			snprintf(GameCfg.TexturePack, sizeof(GameCfg.TexturePack), "%s", PlayerCfg.RTTexturePack);
	}
	pack_changed = d_stricmp(old_pack, gr_texture_pack_key()) != 0;
	profile_changed = PlayerCfg.RTRenderProfile[0] && gr_profile_exists(PlayerCfg.RTRenderProfile) &&
		d_stricmp(PlayerCfg.RTRenderProfile, GameCfg.RenderProfile);

	if (!io || !io->config) // renderer not up yet: startup reads GameCfg itself
	{
		if (profile_changed)
			snprintf(GameCfg.RenderProfile, sizeof(GameCfg.RenderProfile), "%s", PlayerCfg.RTRenderProfile);
		return;
	}
	if (pack_changed || profile_changed)
		gr_show_applying(gr_texture_pack_key());
	if (pack_changed)
		RT_SetTexturePack(GameCfg.UseCustomTextures ? GameCfg.TexturePack : ""); // no-op before bitmaps load
	if (profile_changed)
	{
		gr_apply_render_profile(PlayerCfg.RTRenderProfile);
	}
	{
		// Then the settings this pilot last played with, on top of the profile.
		char path[64];
		gr_pilot_settings_path(path, sizeof(path));
		if (PHYSFSX_exists(path, 0) && RT_DeserializeConfigFromFile(io->config, path))
			RT_ApplyGameLightKeys(io->config);
	}
	RT_ConfigWriteInt(io->config, RT_StringLiteral("smooth_transparency"), PlayerCfg.SmoothTransparency);
	io->config->last_modified_time = RT_GetHighResTime().value;
	RT_SerializeConfigToFile(io->config, RT_RENDER_SETTINGS_CONFIG_FILE);
	WriteConfigFile();
}

static void gr_remember_profile_for_pack(const char *pack, const char *profile)
{
	char packs[GR_MAX_TEXTURE_PACKS + 1][GR_NAME_MAX], profiles[GR_MAX_TEXTURE_PACKS + 1][GR_NAME_MAX];
	int i, n = gr_read_profile_map(packs, profiles, GR_MAX_TEXTURE_PACKS + 1);
	FILE *f;
	for (i = 0; i < n; i++)
		if (!d_stricmp(packs[i], pack))
			break;
	if (i == n)
	{
		if (n == GR_MAX_TEXTURE_PACKS + 1)
			return;
		snprintf(packs[n++], GR_NAME_MAX, "%s", pack);
	}
	snprintf(profiles[i], GR_NAME_MAX, "%s", profile);
	f = fopen(RENDER_PROFILE_MAP_FILE, "w");
	if (!f)
		return;
	for (i = 0; i < n; i++)
		fprintf(f, "%s=%s\n", packs[i], profiles[i]);
	fclose(f);
}

// ------------------------------------------------------------------
// Raytracing Options. The sliders are the renderer's own values (same numbers as the renderer
// window, full range), applied live so the game behind the menu shows each change. Changes belong
// to the pilot: saved with the pilot (Players/<pilot>.rtvars); a profile only changes from the
// renderer window.

static const char *const rt_fsr_mode_names[6] = { "No Upscaling", "High Quality", "Quality", "Balanced", "Performance", "Ultra Performance" };
static const int rt_fsr_mode_values[6] = { 0, 5, 1, 2, 3, 4 }; // amd_fsr2_mode for each name above

#define RT_SLIDER_STEPS 10 // the menu draws one character per notch: keep them small

// Where a slider's value lives.
enum { RT_VAL_CONFIG, RT_VAL_EXPLOSION, RT_VAL_WEAPON, RT_VAL_HEADLIGHT, RT_VAL_LEVEL_LIGHTS };

typedef struct RT_MenuSlider
{
	const char *name;
	int         where;     // RT_VAL_*
	const char *key;       // renderer setting (RT_VAL_CONFIG)
	float       lo, hi;    // the renderer window's range
	float       curve;     // 1 = even steps, >1 = finer steps near the bottom
	const char *fmt;
	int         steps;     // notches (0 = RT_SLIDER_STEPS)
	float       logmin;    // unused (kept for the table below)
	const float *table;    // exact notch values (when set): the slider steps through these
	int         table_n;
	int         inverted;  // 1: slider right = lower value (gamma: right = brighter)
	int         opt;       // menu item index
	int         start;     // notch when the menu opened
	char        label[48];
} RT_MenuSlider;

// Exact light notches: fine at the bottom, landing on 1, 5, 25, 50 ... up to the game's max.
static const float rt_light_notches[] = {
	0.0f, 0.05f, 0.5f, 1.0f, 3.0f, 5.0f, 10.0f, 25.0f, 50.0f, 100.0f, 160.0f };
#define RT_LIGHT_NOTCHES_160 RT_ARRAY_COUNT(rt_light_notches)
#define RT_LIGHT_NOTCHES_100 10 // up to 100.0

static RT_MenuSlider rt_sliders[] = {
	{ "Bloom",             RT_VAL_CONFIG,       "bloom_amount",      0.0f,   0.16f, 1.0f, "%.2f", 16 }, // 0.01 per notch
	{ "Explosion Lights",  RT_VAL_EXPLOSION,    NULL,                0.0f,  160.0f, 1.0f, "%.2f", 0, 0, rt_light_notches, RT_LIGHT_NOTCHES_160 }, // game max 160 (renderer goes to 1000)
	{ "Weapon Lights",     RT_VAL_WEAPON,       NULL,                0.0f,  160.0f, 1.0f, "%.2f", 0, 0, rt_light_notches, RT_LIGHT_NOTCHES_160 },
	{ "Exposure",          RT_VAL_CONFIG,       "exposure",         -2.0f,    2.0f, 1.0f, "%.2f" },
	{ "Specular",          RT_VAL_CONFIG,       "specular_scale",    0.0f,    1.0f, 1.0f, "%.2f" },
	{ "Roughness",         RT_VAL_CONFIG,       "roughness_scale",   0.0f,    1.0f, 1.0f, "%.2f" },
	{ "Gamma",             RT_VAL_CONFIG,       "gamma",            -1.0f,    1.0f, 1.0f, "%.2f", 0, 0, NULL, 0, 1 }, // right = brighter
	{ "Sharpen",           RT_VAL_CONFIG,       "sharpen_amount",    0.0f,    8.0f, 1.0f, "%.2f" },
	{ "Vignette",          RT_VAL_CONFIG,       "vignette_strength", 0.0f,    1.0f, 1.0f, "%.2f" },
	{ "FOV",               RT_VAL_CONFIG,       "fov",              90.0f,  120.0f, 1.0f, "%.0f", 15 }, // full renderer range, 2 degrees per notch
	{ "Headlight",         RT_VAL_HEADLIGHT,    NULL,                0.0f,  100.0f, 1.0f, "%.2f", 0, 0, rt_light_notches, RT_LIGHT_NOTCHES_100 },
	{ "Global Game Lights", RT_VAL_LEVEL_LIGHTS, NULL,                1.0f,   15.0f, 1.0f, "%.2f", 14 }, // 1 per notch
};
enum { RT_S_BLOOM, RT_S_EXPLOSION, RT_S_WEAPON, RT_S_EXPOSURE, RT_S_SPECULAR, RT_S_ROUGHNESS,
       RT_S_GAMMA, RT_S_SHARPEN, RT_S_VIGNETTE, RT_S_FOV, RT_S_HEADLIGHT,
       RT_S_LIGHT_BRIGHTNESS, RT_S_COUNT };

static float rt_slider_get(const RT_MenuSlider *s)
{
	float v = 0.0f;
	switch (s->where)
	{
		case RT_VAL_CONFIG:       RT_ConfigReadFloat(RT_GetRendererIO()->config, RT_StringFromCString(s->key), &v); break;
		case RT_VAL_EXPLOSION:    v = g_rt_dynamic_light_info.explosionBrightMod; break;
		case RT_VAL_WEAPON:       v = g_rt_dynamic_light_info.weaponBrightMod; break;
		case RT_VAL_HEADLIGHT:    v = g_headlights.brightness; break;
		case RT_VAL_LEVEL_LIGHTS: v = g_light_multiplier_default; break;
	}
	return v;
}

static void rt_slider_set(const RT_MenuSlider *s, float v)
{
	switch (s->where)
	{
		case RT_VAL_CONFIG:       RT_ConfigWriteFloat(RT_GetRendererIO()->config, RT_StringFromCString(s->key), v); break;
		case RT_VAL_EXPLOSION:    g_rt_dynamic_light_info.explosionBrightMod = v; break;
		case RT_VAL_WEAPON:       g_rt_dynamic_light_info.weaponBrightMod = v; break;
		case RT_VAL_HEADLIGHT:    g_headlights.brightness = v; break;
		case RT_VAL_LEVEL_LIGHTS: g_light_multiplier_default = g_light_multiplier = v; g_pending_light_update = true; break;
	}
}

static int rt_slider_steps(const RT_MenuSlider *s)
{
	if (s->table)
		return s->table_n - 1;
	return s->steps ? s->steps : RT_SLIDER_STEPS;
}

static int rt_slider_notch(const RT_MenuSlider *s, float v)
{
	if (s->table) // nearest exact notch
	{
		int k, best = 0;
		for (k = 1; k < s->table_n; k++)
			if (fabsf(s->table[k] - v) < fabsf(s->table[best] - v))
				best = k;
		return best;
	}
	float t = RT_CLAMP((v - s->lo) / (s->hi - s->lo), 0.0f, 1.0f);
	if (s->inverted)
		t = 1.0f - t;
	return (int)(powf(t, 1.0f / s->curve) * rt_slider_steps(s) + 0.5f);
}

static float rt_slider_value(const RT_MenuSlider *s, int notch)
{
	if (s->table)
		return s->table[RT_CLAMP(notch, 0, s->table_n - 1)];
	float t = powf((float)notch / rt_slider_steps(s), s->curve);
	if (s->inverted)
		t = 1.0f - t;
	return s->lo + (s->hi - s->lo) * t;
}

static void rt_slider_label(RT_MenuSlider *s, float v)
{
	char num[24];
	snprintf(num, sizeof(num), s->fmt, v);
	snprintf(s->label, sizeof(s->label), "%s: %s", s->name, num);
}

static int  rt_opt_render_mode, rt_opt_fsr_mode; // render mode radios: Off, TAA, FSR
static int  rt_fsr_mode_pending, rt_fsr_mode_start;
static float rt_fsr_sharpness;
static int  rt_fsr_sharpness_changed;
static char rt_fsr_mode_label[48];

static void rt_update_fsr_label(void)
{
	snprintf(rt_fsr_mode_label, sizeof(rt_fsr_mode_label), "  FSR Mode: %s", rt_fsr_mode_names[rt_fsr_mode_pending]);
}

// Render Mode > FSR Mode: the modes and the FSR sharpening in their own window.
static void rt_fsr_mode_menu(void)
{
	static char sharpen_label[48];
	newmenu_item m[9];
	int n = 0, k, opt_first, opt_sharpen;
	opt_first = n;
	for (k = 0; k < 6; k++)
	{
		m[n].type = NM_TYPE_RADIO; m[n].text = (char *)rt_fsr_mode_names[k];
		m[n].value = (k == rt_fsr_mode_pending); m[n].group = 0; n++;
	}
	m[n].type = NM_TYPE_TEXT; m[n].text = ""; n++;
	opt_sharpen = n;
	snprintf(sharpen_label, sizeof(sharpen_label), "FSR Sharpening: %.2f", rt_fsr_sharpness);
	m[n].type = NM_TYPE_SLIDER; m[n].text = sharpen_label; m[n].value = (int)(RT_CLAMP(rt_fsr_sharpness, 0.0f, 1.0f) * RT_SLIDER_STEPS + 0.5f);
	m[n].min_value = 0; m[n].max_value = RT_SLIDER_STEPS; n++;
	int sharpen_start = m[opt_sharpen].value;

	newmenu_do1(NULL, "FSR Mode", n, m, NULL, NULL, opt_first + rt_fsr_mode_pending);

	for (k = 0; k < 6; k++)
		if (m[opt_first + k].value)
			rt_fsr_mode_pending = k;
	if (m[opt_sharpen].value != sharpen_start)
	{
		rt_fsr_sharpness = (float)m[opt_sharpen].value / RT_SLIDER_STEPS;
		RT_ConfigWriteFloat(RT_GetRendererIO()->config, RT_StringLiteral("amd_fsr2_sharpness"), rt_fsr_sharpness);
		rt_fsr_sharpness_changed = 1;
	}
	rt_update_fsr_label();
}

static void rt_show_fsr_mode(newmenu_item *items, int show)
{
	items[rt_opt_fsr_mode].type = show ? NM_TYPE_MENU : NM_TYPE_TEXT;
	items[rt_opt_fsr_mode].text = show ? rt_fsr_mode_label : "";
}

// Raytracing Options state while the menu is open.
static int rt_menu_nitems;
static int start_values[48];
static int opt_ignore_materials, opt_raytracing, opt_pbr, opt_flash_mul, opt_retrace_rays;
static int opt_soft_transparency, opt_lights_shadows, opt_projectile_shadows;

// Pushes the menu's current values into the renderer and the game right away (only what was
// changed since the menu opened), and refreshes the value shown in each slider's label.
static void rt_menu_apply(newmenu_item *m)
{
	RT_Config *config = RT_GetRendererIO()->config;
	int k;

#define RT_WRITE_CHECK(opt, key) \
	if (m[opt].value != start_values[opt]) RT_ConfigWriteInt(config, RT_StringLiteral(key), m[opt].value);
	RT_WRITE_CHECK(opt_ignore_materials, "override_materials");
	RT_WRITE_CHECK(opt_retrace_rays, "retrace_rays");
	RT_WRITE_CHECK(opt_raytracing, "enable_pathtracing");
	RT_WRITE_CHECK(opt_pbr, "enable_pbr");
	RT_WRITE_CHECK(opt_flash_mul, "screen_flash_mul");
	RT_WRITE_CHECK(opt_soft_transparency, "smooth_transparency");
	RT_WRITE_CHECK(opt_lights_shadows, "lights_cast_shadows");
	RT_WRITE_CHECK(opt_projectile_shadows, "projectiles_cast_shadows");
#undef RT_WRITE_CHECK
	for (k = 0; k < 3; k++)
		if (m[rt_opt_render_mode + k].value && !start_values[rt_opt_render_mode + k])
			RT_ConfigWriteInt(config, RT_StringLiteral("upscaling_aa_mode"), k);
	if (rt_fsr_mode_pending != rt_fsr_mode_start)
		RT_ConfigWriteInt(config, RT_StringLiteral("amd_fsr2_mode"), rt_fsr_mode_values[rt_fsr_mode_pending]);

	for (k = 0; k < RT_S_COUNT; k++)
	{
		RT_MenuSlider *s = &rt_sliders[k];
		if (m[s->opt].value != s->start)
		{
			float v = rt_slider_value(s, m[s->opt].value);
			rt_slider_set(s, v);
			rt_slider_label(s, v);
		}
	}


	config->last_modified_time = RT_GetHighResTime().value; // renderer picks the changes up next frame
}

static int rt_menu_any_changed(newmenu_item *m)
{
	int k, changed = rt_fsr_mode_pending != rt_fsr_mode_start || rt_fsr_sharpness_changed;
	for (k = 0; k < rt_menu_nitems; k++)
		changed |= (m[k].value != start_values[k]);
	return changed;
}

int raytrace_config_menuset(newmenu *menu, d_event *event, void *userdata)
{
	newmenu_item *items = newmenu_get_items(menu);
	int citem = newmenu_get_citem(menu);

	userdata = userdata;

	switch (event->type)
	{
		case EVENT_NEWMENU_CHANGED:
			// The FSR Mode line only shows while FSR is the render mode.
			if (citem >= rt_opt_render_mode && citem < rt_opt_render_mode + 3)
				rt_show_fsr_mode(items, items[rt_opt_render_mode + 2].value);
			rt_menu_apply(items); // live: see it in the game behind the menu
			break;

		case EVENT_NEWMENU_SELECTED:
			if (citem == rt_opt_fsr_mode)
			{
				rt_fsr_mode_menu();
				rt_menu_apply(items);
			}
			return 1;		// stay in menu

		default:
			break;
	}

	return 0;
}

void raytrace_config()
{
	newmenu_item m[48];
	int nitems = 0;
	int flash_mul = 1, upscaling, fsr_mode, k;

	RT_RendererIO* io = RT_GetRendererIO();
	RT_Config* config = io->config;

#define RT_CHECK(var, label, key) \
	var = nitems; m[nitems].type = NM_TYPE_CHECK; m[nitems].text = label; \
	m[nitems].value = RT_GetIntFromConfig(config, RT_StringLiteral(key)) != 0; nitems++;
#define RT_SLIDER(index) { \
	RT_MenuSlider *s = &rt_sliders[index]; float v = rt_slider_get(s); \
	rt_slider_label(s, v); s->opt = nitems; s->start = rt_slider_notch(s, v); \
	m[nitems].type = NM_TYPE_SLIDER; m[nitems].text = s->label; m[nitems].value = s->start; \
	m[nitems].min_value = 0; m[nitems].max_value = rt_slider_steps(s); nitems++; }

	RT_CHECK(opt_raytracing, "Enable Pathtracing", "enable_pathtracing");
	RT_CHECK(opt_pbr, "Enable PBR", "enable_pbr");

	m[nitems].type = NM_TYPE_TEXT; m[nitems].text = ""; nitems++;
	m[nitems].type = NM_TYPE_TEXT; m[nitems].text = "Render Mode:"; nitems++;
	upscaling = RT_GetIntFromConfig(config, RT_StringLiteral("upscaling_aa_mode"));
	if (upscaling < 0 || upscaling > 2)
		upscaling = 0;
	rt_opt_render_mode = nitems;
	m[nitems].type = NM_TYPE_RADIO; m[nitems].text = "Off"; m[nitems].value = (upscaling == 0); m[nitems].group = 1; nitems++;
	m[nitems].type = NM_TYPE_RADIO; m[nitems].text = "TAA"; m[nitems].value = (upscaling == 1); m[nitems].group = 1; nitems++;
	m[nitems].type = NM_TYPE_RADIO; m[nitems].text = "FSR"; m[nitems].value = (upscaling == 2); m[nitems].group = 1; nitems++;
	fsr_mode = RT_GetIntFromConfig(config, RT_StringLiteral("amd_fsr2_mode"));
	rt_fsr_mode_pending = 2; // Quality
	for (k = 0; k < 6; k++)
		if (rt_fsr_mode_values[k] == fsr_mode)
			rt_fsr_mode_pending = k;
	rt_fsr_mode_start = rt_fsr_mode_pending;
	rt_fsr_sharpness = 0.0f;
	rt_fsr_sharpness_changed = 0;
	RT_ConfigReadFloat(config, RT_StringLiteral("amd_fsr2_sharpness"), &rt_fsr_sharpness);
	rt_update_fsr_label();
	rt_opt_fsr_mode = nitems;
	m[nitems].type = NM_TYPE_MENU; m[nitems].text = rt_fsr_mode_label; nitems++;
	rt_show_fsr_mode(m, upscaling == 2);

	m[nitems].type = NM_TYPE_TEXT; m[nitems].text = ""; nitems++;
	RT_SLIDER(RT_S_BLOOM);
	RT_SLIDER(RT_S_EXPLOSION);
	RT_SLIDER(RT_S_WEAPON);

	m[nitems].type = NM_TYPE_TEXT; m[nitems].text = ""; nitems++;
	m[nitems].type = NM_TYPE_TEXT; m[nitems].text = "Tonemap:"; nitems++;
	RT_SLIDER(RT_S_EXPOSURE);

	m[nitems].type = NM_TYPE_TEXT; m[nitems].text = ""; nitems++;
	m[nitems].type = NM_TYPE_TEXT; m[nitems].text = "Post Processing:"; nitems++;
	RT_SLIDER(RT_S_GAMMA);
	RT_SLIDER(RT_S_SHARPEN);
	RT_SLIDER(RT_S_VIGNETTE);

	m[nitems].type = NM_TYPE_TEXT; m[nitems].text = ""; nitems++;
	RT_SLIDER(RT_S_HEADLIGHT);
	RT_SLIDER(RT_S_FOV);
	RT_SLIDER(RT_S_LIGHT_BRIGHTNESS);


	m[nitems].type = NM_TYPE_TEXT; m[nitems].text = ""; nitems++;
	// Flash screen multiplier (Ingame Tools > Miscellaneous): 0 = no screen flash, 1 = normal.
	RT_ConfigReadInt(config, RT_StringLiteral("screen_flash_mul"), &flash_mul);
	opt_flash_mul = nitems;
	m[nitems].type = NM_TYPE_SLIDER; m[nitems].text = "Screen Flash (0 = off)"; m[nitems].value = RT_CLAMP(flash_mul, 0, 10);
	m[nitems].min_value = 0; m[nitems].max_value = 10; nitems++;

	// Same order as the renderer window's Pathtracer section.
	m[nitems].type = NM_TYPE_TEXT; m[nitems].text = ""; nitems++;
	RT_CHECK(opt_ignore_materials, "Override Materials", "override_materials");
	RT_CHECK(opt_retrace_rays, "Retrace Rays", "retrace_rays");
	RT_SLIDER(RT_S_SPECULAR);
	RT_SLIDER(RT_S_ROUGHNESS);
	RT_CHECK(opt_soft_transparency, "Smooth Transparency", "smooth_transparency");
	RT_CHECK(opt_lights_shadows, "Lights Cast Shadows", "lights_cast_shadows");
	RT_CHECK(opt_projectile_shadows, "Projectiles Cast Shadows", "projectiles_cast_shadows");

#undef RT_CHECK
#undef RT_SLIDER

	RT_ASSERT(nitems <= RT_ARRAY_COUNT(m));
	for (k = 0; k < nitems; k++)
		start_values[k] = m[k].value;
	rt_menu_nitems = nitems;

	newmenu_do1( NULL, "Raytracing Options", nitems, m, raytrace_config_menuset, NULL, 0 );

	if (!rt_menu_any_changed(m))
		return;
	rt_menu_apply(m);
	RT_SerializeConfigToFile(config, RT_RENDER_SETTINGS_CONFIG_FILE);
}
#endif

int opt_gr_texfilt, opt_gr_movietexfilt, opt_gr_brightness, opt_gr_reticlemenu, opt_gr_alphafx, opt_gr_dynlightcolor, opt_gr_vsync, opt_gr_multisample, opt_gr_fpsindi, opt_gr_disablecockpit;
#ifdef RT_DX12
int opt_gr_raytracemenu; opt_gr_enable_pathtracing;
#endif

#include "vr_graphics_menu.inl"
#ifdef RT_DX12
static int opt_gr_shieldfx;
#endif
int graphics_config_menuset(newmenu *menu, d_event *event, void *userdata)
{
	newmenu_item *items = newmenu_get_items(menu);
	int citem = newmenu_get_citem(menu);

	userdata = userdata;

	switch (event->type)
	{
		case EVENT_NEWMENU_CHANGED:
			if ( citem == opt_gr_texfilt + 3
#ifdef OGL
				&& ogl_maxanisotropy <= 1.0
#endif
#ifdef RT_DX12
				&& false
#endif
				)
			{
				nm_messagebox( TXT_ERROR, 1, TXT_OK, "Anisotropic Filtering not\nsupported by your hardware/driver.");
				items[opt_gr_texfilt + 3].value = 0;
				items[opt_gr_texfilt + 2].value = 1;
			}
			if ( citem == opt_gr_brightness)
				gr_palette_set_gamma(items[citem].value);
#ifdef RT_DX12
			// Picking a pack (or toggling Use Custom Textures) shows that pack's profile right away.
			if (citem == opt_gr_customtextures ||
				(gr_opt_texpack >= 0 && citem >= gr_opt_texpack && citem < gr_opt_texpack + gr_texpack_count))
				gr_menu_sync_profile(items);
#endif
			break;

		case EVENT_NEWMENU_SELECTED:
			if (citem == opt_gr_reticlemenu)
				reticle_config();
#ifdef RT_DX12
			if (citem == opt_gr_vrmenu) vr_graphics_config();
			if (citem == opt_gr_raytracemenu)
				raytrace_config();
			if (citem == gr_opt_profilemenu)
				gr_profile_menu();
			// Enter on Use Custom Textures, a pack or Render Profile: apply now, stay in the menu.
			if (citem >= opt_gr_customtextures && citem <= gr_opt_profilemenu)
				gr_menu_apply(items);
#endif
			
			return 1;		// stay in menu
			break;

		default:
			break;
	}

	return 0;
}

void graphics_config()
{
#ifdef OGL
	// +1 headroom for the conditional VSync item (see gr_vsync_supported); the menu already uses 20 items without it.
	newmenu_item m[23]; // +1 for 3D Briefing Robots
	int i = 0;
#elif RT_DX12
	// +2 for Use Custom Textures and a spacer, + one radio per texture pack folder.
	newmenu_item m[17 + GR_MAX_TEXTURE_PACKS + GR_MAX_RENDER_PROFILES];
	int k;
#else
	// +1 headroom for the conditional VSync item (see gr_vsync_supported).
	newmenu_item m[7];
#endif
	int nitems = 0;

	// Storage for the framerate label and its value. Both must live for as long
	// as the menu does, because the menu keeps POINTERS to them and dereferences
	// those later, inside newmenu_do1().
	char framerate_label[32];
	char framerate_string[8];

#ifdef OGL
	m[nitems].type = NM_TYPE_TEXT; m[nitems].text = "Texture Filtering:"; nitems++;
	opt_gr_texfilt = nitems;
	m[nitems].type = NM_TYPE_RADIO; m[nitems].text = "None (Classical)"; m[nitems].value = 0; m[nitems].group = 0; nitems++;
	m[nitems].type = NM_TYPE_RADIO; m[nitems].text = "Bilinear"; m[nitems].value = 0; m[nitems].group = 0; nitems++;
	m[nitems].type = NM_TYPE_RADIO; m[nitems].text = "Trilinear"; m[nitems].value = 0; m[nitems].group = 0; nitems++;
	m[nitems].type = NM_TYPE_RADIO; m[nitems].text = "Anisotropic"; m[nitems].value = 0; m[nitems].group = 0; nitems++;
	opt_gr_movietexfilt = nitems;
	m[nitems].type = NM_TYPE_CHECK; m[nitems].text = "Movie Filter"; m[nitems].value = GameCfg.MovieTexFilt; nitems++;
	opt_gr_briefingrobots3d = nitems;
	m[nitems].type = NM_TYPE_CHECK; m[nitems].text = "3D Briefing Robots"; m[nitems].value = GameCfg.BriefingRobots3D; nitems++;
	m[nitems].type = NM_TYPE_TEXT; m[nitems].text = ""; nitems++;
#endif
#ifdef RT_DX12
	// Texture packs: every folder in assets\textures is a pack, layered over the Original textures.
	// One at a time; nothing is read from a pack folder unless Use Custom Textures is on.
	opt_gr_customtextures = nitems;
	m[nitems].type = NM_TYPE_CHECK; m[nitems].text = "Use Custom Textures"; m[nitems].value = GameCfg.UseCustomTextures; nitems++;
	gr_texpack_count = RT_ListTexturePacks(gr_texture_packs, GR_MAX_TEXTURE_PACKS);
	gr_opt_texpack = -1;
	if (gr_texpack_count)
	{
		int selected = 0;
		for (k = 0; k < gr_texpack_count; k++)
			if (!d_stricmp(gr_texture_packs[k], GameCfg.TexturePack))
				selected = k;
		gr_opt_texpack = nitems;
		for (k = 0; k < gr_texpack_count; k++)
		{
			m[nitems].type = NM_TYPE_RADIO; m[nitems].text = gr_texture_packs[k];
			m[nitems].value = (k == selected); m[nitems].group = 0; nitems++;
		}
	}
	else
	{
		m[nitems].type = NM_TYPE_TEXT; m[nitems].text = "(no pack folders in assets\\textures)"; nitems++;
	}
	// Render profiles (assets\render_presets), usable with any pack; a pack remembers its last one.
	// The list opens in its own window (gr_profile_menu) so many profiles don't crowd this menu.
	gr_set_pending_profile(GameCfg.RenderProfile);
	gr_opt_profilemenu = nitems;
	m[nitems].type = NM_TYPE_MENU; m[nitems].text = gr_profile_item_text; nitems++;
	m[nitems].type = NM_TYPE_TEXT; m[nitems].text = ""; nitems++;
	snprintf(gr_applied_pack_key, sizeof(gr_applied_pack_key), "%s", gr_texture_pack_key());
	opt_gr_vrmenu = nitems;
	m[nitems].type=NM_TYPE_MENU;m[nitems].text="VR Graphics";nitems++;
	opt_gr_raytracemenu = nitems;
	m[nitems].type = NM_TYPE_MENU; m[nitems].text = "Raytracing Options"; nitems++;
#endif
	opt_gr_brightness = nitems;
	m[nitems].type = NM_TYPE_SLIDER; m[nitems].text = TXT_BRIGHTNESS; m[nitems].value = gr_palette_get_gamma(); m[nitems].min_value = 0; m[nitems].max_value = 16; nitems++;
	opt_gr_reticlemenu = nitems;
	m[nitems].type = NM_TYPE_MENU; m[nitems].text = "Reticle Options"; nitems++;

#ifdef RT_DX12
	opt_gr_shieldfx = nitems;
	m[nitems].type = NM_TYPE_CHECK; m[nitems].text = "Shield Effects"; m[nitems].value = PlayerCfg.ShieldEffects; nitems++;
#endif // RT_DX12

#ifdef OGL
	opt_gr_alphafx = nitems;
	m[nitems].type = NM_TYPE_CHECK; m[nitems].text = "Transparency Effects"; m[nitems].value = PlayerCfg.AlphaEffects; nitems++;
	opt_gr_dynlightcolor = nitems;
	m[nitems].type = NM_TYPE_CHECK; m[nitems].text = "Colored Dynamic Light"; m[nitems].value = PlayerCfg.DynLightColor; nitems++;
// VSync option is shown ONLY when the video backend can genuinely control vsync
// (see gr_vsync_supported()). On the current SDL 1.2 backend it stays hidden,
// because that API layer has no way to enable, disable or even query vsync --
// the setting used to appear unconditionally and silently overrode the Framerate
// value instead, which is exactly the misleading control this check prevents.
//
// A future SDL2 backend returns non-zero and the option appears automatically,
// wired to a real swap interval.
	if (gr_vsync_supported())
	{
		opt_gr_vsync = nitems;
		m[nitems].type = NM_TYPE_CHECK;
		m[nitems].text="VSync";
		m[nitems].value = GameCfg.VSync;
		nitems++;
	}
	else
	{
		// Sentinel, so the read-back below can never index the wrong entry.
		opt_gr_vsync = -1;
	}
	opt_gr_multisample = nitems;
	m[nitems].type = NM_TYPE_CHECK; m[nitems].text="4x multisampling"; m[nitems].value = GameCfg.Multisample; nitems++;
	opt_gr_classicdepth = nitems;
	m[nitems].type = NM_TYPE_CHECK; m[nitems].text="Classic Depth Ordering (SP)"; m[nitems].value = GameCfg.ClassicDepth; nitems++;
	opt_gr_custommodels = nitems;
	m[nitems].type = NM_TYPE_CHECK; m[nitems].text="3D Models"; m[nitems].value = GameCfg.UseCustomModels; nitems++;
	opt_gr_customtextures = nitems;
	m[nitems].type = NM_TYPE_CHECK; m[nitems].text="Custom Textures"; m[nitems].value = GameCfg.UseCustomTextures; nitems++;
	opt_gr_reducedflash = nitems;
	m[nitems].type = NM_TYPE_CHECK; m[nitems].text="Reduced Flash"; m[nitems].value = GameCfg.ReducedFlash; nitems++;
#endif
	opt_gr_fpsindi = nitems;
	m[nitems].type = NM_TYPE_CHECK; m[nitems].text="FPS Counter"; m[nitems].value = GameCfg.FPSIndicator; nitems++;

	opt_gr_disablecockpit = nitems;
	m[nitems].type = NM_TYPE_CHECK; m[nitems].text="Disable Cockpit View"; m[nitems].value = PlayerCfg.DisableCockpit; nitems++;
#ifdef OGL
	m[opt_gr_texfilt+GameCfg.TexFilt].value=1;
#endif


	// Label states the accepted range so the user does not have to guess. Built
	// from the same constants the clamps below use (MIN_FRAMERATE_CAP and
	// MAXIMUM_FPS), so the text can never drift out of step with the values that
	// are actually accepted.
	snprintf(framerate_label, sizeof(framerate_label) - 1, "Framerate (%d-%d)",
		MIN_FRAMERATE_CAP, MAXIMUM_FPS);

	m[nitems].type = NM_TYPE_TEXT;
	m[nitems].text = framerate_label;
	nitems++;

	snprintf(framerate_string,sizeof(framerate_string)-1,"%d",PlayerCfg.maxFps);
	m[nitems].type = NM_TYPE_INPUT; m[nitems].text=framerate_string; m[nitems].text_len=sizeof(framerate_string)-1;  nitems++;

	newmenu_do1( NULL, "Graphics Options", nitems, m, graphics_config_menuset, NULL, 1 );

	// Snapshot the settings that can invalidate the cached GL textures, so the
	// rebuild below only runs when one of them actually changed.
	int old_texfilt = GameCfg.TexFilt;
	int old_multisample = GameCfg.Multisample;
	int old_custommodels = GameCfg.UseCustomModels;

	int old_customtextures = GameCfg.UseCustomTextures;

#ifdef OGL
	for (i = 0; i <= 3; i++)
		if (m[i+opt_gr_texfilt].value)
			GameCfg.TexFilt = i;
	GameCfg.MovieTexFilt = m[opt_gr_movietexfilt].value;
	GameCfg.BriefingRobots3D = m[opt_gr_briefingrobots3d].value;
	PlayerCfg.AlphaEffects = m[opt_gr_alphafx].value;
	PlayerCfg.DynLightColor = m[opt_gr_dynlightcolor].value;
	// The VSync item only exists when the backend supports vsync, so only read it
	// back in that case. opt_gr_vsync is -1 otherwise (see graphics_config).
	if (opt_gr_vsync >= 0)
		GameCfg.VSync = m[opt_gr_vsync].value;
	GameCfg.Multisample = m[opt_gr_multisample].value;
	GameCfg.ClassicDepth = m[opt_gr_classicdepth].value;
	if (GameCfg.UseCustomModels != m[opt_gr_custommodels].value)
	{
		GameCfg.UseCustomModels = m[opt_gr_custommodels].value;
		if (GameCfg.UseCustomModels)
			xmodel_load_all();
		else
			xmodel_free_all();
	}
	GameCfg.UseCustomTextures = m[opt_gr_customtextures].value;
	// Purely local preference -- nothing to rebuild, and the effect sites read it
	// through reduced_flash_wanted() every frame.
	GameCfg.ReducedFlash = m[opt_gr_reducedflash].value;
#endif
	GameCfg.GammaLevel = m[opt_gr_brightness].value;
	GameCfg.FPSIndicator = m[opt_gr_fpsindi].value;
	PlayerCfg.DisableCockpit = m[opt_gr_disablecockpit].value;
#ifdef RT_DX12
	PlayerCfg.ShieldEffects = m[opt_gr_shieldfx].value;
	gr_menu_apply(m); // pack + profile (Esc applies on the way out)
#endif


	PlayerCfg.maxFps=atoi(framerate_string);

	if(PlayerCfg.maxFps < MIN_FRAMERATE_CAP) {
		PlayerCfg.maxFps = MIN_FRAMERATE_CAP;
	} else if (PlayerCfg.maxFps > MAXIMUM_FPS) {
		PlayerCfg.maxFps = MAXIMUM_FPS;
	}

#ifdef OGL
	// Only rebuild the texture cache when something that affects it changed.
	// Toggling Custom Textures is the slowest case (it re-reads the custom
	// texture override tables), so tell the player what is happening rather
	// than leaving the screen frozen with no feedback.
	{
		int texfilt_dirty     = (GameCfg.TexFilt != old_texfilt);
		int multisample_dirty = (GameCfg.Multisample != old_multisample);
		int models_dirty      = (GameCfg.UseCustomModels != old_custommodels);
		int ctex_dirty        = (GameCfg.UseCustomTextures != old_customtextures);

		if (texfilt_dirty || multisample_dirty || models_dirty || ctex_dirty)
		{
			// When ONLY custom textures changed, direction matters: the PNG path
			// loads lazily per bitmap (see ogl_loadbmtexture_f, which early-exits
			// when a valid GL handle is already resident).
			//
			// When ONLY 3D models changed, touch nothing but the models: the ASE
			// parse/free already happened above via xmodel_load_all() /
			// xmodel_free_all(), and each model's own textures GL-upload lazily
			// on first draw. This keeps the models toggle from re-paging
			// GameBitmaps, which belongs to Custom Textures only.
			//
			// Anything else (filtering, multisample) genuinely needs the
			// full rebuild, because those settings are baked into every texture
			// that is already resident.
			int only_ctex = ctex_dirty && !texfilt_dirty && !multisample_dirty && !models_dirty;
			int only_models = models_dirty && !texfilt_dirty && !multisample_dirty && !ctex_dirty;

			if (only_ctex)
			{
				if (GameCfg.UseCustomTextures)
				{
					// Re-TICK (ON): stale plain-bitmap GL handles from the OFF
					// state would make the loader early-exit ("already loaded")
					// and never re-ask for the PNG -- including globals the
					// level never touches (HUD, weapons, projectiles, explosions,
					// powerups, robots). So invalidate ALL handles via
					// gr_set_attributes() + gr_set_mode(), then walk EVERY
					// bitmap so each one checks for its "<name>.png" override
					// again (d2 has no per-level override tables to re-read).
					// One-time hitch, correct.
					show_boxed_message("Applying Texture Settings...", 0);

					gr_set_attributes();
					gr_set_mode(Game_screen_mode);

					if (Game_wind)
						ogl_cache_level_textures();
				}
				else
				{
					// UN-TICK (OFF): plain bitmaps are already resident, so just
					// drop the PNG entries and let them fall back lazily. Fast.
					show_boxed_message("Applying Texture Settings...", 0);

					if (Game_wind)
						ogl_recache_custom_textures();
				}
			}
			else if (only_models)
			{
				// Models-only change: no bitmap or full-cache work. ASE data was
				// already loaded/freed above; GL textures self-populate on next
				// draw. Popup names the toggle so the player gets feedback for
				// the (usually fast) op.
				show_boxed_message("Applying Model Settings...", 0);
			}
			else
			{
				// Draw and FLIP the message before the blocking work starts, so it
				// is actually visible during the pause instead of appearing after.
				show_boxed_message("Applying Texture Settings...", 0);

				gr_set_attributes();
				gr_set_mode(Game_screen_mode);
				if (Game_wind)
					ogl_cache_level_textures();
			}
		}
	}
#endif
}

#if PHYSFS_VER_MAJOR >= 2
typedef struct browser
{
	char	*title;			// The title - needed for making another listbox when changing directory
	int		(*when_selected)(void *userdata, const char *filename);	// What to do when something chosen
	void	*userdata;		// Whatever you want passed to when_selected
	char	**list;			// All menu items
	char	*list_buf;		// Buffer for menu item text: hopefully reduces memory fragmentation this way
	const char	*const *ext_list;		// List of file extensions we're looking for (if looking for a music file many types are possible)
	int		select_dir;		// Allow selecting the current directory (e.g. for Jukebox level song directory)
	int		num_files;		// Number of list items found (including parent directory and current directory if selectable)
	int		max_files;		// How many entries we can have before having to grow the array
	int		max_buf;		// How much text we can have before having to grow the buffer
	char	view_path[PATH_MAX];	// The absolute path we're currently looking at
	int		new_path;		// Whether the view_path is a new searchpath, if so, remove it when finished
} browser;

void list_dir_el(browser *b, const char *origdir, const char *fname)
{
	char *ext;
	const char *const *i = NULL;
	
	ext = strrchr(fname, '.');
	if (ext)
		for (i = b->ext_list; *i != NULL && d_stricmp(ext, *i); i++) {}	// see if the file is of a type we want
	
	if ((!strcmp((PHYSFS_getRealDir(fname)==NULL?"":PHYSFS_getRealDir(fname)), b->view_path)) && (PHYSFS_isDirectory(fname) || (ext && *i))
#if defined(__MACH__) && defined(__APPLE__)
		&& d_stricmp(fname, "Volumes")	// this messes things up, use '..' instead
#endif
		)
		string_array_add(&b->list, &b->list_buf, &b->num_files, &b->max_files, &b->max_buf, fname);
}

int list_directory(browser *b)
{
	if (!string_array_new(&b->list, &b->list_buf, &b->num_files, &b->max_files, &b->max_buf))
		return 0;
	
	strcpy(b->list_buf, "..");		// go to parent directory
	b->list[b->num_files++] = b->list_buf;
	
	if (b->select_dir)
	{
		b->list[b->num_files] = b->list[b->num_files - 1] + strlen(b->list[b->num_files - 1]) + 1;
		strcpy(b->list[b->num_files++], "<this directory>");	// choose the directory being viewed
	}
	
	PHYSFS_enumerateFilesCallback("", (PHYSFS_EnumFilesCallback) list_dir_el, b);
	string_array_tidy(&b->list, &b->list_buf, &b->num_files, &b->max_files, &b->max_buf, 1 + (b->select_dir ? 1 : 0),
#ifdef __LINUX__
					  strcmp
#else
					  d_stricmp
#endif
					  );
					  
	return 1;
}

static int select_file_recursive(char *title, const char *orig_path, const char *const *ext_list, int select_dir, int (*when_selected)(void *userdata, const char *filename), void *userdata);

int select_file_handler(listbox *menu, d_event *event, browser *b)
{
	char newpath[PATH_MAX];
	char **list = listbox_get_items(menu);
	int citem = listbox_get_citem(menu);
	const char *sep = PHYSFS_getDirSeparator();

	memset(newpath, 0, sizeof(char)*PATH_MAX);
	switch (event->type)
	{
#ifdef _WIN32
		case EVENT_KEY_COMMAND:
		{
			if (event_key_get(event) == KEY_CTRLED + KEY_D)
			{
				newmenu_item *m;
				char *text = NULL;
				int rval = 0;

				MALLOC(text, char, 2);
				MALLOC(m, newmenu_item, 1);
				snprintf(text, sizeof(char)*PATH_MAX, "c");
				m->type=NM_TYPE_INPUT; m->text_len = 3; m->text = text;
				rval = newmenu_do( NULL, "Enter drive letter", 1, m, NULL, NULL );
				text[1] = '\0'; 
				snprintf(newpath, sizeof(char)*PATH_MAX, "%s:%s", text, sep);
				if (!rval && strlen(text))
				{
					select_file_recursive(b->title, newpath, b->ext_list, b->select_dir, b->when_selected, b->userdata);
					// close old box.
					event->type = EVENT_WINDOW_CLOSED;
					window_close(listbox_get_window(menu));
				}
				d_free(text);
				d_free(m);
				return 0;
			}
			break;
		}
#endif
		case EVENT_NEWMENU_SELECTED:
			strcpy(newpath, b->view_path);

			if (citem == 0)		// go to parent dir
			{
				char *p;
				
				if ((p = strstr(&newpath[strlen(newpath) - strlen(sep)], sep)))
					if (p != strstr(newpath, sep))	// if this isn't the only separator (i.e. it's not about to look at the root)
						*p = 0;
				
				p = newpath + strlen(newpath) - 1;
				while ((p > newpath) && strncmp(p, sep, strlen(sep)))	// make sure full separator string is matched (typically is)
					p--;
				
				if (p == strstr(newpath, sep))	// Look at root directory next, if not already
				{
#if defined(__MACH__) && defined(__APPLE__)
					if (!d_stricmp(p, "/Volumes"))
						return 1;
#endif
					if (p[strlen(sep)] != '\0')
						p[strlen(sep)] = '\0';
					else
					{
#if defined(__MACH__) && defined(__APPLE__)
						// For Mac OS X, list all active volumes if we leave the root
						strcpy(newpath, "/Volumes");
#else
						return 1;
#endif
					}
				}
				else
					*p = '\0';
			}
			else if (citem == 1 && b->select_dir)
				return !(*b->when_selected)(b->userdata, "");
			else
			{
				if (strncmp(&newpath[strlen(newpath) - strlen(sep)], sep, strlen(sep)))
				{
					strncat(newpath, sep, PATH_MAX - 1 - strlen(newpath));
					newpath[PATH_MAX - 1] = '\0';
				}
				strncat(newpath, list[citem], PATH_MAX - 1 - strlen(newpath));
				newpath[PATH_MAX - 1] = '\0';
			}
			
			if ((citem == 0) || PHYSFS_isDirectory(list[citem]))
			{
				// If it fails, stay in this one
				return !select_file_recursive(b->title, newpath, b->ext_list, b->select_dir, b->when_selected, b->userdata);
			}
			
			return !(*b->when_selected)(b->userdata, list[citem]);
			break;
			
		case EVENT_WINDOW_CLOSE:
			if (b->new_path)
				PHYSFS_removeFromSearchPath(b->view_path);

			if (list)
				d_free(list);
			if (b->list_buf)
				d_free(b->list_buf);
			d_free(b);
			break;
			
		default:
			break;
	}
	
	return 0;
}

static int select_file_recursive(char *title, const char *orig_path, const char *const *ext_list, int select_dir, int (*when_selected)(void *userdata, const char *filename), void *userdata)
{
	browser *b;
	const char *sep = PHYSFS_getDirSeparator();
	char *p;
	char new_path[PATH_MAX];
	
	MALLOC(b, browser, 1);
	if (!b)
		return 0;
	
	b->title = title;
	b->when_selected = when_selected;
	b->userdata = userdata;
	b->ext_list = ext_list;
	b->select_dir = select_dir;
	b->num_files = b->max_files = 0;
	b->view_path[0] = '\0';
	b->new_path = 1;
	
	// Check for a PhysicsFS path first, saves complication!
	if (orig_path && strncmp(orig_path, sep, strlen(sep)) && PHYSFSX_exists(orig_path,0))
	{
		PHYSFSX_getRealPath(orig_path, new_path);
		orig_path = new_path;
	}

	// Set the viewing directory to orig_path, or some parent of it
	if (orig_path)
	{
		// Must make this an absolute path for directory browsing to work properly
#ifdef _WIN32
		if (!(isalpha(orig_path[0]) && (orig_path[1] == ':')))	// drive letter prompt (e.g. "C:"
#elif defined(macintosh)
		if (orig_path[0] == ':')
#else
		if (orig_path[0] != '/')
#endif
		{
			strncpy(b->view_path, PHYSFS_getBaseDir(), PATH_MAX - 1);		// current write directory must be set to base directory
			b->view_path[PATH_MAX - 1] = '\0';
#ifdef macintosh
			orig_path++;	// go past ':'
#endif
			strncat(b->view_path, orig_path, PATH_MAX - 1 - strlen(b->view_path));
			b->view_path[PATH_MAX - 1] = '\0';
		}
		else
		{
			strncpy(b->view_path, orig_path, PATH_MAX - 1);
			b->view_path[PATH_MAX - 1] = '\0';
		}

		p = b->view_path + strlen(b->view_path) - 1;
		b->new_path = PHYSFSX_isNewPath(b->view_path);
		
		while (!PHYSFS_addToSearchPath(b->view_path, 0))
		{
			while ((p > b->view_path) && strncmp(p, sep, strlen(sep)))
				p--;
			*p = '\0';
			
			if (p == b->view_path)
				break;
			
			b->new_path = PHYSFSX_isNewPath(b->view_path);
		}
	}
	
	// Set to user directory if we couldn't find a searchpath
	if (!b->view_path[0])
	{
		strncpy(b->view_path, PHYSFS_getUserDir(), PATH_MAX - 1);
		b->view_path[PATH_MAX - 1] = '\0';
		b->new_path = PHYSFSX_isNewPath(b->view_path);
		if (!PHYSFS_addToSearchPath(b->view_path, 0))
		{
			d_free(b);
			return 0;
		}
	}
	
	if (!list_directory(b))
	{
		d_free(b);
		return 0;
	}
	
	return newmenu_listbox1(title, b->num_files, b->list, 1, 0, (int (*)(listbox *, d_event *, void *))select_file_handler, b) != NULL;
}

#define PATH_HEADER_TYPE NM_TYPE_MENU
#define BROWSE_TXT " (browse...)"

#else

static int select_file_recursive(char *title, const char *orig_path, const char *const *ext_list, int select_dir, int (*when_selected)(void *userdata, const char *filename), void *userdata)
{
	return 0;
}

#define PATH_HEADER_TYPE NM_TYPE_TEXT
#define BROWSE_TXT

#endif

int opt_sm_digivol = -1, opt_sm_musicvol = -1, opt_sm_revstereo = -1, opt_sm_mtype0 = -1, opt_sm_mtype1 = -1, opt_sm_mtype2 = -1, opt_sm_mtype3 = -1, opt_sm_redbook_playorder = -1, opt_sm_mtype3_lmpath = -1, opt_sm_mtype3_lmplayorder1 = -1, opt_sm_mtype3_lmplayorder2 = -1, opt_sm_mtype3_lmplayorder3 = -1, opt_sm_cm_mtype3_file1_b = -1, opt_sm_cm_mtype3_file1 = -1, opt_sm_cm_mtype3_file2_b = -1, opt_sm_cm_mtype3_file2 = -1, opt_sm_cm_mtype3_file3_b = -1, opt_sm_cm_mtype3_file3 = -1, opt_sm_cm_mtype3_file4_b = -1, opt_sm_cm_mtype3_file4 = -1, opt_sm_cm_mtype3_file5_b = -1, opt_sm_cm_mtype3_file5 = -1;

void set_extmusic_volume(int volume);

int get_absolute_path(char *full_path, const char *rel_path)
{
	PHYSFSX_getRealPath(rel_path, full_path);
	return 1;
}

#ifdef USE_SDLMIXER
#define SELECT_SONG(t, s)	select_file_recursive(t, GameCfg.CMMiscMusic[s], jukebox_exts, 0, (int (*)(void *, const char *))get_absolute_path, GameCfg.CMMiscMusic[s])
#endif

int sound_menuset(newmenu *menu, d_event *event, void *userdata)
{
	newmenu_item *items = newmenu_get_items(menu);
	int citem = newmenu_get_citem(menu);
	//int nitems = newmenu_get_nitems(menu);
	int replay = 0;
	int rval = 0;

	switch (event->type)
	{
		case EVENT_NEWMENU_CHANGED:
			if (citem == opt_sm_digivol)
			{
				GameCfg.DigiVolume = items[citem].value;
				digi_set_digi_volume( (GameCfg.DigiVolume*32768)/8 );
				digi_play_sample_once( SOUND_DROP_BOMB, F1_0 );
			}
			else if (citem == opt_sm_musicvol)
			{
				GameCfg.MusicVolume = items[citem].value;
				songs_set_volume(GameCfg.MusicVolume);
			}
			else if (citem == opt_sm_revstereo)
			{
				GameCfg.ReverseStereo = items[citem].value;
			}
			else if (citem == opt_sm_mtype0)
			{
				GameCfg.MusicType = MUSIC_TYPE_NONE;
				replay = 1;
			}
			else if (citem == opt_sm_mtype1)
			{
				GameCfg.MusicType = MUSIC_TYPE_BUILTIN;
				replay = 1;
			}
			else if (citem == opt_sm_mtype2)
			{
				GameCfg.MusicType = MUSIC_TYPE_REDBOOK;
				replay = 1;
			}
#ifdef USE_SDLMIXER
			else if (citem == opt_sm_mtype3)
			{
				GameCfg.MusicType = MUSIC_TYPE_CUSTOM;
				replay = 1;
			}
#endif
			else if (citem == opt_sm_redbook_playorder)
			{
				GameCfg.OrigTrackOrder = items[citem].value;
				replay = (Game_wind != NULL);
			}
#ifdef USE_SDLMIXER
			else if (citem == opt_sm_mtype3_lmplayorder1)
			{
				GameCfg.CMLevelMusicPlayOrder = MUSIC_CM_PLAYORDER_CONT;
				replay = (Game_wind != NULL);
			}
			else if (citem == opt_sm_mtype3_lmplayorder2)
			{
				GameCfg.CMLevelMusicPlayOrder = MUSIC_CM_PLAYORDER_LEVEL;
				replay = (Game_wind != NULL);
			}
			else if (citem == opt_sm_mtype3_lmplayorder3)
			{
				GameCfg.CMLevelMusicPlayOrder = MUSIC_CM_PLAYORDER_RAND;
				replay = (Game_wind != NULL);
			}
#endif
			break;

		case EVENT_NEWMENU_SELECTED:
#ifdef USE_SDLMIXER
			if (citem == opt_sm_mtype3_lmpath)
			{
				static const char *const ext_list[] = { ".m3u", NULL };		// select a directory or M3U playlist
				select_file_recursive(
#ifndef _WIN32
					"Select directory or\nM3U playlist to\n play level music from",
#else
					"Select directory or\nM3U playlist to\n play level music from.\n CTRL-D to change drive",
#endif
									  GameCfg.CMLevelMusicPath, ext_list, 1,	// look in current music path for ext_list files and allow directory selection
									  (int (*)(void *, const char *))get_absolute_path, GameCfg.CMLevelMusicPath);	// just copy the absolute path
			}
			else if (citem == opt_sm_cm_mtype3_file1_b)
#ifndef _WIN32
				SELECT_SONG("Select main menu music", SONG_TITLE);
#else
				SELECT_SONG("Select main menu music.\nCTRL-D to change drive", SONG_TITLE);
#endif
			else if (citem == opt_sm_cm_mtype3_file2_b)
#ifndef _WIN32
				SELECT_SONG("Select briefing music", SONG_BRIEFING);
#else
				SELECT_SONG("Select briefing music.\nCTRL-D to change drive", SONG_BRIEFING);
#endif
			else if (citem == opt_sm_cm_mtype3_file3_b)
#ifndef _WIN32
				SELECT_SONG("Select credits music", SONG_CREDITS);
#else
				SELECT_SONG("Select credits music.\nCTRL-D to change drive", SONG_CREDITS);
#endif
			else if (citem == opt_sm_cm_mtype3_file4_b)
#ifndef _WIN32
				SELECT_SONG("Select escape sequence music", SONG_ENDLEVEL);
#else
				SELECT_SONG("Select escape sequence music.\nCTRL-D to change drive", SONG_ENDLEVEL);
#endif
			else if (citem == opt_sm_cm_mtype3_file5_b)
#ifndef _WIN32
				SELECT_SONG("Select game ending music", SONG_ENDGAME);
#else
				SELECT_SONG("Select game ending music.\nCTRL-D to change drive", SONG_ENDGAME);
#endif
#endif
			rval = 1;	// stay in menu
			break;

		case EVENT_WINDOW_CLOSE:
			d_free(items);
			break;

		default:
			break;
	}

	if (replay)
	{
		songs_uninit();

		if (Game_wind)
			songs_play_level_song( Current_level_num, 0 );
		else
			songs_play_song(SONG_TITLE, 1);
	}

	userdata = userdata;

	return rval;
}

#ifdef USE_SDLMIXER
#define SOUND_MENU_NITEMS 33
#else
#ifdef _WIN32
#define SOUND_MENU_NITEMS 11
#else
#define SOUND_MENU_NITEMS 10
#endif
#endif

void do_sound_menu()
{
	newmenu_item *m;
	int nitems = 0;
	char old_CMLevelMusicPath[PATH_MAX+1], old_CMMiscMusic0[PATH_MAX+1];

	memset(old_CMLevelMusicPath, 0, sizeof(char)*(PATH_MAX+1));
	snprintf(old_CMLevelMusicPath, sizeof(old_CMLevelMusicPath), "%s", GameCfg.CMLevelMusicPath);
	memset(old_CMMiscMusic0, 0, sizeof(char)*(PATH_MAX+1));
	snprintf(old_CMMiscMusic0, sizeof(old_CMMiscMusic0), "%s", GameCfg.CMMiscMusic[SONG_TITLE]);

	MALLOC(m, newmenu_item, SOUND_MENU_NITEMS);
	if (!m)
		return;

	opt_sm_digivol = nitems;
	m[nitems].type = NM_TYPE_SLIDER; m[nitems].text = TXT_FX_VOLUME; m[nitems].value = GameCfg.DigiVolume; m[nitems].min_value = 0; m[nitems++].max_value = 8;

	opt_sm_musicvol = nitems;
	m[nitems].type = NM_TYPE_SLIDER; m[nitems].text = "music volume"; m[nitems].value = GameCfg.MusicVolume; m[nitems].min_value = 0; m[nitems++].max_value = 8;

	opt_sm_revstereo = nitems;
	m[nitems].type = NM_TYPE_CHECK; m[nitems].text = TXT_REVERSE_STEREO; m[nitems++].value = GameCfg.ReverseStereo;

	m[nitems].type = NM_TYPE_TEXT; m[nitems++].text = "";

	m[nitems].type = NM_TYPE_TEXT; m[nitems++].text = "music type:";

	opt_sm_mtype0 = nitems;
	m[nitems].type = NM_TYPE_RADIO; m[nitems].text = "no music"; m[nitems].value = (GameCfg.MusicType == MUSIC_TYPE_NONE); m[nitems].group = 0; nitems++;

#if defined(USE_SDLMIXER) || defined(_WIN32)
	opt_sm_mtype1 = nitems;
	m[nitems].type = NM_TYPE_RADIO; m[nitems].text = "built-in/addon music"; m[nitems].value = (GameCfg.MusicType == MUSIC_TYPE_BUILTIN); m[nitems].group = 0; nitems++;
#endif

	opt_sm_mtype2 = nitems;
	m[nitems].type = NM_TYPE_RADIO; m[nitems].text = "cd music"; m[nitems].value = (GameCfg.MusicType == MUSIC_TYPE_REDBOOK); m[nitems].group = 0; nitems++;

#ifdef USE_SDLMIXER
	opt_sm_mtype3 = nitems;
	m[nitems].type = NM_TYPE_RADIO; m[nitems].text = "jukebox"; m[nitems].value = (GameCfg.MusicType == MUSIC_TYPE_CUSTOM); m[nitems].group = 0; nitems++;

#endif

	m[nitems].type = NM_TYPE_TEXT; m[nitems++].text = "";
#ifdef USE_SDLMIXER
	m[nitems].type = NM_TYPE_TEXT; m[nitems++].text = "cd music / jukebox options:";
#else
	m[nitems].type = NM_TYPE_TEXT; m[nitems++].text = "cd music options:";
#endif

	opt_sm_redbook_playorder = nitems;
	m[nitems].type = NM_TYPE_CHECK; m[nitems].text = "force descent ][ cd track order"; m[nitems++].value = GameCfg.OrigTrackOrder;

#ifdef USE_SDLMIXER
	m[nitems].type = NM_TYPE_TEXT; m[nitems++].text = "";

	m[nitems].type = NM_TYPE_TEXT; m[nitems++].text = "jukebox options:";

	opt_sm_mtype3_lmpath = nitems;
	m[nitems].type = PATH_HEADER_TYPE; m[nitems++].text = "path for level music" BROWSE_TXT;

	m[nitems].type = NM_TYPE_INPUT; m[nitems].text = GameCfg.CMLevelMusicPath; m[nitems++].text_len = NM_MAX_TEXT_LEN-1;

	m[nitems].type = NM_TYPE_TEXT; m[nitems++].text = "";

	m[nitems].type = NM_TYPE_TEXT; m[nitems++].text = "level music play order:";

	opt_sm_mtype3_lmplayorder1 = nitems;
	m[nitems].type = NM_TYPE_RADIO; m[nitems].text = "continuously"; m[nitems].value = (GameCfg.CMLevelMusicPlayOrder == MUSIC_CM_PLAYORDER_CONT); m[nitems].group = 1; nitems++;

	opt_sm_mtype3_lmplayorder2 = nitems;
	m[nitems].type = NM_TYPE_RADIO; m[nitems].text = "one track per level"; m[nitems].value = (GameCfg.CMLevelMusicPlayOrder == MUSIC_CM_PLAYORDER_LEVEL); m[nitems].group = 1; nitems++;

	opt_sm_mtype3_lmplayorder3 = nitems;
	m[nitems].type = NM_TYPE_RADIO; m[nitems].text = "random"; m[nitems].value = (GameCfg.CMLevelMusicPlayOrder == MUSIC_CM_PLAYORDER_RAND); m[nitems].group = 1; nitems++;

	m[nitems].type = NM_TYPE_TEXT; m[nitems++].text = "";

	m[nitems].type = NM_TYPE_TEXT; m[nitems++].text = "non-level music:";

	opt_sm_cm_mtype3_file1_b = nitems;
	m[nitems].type = PATH_HEADER_TYPE; m[nitems++].text = "main menu" BROWSE_TXT;

	opt_sm_cm_mtype3_file1 = nitems;
	m[nitems].type = NM_TYPE_INPUT; m[nitems].text = GameCfg.CMMiscMusic[SONG_TITLE]; m[nitems++].text_len = NM_MAX_TEXT_LEN-1;

	opt_sm_cm_mtype3_file2_b = nitems;
	m[nitems].type = PATH_HEADER_TYPE; m[nitems++].text = "briefing" BROWSE_TXT;

	opt_sm_cm_mtype3_file2 = nitems;
	m[nitems].type = NM_TYPE_INPUT; m[nitems].text = GameCfg.CMMiscMusic[SONG_BRIEFING]; m[nitems++].text_len = NM_MAX_TEXT_LEN-1;

	opt_sm_cm_mtype3_file3_b = nitems;
	m[nitems].type = PATH_HEADER_TYPE; m[nitems++].text = "credits" BROWSE_TXT;

	opt_sm_cm_mtype3_file3 = nitems;
	m[nitems].type = NM_TYPE_INPUT; m[nitems].text = GameCfg.CMMiscMusic[SONG_CREDITS]; m[nitems++].text_len = NM_MAX_TEXT_LEN-1;

	opt_sm_cm_mtype3_file4_b = nitems;
	m[nitems].type = PATH_HEADER_TYPE; m[nitems++].text = "escape sequence" BROWSE_TXT;

	opt_sm_cm_mtype3_file4 = nitems;
	m[nitems].type = NM_TYPE_INPUT; m[nitems].text = GameCfg.CMMiscMusic[SONG_ENDLEVEL]; m[nitems++].text_len = NM_MAX_TEXT_LEN-1;

	opt_sm_cm_mtype3_file5_b = nitems;
	m[nitems].type = PATH_HEADER_TYPE; m[nitems++].text = "game ending" BROWSE_TXT;

	opt_sm_cm_mtype3_file5 = nitems;
	m[nitems].type = NM_TYPE_INPUT; m[nitems].text = GameCfg.CMMiscMusic[SONG_ENDGAME]; m[nitems++].text_len = NM_MAX_TEXT_LEN-1;
#endif

	Assert(nitems == SOUND_MENU_NITEMS);

	newmenu_do1( NULL, "Sound Effects & Music", nitems, m, sound_menuset, NULL, 0 );

#ifdef USE_SDLMIXER
	if ( ((Game_wind != NULL) && strcmp(old_CMLevelMusicPath, GameCfg.CMLevelMusicPath)) || ((Game_wind == NULL) && strcmp(old_CMMiscMusic0, GameCfg.CMMiscMusic[SONG_TITLE])) )
	{
		songs_uninit();

		if (Game_wind)
			songs_play_level_song( Current_level_num, 0 );
		else
			songs_play_song(SONG_TITLE, 1);
	}
#endif
}

#define ADD_CHECK(n,txt,v)  do { m[n].type=NM_TYPE_CHECK; m[n].text=txt; m[n].value=v;} while (0)

int menu_misc_options_handler ( newmenu *menu, d_event *event, void *userdata );

void get_color_name(char* color_name, int color_name_length, int color_value, int use_alternate_colors)
{
	switch (color_value) {
		case 0:  snprintf(color_name, color_name_length, "%s", "Blue"); break;
		case 1:  snprintf(color_name, color_name_length, "%s", "Red"); break;
		case 2:  snprintf(color_name, color_name_length, "%s", "Green"); break;
		case 3:  snprintf(color_name, color_name_length, "%s", "Pink"); break;
		case 4:  snprintf(color_name, color_name_length, "%s", "Orange"); break;
		case 5:
			if (use_alternate_colors)
				snprintf(color_name, color_name_length, "%s", "Purple");
			else
				snprintf(color_name, color_name_length, "%s", "Tan");
			break;
		case 6:
			if (use_alternate_colors)
				snprintf(color_name, color_name_length, "%s", "White");
			else
				snprintf(color_name, color_name_length, "%s", "Mint");
			break;
		case 7:  snprintf(color_name, color_name_length, "%s", "Yellow"); break;
		default: snprintf(color_name, color_name_length, "%s", "???");
	}
}

void print_ship_color(char* color_string, int color_string_length, int color_value)
{
	char color[10];
	if (color_value == 8)
		snprintf(color, SDL_arraysize(color), "%s", "Default");
	else
		get_color_name(color, SDL_arraysize(color), color_value, 1);

	snprintf(color_string, color_string_length, "Wing Color: %s", color);
}

void print_missile_color(char* color_string, int color_string_length, int color_value)
{
	char color[11];
	if (color_value == 8)
		snprintf(color, SDL_arraysize(color), "%s", "Match Ship");
	else
		get_color_name(color, SDL_arraysize(color), color_value, 1);

	snprintf(color_string, color_string_length, "Missiles/Guns: %s", color);
}

void print_my_team_color(char* color_string, int color_string_length, int color_value)
{
	char color[10];
	if (color_value == 8)
		snprintf(color, SDL_arraysize(color), "%s", "Default");
	else
		get_color_name(color, SDL_arraysize(color), color_value, 1);

	snprintf(color_string, color_string_length, "My Team: %s", color);
}

void print_other_team_color(char* color_string, int color_string_length, int color_value)
{
	char color[10];
	if (color_value == 8)
		snprintf(color, SDL_arraysize(color), "%s", "Default");
	else
		get_color_name(color, SDL_arraysize(color), color_value, 1);

	snprintf(color_string, color_string_length, "Other Team: %s", color);
}

struct misc_menu_data {
	char preferred_color[30];
	char missile_color[30];
	char my_team_color[30];
	char other_team_color[30];
};

void do_misc_menu()
{
	newmenu_item m[47];
	int i = 0;
	struct misc_menu_data misc_menu_data;

	do {
		ADD_CHECK(0, "Ship auto-leveling", PlayerCfg.AutoLeveling);
		ADD_CHECK(1, "Missile view", PlayerCfg.MissileViewEnabled);
		ADD_CHECK(2, "Headlight on when picked up", PlayerCfg.HeadlightActiveDefault );
		ADD_CHECK(3, "Show guided missile in main display", PlayerCfg.GuidedInBigWindow );
		ADD_CHECK(4, "Escort robot hot keys",PlayerCfg.EscortHotKeys);
		ADD_CHECK(5, "Persistent Debris",PlayerCfg.PersistentDebris);
		ADD_CHECK(6, "Screenshots w/o HUD",PlayerCfg.PRShot);

		// IGT/RTA speedrun timer readout on the HUD. Defaults on; the clocks
		// keep accumulating underneath either way, this only controls
		// visibility, so toggling it off and on again does not lose the time.
		ADD_CHECK(7, "Show IGT and RTA Speed Run Timers", PlayerCfg.ShowSpeedrunTimers);

		// Velocity/rotation HUD readout. Gated on PlayerCfg.ShowVelocityMeter;
		// the values are recorded to demos regardless, this only controls
		// whether the readout is drawn.
		ADD_CHECK(8, "Show Velocity and Rotation", PlayerCfg.ShowVelocityMeter);

		// Blank spacer, so the toggles above do not visually merge into the
		// "Demo Recording Indicator:" block below it.
		m[9].type = NM_TYPE_TEXT;
		m[9].text = "";

		m[10].type = NM_TYPE_TEXT;
		m[10].text = "Demo Recording Indicator:";

		m[11].type = NM_TYPE_RADIO;
		m[11].text = "Full Text";
		m[11].group = 1;
		m[11].value = (PlayerCfg.DemoRecordingIndicator == 0);

		m[12].type = NM_TYPE_RADIO;
		m[12].text = "Recording Icon";
		m[12].group = 1;
		m[12].value = (PlayerCfg.DemoRecordingIndicator == 1);

		m[13].type = NM_TYPE_RADIO;
		m[13].text = "None";
		m[13].group = 1;
		m[13].value = (PlayerCfg.DemoRecordingIndicator == 2);

		m[14].type = NM_TYPE_TEXT;
		m[14].text = "";

		m[15].type = NM_TYPE_TEXT;
		m[15].text = "Automatically Start Demos:";

		ADD_CHECK(16, "Single-Player", PlayerCfg.AutoDemoSp);
		ADD_CHECK(17, "Multiplayer", PlayerCfg.AutoDemoMp);
		ADD_CHECK(18, "Silent (Don't Show Prompts)", PlayerCfg.AutoDemoHideUi);

		m[19].type = NM_TYPE_TEXT;
		m[19].text = "";

		ADD_CHECK(20, "Movie Subtitles",GameCfg.MovieSubtitles);
		ADD_CHECK(21, "Suppress HUD Messages (SP)",PlayerCfg.NoRedundancy);
		ADD_CHECK(22, "Show Player chat only (Multi)",PlayerCfg.MultiMessages);
		ADD_CHECK(23, "No Rankings (Multi)",PlayerCfg.NoRankings);
		// Row 24 was a spacer (D2 has no D1 BombGauge toggle); it now holds Automap XL.
		ADD_CHECK(24, "XL style controls in Automap",PlayerCfg.AutomapXL);
		ADD_CHECK(25, "Free Flight controls in Automap",PlayerCfg.AutomapFreeFlight);
		ADD_CHECK(26, "No Weapon Autoselect when firing",PlayerCfg.NoFireAutoselect);
		ADD_CHECK(27, "Autoselect after firing",PlayerCfg.SelectAfterFire);
		ADD_CHECK(28, "Only Cycle Autoselect Weapons",PlayerCfg.CycleAutoselectOnly);
		ADD_CHECK(29, "Classic No Ammo Autoselect",PlayerCfg.ClassicAutoselectWeapon);
		ADD_CHECK(30, "Autoselect only once", PlayerCfg.AutoselectOnlyOnce);
		ADD_CHECK(31, "Ammo Warnings",PlayerCfg.VulcanAmmoWarnings);
		ADD_CHECK(32, "Shield Warnings",PlayerCfg.ShieldWarnings);
		ADD_CHECK(33, "No Player Chat Sound",PlayerCfg.NoChatSound);
		ADD_CHECK(34, "Show Observer Messages",PlayerCfg.ShowObsMessages);

		m[35].type = NM_TYPE_TEXT;
		m[35].text = "";

		m[36].type = NM_TYPE_TEXT;
		m[36].text = "My Ship Colors:";

		print_ship_color(misc_menu_data.preferred_color, SDL_arraysize(misc_menu_data.preferred_color),
			PlayerCfg.ShipColor);
		m[37].type = NM_TYPE_SLIDER;
		m[37].value = PlayerCfg.ShipColor;
		m[37].text = misc_menu_data.preferred_color;
		m[37].min_value = 0;
		m[37].max_value = 8;

		print_missile_color(misc_menu_data.missile_color, SDL_arraysize(misc_menu_data.missile_color),
			PlayerCfg.MissileColor);
		m[38].type = NM_TYPE_SLIDER;
		m[38].value = PlayerCfg.MissileColor;
		m[38].text = misc_menu_data.missile_color;
		m[38].min_value = 0;
		m[38].max_value = 8;

		ADD_CHECK(39, "Show Custom Ship Colors", PlayerCfg.ShowCustomColors);

		m[40].type = NM_TYPE_TEXT;
		m[40].text = "";

		m[41].type = NM_TYPE_TEXT;
		m[41].text = "Team Colors:";

		print_my_team_color(misc_menu_data.my_team_color, SDL_arraysize(misc_menu_data.my_team_color),
			PlayerCfg.MyTeamColor);
		m[42].type = NM_TYPE_SLIDER;
		m[42].value = PlayerCfg.MyTeamColor;
		m[42].text = misc_menu_data.my_team_color;
		m[42].min_value = 0;
		m[42].max_value = 8;

		print_other_team_color(misc_menu_data.other_team_color, SDL_arraysize(misc_menu_data.other_team_color),
			PlayerCfg.OtherTeamColor);
		m[43].type = NM_TYPE_SLIDER;
		m[43].value = PlayerCfg.OtherTeamColor;
		m[43].text = misc_menu_data.other_team_color;
		m[43].min_value = 0;
		m[43].max_value = 8;

		if (PlayerCfg.MyTeamColor == 8 && PlayerCfg.OtherTeamColor == 8) {
			// If we're not setting explicit team colors, we don't override what the game host picked
			m[44].type = NM_TYPE_TEXT;
			m[44].text = "Ignore Per-Game Team Colors (N/A)";
		} else {
			m[44].type = NM_TYPE_CHECK;
			m[44].text = "Ignore Per-Game Team Colors";
		}
		m[44].value = PlayerCfg.PreferMyTeamColors;

		m[45].type = NM_TYPE_TEXT;
		m[45].text = "";

		ADD_CHECK(46, "Original D1 Weapons (SP)", PlayerCfg.OriginalD1Weapons);

		i = newmenu_do1(NULL, "Misc Options", SDL_arraysize(m), m, menu_misc_options_handler, &misc_menu_data, i);

		PlayerCfg.AutoLeveling			= m[0].value;
		PlayerCfg.MissileViewEnabled   		= m[1].value;
		PlayerCfg.HeadlightActiveDefault	= m[2].value;
		PlayerCfg.GuidedInBigWindow		= m[3].value;
		PlayerCfg.EscortHotKeys			= m[4].value;
		PlayerCfg.PersistentDebris		= m[5].value;
		PlayerCfg.PRShot 			= m[6].value;
		PlayerCfg.ShowSpeedrunTimers	= m[7].value;
		PlayerCfg.ShowVelocityMeter		= m[8].value;
		if (m[11].value) {
			PlayerCfg.DemoRecordingIndicator = 0;
		}
		else if (m[12].value) {
			PlayerCfg.DemoRecordingIndicator = 1;
		}
		else if (m[13].value) {
			PlayerCfg.DemoRecordingIndicator = 2;
		}
		PlayerCfg.AutoDemoSp			= m[16].value;
		PlayerCfg.AutoDemoMp			= m[17].value;
		PlayerCfg.AutoDemoHideUi		= m[18].value;
		GameCfg.MovieSubtitles 			= m[20].value;
		PlayerCfg.NoRedundancy 			= m[21].value;
		PlayerCfg.MultiMessages 		= m[22].value;
		PlayerCfg.NoRankings 			= m[23].value;
		PlayerCfg.AutomapXL			= m[24].value;
		PlayerCfg.AutomapFreeFlight		= m[25].value;
		PlayerCfg.NoFireAutoselect		= m[26].value;
		PlayerCfg.SelectAfterFire		= m[27].value; if(PlayerCfg.SelectAfterFire) { PlayerCfg.NoFireAutoselect = 1; }
		PlayerCfg.CycleAutoselectOnly		= m[28].value;
		PlayerCfg.ClassicAutoselectWeapon = m[29].value;
		PlayerCfg.AutoselectOnlyOnce = m[30].value;
		PlayerCfg.VulcanAmmoWarnings = m[31].value;
		PlayerCfg.ShieldWarnings = m[32].value;
		PlayerCfg.NoChatSound = m[33].value;
		PlayerCfg.ShowObsMessages = m[34].value;
		PlayerCfg.ShowCustomColors = m[39].value;
		PlayerCfg.PreferMyTeamColors = (PlayerCfg.MyTeamColor == 8 && PlayerCfg.OtherTeamColor == 8) ? 0 : m[44].value;
		PlayerCfg.OriginalD1Weapons = m[46].value;

	} while( i>-1 );

	// Update team colors if they were changed during a game
	if ((Game_mode & GM_TEAM) && (Network_status == NETSTAT_PLAYING)) {
		for (int i = 0; i < N_players; i++)
			if (Players[i].connected)
				multi_reset_object_texture(&Objects[Players[i].objnum]);
	}
}

int menu_misc_options_handler(newmenu* menu, d_event* event, void* userdata)
{
	newmenu_item *menus = newmenu_get_items(menu);
	int citem = newmenu_get_citem(menu);
	struct misc_menu_data* menu_data = (struct misc_menu_data*)userdata;
	
	if (event->type == EVENT_NEWMENU_CHANGED) {
		if (citem == 37) {
			PlayerCfg.ShipColor = menus[citem].value;
			print_ship_color(menu_data->preferred_color, SDL_arraysize(menu_data->preferred_color),
				PlayerCfg.ShipColor);
		} else if (citem == 38) {
			PlayerCfg.MissileColor = menus[citem].value;
			print_missile_color(menu_data->missile_color, SDL_arraysize(menu_data->missile_color),
				PlayerCfg.MissileColor);
		} else if (citem == 42) {
			PlayerCfg.MyTeamColor = menus[citem].value;
			print_my_team_color(menu_data->my_team_color, SDL_arraysize(menu_data->my_team_color),
				PlayerCfg.MyTeamColor);
		} else if (citem == 43) {
			PlayerCfg.OtherTeamColor = menus[citem].value;
			print_other_team_color(menu_data->other_team_color, SDL_arraysize(menu_data->other_team_color),
				PlayerCfg.OtherTeamColor);
		}

		if (PlayerCfg.MyTeamColor == 8 && PlayerCfg.OtherTeamColor == 8) {
			menus[44].type = NM_TYPE_TEXT;
			menus[44].text = "Ignore Per-Game Team Colors (N/A)";
		} else {
			menus[44].type = NM_TYPE_CHECK;
			menus[44].text = "Ignore Per-Game Team Colors";
		}
	}

	return 0;
}

int menu_obs_options_handler ( newmenu *menu, d_event *event, void *userdata );
int select_obs_game_mode_handler(listbox* lb, d_event* event, void* userdata);

const char* Obs_mode_names[] = { "1v1", "FFA", "Teams", "Coop" };
struct obs_menu_data
{
	newmenu* menu;
	int mode;
};
#define OBS_MENU_MODE_SWITCHED -2
#define OBS_MENU_OVERWRITE_MODES -3

void do_obs_menu()
{
	newmenu_item m[33];
	struct obs_menu_data obs_menu_data;
	obs_menu_data.mode = is_observer() ? get_observer_game_mode() : 0;
	int i = 0;

	do {
		int cmode = obs_menu_data.mode;

		char mode_text[40];
		snprintf(mode_text, SDL_arraysize(mode_text), "Currently configuring: %s",
			PlayerCfg.ObsShareSettings ? "All" : Obs_mode_names[cmode]);
		m[0].type = NM_TYPE_TEXT;
		m[0].text = mode_text;

		if (PlayerCfg.ObsShareSettings) {
			m[1].type = NM_TYPE_TEXT;
			m[1].text = "Switch Game Mode (N/A)";
		} else {
			m[1].type = NM_TYPE_MENU;
			m[1].text = "Switch Game Mode";
		}

		ADD_CHECK(2, "Share Settings Across Modes", PlayerCfg.ObsShareSettings);

		m[3].type = NM_TYPE_TEXT;
		m[3].text = "";

		ADD_CHECK(4, "Fly Fast", PlayerCfg.ObsTurbo[cmode]);
		ADD_CHECK(5, "Show Cockpit in First Person", PlayerCfg.ObsShowCockpit[cmode]);

		m[6].type = NM_TYPE_TEXT;
		m[6].text = "";

		m[7].type = NM_TYPE_TEXT;
		m[7].text = "Show on Scoreboard:";

		ADD_CHECK(8, "Shield Text", PlayerCfg.ObsShowScoreboardShieldText[cmode]);
		ADD_CHECK(9, "Shield Bar", PlayerCfg.ObsShowScoreboardShieldBar[cmode]);
		ADD_CHECK(10, "Energy and Ammo Bars", PlayerCfg.ObsShowAmmoBars[cmode]);
		ADD_CHECK(11, "Primary Weapon", PlayerCfg.ObsShowPrimary[cmode]);
		ADD_CHECK(12, "Secondary Weapon", PlayerCfg.ObsShowSecondary[cmode]);

		m[13].type = NM_TYPE_TEXT;
		m[13].text = "";

		m[14].type = NM_TYPE_TEXT;
		m[14].text = "Show on Player:";

		ADD_CHECK(15, "Player Names", PlayerCfg.ObsShowNames[cmode]);
		ADD_CHECK(16, "Damage Text", PlayerCfg.ObsShowDamage[cmode]);
		ADD_CHECK(17, "Shield Text", PlayerCfg.ObsShowShieldText[cmode]);
		ADD_CHECK(18, "Shield Bar", PlayerCfg.ObsShowShieldBar[cmode]);

		m[19].type = NM_TYPE_TEXT;
		m[19].text = "";

		m[20].type = NM_TYPE_TEXT;
		m[20].text = "Show Information:";

		ADD_CHECK(21, "Kill Feed", PlayerCfg.ObsShowKillFeed[cmode]);
		ADD_CHECK(22, "Death Summary", PlayerCfg.ObsShowDeathSummary[cmode]);
		ADD_CHECK(23, "Streaks and Runs", PlayerCfg.ObsShowStreaks[cmode]);
		ADD_CHECK(24, "Kills over Time Graph", PlayerCfg.ObsShowKillGraph[cmode]);
		ADD_CHECK(25, "Damage Breakdown", PlayerCfg.ObsShowBreakdown[cmode]);
		ADD_CHECK(26, "List of Observers", PlayerCfg.ObsShowObs[cmode]);
		ADD_CHECK(27, "Observer Chat", PlayerCfg.ObsChat[cmode]);
		ADD_CHECK(28, "Player Chat", PlayerCfg.ObsPlayerChat[cmode]);
		ADD_CHECK(29, "Bomb/Mine Countdowns", PlayerCfg.ObsShowBombTimes[cmode]);
		ADD_CHECK(30, "Transparent third person ship", PlayerCfg.ObsTransparentThirdPerson[cmode]);
		ADD_CHECK(31, "Increase third person distance", PlayerCfg.ObsIncreaseThirdPersonDist[cmode]);
		ADD_CHECK(32, "Hide energy weapon muzzle", PlayerCfg.ObsHideEnergyWeaponMuzzle[cmode]);

		i = newmenu_do1(NULL, "JinX Mode Options", SDL_arraysize(m), m, menu_obs_options_handler, &obs_menu_data, i);

		PlayerCfg.ObsShareSettings = m[2].value;
		// Note: obs_menu_data.mode may have changed; we kept a copy in cmode which we use here
		PlayerCfg.ObsTurbo[cmode] = m[4].value;
		PlayerCfg.ObsShowCockpit[cmode] = m[5].value;
		PlayerCfg.ObsShowScoreboardShieldText[cmode] = m[8].value;
		PlayerCfg.ObsShowScoreboardShieldBar[cmode] = m[9].value;
		PlayerCfg.ObsShowAmmoBars[cmode] = m[10].value;
		PlayerCfg.ObsShowPrimary[cmode] = m[11].value;
		PlayerCfg.ObsShowSecondary[cmode] = m[12].value;
		PlayerCfg.ObsShowNames[cmode] = m[15].value;
		PlayerCfg.ObsShowDamage[cmode] = m[16].value;
		PlayerCfg.ObsShowShieldText[cmode] = m[17].value;
		PlayerCfg.ObsShowShieldBar[cmode] = m[18].value;
		PlayerCfg.ObsShowKillFeed[cmode] = m[21].value;
		PlayerCfg.ObsShowDeathSummary[cmode] = m[22].value;
		PlayerCfg.ObsShowStreaks[cmode] = m[23].value;
		PlayerCfg.ObsShowKillGraph[cmode] = m[24].value;
		PlayerCfg.ObsShowBreakdown[cmode] = m[25].value;
		PlayerCfg.ObsShowObs[cmode] = m[26].value;
		PlayerCfg.ObsChat[cmode] = m[27].value;
		PlayerCfg.ObsPlayerChat[cmode] = m[28].value;
		PlayerCfg.ObsShowBombTimes[cmode] = m[29].value;
		PlayerCfg.ObsTransparentThirdPerson[cmode] = m[30].value;
		PlayerCfg.ObsIncreaseThirdPersonDist[cmode] = m[31].value;
		PlayerCfg.ObsHideEnergyWeaponMuzzle[cmode] = m[32].value;

		// Only update other modes *after* we update the current one; user may have changed something
		if (i == OBS_MENU_OVERWRITE_MODES || PlayerCfg.ObsShareSettings)
		{
			for (int i = 0; i < SDL_arraysize(Obs_mode_names); i++)
			{
				if (i == cmode)
					continue;

				PlayerCfg.ObsTurbo[i] = PlayerCfg.ObsTurbo[cmode];
				PlayerCfg.ObsShowCockpit[i] = PlayerCfg.ObsShowCockpit[cmode];
				PlayerCfg.ObsShowScoreboardShieldText[i] = PlayerCfg.ObsShowScoreboardShieldText[cmode];
				PlayerCfg.ObsShowScoreboardShieldBar[i] = PlayerCfg.ObsShowScoreboardShieldBar[cmode];
				PlayerCfg.ObsShowAmmoBars[i] = PlayerCfg.ObsShowAmmoBars[cmode];
				PlayerCfg.ObsShowPrimary[i] = PlayerCfg.ObsShowPrimary[cmode];
				PlayerCfg.ObsShowSecondary[i] = PlayerCfg.ObsShowSecondary[cmode];
				PlayerCfg.ObsShowNames[i] = PlayerCfg.ObsShowNames[cmode];
				PlayerCfg.ObsShowDamage[i] = PlayerCfg.ObsShowDamage[cmode];
				PlayerCfg.ObsShowShieldText[i] = PlayerCfg.ObsShowShieldText[cmode];
				PlayerCfg.ObsShowShieldBar[i] = PlayerCfg.ObsShowShieldBar[cmode];
				PlayerCfg.ObsShowKillFeed[i] = PlayerCfg.ObsShowKillFeed[cmode];
				PlayerCfg.ObsShowDeathSummary[i] = PlayerCfg.ObsShowDeathSummary[cmode];
				PlayerCfg.ObsShowStreaks[i] = PlayerCfg.ObsShowStreaks[cmode];
				PlayerCfg.ObsShowKillGraph[i] = PlayerCfg.ObsShowKillGraph[cmode];
				PlayerCfg.ObsShowBreakdown[i] = PlayerCfg.ObsShowBreakdown[cmode];
				PlayerCfg.ObsShowObs[i] = PlayerCfg.ObsShowObs[cmode];
				PlayerCfg.ObsChat[i] = PlayerCfg.ObsChat[cmode];
				PlayerCfg.ObsPlayerChat[i] = PlayerCfg.ObsPlayerChat[cmode];
				PlayerCfg.ObsShowBombTimes[i] = PlayerCfg.ObsShowBombTimes[cmode];
				PlayerCfg.ObsTransparentThirdPerson[i] = PlayerCfg.ObsTransparentThirdPerson[cmode];
				PlayerCfg.ObsIncreaseThirdPersonDist[i] = PlayerCfg.ObsIncreaseThirdPersonDist[cmode];
				PlayerCfg.ObsHideEnergyWeaponMuzzle[i] = PlayerCfg.ObsHideEnergyWeaponMuzzle[cmode];
			}
		}
	} while (i > -1 || i == OBS_MENU_MODE_SWITCHED || i == OBS_MENU_OVERWRITE_MODES);
}

int menu_obs_options_handler ( newmenu *menu, d_event *event, void *userdata )
{
	int citem = newmenu_get_citem(menu);
	struct obs_menu_data* obs_menu_data = (struct obs_menu_data*)userdata;
	obs_menu_data->menu = menu;
	int cmode = obs_menu_data->mode;

	switch (event->type)
	{
		case EVENT_NEWMENU_CHANGED:
			if (citem == 2) {
				newmenu_item* item = &newmenu_get_items(obs_menu_data->menu)[citem];
				if (item->value == 1) {
					// The user enabled sharing settings. Double-check that this is what they want
					int overwrite = nm_messagebox(NULL, 2, "No", "Yes",
						"Are you sure you want to\nreplace settings for\n"
						"all game modes with the\ncurrent settings?");
					if (overwrite == 1) {
						newmenu_set_rval(obs_menu_data->menu, OBS_MENU_OVERWRITE_MODES);
						window_close(newmenu_get_window(obs_menu_data->menu));
						return 1;
					} else {
						// No? Then we switch the checkbox back off again
						item->value = 0;
					}
				} else {
					// We actually need to run an overwrite here too; if the user changed other
					// settings before flipping this checkbox, it's going to be confusing if only
					// the first game mode gets those changes.
					newmenu_set_rval(obs_menu_data->menu, OBS_MENU_OVERWRITE_MODES);
					window_close(newmenu_get_window(obs_menu_data->menu));
					return 1;
				}
			}
			break;

		case EVENT_NEWMENU_SELECTED:
			if (citem == 1) {
				newmenu_listbox1("Select Game Mode", SDL_arraysize(Obs_mode_names), (char **)Obs_mode_names,
					1, cmode, select_obs_game_mode_handler, obs_menu_data);
				return 1;
			}
			break;

		default:
			break;
	}

	return 0;
}

int select_obs_game_mode_handler(listbox* lb, d_event* event, void* userdata)
{
	int citem = listbox_get_citem(lb);
	struct obs_menu_data* obs_menu_data = (struct obs_menu_data*)userdata;

	switch (event->type)
	{
		case EVENT_NEWMENU_SELECTED:
			obs_menu_data->mode = citem;
			newmenu_set_rval(obs_menu_data->menu, OBS_MENU_MODE_SWITCHED);
			window_close(newmenu_get_window(obs_menu_data->menu));
			break;

		default:
			break;
	}

	return 0;
}

#if defined(USE_UDP)
static int multi_player_menu_handler(newmenu *menu, d_event *event, int *menu_choice)
{
	newmenu_item *items = newmenu_get_items(menu);

	switch (event->type)
	{
		case EVENT_NEWMENU_SELECTED:
			// stay in multiplayer menu, even after having played a game
			return do_option(menu_choice[newmenu_get_citem(menu)]);

		case EVENT_WINDOW_CLOSE:
			d_free(menu_choice);
			d_free(items);
			break;

		default:
			break;
	}

	return 0;
}

void do_multi_player_menu()
{
	int *menu_choice;
	newmenu_item *m;
	int num_options = 0;

	MALLOC(menu_choice, int, 3);
	if (!menu_choice)
		return;

	MALLOC(m, newmenu_item, 3);
	if (!m)
	{
		d_free(menu_choice);
		return;
	}

#ifdef USE_UDP
	m[num_options].type=NM_TYPE_MENU; m[num_options].text="HOST GAME"; menu_choice[num_options]=MENU_START_UDP_NETGAME; num_options++;
#ifdef USE_TRACKER
	m[num_options].type=NM_TYPE_MENU; m[num_options].text="FIND LAN/ONLINE GAMES"; menu_choice[num_options]=MENU_JOIN_LIST_UDP_NETGAME; num_options++;
#else
	m[num_options].type=NM_TYPE_MENU; m[num_options].text="FIND LAN GAMES"; menu_choice[num_options]=MENU_JOIN_LIST_UDP_NETGAME; num_options++;
#endif
	m[num_options].type=NM_TYPE_MENU; m[num_options].text="JOIN GAME MANUALLY"; menu_choice[num_options]=MENU_JOIN_MANUAL_UDP_NETGAME; num_options++;
#endif

	newmenu_do3( NULL, TXT_MULTIPLAYER, num_options, m, (int (*)(newmenu *, d_event *, void *))multi_player_menu_handler, menu_choice, 0, NULL );
}
#endif

void do_options_menu()
{
	newmenu_item *m;

	MALLOC(m, newmenu_item, 11);
	if (!m)
		return;

	m[ 0].type = NM_TYPE_MENU;   m[ 0].text="Sound effects & music...";
	m[ 1].type = NM_TYPE_TEXT;   m[ 1].text="";
	m[ 2].type = NM_TYPE_MENU;   m[ 2].text=TXT_CONTROLS_;
	m[ 3].type = NM_TYPE_TEXT;   m[ 3].text="";
	m[ 4].type = NM_TYPE_MENU;   m[ 4].text="Screen resolution...";
	m[ 5].type = NM_TYPE_MENU;   m[ 5].text="Graphics Options...";
	m[ 6].type = NM_TYPE_TEXT;   m[ 6].text="";
	m[ 7].type = NM_TYPE_MENU;   m[ 7].text="Primary autoselect ordering...";
	m[ 8].type = NM_TYPE_MENU;   m[ 8].text="Secondary autoselect ordering...";
	m[ 9].type = NM_TYPE_MENU;   m[ 9].text="Misc Options...";
	m[10].type = NM_TYPE_MENU;   m[10].text="Observer Options...";

	// Fall back to main event loop
	// Allows clean closing and re-opening when resolution changes
	newmenu_do3( NULL, TXT_OPTIONS, 11, m, options_menuset, NULL, 0, NULL );
}

#ifndef RELEASE
int polygon_models_viewer_handler(window *wind, d_event *event)
{
	static int view_idx = 0;
	int key = 0;
	static vms_angvec ang;

	switch (event->type)
	{
		case EVENT_WINDOW_ACTIVATED:
			gr_use_palette_table("groupa.256");
			key_toggle_repeat(1);
			view_idx = 0;
			ang.p = ang.b = 0;
			ang.h = F0_5-1;
			break;
		case EVENT_KEY_COMMAND:
			key = event_key_get(event);
			switch (key)
			{
				case KEY_ESC:
					window_close(wind);
					break;
				case KEY_SPACEBAR:
					view_idx ++;
					if (view_idx >= N_polygon_models) view_idx = 0;
					break;
				case KEY_BACKSP:
					view_idx --;
					if (view_idx < 0 ) view_idx = N_polygon_models - 1;
					break;
				case KEY_A:
					ang.h -= 100;
					break;
				case KEY_D:
					ang.h += 100;
					break;
				case KEY_W:
					ang.p -= 100;
					break;
				case KEY_S:
					ang.p += 100;
					break;
				case KEY_Q:
					ang.b -= 100;
					break;
				case KEY_E:
					ang.b += 100;
					break;
				case KEY_R:
					ang.p = ang.b = 0;
					ang.h = F0_5-1;
					break;
				default:
					break;
			}
			return 1;
		case EVENT_WINDOW_DRAW:
			timer_delay(F1_0/60);
			draw_model_picture(view_idx, &ang);
			gr_set_curfont(GAME_FONT);
			gr_set_fontcolor(BM_XRGB(255,255,255), -1);
			gr_printf(FSPACX(1), FSPACY(1), "ESC: leave\nSPACE/BACKSP: next/prev model (%i/%i)\nA/D: rotate y\nW/S: rotate x\nQ/E: rotate z\nR: reset orientation",view_idx,N_polygon_models-1);
			break;
		case EVENT_WINDOW_CLOSE:
			load_palette(MENU_PALETTE,0,1);
			key_toggle_repeat(0);
			break;
		default:
			break;
	}
	
	return 0;
}

void polygon_models_viewer()
{
	window *wind = window_create(&grd_curscreen->sc_canvas, 0, 0, SWIDTH, SHEIGHT, (int (*)(window *, d_event *, void *))polygon_models_viewer_handler, NULL);
	if (!wind)
	{
		d_event event = { EVENT_WINDOW_CLOSE };
		polygon_models_viewer_handler(NULL, &event);
		return;
	}

	while (window_exists(wind))
		event_process();
}

int gamebitmaps_viewer_handler(window *wind, d_event *event)
{
	static int view_idx = 0;
	int key = 0;
#ifdef OGL
	float scale = 1.0;
#endif
	bitmap_index bi;
	grs_bitmap *bm;
	extern int Num_bitmap_files;

	switch (event->type)
	{
		case EVENT_WINDOW_ACTIVATED:
			gr_use_palette_table("groupa.256");
			key_toggle_repeat(1);
			view_idx = 0;
			break;
		case EVENT_KEY_COMMAND:
			key = event_key_get(event);
			switch (key)
			{
				case KEY_ESC:
					window_close(wind);
					break;
				case KEY_SPACEBAR:
					view_idx ++;
					if (view_idx >= Num_bitmap_files) view_idx = 0;
					break;
				case KEY_BACKSP:
					view_idx --;
					if (view_idx < 0 ) view_idx = Num_bitmap_files - 1;
					break;
				default:
					break;
			}
			return 1;
		case EVENT_WINDOW_DRAW:
			bi.index = view_idx;
			bm = &GameBitmaps[view_idx];
			timer_delay(F1_0/60);
			PIGGY_PAGE_IN(bi);
			gr_clear_canvas( BM_XRGB(0,0,0) );
#ifdef OGL
			scale = (bm->bm_w > bm->bm_h)?(SHEIGHT/bm->bm_w)*0.8:(SHEIGHT/bm->bm_h)*0.8;
			ogl_ubitmapm_cs((SWIDTH/2)-(bm->bm_w*scale/2),(SHEIGHT/2)-(bm->bm_h*scale/2),bm->bm_w*scale,bm->bm_h*scale,bm,-1,F1_0);
#else
			gr_bitmap((SWIDTH/2)-(bm->bm_w/2), (SHEIGHT/2)-(bm->bm_h/2), bm);
#endif
			gr_set_curfont(GAME_FONT);
			gr_set_fontcolor(BM_XRGB(255,255,255), -1);
			gr_printf(FSPACX(1), FSPACY(1), "ESC: leave\nSPACE/BACKSP: next/prev bitmap (%i/%i)",view_idx,Num_bitmap_files-1);
			break;
		case EVENT_WINDOW_CLOSE:
			load_palette(MENU_PALETTE,0,1);
			key_toggle_repeat(0);
			break;
		default:
			break;
	}
	
	return 0;
}

void gamebitmaps_viewer()
{
	window *wind = window_create(&grd_curscreen->sc_canvas, 0, 0, SWIDTH, SHEIGHT, (int (*)(window *, d_event *, void *))gamebitmaps_viewer_handler, NULL);
	if (!wind)
	{
		d_event event = { EVENT_WINDOW_CLOSE };
		gamebitmaps_viewer_handler(NULL, &event);
		return;
	}

	while (window_exists(wind))
		event_process();
}

int sandbox_menuset(newmenu *menu, d_event *event, void *userdata)
{
	switch (event->type)
	{
		case EVENT_NEWMENU_CHANGED:
			break;

		case EVENT_NEWMENU_SELECTED:
			switch(newmenu_get_citem(menu))
			{
				case  0: polygon_models_viewer(); break;
				case  1: gamebitmaps_viewer(); break;
			}
			return 1; // stay in menu until escape
			break;

		case EVENT_WINDOW_CLOSE:
		{
			newmenu_item *items = newmenu_get_items(menu);
			d_free(items);
			break;
		}

		default:
			break;
	}

	userdata = userdata; //kill warning

	return 0;
}

void do_sandbox_menu()
{
	newmenu_item *m;

	MALLOC(m, newmenu_item, 2);
	if (!m)
		return;

	m[ 0].type = NM_TYPE_MENU;   m[ 0].text="Polygon_models viewer";
	m[ 1].type = NM_TYPE_MENU;   m[ 1].text="GameBitmaps viewer";

	newmenu_do3( NULL, "Coder's sandbox", 2, m, sandbox_menuset, NULL, 0, NULL );
}
#endif
