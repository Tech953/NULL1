#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "platform.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <ctype.h>
#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#include <shellapi.h>
#include <wchar.h>
typedef HANDLE Child;
#else
#include <sys/time.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <pthread.h>
#include <spawn.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
extern char **environ;
typedef pid_t Child;
#endif

struct Task { Conn c; void (*handler)(Conn); };
#ifdef _WIN32
static unsigned __stdcall thread_main(void *ptr)
#else
static void *thread_main(void *ptr)
#endif
{
    struct Task task = *(struct Task *)ptr; free(ptr);
    task.handler(task.c); close_conn(task.c); return 0;
}
int start_worker(Conn c, void (*handler)(Conn)) {
    struct Task *task = malloc(sizeof(*task)); if (!task) return 0;
    task->c = c; task->handler = handler;
#ifdef _WIN32
    uintptr_t id = _beginthreadex(NULL,0,thread_main,task,0,NULL);
    if (id) { CloseHandle((HANDLE)id); return 1; }
#else
    pthread_t id;
    if (!pthread_create(&id,NULL,thread_main,task)) { pthread_detach(id); return 1; }
#endif
    free(task); return 0;
}
int platform_start(void) {
#ifdef _WIN32
    WSADATA data; SetConsoleTitleW(L"Trackport MP3 export"); return WSAStartup(MAKEWORD(2,2), &data)==0;
#else
    signal(SIGPIPE, SIG_IGN); return 1;
#endif
}
void secure_socket(Conn c) {
#ifdef _WIN32
    SetHandleInformation((HANDLE)c,HANDLE_FLAG_INHERIT,0);
#else
    fcntl(c,F_SETFD,FD_CLOEXEC);
#endif
}
void socket_deadlines(Conn c) {
#ifdef _WIN32
    DWORD read_ms=12000, write_ms=30000;
#else
    struct timeval read_ms={12,0},write_ms={30,0};
#endif
    setsockopt(c,SOL_SOCKET,SO_RCVTIMEO,(const char *)&read_ms,sizeof(read_ms));
    setsockopt(c,SOL_SOCKET,SO_SNDTIMEO,(const char *)&write_ms,sizeof(write_ms));
}
void open_browser(const char *url) {
#ifdef _WIN32
    ShellExecuteA(NULL,"open",url,NULL,NULL,SW_SHOWNORMAL);
#else
    pid_t helper;char *const args[]={"xdg-open",(char *)url,NULL};
    if (!posix_spawnp(&helper,"xdg-open",NULL,NULL,args,environ)) { (void)helper; }
#endif
}
static int disconnected(Conn c) {
    fd_set readset; struct timeval now={0,0}; char b;
    FD_ZERO(&readset); FD_SET(c,&readset);
#ifdef _WIN32
    int ready=select(0,&readset,NULL,NULL,&now);
#else
    int ready=select(c+1,&readset,NULL,NULL,&now);
#endif
    return ready>0 && recv(c,&b,1,MSG_PEEK)==0;
}
/* Proxy strings reach a curl command line on Windows, so they must not
   contain characters that could break out of a quoted argument. The POSIX
   path passes argv directly and is immune, but the same check keeps both
   paths honest. The username also cannot contain ':', which separates it
   from the password in --proxy-user. */
static int proxy_ok(const struct Proxy *p) {
    if (!p || !p->url || !*p->url) return 1;
    for (const char *s = p->url; *s; s++)
        if ((unsigned char)*s < 33 || (unsigned char)*s >= 127 || *s == '"' || *s == '\\') return 0;
    for (const char *s = p->user; s && *s; s++)
        if ((unsigned char)*s < 33 || (unsigned char)*s >= 127 || *s == '"' || *s == '\\' || *s == ':') return 0;
    for (const char *s = p->pass; s && *s; s++)
        if ((unsigned char)*s < 33 || (unsigned char)*s >= 127 || *s == '"' || *s == '\\') return 0;
    return 1;
}
static FILE *stage(void);
static int stage_too_big(FILE *f,int limit);
static int poll_child(Child child,int *exit_code);
static void abort_child(Child child);
static void release_child(Child child);

/* yt-dlp accepts proxy authentication in the proxy URL. Encode credentials
   as URI userinfo so punctuation cannot alter the proxy authority. */
static int proxy_for_ytdlp(const struct Proxy *p,char *out,size_t cap) {
    static const char hex[]="0123456789ABCDEF";
    size_t used=0;
    if(!p||!p->url||!*p->url){if(cap)out[0]=0;return 1;}
    if(!proxy_ok(p))return 0;
    const char *scheme=strstr(p->url,"://");
    if(!scheme)return 0;
    size_t prefix=(size_t)(scheme-p->url)+3;
    const char *authority=p->url+prefix;
    size_t authority_len=strlen(authority);
    if(authority_len&&authority[authority_len-1]=='/')authority_len--;
    if(prefix+authority_len+1>=cap)return 0;
    memcpy(out,p->url,prefix);used=prefix;
    if(p->user&&*p->user){
        const char *parts[2]={p->user,p->pass?p->pass:""};
        for(int part=0;part<2;part++){
            if(part==1){if(used+1>=cap)return 0;out[used++]=':';}
            for(const unsigned char *s=(const unsigned char *)parts[part];*s;s++){
                unsigned char c=*s;
                if(isalnum(c)||c=='-'||c=='.'||c=='_'||c=='~'){
                    if(used+1>=cap)return 0;
                    out[used++]=(char)c;
                }else{
                    if(used+3>=cap)return 0;
                    out[used++]='%';out[used++]=hex[c>>4];out[used++]=hex[c&15];
                }
            }
        }
        if(used+1>=cap)return 0;
        out[used++]='@';
    }
    if(used+authority_len>=cap)return 0;
    memcpy(out+used,authority,authority_len);used+=authority_len;out[used]=0;
    return 1;
}

#define YTDLP_OUTPUT_LIMIT (1024*1024)
#ifdef _WIN32
static int begin_ytdlp(FILE *f,char **args,const struct Proxy *proxy,Child *child,int *missing) {
    wchar_t cmd[18000];char line[16000];size_t used=0;
    char proxy_url[2048];
    if(!proxy_for_ytdlp(proxy,proxy_url,sizeof(proxy_url)))return 0;
    (void)proxy_url;
    /* The caller supplies a complete, already ordered argv vector. Quote each
       argument using the Windows command-line backslash rules; no shell runs. */
    for(int i=0;args[i];i++){
        const char *s=args[i];size_t n=strlen(s),slashes=0;
        if(used+2>=sizeof(line))return 0;
        line[used++]='"';
        for(size_t j=0;j<n;j++){
            if(s[j]=='\\'){slashes++;continue;}
            if(s[j]=='"'){
                while(slashes--){if(used+1>=sizeof(line))return 0;line[used++]='\\';}
                slashes=0;if(used+2>=sizeof(line))return 0;line[used++]='\\';line[used++]='"';
            }else{
                while(slashes--){if(used+1>=sizeof(line))return 0;line[used++]='\\';}
                slashes=0;if(used+1>=sizeof(line))return 0;line[used++]=s[j];
            }
        }
        while(slashes--){if(used+2>=sizeof(line))return 0;line[used++]='\\';line[used++]='\\';}
        if(used+2>=sizeof(line))return 0;line[used++]='"';line[used++]=' ';
    }
    if(!used||used+1>=sizeof(line))return 0;
    line[used]=0;
    if(!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,line,-1,cmd,18000))return 0;
    HANDLE self=GetCurrentProcess(),output=INVALID_HANDLE_VALUE;
    if(!DuplicateHandle(self,(HANDLE)_get_osfhandle(_fileno(f)),self,&output,0,TRUE,DUPLICATE_SAME_ACCESS))return 0;
    SECURITY_ATTRIBUTES sa={sizeof(sa),NULL,TRUE};
    HANDLE nullDevice=CreateFileW(L"NUL",GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,OPEN_EXISTING,0,NULL);
    if(nullDevice==INVALID_HANDLE_VALUE){CloseHandle(output);return 0;}
    STARTUPINFOW si;PROCESS_INFORMATION pi;memset(&si,0,sizeof(si));memset(&pi,0,sizeof(pi));
    si.cb=sizeof(si);si.dwFlags=STARTF_USESTDHANDLES;si.hStdInput=nullDevice;si.hStdError=nullDevice;si.hStdOutput=output;
    BOOL ok=CreateProcessW(NULL,cmd,NULL,NULL,TRUE,CREATE_NO_WINDOW,NULL,NULL,&si,&pi);
    DWORD error=ok?0:GetLastError();
    CloseHandle(nullDevice);CloseHandle(output);
    if(!ok){if(error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND)*missing=1;return 0;}
    CloseHandle(pi.hThread);*child=pi.hProcess;return 1;
}
static int poll_ytdlp(Child child,int *exit_code){return poll_child(child,exit_code);}
#else
static int begin_ytdlp(FILE *f,char **args,const struct Proxy *proxy,Child *child,int *missing) {
    char proxy_url[2048];
    if(!proxy_for_ytdlp(proxy,proxy_url,sizeof(proxy_url)))return 0;
    (void)proxy_url;
    posix_spawn_file_actions_t a;if(posix_spawn_file_actions_init(&a))return 0;
    int bad=posix_spawn_file_actions_adddup2(&a,fileno(f),STDOUT_FILENO)||
            posix_spawn_file_actions_addopen(&a,STDIN_FILENO,"/dev/null",O_RDONLY,0)||
            posix_spawn_file_actions_addopen(&a,STDERR_FILENO,"/dev/null",O_WRONLY,0);
    int error=bad?EINVAL:posix_spawnp(child,"yt-dlp",&a,NULL,args,environ);
    posix_spawn_file_actions_destroy(&a);
    if(error==ENOENT)*missing=1;
    return !error;
}
static int poll_ytdlp(Child child,int *exit_code){return poll_child(child,exit_code);}
#endif

static int run_ytdlp(char **args,const struct Proxy *proxy,char *output,size_t capacity,size_t *length,int allow_empty) {
    FILE *f=stage();if(!f)return 4;
    Child child;int missing=0;
    if(!begin_ytdlp(f,args,proxy,&child,&missing)){fclose(f);return missing?1:2;}
    int code=1,reason=0,ticks=0;
    while(!poll_ytdlp(child,&code)){
        if(stage_too_big(f,YTDLP_OUTPUT_LIMIT)){reason=3;abort_child(child);break;}
        if(++ticks>=600){reason=2;abort_child(child);break;}
    }
    release_child(child);
    if(!reason&&stage_too_big(f,YTDLP_OUTPUT_LIMIT))reason=3;
    if(!reason&&code)reason=2;
    if(!reason){
        if(fseek(f,0,SEEK_END)){reason=4;}
        else{
            long size=ftell(f);
            if(size<0||size>YTDLP_OUTPUT_LIMIT)reason=3;
            else if(!allow_empty&&size==0)reason=2;
            else if((size_t)size>=capacity)reason=3;
            else if(fseek(f,0,SEEK_SET))reason=4;
            else if(size&&fread(output,1,(size_t)size,f)!=(size_t)size)reason=4;
            else{output[size]=0;*length=(size_t)size;}
        }
    }
    fclose(f);return reason;
}

int ytdlp_search(const char *query,const struct Proxy *proxy,char *output,size_t capacity,size_t *length) {
    char search[1024],proxy_url[2048];
    if(!query||strlen(query)>700||!proxy_for_ytdlp(proxy,proxy_url,sizeof(proxy_url)))return 2;
    if(snprintf(search,sizeof(search),"ytsearch5:%s",query)>=(int)sizeof(search))return 2;
    char *args[32];int i=0;
    args[i++]="yt-dlp";args[i++]="--ignore-config";args[i++]="--no-warnings";args[i++]="--no-progress";
    args[i++]="--flat-playlist";args[i++]="--dump-json";args[i++]="--skip-download";
    args[i++]="--playlist-end";args[i++]="5";args[i++]="--socket-timeout";args[i++]="15";
    args[i++]="--retries";args[i++]="1";args[i++]="--extractor-retries";args[i++]="1";
    if(proxy&&proxy->url&&*proxy->url){args[i++]="--proxy";args[i++]=proxy_url;}
    args[i++]="--";args[i++]=search;args[i]=NULL;
    return run_ytdlp(args,proxy,output,capacity,length,1);
}

int ytdlp_resolve(const char *watch_url,const struct Proxy *proxy,char *output,size_t capacity,size_t *length) {
    char proxy_url[2048];
    if(!watch_url||strlen(watch_url)>512||!proxy_for_ytdlp(proxy,proxy_url,sizeof(proxy_url)))return 2;
    char *args[32];int i=0;
    args[i++]="yt-dlp";args[i++]="--ignore-config";args[i++]="--no-warnings";args[i++]="--no-progress";
    args[i++]="--get-url";args[i++]="--format";args[i++]="bestaudio/best";args[i++]="--no-playlist";
    args[i++]="--socket-timeout";args[i++]="15";args[i++]="--retries";args[i++]="1";
    args[i++]="--extractor-retries";args[i++]="1";args[i++]="--extractor-args";
    args[i++]="youtube:player_client=android";
    if(proxy&&proxy->url&&*proxy->url){args[i++]="--proxy";args[i++]=proxy_url;}
    args[i++]="--";args[i++]=(char *)watch_url;args[i]=NULL;
    int result=run_ytdlp(args,proxy,output,capacity,length,0);
    if(!result){
        size_t n=0;while(n<*length&&output[n]!='\r'&&output[n]!='\n')n++;
        output[n]=0;*length=n;
        if(!n)return 2;
    }
    return result;
}
#ifdef _WIN32
static FILE *stage(void) {
    wchar_t dir[MAX_PATH+1],path[MAX_PATH+1];DWORD n=GetTempPathW(MAX_PATH,dir);
    if (!n || n>=MAX_PATH || !GetTempFileNameW(dir,L"TpM",0,path)) return NULL;
    HANDLE file=CreateFileW(path,GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_DELETE,NULL,
                 OPEN_EXISTING,FILE_ATTRIBUTE_TEMPORARY|FILE_FLAG_DELETE_ON_CLOSE,NULL);
    if (file==INVALID_HANDLE_VALUE) { DeleteFileW(path);return NULL; }
    int fd=_open_osfhandle((intptr_t)file,_O_RDWR|_O_BINARY);
    if (fd<0) { CloseHandle(file);return NULL; }
    FILE *f=_fdopen(fd,"w+b"); if (!f) _close(fd); return f;
}
static int stage_too_big(FILE *f,int limit) {
    DWORD high=0;HANDLE h=(HANDLE)_get_osfhandle(_fileno(f));
    DWORD low=GetFileSize(h,&high);
    return high!=0 || low==INVALID_FILE_SIZE || low>(DWORD)limit+3;
}
static int begin_curl(FILE *f,const char *url,int limit,int catalog,const struct Proxy *proxy,Child *child) {
    wchar_t exe[MAX_PATH+12],cmd[17000];char args[13400];int n;
    UINT dir=GetSystemDirectoryW(exe,MAX_PATH);
    if (!dir||dir>=MAX_PATH) return 0;wcscat(exe,L"\\curl.exe");
    n=format_text(args,sizeof(args),"curl.exe --disable --silent --fail --location --globoff --max-redirs 6 --connect-timeout 12 --max-time %d --proto =https --proto-redir =https --max-filesize %d --write-out %%{http_code} --url \"%s\"",catalog?18:300,limit,url);
    if (n<=0||n>=(int)sizeof(args)||!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,args,-1,cmd,17000))return 0;
    /* Proxy options are appended after the URL; curl accepts options in any
       position. --proxy-anyauth lets a proxy that needs Digest/NTLM
       negotiate instead of failing on Basic. */
    if (proxy && proxy->url && *proxy->url) {
        int m;
        if (!proxy_ok(proxy)) return 0;
        m=format_text(args+n,sizeof(args)-(size_t)n," --noproxy \"\" --proxy \"%s\"",proxy->url);
        if (m<=0||n+m>=(int)sizeof(args)) return 0;
        n+=m;
        if (proxy->user && *proxy->user) {
        m=format_text(args+n,sizeof(args)-(size_t)n," --proxy-anyauth --proxy-user \"%s:%s\"",
                          proxy->user,proxy->pass?proxy->pass:"");
            if (m<=0||n+m>=(int)sizeof(args)) return 0;
            n+=m;
        }
        if (!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,args,-1,cmd,17000)) return 0;
    }
    HANDLE self=GetCurrentProcess(),output=INVALID_HANDLE_VALUE;
    if (!DuplicateHandle(self,(HANDLE)_get_osfhandle(_fileno(f)),self,&output,0,TRUE,DUPLICATE_SAME_ACCESS))return 0;
    SECURITY_ATTRIBUTES sa={sizeof(sa),NULL,TRUE};
    HANDLE nullDevice=CreateFileW(L"NUL",GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,OPEN_EXISTING,0,NULL);
    if (nullDevice==INVALID_HANDLE_VALUE){CloseHandle(output);return 0;}
    STARTUPINFOW si;PROCESS_INFORMATION pi;memset(&si,0,sizeof(si));memset(&pi,0,sizeof(pi));
    si.cb=sizeof(si);si.dwFlags=STARTF_USESTDHANDLES;si.hStdInput=nullDevice;si.hStdError=nullDevice;si.hStdOutput=output;
    BOOL ok=CreateProcessW(exe,cmd,NULL,NULL,TRUE,CREATE_NO_WINDOW,NULL,NULL,&si,&pi);
    CloseHandle(nullDevice);CloseHandle(output);if(!ok)return 0;
    CloseHandle(pi.hThread);*child=pi.hProcess;return 1;
}
static int poll_child(Child child,int *exit_code){
    DWORD result=WaitForSingleObject(child,100);
    if (result==WAIT_TIMEOUT)return 0;
    DWORD code=1;if (result==WAIT_OBJECT_0)GetExitCodeProcess(child,&code);*exit_code=(int)code;return 1;
}
static void abort_child(Child child){TerminateProcess(child,1);WaitForSingleObject(child,5000);}
static void release_child(Child child){CloseHandle(child);}
#else
static FILE *stage(void) { FILE *f=tmpfile();if(f)fcntl(fileno(f),F_SETFD,FD_CLOEXEC);return f; }
static int stage_too_big(FILE *f,int limit){struct stat st;return fstat(fileno(f),&st)||st.st_size>(off_t)limit+3;}
static int begin_curl(FILE *f,const char *url,int limit,int catalog,const struct Proxy *proxy,Child *child) {
    char size[32];snprintf(size,sizeof(size),"%d",limit);
    char userpass[512];
    char *args[32];int i=0;
    args[i++]="curl";args[i++]="--disable";args[i++]="--silent";args[i++]="--fail";
    args[i++]="--location";args[i++]="--globoff";args[i++]="--max-redirs";args[i++]="6";
    args[i++]="--connect-timeout";args[i++]="12";args[i++]="--max-time";args[i++]=catalog?"18":"300";
    args[i++]="--proto";args[i++]="=https";args[i++]="--proto-redir";args[i++]="=https";
    args[i++]="--max-filesize";args[i++]=size;
    if (proxy && proxy->url && *proxy->url) {
        if (!proxy_ok(proxy)) return 0;
        args[i++]="--noproxy";args[i++]="";
        args[i++]="--proxy";args[i++]=(char *)proxy->url;
        if (proxy->user && *proxy->user) {
            if (snprintf(userpass,sizeof(userpass),"%s:%s",proxy->user,proxy->pass?proxy->pass:"")>=(int)sizeof(userpass)) return 0;
            args[i++]="--proxy-anyauth";args[i++]="--proxy-user";args[i++]=userpass;
        }
    }
    args[i++]="--write-out";args[i++]="%{http_code}";
    args[i++]="--url";args[i++]=(char *)url;args[i++]=NULL;
    posix_spawn_file_actions_t a;if(posix_spawn_file_actions_init(&a))return 0;
    int bad=posix_spawn_file_actions_adddup2(&a,fileno(f),STDOUT_FILENO)||
            posix_spawn_file_actions_addopen(&a,STDIN_FILENO,"/dev/null",O_RDONLY,0)||
            posix_spawn_file_actions_addopen(&a,STDERR_FILENO,"/dev/null",O_WRONLY,0);
    int ok=!bad && !posix_spawnp(child,"curl",&a,NULL,args,environ);posix_spawn_file_actions_destroy(&a);return ok;
}
static int poll_child(Child child,int *exit_code){
    int status=0;pid_t r=waitpid(child,&status,WNOHANG);
    if(r==child){*exit_code=WIFEXITED(status)?WEXITSTATUS(status):1;return 1;}
    if(r<0 && errno!=EINTR){*exit_code=1;return 1;}
    struct timespec t={0,100000000L};nanosleep(&t,NULL);return 0;
}
static void abort_child(Child child){int status;kill(child,SIGKILL);while(waitpid(child,&status,0)<0 && errno==EINTR){}}
static void release_child(Child child){(void)child;}
#endif

int fetch_file(Conn client,const char *url,int limit,int catalog,const struct Proxy *proxy,FILE **out,int *length,int *http) {
    FILE *f=stage(); if(!f)return 5;
    Child child; if(!begin_curl(f,url,limit,catalog,proxy,&child)){fclose(f);return 4;}
    int code=1,reason=0;
    while(!poll_child(child,&code)){
        if (stage_too_big(f,limit))reason=2;
        else if (disconnected(client))reason=3;
        if(reason){abort_child(child);break;}
    }
    release_child(child);
    if(!reason && (code==63||stage_too_big(f,limit)))reason=2;
    if(!reason){
        char trailer[4]={0};long size=0;
        if(!fseek(f,0,SEEK_END))size=ftell(f);
        if(size>=3 && !fseek(f,-3,SEEK_END) && fread(trailer,1,3,f)==3)*http=atoi(trailer);
        if(code||*http!=200||size<=3)reason=1;
        else if(fseek(f,0,SEEK_SET))reason=5;
        else {*out=f;*length=(int)(size-3);return 0;}
    }
    fclose(f);return reason;
}
