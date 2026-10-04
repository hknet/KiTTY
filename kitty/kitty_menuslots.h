/*
 * KiTTY: the payloads behind the menu entries numbered IDM_USERCMD + n - the
 * launcher's session menu and the terminal's User Command menu. The table
 * grows as entries are added, up to a ceiling; an entry past the ceiling is
 * not stored but counted, so the menu can say how many it left out.
 *
 * No Win32 and no PuTTY headers: test/test_menuslots.c links this file alone.
 */
#ifndef KITTY_MENUSLOTS_H
#define KITTY_MENUSLOTS_H

/*
 * The ceiling: entries IDM_USERCMD + 0 .. IDM_USERCMD + NB_MENU_MAX - 1,
 * i.e. 0x8000 .. 0x8FFF. The next command id either process handles is
 * IDM_GOHIDE (0x9000, the launcher's Open Sessions entries); the terminal
 * window's next one is IDM_QUIT (0xA840). kitty_specialmenu.c checks both at
 * compile time. This is the only definition.
 */
#define NB_MENU_MAX 0x1000

/* Empty the table (frees every payload) and set the ceiling for the next
 * fill: `cap` <= 0 or above NB_MENU_MAX means NB_MENU_MAX. */
void kitty_menuslots_reset( int cap ) ;
/* Store a copy of `data` as the next entry: its number (0, 1, ...), or -1
 * when the ceiling is reached (the entry is then counted as left out) or the
 * memory ran out. */
int kitty_menuslots_add( const char * data ) ;
/* Entry n's payload, NULL for a number not in use. */
char * kitty_menuslots_get( int n ) ;
/* Entries stored since the last reset. */
int kitty_menuslots_count( void ) ;
/* Entries left out since the last reset. */
int kitty_menuslots_cut( void ) ;
/* The ceiling in force. */
int kitty_menuslots_cap( void ) ;

#endif
