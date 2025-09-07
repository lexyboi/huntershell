#include <bits/stdc++.h>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <direct.h>
#include <processthreadsapi.h>
#pragma comment(lib, "Advapi32.lib")
#else
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <pwd.h>
#include <signal.h>
#endif
#include <filesystem>
#include <curl/curl.h>
#include "json/json.hpp"
using json = nlohmann::json;

using namespace std;
namespace fs = std::filesystem;

static string SHELL_NAME="HunterShell";
static string VERSION="1.0";
static string historyFilePath;
static unordered_map<string,string> aliases;

static bool isWindows() {
#ifdef _WIN32
    return true;
#else
    return false;
#endif
}

static string toUTF8(const wstring &ws) {
#ifdef _WIN32
    if(ws.empty()) return string();
    int sizeNeeded = WideCharToMultiByte(CP_UTF8,0,ws.data(),(int)ws.size(),nullptr,0,nullptr,nullptr);
    string strTo(sizeNeeded,0);
    WideCharToMultiByte(CP_UTF8,0,ws.data(),(int)ws.size(),&strTo[0],sizeNeeded,nullptr,nullptr);
    return strTo;
#else
    return string(ws.begin(), ws.end());
#endif
}

static wstring fromUTF8(const string &s) {
#ifdef _WIN32
    if(s.empty()) return wstring();
    int sizeNeeded = MultiByteToWideChar(CP_UTF8,0,s.data(),(int)s.size(),nullptr,0);
    wstring w(sizeNeeded,0);
    MultiByteToWideChar(CP_UTF8,0,s.data(),(int)s.size(),&w[0],sizeNeeded);
    return w;
#else
    return wstring(s.begin(), s.end());
#endif
}

static void enableANSIOnWindows() {
#ifdef _WIN32
    HANDLE hOut=GetStdHandle(STD_OUTPUT_HANDLE);
    if(hOut==INVALID_HANDLE_VALUE) return;
    DWORD mode=0;
    if(!GetConsoleMode(hOut,&mode)) return;
    mode|=ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    SetConsoleMode(hOut,mode);
    HANDLE hIn=GetStdHandle(STD_INPUT_HANDLE);
    if(hIn!=INVALID_HANDLE_VALUE){
        DWORD inMode=0;
        if(GetConsoleMode(hIn,&inMode)) SetConsoleMode(hIn,inMode|ENABLE_VIRTUAL_TERMINAL_INPUT);
    }
#endif
}

static string getEnv(const string& key) {
#ifdef _WIN32
    DWORD len=GetEnvironmentVariableW(fromUTF8(key).c_str(),nullptr,0);
    if(len==0) return "";
    wstring buf(len,0);
    GetEnvironmentVariableW(fromUTF8(key).c_str(),&buf[0],len);
    if(!buf.empty() && buf.back()==L'\0') buf.pop_back();
    return toUTF8(buf);
#else
    const char* v=getenv(key.c_str());
    return v?string(v):string();
#endif
}

static void setEnvVar(const string& k, const string& v) {
#ifdef _WIN32
    SetEnvironmentVariableW(fromUTF8(k).c_str(), fromUTF8(v).c_str());
#else
    setenv(k.c_str(), v.c_str(), 1);
#endif
}

static string getUserName() {
    string u=getEnv(isWindows()?"USERNAME":"USER");
    if(!u.empty()) return u;
#ifndef _WIN32
    struct passwd* pw=getpwuid(getuid());
    if(pw && pw->pw_name) return pw->pw_name;
#endif
    return "user";
}

static string getHostName() {
#ifdef _WIN32
    WCHAR buf[256]; DWORD sz=256;
    if(GetComputerNameW(buf,&sz)) return toUTF8(wstring(buf,buf+sz));
    return "windows";
#else
    char buf[256]; if(gethostname(buf,sizeof(buf))==0) return string(buf);
    return "unix";
#endif
}

static string currentDir() {
    try { return fs::current_path().u8string(); } catch(...) { return "."; }
}

static string homeDir() {
#ifdef _WIN32
    string p=getEnv("USERPROFILE"); if(!p.empty()) return p;
    string d=getEnv("HOMEDRIVE")+getEnv("HOMEPATH"); if(!d.empty()) return d;
    return "C:\\";
#else
    string p=getEnv("HOME"); if(!p.empty()) return p;
    return "/";
#endif
}

static string tempDir() {
#ifdef _WIN32
    string t=getEnv("TEMP"); if(!t.empty()) return t;
    return homeDir();
#else
    string t=getEnv("TMPDIR"); if(!t.empty()) return t;
    return "/tmp";
#endif
}

static string join(const vector<string>& v, const string& sep=" ") {
    string s; for(size_t i=0;i<v.size();++i){ if(i) s+=sep; s+=v[i]; } return s;
}

static string trim(const string& s) {
    size_t a=0,b=s.size();
    while(a<b && isspace((unsigned char)s[a])) a++;
    while(b>a && isspace((unsigned char)s[b-1])) b--;
    return s.substr(a,b-a);
}

static vector<string> splitArgs(const string& line) {
    vector<string> out;
    string cur;
    bool inS=false,inD=false,esc=false;
    for(size_t i=0;i<line.size();++i){
        char c=line[i];
        if(esc){ cur.push_back(c); esc=false; continue; }
        if(c=='\\'){ esc=true; continue; }
        if(c=='\'' && !inD){ inS=!inS; continue; }
        if(c=='"' && !inS){ inD=!inD; continue; }
        if(isspace((unsigned char)c) && !inS && !inD){ if(!cur.empty()){ out.push_back(cur); cur.clear(); } continue; }
        cur.push_back(c);
    }
    if(!cur.empty()) out.push_back(cur);
    return out;
}

static string expandTilde(const string& p) {
    if(p.rfind("~",0)==0){
        if(p.size()==1) return homeDir();
        if(p.size()>1 && (p[1]=='/' || p[1]=='\\')) return homeDir()+p.substr(1);
    }
    return p;
}

static string normalizePath(const string& p) {
    try {
        fs::path x=fs::weakly_canonical(fs::path(p));
        return x.u8string();
    } catch(...) {
        return fs::path(p).u8string();
    }
}

static bool isExecutableInPath(const string& name, string& full) {
    string pathEnv=getEnv("PATH");
    char sep=isWindows()?';':':';
    vector<string> dirs;
    string cur;
    for(char ch: pathEnv){ if(ch==sep){ if(!cur.empty()) dirs.push_back(cur); cur.clear(); } else cur.push_back(ch); }
    if(!cur.empty()) dirs.push_back(cur);
#ifdef _WIN32
    vector<string> exts;
    string pathext=getEnv("PATHEXT"); if(pathext.empty()) pathext=".EXE;.BAT;.CMD;.COM";
    cur.clear(); for(char ch: pathext){ if(ch==';'){ if(!cur.empty()) exts.push_back(cur); cur.clear(); } else cur.push_back(ch); }
    if(!cur.empty()) exts.push_back(cur);
#endif
    for(auto& d: dirs){
#ifdef _WIN32
        for(auto& e: exts){
            fs::path p=fs::path(d)/ (name+e);
            if(fs::exists(p)){ full=p.u8string(); return true; }
        }
#endif
        fs::path p=fs::path(d)/name;
        if(fs::exists(p)){ full=p.u8string(); return true; }
    }
    return false;
}

static int runSystem(const string& cmd) {
#ifdef _WIN32
    wstring wcmd=fromUTF8(cmd);
    STARTUPINFOW si{0}; si.cb=sizeof(si);
    PROCESS_INFORMATION pi{0};
    vector<wchar_t> buffer(wcmd.begin(), wcmd.end());
    buffer.push_back(L'\0');
    if(!CreateProcessW(nullptr, buffer.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        return system(cmd.c_str());
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code=0;
    GetExitCodeProcess(pi.hProcess,&code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
#else
    return system(cmd.c_str());
#endif
}

static void writeHistory(const string& line) {
    if(historyFilePath.empty()) return;
    ofstream f(historyFilePath, ios::app | ios::binary);
    if(!f) return;
    f<<line<<"\n";
}

static vector<string> readHistory() {
    vector<string> lines;
    if(historyFilePath.empty()) return lines;
    ifstream f(historyFilePath);
    string s;
    while(getline(f,s)) lines.push_back(s);
    return lines;
}

static string humanSize(uint64_t n) {
    const char* u[]={"B","K","M","G","T","P"};
    int i=0; double d=n;
    while(d>=1024.0 && i<5){ d/=1024.0; i++; }
    ostringstream o; o.setf(std::ios::fixed); o<<setprecision(d>=10?0:1)<<d<<u[i];
    return o.str();
}

static int cmd_help(const vector<string>& args) {
    cout<<"Available commands with parameters:\n";
    cout<<"ls [-l] [-a] [-h]\n";
    cout<<"pwd\n";
    cout<<"clear\n";
    cout<<"cd [dir]\n";
    cout<<"whoami\n";
    cout<<"hostname\n";
    cout<<"date\n";
    cout<<"time\n";
    cout<<"uname [-a|-s|-r|-m]\n";
    cout<<"history\n";
    cout<<"man\n";
    cout<<"echo [text]\n";
    cout<<"cat [file]\n";
    cout<<"head [file]\n";
    cout<<"tail [file]\n";
    cout<<"touch [file]\n";
    cout<<"mkdir [dir]\n";
    cout<<"rmdir [dir]\n";
    cout<<"rm [file]\n";
    cout<<"mv [src] [dest]\n";
    cout<<"cp [src] [dest]\n";
    cout<<"tree [dir]\n";
    cout<<"find [string]\n";
    cout<<"grep [string] [file]\n";
    cout<<"wc [-l] [file]\n";
    cout<<"ps\n";
    cout<<"kill [pid]\n";
    cout<<"which [bin]\n";
    cout<<"locate [name]\n";
    cout<<"uptime\n";
    cout<<"df [-h] [path]\n";
    cout<<"free [-m]\n";
    cout<<"env\n";
    cout<<"export VAR=VALUE\n";
    cout<<"alias name=command\n";
    cout<<"ifconfig | ipconfig\n";
    cout<<"ping [host]\n";
    cout<<"tracert [host]\n";
    cout<<"nslookup [host]\n";
    cout<<"who\n";
    cout<<"users\n";
    cout<<"id\n";
    cout<<"lsusb\n";
    cout<<"lspci\n";
    cout<<"top\n";
    cout<<"htop\n";
    cout<<"du [-h] [dir]\n";
    cout<<"stat [file]\n";
    cout<<"strings [file]\n";
    cout<<"basename [path]\n";
    cout<<"dirname [path]\n";
    cout<<"sha256sum [file]\n";
    cout<<"md5sum [file]\n";
    cout<<"chmod [perm] [file]\n";
    cout<<"chown [owner] [file]\n";
    cout<<"mount\n";
    cout<<"netstat [-an]\n";
    cout<<"arp -a\n";
    cout<<"route print\n";
    cout<<"taskkill [/PID]\n";
    cout<<"schtasks\n";
    cout<<"reg query|add|delete\n";
    cout<<"attrib\n";
    cout<<"more [file]\n";
    cout<<"sort [file]\n";
    cout<<"uniq [file]\n";
    cout<<"split [file]\n";
    cout<<"tee [file]\n";
    cout<<"diff [f1] [f2]\n";
    cout<<"cmp [f1] [f2]\n";
    cout<<"base64 [file]\n";
    cout<<"timeout [n] | sleep [n]\n";
    cout<<"curl [url]\n";
    cout<<"wget [url]\n";
    cout<<"sc query|start|stop [name]\n";
    cout<<"driverquery\n";
    cout<<"systeminfo\n";
    cout<<"ver\n";
    cout<<"taskmgr\n";
    cout<<"exit\n";
    return 0;
}

static int cmd_pwd(const vector<string>& a){ cout<<currentDir()<<"\n"; return 0; }

static int cmd_clear(const vector<string>& a){
    if(isWindows()) runSystem("cls"); else runSystem("clear");
    return 0;
}

static int cmd_cd(const vector<string>& a){
    string dest;
    if(a.size()<2) dest=homeDir();
    else dest=expandTilde(a[1]);
    try { fs::current_path(fs::path(dest)); }
    catch(...) { cerr<<"No such directory\n"; return 1; }
    return 0;
}

static int cmd_whoami(const vector<string>& a){ cout<<getUserName()<<"\n"; return 0; }
static int cmd_hostname(const vector<string>& a){ cout<<getHostName()<<"\n"; return 0; }

static int cmd_date(const vector<string>& a){
    time_t t=time(nullptr);
    tm tmv;
#ifdef _WIN32
    localtime_s(&tmv,&t);
#else
    localtime_r(&t,&tmv);
#endif
    char buf[128];
    strftime(buf,sizeof(buf),"%Y-%m-%d", &tmv);
    cout<<buf<<"\n";
    return 0;
}

static int cmd_time(const vector<string>& a){
    time_t t=time(nullptr);
    tm tmv;
#ifdef _WIN32
    localtime_s(&tmv,&t);
#else
    localtime_r(&t,&tmv);
#endif
    char buf[128];
    strftime(buf,sizeof(buf),"%H:%M:%S", &tmv);
    cout<<buf<<"\n";
    return 0;
}

static int cmd_uname(const vector<string>& a){
    bool aopt=false,sopt=false,ropt=false,mopt=false;
    for(size_t i=1;i<a.size();++i){
        if(a[i]=="-a") aopt=true;
        if(a[i]=="-s") sopt=true;
        if(a[i]=="-r") ropt=true;
        if(a[i]=="-m") mopt=true;
    }
#ifdef _WIN32
    string sys="Windows";
    string rel="10";
    SYSTEM_INFO si; GetNativeSystemInfo(&si);
    string mach=(si.wProcessorArchitecture==PROCESSOR_ARCHITECTURE_AMD64)?"x86_64":"x86";
#else
    string sys="Linux";
    string rel="unknown";
    struct utsname un;
    if(uname(&un)==0){ sys=un.sysname; rel=un.release; }
    string mach=(uname(&un)==0)?string(un.machine):"x86_64";
#endif
    if(aopt||(!sopt&&!ropt&&!mopt)){ cout<<sys<<" "<<rel<<" "<<mach<<"\n"; return 0; }
    if(sopt) cout<<sys<<"\n";
    if(ropt) cout<<rel<<"\n";
    if(mopt) cout<<mach<<"\n";
    return 0;
}

static int cmd_history(const vector<string>& a){
    auto lines=readHistory();
    for(size_t i=0;i<lines.size();++i) cout<<i+1<<" "<<lines[i]<<"\n";
    return 0;
}

static int cmd_echo(const vector<string>& a){
    if(a.size()<=1){ cout<<"\n"; return 0; }
    vector<string> v=a; v.erase(v.begin());
    cout<<join(v)<<"\n"; return 0;
}

static int readFileLines(const string& path, vector<string>& out){
    ifstream f(path, ios::binary);
    if(!f) return 1;
    string s; while(getline(f,s)) out.push_back(s);
    return 0;
}

static int cmd_cat(const vector<string>& a){
    if(a.size()<2){ cerr<<"usage: cat [file]\n"; return 1; }
    for(size_t i=1;i<a.size();++i){
        ifstream f(a[i], ios::binary);
        if(!f){ cerr<<"cannot open "<<a[i]<<"\n"; continue; }
        cout<<f.rdbuf();
    }
    return 0;
}

static int cmd_head(const vector<string>& a){
    if(a.size()<2){ cerr<<"usage: head [file]\n"; return 1; }
    vector<string> lines; if(readFileLines(a[1],lines)) { cerr<<"cannot open\n"; return 1; }
    size_t n=min<size_t>(10, lines.size());
    for(size_t i=0;i<n;++i) cout<<lines[i]<<"\n";
    return 0;
}

static int cmd_tail(const vector<string>& a){
    if(a.size()<2){ cerr<<"usage: tail [file]\n"; return 1; }
    vector<string> lines; if(readFileLines(a[1],lines)) { cerr<<"cannot open\n"; return 1; }
    size_t n=min<size_t>(10, lines.size());
    for(size_t i=lines.size()>n?lines.size()-n:0;i<lines.size();++i) cout<<lines[i]<<"\n";
    return 0;
}

static int cmd_touch(const vector<string>& a){
    if(a.size()<2){ cerr<<"usage: touch [file]\n"; return 1; }
    for(size_t i=1;i<a.size();++i){
        ofstream f(a[i], ios::app | ios::binary);
        if(!f){ cerr<<"cannot touch "<<a[i]<<"\n"; }
    }
    return 0;
}

static int cmd_mkdir(const vector<string>& a){
    if(a.size()<2){ cerr<<"usage: mkdir [dir]\n"; return 1; }
    try { fs::create_directories(a[1]); return 0; } catch(...) { cerr<<"mkdir failed\n"; return 1; }
}

static int cmd_rmdir(const vector<string>& a){
    if(a.size()<2){ cerr<<"usage: rmdir [dir]\n"; return 1; }
    try { fs::remove_all(a[1]); return 0; } catch(...) { cerr<<"rmdir failed\n"; return 1; }
}

static int cmd_rm(const vector<string>& a){
    if(a.size()<2){ cerr<<"usage: rm [file]\n"; return 1; }
    int rc=0;
    for(size_t i=1;i<a.size();++i){ try { if(!fs::remove(a[i])){ cerr<<"not found: "<<a[i]<<"\n"; rc=1; } } catch(...) { cerr<<"rm failed "<<a[i]<<"\n"; rc=1; } }
    return rc;
}

static int copyPath(const fs::path& src, const fs::path& dst){
    try{
        if(fs::is_directory(src)){
            fs::create_directories(dst);
            for(auto& p: fs::recursive_directory_iterator(src)){
                auto rel=fs::relative(p.path(),src);
                auto target=dst/rel;
                if(fs::is_directory(p)) fs::create_directories(target);
                else fs::copy_file(p.path(),target, fs::copy_options::overwrite_existing);
            }
        } else {
            fs::create_directories(dst.parent_path());
            fs::copy_file(src,dst, fs::copy_options::overwrite_existing);
        }
        return 0;
    } catch(...) { return 1; }
}

static int cmd_mv(const vector<string>& a){
    if(a.size()<3){ cerr<<"usage: mv [src] [dest]\n"; return 1; }
    try { fs::rename(a[1],a[2]); return 0; } catch(...) { if(copyPath(a[1],a[2])==0){ try{ fs::remove_all(a[1]); }catch(...){} return 0; } cerr<<"mv failed\n"; return 1; }
}

static int cmd_cp(const vector<string>& a){
    if(a.size()<3){ cerr<<"usage: cp [src] [dest]\n"; return 1; }
    return copyPath(a[1],a[2]);
}

static void treePrint(const fs::path& p, string prefix=""){
    cout<<prefix<<p.filename().u8string()<<"\n";
    if(fs::is_directory(p)){
        vector<fs::path> children;
        for(auto& e: fs::directory_iterator(p)) children.push_back(e.path());
        sort(children.begin(), children.end());
        for(size_t i=0;i<children.size();++i){
            bool last=(i==children.size()-1);
            cout<<prefix<<(last?"└─ ":"├─ ");
            treePrint(children[i], prefix+(last?"   ":"│  "));
        }
    }
}

static int cmd_tree(const vector<string>& a){
    fs::path p=a.size()<2?fs::current_path():fs::path(a[1]);
    try { treePrint(p, ""); } catch(...) { cerr<<"tree error\n"; return 1; }
    return 0;
}

static int cmd_find(const vector<string>& a){
    if(a.size()<2){ cerr<<"usage: find [string]\n"; return 1; }
    string key=a[1];
    int count=0;
    try{
        for(auto& e: fs::recursive_directory_iterator(fs::current_path())){
            string n=e.path().filename().u8string();
            if(n.find(key)!=string::npos){ cout<<e.path().u8string()<<"\n"; count++; }
        }
    }catch(...){}
    if(count==0) return 1; return 0;
}

static int cmd_grep(const vector<string>& a){
    if(a.size()<3){ cerr<<"usage: grep [string] [file]\n"; return 1; }
    vector<string> lines; if(readFileLines(a[2],lines)) { cerr<<"cannot open\n"; return 1; }
    string key=a[1]; int hits=0;
    for(size_t i=0;i<lines.size();++i){ if(lines[i].find(key)!=string::npos){ cout<<lines[i]<<"\n"; hits++; } }
    return hits?0:1;
}

static int cmd_wc(const vector<string>& a){
    if(a.size()<2){ cerr<<"usage: wc [-l] [file]\n"; return 1; }
    bool onlyLines=false; string file;
    for(size_t i=1;i<a.size();++i){ if(a[i]=="-l") onlyLines=true; else file=a[i]; }
    if(file.empty()){ cerr<<"file required\n"; return 1; }
    vector<string> lines; if(readFileLines(file,lines)) { cerr<<"cannot open\n"; return 1; }
    if(onlyLines){ cout<<lines.size()<<"\n"; return 0; }
    size_t words=0, bytes=0;
    ifstream f(file, ios::binary); string s; while(f>>s) words++; f.clear(); f.seekg(0,ios::end); bytes=(size_t)f.tellg();
    cout<<lines.size()<<" "<<words<<" "<<bytes<<" "<<file<<"\n";
    return 0;
}

static int cmd_ps(const vector<string>& a){
    if(isWindows()) return runSystem("tasklist");
    else return runSystem("ps aux");
}

static int cmd_kill(const vector<string>& a){
    if(a.size()<2){ cerr<<"usage: kill [pid]\n"; return 1; }
    int pid=stoi(a[1]);
#ifdef _WIN32
    HANDLE h=OpenProcess(PROCESS_TERMINATE,FALSE,(DWORD)pid);
    if(!h){ cerr<<"cannot open process\n"; return 1; }
    BOOL ok=TerminateProcess(h,1); CloseHandle(h);
    if(!ok){ cerr<<"terminate failed\n"; return 1; }
    return 0;
#else
    if(::kill(pid,SIGTERM)!=0){ perror("kill"); return 1; }
    return 0;
#endif
}

static int cmd_which(const vector<string>& a){
    if(a.size()<2){ cerr<<"usage: which [bin]\n"; return 1; }
    string full; if(isExecutableInPath(a[1],full)){ cout<<full<<"\n"; return 0; }
    cerr<<"not found\n"; return 1;
}

static int cmd_locate(const vector<string>& a){
    if(a.size()<2){ cerr<<"usage: locate [name]\n"; return 1; }
    string key=a[1];
    int n=0;
    try{
        for(auto& e: fs::recursive_directory_iterator(fs::current_path())) {
            string p=e.path().u8string();
            if(p.find(key)!=string::npos){ cout<<p<<"\n"; n++; if(n>=5000) break; }
        }
    }catch(...){}
    return n?0:1;
}

static int cmd_uptime(const vector<string>& a){
#ifdef _WIN32
    ULONGLONG ms=GetTickCount64();
    unsigned long s=(unsigned long)(ms/1000ULL);
#else
    unsigned long s=0;
    ifstream f("/proc/uptime"); if(f) { double u; f>>u; s=(unsigned long)u; }
#endif
    unsigned long d=s/86400; s%=86400;
    unsigned long h=s/3600; s%=3600;
    unsigned long m=s/60; s%=60;
    cout<<d<<" days, "<<h<<" hours, "<<m<<" minutes, "<<s<<" seconds\n";
    return 0;
}

static int cmd_df(const vector<string>& a){
    string path=a.size()>1?a.back():currentDir();
    try{
        auto sp=fs::space(path);
        bool human=false; for(auto& x:a) if(x=="-h") human=true;
        if(human){
            cout<<"Filesystem: "<<path<<"\n";
            cout<<"Total: "<<humanSize(sp.capacity)<<"\n";
            cout<<"Free: "<<humanSize(sp.free)<<"\n";
            cout<<"Available: "<<humanSize(sp.available)<<"\n";
        } else {
            cout<<path<<" "<<sp.capacity<<" "<<sp.free<<" "<<sp.available<<"\n";
        }
        return 0;
    }catch(...){ cerr<<"df failed\n"; return 1; }
}

static int cmd_free(const vector<string>& a){
#ifdef _WIN32
    MEMORYSTATUSEX ms; ms.dwLength=sizeof(ms);
    if(GlobalMemoryStatusEx(&ms)){
        bool mflag=false; for(auto& x:a) if(x=="-m") mflag=true;
        unsigned long long total=ms.ullTotalPhys;
        unsigned long long avail=ms.ullAvailPhys;
        if(mflag){ total/=1024*1024; avail/=1024*1024; }
        cout<<"MemTotal "<<total<<(mflag?" MB":" B")<<"\n";
        cout<<"MemFree "<<avail<<(mflag?" MB":" B")<<"\n";
        return 0;
    }
    cerr<<"free failed\n"; return 1;
#else
    ifstream f("/proc/meminfo"); if(!f){ cerr<<"meminfo not available\n"; return 1; }
    string s; while(getline(f,s)) cout<<s<<"\n"; return 0;
#endif
}

static int cmd_env(const vector<string>& a){
#ifdef _WIN32
    LPWCH penv=GetEnvironmentStringsW();
    if(!penv) return 1;
    LPWCH cur=penv;
    while(*cur){
        wstring w=cur; cout<<toUTF8(w)<<"\n"; cur+=w.size()+1;
    }
    FreeEnvironmentStringsW(penv);
#else
    extern char **environ;
    for(char** e=environ; *e; ++e) cout<<*e<<"\n";
#endif
    return 0;
}

static int cmd_export(const vector<string>& a){
    if(a.size()<2){ cerr<<"usage: export VAR=VALUE\n"; return 1; }
    string kv=a[1];
    auto pos=kv.find('=');
    if(pos==string::npos){ cerr<<"must be VAR=VALUE\n"; return 1; }
    string k=kv.substr(0,pos), v=kv.substr(pos+1);
    setEnvVar(k,v); return 0;
}

static int cmd_alias(const vector<string>& a){
    if(a.size()==1){ for(auto& p: aliases) cout<<p.first<<"="<<p.second<<"\n"; return 0; }
    string kv=join(vector<string>(a.begin()+1,a.end())," ");
    auto pos=kv.find('=');
    if(pos==string::npos){ cerr<<"usage: alias name=command\n"; return 1; }
    string k=trim(kv.substr(0,pos));
    string v=trim(kv.substr(pos+1));
    aliases[k]=v; return 0;
}

static int cmd_ifconfig(const vector<string>& a){
    if(isWindows()) return runSystem("ipconfig");
    else { int rc=runSystem("ifconfig"); if(rc!=0) rc=runSystem("ip a"); return rc; }
}

static int cmd_ping(const vector<string>& a){
    if(a.size()<2){ cerr<<"usage: ping [host]\n"; return 1; }
    string host=a[1];
    if(isWindows()) return runSystem("ping -n 4 "+host);
    else return runSystem("ping -c 4 "+host);
}

static int cmd_tracert(const vector<string>& a){
    if(a.size()<2){ cerr<<"usage: tracert [host]\n"; return 1; }
    string host=a[1];
    if(isWindows()) return runSystem("tracert "+host);
    else return runSystem("traceroute "+host);
}

static int cmd_nslookup(const vector<string>& a){
    if(a.size()<2){ cerr<<"usage: nslookup [host]\n"; return 1; }
    return runSystem("nslookup "+a[1]);
}

static int cmd_who(const vector<string>& a){
    cout<<getUserName()<<" tty0\n"; return 0;
}

static int cmd_users(const vector<string>& a){
    cout<<getUserName()<<"\n"; return 0;
}

static int cmd_id(const vector<string>& a){
#ifdef _WIN32
    cout<<"uid=0 gid=0 "<<getUserName()<<"\n"; return 0;
#else
    cout<<"uid="<<getuid()<<" gid="<<getgid()<<" "<<getUserName()<<"\n"; return 0;
#endif
}

static int cmd_lsusb(const vector<string>& a){
    if(isWindows()){ cout<<"Not implemented on Windows\n"; return 1; }
    return runSystem("lsusb");
}

static int cmd_lspci(const vector<string>& a){
    if(isWindows()){ cout<<"Not implemented on Windows\n"; return 1; }
    return runSystem("lspci");
}

static int cmd_top(const vector<string>& a){
    if(isWindows()) return runSystem("tasklist");
    else return runSystem("top -b -n 1");
}

static uint64_t folderSize(const fs::path& p){
    uint64_t s=0;
    try{ for(auto& e: fs::recursive_directory_iterator(p)) if(fs::is_regular_file(e)) s+=fs::file_size(e); } catch(...) {}
    return s;
}

static int cmd_du(const vector<string>& a){
    bool human=false; string dir=currentDir();
    for(size_t i=1;i<a.size();++i){ if(a[i]=="-h") human=true; else dir=a[i]; }
    uint64_t s=folderSize(dir);
    if(human) cout<<humanSize(s)<<" "<<dir<<"\n"; else cout<<s<<" "<<dir<<"\n";
    return 0;
}

static int cmd_stat(const vector<string>& a){
    if(a.size()<2){ cerr<<"usage: stat [file]\n"; return 1; }
    try{
        fs::path p=a[1];
        if(!fs::exists(p)){ cerr<<"not found\n"; return 1; }
        cout<<"Path: "<<p.u8string()<<"\n";
        cout<<"Type: "<<(fs::is_directory(p)?"dir":fs::is_regular_file(p)?"file":"other")<<"\n";
        if(fs::is_regular_file(p)) cout<<"Size: "<<fs::file_size(p)<<"\n";
        auto f=fs::last_write_time(p);
        auto schrono=chrono::time_point_cast<chrono::system_clock::duration>(f-decltype(f)::clock::now()+chrono::system_clock::now());
        time_t t=chrono::system_clock::to_time_t(schrono);
        cout<<"Modified: "<<string(ctime(&t));
        return 0;
    }catch(...){ cerr<<"stat error\n"; return 1; }
}

static int cmd_strings(const vector<string>& a){
    if(a.size()<2){ cerr<<"usage: strings [file]\n"; return 1; }
    ifstream f(a[1], ios::binary);
    if(!f){ cerr<<"cannot open\n"; return 1; }
    string buf((istreambuf_iterator<char>(f)), istreambuf_iterator<char>());
    string cur;
    for(unsigned char c: buf){
        if(isprint(c) && c<127){ cur.push_back((char)c); }
        else {
            if(cur.size()>=4){ cout<<cur<<"\n"; }
            cur.clear();
        }
    }
    if(cur.size()>=4) cout<<cur<<"\n";
    return 0;
}

static int cmd_basename(const vector<string>& a){
    if(a.size()<2){ cerr<<"usage: basename [path]\n"; return 1; }
    cout<<fs::path(a[1]).filename().u8string()<<"\n"; return 0;
}

static int cmd_dirname(const vector<string>& a){
    if(a.size()<2){ cerr<<"usage: dirname [path]\n"; return 1; }
    cout<<fs::path(a[1]).parent_path().u8string()<<"\n"; return 0;
}

static int cmd_sha256sum(const vector<string>& a){
    if(a.size()<2){ cerr<<"usage: sha256sum [file]\n"; return 1; }
#ifdef _WIN32
    return runSystem("certutil -hashfile \""+a[1]+"\" SHA256");
#else
    int rc=runSystem("sha256sum \""+a[1]+"\"");
    if(rc!=0) cerr<<"sha256sum not available\n";
    return rc;
#endif
}

static int cmd_md5sum(const vector<string>& a){
    if(a.size()<2){ cerr<<"usage: md5sum [file]\n"; return 1; }
#ifdef _WIN32
    return runSystem("certutil -hashfile \""+a[1]+"\" MD5");
#else
    int rc=runSystem("md5sum \""+a[1]+"\"");
    if(rc!=0) cerr<<"md5sum not available\n";
    return rc;
#endif
}

static int cmd_chmod(const vector<string>& a){
#ifdef _WIN32
    cout<<"chmod not supported on Windows\n"; return 1;
#else
    if(a.size()<3){ cerr<<"usage: chmod [perm] [file]\n"; return 1; }
    mode_t m=strtoul(a[1].c_str(),nullptr,8);
    if(::chmod(a[2].c_str(), m)!=0){ perror("chmod"); return 1; }
    return 0;
#endif
}

static int cmd_chown(const vector<string>& a){
#ifdef _WIN32
    cout<<"chown not supported on Windows\n"; return 1;
#else
    if(a.size()<3){ cerr<<"usage: chown [owner] [file]\n"; return 1; }
    struct passwd* pw=getpwnam(a[1].c_str());
    if(!pw){ cerr<<"no such user\n"; return 1; }
    if(::chown(a[2].c_str(), pw->pw_uid, (gid_t)-1)!=0){ perror("chown"); return 1; }
    return 0;
#endif
}

static int cmd_mount(const vector<string>& a){
#ifdef _WIN32
    return runSystem("mountvol");
#else
    return runSystem("mount");
#endif
}

static int cmd_netstat(const vector<string>& a){
    string args=join(vector<string>(a.begin()+1,a.end())," ");
    return runSystem("netstat "+args);
}

static int cmd_arp(const vector<string>& a){
    if(isWindows()) return runSystem("arp -a");
    else return runSystem("arp -a");
}

static int cmd_route(const vector<string>& a){
#ifdef _WIN32
    return runSystem("route print");
#else
    return runSystem("route -n");
#endif
}

static int cmd_taskkill(const vector<string>& a){
#ifdef _WIN32
    string args=join(vector<string>(a.begin()+1,a.end())," ");
    if(args.empty()){ cerr<<"usage: taskkill [/PID pid]\n"; return 1; }
    return runSystem("taskkill "+args);
#else
    return cmd_kill(a);
#endif
}

static int cmd_schtasks(const vector<string>& a){
#ifdef _WIN32
    return runSystem("schtasks");
#else
    cout<<"not available\n"; return 1;
#endif
}

static int cmd_reg(const vector<string>& a){
#ifdef _WIN32
    string args=join(vector<string>(a.begin()+1,a.end())," ");
    if(args.empty()){ cerr<<"usage: reg query|add|delete ...\n"; return 1; }
    return runSystem("reg "+args);
#else
    cout<<"not available\n"; return 1;
#endif
}

static int cmd_attrib(const vector<string>& a){
#ifdef _WIN32
    return runSystem("attrib "+join(vector<string>(a.begin()+1,a.end())," "));
#else
    cout<<"not available\n"; return 1;
#endif
}

static int cmd_more(const vector<string>& a){
    if(a.size()<2){ cerr<<"usage: more [file]\n"; return 1; }
#ifdef _WIN32
    return runSystem("more \""+a[1]+"\"");
#else
    return runSystem("more \""+a[1]+"\"");
#endif
}

static int cmd_sort(const vector<string>& a){
    if(a.size()<2){ cerr<<"usage: sort [file]\n"; return 1; }
    vector<string> lines; if(readFileLines(a[1],lines)) { cerr<<"cannot open\n"; return 1; }
    sort(lines.begin(), lines.end()); for(auto& s: lines) cout<<s<<"\n"; return 0;
}

static int cmd_uniq(const vector<string>& a){
    if(a.size()<2){ cerr<<"usage: uniq [file]\n"; return 1; }
    vector<string> lines; if(readFileLines(a[1],lines)) { cerr<<"cannot open\n"; return 1; }
    string last; for(auto& s: lines){ if(s!=last){ cout<<s<<"\n"; last=s; } } return 0;
}

static int cmd_split(const vector<string>& a){
    if(a.size()<2){ cerr<<"usage: split [file]\n"; return 1; }
    vector<string> lines; if(readFileLines(a[1],lines)) { cerr<<"cannot open\n"; return 1; }
    size_t half=lines.size()/2;
    ofstream a1("xaa"), a2("xab");
    for(size_t i=0;i<lines.size();++i){ (i<half?a1:a2)<<lines[i]<<"\n"; }
    return 0;
}

static int cmd_tee(const vector<string>& a){
    if(a.size()<2){ cerr<<"usage: tee [file]\n"; return 1; }
    string line; ofstream f(a[1], ios::app);
    while(getline(cin,line)){ cout<<line<<"\n"; f<<line<<"\n"; }
    return 0;
}

static int cmd_diff(const vector<string>& a){
    if(a.size()<3){ cerr<<"usage: diff [f1] [f2]\n"; return 1; }
    vector<string> A,B; if(readFileLines(a[1],A)||readFileLines(a[2],B)){ cerr<<"cannot open\n"; return 1; }
    size_t n=max(A.size(),B.size()); int rc=0;
    for(size_t i=0;i<n;++i){
        string s1=i<A.size()?A[i]:""; string s2=i<B.size()?B[i]:"";
        if(s1!=s2){ cout<<i+1<<" - "<<s1<<"\n"<<i+1<<" + "<<s2<<"\n"; rc=1; }
    }
    return rc;
}

static int cmd_cmp(const vector<string>& a){
    if(a.size()<3){ cerr<<"usage: cmp [f1] [f2]\n"; return 1; }
    ifstream f1(a[1], ios::binary), f2(a[2], ios::binary);
    if(!f1||!f2){ cerr<<"cannot open\n"; return 1; }
    istreambuf_iterator<char> i1(f1), i2(f2), e;
    size_t pos=0; for(; i1!=e && i2!=e; ++i1,++i2,++pos){ if(*i1!=*i2){ cout<<"differ: byte "<<pos+1<<"\n"; return 1; } }
    if(i1!=e || i2!=e){ cout<<"files differ in length\n"; return 1; }
    cout<<"files are identical\n"; return 0;
}

static string base64Encode(const vector<unsigned char>& data){
    static const char* t="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    string out; size_t i=0;
    while(i<data.size()){
        uint32_t v=0; int n=0;
        for(int j=0;j<3;++j){ v<<=8; if(i<data.size()){ v|=data[i++]; n++; } }
        for(int j=0;j<4;++j){
            if(j<=n) out.push_back(t[(v>>(18-6*j))&0x3F]);
            else out.push_back('=');
        }
        if(n==1) out[out.size()-1]='=';
    }
    return out;
}

static int cmd_base64(const vector<string>& a){
    if(a.size()<2){ cerr<<"usage: base64 [file]\n"; return 1; }
    ifstream f(a[1], ios::binary);
    if(!f){ cerr<<"cannot open\n"; return 1; }
    vector<unsigned char> buf((istreambuf_iterator<char>(f)), istreambuf_iterator<char>());
    cout<<base64Encode(buf)<<"\n"; return 0;
}

static int cmd_timeout_sleep(const vector<string>& a){
    if(a.size()<2){ cerr<<"usage: timeout/sleep [n]\n"; return 1; }
    int n=stoi(a[1]);
#ifdef _WIN32
    Sleep(n*1000);
#else
    sleep(n);
#endif
    return 0;
}

static int cmd_curl(const vector<string>& a){
    if(a.size()<2){ cerr<<"usage: curl [url]\n"; return 1; }
    return runSystem("curl -s "+a[1]);
}

static int cmd_wget(const vector<string>& a){
    if(a.size()<2){ cerr<<"usage: wget [url]\n"; return 1; }
#ifdef _WIN32
    return runSystem("powershell -c \"Invoke-WebRequest "+a[1]+" -OutFile out.txt\"");
#else
    return runSystem("wget "+a[1]);
#endif
}

static int cmd_sc(const vector<string>& a){
#ifdef _WIN32
    string args=join(vector<string>(a.begin()+1,a.end())," ");
    return runSystem("sc "+args);
#else
    cout<<"not available\n"; return 1;
#endif
}

static int cmd_driverquery(const vector<string>& a){
#ifdef _WIN32
    return runSystem("driverquery");
#else
    cout<<"not available\n"; return 1;
#endif
}

static int cmd_systeminfo(const vector<string>& a){
#ifdef _WIN32
    return runSystem("systeminfo");
#else
    return runSystem("uname -a");
#endif
}

static int cmd_ver(const vector<string>& a){
#ifdef _WIN32
    return runSystem("ver");
#else
    return runSystem("uname -r");
#endif
}

static int cmd_taskmgr(const vector<string>& a){
#ifdef _WIN32
    return runSystem("start taskmgr");
#else
    return runSystem("top");
#endif
}

static int list_ls(const vector<string>& a){
    bool optl=false, opta=false, opth=false;
    for(size_t i=1;i<a.size();++i){ if(a[i]=="-l") optl=true; else if(a[i]=="-a") opta=true; else if(a[i]=="-h"||a[i]=="-lh"||a[i]=="-hl") opth=true; }
    vector<fs::directory_entry> items;
    for(auto& e: fs::directory_iterator(fs::current_path())) items.push_back(e);
    sort(items.begin(), items.end(), [](auto& x, auto& y){ return x.path().filename().u8string()<y.path().filename().u8string(); });
    if(!optl){
        for(auto& e: items){
            string name=e.path().filename().u8string();
            if(!opta && !name.empty() && name[0]=='.') continue;
            cout<<name<<"\n";
        }
    } else {
        for(auto& e: items){
            string name=e.path().filename().u8string();
            if(!opta && !name.empty() && name[0]=='.') continue;
            uintmax_t size=0; try{ if(fs::is_regular_file(e)) size=fs::file_size(e); }catch(...){}
            string sz=opth?humanSize(size):to_string(size);
            cout<<(fs::is_directory(e)?"d":"-")<<" "<<setw(10)<<sz<<" "<<name<<"\n";
        }
    }
    return 0;
}

static int dispatch(vector<string> args);

static int expandAliasesAndEnvAndRun(const string& line){
    string s=line;
    for(auto& p: aliases){
        string key=p.first+" ";
        if(s.rfind(key,0)==0) s=p.second+" "+s.substr(key.size());
        if(s==p.first) s=p.second;
    }
    string out; out.reserve(s.size()*2);
    for(size_t i=0;i<s.size();){
        if(s[i]=='$'){
            size_t j=i+1;
            if(j<s.size() && s[j]=='{'){ size_t k=s.find('}',j+1); if(k==string::npos){ out.push_back(s[i++]); continue; } string var=s.substr(j+1,k-(j+1)); out+=getEnv(var); i=k+1; }
            else {
                size_t k=j; while(k<s.size() && (isalnum((unsigned char)s[k])||s[k]=='_')) k++;
                string var=s.substr(j,k-j); out+=getEnv(var); i=k;
            }
        } else {
            out.push_back(s[i++]);
        }
    }
    auto tokens=splitArgs(out);
    if(tokens.empty()) return 0;
    if(tokens[0]=="huntershell" && tokens.size()>2 && (tokens[1]=="/c" || tokens[1]=="-c")) {
        vector<string> sub(tokens.begin()+2,tokens.end());
        string one=join(sub," ");
        return expandAliasesAndEnvAndRun(one);
    }
    return dispatch(tokens);
}

static int dispatch(vector<string> args){
    if(args.empty()) return 0;
    string cmd=args[0];
    if(cmd=="help") return cmd_help(args);
    if(cmd=="ls") return list_ls(args);
    if(cmd=="pwd") return cmd_pwd(args);
    if(cmd=="clear") return cmd_clear(args);
    if(cmd=="cd") return cmd_cd(args);
    if(cmd=="whoami") return cmd_whoami(args);
    if(cmd=="hostname") return cmd_hostname(args);
    if(cmd=="date") return cmd_date(args);
    if(cmd=="time") return cmd_time(args);
    if(cmd=="uname") return cmd_uname(args);
    if(cmd=="history") return cmd_history(args);
    if(cmd=="man"){ cout<<"manual not available\n"; return 0; }
    if(cmd=="echo") return cmd_echo(args);
    if(cmd=="cat") return cmd_cat(args);
    if(cmd=="head") return cmd_head(args);
    if(cmd=="tail") return cmd_tail(args);
    if(cmd=="touch") return cmd_touch(args);
    if(cmd=="mkdir") return cmd_mkdir(args);
    if(cmd=="rmdir") return cmd_rmdir(args);
    if(cmd=="rm") return cmd_rm(args);
    if(cmd=="mv") return cmd_mv(args);
    if(cmd=="cp") return cmd_cp(args);
    if(cmd=="tree") return cmd_tree(args);
    if(cmd=="find") return cmd_find(args);
    if(cmd=="grep") return cmd_grep(args);
    if(cmd=="wc") return cmd_wc(args);
    if(cmd=="ps") return cmd_ps(args);
    if(cmd=="kill") return cmd_kill(args);
    if(cmd=="which") return cmd_which(args);
    if(cmd=="locate") return cmd_locate(args);
    if(cmd=="uptime") return cmd_uptime(args);
    if(cmd=="df") return cmd_df(args);
    if(cmd=="free") return cmd_free(args);
    if(cmd=="env") return cmd_env(args);
    if(cmd=="export") return cmd_export(args);
    if(cmd=="alias") return cmd_alias(args);
    if(cmd=="ifconfig"||cmd=="ipconfig") return cmd_ifconfig(args);
    if(cmd=="ping") return cmd_ping(args);
    if(cmd=="tracert"||cmd=="traceroute") return cmd_tracert(args);
    if(cmd=="nslookup") return cmd_nslookup(args);
    if(cmd=="who") return cmd_who(args);
    if(cmd=="users") return cmd_users(args);
    if(cmd=="id") return cmd_id(args);
    if(cmd=="lsusb") return cmd_lsusb(args);
    if(cmd=="lspci") return cmd_lspci(args);
    if(cmd=="top"||cmd=="htop") return cmd_top(args);
    if(cmd=="du") return cmd_du(args);
    if(cmd=="stat") return cmd_stat(args);
    if(cmd=="strings") return cmd_strings(args);
    if(cmd=="basename") return cmd_basename(args);
    if(cmd=="dirname") return cmd_dirname(args);
    if(cmd=="sha256sum") return cmd_sha256sum(args);
    if(cmd=="md5sum") return cmd_md5sum(args);
    if(cmd=="chmod") return cmd_chmod(args);
    if(cmd=="chown") return cmd_chown(args);
    if(cmd=="mount") return cmd_mount(args);
    if(cmd=="netstat") return cmd_netstat(args);
    if(cmd=="arp") return cmd_arp(args);
    if(cmd=="route") return cmd_route(args);
    if(cmd=="taskkill") return cmd_taskkill(args);
    if(cmd=="schtasks") return cmd_schtasks(args);
    if(cmd=="reg") return cmd_reg(args);
    if(cmd=="attrib") return cmd_attrib(args);
    if(cmd=="more") return cmd_more(args);
    if(cmd=="sort") return cmd_sort(args);
    if(cmd=="uniq") return cmd_uniq(args);
    if(cmd=="split") return cmd_split(args);
    if(cmd=="tee") return cmd_tee(args);
    if(cmd=="diff") return cmd_diff(args);
    if(cmd=="cmp") return cmd_cmp(args);
    if(cmd=="base64") return cmd_base64(args);
    if(cmd=="timeout"||cmd=="sleep") return cmd_timeout_sleep(args);
    if(cmd=="curl") return cmd_curl(args);
    if(cmd=="wget") return cmd_wget(args);
    if(cmd=="sc") return cmd_sc(args);
    if(cmd=="driverquery") return cmd_driverquery(args);
    if(cmd=="systeminfo") return cmd_systeminfo(args);
    if(cmd=="ver") return cmd_ver(args);
    if(cmd=="taskmgr") return cmd_taskmgr(args);
    if(cmd=="exit"||cmd=="logout") return -9999;
    string full;
    if(isExecutableInPath(cmd,full)) {
        vector<string> tail(args.begin()+1,args.end());
        string cmdline="\""+full+"\"";
        for(auto& t: tail){ cmdline+=" \""+t+"\""; }
        return runSystem(cmdline);
    }
    int rc=runSystem(join(args," "));
    if(rc!=0) cerr<<cmd<<": command not found\n";
    return rc;
}

static string makePrompt() {
    string ESC="\x1b";
    string GREEN=ESC+"[32m";
    string YELLOW=ESC+"[33m";
    string RESET=ESC+"[0m";
    string user=getUserName();
    string host=getHostName();
    string cwd=currentDir();
    return GREEN+user+"@"+host+":~$"+RESET+" "+YELLOW+cwd+RESET+" ";
}

static void setTitle() {
#ifdef _WIN32
    string path=currentDir();
    wstring w=fromUTF8(path);
    SetConsoleTitleW(w.c_str());
#endif
}

static void initHistory() {
    string t=tempDir();
    fs::create_directories(t);
    historyFilePath = (fs::path(t) / "huntershell_history.txt").string();
}

static int interactiveLoop() {
    string line;
    while(true){
        setTitle();
        cout<<makePrompt();
        if(!std::getline(cin,line)) break;
        line=trim(line);
        if(line.empty()) continue;
        writeHistory(line);
        int rc=expandAliasesAndEnvAndRun(line);
        if(rc==-9999) break;
    }
    return 0;
}

size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    ((string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

bool isVersionOlder(const string& latest, const string& current) {
    string latestClean = latest[0]=='v'?latest.substr(1):latest;
    return latestClean != current;
}

void checkForUpdate() {
    string url = "https://huntershell.vercel.app/hunter/shell/api/releases/latest/info";
    string response;
    CURL* curl = curl_easy_init();
    if(curl) {
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        CURLcode res = curl_easy_perform(curl);
        curl_easy_cleanup(curl);
        if(res != CURLE_OK) return;
    }
    try {
        auto j = json::parse(response);
        string latestVersion = j["latest"];
        if(isVersionOlder(latestVersion, VERSION)){
            cout << "\033[33mThe version you're using is outdated, new version is available. Version: " << latestVersion << "\033[0m" << endl;
        }
    } catch(...) {}
}

int main(int argc, char** argv){
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
    enableANSIOnWindows();
    initHistory();
    checkForUpdate();
    if(argc>=3 && (string(argv[1])=="/c" || string(argv[1])=="-c")){
        vector<string> vec;
        for(int i=2;i<argc;++i) vec.push_back(argv[i]);
        string cmd=join(vec," ");
        int rc=expandAliasesAndEnvAndRun(cmd);
        return rc<0?0:rc;
    }
    if(argc>1){
        vector<string> vec;
        for(int i=1;i<argc;++i) vec.push_back(argv[i]);
        string cmd=join(vec," ");
        int rc=expandAliasesAndEnvAndRun(cmd);
        return rc<0?0:rc;
    }
    return interactiveLoop();
}
