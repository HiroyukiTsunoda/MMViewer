#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <gdiplus.h>
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <stdexcept>
#include <thread>
#include <vector>
#include <chrono>
#include <map>
#include <condition_variable>
#include <cmath>
#include <deque>
#include <unordered_map>
#include <utility>
#include <cstring>
#include <string_view>
#include "core.h"
#include "startup.h"
#include "single_instance.h"
#include "folder_icons.h"
#include "viewer.h"
#include "resource.h"

namespace fs = std::filesystem;
constexpr wchar_t WindowClass[] = L"MMViewer.Native.Main.v1";
constexpr UINT LoadDone = WM_APP + 1, TreeDone = WM_APP + 2, WatchDone = WM_APP + 3, StartupDone = WM_APP + 4;
constexpr UINT_PTR RootDragTimer = 0x4d4d;
enum { UiOpenFile=101, OpenFolder, NewTab, CloseTab, Sidebar, Theme, Source, Wide, Find, Reload,
       ZoomIn, ZoomOut, ZoomReset, Reopen, SearchNext, SearchPrev, SearchClose, CollapseAll, ExpandAll, About, RemoveFromList, CloseAllTabs };
struct FolderPathLess {
 bool operator()(const std::wstring& left,const std::wstring& right) const {return _wcsicmp(left.c_str(),right.c_str())<0;}
};
// Hash and equality fold ASCII case only, and in the same way. Paths that
// differ in other letters are distinct keys here; SelectTreeFile walks the
// control for those, as its own comparison is the locale-aware one.
struct PathHash {
 size_t operator()(const std::wstring& path) const {size_t hash=14695981039346656037ULL;for(wchar_t c:path){hash^=static_cast<size_t>(c>=L'A'&&c<=L'Z'?c+32:c);hash*=1099511628211ULL;}return hash;}
};
struct PathEqual {
 bool operator()(const std::wstring& left,const std::wstring& right) const {
    if(left.size()!=right.size())return false;
    for(size_t i=0;i<left.size();++i){wchar_t a=left[i],b=right[i];if(a>=L'A'&&a<=L'Z')a+=32;if(b>=L'A'&&b<=L'Z')b+=32;if(a!=b)return false;}
    return true;
 }
};
struct Tab {
    std::wstring path, encoding, status, registrationIssue;
    int font=16; double scroll=0; bool source=false, cp932=false, autoRefresh=true;
    unsigned long long id=0;
};
struct Loaded {
    unsigned long long id=0, generation=0;
    std::shared_ptr<mm::Document> doc;
    std::shared_ptr<std::atomic_bool> cancel;
    std::string error;
    WIN32_FILE_ATTRIBUTE_DATA stamp{};
    bool stampValid=false, force=false;
};
struct CachedDocument {
    unsigned long long id=0;
    std::shared_ptr<mm::Document> doc;
    WIN32_FILE_ATTRIBUTE_DATA stamp{};
    size_t bytes=0;
    bool cp932=false, stampValid=false;
};
struct Scanned { unsigned long long folderId=0,generation=0; std::unique_ptr<mm::FolderNode> tree; std::wstring error; mm::FolderScanCoverage coverage; };
struct FolderRegistration {
    std::wstring path,issue;
    std::unique_ptr<mm::FolderNode> tree;
    mm::FolderNode pending;
    // Nodes whose rows are still being deleted (the rows point at them), the
    // newest scan result waiting for those rows, and the count of queued work.
    std::list<mm::FolderNode> retiring;
    std::vector<std::unique_ptr<mm::FolderNode>> retiringRoots;
    std::unique_ptr<mm::FolderNode> deferredTree;bool deferred=false;
    size_t queuedTasks=0;
    unsigned long long id=0,generation=0;
    std::shared_ptr<std::atomic_bool> cancel;
    bool scanning=false,rescanRequested=false;
};
struct StartupChecked {unsigned long long generation=0;std::vector<mm::PathCheck> checks;};
// One unit of native tree work, executed in bounded slices on the UI thread.
struct TreeTask {
    enum Kind { Insert, Delete, ExpandAll, CollapseAll } kind=Insert;
    FolderRegistration* folder=nullptr;
    // Insert: resumable depth-first insertion of the children [first,stop) of
    // node under row, starting after the row `after`, with their descendants.
    // A partial run leaves the parent row's state alone; a large run closes an
    // open parent while its rows arrive and reopens it at the end.
    struct Frame { mm::FolderNode* node; HTREEITEM row; std::list<mm::FolderNode>::iterator next; HTREEITEM last; std::list<mm::FolderNode>::iterator stop; bool partial=false; };
    std::vector<Frame> frames;mm::FolderNode* node=nullptr;HTREEITEM row=nullptr,after=nullptr;std::list<mm::FolderNode>::iterator first,stop;bool partial=false,started=false,reopen=false;size_t estimate=0;
    // Delete: the subtrees seeded in `pending` (siblings under row) are
    // collected parent-first, then deleted in reverse order. A large batch
    // closes an open parent while its rows go and reopens it at the end.
    // ExpandAll / CollapseAll: directory rows in processing order.
    std::vector<HTREEITEM> rows,pending;size_t position=0;bool collected=false;
};
class Worker {
 std::mutex mutex; std::condition_variable_any ready; std::function<void()> pending;
 std::deque<std::pair<unsigned long long,std::function<void()>>> queued;
 std::vector<std::function<void()>> cleanup;
 bool busy=false; // Set under the mutex while a job runs, so Pending counts it.
 std::jthread thread{[this](std::stop_token stop){for(;;){std::function<void()> work;{std::unique_lock lock(mutex);ready.wait(lock,stop,[&]{return bool(pending)||!queued.empty()||!cleanup.empty();});if(!cleanup.empty()){work=std::move(cleanup.back());cleanup.pop_back();}else if(stop.stop_requested())break;else if(pending){work=std::move(pending);pending={};}else if(!queued.empty()){work=std::move(queued.front().second);queued.pop_front();}busy=bool(work);}if(work){work();std::lock_guard lock(mutex);busy=false;}}}};
public:
 void Queue(std::function<void()> work){{std::lock_guard lock(mutex);pending=std::move(work);}ready.notify_one();}
 void Append(unsigned long long key,std::function<void()> work){{std::lock_guard lock(mutex);auto old=std::find_if(queued.begin(),queued.end(),[&](const auto& entry){return entry.first==key;});if(old!=queued.end())old->second=std::move(work);else queued.emplace_back(key,std::move(work));}ready.notify_one();}
 // Cleanup owns retired I/O buffers and must run even when replaceable work is
 // cancelled; dropping its closure on the UI thread could wait for disk I/O.
 void Cleanup(std::function<void()> work){{std::lock_guard lock(mutex);cleanup.push_back(std::move(work));}ready.notify_one();}
 void CancelPending(){std::lock_guard lock(mutex);pending={};queued.clear();if(thread.joinable())CancelSynchronousIo(thread.native_handle());}
 size_t Pending(){std::lock_guard lock(mutex);return queued.size()+(pending?1:0)+(busy?1:0);}
 void Stop(){thread.request_stop();CancelPending();ready.notify_all();if(thread.joinable())thread.join();}
 ~Worker(){Stop();}
};
struct Watch {
    HANDLE dir=INVALID_HANDLE_VALUE, event=nullptr; OVERLAPPED ov{};
    // 64 KiB absorbs a build's burst of notifications instead of overflowing
    // into a full rescan, and is the largest size accepted for network shares.
    std::vector<BYTE> data=std::vector<BYTE>(65536);
    bool recursive=false,treeWatch=false,pending=false; std::wstring path;
    void Cancel(){if(pending&&dir!=INVALID_HANDLE_VALUE)CancelIoEx(dir,&ov);}
    ~Watch(){ if(dir!=INVALID_HANDLE_VALUE){Cancel();if(pending){DWORD ignored;GetOverlappedResult(dir,&ov,&ignored,TRUE);}CloseHandle(dir);}if(event)CloseHandle(event);}
    bool Arm(){
      for(;;){
        ResetEvent(event);ov={};ov.hEvent=event;
        pending=ReadDirectoryChangesW(dir,data.data(),DWORD(data.size()),recursive,
          FILE_NOTIFY_CHANGE_FILE_NAME|FILE_NOTIFY_CHANGE_DIR_NAME|FILE_NOTIFY_CHANGE_LAST_WRITE|FILE_NOTIFY_CHANGE_SIZE,nullptr,&ov,nullptr)!=FALSE;
        // A share that rejects the large buffer still works with the earlier size.
        if(pending||GetLastError()!=ERROR_INVALID_PARAMETER||data.size()<=4096)return pending;
        data.assign(4096,0);
      }
    }
};
struct WatchSpec {bool recursive=false,treeWatch=false;};
using WatchPlan=std::map<std::wstring,WatchSpec,FolderPathLess>;
struct Watched {unsigned long long generation=0;WatchPlan plan;std::vector<std::unique_ptr<Watch>> watches;bool limited=false,rescanAfter=false,retry=false,recovery=false;};

// One-shot reader for session.ini. Startup previously issued one profile API
// call per key, and every call reparsed the whole file, so restoring many
// tabs grew quadratically. The rules mirror the profile functions: a leading
// FF FE selects UTF-16LE and anything else is the ANSI code page; section and
// key lookups ignore case; only the first section of a name is consulted and
// the first key of a name within it wins; values lose surrounding blanks and
// one pair of matching quotes.
struct SessionFile {
    struct Section { std::wstring name; std::vector<std::pair<std::wstring,std::wstring>> keys; };
    std::vector<Section> sections;
    static constexpr size_t MaximumBytes = 16 * 1024 * 1024;
    static SessionFile Load(const std::wstring& path) {
        SessionFile result;
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return result;
        LARGE_INTEGER size{};
        // An unreadable or implausibly large file parses as empty, so Restore
        // can still fall back to the previous complete session in .bak.
        if (!GetFileSizeEx(file, &size) || size.QuadPart > static_cast<LONGLONG>(MaximumBytes)) { CloseHandle(file); return result; }
        std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
        size_t used = 0;
        while (used < bytes.size()) {
            DWORD count = 0;
            if (!ReadFile(file, bytes.data() + used, DWORD(std::min<size_t>(bytes.size() - used, 1 << 20)), &count, nullptr) || !count) break;
            used += count;
        }
        CloseHandle(file);
        bytes.resize(used);
        std::wstring text;
        if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xff && static_cast<unsigned char>(bytes[1]) == 0xfe) {
            text.resize((bytes.size() - 2) / 2);
            if (!text.empty()) memcpy(text.data(), bytes.data() + 2, text.size() * sizeof(wchar_t));
        } else if (!bytes.empty()) {
            const int length = MultiByteToWideChar(CP_ACP, 0, bytes.data(), int(bytes.size()), nullptr, 0);
            if (length > 0) { text.resize(size_t(length)); MultiByteToWideChar(CP_ACP, 0, bytes.data(), int(bytes.size()), text.data(), length); }
        }
        result.Parse(text);
        return result;
    }
    static std::wstring_view Trim(std::wstring_view value) {
        while (!value.empty() && (value.front() == L' ' || value.front() == L'\t')) value.remove_prefix(1);
        while (!value.empty() && (value.back() == L' ' || value.back() == L'\t')) value.remove_suffix(1);
        return value;
    }
    static bool Same(std::wstring_view left, std::wstring_view right) {
        return left.size() == right.size() && _wcsnicmp(left.data(), right.data(), left.size()) == 0;
    }
    void Parse(const std::wstring& text) {
        Section* current = nullptr;
        for (size_t at = 0; at <= text.size();) {
            size_t end = text.find_first_of(L"\r\n", at);
            if (end == std::wstring::npos) end = text.size();
            const auto line = Trim(std::wstring_view(text).substr(at, end - at));
            at = end + 1;
            if (line.empty() || line.front() == L';') continue;
            if (line.front() == L'[') {
                const auto close = line.find(L']');
                const auto name = Trim(line.substr(1, close == std::wstring_view::npos ? std::wstring_view::npos : close - 1));
                current = nullptr;
                bool seen = false;
                for (const auto& section : sections) if (Same(section.name, name)) { seen = true; break; }
                if (!seen) { sections.push_back({std::wstring(name), {}}); current = &sections.back(); }
                continue;
            }
            if (!current) continue;
            const auto equals = line.find(L'=');
            if (equals == std::wstring_view::npos) continue;
            const auto key = Trim(line.substr(0, equals));
            auto value = Trim(line.substr(equals + 1));
            if (value.size() >= 2 && (value.front() == L'"' || value.front() == L'\'') && value.back() == value.front()) value = value.substr(1, value.size() - 2);
            bool duplicate = false;
            for (const auto& entry : current->keys) if (Same(entry.first, key)) { duplicate = true; break; }
            if (!duplicate) current->keys.emplace_back(std::wstring(key), std::wstring(value));
        }
    }
    const std::wstring* Find(const wchar_t* section, const wchar_t* key) const {
        for (const auto& entry : sections) {
            if (!Same(entry.name, section)) continue;
            for (const auto& [name, value] : entry.keys) if (Same(name, key)) return &value;
            return nullptr;
        }
        return nullptr;
    }
    bool Has(const wchar_t* section, const wchar_t* key) const { return Find(section, key) != nullptr; }
    std::wstring Get(const wchar_t* section, const wchar_t* key, const wchar_t* fallback = L"") const {
        const auto* value = Find(section, key);
        return value ? *value : std::wstring(fallback);
    }
    int Int(const wchar_t* section, const wchar_t* key, int fallback) const {
        const auto* value = Find(section, key);
        // GetPrivateProfileInt returns the default for a missing or empty value.
        return value && !value->empty() ? Integer(*value) : fallback;
    }
    // GetPrivateProfileInt semantics: blanks, a sign, an optional 0x prefix,
    // digits up to the first other character, 32-bit wraparound, else zero.
    static int Integer(std::wstring_view text) {
        size_t at = 0;
        while (at < text.size() && (text[at] == L' ' || text[at] == L'\t')) ++at;
        bool negative = false;
        if (at < text.size() && (text[at] == L'+' || text[at] == L'-')) { negative = text[at] == L'-'; ++at; }
        unsigned base = 10;
        if (at + 1 < text.size() && text[at] == L'0' && (text[at + 1] == L'x' || text[at + 1] == L'X')) { base = 16; at += 2; }
        unsigned long long magnitude = 0;
        for (; at < text.size(); ++at) {
            unsigned digit = 0;
            if (text[at] >= L'0' && text[at] <= L'9') digit = text[at] - L'0';
            else if (base == 16 && text[at] >= L'a' && text[at] <= L'f') digit = 10 + text[at] - L'a';
            else if (base == 16 && text[at] >= L'A' && text[at] <= L'F') digit = 10 + text[at] - L'A';
            else break;
            magnitude = (magnitude * base + digit) & 0xffffffffull;
        }
        const auto value = static_cast<unsigned long>((negative ? 0ull - magnitude : magnitude) & 0xffffffffull);
        return static_cast<int>(value);
    }
    std::vector<std::wstring> SectionNames() const {
        std::vector<std::wstring> names;
        names.reserve(sections.size());
        for (const auto& section : sections) names.push_back(section.name);
        return names;
    }
};

class App {
public:
 HWND hwnd{},tabsH{},tabScrollH{},treeH{},splitH{},searchH{},countH{}; mm::NativeDocumentView view;
 std::vector<Tab> tabs,closed; int active=-1, fontDefault=16; unsigned long long nextId=1,loadGeneration=0,nextFolderId=1;
 Worker readWorker,watchWorker;Worker scanners[4]; std::shared_ptr<std::atomic_bool> readCancel,readRecheck,watchCancel,encodingPromptCancel; unsigned long long displayedTabId=0,watchGeneration=0;
 std::jthread startupThread;std::shared_ptr<std::atomic_bool> startupCancel;unsigned long long startupGeneration=0;bool startupChecking=false;
 std::vector<std::unique_ptr<FolderRegistration>> folders; std::vector<std::unique_ptr<Watch>> watches;
 std::set<std::wstring,FolderPathLess> watchRescanFolders;
 std::set<std::wstring,FolderPathLess> expanded;
 std::set<std::wstring,FolderPathLess> excludedFolders;
 // Directory paths the tree currently shows, rebuilt from the model whenever
 // the tree changes. Notification handling never walks the native control.
 std::set<std::wstring,FolderPathLess> knownFolders;
 // File rows by path, so the active tab's row is found without walking the
 // control. An entry leaves in TVN_DELETEITEM, only while it names that row.
 std::unordered_map<std::wstring,HTREEITEM,PathHash,PathEqual> rowsByPath;
 unsigned long long pollCount=0; // PollWatches runs; diagnostics and tests.
 std::map<std::wstring,int> tabNameCounts;
 std::wstring ini; std::shared_ptr<mm::Document> currentDoc;
 // UI-thread LRU. Closed-tab history stores settings only; decoded document
 // bodies are bounded independently of the number of open registrations.
 static constexpr size_t DocumentCacheCount=8, DocumentCacheBytes=32*1024*1024;
 std::deque<CachedDocument> documentCache;
 size_t documentCacheBytes=0;
 // Row work queue; see the folder tree section. fastExit skips destroying the
 // window at close so that thousands of rows never delay shutdown.
 static constexpr UINT_PTR TreeTaskTimer=6;
 static constexpr long long TreeSliceMs=25;
 static constexpr size_t SyncInsertRows=200,SyncDeleteRows=30;
 std::deque<TreeTask> treeTasks;bool treeTaskRunning=false,fastExit=false;unsigned finishingTreeTasks=0;
 std::vector<std::unique_ptr<FolderRegistration>> retiringFolders;
 struct TreeUpdateStats{size_t rowsInserted=0,rowsDeleted=0,slices=0;double totalMs=0,maxSliceMs=0;} treeStats;
 std::chrono::steady_clock::time_point treeWorkStarted{};
 bool loading=false,treeDirty=false,watchLimited=false,watchRescanPending=false,watchRetryPending=false,watchSetupPending=false; bool dark=false,sidebar=true,wide=false,searching=false,closing=false,treeUpdating=false,dragSplit=false;
 bool saveRetryPending=false,closePrompt=false;
 int sideWidth=280,winW=1100,winH=800,tabDrag=-1,splitStartX=0,splitStartWidth=0; POINT dragStart{};bool splitHover=false;
 unsigned long long rootDragId=0,rootDropId=0;bool rootDragging=false,rootDropAfter=false;POINT rootDragStart{},rootDragPoint{};
 int treeWheelRemainder=0,tabWheelRemainder=0;
 unsigned long long watchOverflowCount=0; // Completions whose buffer overflowed; diagnostics and tests.
 HFONT uiFont{}; HBRUSH background{},panel{},editBrush{}; HIMAGELIST folderImages{}; UINT dpi=96;
 HICON appIcon{},appSmallIcon{};
 std::vector<std::pair<int,HWND>> buttons;
 std::wstring notice=L"フォルダを開いてMarkdownを読み始めましょう";
 static LRESULT CALLBACK Proc(HWND h,UINT m,WPARAM w,LPARAM l) {
   App* a=reinterpret_cast<App*>(GetWindowLongPtrW(h,GWLP_USERDATA));
   if(m==WM_NCCREATE){a=static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);a->hwnd=h;SetWindowLongPtrW(h,GWLP_USERDATA,LONG_PTR(a));}
   return a?a->Message(m,w,l):DefWindowProcW(h,m,w,l);
 }
 int D(int n) const{return MulDiv(n,dpi,96);}
 int SidebarMaxPixels(int width)const{return std::max(D(160),std::min(D(600),width-D(300)-D(4)));}
 int SidebarPixels(int width)const{return sidebar?std::clamp(D(sideWidth),D(160),SidebarMaxPixels(width)):0;}
 COLORREF Bg()const{return dark?RGB(20,24,31):RGB(255,255,255);}
 COLORREF Panel()const{return dark?RGB(29,34,43):RGB(244,246,249);}
 COLORREF Text()const{return dark?RGB(226,232,241):RGB(30,40,55);}
 COLORREF Muted()const{return dark?RGB(149,161,180):RGB(99,113,132);}
 COLORREF Accent()const{return dark?RGB(117,177,255):RGB(31,105,207);}
 bool HasFolders()const{return !folders.empty();}
 FolderRegistration* FindFolder(const std::wstring& path)const{for(const auto& folder:folders)if(_wcsicmp(folder->path.c_str(),path.c_str())==0)return folder.get();return nullptr;}
 FolderRegistration* FindFolder(unsigned long long id)const{for(const auto& folder:folders)if(folder->id==id)return folder.get();return nullptr;}
 std::vector<std::wstring> FolderPaths()const{std::vector<std::wstring> paths;paths.reserve(folders.size());for(const auto& folder:folders)paths.push_back(folder->path);return paths;}
 std::wstring ReadIni(const wchar_t* section,const wchar_t* key,const wchar_t* fallback=L"") {
    std::vector<wchar_t> s(32768); GetPrivateProfileStringW(section,key,fallback,s.data(),DWORD(s.size()),ini.c_str()); return s.data();
 }
 // The argument-free overloads read the file themselves; Restore parses it
 // once and shares that result with every step.
 void RestoreExcludedFolders(){RestoreExcludedFolders(SessionFile::Load(ini));}
 void RestoreExcludedFolders(const SessionFile& session){
    excludedFolders.clear();
    const int count=std::clamp(session.Int(L"App",L"ExcludedFolders",0),0,5000);
    for(int i=0;i<count;i++){auto path=session.Get(L"ExcludedFolders",std::to_wstring(i).c_str());if(!path.empty())excludedFolders.insert(std::move(path));}
 }
 void RestoreFolders(){RestoreFolders(SessionFile::Load(ini));}
 void RestoreFolders(const SessionFile& session){
    folders.clear();std::set<std::wstring,FolderPathLess> seen;
    auto add=[&](std::wstring path){
      if(path.empty())return;
      try{path=mm::NormalizePath(path);}catch(const std::exception&){return;}
      if(!seen.insert(path).second)return;
      auto folder=std::make_unique<FolderRegistration>();folder->path=std::move(path);folder->id=nextFolderId++;
      auto name=fs::path(folder->path).filename().wstring();folder->pending={name.empty()?folder->path:name,folder->path,true,{}};
      folders.push_back(std::move(folder));
    };
    // An explicitly saved empty list must never revive the legacy root.
    if(!session.Has(L"App",L"Roots")){add(session.Get(L"App",L"Root"));return;}
    const int count=std::clamp(session.Int(L"App",L"Roots",0),0,5000);
    for(int i=0;i<count;i++)add(session.Get(L"Roots",std::to_wstring(i).c_str()));
 }
 void Restore(){
    PWSTR appData=nullptr; if(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&appData))){ini=std::wstring(appData)+L"\\MMViewer";CoTaskMemFree(appData);}
    else {wchar_t tmp[MAX_PATH];GetTempPathW(MAX_PATH,tmp);ini=std::wstring(tmp)+L"MMViewer";}
    fs::create_directories(ini);ini+=L"\\session.ini";
    auto session=SessionFile::Load(ini);
    if(session.Int(L"App",L"Format",0)!=1){
      auto backup=SessionFile::Load(ini+L".bak");
      if(backup.Int(L"App",L"Format",0)==1){CopyFileW((ini+L".bak").c_str(),ini.c_str(),FALSE);session=std::move(backup);}
    }
    DWORD light=1,len=sizeof(light); RegGetValueW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",L"AppsUseLightTheme",RRF_RT_REG_DWORD,nullptr,&light,&len);
    dark=session.Int(L"App",L"Dark",light?0:1)!=0;
    sidebar=session.Int(L"App",L"Sidebar",1)!=0;
    wide=session.Int(L"App",L"Wide",0)!=0;
    sideWidth=std::clamp(session.Int(L"App",L"SidebarWidth",280),160,600);
    winW=std::clamp(session.Int(L"App",L"Width",1100),640,3000);
    winH=std::clamp(session.Int(L"App",L"Height",800),480,2000);
    fontDefault=std::clamp(session.Int(L"App",L"Font",16),10,40);
    RestoreFolders(session);
    // Enumerate actual sections rather than looping over an untrusted count.
    // Numeric order preserves Tab2 before Tab10, including sessions over 100 tabs.
    std::map<unsigned long long,std::wstring> savedTabs;
    for(const auto& name:session.SectionNames()){
      if(_wcsnicmp(name.c_str(),L"Tab",3)!=0||name.size()<=3)continue;
      unsigned long long index=0;bool valid=true;
      for(const wchar_t* digit=name.c_str()+3;*digit;++digit){if(*digit<L'0'||*digit>L'9'||index>((~0ull)-(*digit-L'0'))/10){valid=false;break;}index=index*10+(*digit-L'0');}
      if(valid)savedTabs.try_emplace(index,name);
    }
    const auto savedActive=std::max(0,session.Int(L"App",L"Active",0));
    int restoredActive=-1;
    for(const auto&[index,sec]:savedTabs){Tab t;t.path=session.Get(sec.c_str(),L"Path");if(t.path.empty())continue;t.id=nextId++;
      t.font=std::clamp(session.Int(sec.c_str(),L"Font",fontDefault),10,40);
      const auto scroll=_wtof(session.Get(sec.c_str(),L"Scroll",L"0").c_str());t.scroll=std::isfinite(scroll)?std::clamp(scroll,0.0,1.0):0.0;
      t.source=session.Int(sec.c_str(),L"Source",0)!=0;t.cp932=session.Int(sec.c_str(),L"CP932",0)!=0;
      t.autoRefresh=session.Int(sec.c_str(),L"AutoRefresh",1)!=0;
      if(restoredActive<0||index<=static_cast<unsigned long long>(savedActive))restoredActive=int(tabs.size());tabs.push_back(std::move(t));
    }
    active=restoredActive;
    const int n=std::min(5000,session.Int(L"App",L"Expanded",0));
    for(int i=0;i<n;i++)expanded.insert(session.Get(L"Expanded",std::to_wstring(i).c_str()));
    RestoreExcludedFolders(session);
 }
 // Autosave after every scroll pause skips the disk flush: the write and the
 // atomic replacement stay, and a torn file falls back to the .bak session.
 // Closing and explicit saves remain durable.
 bool Save(bool durable=true){
    if(ini.empty())return false;Capture();
    // Assemble Format 1 in memory. Per-key profile writes repeatedly reparsed
    // and flushed the entire INI while the UI thread was saving many tabs.
    std::wstring text=L"\ufeff[App]\r\n";
    const auto set=[&](const std::wstring& key,const std::wstring& value){text+=key+L"="+value+L"\r\n";};
    const auto section=[&](const std::wstring& name){text+=L"\r\n["+name+L"]\r\n";};
    set(L"Format",L"1");set(L"Dark",std::to_wstring(dark));set(L"Sidebar",std::to_wstring(sidebar));set(L"SidebarWidth",std::to_wstring(sideWidth));
    set(L"Wide",std::to_wstring(wide));set(L"Root",L"");set(L"Font",std::to_wstring(fontDefault));set(L"Roots",std::to_wstring(folders.size()));
    WINDOWPLACEMENT wp{sizeof(wp)};const bool placed=GetWindowPlacement(hwnd,&wp)!=FALSE;
    set(L"Width",std::to_wstring(placed?MulDiv(wp.rcNormalPosition.right-wp.rcNormalPosition.left,96,dpi):winW));
    set(L"Height",std::to_wstring(placed?MulDiv(wp.rcNormalPosition.bottom-wp.rcNormalPosition.top,96,dpi):winH));
    set(L"Active",std::to_wstring(active));set(L"Tabs",std::to_wstring(tabs.size()));
    set(L"Expanded",std::to_wstring(expanded.size()));set(L"ExcludedFolders",std::to_wstring(excludedFolders.size()));
    section(L"Roots");for(size_t i=0;i<folders.size();++i)set(std::to_wstring(i),folders[i]->path);
    for(size_t i=0;i<tabs.size();++i){section(L"Tab"+std::to_wstring(i));const auto& tab=tabs[i];
      set(L"Path",tab.path);set(L"Font",std::to_wstring(tab.font));set(L"Scroll",std::to_wstring(tab.scroll));
      set(L"Source",std::to_wstring(tab.source));set(L"CP932",std::to_wstring(tab.cp932));set(L"AutoRefresh",std::to_wstring(tab.autoRefresh));}
    section(L"Expanded");size_t index=0;for(const auto& path:expanded)set(std::to_wstring(index++),path);
    section(L"ExcludedFolders");index=0;for(const auto& path:excludedFolders)set(std::to_wstring(index++),path);
    const auto tmp=ini+L".tmp";HANDLE file=CreateFileW(tmp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return false;
    const auto* data=reinterpret_cast<const BYTE*>(text.data());size_t remaining=text.size()*sizeof(wchar_t);bool written=true;
    while(remaining){DWORD bytes=0;const auto chunk=DWORD(std::min<size_t>(remaining,1024*1024));
      if(!WriteFile(file,data,chunk,&bytes,nullptr)||bytes==0){written=false;break;}data+=bytes;remaining-=bytes;}
    if(written&&durable)written=FlushFileBuffers(file)!=FALSE;if(!CloseHandle(file))written=false;
    if(!written){DeleteFileW(tmp.c_str());return false;}
    // The previous complete session becomes the backup in the same replacement
    // operation. A failed write/replace must never install a partial session.
    bool replaced=ReplaceFileW(ini.c_str(),tmp.c_str(),(ini+L".bak").c_str(),0,nullptr,nullptr)!=FALSE;
    if(!replaced&&GetLastError()==ERROR_FILE_NOT_FOUND&&GetFileAttributesW(ini.c_str())==INVALID_FILE_ATTRIBUTES)
      replaced=MoveFileExW(tmp.c_str(),ini.c_str(),MOVEFILE_WRITE_THROUGH)!=FALSE;
    if(!replaced)DeleteFileW(tmp.c_str());return replaced;
 }
 void CancelStartupCheck(){
    if(startupCancel)*startupCancel=true;++startupGeneration;startupChecking=false;
    if(startupThread.joinable()){startupThread.request_stop();CancelSynchronousIo(startupThread.native_handle());}
 }
 void StartStartupCheck(){
    if(startupChecking)return;if(startupThread.joinable())startupThread.join();
    std::vector<mm::RegisteredPath> paths;paths.reserve(folders.size()+tabs.size());
    for(const auto& folder:folders)if(!folder->path.empty())paths.push_back({folder->path,true});
    for(const auto& tab:tabs)if(!tab.path.empty())paths.push_back({tab.path,false});
    if(paths.empty())return;startupCancel=std::make_shared<std::atomic_bool>(false);auto cancel=startupCancel;
    auto generation=++startupGeneration;auto h=hwnd;startupChecking=true;
    // Metadata only, including paused tabs. This transient thread exits as soon
    // as all registrations have been checked and never reads document bodies.
    startupThread=std::jthread([h,generation,cancel,paths=std::move(paths)]{
      auto result=std::make_unique<StartupChecked>();result->generation=generation;
      try{result->checks=mm::CheckRegisteredPaths(paths,cancel.get());}catch(const std::exception&){if(*cancel)return;}
      if(!*cancel&&PostMessageW(h,StartupDone,0,LPARAM(result.get())))result.release();
    });
 }
 // Our own messages are UTF-8, but a library message in the ANSI code page
 // must never throw inside a catch handler or a message handler.
 static std::wstring ErrorText(const std::string& text){
    try{return mm::Wide(text);}catch(const std::exception&){}
    const int length=MultiByteToWideChar(CP_ACP,0,text.data(),int(text.size()),nullptr,0);
    if(length<=0)return L"（エラー内容を表示できません）";
    std::wstring result(size_t(length),L'\0');MultiByteToWideChar(CP_ACP,0,text.data(),int(text.size()),result.data(),length);return result;
 }
 static std::wstring PresenceIssue(const mm::PathCheck& check){
    switch(check.presence){case mm::PathPresence::Exists:return {};
      case mm::PathPresence::Missing:return check.directory?L"フォルダが見つかりません":L"ファイルが見つかりません";
      case mm::PathPresence::WrongType:return check.directory?L"フォルダではありません":L"ファイルではありません";
      default:return L"存在を確認できません（Windows エラー "+std::to_wstring(check.error)+L"）";}
 }
 void AcceptStartupCheck(std::unique_ptr<StartupChecked> result){
    if(result->generation!=startupGeneration||(startupCancel&&*startupCancel))return;startupChecking=false;size_t issues=0;
    for(const auto& check:result->checks){auto issue=PresenceIssue(check);
      if(check.directory){if(auto* folder=FindFolder(check.path)){folder->issue=issue;if(!issue.empty())++issues;}}
      else for(auto& tab:tabs)if(_wcsicmp(tab.path.c_str(),check.path.c_str())==0){tab.registrationIssue=issue;if(!issue.empty())++issues;}
    }
    if(issues)notice=L"起動時の存在確認: "+std::to_wstring(issues)+L"件が要確認です";
    RebuildTabs();UpdateRootTitle();InvalidateRect(hwnd,nullptr,FALSE);
 }
 void Capture(){if(active>=0&&active<int(tabs.size())&&view.Handle()&&displayedTabId==tabs[active].id)tabs[active].scroll=view.ScrollRatio();}
 void Dirty(){if(!closing&&!saveRetryPending&&!closePrompt)SetTimer(hwnd,3,700,nullptr);}
 void RetrySaveLater(){
    saveRetryPending=true;notice=L"状態を保存できません。10秒後に再試行します";
    if(!closing)SetTimer(hwnd,3,10000,nullptr);InvalidateRect(hwnd,nullptr,FALSE);
 }
 void AutoSave(){
    KillTimer(hwnd,3);if(closing)return;
    if(closePrompt){SetTimer(hwnd,3,10000,nullptr);return;}
    if(!Save(false)){RetrySaveLater();return;}
    saveRetryPending=false;if(notice==L"状態を保存できません。10秒後に再試行します")notice.clear();InvalidateRect(hwnd,nullptr,FALSE);
 }
 bool ConfirmClose(){
    if(closing||closePrompt)return false;
    while(!Save()){
      closePrompt=true;
      const auto answer=MessageBoxW(hwnd,L"タブや表示設定を保存できませんでした。\n\nはい: 保存を再試行します。\nいいえ: 保存せずに終了します。\nキャンセル: 終了を取り消して戻ります。\n\n前回の保存内容は変更していません。",L"状態を保存できません",MB_YESNOCANCEL|MB_ICONWARNING);
      closePrompt=false;
      if(answer==IDNO)return true;
      if(answer!=IDYES){RetrySaveLater();return false;}
    }
    saveRetryPending=false;return true;
 }
 void UpdateAppIcons(){
    const auto instance=reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd,GWLP_HINSTANCE));
    auto update=[&](HICON& owned,WPARAM kind,int widthMetric,int heightMetric){
      HICON icon=nullptr;const int width=GetSystemMetricsForDpi(widthMetric,dpi),height=GetSystemMetricsForDpi(heightMetric,dpi);
      if(FAILED(LoadIconWithScaleDown(instance,MAKEINTRESOURCEW(IDI_MMVIEWER),width,height,&icon)))
        icon=static_cast<HICON>(LoadImageW(instance,MAKEINTRESOURCEW(IDI_MMVIEWER),IMAGE_ICON,width,height,0));
      if(!icon){
        if(owned)return; // Retain a usable icon if a later DPI reload fails.
        auto fallback=LoadIconW(instance,MAKEINTRESOURCEW(IDI_MMVIEWER));
        if(!fallback)fallback=LoadIconW(nullptr,IDI_APPLICATION);
        SendMessageW(hwnd,WM_SETICON,kind,LPARAM(fallback));return;
      }
      SendMessageW(hwnd,WM_SETICON,kind,LPARAM(icon));
      if(owned)DestroyIcon(owned);owned=icon;
    };
    update(appIcon,ICON_BIG,SM_CXICON,SM_CYICON);
    update(appSmallIcon,ICON_SMALL,SM_CXSMICON,SM_CYSMICON);
 }
 void ReleaseAppIcons(){
    // Detach owned handles before freeing them. LoadIcon fallbacks are shared.
    if(appIcon){SendMessageW(hwnd,WM_SETICON,ICON_BIG,0);DestroyIcon(appIcon);appIcon=nullptr;}
    if(appSmallIcon){SendMessageW(hwnd,WM_SETICON,ICON_SMALL,0);DestroyIcon(appSmallIcon);appSmallIcon=nullptr;}
 }
 void CreateControls(){
    dpi=GetDpiForWindow(hwnd);uiFont=CreateFontW(-D(13),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Yu Gothic UI");
    tabsH=CreateWindowExW(0,WC_TABCONTROLW,L"",WS_CHILD|WS_VISIBLE|WS_CLIPSIBLINGS|WS_CLIPCHILDREN|TCS_OWNERDRAWFIXED|TCS_FIXEDWIDTH|TCS_FOCUSNEVER,0,0,0,0,hwnd,HMENU(201),nullptr,nullptr);
    SendMessageW(tabsH,WM_SETFONT,WPARAM(uiFont),FALSE);TabCtrl_SetItemSize(tabsH,D(170),D(32));
    SetWindowSubclass(tabsH,TabProc,1,DWORD_PTR(this));
    treeH=CreateWindowExW(0,WC_TREEVIEWW,L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|TVS_SHOWSELALWAYS|TVS_DISABLEDRAGDROP,0,0,0,0,hwnd,HMENU(202),nullptr,nullptr);
    SendMessageW(treeH,WM_SETFONT,WPARAM(uiFont),FALSE);TreeView_SetItemHeight(treeH,D(27));TreeView_SetIndent(treeH,D(18));
    SetWindowSubclass(treeH,TreeProc,1,DWORD_PTR(this));
    splitH=CreateWindowExW(0,L"STATIC",L"",WS_CHILD|WS_VISIBLE|SS_NOTIFY,0,0,0,0,hwnd,HMENU(206),nullptr,nullptr);
    SetWindowSubclass(splitH,SplitterProc,1,DWORD_PTR(this));
    struct B{int id;const wchar_t*label;};
    for(auto b:{B{Sidebar,L"☰"},B{OpenFolder,L"フォルダを開く"},B{UiOpenFile,L"ファイルを開く"},B{Reload,L"更新"},B{Find,L"検索"},B{Source,L"ソース"},B{Wide,L"全幅表示"},B{Theme,L"テーマ"},B{NewTab,L"＋"}}){
      HWND h=CreateWindowExW(0,L"BUTTON",b.label,WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,0,0,0,0,hwnd,HMENU(INT_PTR(b.id)),nullptr,nullptr);SendMessageW(h,WM_SETFONT,WPARAM(uiFont),FALSE);buttons.push_back({b.id,h});}
    searchH=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"",WS_CHILD|WS_TABSTOP|ES_AUTOHSCROLL,0,0,0,0,hwnd,HMENU(203),nullptr,nullptr);SendMessageW(searchH,WM_SETFONT,WPARAM(uiFont),FALSE);SendMessageW(searchH,EM_SETCUEBANNER,TRUE,LPARAM(L"本文を検索…"));SetWindowSubclass(searchH,SearchProc,1,DWORD_PTR(this));
    countH=CreateWindowExW(0,L"STATIC",L"",WS_CHILD|SS_CENTERIMAGE,0,0,0,0,hwnd,HMENU(204),nullptr,nullptr);SendMessageW(countH,WM_SETFONT,WPARAM(uiFont),FALSE);
    for(auto b:{B{SearchPrev,L"↑"},B{SearchNext,L"↓"},B{SearchClose,L"×"}}){HWND h=CreateWindowExW(0,L"BUTTON",b.label,WS_CHILD|BS_OWNERDRAW,0,0,0,0,hwnd,HMENU(INT_PTR(b.id)),nullptr,nullptr);buttons.push_back({b.id,h});}
    view.Create(hwnd,205);view.onZoom=[this](int d){Zoom(d);};view.onLink=[this](const std::wstring& s){Link(s);};view.onScroll=[this]{Dirty();};
    ApplyTheme();RebuildTabs();Layout();ApplyTree(nullptr);UpdateAppIcons();DragAcceptFiles(hwnd,TRUE);
 }
 void ApplyTheme(){
    if(background)DeleteObject(background);if(panel)DeleteObject(panel);if(editBrush)DeleteObject(editBrush);background=CreateSolidBrush(Bg());panel=CreateSolidBrush(Panel());editBrush=CreateSolidBrush(Bg());
    BOOL flag=dark;DwmSetWindowAttribute(hwnd,20,&flag,sizeof(flag));
    SetWindowTheme(treeH,dark?L"DarkMode_Explorer":nullptr,dark?L"ScrollBar":nullptr);
    TreeView_SetInsertMarkColor(treeH,Accent());
    TreeView_SetBkColor(treeH,Panel());TreeView_SetTextColor(treeH,Text());RebuildFolderImages();view.SetTheme(dark);view.SetWide(wide);
    EnsureTabScroller(true);
    InvalidateRect(hwnd,nullptr,TRUE);RedrawWindow(hwnd,nullptr,nullptr,RDW_INVALIDATE|RDW_FRAME|RDW_ALLCHILDREN);Dirty();
 }
 void RebuildFolderImages(){
    auto images=mm::CreateFolderImages(dpi,dark);if(!images)return;
    TreeView_SetImageList(treeH,images,TVSIL_NORMAL);if(folderImages)ImageList_Destroy(folderImages);folderImages=images;
 }
 void Layout(bool sidebarOnly=false){
    if(!tabsH)return;RECT r;GetClientRect(hwnd,&r);int w=r.right,h=r.bottom,side=SidebarPixels(w);
    if(!sidebar){EndSplitDrag();EndRootDrag();splitHover=false;}
    if(!sidebarOnly){
    MoveWindow(tabsH,0,D(42),std::max(0,w-D(42)),D(37),TRUE);int x=D(9);
    for(auto [id,b]:buttons){if(id==NewTab){MoveWindow(b,w-D(40),D(45),D(36),D(31),TRUE);continue;}if(id>=SearchNext&&id<=SearchClose)continue;
      bool compact=w<D(800);if(id==OpenFolder)SetWindowTextW(b,compact?L"フォルダ":L"フォルダを開く");if(id==UiOpenFile)SetWindowTextW(b,compact?L"ファイル":L"ファイルを開く");if(id==Wide)SetWindowTextW(b,compact?L"全幅":L"全幅表示");
      int bw=id==Sidebar?32:id==OpenFolder||id==UiOpenFile?(compact?78:116):id==Wide?(compact?54:94):(compact?54:62);MoveWindow(b,x,D(6),D(bw),D(30),TRUE);x+=D(bw+5);}
    }
    int top=D(85),bottom=D(29);MoveWindow(treeH,D(8),top+D(4),std::max(0,side-D(8)-D(4)),std::max(0,h-top-D(4)-bottom),TRUE);ShowWindow(treeH,sidebar?SW_SHOW:SW_HIDE);
    MoveWindow(splitH,side-D(4),top,2*D(4),std::max(0,h-top-bottom),TRUE);ShowWindow(splitH,sidebar?SW_SHOW:SW_HIDE);
    int contentX=side+(sidebar?D(4):0);int sy=top;
    ShowWindow(searchH,searching?SW_SHOW:SW_HIDE);ShowWindow(countH,searching?SW_SHOW:SW_HIDE);
    if(searching){MoveWindow(searchH,contentX+D(18),sy+D(6),std::max(D(80),w-contentX-D(220)),D(29),TRUE);MoveWindow(countH,w-D(188),sy+D(6),D(74),D(28),TRUE);}
    for(auto[id,b]:buttons)if(id>=SearchNext&&id<=SearchClose){ShowWindow(b,searching?SW_SHOW:SW_HIDE);int off=id==SearchPrev?108:id==SearchNext?75:42;MoveWindow(b,w-D(off),sy+D(5),D(29),D(30),TRUE);}
    int contentTop=top+(searching?D(42):0);MoveWindow(view.Handle(),contentX,contentTop,std::max(1,w-contentX),std::max(1,h-contentTop-bottom),TRUE);InvalidateRect(hwnd,nullptr,TRUE);
 }
 void EndSplitDrag(){
    if(!dragSplit)return;dragSplit=false;if(GetCapture()==splitH)ReleaseCapture();InvalidateRect(splitH,nullptr,FALSE);Dirty();
 }
 void MoveSplitter(POINT point){
    ClientToScreen(splitH,&point);RECT client{};GetClientRect(hwnd,&client);
    const int pixels=std::clamp(splitStartWidth+int(point.x)-splitStartX,D(160),SidebarMaxPixels(client.right));
    const int width=std::clamp(MulDiv(pixels,96,dpi),160,600);
    if(width!=sideWidth){sideWidth=width;Layout(true);}
 }
 void PaintSplitter(HDC dc){
    RECT client{};GetClientRect(splitH,&client);FillRect(dc,&client,panel);
    const int width=D((splitHover||dragSplit)?2:1);RECT line=client;line.left=(client.right-width)/2;line.right=line.left+width;
    HBRUSH brush=CreateSolidBrush((splitHover||dragSplit)?Accent():(dark?RGB(50,60,74):RGB(218,224,233)));
    FillRect(dc,&line,brush);DeleteObject(brush);
 }
 static LRESULT CALLBACK SplitterProc(HWND h,UINT m,WPARAM w,LPARAM l,UINT_PTR,DWORD_PTR data){
    auto*a=reinterpret_cast<App*>(data);
    switch(m){
      case WM_SETCURSOR:SetCursor(LoadCursorW(nullptr,IDC_SIZEWE));return TRUE;
      case WM_ERASEBKGND:return 1;
      case WM_PAINT:{PAINTSTRUCT paint{};auto dc=BeginPaint(h,&paint);a->PaintSplitter(dc);EndPaint(h,&paint);return 0;}
      case WM_PRINT:case WM_PRINTCLIENT:a->PaintSplitter(HDC(w));return 0;
      case WM_LBUTTONDOWN:{if(!a->sidebar)return 0;POINT point{GET_X_LPARAM(l),GET_Y_LPARAM(l)};ClientToScreen(h,&point);
        RECT client{};GetClientRect(a->hwnd,&client);a->splitStartX=point.x;a->splitStartWidth=a->SidebarPixels(client.right);
        SetCapture(h);a->dragSplit=GetCapture()==h;SetCursor(LoadCursorW(nullptr,IDC_SIZEWE));InvalidateRect(h,nullptr,FALSE);return 0;}
      case WM_MOUSEMOVE:
        if(!a->splitHover){a->splitHover=true;TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,h,0};TrackMouseEvent(&track);InvalidateRect(h,nullptr,FALSE);}
        if(a->dragSplit){if(w&MK_LBUTTON)a->MoveSplitter({GET_X_LPARAM(l),GET_Y_LPARAM(l)});else a->EndSplitDrag();}return 0;
      case WM_MOUSELEAVE:a->splitHover=false;InvalidateRect(h,nullptr,FALSE);return 0;
      case WM_LBUTTONUP:if(a->dragSplit){a->MoveSplitter({GET_X_LPARAM(l),GET_Y_LPARAM(l)});a->EndSplitDrag();}return 0;
      case WM_CANCELMODE:case WM_CAPTURECHANGED:a->EndSplitDrag();return 0;
    }
    return DefSubclassProc(h,m,w,l);
 }
 // Duplicate-name counts come from RebuildTabs, so a title costs one lookup
 // instead of a pass over every tab; painting the strip used to be quadratic.
 std::wstring TabTitle(size_t i,int same=-1){if(tabs[i].path.empty())return L"新しいタブ";fs::path p(tabs[i].path);auto name=p.filename().wstring();if(same<0){const auto counted=tabNameCounts.find(name);same=counted==tabNameCounts.end()?1:counted->second;}if(same>1)name+=L" — "+p.parent_path().filename().wstring();if(!tabs[i].registrationIssue.empty())name+=L" [要確認]";return name;}
 void RebuildTabs(){
    tabNameCounts.clear();for(const auto& tab:tabs)++tabNameCounts[fs::path(tab.path).filename().wstring()];
    const bool visible=(GetWindowLongPtrW(tabsH,GWL_STYLE)&WS_VISIBLE)!=0;
    SendMessageW(tabsH,WM_SETREDRAW,FALSE,0);TabCtrl_DeleteAllItems(tabsH);
    for(size_t i=0;i<tabs.size();i++){auto title=TabTitle(i)+L"    ×";TCITEMW item{};item.mask=TCIF_TEXT;item.pszText=title.data();TabCtrl_InsertItem(tabsH,int(i),&item);}
    SendMessageW(tabsH,WM_SETREDRAW,TRUE,0);if(!visible)ShowWindow(tabsH,SW_HIDE);
    TabCtrl_SetCurSel(tabsH,active);EnsureTabScroller();InvalidateRect(tabsH,nullptr,TRUE);
 }
 void EnsureTabScroller(bool refresh=false){
    if(!tabsH)return;
    if(!tabScrollH){tabScrollH=FindWindowExW(tabsH,nullptr,UPDOWN_CLASSW,nullptr);if(!tabScrollH)return;
      SetWindowSubclass(tabScrollH,TabScrollProc,1,DWORD_PTR(this));refresh=true;}
    if(refresh){SetWindowTheme(tabScrollH,dark?L"DarkMode_Explorer":nullptr,nullptr);RedrawWindow(tabScrollH,nullptr,nullptr,RDW_INVALIDATE|RDW_FRAME);}
    RECT bounds{};GetWindowRect(tabScrollH,&bounds);MapWindowPoints(HWND_DESKTOP,tabsH,reinterpret_cast<POINT*>(&bounds),2);
    const int top=TabScrollerTop(bounds.bottom-bounds.top);
    if(top>=0&&bounds.top!=top)SetWindowPos(tabScrollH,nullptr,bounds.left,top,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
 }
 int TabScrollerTop(int height) const {
    RECT row{},client{};
    if(height<=0||!GetClientRect(tabsH,&client)||!TabCtrl_GetItemRect(tabsH,0,&row))return -1;
    return std::clamp(int(row.top+row.bottom-height)/2,0,std::max(0,int(client.bottom)-height));
 }
 void ScrollTabStrip(int delta){
    EnsureTabScroller();
    if(!tabScrollH||!(GetWindowLongPtrW(tabScrollH,GWL_STYLE)&WS_VISIBLE)){tabWheelRemainder=0;return;}
    tabWheelRemainder+=delta;const int steps=tabWheelRemainder/WHEEL_DELTA;tabWheelRemainder%=WHEEL_DELTA;if(!steps)return;
    int low=0,high=0;SendMessageW(tabScrollH,UDM_GETRANGE32,WPARAM(&low),LPARAM(&high));if(low>high)std::swap(low,high);
    const int current=int(SendMessageW(tabScrollH,UDM_GETPOS32,0,0));const int next=std::clamp(current-steps,low,high);if(next==current)return;
    // Move the tab strip through its native scroll control without changing
    // selection, focus, the displayed document, or the tab order.
    SendMessageW(tabScrollH,UDM_SETPOS32,0,next);
    SendMessageW(tabsH,WM_HSCROLL,MAKEWPARAM(SB_THUMBPOSITION,WORD(next)),LPARAM(tabScrollH));
 }
 bool TabWheel(const MSG& msg){
    if(msg.message!=WM_MOUSEWHEEL)return false;
    const auto keys=GET_KEYSTATE_WPARAM(msg.wParam);
    if(keys&MK_CONTROL){tabWheelRemainder=0;return false;}
    if(msg.hwnd!=hwnd&&!IsChild(hwnd,msg.hwnd))return false;
    RECT strip{},client{};GetWindowRect(tabsH,&strip);GetClientRect(hwnd,&client);
    POINT right{client.right,0};ClientToScreen(hwnd,&right);strip.right=right.x;
    if(!PtInRect(&strip,{GET_X_LPARAM(msg.lParam),GET_Y_LPARAM(msg.lParam)})){tabWheelRemainder=0;return false;}
    ScrollTabStrip(GET_WHEEL_DELTA_WPARAM(msg.wParam));return true;
 }
 static LRESULT CALLBACK TabScrollProc(HWND h,UINT m,WPARAM w,LPARAM l,UINT_PTR id,DWORD_PTR data){
    auto*a=reinterpret_cast<App*>(data);
    if(m==WM_WINDOWPOSCHANGING){
      // The native tab control positions its arrows at the bottom of the row.
      // Keep its horizontal placement and size, but center every reposition
      // against the actual tab rectangles (also after selection/DPI changes).
      auto*position=reinterpret_cast<WINDOWPOS*>(l);RECT bounds{};GetWindowRect(h,&bounds);
      MapWindowPoints(HWND_DESKTOP,a->tabsH,reinterpret_cast<POINT*>(&bounds),2);
      const int top=a->TabScrollerTop(position->flags&SWP_NOSIZE?bounds.bottom-bounds.top:position->cy);
      if(top>=0&&(!(position->flags&SWP_NOMOVE)||bounds.top!=top)){
        if(position->flags&SWP_NOMOVE)position->x=bounds.left;position->y=top;position->flags&=~SWP_NOMOVE;
      }
    }
    if(m==WM_MOUSEWHEEL){MSG msg{};msg.hwnd=h;msg.message=m;msg.wParam=w;msg.lParam=l;if(a->TabWheel(msg))return 0;}
    if(m==WM_NCDESTROY){if(a->tabScrollH==h)a->tabScrollH=nullptr;RemoveWindowSubclass(h,TabScrollProc,id);}
    return DefSubclassProc(h,m,w,l);
 }
 void Switch(int index){if(index<0||index>=int(tabs.size()))return;Capture();active=index;TabCtrl_SetCurSel(tabsH,active);
    // Mouse selection is already changed when TCN_SELCHANGE arrives. Native
    // partial paints may still show the old active tab, so repaint the strip.
    InvalidateRect(tabsH,nullptr,FALSE);LoadActive();Dirty();}
 void AddEmpty(){Capture();Tab t;t.id=nextId++;t.font=fontDefault;tabs.push_back(t);active=int(tabs.size())-1;RebuildTabs();LoadActive();Dirty();}
 void Close(int index){if(index<0||index>=int(tabs.size()))return;Capture();const bool wasActive=index==active;if(!tabs[index].path.empty()){closed.push_back(tabs[index]);if(closed.size()>10)closed.erase(closed.begin());}
    ForgetDocument(tabs[index].id);tabs.erase(tabs.begin()+index);if(active>index)active--;else if(active>=int(tabs.size()))active=int(tabs.size())-1;RebuildTabs();if(wasActive)LoadActive(false);SetupWatches();Dirty();}
 void CloseAll(){
    if(tabs.empty())return;Capture();
    auto remember=[&](int index){if(index>=0&&index<int(tabs.size())&&!tabs[index].path.empty()){
      closed.push_back(tabs[index]);if(closed.size()>10)closed.erase(closed.begin());}};
    // Keep the same bounded reopen history; the active document reopens first.
    for(int i=int(tabs.size())-1;i>=0;--i)if(i!=active)remember(i);remember(active);
    tabs.clear();active=-1;tabDrag=-1;documentCache.clear();documentCacheBytes=0;
    pendingAnchor.clear();pendingAnchorPath.clear();RebuildTabs();
    const bool updating=treeUpdating;treeUpdating=true;TreeView_SelectItem(treeH,nullptr);treeUpdating=updating;
    // Cancel any in-flight read and update the UI/watches only once.
    LoadActive(false);notice.clear();SetupWatches();Dirty();InvalidateRect(hwnd,nullptr,FALSE);
 }
 void ShowTabMenu(POINT point){
    HMENU menu=CreatePopupMenu();if(!menu)return;
    AppendMenuW(menu,MF_STRING|(tabs.empty()?MF_GRAYED:MF_ENABLED),CloseAllTabs,L"全て閉じる");
    const int command=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,point.x,point.y,0,hwnd,nullptr);
    DestroyMenu(menu);if(command==CloseAllTabs&&!closing)Command(command);
 }
 void OpenPaths(const std::vector<std::wstring>&paths,bool revealInSidebar=false){
    std::map<std::wstring,int,FolderPathLess> indices;
    for(size_t i=0;i<tabs.size();i++)if(!tabs[i].path.empty())indices.emplace(tabs[i].path,int(i));
    bool selected=false,changed=false;
    for(auto p:paths){std::error_code ec;if(fs::is_directory(p,ec)){OpenRoot(p);continue;}if(!mm::IsMarkdown(p)){notice=L".md / .markdown ファイルを選択してください";continue;}
      p=mm::NormalizePath(p);if(revealInSidebar)EnsureFileFolder(p);if(!selected)Capture();selected=true;
      auto found=indices.find(p);if(found!=indices.end()){active=found->second;continue;}
      Tab t;t.path=p;t.id=nextId++;t.font=fontDefault;t.autoRefresh=!IsExcluded(p);
      if(active>=0&&tabs[active].path.empty())tabs[active]=std::move(t);else{tabs.push_back(std::move(t));active=int(tabs.size())-1;}
      indices.emplace(std::move(p),active);changed=true;
    }
    // A multi-file drop needs one tab-strip rebuild and only the final document
    // read. Intermediate selections must not create and immediately cancel I/O.
    if(changed)RebuildTabs();else if(selected){TabCtrl_SetCurSel(tabsH,active);InvalidateRect(tabsH,nullptr,FALSE);}
    if(selected){LoadActive();if(revealInSidebar){if(!sidebar){sidebar=true;Layout();}SelectTreeFile(true);}}SetupWatches();Dirty();
 }
 void SelectFiles(){IFileOpenDialog* dialog=nullptr;if(FAILED(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog))))return;
    DWORD options;dialog->GetOptions(&options);dialog->SetOptions(options|FOS_ALLOWMULTISELECT|FOS_FILEMUSTEXIST|FOS_FORCEFILESYSTEM);COMDLG_FILTERSPEC filter[]={ {L"Markdown",L"*.md;*.markdown"},{L"すべてのファイル",L"*.*"} };dialog->SetFileTypes(2,filter);
    std::vector<std::wstring> paths;if(SUCCEEDED(dialog->Show(hwnd))){IShellItemArray*items=nullptr;if(SUCCEEDED(dialog->GetResults(&items))){DWORD n;items->GetCount(&n);for(DWORD i=0;i<n;i++){IShellItem*item;PWSTR path=nullptr;if(SUCCEEDED(items->GetItemAt(i,&item))){if(SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH,&path))){paths.emplace_back(path);CoTaskMemFree(path);}item->Release();}}items->Release();}}dialog->Release();OpenPaths(paths);
 }
 void SelectFolder(){IFileOpenDialog* dialog=nullptr;if(FAILED(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog))))return;DWORD opts;dialog->GetOptions(&opts);dialog->SetOptions(opts|FOS_PICKFOLDERS|FOS_FORCEFILESYSTEM);dialog->SetTitle(L"Markdownを読み込むフォルダを選択");
    if(SUCCEEDED(dialog->Show(hwnd))){IShellItem*item;PWSTR path=nullptr;if(SUCCEEDED(dialog->GetResult(&item))){if(SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH,&path))){OpenRoot(path);CoTaskMemFree(path);}item->Release();}}dialog->Release();}
 void Welcome(){loading=false;displayedTabId=0;currentDoc.reset();view.SetDocument("# Markdownを、気持ちよく。\n\nフォルダを開くと、Markdownがある階層だけを左側に表示します。\n\n- **Ctrl + Shift + O** フォルダを開く\n- **Ctrl + O** ファイルを開く\n- **Ctrl + B** サイドメニューを開閉\n- **Ctrl + ホイール** 文字と図を拡大・縮小\n- **Ctrl + F** 本文を検索\n\nファイルやフォルダを、このウィンドウへドロップしても開けます。\n\n---\n\nC++ / Win32で動く、オフライン専用の閲覧アプリです。\n",L"",active>=0?tabs[active].font:fontDefault,false);SetWindowTextW(hwnd,L"MMViewer");}
 static bool SameDocumentStamp(const WIN32_FILE_ATTRIBUTE_DATA& left,const WIN32_FILE_ATTRIBUTE_DATA& right){
    return !(left.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)&&!(right.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)
      &&CompareFileTime(&left.ftLastWriteTime,&right.ftLastWriteTime)==0
      &&CompareFileTime(&left.ftCreationTime,&right.ftCreationTime)==0
      &&left.nFileSizeLow==right.nFileSizeLow&&left.nFileSizeHigh==right.nFileSizeHigh;
 }
 static size_t DocumentBytes(const mm::Document& doc){
    size_t bytes=sizeof(doc)+(doc.path.capacity()+1)*sizeof(wchar_t)+doc.source.capacity()+1
      +doc.html.capacity()+1+doc.encoding.capacity()+1+doc.hash.capacity()+1;
    for(const auto& reference:doc.references)bytes+=sizeof(reference)+reference.first.capacity()+1+reference.second.capacity()+1;
    return bytes;
 }
 void ForgetDocument(unsigned long long id){
    for(auto it=documentCache.begin();it!=documentCache.end();++it)if(it->id==id){documentCacheBytes-=it->bytes;documentCache.erase(it);return;}
 }
 CachedDocument FindDocument(const Tab& tab){
    for(auto it=documentCache.begin();it!=documentCache.end();++it)if(it->id==tab.id&&it->cp932==tab.cp932&&it->doc->path==tab.path){
      auto entry=*it;documentCache.erase(it);documentCache.push_back(entry);return entry;
    }
    return {};
 }
 void RememberDocument(const Tab& tab,const std::shared_ptr<mm::Document>& doc,const WIN32_FILE_ATTRIBUTE_DATA& stamp,bool stampValid){
    ForgetDocument(tab.id);const auto bytes=DocumentBytes(*doc);
    if(bytes>DocumentCacheBytes)return;
    while(!documentCache.empty()&&(documentCache.size()>=DocumentCacheCount||documentCacheBytes>DocumentCacheBytes-bytes)){
      documentCacheBytes-=documentCache.front().bytes;documentCache.pop_front();
    }
    documentCache.push_back({tab.id,doc,stamp,bytes,tab.cp932,stampValid});documentCacheBytes+=bytes;
 }
 void ApplyPendingAnchor(const Tab& tab){
    if(!pendingAnchor.empty()&&pendingAnchorPath==tab.path){view.ScrollToAnchor(pendingAnchor);pendingAnchor.clear();pendingAnchorPath.clear();}
 }
 void PresentDocument(Tab& tab,const std::shared_ptr<mm::Document>& doc){
    currentDoc=doc;tab.encoding=mm::Wide(doc->encoding);
    view.SetDocument(doc->source,tab.path,tab.font,tab.source);displayedTabId=tab.id;view.SetScrollRatio(tab.scroll);ApplyPendingAnchor(tab);
    SetWindowTextW(hwnd,(fs::path(tab.path).filename().wstring()+L" — MMViewer").c_str());SelectTreeFile();
 }
 void RecheckAfterLoad(const Loaded& result){
    if(!readRecheck||readRecheck!=result.cancel)return;readRecheck.reset();
    if(closing||result.cancel!=readCancel||*result.cancel||active<0||active>=int(tabs.size())
      ||result.id!=tabs[active].id||result.generation!=loadGeneration)return;
    // A save notification may arrive after this request read its bytes but
    // before its completion is accepted. Coalesce it into one metadata probe.
    CheckActive();
 }
 void LoadActive(bool manual=true,bool force=false,bool backgroundCheck=false){
    readRecheck.reset();
    if(!manual&&active>=0&&!tabs[active].path.empty()&&(!tabs[active].autoRefresh||IsExcluded(tabs[active].path))){
      if(loading){if(readCancel)*readCancel=true;readWorker.CancelPending();++loadGeneration;}loading=false;
      if(displayedTabId!=tabs[active].id){auto cached=FindDocument(tabs[active]);if(cached.doc)PresentDocument(tabs[active],cached.doc);else{currentDoc.reset();displayedTabId=0;view.SetDocument("# 自動更新を停止しています\n\nフォルダを開き直すか、タブを選択すると読み込めます。",tabs[active].path,tabs[active].font,false);}}
      return;
    }
    if(readCancel)*readCancel=true;readWorker.CancelPending();if(!backgroundCheck)++loadGeneration;if(active<0||tabs[active].path.empty()){Welcome();return;}
    auto& tab=tabs[active];auto cached=FindDocument(tab);if(force)view.InvalidateDocumentCache(tab.path);
    if(displayedTabId!=tab.id){
      if(cached.doc)PresentDocument(tab,cached.doc);
      else{currentDoc.reset();displayedTabId=0;view.SetDocument("読み込み中…",tab.path,tab.font,false);}
    }
    readCancel=std::make_shared<std::atomic_bool>(false);auto cancel=readCancel;auto t=tab;loading=true;
    notice=displayedTabId==t.id?L"":L"読み込み中…";InvalidateRect(hwnd,nullptr,FALSE);
    auto h=hwnd;auto generation=loadGeneration;
    // Both metadata and body reads run off the UI thread. Switching a cached
    // tab presents it first, then probes only metadata if its file is unchanged.
    readWorker.Queue([h,t,generation,cancel,cached=std::move(cached),force]{
      if(*cancel)return;auto result=std::make_unique<Loaded>();result->id=t.id;result->generation=generation;result->cancel=cancel;result->force=force;
      result->stampValid=GetFileAttributesExW(t.path.c_str(),GetFileExInfoStandard,&result->stamp)!=FALSE;
      if(*cancel)return;
      if(!force&&cached.doc&&cached.stampValid&&result->stampValid&&SameDocumentStamp(cached.stamp,result->stamp))result->doc=cached.doc;
      else try{result->doc=std::make_shared<mm::Document>(mm::ReadDocument(t.path,t.cp932,cancel.get()));}catch(const std::exception&e){result->error=e.what();}
      if(!*cancel&&PostMessageW(h,LoadDone,0,LPARAM(result.get())))result.release();
    });
 }
 void AcceptLoad(std::unique_ptr<Loaded> result){
    const auto current=[&]{return !closing&&active>=0&&active<int(tabs.size())&&result->generation==loadGeneration
      &&result->id==tabs[active].id&&result->cancel==readCancel&&(!result->cancel||!*result->cancel);};
    if(!current())return;
    loading=false;
    if(result->error=="ENCODING_REQUIRED"){
      // The modal loop can open/close tabs or accept a newer read. Keep no Tab&
      // across it, and never apply its answer to a replacement request.
      auto previousPrompt=std::exchange(encodingPromptCancel,result->cancel);
      const auto answer=MessageBoxW(hwnd,L"UTF-8として読み込めません。日本語のCP932として開きますか？",L"文字コードの選択",MB_YESNO|MB_ICONQUESTION);
      encodingPromptCancel=std::move(previousPrompt);
      if(!current())return;
      if(answer==IDYES){tabs[active].cp932=true;LoadActive();return;}
    }
    auto&t=tabs[active];
    if(!result->error.empty()){
      for(auto& cached:documentCache)if(cached.id==t.id)cached.stampValid=false;
      notice=L"読み込めません: "+ErrorText(result->error);t.status=notice;
      if(!currentDoc||currentDoc->path!=t.path){currentDoc.reset();displayedTabId=0;view.SetDocument("# ファイルを読み込めません\n\n"+mm::Utf8(t.path)+"\n\n"+result->error,t.path,t.font,false);}
      InvalidateRect(hwnd,nullptr,FALSE);RecheckAfterLoad(*result);return;
    }
    if(!result->doc){RecheckAfterLoad(*result);return;}
    const bool unchanged=displayedTabId==t.id&&currentDoc&&currentDoc->path==result->doc->path&&currentDoc->hash==result->doc->hash;
    if(displayedTabId==t.id&&(!unchanged||result->force))t.scroll=view.ScrollRatio();
    auto doc=unchanged?currentDoc:result->doc;RememberDocument(t,doc,result->stamp,result->stampValid);
    if(!unchanged||result->force)PresentDocument(t,doc);else ApplyPendingAnchor(t);
    t.encoding=mm::Wide(doc->encoding);t.status=L"";if(!t.registrationIssue.empty()){t.registrationIssue.clear();RebuildTabs();}
    notice=L"";InvalidateRect(hwnd,nullptr,FALSE);RecheckAfterLoad(*result);
 }
 std::wstring pendingAnchor,pendingAnchorPath;
 void Link(const std::wstring&target){auto s=mm::Utf8(target);if(s.rfind("https://",0)==0||s.rfind("http://",0)==0){ShellExecuteW(hwnd,L"open",target.c_str(),nullptr,nullptr,SW_SHOWNORMAL);return;}if(active<0)return;
    if(!s.empty()&&s[0]=='#'){view.ScrollToAnchor(mm::Wide(mm::Fragment(s)));return;}auto path=mm::ResolveReference(tabs[active].path,s);if(!path.empty()&&mm::IsMarkdown(path)){pendingAnchor=mm::Wide(mm::Fragment(s));pendingAnchorPath=path;OpenPaths({path});}else{notice=L"このリンク形式は開けません";InvalidateRect(hwnd,nullptr,FALSE);}}
 void EnsureFileFolder(const std::wstring& path){
    const auto parent=fs::path(path).parent_path().wstring();
    if(parent.empty())return;
    // Reuse a registered ancestor, including while its asynchronous scan is
    // pending. A missing row must not create a duplicate nested registration.
    FolderRegistration* owner=nullptr;
    for(const auto& folder:folders)if(IsWithinFolder(parent,folder->path)
      &&(!owner||folder->path.size()>owner->path.size()))owner=folder.get();
    if(!owner||IsExcluded(path)){OpenRoot(parent);return;}
    // The file may have been created since the last scan, before its watch
    // notification arrives. Scan completion selects the current active tab.
    if(!rowsByPath.contains(path)&&!owner->scanning&&!owner->queuedTasks)
      watchRescanFolders.insert(owner->path);
 }
 void OpenRoot(const std::wstring&path){
    const auto normalized=mm::NormalizePath(path);auto*folder=FindFolder(normalized);const bool addedRoot=!folder;
    if(!folder){auto added=std::make_unique<FolderRegistration>();added->path=normalized;added->id=nextFolderId++;
      auto name=fs::path(normalized).filename().wstring();added->pending={name.empty()?normalized:name,normalized,true,{}};
      folder=added.get();folders.push_back(std::move(added));expanded.insert(normalized);}
    folder->issue.clear();sidebar=true;
    std::erase_if(excludedFolders,[&](const auto& excluded){
      const bool restore=IsWithinFolder(excluded,normalized)||IsWithinFolder(normalized,excluded);
      if(restore)for(const auto& registered:folders)if(IsWithinFolder(excluded,registered->path))watchRescanFolders.insert(registered->path);
      return restore;});
    for(auto&t:tabs)if(IsWithinFolder(t.path,normalized))t.autoRefresh=true;
    // Keep every other tree and its expansion state while only this root loads.
    if(addedRoot){treeUpdating=true;InsertRow(folder->pending,TVI_ROOT,TVI_LAST);treeUpdating=false;knownFolders.insert(normalized);UpdateRootTitle();InvalidateRect(treeH,nullptr,TRUE);}
    watchRescanFolders.insert(normalized);notice=L"Markdownのあるフォルダを検索中…";
    SetupWatches();Layout();Dirty();
 }
 // A scan already queued or running keeps going; the request is honoured once
 // its result is in, so a burst of changes cannot restart a long scan forever.
 void RefreshFolder(FolderRegistration& folder){
    if(folder.scanning){folder.rescanRequested=true;return;}
    folder.cancel=std::make_shared<std::atomic_bool>(false);auto cancel=folder.cancel;
    auto path=folder.path;auto id=folder.id;auto generation=++folder.generation;folder.scanning=true;folder.rescanRequested=false;auto h=hwnd;
    notice=L"Markdownのあるフォルダを検索中…";InvalidateRect(hwnd,nullptr,FALSE);
    std::vector<std::wstring> excluded(excludedFolders.begin(),excludedFolders.end());
    mm::FolderScanCoverage coverage;
    for(const auto& open:expanded)if(OwnsExpansion(folder,open))coverage.trackedFolders.push_back(open);
    Scanner().Append(id,[h,cancel,path,id,generation,excluded=std::move(excluded),coverage=std::move(coverage)]()mutable{if(*cancel)return;auto result=std::make_unique<Scanned>();result->folderId=id;result->generation=generation;result->coverage=std::move(coverage);try{result->tree=std::make_unique<mm::FolderNode>(mm::ScanFolder(path,cancel.get(),excluded,&result->coverage));}catch(const std::exception&e){result->error=ErrorText(e.what());}
      // A lost result would leave the registration scanning for good, so the
      // post is retried until it succeeds, the scan is cancelled (closing and
      // removal cancel first) or the window is gone. A full queue drains soon.
      while(!*cancel&&IsWindow(h)){if(PostMessageW(h,TreeDone,0,LPARAM(result.get()))){result.release();break;}Sleep(20);}});
 }
 // Roots scan in parallel on a small pool. The least loaded thread takes the
 // next request, so one slow root no longer holds up the others at startup.
 Worker& Scanner(){Worker* best=&scanners[0];size_t load=best->Pending();for(auto& worker:scanners){const auto pending=worker.Pending();if(pending<load){best=&worker;load=pending;}}return *best;}
 void RefreshTree(){watchRescanFolders.clear();for(auto& folder:folders)RefreshFolder(*folder);}
 void RefreshPendingTrees(){auto pending=std::move(watchRescanFolders);watchRescanFolders.clear();for(const auto& path:pending)if(auto*folder=FindFolder(path))RefreshFolder(*folder);}
 void CancelFolderScans(){
    for(auto& folder:folders){if(folder->cancel)*folder->cancel=true;++folder->generation;folder->scanning=false;folder->rescanRequested=false;}
    for(auto& worker:scanners)worker.CancelPending();
 }
 // ---- Folder tree rows -------------------------------------------------------
 // Rows mirror the model (folder.tree or folder.pending) except for work still
 // queued in treeTasks. Only MergeTree and ReplaceFolderTree change a model,
 // and children live in a std::list, so a row's lParam stays valid until that
 // row is deleted. Every native row operation costs an accessibility
 // notification (hundreds of microseconds on some machines), so updates apply
 // the difference between scans, and large batches run in bounded slices from
 // a timer so input and painting continue in between.
 static size_t CountNodes(const mm::FolderNode& node){size_t count=1;for(const auto& child:node.children)count+=CountNodes(child);return count;}
 mm::FolderNode* NodeOf(HTREEITEM row)const{
    TVITEMW info{};info.hItem=row;info.mask=TVIF_PARAM;
    return row&&TreeView_GetItem(treeH,&info)?reinterpret_cast<mm::FolderNode*>(info.lParam):nullptr;
 }
 void SetRowNode(HTREEITEM row,mm::FolderNode& node){TVITEMW info{};info.hItem=row;info.mask=TVIF_PARAM;info.lParam=LPARAM(&node);TreeView_SetItem(treeH,&info);}
 HTREEITEM InsertRow(mm::FolderNode& node,HTREEITEM parent,HTREEITEM after){
    TVINSERTSTRUCTW in{};in.hParent=parent;in.hInsertAfter=after;in.item.mask=TVIF_TEXT|TVIF_PARAM|TVIF_IMAGE|TVIF_SELECTEDIMAGE;
    in.item.pszText=node.name.data();in.item.lParam=LPARAM(&node);
    const bool open=node.directory&&!node.children.empty()&&expanded.contains(node.path);
    in.item.iImage=in.item.iSelectedImage=node.directory?(open?1:0):I_IMAGENONE;
    auto row=TreeView_InsertItem(treeH,&in);if(row){++treeStats.rowsInserted;if(!node.directory)rowsByPath[node.path]=row;}return row;
 }
 void UpdateFolderItem(HTREEITEM item,bool saveState){
    TVITEMW info{};info.hItem=item;info.mask=TVIF_PARAM|TVIF_STATE|TVIF_IMAGE|TVIF_SELECTEDIMAGE;info.stateMask=TVIS_EXPANDED;
    if(!TreeView_GetItem(treeH,&info))return;auto*node=reinterpret_cast<mm::FolderNode*>(info.lParam);if(!node||!node->directory)return;
    bool open=(info.state&TVIS_EXPANDED)!=0;const int image=open?1:0;
    if(info.iImage!=image||info.iSelectedImage!=image){info.mask=TVIF_IMAGE|TVIF_SELECTEDIMAGE;info.iImage=info.iSelectedImage=image;TreeView_SetItem(treeH,&info);}
    if(saveState){if(open)expanded.insert(node->path);else expanded.erase(node->path);Dirty();}
 }
 // Applied to scan results and copies only; live models change in MergeTree.
 void FilterExcludedFolders(mm::FolderNode& node){
    std::erase_if(node.children,[&](mm::FolderNode& child){
      if(!child.directory)return false;
      if(excludedFolders.contains(child.path))return true;
      FilterExcludedFolders(child);return child.children.empty();
    });
 }
 void RebuildKnownFolders(){
    knownFolders.clear();
    std::function<void(const mm::FolderNode&)> add=[&](const mm::FolderNode& node){
      if(!node.directory)return;knownFolders.insert(node.path);for(const auto& child:node.children)add(child);};
    for(const auto& folder:folders)add(folder->tree?*folder->tree:folder->pending);
 }
 // Registrations are matched by the node their root row points at, never by
 // path: a path removed and registered again must not adopt the doomed rows.
 FolderRegistration* FolderForRoot(const mm::FolderNode* node)const{
    if(!node)return nullptr;
    auto owns=[&](const FolderRegistration& f){if(node==f.tree.get()||node==&f.pending)return true;for(const auto& root:f.retiringRoots)if(node==root.get())return true;return false;};
    for(const auto& f:folders)if(owns(*f))return f.get();
    for(const auto& f:retiringFolders)if(owns(*f))return f.get();
    return nullptr;
 }
 FolderRegistration* FolderForItem(HTREEITEM item)const{
    if(!item)return nullptr;while(auto parent=TreeView_GetParent(treeH,item))item=parent;
    return FolderForRoot(NodeOf(item));
 }
 HTREEITEM RootRow(const FolderRegistration& folder)const{
    for(auto row=TreeView_GetRoot(treeH);row;row=TreeView_GetNextSibling(treeH,row))if(FolderForRoot(NodeOf(row))==&folder)return row;
    return nullptr;
 }
 bool HasRootItem(const FolderRegistration& folder)const{return RootRow(folder)!=nullptr;}
 bool ReorderRootFolders(unsigned long long sourceId,unsigned long long targetId,bool after){
    auto source=std::find_if(folders.begin(),folders.end(),[&](const auto& f){return f->id==sourceId;});
    auto target=std::find_if(folders.begin(),folders.end(),[&](const auto& f){return f->id==targetId;});
    if(source==folders.end()||target==folders.end()||source==target)return false;
    const size_t from=source-folders.begin();size_t to=target-folders.begin()+(after?1:0);if(from<to)--to;if(from==to)return false;
    auto move=[&](size_t a,size_t b){auto folder=std::move(folders[a]);folders.erase(folders.begin()+a);folders.insert(folders.begin()+b,std::move(folder));};
    // Sort only existing top-level rows. Descendants, scans and watches retain
    // their identities; the comparator does no file I/O or linear root lookup.
    // Rows of retiring registrations sort after every known root.
    std::unordered_map<LPARAM,int> order;order.reserve(folders.size());
    move(from,to);
    for(size_t i=0;i<folders.size();++i){auto& folder=*folders[i];order.emplace(LPARAM(folder.tree?folder.tree.get():&folder.pending),int(i));}
    TVSORTCB sort{TVI_ROOT,[](LPARAM left,LPARAM right,LPARAM data)->int{
      const auto& positions=*reinterpret_cast<const std::unordered_map<LPARAM,int>*>(data);
      auto a=positions.find(left),b=positions.find(right);
      if(a==positions.end()||b==positions.end())return (a==positions.end())-(b==positions.end());
      return a->second-b->second;
    },LPARAM(&order)};
    if(!TreeView_SortChildrenCB(treeH,&sort,FALSE)){move(to,from);return false;}
    InvalidateRect(treeH,nullptr,FALSE);Dirty();return true;
 }
 void EndRootDrag(){
    if(!rootDragId)return;rootDragId=rootDropId=0;rootDragging=false;KillTimer(treeH,RootDragTimer);
    TreeView_SetInsertMark(treeH,nullptr,FALSE);if(GetCapture()==treeH){ReleaseCapture();SetCursor(LoadCursorW(nullptr,IDC_ARROW));}
 }
 void UpdateRootDrop(POINT point){
    rootDragPoint=point;rootDropId=0;HTREEITEM mark=nullptr;RECT client{};GetClientRect(treeH,&client);
    if(PtInRect(&client,point)){
      TVHITTESTINFO hit{};hit.pt=point;auto item=TreeView_HitTest(treeH,&hit);
      if(item&&!TreeView_GetParent(treeH,item)){auto* folder=FolderForItem(item);RECT row{};
        if(folder&&folder->id!=rootDragId&&TreeView_GetItemRect(treeH,item,&row,FALSE)){
          rootDropId=folder->id;rootDropAfter=point.y>=(row.top+row.bottom)/2;mark=item;
        }
      }
    }
    TreeView_SetInsertMark(treeH,mark,rootDropAfter);SetCursor(LoadCursorW(nullptr,rootDropId?IDC_SIZEALL:IDC_NO));
 }
 // ---- Tree task queue ----------------------------------------------------------
 // Statistics cover one drain of the queue; work that FinishTreeTasks itself
 // starts (deferred results) extends the same measurement.
 void BeginTreeWork(){if(treeTasks.empty()&&!finishingTreeTasks){treeStats={};treeWorkStarted=std::chrono::steady_clock::now();}}
 void EnqueueInsert(FolderRegistration& folder,mm::FolderNode& node,HTREEITEM row){
    EnqueueRun(folder,node,row,node.children.begin(),node.children.end(),nullptr,false,CountNodes(node)-1);
 }
 void EnqueueRun(FolderRegistration& folder,mm::FolderNode& parent,HTREEITEM parentRow,std::list<mm::FolderNode>::iterator first,std::list<mm::FolderNode>::iterator stop,HTREEITEM after,bool partial,size_t estimate){
    TreeTask task;task.kind=TreeTask::Insert;task.folder=&folder;task.node=&parent;task.row=parentRow;task.first=first;task.stop=stop;task.after=after;task.partial=partial;task.estimate=estimate;
    treeTasks.push_back(std::move(task));++folder.queuedTasks;
 }
 void EnqueueDelete(FolderRegistration& folder,HTREEITEM row){EnqueueDeleteRun(folder,TreeView_GetParent(treeH,row),{row},0);}
 void EnqueueDeleteRun(FolderRegistration& folder,HTREEITEM parentRow,std::vector<HTREEITEM> seeds,size_t estimate){
    if(seeds.empty())return;
    TreeTask task;task.kind=TreeTask::Delete;task.folder=&folder;task.row=parentRow;task.pending=std::move(seeds);task.estimate=estimate;
    treeTasks.push_back(std::move(task));++folder.queuedTasks;
 }
 void ScheduleTreeTasks(){if(!treeTasks.empty()&&!closing)SetTimer(hwnd,TreeTaskTimer,10,nullptr);}
 // Small batches finish before returning to the caller; anything larger runs in
 // timer slices so the window keeps answering input while rows change.
 void StartTreeWork(bool wasIdle,size_t inserts,size_t deletes){
    if(treeTasks.empty())return;
    if(wasIdle&&inserts<=SyncInsertRows&&deletes<=SyncDeleteRows)FlushTreeTasks();else ScheduleTreeTasks();
 }
 bool TreeTasksPending()const{return !treeTasks.empty();}
 void FlushTreeTasks(){while(!treeTasks.empty()&&!treeTaskRunning)RunTreeTasks(false);}
 void RunTreeTasks(bool timed){
    if(treeTaskRunning||treeTasks.empty())return;treeTaskRunning=true;
    const auto sliceStart=std::chrono::steady_clock::now();
    const auto deadline=timed?sliceStart+std::chrono::milliseconds(TreeSliceMs):std::chrono::steady_clock::time_point::max();
    treeUpdating=true;
    while(!treeTasks.empty()){
      auto& task=treeTasks.front();
      const bool done=StepTreeTask(task,deadline);
      if(done){if(task.folder&&task.folder->queuedTasks)--task.folder->queuedTasks;treeTasks.pop_front();}
      if(std::chrono::steady_clock::now()>=deadline)break;
    }
    treeUpdating=false;
    if(timed){const double sliceMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-sliceStart).count();++treeStats.slices;treeStats.maxSliceMs=std::max(treeStats.maxSliceMs,sliceMs);}
    treeTaskRunning=false;
    if(treeTasks.empty())FinishTreeTasks();else ScheduleTreeTasks();
 }
 void FinishTreeTasks(){
    KillTimer(hwnd,TreeTaskTimer);
    ++finishingTreeTasks;struct Leave{unsigned& depth;~Leave(){--depth;}} leave{finishingTreeTasks};
    treeStats.totalMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-treeWorkStarted).count();
    for(auto& f:folders)if(!f->queuedTasks){f->retiring.clear();f->retiringRoots.clear();}
    std::erase_if(retiringFolders,[](const auto& f){return f->queuedTasks==0;});
    UpdateRootTitle();RebuildKnownFolders();SelectTreeFile();InvalidateRect(treeH,nullptr,TRUE);
    // Results that arrived while rows of the same registration were changing.
    std::vector<unsigned long long> waiting;
    for(const auto& f:folders)if(f->deferred&&!f->queuedTasks)waiting.push_back(f->id);
    for(auto id:waiting)if(auto* f=FindFolder(id);f&&f->deferred&&!f->queuedTasks){f->deferred=false;auto tree=std::move(f->deferredTree);ReplaceFolderTree(*f,std::move(tree));}
 }
 void CollectDirectoryRows(TreeTask& task){
    task.rows.clear();
    WalkItems(TreeView_GetRoot(treeH),[&](HTREEITEM item,mm::FolderNode* node){if(node&&node->directory)task.rows.push_back(item);});
    // Expanding deeper folders first keeps them invisible until their ancestor
    // opens, so the control recalculates its rows once per ancestor.
    if(task.kind==TreeTask::ExpandAll)std::reverse(task.rows.begin(),task.rows.end());
    task.collected=true;
 }
 // Deleting the subtree that holds the first visible row makes the control
 // re-derive that row by walking every item after each later insertion.
 // Move the viewport and the selection out of the subtree before it goes.
 void PrepareDelete(HTREEITEM row){
    auto within=[&](HTREEITEM item){for(;item;item=TreeView_GetParent(treeH,item))if(item==row)return true;return false;};
    if(within(TreeView_GetFirstVisible(treeH))){
      TreeView_Expand(treeH,row,TVE_COLLAPSE);
      auto other=TreeView_GetPrevVisible(treeH,row);if(!other)other=TreeView_GetNextVisible(treeH,row);
      if(other)TreeView_SelectSetFirstVisible(treeH,other);
    }
    if(within(TreeView_GetSelection(treeH)))TreeView_SelectItem(treeH,nullptr);
 }
 bool StepTreeTask(TreeTask& task,std::chrono::steady_clock::time_point deadline){
    const auto expired=[&]{return std::chrono::steady_clock::now()>=deadline;};
    switch(task.kind){
    case TreeTask::Insert:{
      if(!task.started){
        task.started=true;task.frames.push_back({task.node,task.row,task.first,task.after,task.stop,task.partial});
        // Many rows arriving under an open folder would each recalculate the
        // visible rows; close it while they arrive and reopen it at the end.
        if(task.partial&&task.estimate>SyncInsertRows&&task.row&&(TreeView_GetItemState(treeH,task.row,TVIS_EXPANDED)&TVIS_EXPANDED)){TreeView_Expand(treeH,task.row,TVE_COLLAPSE);task.reopen=true;}
      }
      while(!task.frames.empty()){
        auto& frame=task.frames.back();
        if(frame.next!=frame.stop){
          auto childIt=frame.next++;
          auto row=InsertRow(*childIt,frame.row,frame.last?frame.last:TVI_FIRST);
          if(!row){task.folder->retiring.splice(task.folder->retiring.end(),frame.node->children,childIt);continue;}
          frame.last=row;
          if(childIt->directory&&!childIt->children.empty())task.frames.push_back({&*childIt,row,childIt->children.begin(),nullptr,childIt->children.end(),false});
          else if(childIt->directory)UpdateFolderItem(row,false);
        }else{
          // A folder opens only once all of its rows exist, so the control
          // recalculates visible rows once rather than per insertion.
          if(frame.node->directory&&!frame.partial){if(!frame.node->children.empty()&&expanded.contains(frame.node->path))TreeView_Expand(treeH,frame.row,TVE_EXPAND);UpdateFolderItem(frame.row,false);}
          task.frames.pop_back();
        }
        if(expired()&&!task.frames.empty())return false;
      }
      if(task.reopen){task.reopen=false;TreeView_Expand(treeH,task.row,TVE_EXPAND);UpdateFolderItem(task.row,false);}
      return true;
    }
    case TreeTask::Delete:{
      if(!task.collected){
        if(!task.started){
          task.started=true;
          // Rows under an open parent are recalculated as each one goes; a
          // large batch closes the parent first and reopens it at the end.
          if(task.estimate>SyncDeleteRows&&task.row&&(TreeView_GetItemState(treeH,task.row,TVIS_EXPANDED)&TVIS_EXPANDED)){TreeView_Expand(treeH,task.row,TVE_COLLAPSE);task.reopen=true;}
          for(auto seed:task.pending)PrepareDelete(seed);
        }
        while(!task.pending.empty()){
          auto row=task.pending.back();task.pending.pop_back();task.rows.push_back(row);
          for(auto child=TreeView_GetChild(treeH,row);child;child=TreeView_GetNextSibling(treeH,child))task.pending.push_back(child);
          if(expired())return false;
        }
        task.collected=true;task.position=task.rows.size();
      }
      // Every row was recorded after its parent, so deleting in reverse order
      // removes children first and each call destroys exactly one row.
      while(task.position>0){
        TreeView_DeleteItem(treeH,task.rows[--task.position]);++treeStats.rowsDeleted;
        if(expired()&&task.position>0)return false;
      }
      if(task.reopen){task.reopen=false;TreeView_Expand(treeH,task.row,TVE_EXPAND);UpdateFolderItem(task.row,false);}
      return true;
    }
    case TreeTask::ExpandAll:case TreeTask::CollapseAll:{
      if(!task.collected)CollectDirectoryRows(task);
      while(task.position<task.rows.size()){
        auto row=task.rows[task.position++];
        TreeView_Expand(treeH,row,task.kind==TreeTask::ExpandAll?TVE_EXPAND:TVE_COLLAPSE);UpdateFolderItem(row,true);
        if(expired())return task.position>=task.rows.size();
      }
      return true;
    }
    }
    return true;
 }
 void ExpandOrCollapseAll(bool expand){
    const bool wasIdle=treeTasks.empty();if(wasIdle)BeginTreeWork();
    TreeTask task;task.kind=expand?TreeTask::ExpandAll:TreeTask::CollapseAll;
    if(wasIdle)CollectDirectoryRows(task); // Otherwise collected once queued insertions are done.
    const size_t rows=task.rows.size();
    treeTasks.push_back(std::move(task));
    StartTreeWork(wasIdle,rows,0);
 }
 // ---- Applying a scan result -----------------------------------------------------
 // Merge the incoming children into the live ones in sibling order. Matching
 // entries stay (recursing into changed folders); entries only in the live
 // model retire with their rows; entries only in the incoming model move into
 // the live model, and each run of them between two matched entries gets its
 // rows, with their descendants, through one queued task.
 void MergeTree(FolderRegistration& folder,mm::FolderNode& current,mm::FolderNode& incoming,HTREEITEM parentRow,size_t& inserts,size_t& deletes){
    const auto order=[](std::list<mm::FolderNode>::iterator a,std::list<mm::FolderNode>::iterator b,bool aEnd,bool bEnd){
      if(aEnd)return 1;if(bEnd)return -1;if(mm::FolderNodeBefore(*a,*b))return -1;if(mm::FolderNodeBefore(*b,*a))return 1;return 0;};
    auto oldIt=current.children.begin();auto newIt=incoming.children.begin();
    HTREEITEM row=TreeView_GetChild(treeH,parentRow),last=nullptr;
    // Retiring entries leave the list at once, so a run stays contiguous and
    // ends at the next matched entry or the end, neither of which moves while
    // the task waits. Its rows go after the matched row before the run.
    auto runFirst=current.children.end();HTREEITEM runAfter=nullptr;size_t runRows=0;
    std::vector<HTREEITEM> victims;size_t victimRows=0;
    const auto flush=[&]{
      if(runRows){EnqueueRun(folder,current,parentRow,runFirst,oldIt,runAfter,true,runRows);inserts+=runRows;runRows=0;}
      if(!victims.empty()){EnqueueDeleteRun(folder,parentRow,std::move(victims),victimRows);victims.clear();victimRows=0;}
    };
    while(oldIt!=current.children.end()||newIt!=incoming.children.end()){
      const int o=order(oldIt,newIt,oldIt==current.children.end(),newIt==incoming.children.end());
      if(o==0){
        flush();
        if(row&&oldIt->directory&&!(*oldIt==*newIt))MergeTree(folder,*oldIt,*newIt,row,inserts,deletes);
        last=row;if(row)row=TreeView_GetNextSibling(treeH,row);++oldIt;++newIt;
      }else if(o<0){
        auto victim=row;if(row)row=TreeView_GetNextSibling(treeH,row);
        const auto rows=CountNodes(*oldIt);deletes+=rows;
        folder.retiring.splice(folder.retiring.end(),current.children,oldIt++);
        if(victim){victims.push_back(victim);victimRows+=rows;}
      }else{
        auto moved=newIt++;current.children.splice(oldIt,incoming.children,moved);
        if(!runRows){runFirst=moved;runAfter=last;}
        runRows+=CountNodes(*moved);
      }
    }
    flush();
 }
 void RebuildTree(const std::function<void()>& change={}){
    EndRootDrag();
    if(change)change();
    const bool wasIdle=treeTasks.empty();if(wasIdle)BeginTreeWork();
    size_t inserts=0,deletes=0;
    treeUpdating=true;
    // Rows without a registration cannot stay; every registration gets its
    // root row in registration order. Existing rows are left untouched.
    for(auto row=TreeView_GetRoot(treeH);row;){auto next=TreeView_GetNextSibling(treeH,row);if(!FolderForRoot(NodeOf(row)))TreeView_DeleteItem(treeH,row);row=next;}
    HTREEITEM previous=nullptr;
    for(auto& f:folders){
      auto row=RootRow(*f);
      if(!row){
        auto& node=f->tree?*f->tree:f->pending;
        row=InsertRow(node,TVI_ROOT,previous?previous:TVI_FIRST);
        if(row){if(node.directory&&!node.children.empty()){inserts+=CountNodes(node)-1;EnqueueInsert(*f,node,row);}else UpdateFolderItem(row,false);}
      }
      if(row)previous=row;
    }
    treeUpdating=false;
    UpdateRootTitle();RebuildKnownFolders();InvalidateRect(treeH,nullptr,TRUE);
    StartTreeWork(wasIdle,inserts,deletes);
 }
 void ApplyTree(std::unique_ptr<mm::FolderNode> tree){
    auto*folder=tree?FindFolder(tree->path):nullptr;
    if(folder)ReplaceFolderTree(*folder,std::move(tree));else RebuildTree();
 }
 // tree==nullptr shows the placeholder row again. While rows of this
 // registration are still changing, the newest result waits its turn.
 void ReplaceFolderTree(FolderRegistration& folder,std::unique_ptr<mm::FolderNode> tree){
    if(folder.queuedTasks){folder.deferredTree=std::move(tree);folder.deferred=true;return;}
    EndRootDrag();
    const bool wasIdle=treeTasks.empty();if(wasIdle)BeginTreeWork();
    size_t inserts=0,deletes=0;
    auto root=RootRow(folder);
    treeUpdating=true;
    if(!root){
      HTREEITEM after=TVI_FIRST;
      for(const auto& f:folders){if(f.get()==&folder)break;if(auto row=RootRow(*f))after=row;}
      folder.tree=std::move(tree);
      auto& node=folder.tree?*folder.tree:folder.pending;
      root=InsertRow(node,TVI_ROOT,after);
      if(root){if(node.directory&&!node.children.empty()){inserts+=CountNodes(node)-1;EnqueueInsert(folder,node,root);}else UpdateFolderItem(root,false);}
    }else if(!tree){
      if(folder.tree){
        std::vector<HTREEITEM> children;
        for(auto child=TreeView_GetChild(treeH,root);child;child=TreeView_GetNextSibling(treeH,child))children.push_back(child);
        const auto rows=CountNodes(*folder.tree)-1;deletes+=rows;EnqueueDeleteRun(folder,root,std::move(children),rows);
        folder.retiringRoots.push_back(std::move(folder.tree));
        SetRowNode(root,folder.pending);UpdateFolderItem(root,false);
      }
    }else if(!folder.tree){
      folder.tree=std::move(tree);SetRowNode(root,*folder.tree);
      if(!folder.tree->children.empty()){inserts+=CountNodes(*folder.tree)-1;EnqueueInsert(folder,*folder.tree,root);}
      else UpdateFolderItem(root,false);
    }else{
      MergeTree(folder,*folder.tree,*tree,root,inserts,deletes);
    }
    treeUpdating=false;
    UpdateRootTitle();RebuildKnownFolders();InvalidateRect(treeH,nullptr,TRUE);
    StartTreeWork(wasIdle,inserts,deletes);
 }
 void UpdateRootTitle(){
    for(auto item=TreeView_GetRoot(treeH);item;item=TreeView_GetNextSibling(treeH,item)){
      auto*node=NodeOf(item);auto*folder=FolderForRoot(node);if(!folder||!FindFolder(folder->id))continue;
      auto title=node->name;if(!folder->issue.empty())title+=L" [要確認]";
      // One extra character distinguishes a longer existing title whose
      // prefix matches, while avoiding repeated unchanged TVM_SETITEM calls.
      TVITEMW info{};info.hItem=item;
      std::wstring current(title.size()+2,L'\0');info.mask=TVIF_TEXT;info.pszText=current.data();info.cchTextMax=int(current.size());
      if(TreeView_GetItem(treeH,&info)&&wcscmp(current.c_str(),title.c_str())==0)continue;
      info.mask=TVIF_TEXT;info.pszText=title.data();TreeView_SetItem(treeH,&info);
    }
 }
 void AcceptTree(std::unique_ptr<Scanned>result){
    auto*folder=FindFolder(result->folderId);if(!folder||result->generation!=folder->generation||(folder->cancel&&*folder->cancel))return;
    folder->scanning=false;folder->issue=result->error;
    if(result->tree&&result->error.empty()&&result->coverage.complete){
      bool changed=false;
      for(const auto& missing:result->coverage.missingFolders)
        if(OwnsExpansion(*folder,missing))changed=expanded.erase(missing)!=0||changed;
      if(changed)Dirty();
    }
    // An unchanged scan keeps the native rows and the model they point to;
    // only the title can differ, through the issue text.
    if(result->tree)FilterExcludedFolders(*result->tree);
    const mm::FolderNode* current=folder->deferred?folder->deferredTree.get():folder->tree.get();
    const bool unchanged=current&&result->tree?*current==*result->tree:!current&&!result->tree;
    const bool emptyResult=result->tree&&result->tree->children.empty();
    if(unchanged&&HasRootItem(*folder))UpdateRootTitle();
    else ReplaceFolderTree(*folder,std::move(result->tree));
    notice=result->error.empty()?(emptyResult?L"一覧に表示する .md はありません":L""):result->error;InvalidateRect(hwnd,nullptr,FALSE);
    if(folder->rescanRequested){folder->rescanRequested=false;RefreshFolder(*folder);}
 }
 static bool IsWithinFolder(const std::wstring& path,const std::wstring& folder){
    if(_wcsicmp(path.c_str(),folder.c_str())==0)return true;
    return path.size()>folder.size()&&_wcsnicmp(path.c_str(),folder.c_str(),folder.size())==0
      &&(!folder.empty()&&(folder.back()==L'\\'||path[folder.size()]==L'\\'));
 }
 bool OwnsExpansion(const FolderRegistration& folder,const std::wstring& path)const{
    if(!IsWithinFolder(path,folder.path))return false;
    // A separately registered nested root owns its history, including while
    // temporarily unavailable. A broader root must not erase those settings.
    for(const auto& other:folders)
      if(other->path.size()>folder.path.size()&&IsWithinFolder(path,other->path))return false;
    return true;
 }
 void RemoveFolderFromList(std::wstring path){
    if(!FindFolder(path)&&!knownFolders.contains(path))return;
    std::vector<unsigned long long> resume;
    for(const auto& folder:folders)if(folder->scanning&&!IsWithinFolder(folder->path,path))resume.push_back(folder->id);
    CancelStartupCheck();
    // Stop the scan threads' I/O before resuming retained roots. IDs and
    // per-root generations reject results already queued by a removed root.
    CancelFolderScans();
    treeDirty=false;KillTimer(hwnd,2);KillTimer(hwnd,4);watchLimited=false;watchRescanPending=false;CancelWatchSetup();
    for(auto&t:tabs)if(IsWithinFolder(t.path,path))t.autoRefresh=false;
    if(active>=0&&IsWithinFolder(tabs[active].path,path)){
      if(readCancel)*readCancel=true;readRecheck.reset();readWorker.CancelPending();++loadGeneration;loading=false;
      if(displayedTabId!=tabs[active].id){currentDoc.reset();view.SetDocument("# 読み込みを停止しました\n\nフォルダをリストから外したため、自動更新を停止しています。",tabs[active].path);}
    }
    EndRootDrag();
    const bool wasIdle=treeTasks.empty();if(wasIdle)BeginTreeWork();
    size_t deletes=0;
    // Registrations inside the removed path retire together with their rows;
    // the registration object outlives the rows that still point into it.
    for(auto it=folders.begin();it!=folders.end();){
      auto& f=**it;
      if(!IsWithinFolder(f.path,path)){++it;continue;}
      std::erase_if(treeTasks,[&](const TreeTask& task){if(task.folder==&f&&task.kind==TreeTask::Insert){--f.queuedTasks;return true;}return false;});
      f.deferred=false;f.deferredTree.reset();
      treeUpdating=true;
      if(auto row=RootRow(f)){deletes+=CountNodes(f.tree?*f.tree:f.pending);EnqueueDelete(f,row);}
      treeUpdating=false;
      retiringFolders.push_back(std::move(*it));it=folders.erase(it);
    }
    std::erase_if(excludedFolders,[&](const auto& excluded){return IsWithinFolder(excluded,path);});
    excludedFolders.insert(path);
    std::erase_if(expanded,[&](const auto& open){return IsWithinFolder(open,path);});
    std::erase_if(watchRescanFolders,[&](const auto& pending){return IsWithinFolder(pending,path);});
    // A remaining root that contains the path drops that branch as a difference.
    // The newest known model is the deferred result while its rows still change.
    bool replaced=false;
    for(auto& f:folders)if(IsWithinFolder(path,f->path)){
      const mm::FolderNode* base=f->deferred?f->deferredTree.get():f->tree.get();if(!base)continue;
      auto filtered=std::make_unique<mm::FolderNode>(*base);FilterExcludedFolders(*filtered);ReplaceFolderTree(*f,std::move(filtered));replaced=true;
    }
    UpdateRootTitle();RebuildKnownFolders();InvalidateRect(treeH,nullptr,TRUE);
    StartTreeWork(wasIdle&&!replaced,0,deletes);
    if(treeTasks.empty())FinishTreeTasks(); // Retire registrations whose rows were already gone.
    SetupWatches();for(auto id:resume)if(auto*folder=FindFolder(id))RefreshFolder(*folder);
    notice=L"「"+fs::path(path).filename().wstring()+L"」をリストから削除し、探索・自動更新を停止しました";InvalidateRect(hwnd,nullptr,FALSE);Dirty();
 }
 std::wstring FolderAt(POINT screen){
    if(!sidebar)return {};
    ScreenToClient(treeH,&screen);TVHITTESTINFO hit{};hit.pt=screen;
    auto item=TreeView_HitTest(treeH,&hit);if(!item||!(hit.flags&TVHT_ONITEM))return {};
    TVITEMW info{};info.mask=TVIF_PARAM;info.hItem=item;if(!TreeView_GetItem(treeH,&info))return {};
    auto*node=reinterpret_cast<mm::FolderNode*>(info.lParam);return node&&node->directory?node->path:L"";
 }
 HMENU TreeMenu(bool folder){
    HMENU menu=CreatePopupMenu();
    if(folder){AppendMenuW(menu,MF_STRING,RemoveFromList,L"リストから削除");AppendMenuW(menu,MF_SEPARATOR,0,nullptr);}
    AppendMenuW(menu,MF_STRING,ExpandAll,L"すべて展開");AppendMenuW(menu,MF_STRING,CollapseAll,L"すべて折りたたむ");AppendMenuW(menu,MF_STRING,Reload,L"フォルダを更新");return menu;
 }
 void RunTreeMenuCommand(int command,const std::wstring& path,unsigned long long folderId){
    if(closing)return; // The popup's modal loop may have pumped WM_CLOSE.
    // Re-adding a removed path creates a new registration, so an old popup
    // cannot act on it. Adding an unrelated root leaves this popup valid.
    auto*folder=FindFolder(folderId);
    if(folderId&&(!folder||(!path.empty()&&!IsWithinFolder(path,folder->path))))return;
    if(command==RemoveFromList){if(folder)RemoveFolderFromList(path);}else if(command)Command(command);
 }
 void ShowTreeMenu(POINT point,const std::wstring& path,unsigned long long folderId=0){
    if(!folderId){TVHITTESTINFO hit{};hit.pt=point;ScreenToClient(treeH,&hit.pt);if(auto*folder=FolderForItem(TreeView_HitTest(treeH,&hit)))folderId=folder->id;}
    HMENU menu=TreeMenu(!path.empty());
    int command=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,point.x,point.y,0,hwnd,nullptr);DestroyMenu(menu);
    RunTreeMenuCommand(command,path,folderId);
 }
 void ShowTreeMenu(POINT point){ShowTreeMenu(point,FolderAt(point));}
 void WalkItems(HTREEITEM item,const std::function<void(HTREEITEM,mm::FolderNode*)>&fn){for(;item;item=TreeView_GetNextSibling(treeH,item)){TVITEMW info{};info.hItem=item;info.mask=TVIF_PARAM;TreeView_GetItem(treeH,&info);fn(item,reinterpret_cast<mm::FolderNode*>(info.lParam));WalkItems(TreeView_GetChild(treeH,item),fn);}}
 void SelectTreeFile(bool ensureVisible=false){
    if(active<0)return;auto matches=[&](const mm::FolderNode* node){return node&&!node->directory&&_wcsicmp(node->path.c_str(),tabs[active].path.c_str())==0;};
    TVITEMW selected{};selected.hItem=TreeView_GetSelection(treeH);selected.mask=TVIF_PARAM;
    if(selected.hItem&&TreeView_GetItem(treeH,&selected)&&matches(reinterpret_cast<mm::FolderNode*>(selected.lParam))){
      if(ensureVisible){const bool updating=treeUpdating;treeUpdating=true;TreeView_EnsureVisible(treeH,selected.hItem);treeUpdating=updating;}return;}
    HTREEITEM match=nullptr;
    if(auto found=rowsByPath.find(tabs[active].path);found!=rowsByPath.end()&&matches(NodeOf(found->second)))match=found->second;
    // Paths outside every root have no row. Anything else still walks the
    // control, in case the index and the comparison fold letters differently.
    if(!match&&std::any_of(folders.begin(),folders.end(),[&](const auto& folder){return IsWithinFolder(tabs[active].path,folder->path);}))
      WalkItems(TreeView_GetRoot(treeH),[&](auto item,auto*node){if(!match&&matches(node))match=item;});
    if(match){treeUpdating=true;TreeView_SelectItem(treeH,match);TreeView_EnsureVisible(treeH,match);treeUpdating=false;}
 }
 bool IsExcluded(const std::wstring& path)const{return std::any_of(excludedFolders.begin(),excludedFolders.end(),[&](const auto& excluded){return IsWithinFolder(path,excluded);});}
 static void CheckWatchCancellation(const std::atomic_bool* cancel){if(cancel&&*cancel)throw std::runtime_error("WATCH_CANCELLED");}
 static bool IsExcludedPath(const std::wstring& path,const std::vector<std::wstring>& excluded){return std::any_of(excluded.begin(),excluded.end(),[&](const auto& folder){return IsWithinFolder(path,folder);});}
 static void PlanTreeWatches(const std::wstring& folder,WatchPlan& plan,const std::vector<std::wstring>& excluded,const std::atomic_bool* cancel,bool& limited){
    CheckWatchCancellation(cancel);if(IsExcludedPath(folder,excluded))return;
    // A registered child can share an already planned recursive parent. This
    // also avoids charging duplicate registrations against the global limit.
    for(auto&[path,spec]:plan)if(spec.recursive&&IsWithinFolder(folder,path)){spec.treeWatch=true;return;}
    const bool split=std::any_of(excluded.begin(),excluded.end(),[&](const auto& path){return IsWithinFolder(path,folder);});
    if(!split)std::erase_if(plan,[&](const auto& entry){return _wcsicmp(entry.first.c_str(),folder.c_str())!=0&&IsWithinFolder(entry.first,folder);});
    if(!plan.contains(folder)&&plan.size()>=64){limited=true;return;}
    plan[folder]={!split,true};if(!split)return;
    // Windows cannot exclude a subtree from a recursive watch. Enumerate only
    // its ancestors here, in the worker, and watch retained siblings recursively.
    struct Finder{HANDLE value=INVALID_HANDLE_VALUE;~Finder(){if(value!=INVALID_HANDLE_VALUE)FindClose(value);}} find;
    WIN32_FIND_DATAW item{};auto pattern=(fs::path(folder)/L"*").wstring();CheckWatchCancellation(cancel);
    find.value=FindFirstFileExW(pattern.c_str(),FindExInfoBasic,&item,FindExSearchLimitToDirectories,nullptr,FIND_FIRST_EX_LARGE_FETCH);
    CheckWatchCancellation(cancel);if(find.value==INVALID_HANDLE_VALUE){limited=true;return;}
    do {CheckWatchCancellation(cancel);if((item.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)&&!(item.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)
      &&wcscmp(item.cFileName,L".")!=0&&wcscmp(item.cFileName,L"..")!=0){
        const auto path=(fs::path(folder)/item.cFileName).wstring();if(IsExcludedPath(path,excluded))continue;
        PlanTreeWatches(path,plan,excluded,cancel,limited);
      }
    }while(FindNextFileW(find.value,&item));const auto error=GetLastError();CheckWatchCancellation(cancel);if(error!=ERROR_NO_MORE_FILES)limited=true;
 }
 void CancelWatchSetup(){
    KillTimer(hwnd,5);watchRetryPending=false;watchSetupPending=false;
    if(watchCancel)*watchCancel=true;++watchGeneration;watchWorker.CancelPending();
    MSG message;while(PeekMessageW(&message,hwnd,WatchDone,WatchDone,PM_REMOVE)){
      std::unique_ptr<Watched> stale(reinterpret_cast<Watched*>(message.lParam));RetireWatches(std::move(stale->watches));
    }
 }
 void RetireWatches(std::vector<std::unique_ptr<Watch>> retired){
    if(retired.empty())return;for(auto& watch:retired)watch->Cancel();
    auto batch=std::make_shared<std::vector<std::unique_ptr<Watch>>>(std::move(retired));
    watchWorker.Cleanup([batch]{batch->clear();});
 }
 bool KeepWatch(const Watch& watch)const{
    if(!watch.pending||IsExcluded(watch.path))return false;
    if(watch.recursive&&std::any_of(excludedFolders.begin(),excludedFolders.end(),[&](const auto& excluded){return IsWithinFolder(excluded,watch.path);}))return false;
    if(watch.treeWatch&&std::any_of(folders.begin(),folders.end(),[&](const auto& folder){return IsWithinFolder(watch.path,folder->path);}))return true;
    return !watch.recursive&&std::any_of(tabs.begin(),tabs.end(),[&](const auto& tab){return tab.autoRefresh&&!tab.path.empty()&&!IsExcluded(tab.path)&&_wcsicmp(fs::path(tab.path).parent_path().c_str(),watch.path.c_str())==0;});
 }
 void SetupWatches(bool rescanAfter=false){
    if(closing)return;
    const bool recovery=watchRetryPending||watchLimited||std::any_of(watches.begin(),watches.end(),[](const auto& watch){return !watch->pending;});
    watchRescanPending|=rescanAfter;rescanAfter=watchRescanPending;CancelWatchSetup();std::vector<std::unique_ptr<Watch>> retired;
    for(auto it=watches.begin();it!=watches.end();)if(!KeepWatch(**it)){retired.push_back(std::move(*it));it=watches.erase(it);}else ++it;
    RetireWatches(std::move(retired));UpdateWatchTimer();
    watchCancel=std::make_shared<std::atomic_bool>(false);auto cancel=watchCancel;auto generation=watchGeneration;auto h=hwnd;auto roots=FolderPaths();
    std::sort(roots.begin(),roots.end(),[](const auto& left,const auto& right){return left.size()!=right.size()?left.size()<right.size():FolderPathLess{}(left,right);});
    std::vector<std::wstring> excluded(excludedFolders.begin(),excludedFolders.end()),tabFolders;
    for(const auto& tab:tabs)if(tab.autoRefresh&&!tab.path.empty()&&!IsExcluded(tab.path))tabFolders.push_back(fs::path(tab.path).parent_path().wstring());
    WatchPlan existing;for(const auto& watch:watches)existing[watch->path]={watch->recursive,watch->treeWatch};
    watchSetupPending=true;
    watchWorker.Queue([h,cancel,generation,roots=std::move(roots),excluded=std::move(excluded),tabFolders=std::move(tabFolders),existing=std::move(existing),rescanAfter,recovery]{
      auto result=std::make_unique<Watched>();result->generation=generation;result->rescanAfter=rescanAfter;result->recovery=recovery;
      try{
        CheckWatchCancellation(cancel.get());result->plan=existing;
        for(auto&[path,spec]:result->plan)spec.treeWatch=std::any_of(roots.begin(),roots.end(),[&](const auto& root){return IsWithinFolder(path,root);});
        for(const auto& root:roots){CheckWatchCancellation(cancel.get());PlanTreeWatches(root,result->plan,excluded,cancel.get(),result->limited);}
        for(const auto& path:tabFolders){CheckWatchCancellation(cancel.get());bool covered=std::any_of(result->plan.begin(),result->plan.end(),[&](const auto& entry){return _wcsicmp(path.c_str(),entry.first.c_str())==0||(entry.second.recursive&&IsWithinFolder(path,entry.first));});
          if(!covered){if(result->plan.size()>=64){result->limited=true;break;}result->plan[path]={false,false};}}
        for(auto it=result->plan.begin();it!=result->plan.end();){CheckWatchCancellation(cancel.get());const auto&[path,spec]=*it;auto old=existing.find(path);
          if(old!=existing.end()&&old->second.recursive==spec.recursive){++it;continue;}
          // Existing handles stay armed until this result is accepted. Count
          // them too, so replacing covered child watches cannot exceed 64.
          if(existing.size()+result->watches.size()>=64){result->limited=true;it=result->plan.erase(it);continue;}
          auto watch=std::make_unique<Watch>();watch->path=path;watch->recursive=spec.recursive;watch->treeWatch=spec.treeWatch;
          CheckWatchCancellation(cancel.get());watch->dir=CreateFileW(path.c_str(),FILE_LIST_DIRECTORY,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OVERLAPPED,nullptr);
          CheckWatchCancellation(cancel.get());if(watch->dir==INVALID_HANDLE_VALUE){result->limited=true;result->retry=true;it=result->plan.erase(it);continue;}
          watch->event=CreateEventW(nullptr,TRUE,FALSE,nullptr);CheckWatchCancellation(cancel.get());
          if(watch->event&&watch->Arm()){CheckWatchCancellation(cancel.get());result->watches.push_back(std::move(watch));++it;}
          else{result->limited=true;result->retry=true;it=result->plan.erase(it);}
        }
        // If a new parent watch cannot be opened (or would exceed the limit),
        // retain its previously working child registrations instead of losing
        // the other roots' monitoring during an unrelated folder addition.
        for(const auto&[path,spec]:existing){CheckWatchCancellation(cancel.get());const bool covered=std::any_of(result->plan.begin(),result->plan.end(),[&](const auto& entry){return _wcsicmp(path.c_str(),entry.first.c_str())==0||(entry.second.recursive&&IsWithinFolder(path,entry.first));});
          if(!covered){auto restored=spec;restored.treeWatch=std::any_of(roots.begin(),roots.end(),[&](const auto& root){return IsWithinFolder(path,root);});result->plan[path]=restored;}
        }
      }catch(const std::exception&){if(*cancel)return;result->limited=true;result->retry=true;}
      if(!*cancel&&PostMessageW(h,WatchDone,0,LPARAM(result.get())))result.release();
    });
 }
 void AcceptWatches(std::unique_ptr<Watched> result){
    if(closing||result->generation!=watchGeneration||(watchCancel&&*watchCancel)){RetireWatches(std::move(result->watches));return;}
    watchSetupPending=false;
    std::vector<std::unique_ptr<Watch>> retired;
    for(auto it=watches.begin();it!=watches.end();){auto spec=result->plan.find((*it)->path);
      if(!(*it)->pending||spec==result->plan.end()||spec->second.recursive!=(*it)->recursive){
        if(!(*it)->pending&&spec!=result->plan.end()){result->retry=true;result->limited=true;}
        retired.push_back(std::move(*it));it=watches.erase(it);
      }else{(*it)->treeWatch=spec->second.treeWatch;++it;}}
    const bool recovered=result->recovery&&!result->watches.empty();
    if(recovered)for(const auto& watch:result->watches)for(const auto& folder:folders)
      if(IsWithinFolder(watch->path,folder->path)||IsWithinFolder(folder->path,watch->path))watchRescanFolders.insert(folder->path);
    for(auto& watch:result->watches)watches.push_back(std::move(watch));RetireWatches(std::move(retired));UpdateWatchTimer();
    watchLimited=result->limited;KillTimer(hwnd,4);if(watchLimited)SetTimer(hwnd,4,30000,nullptr);
    if(result->retry||watchRetryPending)ScheduleWatchRetry();
    if(recovered)CheckActive();
    watchRescanPending=false;if(result->rescanAfter)RefreshTree();else RefreshPendingTrees();InvalidateRect(hwnd,nullptr,FALSE);
 }
 void ScheduleWatchRetry(){
    if(closing)return;
    if(!watchRetryPending){watchRetryPending=true;SetTimer(hwnd,5,3000,nullptr);}
 }
 // The completion poll runs only while something is watched.
 void UpdateWatchTimer(){if(watches.empty())KillTimer(hwnd,1);else if(!closing)SetTimer(hwnd,1,300,nullptr);}
 static bool HasMarkdownSuffix(std::wstring_view name){
    // The name after its last separator must be longer than the suffix: a file
    // called ".md" has no extension, as mm::IsMarkdown sees it.
    const auto separator=name.find_last_of(L"\\/");const auto fileName=separator==std::wstring_view::npos?name:name.substr(separator+1);
    const auto ends=[&](std::wstring_view suffix){return fileName.size()>suffix.size()&&_wcsnicmp(fileName.data()+fileName.size()-suffix.size(),suffix.data(),suffix.size())==0;};
    return ends(L".md")||ends(L".markdown");
 }
 static std::wstring JoinWatchPath(const std::wstring& folder,std::wstring_view name){
    std::wstring result;result.reserve(folder.size()+1+name.size());result+=folder;
    if(result.empty()||(result.back()!=L'\\'&&result.back()!=L'/'))result+=L'\\';result.append(name);return result;
 }
 // True while every root a notification under this watch could mark is
 // already waiting for its rescan and a watch update is still due (treeDirty):
 // a directory created meanwhile then shows up through that rescan and, where
 // watches are split, gets its own watch from that update, so the disk need
 // not be asked about it now.
 bool RescanPending(const Watch& watch)const{
    if(!treeDirty)return false;
    bool any=false;
    for(const auto& folder:folders)if(IsWithinFolder(watch.path,folder->path)||IsWithinFolder(folder->path,watch.path)){any=true;if(!watchRescanFolders.contains(folder->path))return false;}
    return any;
 }
 // A structural change schedules a rescan of just the registered roots that
 // contain it; a lost or overflowed watch also covers roots inside its path.
 void MarkTreeDirty(const std::wstring& path){
    treeDirty=true;
    for(const auto& folder:folders)if(IsWithinFolder(path,folder->path))watchRescanFolders.insert(folder->path);
 }
 void MarkWatchDirty(const Watch& watch){
    treeDirty=true;
    for(const auto& folder:folders)if(IsWithinFolder(watch.path,folder->path)||IsWithinFolder(folder->path,watch.path))watchRescanFolders.insert(folder->path);
 }
 void PollWatches(){
    if(closing)return;bool change=false;++pollCount;
    for(auto& w:watches){
      if(!w->pending||WaitForSingleObject(w->event,0)!=WAIT_OBJECT_0)continue;
      DWORD bytes=0;const bool ok=GetOverlappedResult(w->dir,&w->ov,&bytes,FALSE)!=FALSE;const auto error=ok?ERROR_SUCCESS:GetLastError();w->pending=false;
      if(!ok||bytes==0){if(bytes==0&&(ok||error==ERROR_NOTIFY_ENUM_DIR))++watchOverflowCount;change=true;if(w->treeWatch)MarkWatchDirty(*w);}
      else for(DWORD offset=0;offset<bytes;){
        auto*n=reinterpret_cast<FILE_NOTIFY_INFORMATION*>(w->data.data()+offset);
        const std::wstring_view name(n->FileName,n->FileNameLength/sizeof(wchar_t));const bool markdown=HasMarkdownSuffix(name);
        // Content changes of other files never matter; they are the bulk of a
        // build's notifications and leave before a path is even built.
        if(markdown||(n->Action!=FILE_ACTION_MODIFIED&&w->treeWatch)){
          const auto full=JoinWatchPath(w->path,name);
          if(!IsExcluded(full)){
            if(markdown){change=true;if(w->treeWatch&&n->Action!=FILE_ACTION_MODIFIED)MarkTreeDirty(full);}
            else{
              // A vanished path the tree showed as a folder matters as much as a
              // new directory, which may have arrived with contents through a
              // move. A removed name cannot be examined, and while the roots
              // this watch covers already wait for a rescan, the disk is not
              // asked about every further name. Neither touches the control.
              bool folder=knownFolders.contains(full);
              if(!folder&&n->Action!=FILE_ACTION_REMOVED&&n->Action!=FILE_ACTION_RENAMED_OLD_NAME&&!RescanPending(*w)){const auto attributes=GetFileAttributesW(full.c_str());folder=attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&FILE_ATTRIBUTE_DIRECTORY);}
              if(folder){change=true;MarkTreeDirty(full);}
            }
          }
        }
        if(!n->NextEntryOffset)break;offset+=n->NextEntryOffset;
      }
      // Overflow can reuse the handle after rescanning. Other completion errors
      // require reopening it; repeatedly rearming a dead handle can spin.
      if((!ok&&error!=ERROR_NOTIFY_ENUM_DIR)||!w->Arm()){change=true;if(w->treeWatch)MarkWatchDirty(*w);ScheduleWatchRetry();}
    }
    if(change)SetTimer(hwnd,2,350,nullptr);
 }
 void CheckActive(){
    if(closing||(encodingPromptCancel&&encodingPromptCancel==readCancel)||active<0||tabs[active].path.empty()||!tabs[active].autoRefresh||IsExcluded(tabs[active].path))return;
    if(loading){readRecheck=readCancel;return;}Capture();LoadActive(false,false,true);
 }
 void Zoom(int delta){int font=active>=0?tabs[active].font:fontDefault;font=delta==999?16:std::clamp(font+delta,10,40);if(active>=0)tabs[active].font=font;fontDefault=font;view.SetFontSize(font);Dirty();InvalidateRect(hwnd,nullptr,FALSE);}
 void DoSearch(int direction){int n=GetWindowTextLengthW(searchH);std::wstring q(n+1,L'\0');GetWindowTextW(searchH,q.data(),n+1);q.resize(n);view.Search(q,direction);auto[current,total]=view.SearchResult();SetWindowTextW(countH,(std::to_wstring(current)+L" / "+std::to_wstring(total)).c_str());}
 void Command(int id){switch(id){case UiOpenFile:SelectFiles();break;case OpenFolder:SelectFolder();break;case NewTab:AddEmpty();break;case CloseTab:Close(active);break;case CloseAllTabs:CloseAll();break;case Sidebar:sidebar=!sidebar;Layout();Dirty();break;
    case Theme:dark=!dark;ApplyTheme();break;case Source:if(active>=0){tabs[active].source=!tabs[active].source;view.SetSourceMode(tabs[active].source);Dirty();}break;
    case Wide:wide=!wide;view.SetWide(wide);Dirty();break;case Find:searching=true;Layout();SetFocus(searchH);SendMessageW(searchH,EM_SETSEL,0,-1);break;
    case SearchNext:DoSearch(1);break;case SearchPrev:DoSearch(-1);break;case SearchClose:searching=false;SetWindowTextW(searchH,L"");DoSearch(0);Layout();SetFocus(view.Handle());break;
    case Reload:
      Capture();LoadActive(true,true);
      // Reconcile missing Markdown and empty descendant folders immediately
      // on the scan workers, even if rebuilding directory watches is blocked.
      // Keep the post-watch scan too: a new watch cannot report changes that
      // happened between this first scan and the moment it was armed.
      RefreshTree();SetupWatches(true);break;
    case ZoomIn:Zoom(1);break;case ZoomOut:Zoom(-1);break;case ZoomReset:Zoom(999);break;
    case Reopen:if(!closed.empty()){Capture();auto t=closed.back();closed.pop_back();int existing=-1;for(size_t i=0;i<tabs.size();i++)if(_wcsicmp(tabs[i].path.c_str(),t.path.c_str())==0){existing=int(i);break;}if(existing>=0){Switch(existing);break;}t.id=nextId++;tabs.push_back(t);active=int(tabs.size())-1;RebuildTabs();LoadActive();SetupWatches();Dirty();}break;
    case CollapseAll:case ExpandAll:ExpandOrCollapseAll(id==ExpandAll);Dirty();break;
    case About:MessageBoxW(hwnd,L"MMViewer 0.1.0\nC++ / Win32 · Markdown / Mermaidビューアー\n\nMarkdown parser: md4c 0.5.2 (MIT)\nMermaid: 公式 11.17.2 (MIT) / WebView2\nELK・ZenUML・KaTeX同梱\n\n.md を含むフォルダだけを階層表示します。\n図の表示にはWebView2 Runtimeが必要です。\n描画エンジンと依存ライセンスはexeに同梱しています。",L"MMViewerについて",MB_OK);break;}}
 bool Key(MSG&msg){if(msg.message!=WM_KEYDOWN)return false;bool ctrl=GetKeyState(VK_CONTROL)<0,shift=GetKeyState(VK_SHIFT)<0;int key=int(msg.wParam),id=0;
    if(key==VK_ESCAPE&&rootDragId){EndRootDrag();return true;}
    if(!ctrl&&GetKeyState(VK_MENU)>=0&&(key==VK_HOME||key==VK_END||key==VK_PRIOR||key==VK_NEXT)
      &&(msg.hwnd==hwnd||IsChild(hwnd,msg.hwnd))&&msg.hwnd!=view.Handle()
      &&msg.hwnd!=searchH&&msg.hwnd!=treeH&&!IsChild(treeH,msg.hwnd)){
      // Tab and toolbar focus should not make document navigation disappear.
      // The search edit and folder tree retain their native key behavior.
      SetFocus(view.Handle());SendMessageW(view.Handle(),WM_KEYDOWN,msg.wParam,msg.lParam);return true;
    }
    if(ctrl){switch(key){case 'O':id=shift?OpenFolder:UiOpenFile;break;case 'T':id=shift?Reopen:NewTab;break;case 'W':id=CloseTab;break;case 'B':id=Sidebar;break;case 'F':id=Find;break;case 'U':id=Source;break;case VK_OEM_PLUS:case VK_ADD:id=ZoomIn;break;case VK_OEM_MINUS:case VK_SUBTRACT:id=ZoomOut;break;case '0':id=ZoomReset;break;case VK_TAB:if(!tabs.empty())Switch((active+(shift?int(tabs.size())-1:1))%int(tabs.size()));return true;}}
    else if(key==VK_F1)id=About;else if(key==VK_F5)id=Reload;else if(key==VK_F3)id=shift?SearchPrev:SearchNext;else if(key==VK_ESCAPE&&searching)id=SearchClose;
    if(id){Command(id);return true;}return false;}
 static LRESULT CALLBACK SearchProc(HWND h,UINT m,WPARAM w,LPARAM l,UINT_PTR,DWORD_PTR data){auto*a=reinterpret_cast<App*>(data);if(m==WM_KEYDOWN&&w==VK_RETURN){a->DoSearch(GetKeyState(VK_SHIFT)<0?-1:1);return 0;}return DefSubclassProc(h,m,w,l);}
 static LRESULT CALLBACK TreeProc(HWND h,UINT m,WPARAM w,LPARAM l,UINT_PTR,DWORD_PTR data){
    auto*a=reinterpret_cast<App*>(data);
    if(m==WM_MOUSEWHEEL){
      const auto keys=GET_KEYSTATE_WPARAM(w);
      if((keys&MK_SHIFT)&&!(keys&MK_CONTROL)){
        a->treeWheelRemainder+=GET_WHEEL_DELTA_WPARAM(w);
        const int steps=a->treeWheelRemainder/WHEEL_DELTA;a->treeWheelRemainder%=WHEEL_DELTA;
        if(steps)return DefSubclassProc(h,WM_MOUSEHWHEEL,MAKEWPARAM(keys&~MK_SHIFT,WORD(-steps*WHEEL_DELTA)),l);
        return 0;
      }
      a->treeWheelRemainder=0;
    }
    if(m==WM_CANCELMODE||m==WM_CAPTURECHANGED||m==WM_KILLFOCUS||(m==WM_SHOWWINDOW&&!w)||m==WM_SIZE)a->EndRootDrag();
    if(m==WM_KEYDOWN&&w==VK_ESCAPE&&a->rootDragId){a->EndRootDrag();return 0;}
    if(m==WM_SETCURSOR&&a->rootDragging){SetCursor(LoadCursorW(nullptr,a->rootDropId?IDC_SIZEALL:IDC_NO));return TRUE;}
    if(m==WM_TIMER&&w==RootDragTimer){
      if(a->rootDragging&&GetCapture()==h){RECT client{};GetClientRect(h,&client);auto p=a->rootDragPoint;
        if(p.x>=0&&p.x<client.right){if(p.y<a->D(20))SendMessageW(h,WM_VSCROLL,SB_LINEUP,0);else if(p.y>=client.bottom-a->D(20))SendMessageW(h,WM_VSCROLL,SB_LINEDOWN,0);}
        a->UpdateRootDrop(p);
      }return 0;
    }
    if(m==WM_MOUSEMOVE&&a->rootDragId){
      if(!(w&MK_LBUTTON)||GetCapture()!=h||!a->FindFolder(a->rootDragId)){a->EndRootDrag();return 0;}
      POINT point{GET_X_LPARAM(l),GET_Y_LPARAM(l)};
      if(!a->rootDragging&&(abs(point.x-a->rootDragStart.x)>=GetSystemMetricsForDpi(SM_CXDRAG,a->dpi)||abs(point.y-a->rootDragStart.y)>=GetSystemMetricsForDpi(SM_CYDRAG,a->dpi))){
        a->rootDragging=true;SetTimer(h,RootDragTimer,100,nullptr);
      }
      if(a->rootDragging)a->UpdateRootDrop(point);return 0;
    }
    if(m==WM_LBUTTONUP&&a->rootDragId){
      POINT point{GET_X_LPARAM(l),GET_Y_LPARAM(l)};const bool dragged=a->rootDragging;
      if(dragged)a->UpdateRootDrop(point);auto source=a->rootDragId,target=a->rootDropId;const bool after=a->rootDropAfter;a->EndRootDrag();
      if(dragged){if(target)a->ReorderRootFolders(source,target,after);}
      else{TVHITTESTINFO hit{};hit.pt=point;auto item=TreeView_HitTest(h,&hit);auto*folder=item&&!TreeView_GetParent(h,item)?a->FolderForItem(item):nullptr;
        if(folder&&folder->id==source&&(hit.flags&(TVHT_ONITEMICON|TVHT_ONITEMLABEL))){TreeView_Expand(h,item,TVE_TOGGLE);a->UpdateFolderItem(item,true);}}
      return 0;
    }
    if(m==WM_RBUTTONDOWN){a->EndRootDrag();return 0;}
    if(m==WM_RBUTTONUP||m==WM_CONTEXTMENU){
      a->EndRootDrag();
      if(!a->sidebar)return 0;POINT point{GET_X_LPARAM(l),GET_Y_LPARAM(l)};
      if(m==WM_RBUTTONUP)ClientToScreen(h,&point);
      else if(point.x==-1&&point.y==-1){
        auto item=TreeView_GetSelection(h);if(!item)item=TreeView_GetRoot(h);RECT row{};
        if(!item||!TreeView_GetItemRect(h,item,&row,TRUE))return 0;
        TVITEMW info{};info.mask=TVIF_PARAM;info.hItem=item;if(!TreeView_GetItem(h,&info))return 0;
        auto*node=reinterpret_cast<mm::FolderNode*>(info.lParam);
        const std::wstring path=node&&node->directory?node->path:L"";
        RECT client{};GetClientRect(h,&client);
        point={std::clamp(row.left+2,0L,std::max(0L,client.right-1)),std::clamp((row.top+row.bottom)/2,0L,std::max(0L,client.bottom-1))};ClientToScreen(h,&point);
        auto*folder=a->FolderForItem(item);a->ShowTreeMenu(point,path,folder?folder->id:0);return 0;
      }
      a->ShowTreeMenu(point);return 0;
    }
    if(m==WM_LBUTTONDOWN||m==WM_LBUTTONDBLCLK){
      TVHITTESTINFO hit{};hit.pt={GET_X_LPARAM(l),GET_Y_LPARAM(l)};auto item=TreeView_HitTest(h,&hit);
      if(item&&(hit.flags&TVHT_ONITEMRIGHT)){
        auto*node=a->NodeOf(item);
        // Use the normal selection notification to open files from the row's trailing space.
        if(node&&!node->directory){SetFocus(h);TreeView_SelectItem(h,item);return 0;}
      }
      if(item&&(hit.flags&(TVHT_ONITEMICON|TVHT_ONITEMLABEL))){
        TVITEMW info{};info.mask=TVIF_PARAM;info.hItem=item;TreeView_GetItem(h,&info);auto*node=reinterpret_cast<mm::FolderNode*>(info.lParam);
        if(node&&node->directory){
          if(m==WM_LBUTTONDOWN){a->EndRootDrag();SetFocus(h);TreeView_SelectItem(h,item);
            if(!TreeView_GetParent(h,item)){auto*folder=a->FolderForItem(item);if(folder){SetCapture(h);if(GetCapture()==h){a->rootDragId=folder->id;a->rootDragStart={GET_X_LPARAM(l),GET_Y_LPARAM(l)};}return 0;}}
            TreeView_Expand(h,item,TVE_TOGGLE);a->UpdateFolderItem(item,true);}
          return 0;
        }
      }
    }
    return DefSubclassProc(h,m,w,l);
 }
 static LRESULT CALLBACK TabProc(HWND h,UINT m,WPARAM w,LPARAM l,UINT_PTR,DWORD_PTR data){auto*a=reinterpret_cast<App*>(data);POINT p{GET_X_LPARAM(l),GET_Y_LPARAM(l)};TCHITTESTINFO hit{p,0};
    if(m==WM_RBUTTONDOWN){a->tabDrag=-1;return 0;}
    if(m==WM_RBUTTONUP||m==WM_CONTEXTMENU){
      a->tabDrag=-1;POINT screen=p;
      if(m==WM_CONTEXTMENU&&p.x==-1&&p.y==-1){
        RECT row{};if(a->active<0||!TabCtrl_GetItemRect(h,a->active,&row))return 0;
        screen={row.left+2,row.bottom};ClientToScreen(h,&screen);
      }else{
        if(m==WM_CONTEXTMENU){ScreenToClient(h,&p);hit.pt=p;}
        if(TabCtrl_HitTest(h,&hit)<0)return 0;
        if(m==WM_RBUTTONUP)ClientToScreen(h,&screen);
      }
      a->ShowTabMenu(screen);return 0;
    }
    if(m==WM_MOUSEWHEEL){MSG msg{};msg.hwnd=h;msg.message=m;msg.wParam=w;msg.lParam=l;if(a->TabWheel(msg))return 0;}
    if(m==WM_ERASEBKGND)return 1;
    if(m==WM_PAINT||m==WM_PRINTCLIENT){PAINTSTRUCT paint{};HDC dc=m==WM_PRINTCLIENT?HDC(w):BeginPaint(h,&paint);RECT client;GetClientRect(h,&client);FillRect(dc,&client,a->panel);const int count=TabCtrl_GetItemCount(h);for(int i=0;i<count;i++){DRAWITEMSTRUCT item{};item.CtlType=ODT_TAB;item.itemID=i;item.hDC=dc;item.hwndItem=h;if(!TabCtrl_GetItemRect(h,i,&item.rcItem)||item.rcItem.right<=client.left||item.rcItem.left>=client.right)continue;a->DrawItem(&item);}if(m==WM_PAINT)EndPaint(h,&paint);return 0;}
    if(m==WM_LBUTTONDOWN){int index=TabCtrl_HitTest(h,&hit);if(index>=0){RECT r;TabCtrl_GetItemRect(h,index,&r);if(p.x>=r.right-a->D(27)){a->Close(index);return 0;}a->tabDrag=index;a->dragStart=p;}}
    if(m==WM_MBUTTONUP){a->Close(TabCtrl_HitTest(h,&hit));return 0;}
    if(m==WM_MOUSEMOVE&&(w&MK_LBUTTON)&&a->tabDrag>=0&&abs(p.x-a->dragStart.x)>a->D(8)){int to=TabCtrl_HitTest(h,&hit);if(to>=0&&to!=a->tabDrag){a->Capture();auto t=a->tabs[a->tabDrag];auto activeId=a->active>=0?a->tabs[a->active].id:0;a->tabs.erase(a->tabs.begin()+a->tabDrag);a->tabs.insert(a->tabs.begin()+to,t);for(size_t i=0;i<a->tabs.size();i++)if(a->tabs[i].id==activeId)a->active=int(i);a->tabDrag=to;a->dragStart=p;a->RebuildTabs();a->Dirty();}}
    if(m==WM_LBUTTONUP)a->tabDrag=-1;
    const auto result=DefSubclassProc(h,m,w,l);
    if(m==WM_SIZE||m==WM_PARENTNOTIFY||m==TCM_INSERTITEMW||m==TCM_SETITEMSIZE)a->EnsureTabScroller();
    if(m==WM_HSCROLL)InvalidateRect(h,nullptr,FALSE);
    return result;}
 void Paint(HDC external=nullptr){PAINTSTRUCT ps{};HDC dc=external?external:BeginPaint(hwnd,&ps);RECT r;GetClientRect(hwnd,&r);FillRect(dc,&r,background);RECT toolbar{0,0,r.right,D(42)};FillRect(dc,&toolbar,panel);
    SetBkMode(dc,TRANSPARENT);SelectObject(dc,uiFont);SetTextColor(dc,Muted());
    if(sidebar){int side=SidebarPixels(r.right);RECT sr{0,D(85),side,r.bottom-D(29)};FillRect(dc,&sr,panel);}
    RECT status{0,r.bottom-D(29),r.right,r.bottom};FillRect(dc,&status,panel);status.left=D(12);status.right-=D(235);std::wstring text=notice.empty()?(active>=0?tabs[active].path:L"準備完了"):notice;
    if(active>=0&&!tabs[active].registrationIssue.empty())text=tabs[active].registrationIssue+L" · "+tabs[active].path;else for(const auto& folder:folders)if(!folder->issue.empty()){text=folder->issue+L" · "+folder->path;break;}
    if(saveRetryPending)text=L"状態を保存できません。10秒間隔で再試行します";
    if(watchLimited&&HasFolders())text=L"一部の自動更新: 最大30秒間隔  ·  "+text;DrawTextW(dc,text.c_str(),-1,&status,DT_SINGLELINE|DT_VCENTER|DT_PATH_ELLIPSIS);
    status.left=r.right-D(225);status.right=r.right-D(12);text=L"拡大率: "+std::to_wstring(MulDiv(view.FontSize(),100,16))+L"%";
    if(active>=0&&!tabs[active].encoding.empty())text=tabs[active].encoding+L"    "+text;
    SetTextColor(dc,Text());DrawTextW(dc,text.c_str(),-1,&status,DT_SINGLELINE|DT_VCENTER|DT_RIGHT);if(!external)EndPaint(hwnd,&ps);
 }
 void DrawItem(DRAWITEMSTRUCT*d){HDC dc=d->hDC;RECT r=d->rcItem;bool selected=d->CtlType==ODT_TAB?int(d->itemID)==active:(d->itemState&ODS_SELECTED)!=0;HBRUSH b=CreateSolidBrush(selected?Bg():Panel());FillRect(dc,&r,b);DeleteObject(b);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,selected?Accent():Text());SelectObject(dc,uiFont);
    std::wstring label;if(d->CtlType==ODT_TAB){if(d->itemID<tabs.size())label=TabTitle(d->itemID);r.left+=D(12);r.right-=D(28);DrawTextW(dc,label.c_str(),-1,&r,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS);RECT close=d->rcItem;close.left=close.right-D(27);DrawTextW(dc,L"×",1,&close,DT_SINGLELINE|DT_VCENTER|DT_CENTER);if(selected){RECT line=d->rcItem;line.bottom=line.top+D(3);b=CreateSolidBrush(Accent());FillRect(dc,&line,b);DeleteObject(b);}}
    else{wchar_t text[128];GetWindowTextW(d->hwndItem,text,128);label=text;DrawTextW(dc,label.c_str(),-1,&r,DT_SINGLELINE|DT_VCENTER|DT_CENTER);if(d->itemState&ODS_FOCUS){InflateRect(&r,-2,-2);DrawFocusRect(dc,&r);}}
 }
 LRESULT DrawTreeItem(NMTVCUSTOMDRAW* draw){
    if(draw->nmcd.dwDrawStage==CDDS_PREPAINT)return CDRF_NOTIFYITEMDRAW;
    if(draw->nmcd.dwDrawStage!=CDDS_ITEMPREPAINT)return CDRF_DODEFAULT;
    const bool selected=TreeView_GetSelection(treeH)==HTREEITEM(draw->nmcd.dwItemSpec);
    const bool focused=GetFocus()==treeH;
    draw->clrText=Text();
    draw->clrTextBk=selected?(dark?(focused?RGB(43,76,119):RGB(46,56,72)):(focused?RGB(211,229,253):RGB(226,232,240))):Panel();
    // Override only paint state; keep the native selection for keys and accessibility.
    // Otherwise common controls replace our colors with the system selection palette.
    draw->nmcd.uItemState&=~CDIS_SELECTED;
    return CDRF_DODEFAULT;
 }
 LRESULT Message(UINT m,WPARAM w,LPARAM l){switch(m){case WM_CREATE:CreateControls();return 0;case WM_SIZE:Layout();return 0;case WM_ERASEBKGND:return 1;case WM_PAINT:Paint();return 0;case WM_PRINTCLIENT:Paint(HDC(w));return 0;
    case WM_GETMINMAXINFO:{auto*p=reinterpret_cast<MINMAXINFO*>(l);p->ptMinTrackSize={D(640),D(480)};return 0;}
    case WM_DPICHANGED:{dpi=HIWORD(w);if(uiFont)DeleteObject(uiFont);uiFont=CreateFontW(-D(13),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Yu Gothic UI");for(auto[id,b]:buttons)SendMessageW(b,WM_SETFONT,WPARAM(uiFont),TRUE);SendMessageW(treeH,WM_SETFONT,WPARAM(uiFont),TRUE);SendMessageW(tabsH,WM_SETFONT,WPARAM(uiFont),TRUE);SendMessageW(searchH,WM_SETFONT,WPARAM(uiFont),TRUE);SendMessageW(countH,WM_SETFONT,WPARAM(uiFont),TRUE);TabCtrl_SetItemSize(tabsH,D(170),D(32));TreeView_SetIndent(treeH,D(18));TreeView_SetItemHeight(treeH,D(27));RebuildFolderImages();UpdateAppIcons();auto*r=reinterpret_cast<RECT*>(l);SetWindowPos(hwnd,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE);return 0;}
    case WM_DRAWITEM:DrawItem(reinterpret_cast<DRAWITEMSTRUCT*>(l));return TRUE;
    case WM_CTLCOLORSTATIC:case WM_CTLCOLOREDIT:SetTextColor(HDC(w),Text());SetBkColor(HDC(w),Bg());return LRESULT(editBrush);
    case WM_COMMAND:if(LOWORD(w)==203&&HIWORD(w)==EN_CHANGE)DoSearch(0);else Command(LOWORD(w));return 0;
    case WM_NOTIFY:{auto*n=reinterpret_cast<NMHDR*>(l);if(n->hwndFrom==tabsH&&n->code==TCN_SELCHANGE)Switch(TabCtrl_GetCurSel(tabsH));
      if(n->hwndFrom==treeH){if(n->code==NM_CUSTOMDRAW)return DrawTreeItem(reinterpret_cast<NMTVCUSTOMDRAW*>(l));
      if(n->code==TVN_SELCHANGEDW&&!treeUpdating){auto*t=reinterpret_cast<NMTREEVIEWW*>(l);auto*node=reinterpret_cast<mm::FolderNode*>(t->itemNew.lParam);if(node&&!node->directory)OpenPaths({node->path});}
      if(n->code==TVN_ITEMEXPANDEDW){auto*t=reinterpret_cast<NMTREEVIEWW*>(l);UpdateFolderItem(t->itemNew.hItem,!treeUpdating);}
      // A row's node stays allocated until the row is gone (folder tree section), so its path can be read here.
      if(n->code==TVN_DELETEITEMW){auto*t=reinterpret_cast<NMTREEVIEWW*>(l);auto*node=reinterpret_cast<mm::FolderNode*>(t->itemOld.lParam);
        if(node&&!node->directory){auto found=rowsByPath.find(node->path);if(found!=rowsByPath.end()&&found->second==t->itemOld.hItem)rowsByPath.erase(found);}}
      }
      return 0;}
    case WM_ENTERSIZEMOVE:view.SetLiveResize(true);return 0;
    case WM_EXITSIZEMOVE:view.SetLiveResize(false);return 0;
    case WM_MOUSEWHEEL:if(GET_KEYSTATE_WPARAM(w)&MK_CONTROL){Zoom(GET_WHEEL_DELTA_WPARAM(w)>0?1:-1);return 0;}break;
    case WM_TIMER:if(w==1)PollWatches();else if(w==2){KillTimer(hwnd,2);CheckActive();if(treeDirty){treeDirty=false;SetupWatches();}}else if(w==3)AutoSave();else if(w==4&&watchLimited){CheckActive();if(HasFolders())RefreshTree();}else if(w==5&&watchRetryPending&&!watchSetupPending)SetupWatches();else if(w==TreeTaskTimer)RunTreeTasks(true);return 0;
    case WM_ACTIVATE:if(LOWORD(w)!=WA_INACTIVE)CheckActive();return 0;
    case LoadDone:AcceptLoad(std::unique_ptr<Loaded>(reinterpret_cast<Loaded*>(l)));return 0;
    case TreeDone:AcceptTree(std::unique_ptr<Scanned>(reinterpret_cast<Scanned*>(l)));return 0;
    case WatchDone:AcceptWatches(std::unique_ptr<Watched>(reinterpret_cast<Watched*>(l)));return 0;
    case StartupDone:AcceptStartupCheck(std::unique_ptr<StartupChecked>(reinterpret_cast<StartupChecked*>(l)));return 0;
    case WM_DROPFILES:{HDROP drop=HDROP(w);UINT n=DragQueryFileW(drop,0xffffffff,nullptr,0);std::vector<std::wstring>paths;for(UINT i=0;i<n;i++){UINT len=DragQueryFileW(drop,i,nullptr,0);std::wstring path(len+1,L'\0');DragQueryFileW(drop,i,path.data(),len+1);path.resize(len);paths.push_back(path);}DragFinish(drop);OpenPaths(paths);return 0;}
    case WM_COPYDATA:{if(closing)return FALSE;auto*c=reinterpret_cast<COPYDATASTRUCT*>(l);if(!c||c->dwData!=0x4d4d||c->cbData>1024*1024||c->cbData%2||!c->lpData)return FALSE;const wchar_t*p=static_cast<const wchar_t*>(c->lpData);size_t n=c->cbData/2;std::vector<std::wstring>paths;size_t start=0;for(size_t i=0;i<n;i++)if(p[i]==0){if(i>start)paths.emplace_back(p+start,i-start);start=i+1;}OpenPaths(paths,true);ShowWindow(hwnd,IsIconic(hwnd)?SW_RESTORE:SW_SHOW);SetForegroundWindow(hwnd);return TRUE;}
    case WM_CLOSE:if(!ConfirmClose())return 0;closing=true;RemovePropW(hwnd,mm::InstanceReadyProperty);SetPropW(hwnd,mm::InstanceClosingProperty,HANDLE(1));KillTimer(hwnd,TreeTaskTimer);treeTasks.clear();CancelStartupCheck();if(startupThread.joinable())startupThread.join();CancelFolderScans();if(readCancel)*readCancel=true;CancelWatchSetup();RetireWatches(std::move(watches));readWorker.Stop();for(auto& worker:scanners)worker.Stop();watchWorker.Stop();{MSG msg;while(PeekMessageW(&msg,hwnd,LoadDone,StartupDone,PM_REMOVE)){if(msg.message==LoadDone)delete reinterpret_cast<Loaded*>(msg.lParam);else if(msg.message==TreeDone)delete reinterpret_cast<Scanned*>(msg.lParam);else if(msg.message==WatchDone)delete reinterpret_cast<Watched*>(msg.lParam);else delete reinterpret_cast<StartupChecked*>(msg.lParam);}}
      // The product hides the window and lets process exit reclaim it: destroying
      // thousands of tree rows one by one would keep a closed window alive for
      // seconds. Tests destroy the window so their process can continue.
      // Detach this object first: later messages to the surviving window (for
      // example WM_PARENTNOTIFY while ~App destroys the document view) must not
      // reach a destroyed App.
      if(fastExit){ShowWindow(hwnd,SW_HIDE);SetWindowLongPtrW(hwnd,GWLP_USERDATA,0);PostQuitMessage(0);}else DestroyWindow(hwnd);return 0;
    case WM_DESTROY:ReleaseAppIcons();if(folderImages){if(IsWindow(treeH))TreeView_SetImageList(treeH,nullptr,TVSIL_NORMAL);ImageList_Destroy(folderImages);folderImages=nullptr;}if(uiFont)DeleteObject(uiFont);if(background)DeleteObject(background);if(panel)DeleteObject(panel);if(editBrush)DeleteObject(editBrush);PostQuitMessage(0);return 0;
 }return DefWindowProcW(hwnd,m,w,l);}
};

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int show){
 SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
 int argc=0;auto argv=CommandLineToArgvW(GetCommandLineW(),&argc);std::vector<std::wstring>paths;for(int i=1;i<argc;i++)paths.push_back(argv[i]);LocalFree(argv);
 std::wstring payload;for(const auto&path:paths){payload+=mm::NormalizePath(path);payload+=L'\0';}payload+=L'\0';
 mm::SingleInstance singleInstance;
 const auto launch=singleInstance.Route(WindowClass,payload);
 if(launch==mm::InstanceLaunch::Forwarded)return 0;
 if(launch==mm::InstanceLaunch::Failed){MessageBoxW(nullptr,L"MMViewerの起動準備またはファイルの受け渡しを確認できませんでした。\n起動済みのMMViewerの状態を確認して、もう一度開いてください。",L"MMViewer",MB_OK|MB_ICONERROR);return 1;}
 CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);Gdiplus::GdiplusStartupInput input;ULONG_PTR token=0;const bool gdiplusStarted=Gdiplus::GdiplusStartup(&token,&input,nullptr)==Gdiplus::Ok;
 INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_TREEVIEW_CLASSES|ICC_TAB_CLASSES|ICC_STANDARD_CLASSES};InitCommonControlsEx(&controls);
 int exitCode=0;{App app;app.fastExit=true;try{app.Restore();}catch(...){ }WNDCLASSEXW wc{sizeof(wc)};wc.lpfnWndProc=App::Proc;wc.hInstance=instance;wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.hIcon=LoadIconW(instance,MAKEINTRESOURCEW(IDI_MMVIEWER));if(!wc.hIcon)wc.hIcon=LoadIconW(nullptr,IDI_APPLICATION);wc.lpszClassName=WindowClass;RegisterClassExW(&wc);
 auto h=CreateWindowExW(WS_EX_ACCEPTFILES,WindowClass,L"MMViewer",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,CW_USEDEFAULT,CW_USEDEFAULT,MulDiv(app.winW,GetDpiForSystem(),96),MulDiv(app.winH,GetDpiForSystem(),96),nullptr,nullptr,instance,&app);
 if(h){app.view.EnableOfficialMermaid();ShowWindow(h,show);UpdateWindow(h);app.LoadActive(false);app.SetupWatches(true);if(!paths.empty())app.OpenPaths(paths,true);app.StartStartupCheck();
   if(!SetPropW(h,mm::InstanceReadyProperty,HANDLE(1))){MessageBoxW(h,L"MMViewerの起動状態を登録できませんでした。",L"MMViewer",MB_OK|MB_ICONERROR);DestroyWindow(h);exitCode=1;}
   else{MSG msg{};BOOL received;while((received=GetMessageW(&msg,nullptr,0,0))>0){if(app.TabWheel(msg)||app.Key(msg))continue;TranslateMessage(&msg);DispatchMessageW(&msg);}exitCode=received<0?1:int(msg.wParam);}
 }else exitCode=1;}
 if(gdiplusStarted)Gdiplus::GdiplusShutdown(token);CoUninitialize();return exitCode;
}
