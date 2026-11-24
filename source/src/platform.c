#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "platform.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
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
static int begin_curl(FILE *f,const char *url,int limit,int catalog,Child *child) {
    wchar_t exe[MAX_PATH+12],cmd[17000];char args[13400];int n;
    UINT dir=GetSystemDirectoryW(exe,MAX_PATH);
    if (!dir||dir>=MAX_PATH) return 0;wcscat(exe,L"\\curl.exe");
    n=format_text(args,sizeof(args),"curl.exe --disable --silent --fail --location --globoff --max-redirs 6 --connect-timeout 12 --max-time %d --proto =https --proto-redir =https --max-filesize %d --write-out %%{http_code} --url \"%s\"",catalog?18:300,limit,url);
    if (n<=0||n>=(int)sizeof(args)||!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,args,-1,cmd,17000))return 0;
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
static int begin_curl(FILE *f,const char *url,int limit,int catalog,Child *child) {
    char size[32];snprintf(size,sizeof(size),"%d",limit);
    char *const args[]={"curl","--disable","--silent","--fail","--location","--globoff","--max-redirs","6",
        "--connect-timeout","12","--max-time",catalog?"18":"300","--proto","=https","--proto-redir","=https",
        "--max-filesize",size,"--write-out","%{http_code}","--url",(char *)url,NULL};
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

int fetch_file(Conn client,const char *url,int limit,int catalog,FILE **out,int *length,int *http) {
    FILE *f=stage(); if(!f)return 5;
    Child child; if(!begin_curl(f,url,limit,catalog,&child)){fclose(f);return 4;}
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
