/*
 * kitty_store.c - the file-based settings store used when KiTTY does not
 * save into the registry (savemode=dir / portable mode).
 * It works out where the configuration lives (current directory, the
 * program's own directory or an explicit configuration directory) and fills
 * in the session, host-key and jumplist paths; it packs and unpacks names so
 * they are legal file names. Session files themselves are read and written by
 * kitty_storage.c (ksf_load / ksf_save), which takes both line forms.
 */
#include <limits.h>


#include "kitty_store.h"
#include "kitty_common.h"
#include "kitty_tools.h"
#include "kitty_oldwin.h"   /* record what an older Windows does not have */
#include "kitty_msgbox.h"   /* themed MessageBox routing */
#include "kitty_text.h"     /* shared captions */
#include "kitty.h"
#include "kitty_registry.h"
#include "kitty_inikeys.h"   /* KI_*: the kitty.ini key names */
#include "kitty_params.h"    /* kitty_read_ini_dup */
#include "kitty_store_acl.h" /* the session folder: owner-only rights */


#ifndef snewn
#define snewn(n, type) ((type *)safemalloc((n), sizeof(type)))
#endif
#ifndef sfree
#define sfree safefree
#endif




char * ltoa (long int __val, char *__s, int __radix) ;
char * itoa (int __val, char *__s, int __radix)  ;
const char *win_strerror(int error) ;
/* The folder store's paths, each held whole (NULL until loadPath): a value
 * of putty.conf or kitty.ini, %VAR% expanded, used to be joined into 520
 * bytes with strcpy/strcat. */
char *seedpath = NULL;
char *sesspath = NULL;
char *initialsesspath = NULL;
char *sshkpath = NULL;
char *jumplistpath = NULL;
char *oldpath = NULL;
char sessionsuffix[16] = "\0";
char keysuffix[16] = "\0";

/* *slot takes over v (the old value freed) */
static void kstore_set(char **slot, char *v)
{
	sfree(*slot);
	*slot = v;
}

/* The current directory whole (sfree it); "" when it cannot be read. */
static char *kstore_cwd_dup(void)
{
	DWORD need = GetCurrentDirectoryA(0, NULL), n;
	int tries;
	for (tries = 0; need && tries < 4; tries++) {
		char *buf = snewn(need, char);
		n = GetCurrentDirectoryA(need, buf);
		if (n == 0)
			break;
		if (n < need)
			return buf;
		sfree(buf);
		need = n;
	}
	return dupstr("");
}

/* KiTTY (hknet/KiTTY#56): what putty.conf actually held, kept raw for the
 * one-time move into kitty.ini (kitty_store_ini_takeover below), plus the
 * paths loadPath() falls back to and resolves relative names against. */
#define KSTORE_PC_SESSIONS      1
#define KSTORE_PC_SESSIONSUFFIX 2
#define KSTORE_PC_SSHHOSTKEYS   4
#define KSTORE_PC_KEYSUFFIX     8
static int kstore_pc_found = 0;
static char *kstore_pc_sessions = NULL;          /* each whole, NULL = none */
static char *kstore_pc_sshhostkeys = NULL;
static char kstore_pc_sessionsuffix[64] = "";
static char kstore_pc_keysuffix[64] = "";
static char *kstore_basepath = NULL;
static char *kstore_default_sesspath = NULL;
static void kstore_pc_keep(char *dst, size_t len, const char *raw, int bit)
{
	snprintf(dst, len, "%s", raw);
	str_rtrim(dst, " \n\r\t");
	kstore_pc_found |= bit;
}
static void kstore_pc_keep_dup(char **dst, const char *raw, int bit)
{
	kstore_set(dst, dupstr(raw));
	str_rtrim(*dst, " \n\r\t");
	kstore_pc_found |= bit;
}

static const char hex[16] = "0123456789ABCDEF";

void fatal_error(const char *p, ...) { exit(0) ; }
/*
void *safemalloc(size_t n, size_t size) {
    void *p;

    if (n > INT_MAX / size) {
	p = NULL;
    } else {
	size *= n;
	if (size == 0) size = 1;
#ifdef MINEFIELD
	p = minefield_c_malloc(size);
#else
	p = malloc(size);
#endif
    }

    if (!p)
       fatal_error("out of memory");

    return p;
}
void safefree(void *ptr)
{
    if (ptr) {
#ifdef MINEFIELD
	minefield_c_free(ptr);
#else
	free(ptr);
#endif
    }
}
*/

/* JK: my generic function for simplyfing error reporting */
DWORD errorShow(const char* pcErrText, const char* pcErrParam) {
	HWND hwRodic;
	DWORD erChyba;
	char pcBuf[16];
	char* pcHlaska = snewn((pcErrParam?strlen(pcErrParam):0) + strlen(pcErrText) + 256 + 2*MAX_PATH, char);
	
	erChyba = GetLastError();		
	ltoa(erChyba, pcBuf, 10);

	strcpy(pcHlaska, KT_STORE_ERR_PREFIX);
	strcat(pcHlaska, pcErrText);
	strcat(pcHlaska, "\n");
	strcat(pcHlaska, KT_STORE_ERR_DIRECTORY);
	{
		/* the folder whole; the message grows to hold it */
		char *currpath = kstore_cwd_dup();
		pcHlaska = sresize(pcHlaska, strlen(pcHlaska) + strlen(currpath) +
		                   (pcErrParam?strlen(pcErrParam):0) + 256, char);
		strcat(pcHlaska, currpath);
		sfree(currpath);
	}
	strcat(pcHlaska, "\n");

	if (pcErrParam) {
		strcat(pcHlaska, pcErrParam);
		strcat(pcHlaska, "\n");
	}
    strcat(pcHlaska, KT_STORE_ERR_CODE);
	strcat(pcHlaska, pcBuf);

    /* JK: get parent-window and show */
    hwRodic = GetActiveWindow();
    if (hwRodic != NULL) { hwRodic = GetLastActivePopup(hwRodic);}
  
	if (MessageBox(hwRodic, pcHlaska, KT_CAP_ERROR, MB_OK|MB_APPLMODAL|MB_ICONEXCLAMATION) == 0) {
        /* JK: this is really bad -> just ignore */
	sfree(pcHlaska);
        return 0;
    }
    
    //MessageBox(GetActiveWindow(),pcHlaska, "Error", MB_OK|MB_APPLMODAL|MB_ICONEXCLAMATION) ;
    

	sfree(pcHlaska);
	return erChyba;
};

/* JK: pack string for use as filename - pack < > : " / \ | */
void packstr(const char *in, char *out) {
EMERGENCY_INIT
    while (*in) {
EMERGENCY_BREAK
		if (*in == '<' || *in == '>' || *in == ':' || *in == '"' ||
	    *in == '/' || *in == '|') {
	    *out++ = '%';
	    *out++ = hex[((unsigned char) *in) >> 4];
	    *out++ = hex[((unsigned char) *in) & 15];
	} else
	    *out++ = *in;
	in++;
    }
    *out = '\0';
    return;
}

/*
 * JK: create directory if specified as dir1\dir2\dir3 and dir1|2 doesn't exists
 * handle if part of path already exists
*/
int createPath(char* dir) {
    char *p;
	if( GetReadOnlyFlag() ) { return 1 ; }
	p = strrchr(dir, '\\');

	if (p == NULL) {
		/* what if it already exists */
		if (!SetCurrentDirectory(dir)) {
			CreateDirectory(dir, NULL);
			return SetCurrentDirectory(dir);
		}
		return 1;
	}
	
	*p = '\0';
	if( !createPath(dir) ) { MessageBox(NULL,KT_STORE_MKDIR_FAILED_BANG,KT_CAP_ERROR,MB_OK|MB_ICONERROR) ; }
	*p = '\\';
	++p;
	/* what if it already exists */
	if (!SetCurrentDirectory(dir)) {
		CreateDirectory(p, NULL);
		return SetCurrentDirectory(p) ;
	}
	return 1;
}

/*
 * JK: join path pcMain.pcSuf solving extra cases, the result whole (sfree
 * it); pcMain as path from WinAPI ::GetCurrentDirectory()/GetModuleFileName(),
 * pcSuf as user input path from config. The expansion is whole too: one
 * longer than MAX_PATH was taken as done with the buffer never filled.
*/
static char* joinPath(const char* pcMain, const char* pcSuf) {

	char* pcBuf = kitty_expand_env_strict_dup(pcSuf);
	char* pcDest;

	/* at first ExpandEnvironmentStrings */
	if (pcBuf == NULL) {
		/* JK: failure -> revert back - but it ussualy won't work, so report error to user! */
		errorShow(KT_STORE_EXPAND_ENV_FAILED, pcSuf);
		pcBuf = dupstr(pcSuf);
	}
	/* now ExpandEnvironmentStringsForUser - only on win2000Pro and above */
	/* It's much more tricky than I've expected, so it's ToDo */
	/*
	static HMODULE userenv_module = NULL;
	typedef BOOL (WINAPI *p_ExpandESforUser_t) (HANDLE, LPCTSTR, LPTSTR, DWORD);
	static p_ExpandESforUser_t p_ExpandESforUser = NULL;
	
	HMODULE userenv_module = LoadLibrary("USERENV.DLL");

	if (userenv_module) {
	    p_ExpandESforUser = (p_ExpandESforUser_t) kitty_api_from(shell32_module, "shell32.dll", "ExpandEnvironmentStringsForUserA", KITTY_API_OPTIONAL,
                                  "expanding %USERPROFILE% for the logged-on user");
		
		if (p_ExpandESforUser) {

			TOKEN_IMPERSONATE

			if (0 == (p_ExpandESforUser(NULL, pcSuf, pcBuf,	MAX_PATH))) {
	    		*//* JK: failure -> revert back - but it ussualy won't work, so report error to user! *//*
				errorShow(KT_STORE_EXPAND_ENV_USER_FAILED, pcBuf);
				strncpy(pcSuf, pcBuf, strlen(pcSuf));
			}
		}
	}*/

	/* expand done, resutl in pcBuf */

	if ((*pcBuf == '/') || (*pcBuf == '\\')) {
		/* everything ok */
		pcDest = dupcat(pcMain, pcBuf);
	}
	else {
		if (*pcBuf && *(pcBuf+1) == ':') {
			/* absolute path */
			pcDest = dupstr(pcBuf);
		}
		else {
			/* some weird relative path - add '\' */
			pcDest = dupcat(pcMain, "\\", pcBuf);
		}
	}
	sfree(pcBuf);
	return pcDest;
}

extern char *GetKittyIniFile(void);

/*
 * JK: init path variables from config or otherwise
 * as of 1.5 GetModuleFileName solves our currentDirectory problem
*/
int loadPath(void) {
	if( sesspath && *sesspath != '\0') { return 0 ; }

	char *fileCont = NULL;
	DWORD fileSize;
	DWORD bytesRead;
	char *p = NULL;
	char *p2 = NULL;
	HANDLE hFile ;
	const char *base;

	/* every path whole: the program's folder could be cut to another one in
	 * a code page whose bytes outnumber its characters */
	char* puttypath = kitty_module_dir_dup(NULL);

	/* JK:  save path/curdir */
	kstore_set(&oldpath, kstore_cwd_dup());

	/* KiTTY: putty.conf is read beside the exe only. Classic KiTTY looked in
	 * the working directory first, so a shortcut with another "Start in", or
	 * a script run from another folder, fed it whatever putty.conf sat there:
	 * its paths were copied into kitty.ini once on a copy not yet migrated,
	 * and acted on at every start where kitty.ini cannot be written. */
	if (puttypath != NULL)
		SetCurrentDirectory(puttypath);
	else puttypath = kstore_cwd_dup();

	/* JK: set default values - if there is a config file, it will be overwitten */
	base = GetConfigDirectory() != NULL ? GetConfigDirectory() : puttypath ;   // another config directory is set
	kstore_set(&sesspath, dupprintf("%s\\Sessions", base));
	kstore_set(&initialsesspath, dupstr(sesspath));
	kstore_set(&sshkpath, dupprintf("%s\\SshHostKeys", base));
	kstore_set(&jumplistpath, dupprintf("%s\\Jumplist", base));
	kstore_set(&seedpath, dupprintf("%s\\putty.rnd", base));
	/* KiTTY: kept for kitty_store_ini_takeover(), which resolves kitty.ini's
	 * paths the way putty.conf's are resolved below. */
	kstore_set(&kstore_basepath, dupstr(puttypath));
	kstore_set(&kstore_default_sesspath, dupstr(sesspath));
	kstore_pc_found = 0;

	/* Once kitty_store_ini_takeover() has moved putty.conf's keys into
	 * kitty.ini (puttyconfmigrated=yes), kitty.ini is the one source and
	 * putty.conf is not opened at all. */
	{
		const char *ini = GetKittyIniFile() ;
		char mig[8] ;
		if( ini && ini[0] && readINI( ini, INIT_SECTION, KI_PUTTYCONFMIGRATED, mig, sizeof(mig) ) && !stricmp( mig, "yes" ) )
			hFile = INVALID_HANDLE_VALUE ;
		else
			hFile = CreateFile("putty.conf",GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
	}
	/* Test Sessions directory (readonly=yes: not created) */
	if(get_param("INIFILE")==SAVEMODE_DIR && !GetReadOnlyFlag())
	if( !existdirectory(sesspath) ) {
		if( !kitty_store_acl_mkdir(sesspath) && !MakeDir(sesspath) ) {
			MessageBox( NULL, KT_STORE_SESSDIR_FAILED, KT_CAP_ERROR, MB_OK|MB_ICONERROR ) ;
		}
	}

	/* JK: now we can pre-clean-up */
	SetCurrentDirectory(oldpath);

	if (hFile != INVALID_HANDLE_VALUE) {
		fileSize = GetFileSize(hFile, NULL);
		fileCont = snewn(fileSize+16, char);

		if (!ReadFile(hFile, fileCont, fileSize, &bytesRead, NULL))
		{
			errorShow(KT_STORE_CONF_READ_FAILED, NULL);
			/* JK: default values are already there and clean-up at end */
		}
		else {
			/* JK: parse conf file to path variables */
			*(fileCont+fileSize+1) = '\0';
			*(fileCont+fileSize) = '\n';
			p = fileCont;
EMERGENCY_INIT
			while (p) {
EMERGENCY_BREAK
				if (*p == ';') {	/* JK: comment -> skip line */
					p = strchr(p, '\n');
					++p;
					continue;
				}
				p2 = strchr(p, '=');
				if (!p2) break;
				*p2 = '\0';
				++p2;

				if (!strcmp(p, "Sessions")) {
					p = strchr(p2, '\n');
					*p = '\0';
					kstore_pc_keep_dup(&kstore_pc_sessions, p2, KSTORE_PC_SESSIONS);
					kstore_set(&sesspath, joinPath(puttypath, p2));
					str_rtrim(sesspath, " \n\r\t");
				}
				else if (!strcmp(p, "SshHostKeys")) {
					p = strchr(p2, '\n');
					*p = '\0';
					kstore_pc_keep_dup(&kstore_pc_sshhostkeys, p2, KSTORE_PC_SSHHOSTKEYS);
					kstore_set(&sshkpath, joinPath(puttypath, p2));
					str_rtrim(sshkpath, " \n\r\t");
				}
				else if (!strcmp(p, "Jumplist")) {
					p = strchr(p2, '\n');
					*p = '\0';
					kstore_set(&jumplistpath, joinPath(puttypath, p2));
					str_rtrim(jumplistpath, " \n\r\t");
				}
				else if (!strcmp(p, "seedfile")) {
					p = strchr(p2, '\n');
					*p = '\0';
					kstore_set(&seedpath, joinPath(puttypath, p2));
					str_rtrim(seedpath, " \n\r\t");
				}
				else if (!strcmp(p, "sessionsuffix")) {
					p = strchr(p2, '\n');
					*p = '\0';
					kstore_pc_keep(kstore_pc_sessionsuffix, sizeof(kstore_pc_sessionsuffix), p2, KSTORE_PC_SESSIONSUFFIX);
					snprintf(sessionsuffix, sizeof(sessionsuffix), "%s", p2);
					str_rtrim( sessionsuffix, " \n\r\t" ) ;
				}
				else if (!strcmp(p, "keysuffix")) {
					p = strchr(p2, '\n');
					*p = '\0';
					kstore_pc_keep(kstore_pc_keysuffix, sizeof(kstore_pc_keysuffix), p2, KSTORE_PC_KEYSUFFIX);
					snprintf(keysuffix, sizeof(keysuffix), "%s", p2);
					str_rtrim( keysuffix, " \n\r\t" ) ;
				}
				++p;
			}
		}
		CloseHandle(hFile);
		sfree(fileCont);
	}
	/* else - INVALID_HANDLE {
		 * JK: unable to read conf file - probably doesn't exists
		 * we won't create one, user wants putty light, just fall back to defaults
		 * and defaults are already there
	}*/

	sfree(puttypath);
	return 1;
}
char * SetSessPath( const char * dec ) {
	int i ;
	char *pst ;
	if( !sesspath || !initialsesspath ) return "" ;
	if( !strcmp(dec,"..") ) {
		if( strcmp(sesspath, initialsesspath) ) {
			i=strlen(sesspath)-1 ;
			while( (sesspath[i]!='\\') && (i>0) ) i--;
			sesspath[i]='\0';
		}
	} else {
		kstore_set( &sesspath, dupcat( sesspath, "\\", dec ) ) ;
	}
	CleanFolderName( sesspath ) ;
	pst = sesspath+strlen(initialsesspath ) ;
	while( pst[0]=='\\' ) pst++ ;
	return pst ;
}

/*
 * KiTTY (hknet/KiTTY#56): the folder store's four path settings move from
 * classic KiTTY's putty.conf into kitty.ini [KiTTY] - sessions, sshhostkeys,
 * and the file-name endings under old KiTTY's own key names, fileextension
 * (putty.conf's sessionsuffix) and hostkeyextension (its keysuffix). Called
 * once per start in savemode=dir, after loadPath() and before the store is
 * switched on.
 *
 * 0.85.1.13 wrote the endings as sessionsuffix / keysuffix: each is copied
 * into fileextension / hostkeyextension unless that one is set already, and
 * removed - one key per ending from then on.
 *
 * ONCE: each key putty.conf has and kitty.ini lacks is copied over, and
 * puttyconfmigrated=yes records that it happened, so a key later edited or
 * removed in kitty.ini is never copied back. putty.conf itself is left as it
 * is, for older copies of KiTTY that still read it. Jumplist= and seedfile=
 * are not carried over (Windows keeps the jump list now; the seed file has no
 * use here).
 *
 * After the move kitty.ini is the one source: a key it does not have means
 * the default, whatever putty.conf holds. Where kitty.ini cannot be written
 * (conf=no, readonly=yes) nothing moves and putty.conf keeps working, with a
 * key kitty.ini does carry still winning.
 */
extern int GetNoKittyFileFlag(void);
extern int writeINI(const char *filename, const char *section, const char *key, const char *value);
extern int delINI(const char *filename, const char *section, const char *key);
void kitty_set_session_suffix(const char *suffix);
void kitty_set_hostkey_dir(const char *dir);
void kitty_set_hostkey_suffix(const char *suffix);

/* A kitty.ini [KiTTY] value whole (sfree it); NULL when the key is absent
 * or empty. */
static char *kstore_ini_dup( const char *ini, const char *key ) {
	char *v = ( ini && ini[0] ) ? kitty_read_ini_dup( ini, INIT_SECTION, key ) : NULL ;
	if( v && !v[0] ) { sfree( v ) ; v = NULL ; }
	return v ;
}

int kitty_store_ini_takeover(void) {
	const char *ini = GetKittyIniFile() ;
	char v[64], *pv ;
	int can_write = ini && ini[0] && !GetNoKittyFileFlag() && !GetReadOnlyFlag() ;
	int migrated = 0, moved_now = 0, ini_only ;
	const char *suffix = "", *hkdir = "", *keysuf = "" ;
	static char suffix_buf[64], keysuf_buf[64], *hkdir_buf = NULL ;

	if( ini && ini[0] && readINI( ini, INIT_SECTION, KI_PUTTYCONFMIGRATED, v, sizeof(v) ) && !stricmp( v, "yes" ) )
		migrated = 1 ;
	if( !migrated && can_write && kstore_pc_found ) {
		const struct { int bit; const char *key; const char *val; } keys[] = {
			{ KSTORE_PC_SESSIONS, KI_SESSIONS, kstore_pc_sessions ? kstore_pc_sessions : "" },
			{ KSTORE_PC_SESSIONSUFFIX, KI_FILEEXTENSION, kstore_pc_sessionsuffix },
			{ KSTORE_PC_SSHHOSTKEYS, KI_SSHHOSTKEYS, kstore_pc_sshhostkeys ? kstore_pc_sshhostkeys : "" },
			{ KSTORE_PC_KEYSUFFIX, KI_HOSTKEYEXTENSION, kstore_pc_keysuffix },
		} ;
		int i, all = 1 ;
		/* An EMPTY key in kitty.ini (a template line such as "sessions=")
		 * holds nothing either: putty.conf's value is carried over it. The
		 * move counts as done only when every key arrived: it was marked
		 * done regardless and a key that failed to write was then neither
		 * in kitty.ini nor read from putty.conf any more. */
		for( i = 0 ; i < (int)(sizeof(keys)/sizeof(keys[0])) ; i++ )
			if( (kstore_pc_found & keys[i].bit) ) {
				char *have = kstore_ini_dup( ini, keys[i].key ) ;
				if( !have && !writeINI( ini, INIT_SECTION, keys[i].key, keys[i].val ) )
					all = 0 ;
				sfree( have ) ;
			}
		if( all && writeINI( ini, INIT_SECTION, KI_PUTTYCONFMIGRATED, "yes" ) )
			migrated = moved_now = 1 ;   /* the caller reports it (kitty_startup.c) */
	}
	/* 0.85.1.13's names for the endings: into old KiTTY's keys, then gone */
	if( can_write ) {
		static const char *const renames[2][2] = {
			{ KI_SESSIONSUFFIX, KI_FILEEXTENSION },
			{ KI_KEYSUFFIX, KI_HOSTKEYEXTENSION },
		} ;
		int i ;
		for( i = 0 ; i < 2 ; i++ )
			if( ( pv = kitty_read_ini_dup( ini, INIT_SECTION, renames[i][0] ) ) != NULL ) {
				char cur[64] ;
				str_rtrim( pv, " \n\r\t" ) ;
				/* the old name goes only once its value is under the new one */
				if( pv[0] && ( !readINI( ini, INIT_SECTION, renames[i][1], cur, sizeof(cur) ) || !cur[0] ) &&
				    !writeINI( ini, INIT_SECTION, renames[i][1], pv ) ) {
					sfree( pv ) ;
					continue ;
				}
				sfree( pv ) ;
				delINI( ini, INIT_SECTION, renames[i][0] ) ;
			}
	}
	/* kitty.ini alone: after the move, or when there is no putty.conf key to
	 * compete with. Otherwise putty.conf's values stand where kitty.ini is
	 * silent. */
	ini_only = migrated || !kstore_pc_found ;

	if( ( pv = kstore_ini_dup( ini, KI_SESSIONS ) ) != NULL ) {
		/* the folder whole: it was cut to 520 bytes, and a long expansion
		 * left joinPath's buffer unfilled */
		char *path = joinPath( kstore_basepath ? kstore_basepath : "", pv ) ;
		sfree( pv ) ;
		str_rtrim( path, " \n\r\t\\" ) ;
		/* "C:\" trimmed is "C:", which means the current folder of drive C:
		 * - the root keeps its backslash. */
		if( strlen( path ) == 2 && path[1] == ':' ) { char *r = dupcat( path, "\\" ) ; sfree( path ) ; path = r ; }
		kstore_set( &sesspath, path ) ;
		kstore_set( &initialsesspath, dupstr( sesspath ) ) ;
	} else if( ini_only && (kstore_pc_found & KSTORE_PC_SESSIONS) && kstore_default_sesspath && kstore_default_sesspath[0] ) {
		kstore_set( &sesspath, dupstr( kstore_default_sesspath ) ) ;
		kstore_set( &initialsesspath, dupstr( sesspath ) ) ;
	}
	if( !GetReadOnlyFlag() && sesspath && sesspath[0] && !existdirectory( sesspath ) )
		if( !kitty_store_acl_mkdir( sesspath ) ) MakeDir( sesspath ) ;

	/* the endings read as old KiTTY read them: a dot in front if missing.
	 * Where kitty.ini could not be written, a 0.85.1.13 name still counts. */
	if( ini && ini[0] && ( ( readINI( ini, INIT_SECTION, KI_FILEEXTENSION, suffix_buf, sizeof(suffix_buf) ) && suffix_buf[0] ) ||
	                       readINI( ini, INIT_SECTION, KI_SESSIONSUFFIX, suffix_buf, sizeof(suffix_buf) ) ) ) {
		kitty_ext_dot( suffix_buf, sizeof(suffix_buf) ) ;
		suffix = suffix_buf ;
	} else if( !ini_only )
		suffix = kstore_pc_sessionsuffix ;

	if( ( pv = kstore_ini_dup( ini, KI_SSHHOSTKEYS ) ) != NULL ) {
		/* the host-key folder - the trust store - whole */
		kstore_set( &hkdir_buf, joinPath( kstore_basepath ? kstore_basepath : "", pv ) ) ;
		sfree( pv ) ;
		str_rtrim( hkdir_buf, " \n\r\t" ) ;
		hkdir = hkdir_buf ;
	} else if( !ini_only && (kstore_pc_found & KSTORE_PC_SSHHOSTKEYS) && sshkpath ) {
		hkdir = sshkpath ;   /* putty.conf's, resolved by loadPath() */
	}

	if( ini && ini[0] && ( ( readINI( ini, INIT_SECTION, KI_HOSTKEYEXTENSION, keysuf_buf, sizeof(keysuf_buf) ) && keysuf_buf[0] ) ||
	                       readINI( ini, INIT_SECTION, KI_KEYSUFFIX, keysuf_buf, sizeof(keysuf_buf) ) ) ) {
		kitty_ext_dot( keysuf_buf, sizeof(keysuf_buf) ) ;
		keysuf = keysuf_buf ;
	} else if( !ini_only )
		keysuf = kstore_pc_keysuffix ;

	kitty_set_session_suffix( suffix ) ;
	kitty_set_hostkey_dir( hkdir ) ;
	kitty_set_hostkey_suffix( keysuf ) ;
	/* fileextension is also the registered file type, the file dialogs'
	 * filter and the export bundles' ending (FileExtension, read before
	 * this): the same value, also when it was copied over just now */
	if( suffix[0] && strlen( suffix ) < sizeof(FileExtension) )
		snprintf( FileExtension, sizeof(FileExtension), "%s", suffix ) ;
	return moved_now ;
}

/*
 * KiTTY (hknet/KiTTY#56): the host-key files renamed to the ending in force
 * (hostkeyextension), once each time it changes - from no ending, or from
 * the one recorded in hostkeyextensionapplied - so the folder matches what
 * is read. A file whose new name already exists is left as it is (the
 * suffixed one wins); the store's reader also takes a file without the
 * ending and renames it (kitty_storage.c), for files that arrive later.
 * After the store is switched on; nothing happens where kitty.ini cannot be
 * written, since the change could not be recorded. Returns the number of
 * files renamed; the names left as they are go into `clashes` (may be NULL),
 * one per line, as far as they fit. dry_run counts what would be renamed and
 * changes nothing, the record included. The callers - the panel's Apply and
 * the configuration box's start-up question - show both.
 */
const char *kitty_hostkey_suffix(void);
char *portable_subdir_path(const char *subdir);
static int kstore_ends_with( const char *s, const char *end ) {
	size_t ls = strlen( s ), le = strlen( end ) ;
	return le && ls > le && !stricmp( s + ls - le, end ) ;
}

/* The ending last applied (hostkeyextensionapplied, fileextensionapplied):
 * "" when none is recorded. */
void kitty_store_ending_applied( const char *key, char *buf, size_t n ) {
	const char *ini = GetKittyIniFile() ;
	if( !n ) return ;
	buf[0] = '\0' ;
	if( ini && ini[0] && !GetNoKittyFileFlag() )
		readINI( ini, INIT_SECTION, key, buf, (int)n ) ;
	str_rtrim( buf, " \n\r\t" ) ;
}
/* Record `now` as applied (the files renamed to it, or the rename declined). */
void kitty_store_ending_mark( const char *key, const char *now ) {
	const char *ini = GetKittyIniFile() ;
	if( !ini || !ini[0] || GetNoKittyFileFlag() || GetReadOnlyFlag() ) return ;
	if( now && now[0] ) writeINI( ini, INIT_SECTION, key, now ) ;
	else delINI( ini, INIT_SECTION, key ) ;
}

int kitty_store_hostkey_rename( int dry_run, strbuf *clashes ) {
	const char *ini = GetKittyIniFile() ;
	const char *now = kitty_hostkey_suffix() ;
	char was[64] = "", *pat ;
	char *dir ;
	int done = 0 ;
	WIN32_FIND_DATAA fd ;
	HANDLE h ;

	if( !ini || !ini[0] || GetNoKittyFileFlag() || GetReadOnlyFlag() ) return 0 ;
	readINI( ini, INIT_SECTION, KI_HOSTKEYEXTENSIONAPPLIED, was, sizeof(was) ) ;
	str_rtrim( was, " \n\r\t" ) ;
	if( !stricmp( was, now ) ) return 0 ;
	if( !( dir = portable_subdir_path( "SshHostKeys" ) ) ) return 0 ;
	/* every name whole, and every clash listed: the list was cut where it
	 * filled its buffer */
	pat = dupprintf( "%s\\*", dir ) ;
	h = FindFirstFileA( pat, &fd ) ;
	sfree( pat ) ;
	if( h != INVALID_HANDLE_VALUE ) {
		do {
			char *base, *from, *to ;
			if( fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ) continue ;
			if( now[0] && kstore_ends_with( fd.cFileName, now ) ) continue ;   /* already right */
			base = dupstr( fd.cFileName ) ;
			if( was[0] && kstore_ends_with( base, was ) )
				base[strlen( base ) - strlen( was )] = '\0' ;
			from = dupprintf( "%s\\%s", dir, fd.cFileName ) ;
			to = dupprintf( "%s\\%s%s", dir, base, now ) ;
			sfree( base ) ;
			/* the new name taken already: that file wins, this one stays */
			if( stricmp( from, to ) ) {
				if( GetFileAttributesA( to ) == INVALID_FILE_ATTRIBUTES &&
				    ( dry_run || MoveFileA( from, to ) ) )
					done++ ;
				else if( clashes ) {
					if( clashes->len ) put_byte( clashes, '\n' ) ;
					put_dataz( clashes, fd.cFileName ) ;
				}
			}
			sfree( from ) ;
			sfree( to ) ;
		} while( FindNextFileA( h, &fd ) ) ;
		FindClose( h ) ;
	}
	sfree( dir ) ;
	if( !dry_run )
		kitty_store_ending_mark( KI_HOSTKEYEXTENSIONAPPLIED, now ) ;
	return done ;
}

bool ReadPortableValue(const char *buffer, const char * name, char * value, const int maxlen) {
	bool test = false ;
	if( (buffer==NULL) || (name==NULL) ) { return false ; }
	if( (strlen(buffer)==0) || (strlen(name)==0) ) { return false ; }
	if( maxlen<=0 ) { return false ; }
	char *b = (char*)malloc(strlen(name)+2), *pst ;
	sprintf(b,"%s\\",name) ;
	if( strstr(buffer,b)==buffer ) {
		size_t n ;
		pst = (char*)buffer + strlen(b) ;
		test = true ;
		value[0]='\0';
		/* "value\" then the line's end. The line end is taken off FIRST: a
		 * file's last line may have none (a file written by hand or by
		 * another tool), and the old test looked for the closing '\' one
		 * character before the end - such a line read as empty, a proxy's
		 * port as 0. CR too, for a file copied with CRLF line ends. */
		n = strlen(pst) ;
		while( n>0 && (pst[n-1]=='\n' || pst[n-1]=='\r') ) n-- ;
		if( n>0 && pst[n-1]=='\\' && (int)(n-1)<maxlen ) {
			memcpy( value, pst, n-1 ) ;
			value[n-1] = '\0' ;
		}
		unmungestr( value, value, MAX_VALUE_NAME ) ;
	}
	free(b) ;
	return test ;
}
