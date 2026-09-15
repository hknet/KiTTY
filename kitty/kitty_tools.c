/*
 * kitty_tools.c - the small general-purpose helpers the KiTTY additions
 * reuse everywhere: string editing (right-trim, insert, delete, find a
 * substring), tests and sizes for files and directories, removal of doubled
 * backslashes, a NULL-terminated string list (add, delete, move an entry up),
 * setting an environment variable, and creating a directory path one level
 * at a time. Nothing here knows about sessions, windows or the registry; the
 * declarations are in kitty_tools.h.
 */
#include "kitty_tools.h"

/* Remove, in place, the trailing characters belonging to `set` (right-trim).
   Replaces the while(strlen...) loops copied all over the place; safe on an
   empty string (never indexes s[-1]). Returns s. */
char *str_rtrim( char *s, const char *set ) {
	size_t l ;
	if( s == NULL ) return NULL ;
	l = strlen( s ) ;
	while( l > 0 && strchr( set, s[l-1] ) != NULL ) s[--l] = '\0' ;
	return s ;
}

/* Insert one string into another */
int insert( char * ch, const char * c, const int ipos ) {
	int i = ipos, len = strlen( c ), k ;
	if( ( ch == NULL ) || ( c == NULL ) ) return -1 ;
	if( len > 0 ) {
		if( (size_t) i > ( strlen( ch ) + 1 ) ) i = strlen( ch ) + 1 ;
		for( k = strlen( ch ) ; k >= ( i - 1 ) ; k-- ) ch[k + len] = ch[k] ;
		for( k = 0 ; k < len ; k++ ) ch[k + i - 1] = c[k] ; 
	}
	return strlen( ch ) ; 
}

/* Delete part of a string */
int del( char * ch, const int start, const int length ) {
	int k, len = strlen( ch ) ;
	if( ch == NULL ) return -1 ;
	if( ( start == 1 ) && ( length >= len ) ) { ch[0] = '\0' ; len = 0 ; }
	if( ( start > 0 ) && ( start <= len ) && ( length > 0 ) ) {
		for( k = start - 1 ; k < ( len - length ) ; k++ ) {
			if( k < ( len - length ) ) {
				ch[k] = ch[ k + length ] ;
			} else  {
				ch[k] = '\0' ;
			}
		}
		k = len - length ;
		if( ( start + length ) > len ) k = start - 1 ;
		ch[k] = '\0' ; 
	}
	return strlen( ch ) ; 
}

/* Find the position of one string inside another string */
int poss( const char * c, const char * ch ) {
	char * c1 , * ch1 , * cc ;
	int res ;
	if( ( ch == NULL ) || ( c == NULL ) ) return -1 ;
	if( ( c1 = (char *) malloc( strlen( c ) + 1 ) ) == NULL ) return -2 ;
	if( ( ch1 = (char *) malloc( strlen( ch ) + 1 ) ) == NULL ) { free( c1 ) ; return -3 ; }
	strcpy( c1, c ) ; strcpy( ch1, ch ) ;
	cc = (char *) strstr( ch1, c1 ) ;
	if( cc == NULL ) res = 0 ;
	else res = (int) ( cc - ch1 ) + 1 ;
	if( (size_t) res > strlen( ch ) ) res = 0 ;
	free( ch1 ) ;
	free( c1 ) ;
	return res ; 
}

// Test whether a file exists
int existfile( const char * filename ) {
	struct _stat statBuf ;
	
	if( filename == NULL ) return 0 ;
	if( strlen(filename)==0 ) return 0 ;
	if( _stat( filename, &statBuf ) == -1 ) return 0 ;
	
	if( ( statBuf.st_mode & _S_IFMT ) == _S_IFREG ) { return 1 ; }
	else { return 0 ; }
}

// Test whether a directory exists
int existdirectory( const char * filename ) {
	struct _stat statBuf ;
	
	if( filename == NULL ) return 0 ;
	if( strlen(filename)==0 ) return 0 ;
	if( _stat( filename, &statBuf ) == -1 ) return 0 ;
	
	if( ( statBuf.st_mode & _S_IFMT ) == _S_IFDIR ) { return 1 ; }
	else { return 0 ; }
}

/* Return the size of a file */
long filesize( const char * filename ) {
	FILE * fp ;
	long length ;

	if( filename == NULL ) return 0 ;
	if( strlen( filename ) <= 0 ) return 0 ;
	
	if( ( fp = fopen( filename, "r" ) ) == 0 ) return 0 ;
	
	fseek( fp, 0L, SEEK_END ) ;
	length = ftell( fp ) ;
	
	fclose( fp ) ;
	return length ;
}

// Remove doubled backslashes
void DelDoubleBackSlash( char * st ) {
	int i=0,j ;
	while( st[i] != '\0' ) {
		if( (st[i] == '\\' )&&(st[i+1]=='\\' ) ) {
			for( j=i+1 ; j<strlen( st ) ; j++ ) st[j]=st[j+1] ;
		}
		else i++ ;
	}
}

// Add a string to a list of strings
int StringList_Add( char **list, const char * name ) {
	int i = 0 ;
	if( name == NULL ) return 1 ;
	while( list[i] != NULL ) {
		if( !stricmp( name, list[i] ) ) return 1 ;
		i++ ;
	}
	if( ( list[i] = (char*) malloc( strlen( name ) + 1 ) ) == NULL ) return 0 ;
	strcpy( list[i], name ) ;
	list[i+1] = NULL ;
	return 1 ;
}

// Remove a string from a list of strings
void StringList_Del( char **list, const char * name ) {
	int i = 0 ;
	while( list[i] != NULL ) {
		if( strlen( list[i] ) > 0 )
			if( !strcmp( list[i], name ) ) {
				strcpy( list[i], "" ) ;
			}
		i++;
	}
}

// Reorder a list of strings by moving the selected one up one place
void StringList_Up( char **list, const char * name ) {
	char *buffer ;
	int i = 0 ;
	while( list[i] != NULL ) {
		if( !strcmp( list[i], name ) ) {
			if( i > 0 ) {
				buffer=(char*)malloc( strlen(list[i-1])+1 ) ;
				strcpy( buffer, list[i-1] ) ;
				free( list[i-1] ) ; list[i-1] = NULL ;
				list[i-1]=(char*)malloc( strlen(list[i])+1 ) ;
				strcpy( list[i-1], list[i] ) ;
				free( list[i] ) ;
				list[i] = (char*) malloc( strlen( buffer ) +1 ) ;
				strcpy( list[i], buffer );
				free( buffer );
			}
			return ;
		}
		i++ ;
	}
}

// Set an environment variable
int putenv (const char *string) ;
int set_env( char * name, char * value ) {
	int res = 0 ;
	char * buffer = NULL ;
	if( (buffer = (char*) malloc( strlen(name)+strlen(value)+2 ) ) == NULL ) return -1 ;
	sprintf( buffer,"%s=%s", name, value ) ; 
	res = putenv( (const char *) buffer ) ;
	free( buffer ) ;
	return res ;
}

// Create a directory path recursively (dir1 / dir2 / ...)
int _mkdir (const char*);
int MakeDir( const char * directory ) {
	char buffer[MAX_VALUE_NAME], fullpath[MAX_VALUE_NAME], *p, *pst ;
	int i,j ;
	
	if( directory==NULL ) { return 1 ; } 
	if( strlen(directory)==0 ) { return 1 ; }

	for( i=0, j=0 ; i<=strlen(directory) ; i++,j++ ) { // remove the spaces after a '\'
		if( (directory[i]=='\\')||(directory[i]=='/') ) {
			fullpath[j]='\\' ;
			while( (directory[i+1]==' ')||(directory[i+1]=='	') ) i++ ;
		} else 
			fullpath[j]=directory[i] ;
	}
	fullpath[j+1]='\0' ;
		
	// remove the spaces, the / and the \\ at the end
	str_rtrim( fullpath, " \t/\\" ) ;

	for( i=strlen(fullpath), j=strlen(fullpath) ; i>=0 ; i--, j-- ) { // remove the spaces before a '\'
		if( fullpath[i] == '\\' ) {
			buffer[j]='\\' ;
			while( (i>0)&&((fullpath[i-1]==' ')||(fullpath[i-1]=='	')) ) i-- ;
		} else
			buffer[j]=fullpath[i] ;
	}
	j++;
		
	// remove the spaces at the start
	while( ((buffer+j)[0]==' ')||((buffer+j)[0]=='	') ) j++ ;
	strcpy( fullpath, buffer+j ) ;
	
	// create the directories
	if( !existdirectory(fullpath) ) {
		pst = fullpath ;
		while( (strlen(pst)>0)&&((p=strstr(pst,"\\"))!=NULL) ) {
			p[0]='\0' ;
			_mkdir( fullpath ) ;
			p[0]='\\' ;
			pst=p+1;
		}
		_mkdir( fullpath ) ;
	}
		
	return existdirectory(fullpath) ;
}
