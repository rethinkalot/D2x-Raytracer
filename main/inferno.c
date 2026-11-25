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
 * inferno.c: Entry point of program (main procedure)
 *
 * After main initializes everything, most of the time is spent in the loop
 * while (window_get_front())
 * In this loop, the main menu is brought up first.
 *
 * main() for Inferno
 *
 */

char copyright[] = "DESCENT II  COPYRIGHT (C) 1994-1996 PARALLAX SOFTWARE CORPORATION";

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <SDL/SDL.h>

#ifdef __unix__
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#endif

#include "pstypes.h"
#include "strutil.h"
#include "console.h"
#include "gr.h"
#include "key.h"
#include "3d.h"
#include "bm.h"
#include "inferno.h"
#include "dxxerror.h"
#include "game.h"
#include "gameseg.h"
#include "segment.h"		//for Side_to_verts
#include "u_mem.h"
#include "screens.h"
#include "texmap.h"
#include "texmerge.h"
#include "menu.h"
#include "digi.h"
#include "palette.h"
#include "args.h"
#include "titles.h"
#include "text.h"
#include "gauges.h"
#include "gamefont.h"
#include "kconfig.h"
#include "newmenu.h"
#include "config.h"
#include "multi.h"
#include "songs.h"
#include "gameseq.h"
#include "gamepal.h"
#include "movie.h"
#include "playsave.h"
#include "collide.h"
#include "newdemo.h"
#include "mission.h"
#include "joy.h"
#include "../texmap/scanline.h" //for select_tmap -MM
#include "event.h"
#include "rbaudio.h"
#ifndef __LINUX__
#include "messagebox.h"
#endif
#ifdef EDITOR
#include "editor/editor.h"
#include "editor/kdefs.h"
#include "ui.h"
#endif
#include "vers_id.h"
#ifdef USE_UDP
#include "net_udp.h"
#endif

#if defined(RT_DX12)
#include "Core/Arena.h"
#include "RTgr.h"
#include "piggy.h"
#include "textures.h"
#include "robot.h"
#include "polyobj.h"
#endif

//Current version number

int Screen_mode=-1;					//game screen or editor screen?
int descent_critical_error = 0;
unsigned int descent_critical_deverror = 0;
unsigned int descent_critical_errcode = 0;

#ifdef	EDITOR
char	Auto_file[128] = "";
#endif

extern void piggy_init_pigfile(char *filename);
extern void arch_init(void);

//read help from a file & print to screen
void print_commandline_help()
{
	printf( "\n System Options:\n\n");
	printf( "  -nonicefps                    Don't free CPU-cycles\n");
	printf( "  -maxfps <n>                   Set maximum framerate to <n>\n\t\t\t\t(default: %i, availble: 1-%i)\n", MAXIMUM_FPS, MAXIMUM_FPS);
	printf( "  -hogdir <s>                   set shared data directory to <s>\n");
	printf( "  -nohogdir                     don't try to use shared data directory\n");
	printf( "  -use_players_dir              put player files and saved games in Players subdirectory\n");
	printf( "  -lowmem                       Lowers animation detail for better performance with\n\t\t\t\tlow memory\n");
	printf( "  -pilot <s>                    Select pilot <s> automatically\n");
	printf( "  -autodemo                     Start in demo mode\n");
	printf( "  -window                       Run the game in a window\n");
	printf( "  -noborders                    Do not show borders in window mode\n");
	printf( "  -nomovies                     Don't play movies\n");

	printf( "\n Controls:\n\n");
	printf( "  -nocursor                     Hide mouse cursor\n");
	printf( "  -nomouse                      Deactivate mouse\n");
	printf( "  -nojoystick                   Deactivate joystick\n");
	printf( "  -nostickykeys                 Make CapsLock and NumLock non-sticky\n");

	printf( "\n Sound:\n\n");
	printf( "  -nosound                      Disables sound output\n");
	printf( "  -nomusic                      Disables music output\n");
	printf( "  -sound11k                     Use 11KHz sounds\n");
#ifdef    USE_SDLMIXER
	printf( "  -nosdlmixer                   Disable Sound output via SDL_mixer\n");
#endif // USE SDLMIXER

	printf( "\n Graphics:\n\n");
	printf( "  -lowresfont                   Force to use LowRes fonts\n");
	printf( "  -lowresgraphics               Force to use LowRes graphics\n");
	printf( "  -lowresmovies                 Play low resolution movies if available (for slow machines)\n");
#ifdef    OGL
	printf( "  -gl_fixedfont                 Do not scale fonts to current resolution\n");
#endif // OGL

#if defined(USE_UDP)
	printf( "\n Multiplayer:\n\n");
	printf( "  -udp_hostaddr <s>             Use IP address/Hostname <s> for manual game joining\n\t\t\t\t(default: %s)\n", UDP_MANUAL_ADDR_DEFAULT);
	printf( "  -udp_hostport <n>             Use UDP port <n> for manual game joining (default: %i)\n", UDP_PORT_DEFAULT);
	printf( "  -udp_myport <n>               Set my own UDP port to <n> (default: %i)\n", UDP_PORT_DEFAULT);
#ifdef USE_TRACKER
	printf( "  -tracker_hostaddr <n>         Address of Tracker server to register/query games to/from\n\t\t\t\t(default: %s)\n", TRACKER_ADDR_DEFAULT);
	printf( "  -tracker_hostport <n>         Port of Tracker server to register/query games to/from\n\t\t\t\t(default: %i)\n", TRACKER_PORT_DEFAULT);
#endif // USE_TRACKER
#endif // defined(USE_UDP)

#ifdef    EDITOR
	printf( "\n Editor:\n\n");
	printf( "  -autoload <s>                 Autoload level <s> in the editor\n");
	printf( "  -macdata                      Read and write mac data files in editor (swap colors)\n");
	printf( "  -hoarddata                    Make the hoard ham file from some files, then exit\n");
#endif // EDITOR

	printf( "\n Debug (use only if you know what you're doing):\n\n");
	printf( "  -debug                        Enable debugging output.\n");
	printf( "  -verbose                      Enable verbose output.\n");
	printf( "  -safelog                      Write gamelog.txt unbuffered.\n\t\t\t\tUse to keep helpful output to trace program crashes.\n");
	printf( "  -norun                        Bail out after initialization\n");
	printf( "  -renderstats                  Enable renderstats info by default\n");
	printf( "  -text <s>                     Specify alternate .tex file\n");
	printf( "  -tmap <s>                     Select texmapper <s> to use\n\t\t\t\t(default: c, available: c, fp, quad, i386)\n");
	printf( "  -showmeminfo                  Show memory statistics\n");
	printf( "  -nodoublebuffer               Disable Doublebuffering\n");
	printf( "  -bigpig                       Use uncompressed RLE bitmaps\n");
	printf( "  -16bpp                        Use 16Bpp instead of 32Bpp\n");
#ifdef    OGL
	printf( "  -gl_oldtexmerge               Use old texmerge, uses more ram, but might be faster\n");
	printf( "  -gl_intensity4_ok <n>         Override DbgGlIntensity4Ok (default: 1)\n");
	printf( "  -gl_luminance4_alpha4_ok <n>  Override DbgGlLuminance4Alpha4Ok (default: 1)\n");
	printf( "  -gl_rgba2_ok <n>              Override DbgGlRGBA2Ok (default: 1)\n");
	printf( "  -gl_readpixels_ok <n>         Override DbgGlReadPixelsOk (default: 1)\n");
	printf( "  -gl_gettexlevelparam_ok <n>   Override DbgGlGetTexLevelParamOk (default: 1)\n");
#else
	printf( "  -hwsurface                    Use SDL HW Surface\n");
	printf( "  -asyncblit                    Use queued blits over SDL. Can speed up rendering\n");
#endif // OGL

	printf( "\n Help:\n\n");
	printf( "  -help, -h, -?, ?             View this help screen\n");
	printf( "\n\n");
}

int Quitting = 0;

// Default event handler for everything except the editor
int standard_handler(d_event *event)
{
	int key;

	if (Quitting)
	{
		window *wind = window_get_front();
		if (!wind)
			return 0;
	
		if (wind == Game_wind)
		{
			int choice;
			Quitting = 0;
			choice=nm_messagebox( NULL, 2, TXT_YES, TXT_NO, TXT_ABORT_GAME );
			if (choice != 0)
				return 0;
			else
			{
				GameArg.SysAutoDemo = 0;
				Quitting = 1;
			}
		}
		
		// Close front window, let the code flow continue until all windows closed or quit cancelled
		if (!window_close(wind))
			Quitting = 0;
		
		return 1;
	}

	switch (event->type)
	{
		case EVENT_MOUSE_BUTTON_DOWN:
		case EVENT_MOUSE_BUTTON_UP:
			// No window selecting
			// We stay with the current one until it's closed/hidden or another one is made
			// Not the case for the editor
			break;

		case EVENT_KEY_COMMAND:
			key = event_key_get(event);

			switch (key)
			{
#ifdef macintosh
				case KEY_COMMAND + KEY_SHIFTED + KEY_3:
#endif
				case KEY_PRINT_SCREEN:
				{
					gr_set_current_canvas(NULL);
					save_screen_shot(0);
					return 1;
				}

				case KEY_ALTED+KEY_ENTER:
				case KEY_ALTED+KEY_PADENTER:
					if (Game_wind)
						if (Game_wind == window_get_front())
							return 0;
					gr_toggle_fullscreen();
					return 1;

#ifndef NDEBUG
				case KEY_BACKSP:
					Int3();
					return 1;
#endif

#if defined(__APPLE__) || defined(macintosh)
				case KEY_COMMAND+KEY_Q:
					// Alt-F4 already taken, too bad
					Quitting = 1;
					return 1;
#endif
				case KEY_SHIFTED + KEY_ESC:
					con_showup();
					return 1;
			}
			break;

		case EVENT_WINDOW_DRAW:
		case EVENT_IDLE:
			//see if redbook song needs to be restarted
			RBACheckFinishedHook();
			return 1;

		case EVENT_QUIT:
#ifdef EDITOR
			if (SafetyCheck())
#endif
				Quitting = 1;
			return 1;

		default:
			break;
	}

	return 0;
}

jmp_buf LeaveEvents;
#define PROGNAME argv[0]

//	DESCENT II by Parallax Software
//		Descent Main

int main(int argc, char *argv[])
{
	mem_init();
#ifdef __LINUX__
	error_init(NULL);
#else
	error_init(msgbox_error);
	set_warn_func(msgbox_warning);
#endif
	PHYSFSX_init(argc, argv);
	//RT_LOG_INIT(RT_FILTERFLAG_ALL);
	con_init();  // Initialise the console

	setbuf(stdout, NULL); // unbuffered output via printf
#ifndef SHIPPING_BUILD
#ifdef _WIN32
	freopen( "CON", "w", stdout );
	freopen( "CON", "w", stderr );
#endif
#endif //SHIPPING_BUILD
	if (GameArg.SysShowCmdHelp) {
		print_commandline_help();

		return(0);
	}

	printf("\nType %s -help' for a list of command-line options.\n\n", PROGNAME);

	PHYSFSX_listSearchPathContent();
	
	if (!PHYSFSX_checkSupportedArchiveTypes())
		return(0);

	if (! PHYSFSX_contfile_init("descent2.hog", 1)) {
		if (! PHYSFSX_contfile_init("d2demo.hog", 1))
#define DXX_NAME_NUMBER	"2"
#define DXX_HOGFILE_NAMES	"descent2.hog or d2demo.hog"
#if defined(__unix__) && !defined(__APPLE__)
#define DXX_HOGFILE_PROGRAM_DATA_DIRECTORY	\
			      "\t$HOME/.d" DXX_NAME_NUMBER "x-raytracer\n"	\
			      "\t" SHAREPATH "\n"
#else
#define DXX_HOGFILE_PROGRAM_DATA_DIRECTORY	\
				  "\tDirectory containing D" DXX_NAME_NUMBER "X\n"
#endif
#if (defined(__APPLE__) && defined(__MACH__)) || defined(macintosh)
#define DXX_HOGFILE_APPLICATION_BUNDLE	\
				  "\tIn 'Resources' inside the application bundle\n"
#else
#define DXX_HOGFILE_APPLICATION_BUNDLE	""
#endif
#define DXX_MISSING_HOGFILE_ERROR_TEXT	\
		"Could not find a valid hog file (" DXX_HOGFILE_NAMES ")\nPossible locations are:\n"	\
		DXX_HOGFILE_PROGRAM_DATA_DIRECTORY	\
		"\tIn a subdirectory called 'Data'\n"	\
		DXX_HOGFILE_APPLICATION_BUNDLE	\
		"Or use the -hogdir option to specify an alternate location."
		Error(DXX_MISSING_HOGFILE_ERROR_TEXT);
	}

	load_text();

	//print out the banner title
	con_printf(CON_NORMAL, "%s%s  %s\n", DESCENT_VERSION, PHYSFSX_exists(MISSION_DIR "d2x.hog",1) ? "  Vertigo Enhanced" : "", g_descent_build_datetime); // D2X version
	con_printf(CON_NORMAL, "This is a MODIFIED version of Descent 2, based on %s.\n", BASED_VERSION);
	con_printf(CON_NORMAL, "%s\n%s\n",TXT_COPYRIGHT,TXT_TRADEMARK);
	con_printf(CON_NORMAL, "Copyright (C) 1999 Peter Hawkins, 2002 Bradley Bell, 2005-2011 Christian Beckhaeuser\n\n");

	if (GameArg.DbgVerbose)
		con_printf(CON_VERBOSE,"%s%s", TXT_VERBOSE_1, "\n");
	
	ReadConfigFile();

	PHYSFSX_addArchiveContent();

	arch_init();

	select_tmap(GameArg.DbgTexMap);

	Lighting_on = 1;

	con_printf(CON_VERBOSE, "Going into graphics mode...\n");
	gr_set_mode(Game_screen_mode);

	// Load the palette stuff. Returns non-zero if error.
	con_printf(CON_DEBUG, "Initializing palette system...\n" );
	gr_use_palette_table(D2_DEFAULT_PALETTE );

	con_printf(CON_DEBUG, "Initializing font system...\n" );
	gamefont_init();	// must load after palette data loaded.

	set_default_handler(standard_handler);

	con_printf( CON_DEBUG, "Initializing movie libraries...\n" );
	init_movies();		//init movie libraries

#ifndef QUICK_START
	if (!GameArg.SysNoTitles)
		show_titles();
#endif
	set_screen_mode(SCREEN_MENU);

	con_printf( CON_DEBUG, "\nDoing gamedata_init..." );
	gamedata_init();

	#ifdef EDITOR
	if (GameArg.EdiSaveHoardData) {
		save_hoard_data();
		exit(1);
	}
	#endif

	if (GameArg.DbgNoRun)
		return(0);

	con_printf( CON_DEBUG, "\nInitializing texture caching system..." );
	texmerge_init( 10 );		// 10 cache bitmaps

	//piggy_init_pigfile("groupa.pig");	//get correct pigfile
	load_palette("groupa.256", 1, 0);

	load_extra_models();

	void rt_init();
	rt_init();

	con_printf( CON_DEBUG, "\nRunning game...\n" );
	init_game();

	Players[Player_num].callsign[0] = '\0';

	//	If built with editor, option to auto-load a level and quit game
	//	to write certain data.
	#ifdef	EDITOR
	if (GameArg.EdiAutoLoad) {
		strcpy(Auto_file, GameArg.EdiAutoLoad);
		strcpy(Players[0].callsign, "dummy");
	} else
	#endif
	{
		if(GameArg.SysPilot)
		{
			char filename[32] = "";
			int j;

			if (GameArg.SysUsePlayersDir)
				strcpy(filename, "Players/");
			strncat(filename, GameArg.SysPilot, 12);
			filename[8 + 12] = '\0';	// unfortunately strncat doesn't put the terminating 0 on the end if it reaches 'n'
			for (j = GameArg.SysUsePlayersDir? 8 : 0; filename[j] != '\0'; j++) {
				switch (filename[j]) {
					case ' ':
						filename[j] = '\0';
				}
			}
			if(!strstr(filename,".plr")) // if player hasn't specified .plr extension in argument, add it
				strcat(filename,".plr");
			if(PHYSFSX_exists(filename,0))
			{
				strcpy(strstr(filename,".plr"),"\0");
				strcpy(Players[Player_num].callsign, GameArg.SysUsePlayersDir? &filename[8] : filename);
				read_player_file();
				WriteConfigFile();
			}
		}
	}

#ifdef EDITOR
	if (GameArg.EdiAutoLoad) {
		strcpy((char *)&Level_names[0], Auto_file);
		LoadLevel(1, 1);
	}
	else
#endif
	{
#ifdef RT_DX12
    for(int test_argument=1;test_argument<Num_args;++test_argument) {
        if(!d_stricmp(Args[test_argument],"-vr-graphics-menu-test")) {
            extern void vr_graphics_menu_test(void);
            vr_graphics_menu_test();return 0;
        }
    }
#endif
#ifdef RT_DX12
        for(int argument=1;argument<Num_args;++argument) {
            if(!d_stricmp(Args[argument],"-openvr-leveltest")) {
                char* regression_mission=FULL_MISSION_FILENAME;
                for(int a=1;a+1<Num_args;++a)
                    if(!d_stricmp(Args[a],"-openvr-test-mission")) regression_mission=Args[a+1];
                if(!load_mission_by_name(regression_mission)) return 3;
                for(int a=1;a<Num_args;++a) if(!d_stricmp(Args[a],"-openvr-model-cycle")) {
                    RT_InitAllPolyModels();
                    if(!load_mission_by_name(FULL_MISSION_FILENAME)) return 3;
                    RT_InitAllPolyModels();
                    if(!RT_AuditPolyModelCache("vr-model-cycle-base.csv")) return 7;
                    if(!load_mission_by_name(regression_mission)) return 3;
                }
                for(int a=1;a<Num_args;++a) if(!d_stricmp(Args[a],"-openvr-model-audit")) {
                    int valid=RT_AuditPolyModelCache("vr-model-cache.csv");
                    FILE* report=fopen("vr-model-robots.csv","wb");
                    if(!report) return 7;
                    fprintf(report,"robot,model,ship_model\n");
                    for(int robot=0;robot<N_robot_types;++robot) {
                        fprintf(report,"%d,%d,%d\n",robot,Robot_info[robot].model_num,Player_ship->model_num);
                        if(Robot_info[robot].model_num==Player_ship->model_num) valid=0;
                    }
                    fclose(report);
                    if(!valid) return 7;
                    RT_InitAllPolyModels();
                    if(!RT_AuditPolyModelCache("vr-model-cache-uploaded.csv")) return 7;
                    if(!RT_AuditRobotDefinitions("vr-model-definitions.csv")) return 7;
                }
                // Same level setup as StartNewGame, omitting interactive intros.
                Game_mode=GM_NORMAL;GameTime64=0;Next_level_num=0;N_players=1;
                InitPlayerObject();init_player_stats_game(Player_num);
                int regression_level=1, regression_segment=-1;
                for(int a=1;a+1<Num_args;++a) {
                    if(!d_stricmp(Args[a],"-openvr-test-level")) regression_level=atoi(Args[a+1]);
                    if(!d_stricmp(Args[a],"-openvr-test-segment")) regression_segment=atoi(Args[a+1]);
                }
                if(regression_level==0 || regression_level>Last_level || regression_level<Last_secret_level) return 5;
                StartNewLevelSub(regression_level,1,regression_level<0);
                for(int a=1;a<Num_args;++a) if(!d_stricmp(Args[a],"-openvr-all-robots-render"))
                    return RT_RenderAllRobotStates() ? 0 : 7;
                for(int a=1;a<Num_args;++a) if(!d_stricmp(Args[a],"-openvr-light-audit")) {
                    extern int RT_IsLight(int tmap);
                    FILE* audit=fopen("vr-leveltest-light-audit.csv","wb");
                    if(audit) {
                        fprintf(audit,"segment,side,texture,name,emission,rt_definition,authored_light\n");
                        for(int seg=0;seg<=Highest_segment_index;++seg) for(int side_index=0;side_index<6;++side_index) {
                            side* s=&Segments[seg].sides[side_index];
                            if(Segments[seg].children[side_index]>=0 && s->wall_num<0) continue;
                            float authored=0;
                            for(int corner=0;corner<4;++corner) authored+=f2fl(s->uvls[corner].l)*0.25f;
                            int maps[2]={s->tmap_num,s->tmap_num2 & 0x3fff};
                            for(int layer=0;layer<2;++layer) {
                                int map=maps[layer]; if(layer && !map) continue;
                                char name[13]={0};piggy_get_bitmap_name(Textures[map].index,name);
                                fprintf(audit,"%d,%d,%d,%s,%f,%d,%f\n",seg,side_index,map,name,f2fl(TmapInfo[map].lighting),RT_IsLight(map),authored);
                            }
                        }
                        fclose(audit);
                    }
                }
                if(regression_segment>=0) {
                    if(regression_segment>Highest_segment_index) return 6;
                    compute_segment_center(&Viewer->pos,&Segments[regression_segment]);
                    obj_relink((int)(Viewer-Objects),regression_segment);
                    for(int side=0;side<6;++side) {
                        int child=Segments[regression_segment].children[side];
                        if(child>=0) {
                            vms_vector center,direction;
                            compute_segment_center(&center,&Segments[child]);
                            vm_vec_sub(&direction,&center,&Viewer->pos);
                            vm_vector_2_matrix(&Viewer->orient,&direction,NULL,NULL);
                            break;
                        }
                    }
                }
                extern void game_render_frame(void);
                FILE* scene=fopen("vr-leveltest-scene.json","wb");
                if(scene) {
                    fprintf(scene,"{\"mission\":\"%s\",\"level\":%d,\"segments\":%d,\"start_segment\":%d}\n",
                        Current_mission_filename,Current_level_num,Highest_segment_index+1,Viewer->segnum);
                    fclose(scene);
                }
                int turning_regression=0;
                for(int a=1;a<Num_args;++a)
                    if(!d_stricmp(Args[a],"-openvr-turntest")) turning_regression=1;
                const vms_matrix regression_orientation=Viewer->orient;
                for(int frame=0;frame<120;++frame) {
                    if(turning_regression) {
                        vms_angvec turn={0};vms_matrix rotation;
                        turn.h=(fixang)((frame*65536)/120);
                        turn.p=(fixang)((frame<60 ? frame : 120-frame)*64-1920);
                        vm_angles_2_matrix(&rotation,&turn);
                        vm_matrix_x_matrix(&Viewer->orient,&regression_orientation,&rotation);
                    }
                    FrameTime=F1_0/72;
                    RT_BeginFrame();RT_StartImGuiFrame();
                    game_render_frame();
                    if(frame==90) for(int a=1;a<Num_args;++a)
                        if(!d_stricmp(Args[a],"-desktop-screenshot-test")) RT_QueueScreenshot("desktop-screenshot-test.png");
                    RT_EndImguiFrame();RT_EndFrame();gr_flip(); // Match the event loop presentation/reset boundary.
                }
                return 0;
            }
        }
#endif
		Game_mode = GM_GAME_OVER;
		DoMenu();
	}

#ifdef QUICK_START
	select_mission(0, "New Game\n\nSelect mission", do_new_game_menu);
#endif //QUICK_START

	setjmp(LeaveEvents);
	while (window_get_front())
		// Send events to windows and the default handler
		event_process();
	
	// Tidy up - avoids a crash on exit
	{
		window *wind;

		show_menus();
		while ((wind = window_get_front()))
			window_close(wind);
	}

	WriteConfigFile();
	show_order_form();

	con_printf( CON_DEBUG, "\nCleanup...\n" );
	close_game();
	texmerge_close();
	gamedata_close();
	gamefont_close();
	free_text();
	args_exit();
	newmenu_free_background();
	free_mission();
	PHYSFSX_removeArchiveContent();

	return(0);		//presumably successful exit
}
