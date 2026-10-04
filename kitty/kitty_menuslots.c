/*
 * KiTTY: the payloads behind the IDM_USERCMD + n menu entries, in a table
 * that grows as the menu is filled (see kitty_menuslots.h). One table per
 * process: a process builds either the launcher's menu or a terminal's User
 * Command menu, and every rebuild starts with kitty_menuslots_reset().
 */
#include <stdlib.h>
#include <string.h>

#include "kitty_menuslots.h"

static char ** slots = NULL ;
static int slots_n = 0, slots_alloc = 0 ;
static int slots_cap = NB_MENU_MAX ;
static int slots_cut = 0 ;

void kitty_menuslots_reset( int cap ) {
	int i ;
	for( i = 0 ; i < slots_n ; i++ ) free( slots[i] ) ;
	free( slots ) ;
	slots = NULL ;
	slots_n = slots_alloc = 0 ;
	slots_cut = 0 ;
	slots_cap = ( cap <= 0 || cap > NB_MENU_MAX ) ? NB_MENU_MAX : cap ;
}

int kitty_menuslots_add( const char * data ) {
	size_t len ;
	char * copy ;
	if( data == NULL ) data = "" ;
	if( slots_n >= slots_cap ) { slots_cut++ ; return -1 ; }
	if( slots_n == slots_alloc ) {
		int na = slots_alloc ? slots_alloc * 2 : 64 ;
		char ** nv ;
		if( na > slots_cap ) na = slots_cap ;
		nv = (char **)realloc( slots, (size_t)na * sizeof(*nv) ) ;
		if( nv == NULL ) { slots_cut++ ; return -1 ; }
		slots = nv ;
		slots_alloc = na ;
	}
	len = strlen( data ) ;
	if( ( copy = (char *)malloc( len + 1 ) ) == NULL ) { slots_cut++ ; return -1 ; }
	memcpy( copy, data, len + 1 ) ;
	slots[slots_n] = copy ;
	return slots_n++ ;
}

char * kitty_menuslots_get( int n ) {
	if( n < 0 || n >= slots_n ) return NULL ;
	return slots[n] ;
}

int kitty_menuslots_count( void ) { return slots_n ; }
int kitty_menuslots_cut( void ) { return slots_cut ; }
int kitty_menuslots_cap( void ) { return slots_cap ; }
