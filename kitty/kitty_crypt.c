#include "kitty_crypt.h"

int cryptstring( const int mode, char * st, const char * key ) {
	if( mode>1 ) return strlen(st);
	if( strlen(st)==0 ) { return 0 ; }
	return bcrypt_string_base64( st, st, strlen( st ), key, 0 ) ; 
}

int decryptstring( const int mode, char * st, const char * key ) { 
	if( mode>1 ) return strlen(st);
	if( strlen(st)==0 ) { return 0 ; }
	int res = buncrypt_string_base64( st, st, strlen( st ), key ) ; 
	if( res == 0 ) strcpy( st, "" ) ;
	return res ;
	}

void dopasskey( int mode, char * passkey, const char * host, const char * termtype ) {
    if( mode > 0 ) {
	strcpy( passkey, "KiTTY" ) ;
    } else {
	if( strlen(host)>0 ) {
		if( (strlen(host)+strlen(termtype)) < 1000 ) { 
			sprintf( passkey, "%s%sKiTTY", host, termtype ) ;
		} else {
			strcpy( passkey, "" ) ;
		}
	} else { 
		if( strlen(termtype) < 1000 ) { 
			sprintf( passkey, "%sKiTTY", termtype ) ;
		} else {
			strcpy( passkey, "" ) ;
		}
	}
    }
}

int cryptpassword( const int mode, char * password, const char * host, const char * termtype ) {
	char PassKey[1024] = "" ;
	int ret ;
	dopasskey( mode, PassKey, host, termtype ) ;
	ret = cryptstring( mode, password, PassKey ) ;
	return ret ;
}

int decryptpassword( const int mode, char * password, const char * host, const char * termtype ) {
	char PassKey[1024] = "" ;
	int ret ;
	dopasskey( mode, PassKey, host, termtype ) ;
	ret = decryptstring( mode, password, PassKey ) ;
	return ret ;
}

//static char MASKKEY[128] = MASTER_PASSWORD ;
static char MASKKEY[128] = "¤¥©ª³¼½¾" ;

void MASKPASS( const int mode, char * password ) {
	if( mode > 0 ) return ;
	if( password==NULL ) return ;
	if( strlen(password)==0) return ;
	
	int i,j=0, len=strlen(password) ;
	char c, *buffer ;
	buffer=(char*)malloc(strlen(password)+1);
	buffer[0]='\0' ;
	if( (len>0)&&(strlen(MASKKEY)>0) )
	for( i=0 ; i<len ; i++ ) {
		c = password[i]^MASKKEY[j] ;
		if( c==0 ) { free(buffer) ; return ; }
		buffer[i]=c ; buffer[i+1] = '\0' ;
		j++ ; if(MASKKEY[j]=='\0') j=0 ;
		}
	strcpy( password, buffer ) ;
	memset(buffer,0,strlen(password) );
	free(buffer) ;
}
