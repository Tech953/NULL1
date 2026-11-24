#ifndef TRACKPORT_PLATFORM_H
#define TRACKPORT_PLATFORM_H
#include <stdio.h>
#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <process.h>
typedef SOCKET Conn;
typedef int SockLen;
#define INVALID_CONN INVALID_SOCKET
#define close_conn closesocket
#define text_cmp _strnicmp
#define format_text _snprintf
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <strings.h>
typedef int Conn;
typedef socklen_t SockLen;
#define INVALID_CONN (-1)
#define close_conn close
#define text_cmp strncasecmp
#define format_text snprintf
#endif
int platform_start(void);
void secure_socket(Conn c);
void socket_deadlines(Conn c);
int start_worker(Conn c, void (*handler)(Conn));
void open_browser(const char *url);
/* Proxy settings for an outbound fetch. url is a bare scheme + authority
   (http://, https://, socks5:// or socks5h://); user/pass are optional.
   The settings are applied to curl's proxy options (CURLOPT_PROXY and
   CURLOPT_PROXYUSERPWD), so a SOCKS5 proxy is honoured by the same code
   path as an HTTP proxy. An empty url means "no proxy". */
struct Proxy { const char *url, *user, *pass; };
/* A completed HTTP response is staged in a temporary file. The final three
   bytes are curl's HTTP status trailer and are excluded from served audio. */
int fetch_file(Conn client, const char *url, int limit, int catalog,
               const struct Proxy *proxy, FILE **out, int *length, int *http);
#endif
