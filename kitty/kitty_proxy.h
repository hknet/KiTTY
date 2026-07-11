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
void kitty_proxy_resolve_selection( Conf *conf ) ;
int SaveProxyInfo( Conf *conf, const char *name ) ;
int DeleteProxyInfo( const char *name ) ;
int kitty_proxy_any_plaintext_password( void ) ;
void kitty_migrate_old_proxies( void ) ;
int kitty_export_proxies_to_dir( const char *dir ) ;   /* Piece 7 */
int kitty_import_proxies_from_dir( const char *dir ) ;
int kitty_proxy_edit_dialog( HWND owner ) ;   /* kitty_proxy_gui.c */
void SetProxySelectionFlag( const int flag ) ;
int LoadProxyInfo( Conf * conf, const char * name ) ;
void InitProxyList(void) ;
int LoadProxyInfo( Conf * conf, const char * name ) ;
#endif
