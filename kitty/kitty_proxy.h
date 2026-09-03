#ifndef KITTY_PROXY
#define KITTY_PROXY

#define MAX_PROXY 100
struct Proxies {
	char *name;
	int val;
};
extern struct Proxies proxies[MAX_PROXY] ;
    
int GetProxySelectionFlag() ;
int kitty_has_proxy_definitions( void ) ;
int kitty_proxy_choice_shown( void ) ;
int kitty_proxy_editor_available( void ) ;
/* The Application tab's "Named proxies" panel (design 9.3b): the same
 * named-proxy definitions, as an ordinary config-box panel. */
struct controlbox ;
void kitty_proxy_build_panel( struct controlbox *b ) ;
void kitty_proxy_panel_preselect( const char *name ) ;
void kitty_proxy_resolve_selection( Conf *conf ) ;
int SaveProxyInfo( Conf *conf, const char *name ) ;
int DeleteProxyInfo( const char *name ) ;
void kitty_migrate_old_proxies( void ) ;
int kitty_export_proxies_to_dir( const char *dir ) ;   /* Piece 7 */
int kitty_import_proxies_from_dir( const char *dir, int overwrite, int *skippedOut ) ;
int kitty_proxy_name_exists( const char *name ) ;
int kitty_proxies_dir_collisions( const char *dir ) ;
/* How a named proxy's HOST must be read. Stored per definition as ProxyHostIs;
 * absent means "follow kitty.ini [KiTTY] namedproxy", which itself defaults to
 * the saved-session-first behaviour PuTTY has always had. */
#define KITTY_PROXYHOST_FOLLOW   (-1)
#define KITTY_PROXYHOST_SESSION  0    /* may be a saved session (the old way) */
#define KITTY_PROXYHOST_HOSTNAME 1    /* a hostname, full stop */
int kitty_proxy_host_kind( const char *name ) ;   /* one of the three above */
int kitty_named_proxy_default_hostname( void ) ;  /* the global, 0/1 */

void SetProxySelectionFlag( const int flag ) ;
int LoadProxyInfo( Conf * conf, const char * name ) ;
void InitProxyList(void) ;
int LoadProxyInfo( Conf * conf, const char * name ) ;
#endif
