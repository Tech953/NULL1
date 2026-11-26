#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "platform.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#ifndef _WIN32
#include <sys/select.h>
#include <stdatomic.h>
#endif
#include "assets.inc"

#define REQUEST_CAP 16384
#define URL_CAP 12288
#define AUDIO_LIMIT (256*1024*1024)
#define JSON_LIMIT (1024*1024)
static char authority[64],origin[84];
#ifdef _WIN32
static volatile LONG workers=0,stopping=0;
#ifdef __TINYC__
/* TinyCC's Windows import library can bind InterlockedExchangeAdd as a
   required kernel32 export on systems where that entry point is unavailable.
   Use the x86/x64 atomic instruction directly so the EXE has no such import. */
static LONG counter_add(volatile LONG *target,LONG value) {
    __asm__ __volatile__("lock xaddl %0, %1"
        : "+r"(value), "+m"(*target)
        :
        : "memory");
    return value;
}
#define ADD(p,x) counter_add((p),(x))
#else
#define ADD(p,x) InterlockedExchangeAdd((p),(x))
#endif
#define VALUE(p) InterlockedCompareExchange((p),0,0)
#else
static _Atomic int workers=0,stopping=0;
#define ADD(p,x) atomic_fetch_add((p),(x))
#define VALUE(p) atomic_load(p)
#endif

static int send_all(Conn c,const void *data,int n) {
    const char *p=data;
    while(n>0){int sent=send(c,p,n,0);if(sent<=0)return 0;p+=sent;n-=sent;}return 1;
}
static int headers(Conn c,int status,const char *reason,const char *type,int n,const char *codec) {
    char h[940];
    int len=format_text(h,sizeof(h),
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %d\r\n%s"
        "Connection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n"
        "Referrer-Policy: no-referrer\r\nX-Frame-Options: DENY\r\n"
        "Content-Security-Policy: default-src 'self'; script-src 'self'; style-src 'self'; "
        "connect-src 'self'; worker-src 'self'; media-src 'self' blob:; img-src 'self' data:; "
        "object-src 'none'; frame-ancestors 'none'; base-uri 'none'; form-action 'none'\r\n\r\n",
        status,reason,type,n,codec?"Content-Encoding: gzip\r\n":"");
    return len>0 && len<(int)sizeof(h) && send_all(c,h,len);
}
static void message(Conn c,int status,const char *reason,const char *text){
    int n=(int)strlen(text);if(headers(c,status,reason,"text/plain; charset=utf-8",n,NULL))send_all(c,text,n);
}
/* Only origin-form requests from this instance's exact loopback authority
   are accepted. A cross-origin form cannot set the custom action header. */
static int header_value(const char *line,const char *key,char *dest,size_t cap) {
    size_t k=strlen(key);int found=0;dest[0]=0;
    while(*line){
        const char *e=strstr(line,"\r\n");if(!e||e==line)break;
        if((size_t)(e-line)>k && !text_cmp(line,key,k) && line[k]==':'){
            const char *s=line+k+1;while(s<e&&(*s==' '||*s=='\t'))s++;
            const char *end=e;while(end>s&&(end[-1]==' '||end[-1]=='\t'))end--;
            if(found||(size_t)(end-s)>=cap)return -1;
            memcpy(dest,s,(size_t)(end-s));dest[end-s]=0;found=1;
        }line=e+2;
    }return found;
}
static int valid_url(const char *url,int catalog){
    if(strncmp(url,"https://",8))return 0;
    const char *host=url+8,*q=host;
    while(*q && *q!='/' && *q!='?' && *q!='#'){if(*q=='@')return 0;q++;}
    size_t n=(size_t)(q-host);if(!n||n>300)return 0;
    for(const unsigned char *p=(const unsigned char*)url;*p;p++){
        if(*p<33||*p>=127||*p=='"'||*p=='\\'||*p=='<'||*p=='>')return 0;
    }
    if(catalog){
        const char *apple="https://itunes.apple.com/lookup?",*spot="https://open.spotify.com/oembed?";
        return !strncmp(url,apple,strlen(apple))||!strncmp(url,spot,strlen(spot));
    }
    const char *pages[]={"open.spotify.com","music.apple.com","itunes.apple.com","spotify.link","spotify.app.link","apple.co"};
    for(size_t i=0;i<sizeof(pages)/sizeof(pages[0]);i++)if(n==strlen(pages[i])&&!text_cmp(host,pages[i],n))return 0;
    return 1;
}
/* Proxy settings arrive as X-Proxy-* headers on every local request. A bare
   scheme + authority only: no userinfo, path, query or fragment, since
   credentials travel in their own fields. */
static int proxy_url_ok(const char *u) {
    const char *schemes[]={"http://","https://","socks5://","socks5h://"};
    const char *rest=NULL;
    for(size_t i=0;i<sizeof(schemes)/sizeof(schemes[0]);i++){
        size_t n=strlen(schemes[i]);
        if(!strncmp(u,schemes[i],n)){rest=u+n;break;}
    }
    if(!rest||!*rest)return 0;
    size_t length=strlen(rest);
    if(length&&rest[length-1]=='/')length--;
    if(!length)return 0;
    for(const char *p=u;*p;p++){
        unsigned char c=(unsigned char)*p;
        if(c<33||c>=127||c=='"'||c=='\\')return 0;
    }
    for(size_t i=0;i<length;i++)if(rest[i]=='@'||rest[i]=='/'||rest[i]=='?'||rest[i]=='#')return 0;
    return 1;
}
static int proxy_cred_ok(const char *s) {
    for(const char *p=s;*p;p++){
        unsigned char c=(unsigned char)*p;
        if(c<33||c>=127||c=='"'||c=='\\')return 0;
    }
    return 1;
}
/* Reads the proxy headers into caller-owned buffers. Returns 1 on success
   (proxy.url is NULL when the proxy is not enabled) and 0 when the headers
   are malformed; callers reject the request in that case so a broken proxy
   setting fails closed instead of fetching directly. */
static int parse_proxy(const char *lines,struct Proxy *proxy,char *url,size_t urlCap,char *user,size_t userCap,char *pass,size_t passCap) {
    char enabled[8];
    int has=header_value(lines,"X-Proxy-Enabled",enabled,sizeof(enabled));
    proxy->url=NULL;proxy->user=NULL;proxy->pass=NULL;
    if(has<0)return 0;
    if(has==0)return 1;
    if(strcmp(enabled,"1"))return 0;
    if(header_value(lines,"X-Proxy-Url",url,urlCap)!=1||!proxy_url_ok(url))return 0;
    if(header_value(lines,"X-Proxy-User",user,userCap)<0||
       header_value(lines,"X-Proxy-Pass",pass,passCap)<0)return 0;
    if(!proxy_cred_ok(user)||!proxy_cred_ok(pass)||strchr(user,':'))return 0;
    proxy->url=url;proxy->user=user;proxy->pass=pass;
    return 1;
}
static void file_response(Conn c,const char *url,int catalog,const struct Proxy *proxy){
    FILE *file=NULL;int n=0,http=0,reason=fetch_file(c,url,catalog?JSON_LIMIT:AUDIO_LIMIT,catalog,proxy,&file,&n,&http);
    if(reason){
        if(reason==3)return;
        if(reason==2)message(c,413,"Too Large",catalog?"The catalog reply is too large.":"This file exceeds the 256 MiB import limit.");
        else if(reason==4)message(c,503,"Transfer Unavailable","The system HTTPS downloader could not start. On Windows, check that System32 contains curl.exe. Local files still work.");
        else if(reason==5)message(c,507,"Temp File Error","A temporary file could not be created or read. Check free disk space and retry.");
        else if(http==401||http==403)message(c,502,"Access Denied","The source denied the download. Use a current direct download URL or add a local file.");
        else if(http==404||http==410)message(c,502,"Source Missing","The download link is missing or expired.");
        else if(http==206)message(c,502,"Partial Source","The source only returned a partial recording. Use a full-file download URL.");
        else message(c,502,"Incomplete Transfer","The source did not return a complete download. Check the link, connection and file permissions, then retry.");
        return;
    }
    if(headers(c,200,"OK",catalog?"application/json":"application/octet-stream",n,NULL)){
        char buffer[32768];int remaining=n;
        while(remaining>0){int count=remaining<(int)sizeof(buffer)?remaining:(int)sizeof(buffer);
            int got=(int)fread(buffer,1,count,file);if(!got||!send_all(c,buffer,got))break;remaining-=got;}
    }
    fclose(file);
}
/* Fetches a small test URL through the configured proxy and reports only
   success or failure; the fetched body is discarded. */
static void proxy_test_response(Conn c,const char *url,const struct Proxy *proxy){
    FILE *file=NULL;int n=0,http=0,reason=fetch_file(c,url,JSON_LIMIT,1,proxy,&file,&n,&http);
    if(reason){
        if(reason==2)message(c,413,"Too Large","The test reply is too large.");
        else if(reason==4)message(c,503,"Transfer Unavailable","The system HTTPS downloader could not start. On Windows, check that System32 contains curl.exe.");
        else if(reason==5)message(c,507,"Temp File Error","A temporary file could not be created or read. Check free disk space and retry.");
        else if(http==401||http==403)message(c,502,"Access Denied","The proxy or the test address denied the request.");
        else message(c,502,"Proxy Test Failed","The test address could not be reached through the proxy. Check the proxy URL and credentials.");
        return;
    }
    fclose(file);
    message(c,200,"OK","Proxy OK");
}
static void serve(Conn c){
    char data[REQUEST_CAP+1],method[12],path[500],ver[12],host[80],actual_origin[100],action[8],length[32],te[64];
    int used=0;char *split=NULL;
    while(used<REQUEST_CAP && !split){
        int n=recv(c,data+used,REQUEST_CAP-used,0);if(n<=0)return;used+=n;data[used]=0;split=strstr(data,"\r\n\r\n");
    }
    if(!split){message(c,431,"Headers Too Large","The request headers are too large.");return;}
    const int header_end=(int)(split-data)+4;
    if(memchr(data,0,header_end)){message(c,400,"Bad Request","Invalid request.");return;}
    char *first=strstr(data,"\r\n");if(!first)return;
    *first=0;
    if(sscanf(data,"%11s %499s %11s",method,path,ver)!=3||strcmp(ver,"HTTP/1.1")){message(c,400,"Bad Request","Invalid request.");return;}
    const char *lines=first+2;
    if(header_value(lines,"Host",host,sizeof(host))!=1||strcmp(host,authority)){message(c,403,"Forbidden","Use the address printed by Trackport.");return;}
    if(!strcmp(method,"GET")||!strcmp(method,"HEAD")){
        const char *route=!strcmp(path,"/")?"/index.html":path;
        for(int i=0;i<asset_count;i++){
            const struct Asset *a=&assets[i];
            if(!strcmp(route,a->route)){if(headers(c,200,"OK",a->mime,a->size,"gzip")&&!strcmp(method,"GET"))send_all(c,a->data,a->size);return;}
        }message(c,404,"Not Found","This page could not be found.");return;
    }
    if(strcmp(method,"POST")){message(c,405,"Method Not Allowed","Unsupported request method.");return;}
    if(header_value(lines,"Origin",actual_origin,sizeof(actual_origin))!=1||strcmp(actual_origin,origin)||
       header_value(lines,"X-Trackport",action,sizeof(action))!=1||strcmp(action,"1")){
        message(c,403,"Forbidden","Open the app in its original local tab.");return;
    }
    if(header_value(lines,"Transfer-Encoding",te,sizeof(te))!=0||
       header_value(lines,"Content-Length",length,sizeof(length))!=1||!length[0]){
        message(c,400,"Bad Request","A fixed request length is required.");return;
    }
    for(char *p=length;*p;p++)if(!isdigit((unsigned char)*p)){message(c,400,"Bad Request","Invalid request length.");return;}
    if(strlen(length)>5||atoi(length)>URL_CAP){message(c,413,"Too Large","The download URL is too long.");return;}
    const int expects=atoi(length);char url[URL_CAP+1];int have=used-header_end;if(have>expects)have=expects;
    memcpy(url,data+header_end,have);
    while(have<expects){int got=recv(c,url+have,expects-have,0);if(got<=0)return;have+=got;}url[expects]=0;
    if(memchr(url,0,expects)){message(c,400,"Bad Request","Invalid URL.");return;}
    if(!strcmp(path,"/api/quit")){message(c,200,"OK","Session ended.");ADD(&stopping,1);return;}
    const int catalog=!strcmp(path,"/api/catalog");
    const int test=!strcmp(path,"/api/proxy-test");
    if(!catalog&&!test&&strcmp(path,"/api/audio")){message(c,404,"Not Found","Unknown action.");return;}
    struct Proxy proxy;char proxyUrl[512],proxyUser[128],proxyPass[128];
    if(!parse_proxy(lines,&proxy,proxyUrl,sizeof(proxyUrl),proxyUser,sizeof(proxyUser),proxyPass,sizeof(proxyPass))){
        message(c,400,"Bad Request","Invalid proxy setting.");return;}
    if(test){
        if(!proxy.url){message(c,400,"Bad Request","Enable the proxy first.");return;}
        if(!valid_url(url,0)){message(c,400,"Bad Request","Enter a valid HTTPS test URL.");return;}
        proxy_test_response(c,url,&proxy);
        return;
    }
    if(!valid_url(url,catalog)){message(c,400,"Bad Request","Enter a valid direct HTTPS download URL, or use the Track link tab.");return;}
    file_response(c,url,catalog,&proxy);
}
static void handle(Conn c){serve(c);ADD(&workers,-1);}
int main(int argc,char **argv){
    int browser=1;
    if(argc==2&&!strcmp(argv[1],"--no-browser"))browser=0;
    else if(argc>1){puts("Usage: Trackport [--no-browser]");return 2;}
    if(!platform_start()){fputs("Unable to initialize the local network interface.\n",stderr);return 1;}
    Conn s=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);struct sockaddr_in addr;
    if(s==INVALID_CONN)return 1;
    secure_socket(s);memset(&addr,0,sizeof(addr));addr.sin_family=AF_INET;addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    SockLen size=sizeof(addr);
    if(bind(s,(struct sockaddr *)&addr,sizeof(addr))||listen(s,16)||getsockname(s,(struct sockaddr *)&addr,&size)){
        fputs("Trackport could not open a local listening port.\n",stderr);close_conn(s);return 1;}
    format_text(authority,sizeof(authority),"127.0.0.1:%u",(unsigned)ntohs(addr.sin_port));
    format_text(origin,sizeof(origin),"http://%s",authority);
    printf("Trackport - MP3 export\n\nOpen %s/ in a current browser.\n\nKeep this window open while working. Use End session in the app when finished.\n",origin);fflush(stdout);
    if(browser)open_browser(origin);
    while(!VALUE(&stopping)){
        fd_set set;struct timeval pause={0,250000};FD_ZERO(&set);FD_SET(s,&set);
#ifdef _WIN32
        int n=select(0,&set,NULL,NULL,&pause);
#else
        int n=select(s+1,&set,NULL,NULL,&pause);
#endif
        if(n<=0)continue;
        Conn c=accept(s,NULL,NULL);if(c==INVALID_CONN)continue;secure_socket(c);socket_deadlines(c);
        if(ADD(&workers,1)>=16){ADD(&workers,-1);close_conn(c);continue;}
        if(!start_worker(c,handle)){ADD(&workers,-1);close_conn(c);}
    }
    close_conn(s);return 0;
}
