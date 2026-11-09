// Game profile: technical facts about ONE game (what the host must present or allow), as opposed to
// configs.cfg, which holds the user's preferences. Files live in the fixed folder c:\system\apps\symbridge\profiles\ (installed by the .sis) :
//   default.prf        read first (every key optional)
//   <UID3>.prf         8 hex digits, overrides the keys it contains (e.g. 0548E337.prf)
// Missing files or keys fall back to the built-in values, which equal the Captain Dynamo behaviour.
//
// Keys (all optional):
//   screen_width / screen_height   size of the "screen" the game is told about (32..640)
//   keys_plain       				1 = the control-stack route also gets the plain EEventKey (menus of games that read only that)
//   auto_cmd                       command id sent once to the game's AppUi::HandleCommandL after start-up (0 = none)
//   display_mode                   7 = EColor64K (default); 10 = EColor4K: the game is told the screen is 12-bit
//                                  (the host still shows RGB565; 4K bitmaps are converted when they are blitted)
//   thread_stack_min               minimum stack (0x4000..0x40000) of the threads the game creates
//   thread_high_priority           less | normal | more: what the old "realtime / high" requests become
//   match_code_size                game profile only: ignored when the .app's code size differs
//   link_report                    1 = write one line per import to <app>_links.txt
//   stop_state_tls                 exit trick, see TGameProfile below; 0 = off
//   stop_state_field, stop_state_value
//
// Keys that configs.cfg has too (the profile value wins for this game; see hostconfig.h):
//   show_fps, max_fps, softkey_left_cmd, softkey_right_cmd
// Not allowed here: scale_mode, scale_key and logging (configs.cfg only; they are logged as unknown keys).
#ifndef PROFILE_H
#define PROFILE_H

#include <e32std.h>
#include <f32file.h>
#include "hostlog.h"
#include "hostconfig.h"       // TSharedPrefs

struct TGameProfile
    {
    TInt iScreenW;          // screen_width     size of the "screen" the game is told about
    TInt iScreenH;          // screen_height
	TInt iDisplayMode;      // display_mode     7 = EColor64K, 10 = EColor4K (what the game is told)
    TInt iStackMin;         // thread_stack_min minimum stack of threads the game creates
    TInt iHighPriorityAs;   // thread_high_priority  less | normal | more: what old "realtime/high" requests become
    TInt iMatchCodeSize;    // match_code_size  game profile only: ignored if the .app's code size differs
    TInt iLinkReport;       // link_report       write one line per import to <app>_links.txt when 1

    // Exit trick for games whose engine thread only stops when a state word in its own control block says
    // so. The game keeps the block's address in its Dll::Tls data: at byte offset stop_state_tls of the
    // TLS data there is the pointer to the block, and the word at byte offset stop_state_field of the block
    // is set to stop_state_value (a value that makes the game's signal handler stop its scheduler). This only
    // makes sense for a game with exactly that layout: a game that stops itself, or has another layout,
    // must switch it off with stop_state_tls=0. The defaults are the Captain Dynamo layout.
    TInt iStopStateTls;     // stop_state_tls    0 = no trick
    TInt iStopStateField;   // stop_state_field
    TInt iStopStateValue;   // stop_state_value
	
	TInt iKeysPlain;        // keys_plain       1 = the control-stack route also gets the plain EEventKey (menus of games that read only that)
	TInt iAutoCmd;          // auto_cmd   command id sent once to the game's AppUi::HandleCommandL after start-up (0 = none)
		
    // show_fps, max_fps and the softkey commands as this game's profile sets them; a field the profile does
    // not mention is KPrefUnset and the configs.cfg value is used.
    TSharedPrefs iPrefs;
    };

void Profile_Load(RFs& aFs, TUint32 aUid3, TInt aCodeSize, TGameProfile& aProfile, THostLog& aLog);
#endif
