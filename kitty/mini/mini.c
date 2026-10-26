/*
 * Small .ini file library: an ini file read into a list of sections, each
 * holding a list of key/value pairs, and written back out again. Provides the
 * section and key primitives (create, add, delete, look up, free), the parser
 * that turns a file into that tree, the writer that stores it, and the
 * one-shot readINI / writeINI / delINI helpers. Values are cleaned as they are
 * read (see mini_clean_value), and reads are served from a cached copy of the
 * file that is reloaded only when the file's timestamp changes.
 */
#include "mini.h"
#include <time.h>   /* time(): the cache compares the file's timestamp */
#include <windows.h>   /* storeINI: ReplaceFile and Windows' reason on failure */
#include "../kitty_inifile.h"   /* kitty_inifile_write_blocked: readonly=yes */

#ifdef DEBUG_MODE
#include "leaktracker.h"
#endif

#define STYPE_NULL 0
#define STYPE_SECTION 1
#define STYPE_KEY 2

#define SINI_MAX_SIZE 4096


SSECTION * newSECTION( const char * name ) {
	SSECTION * newSection = NULL ;
	if( name==NULL ) return NULL ;
	if( strlen(name)<=0 ) return NULL ;
	if( ( newSection = (SSECTION*) malloc( sizeof( SSECTION ) ) ) != NULL ) {
		newSection->type = STYPE_SECTION ;
		if( ( newSection->name = (char*) malloc( strlen( name ) + 1 ) ) != NULL )
			strcpy( newSection->name, name ) ;
		newSection->next = NULL ;
		newSection->first = NULL ;
		}
	return newSection ;
	}

SKEY * newKEY( const char * name, const char * value ) {
	SKEY * newKey = NULL ;
	if( name==NULL ) return NULL ;
	if( strlen(name)<= 0 ) return NULL ;
	if( ( newKey = (SKEY*) malloc( sizeof( SKEY ) ) ) != NULL ) {
		newKey->type = STYPE_KEY ;
		if( ( newKey->name = (char*) malloc( strlen( name ) + 1 ) ) != NULL )
			strcpy( newKey->name, name ) ;
		if( ( newKey->value = (char*) malloc( strlen( value ) + 1 ) ) != NULL )
			strcpy( (char*)newKey->value, value ) ;
		newKey->next = NULL ;
		}
	return newKey ;
	}

SINI * newINI( void ) {
	SINI * newIni = NULL ;
	if( ( newIni = (SINI*) malloc( sizeof( SINI ) ) ) != NULL ) {
		newIni->name = NULL ;
		newIni->first = NULL ;
		}
	return newIni ;
	}

void freeSECTION( SSECTION ** Section ) {
	if( Section == NULL ) return ;
	if( (*Section) == NULL ) return ;

	if( (*Section)->next != NULL ) 
		{ freeSECTION( &((*Section)->next) ) ; (*Section)->next = NULL ; }
	if( (*Section)->first != NULL ) 
		{ freeKEY( &((*Section)->first) ) ; (*Section)->first = NULL ; }

	if( (*Section)->name != NULL ) { free( (*Section)->name ) ; (*Section)->name = NULL ; }

	(*Section)->type = STYPE_NULL ;

	free( *Section ) ;
	(*Section) = NULL ;
	}

void freeKEY( SKEY ** Key ) {
	if( Key == NULL ) return ;
	if( (*Key) == NULL ) return ;

	if( (*Key)->next != NULL ) 
		{ freeKEY( &((*Key)->next) ) ; (*Key)->next = NULL ; }
	
	if( (*Key)->name != NULL ) { free( (*Key)->name ) ; (*Key)->name = NULL ; }
	if( (*Key)->value != NULL ) { free( (*Key)->value ) ; (*Key)->value = NULL ; }
	
	(*Key)->type = STYPE_NULL ;
	
	free( *Key ) ;
	(*Key) = NULL ;
	}
	
void freeINI( SINI ** Ini ) {
	if( Ini == NULL ) return ;
	if( (*Ini) == NULL ) return ;
	if( (*Ini)->name != NULL ) { free( (*Ini)->name ) ; (*Ini)->name = NULL ; }
	freeSECTION( & ((*Ini)->first) ) ;
	free( *Ini ) ;
	(*Ini) = NULL ;
	}

int addSECTION( SSECTION * Section, SSECTION * SectionAdd ) {
	SSECTION * Current ;
	if( Section == NULL ) return 0 ;
	if( SectionAdd == NULL ) return 0 ;
	Current = Section->next ;
	
	if( Current == NULL ) {
		Section->next = SectionAdd ;
		}
	else {
		while( Current->next != NULL ) { Current = Current->next ; }
		Current->next = SectionAdd ;
		}
	return 1 ;
	}
	
int addINI( SINI * Ini, SSECTION * Section ) {
	int return_code = 1 ;
	if( Ini->first == NULL ) Ini->first = Section ;
	else return_code = addSECTION( Ini->first, Section ) ;
	return return_code ;
	}

int delSECTION( SSECTION * Section, const char * name ) {
	int return_code = 0 ;
	SSECTION * Current, * Last = NULL, * Next = NULL ;
	if( Section == NULL ) return 0 ;
	if( name == NULL ) return 0 ;
	if( strlen( name ) == 0 ) return 0 ; 
	Current = Section->next ;
	if( Current == NULL ) return 0 ;

	while( (Current != NULL) && ( strcmp( Current->name, name ) ) ) {
		Last = Current ;
		Current = Current->next ;
		Next = Current->next ;
		}
	if( !strcmp( Current->name, name ) && ( Current->type = STYPE_SECTION ) ) {
		freeSECTION( &Current ) ;
		if( Last != NULL ) Last->next = Next ;
		else Section->next = Next ;
		return_code = 1 ;
		}
	return return_code ;
	}

int addKEY( SSECTION * Section, SKEY * Key ) {
	SKEY * Current ;
	if( Section == NULL ) return 0 ;
	if( Key == NULL ) return 0 ;
	
	if( ( Current = getKEY( Section, Key->name ) ) != NULL ) { // if the key already exists, replace its value in place
		//if( !delKEY( Section, Key->name ) ) return 0 ;
		if( Current->value != NULL ) { free( Current->value ) ; Current->value = NULL ; }
		if( ( Current->value = malloc( strlen( (const char*)(Key->value) ) + 1 ) ) == NULL ) return 0 ;
		//strcpy( Current->value, Key->value ) ;
		memcpy( Current->value, Key->value, strlen( (const char*)Key->value ) + 1 ) ;
		freeKEY( &Key ) ;
		return 1 ; 
		}
	
	Current = Section->first ;

	if( Current == NULL ) { 
		Section->first = Key ;
		}
	else {
		while( Current->next != NULL ) { Current = Current->next ; }
		Current->next = Key ;
		}
	return 1 ;
	}

void freeSKEY( SKEY ** c ) { free( *c ) ; }
	
int delKEY( SSECTION * Section, const char * name ) {
	int return_code = 0 ;
	SKEY * Current, * Last = NULL, * Next = NULL ;
	if( Section == NULL ) return 0 ;
	if( name == NULL ) return 0 ;
	if( strlen( name ) == 0 ) return 0 ; 
	Current = Section->first ;
	if( Current == NULL ) return 0 ;
	while( (Current != NULL ) && ( strcmp( Current->name, name ) ) ) {
		Last = Current ;
		Current = Current->next ;
		if( Current != NULL ) Next = Current->next ; else Next = NULL ;
		}
	if( Current != NULL )
		if( !strcmp( Current->name, name ) && ( Current->type = STYPE_KEY ) ) {
			free( Current->name ) ; Current->name = NULL ;
			if( Current->value != NULL ) 
				{ free( Current->value ) ; Current->value = NULL ; }
			Current->type = STYPE_NULL ;
			freeSKEY( &Current ) ;
			Current = NULL ;
			if( Last != NULL ) Last->next = Next ;
			else Section->first = Next ;
			return_code = 1 ;
			}
	return return_code ;
	}

SSECTION * getSECTION( SSECTION * Section, const char * name ) {
	SSECTION * Current ;
	if( Section == NULL ) return NULL ;
	if( name == NULL ) return Section ;
	if( strlen( name ) == 0 ) return Section ;
	Current = Section ;
	while( ( Current != NULL ) && ( strcmp( Current->name, name ) ) ) {
		Current = Current->next ;
		}
	return Current ;
	}
	
SSECTION * lastSECTION( SSECTION * Section ) {
	SSECTION * Current ;
	if( Section == NULL ) return NULL ;
	Current = Section ;
	while( Current->next != NULL ) {
		Current = Current->next ;
		}
	return Current ;
	}

SKEY * getKEY( SSECTION * Section, const char * name ) {
	SKEY * Current ;
	if( Section == NULL ) return NULL ;
	if( name == NULL ) return Section->first ;
	if( strlen( name ) == 0 ) return Section->first ;
	Current = Section->first ;
	while( ( Current != NULL ) && ( strcmp( Current->name, name ) ) ) {
		Current = Current->next ;
		}
	return Current ;
	}

char * getvalueKEY( SKEY * Key ) {
	if( Key == NULL ) return NULL ;
	return (char*)Key->value ;
	}

int setKEY( SKEY * Key, char * value ) {
	if( Key == NULL ) return 0 ;
	if( Key->value != NULL ) { free( Key->value ) ; Key->value = NULL ; }
	if( value != NULL ) {
		if( ( Key->value = (char*) malloc( strlen( value ) + 1 ) ) == NULL ) return 0 ;
		strcpy( (char*)Key->value, value ) ;
		}
	return 1 ;
	}
	
SSECTION * getINI( SINI * Ini ) {
	if( Ini == NULL ) return NULL ;
	return Ini->first ;
	}
	
void printKEY( SKEY * Key ) {
	if( Key == NULL ) return ;
	printf( "%s=%s\n", Key->name, (char*)Key->value ) ;
	if( Key->next != NULL ) printKEY( Key->next ) ;
	}

void printSECTION( SSECTION * Section ) {
	if( Section == NULL ) return ;
	if( Section->type == STYPE_SECTION ) printf( "[%s]\n", Section->name ) ;
	if( Section->first != NULL ) printKEY( Section->first ) ;
	if( Section->next != NULL ) printSECTION( Section->next ) ;
	}
	
void printINI( SINI * Ini ) {
	if( Ini == NULL ) return ;
	printSECTION( Ini->first ) ;
	}

/*
 * KiTTY: tidy the VALUE side of a "key = value" line - strip whitespace at both
 * ends, then one pair of matching quotes.
 *
 * The value used to be taken verbatim from after the '=' to the end of the
 * line, while the key name was already being right-trimmed. So
 * "configdir = C:\somewhere" carried a LEADING SPACE into the path, the
 * directory was never found, and the natural next move - quoting the path -
 * made it worse by adding two more characters that were also kept
 * (cyd01/KiTTY#549: the reporter reasonably concluded that paths with spaces
 * were unsupported; spaces in the path were never the problem). Trailing
 * spaces are invisible and did the same damage.
 *
 * QUOTES ARE THE WAY BACK. A value wrapped in one pair of matching quotes
 * keeps everything BETWEEN them exactly as written - leading and trailing
 * spaces and tabs included. Only the whitespace OUTSIDE the quotes and the
 * one wrapping pair are removed, so
 *
 *     key =  " two spaces here:  "
 *
 * yields ` two spaces here:  `. It used to trim inside the quotes as well,
 * which left no way at all to store such a value and made quoting look like
 * a no-op; the application notification, which keeps the note exactly as it
 * was typed, is the first key that needs it.
 *
 * An UNQUOTED value is trimmed at both ends as before, which is what makes
 * "configdir = C:\somewhere" work.
 */
void mini_clean_value( char * value ) {
	size_t l ;
	char * p = value ;
	if( value == NULL ) return ;
	while( *p==' ' || *p=='\t' ) p++ ;
	if( p != value ) memmove( value, p, strlen(p)+1 ) ;
	while( (l=strlen(value))>0 && (value[l-1]==' '||value[l-1]=='\t') ) value[l-1]='\0' ;
	l = strlen( value ) ;
	if( l >= 2 && ( (value[0]=='"' && value[l-1]=='"') || (value[0]=='\'' && value[l-1]=='\'') ) ) {
		/* Inside the quotes is the value, verbatim. */
		memmove( value, value+1, l-2 ) ;
		value[l-2] = '\0' ;
	}
}

/* One line of fp in full, without its line end (free it); NULL at the end of
 * the file or when memory runs out (*oom set). A long line is never split:
 * the pieces fgets made of it were read as lines of their own. */
static char * mini_read_line( FILE * fp, int * oom ) {
	size_t len = 0, cap = 256 ;
	char * line = (char*) malloc( cap ), * grown ;
	int c, any = 0 ;
	*oom = 0 ;
	if( line == NULL ) { *oom = 1 ; return NULL ; }
	while( ( c = fgetc( fp ) ) != EOF ) {
		any = 1 ;
		if( c == '\n' ) break ;
		if( len + 1 >= cap ) {
			if( ( grown = (char*) realloc( line, cap * 2 ) ) == NULL ) { free( line ) ; *oom = 1 ; return NULL ; }
			line = grown ; cap *= 2 ;
		}
		line[len++] = (char) c ;
	}
	if( !any ) { free( line ) ; return NULL ; }
	line[len] = '\0' ;
	return line ;
}

int loadINI( SINI * Ini, const char * filename ) {
	FILE * fp ;
	SSECTION * Section, * Last = NULL ;
	SKEY * Key ;
	char * buffer, * name, * value ;
	unsigned int i, p ;
	int oom = 0 ;
	if( Ini == NULL ) return 0 ;
	if( filename==NULL ) return 0 ;
	if( strlen(filename)<=0 ) return 0 ;
	freeSECTION( &(Ini->first) ) ;
	if( ( fp = fopen( filename, "r" ) ) == NULL ) { return 0 ; }
	while( ( buffer = mini_read_line( fp, &oom ) ) != NULL ) {
		/* name and value never longer than the line */
		size_t blen = strlen( buffer ) ;
		name = (char*) malloc( blen + 1 ) ;
		value = (char*) malloc( blen + 1 ) ;
		if( name == NULL || value == NULL ) { free( name ) ; free( value ) ; free( buffer ) ; oom = 1 ; break ; }
		{ size_t _l; while( (_l=strlen(buffer))>0 && (buffer[_l-1]=='\n'||buffer[_l-1]=='\r') ) buffer[_l-1]='\0' ; }
		while( (buffer[0]==' ')||(buffer[0]=='\t') ) 
			for( i=0; i<strlen(buffer); i++ )
				buffer[i] = buffer[i+1] ;
		if( buffer[0] == '[' ) { // new section
			{ size_t _l; while( (_l=strlen(buffer))>0 && (buffer[_l-1]==' '||buffer[_l-1]=='\t') ) buffer[_l-1]='\0' ; } 
			if( buffer[strlen(buffer)-1]==']' ) {
				buffer[strlen(buffer)-1]='\0' ;
				if( (Section = getSECTION( Ini->first, buffer+1 )) == NULL ) { //look for an already existing section
					Section = newSECTION( buffer+1 ) ;
					if( Ini->first == NULL ) Ini->first = Section ;
 					else addSECTION( Ini->first, Section ) ;
 					}
				Last = Section ;
				}
			}
		else if( Last!= NULL ) { // new key in the current section
			name[0] = '\0' ; value[0] = '\0' ;
			p = 0 ;
			for( i=0; (i<strlen(buffer))&&(buffer[i]!='='); i++ ) p = i+1 ;
			if( (p>0) && (p<(strlen(buffer)-1) ) ) {
				for( i=0; i<p; i++ ) { name[i] = buffer[i] ; } name[p]='\0' ;
				for( i=p+1; i<strlen(buffer); i++ ) { value[i-p-1]=buffer[i] ; value[i-p]='\0' ; }
				{ size_t _l; while( (_l=strlen(name))>0 && (name[_l-1]==' '||name[_l-1]=='\t') ) name[_l-1]='\0' ; }
				mini_clean_value( value ) ;
				Key = newKEY( name, value ) ;
				addKEY( Last, Key ) ;
				}
			else if( p==(strlen(buffer)-1) ) {
				buffer[strlen(buffer)-1] = '\0' ;
				strcpy( name, buffer ) ;
				{ size_t _l; while( (_l=strlen(name))>0 && (name[_l-1]==' '||name[_l-1]=='\t') ) name[_l-1]='\0' ; }
				strcpy( value, "" ) ;
				Key = newKEY( name, value ) ;
				addKEY( Last, Key ) ;
				}
			}
		free( name ) ;
		free( value ) ;
		free( buffer ) ;
		}
	/* a read that stopped early is a failure: what was loaded is only part of
	 * the file and writeINI would store that part over the whole */
	if( oom ) { fclose( fp ) ; SetLastError( ERROR_NOT_ENOUGH_MEMORY ) ; return 0 ; }
	if( ferror( fp ) ) { fclose( fp ) ; SetLastError( ERROR_READ_FAULT ) ; return 0 ; }
	fclose( fp ) ;
	return 1 ;
	}

int storeKEY( SKEY * Key, FILE * fp ) {
	if( Key == NULL ) return 0 ;
	if( fp==NULL ) return 0 ;
	fprintf( fp, "%s=%s\n", Key->name, (char*)Key->value ) ;
	if( Key->next != NULL ) storeKEY( Key->next, fp ) ;
	return 1 ;
	}
	
int storeSECTION( SSECTION * Section, FILE * fp ) {
	if( Section == NULL ) return 0 ;
	if( fp==NULL ) return 0 ;
	fprintf( fp, "[%s]\n", Section->name ) ;
	if( Section->first != NULL ) storeKEY( Section->first, fp ) ;
	if( Section->next != NULL ) storeSECTION( Section->next, fp ) ;
	return 1 ;
	}

/* The whole file is written to a temporary one beside it and swapped in only
 * once every byte is on disk. fopen(filename, "w") emptied kitty.ini first, so
 * a full disk, a locked file or a crash between the two left it truncated -
 * while this reported success. Now a failure leaves the old file untouched and
 * returns 0, with Windows' reason in GetLastError(). ReplaceFile keeps the
 * file's rights and attributes; a file that is not there yet is moved in. */
int storeINI( SINI * Ini, const char * filename ) {
	FILE * fp ;
	char * tmp ;
	size_t tn ;
	DWORD err = 0 ;
	int ok, tries ;
	if( filename==NULL ) return 0 ;
	if( strlen(filename)<=0 ) return 0 ;
	if( kitty_inifile_write_blocked( filename ) ) return KITTY_RO_REFUSED ;
	/* the temporary name in full: "<file>.<pid>.tmp" */
	tn = strlen( filename ) + 32 ;
	if( ( tmp = (char*) malloc( tn ) ) == NULL ) { SetLastError( ERROR_NOT_ENOUGH_MEMORY ) ; return 0 ; }
	snprintf( tmp, tn, "%s.%lu.tmp", filename, (unsigned long)GetCurrentProcessId() ) ;
	if( ( fp = fopen( tmp, "w" ) ) == NULL ) { err = GetLastError() ; free( tmp ) ; SetLastError( err ) ; return 0 ; }

	storeSECTION( getINI( Ini ), fp ) ;
	ok = !ferror( fp ) && fflush( fp ) == 0 ;
	if( fclose( fp ) != 0 ) ok = 0 ;
	if( !ok ) { err = GetLastError() ; DeleteFileA( tmp ) ; free( tmp ) ; SetLastError( err ? err : ERROR_WRITE_FAULT ) ; return 0 ; }

	/* another process may have the file open for a moment (a reader) */
	for( tries = 0 ; tries < 10 ; tries++ ) {
		if( GetFileAttributesA( filename ) == INVALID_FILE_ATTRIBUTES ) {
			if( MoveFileExA( tmp, filename, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH ) ) { free( tmp ) ; return 1 ; }
		} else if( ReplaceFileA( filename, tmp, NULL, REPLACEFILE_IGNORE_MERGE_ERRORS, NULL, NULL ) ) {
			free( tmp ) ;
			return 1 ;
		}
		err = GetLastError() ;
		if( err != ERROR_SHARING_VIOLATION && err != ERROR_LOCK_VIOLATION && err != ERROR_UNABLE_TO_REMOVE_REPLACED ) break ;
		Sleep( 50 ) ;
	}
	DeleteFileA( tmp ) ;
	free( tmp ) ;
	SetLastError( err ) ;
	return 0 ;
	}
	
/* Improvement so that the file is not read again every time */
static char * mini_filename = NULL ;
static SINI * mini_Ini = NULL ;

/*
 * kitty.ini is read and written through Windows' own ini functions, the ones
 * kageant, kittygen and the console tools have always used (kitty_inifile.c):
 * one reader and one writer for the file across the suite. A write changes
 * its one line and leaves the rest of the file - the template's comments,
 * the order, the layout - as it was; the whole-file rewrite this replaced
 * dropped every comment line at the first save, and its swap (ReplaceFile)
 * was refused while anything held the file open, Windows' own ini cache in
 * this very process included (error 1175). Windows reads a section written
 * twice only where it first appears; kitty.exe merges such files once at
 * start (kitty_startup.c ConsolidateKittyIni), keeping what was read here.
 */

/* A value GetPrivateProfileString returns when the key is not there - the
 * call cannot distinguish "absent" from "empty" otherwise. */
static const char mini_absent[] = "\x01kitty.ini:absent\x01" ;

int readINI( const char * filename, const char * section, const char * key, char * pStr, size_t pStrSize) {
	char * buf ;
	DWORD cap ;
	if( filename == NULL || !filename[0] || section == NULL || !section[0] || key == NULL ) return 0 ;
	if( pStr == NULL || pStrSize == 0 ) return 0 ;
	cap = pStrSize < 65536 ? (DWORD)pStrSize : 65536 ;
	if( cap < sizeof(mini_absent) ) cap = sizeof(mini_absent) ;
	if( ( buf = (char*)malloc( cap ) ) == NULL ) return 0 ;
	GetPrivateProfileStringA( section, key, mini_absent, buf, cap, filename ) ;
	if( !strcmp( buf, mini_absent ) ) { free( buf ) ; return 0 ; }
	{ size_t n = strlen( buf ) ; if( n >= pStrSize ) n = pStrSize - 1 ; memcpy( pStr, buf, n ) ; pStr[n] = '\0' ; }
	free( buf ) ;
	return 1 ;
	}

/* 1 written; 0 not, Windows' reason in GetLastError(). A NULL key makes sure
 * the section exists (an empty one is written only where there is none).
 * The kitty.ini in charge under readonly=yes: nothing is written,
 * KITTY_RO_REFUSED (delINI too); any other file is written as ever. */
int writeINI( const char * filename, const char * section, const char * key, const char * value ) {
	if( filename == NULL || !filename[0] || section == NULL || !section[0] ) return 0 ;
	if( kitty_inifile_write_blocked( filename ) ) return KITTY_RO_REFUSED ;
	if( key == NULL || !key[0] ) {
		char probe[4] ;
		if( GetPrivateProfileSectionA( section, probe, sizeof(probe), filename ) > 0 ) return 1 ;
		return WritePrivateProfileSectionA( section, "\0", filename ) ? 1 : 0 ;
		}
	return WritePrivateProfileStringA( section, key, value ? value : "", filename ) ? 1 : 0 ;
	}

/* One key, or with a NULL key the whole section. */
int delINI( const char * filename, const char * section, const char * key ) {
	if( filename == NULL || !filename[0] || section == NULL || !section[0] ) return 0 ;
	if( kitty_inifile_write_blocked( filename ) ) return KITTY_RO_REFUSED ;
	return WritePrivateProfileStringA( section, ( key && key[0] ) ? key : NULL, NULL, filename ) ? 1 : 0 ;
	}

int delINISec( const char * filename, const char * section, const char * key ) {
	int return_code = 0 ;
	char * newname = NULL ;
	if( (newname=(char*)malloc( strlen(filename)+5 )) == NULL ) return 0 ;
	sprintf( newname, "%s.new", filename );
	return_code = delINI( newname, section, key ) ;
	free(newname) ;
	return return_code ;
	}
		
void destroyINI( void ) { 
	if(mini_filename!=NULL) { free(mini_filename) ; } mini_filename=NULL ;
	freeINI( &mini_Ini ) ;
	}

int ini_main( int argc, char *argv[], char *arge[] ) {
	char buf[256];

	
	//SINI * INI = newINI( ) ; loadINI( INI, "test.ini" ) ; printINI( INI ) ; freeINI( &INI ) ;
	//delINI( "test.ini", "new_section", "password") ;
	
	system( "rm test.ini" ) ;

	writeINI( "test.ini", "Section1", "Name1", "Value1") ;
	writeINI( "test.ini", "Section1", "Name2", "Value2") ;
	system( "cat test.ini" ) ; system( "echo." ) ;
	writeINI( "test.ini", "Section2", "Name1", "Value1") ;
	
	system( "cat test.ini" ) ; system( "echo." ) ;
	writeINI( "test.ini", "Section1", "Name1", "Value3") ;
	
	system( "cat test.ini" ) ; system( "echo." ) ;
	writeINI( "test.ini", "Section3", NULL, "") ;
	
	system( "cat test.ini" ) ; system( "echo." ) ;
	delINI( "test.ini", "Section2", "Name1" );
	system( "cat test.ini" ) ; system( "echo." ) ;
	
	readINI( "test.ini", "Section1", "Name1", buf, sizeof(buf)); printf("Name1=%s\n",buf);
	readINI( "test.ini", "Section1", "Name2", buf, sizeof(buf)); printf("Name2=%s\n",buf);
	
	char b[10];
	fscanf( stdin, "%s", b) ;
	readINI( "test.ini", "Section1", "Name1", buf, sizeof(buf)); printf("Name1=%s\n",buf);
	
	fscanf( stdin, "%s", b) ;
	readINI( "test.ini", "Section1", "Name1", buf, sizeof(buf)); printf("Name1=%s\n",buf);
	
	destroyINI();

	return 1;
	}

#ifdef MAIN
int main( int argc, char *argv[], char *arge[] ) {
	int return_code = 0 ;
	return_code = ini_main( argc, argv, arge ) ;
#ifdef DEBUG_MODE
	_leaktracker61_EnableReportLeakOnApplication_Exit("ini.leaktracker.log", 1);
	_leaktracker61_DumpAllLeaks("ini.leaktracker.log", 0);
	printf("Leak tracker log in ini.leaktracker.log file\n");
#endif
	return return_code ;
	}
#endif
