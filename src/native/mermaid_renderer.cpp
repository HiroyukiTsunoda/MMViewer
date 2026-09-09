#include "mermaid_renderer.h"
#include "resource.h"
#include <WebView2.h>
#include <wrl.h>
#include <shlwapi.h>
#include <algorithm>
#include <cmath>
#include <deque>
#include <filesystem>
#include <map>
#include <sstream>
#include <vector>

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Callback;
namespace mm {
namespace {
constexpr wchar_t Page[] = L"https://mmviewer.invalid/index.html";
ComPtr<IStream> ResourceStream(int id) {
    auto module=GetModuleHandleW(nullptr);auto resource=FindResourceW(module,MAKEINTRESOURCEW(id),RT_RCDATA);
    if(!resource)return {};
    auto size=SizeofResource(module,resource);auto memory=LoadResource(module,resource);
    ComPtr<IStream> stream;stream.Attach(SHCreateMemStream(static_cast<const BYTE*>(LockResource(memory)),size));return stream;
}
std::wstring WideSource(const std::string& text) {
    int size=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),int(text.size()),nullptr,0);
    if(!size&&!text.empty())return {};
    std::wstring value(size,L'\0');MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),int(text.size()),value.data(),size);return value;
}
}
struct MermaidRenderer::State : std::enable_shared_from_this<State> {
    struct Job {unsigned long long id;std::string source;std::shared_ptr<MermaidImage> image;std::function<void()> changed;};
    HWND host=nullptr;bool starting=false,ready=false,closed=false;std::wstring profile,fatal;
    unsigned long long nextId=1;std::deque<Job> pending;std::unique_ptr<Job> active;
    std::map<std::string,std::weak_ptr<MermaidImage>> cache;
    ComPtr<ICoreWebView2Environment> environment;ComPtr<ICoreWebView2Controller> controller;ComPtr<ICoreWebView2> web;
    explicit State(std::wstring folder):profile(std::move(folder)){}
    static LRESULT CALLBACK Proc(HWND h,UINT m,WPARAM w,LPARAM l) {
        auto* self=reinterpret_cast<State*>(GetWindowLongPtrW(h,GWLP_USERDATA));
        if(m==WM_NCCREATE){self=static_cast<State*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);SetWindowLongPtrW(h,GWLP_USERDATA,LONG_PTR(self));}
        if(m==WM_TIMER&&self){self->Fail(L"Mermaidの描画がタイムアウトしました。MMViewerを再起動してください。");return 0;}
        return DefWindowProcW(h,m,w,l);
    }
    void Shutdown(){closed=true;if(host)KillTimer(host,1);if(controller)controller->Close();web.Reset();controller.Reset();environment.Reset();if(host)DestroyWindow(host);host=nullptr;pending.clear();active.reset();}
    void Fail(const std::wstring& message) {
        if(closed)return;fatal=message;ready=false;if(host)KillTimer(host,1);
        std::deque<Job> failed=std::move(pending);if(active){failed.push_front(std::move(*active));active.reset();}
        for(auto& job:failed){job.image->error=message;job.image->complete=true;if(job.changed)job.changed();}
    }
    void Complete(const std::wstring& error={}) {
        if(!active||closed)return;if(host)KillTimer(host,1);
        auto job=std::move(active);job->image->error=error;job->image->complete=true;
        if(job->changed)job->changed();Next();
    }
    void Start() {
        if(starting||closed)return;starting=true;
        if(profile.empty()){
            wchar_t local[32768]{};DWORD count=GetEnvironmentVariableW(L"LOCALAPPDATA",local,32768);
            if(!count||count>=32768){Fail(L"WebView2の保存先を取得できません。");return;}
            profile=(std::filesystem::path(local)/L"MMViewer"/L"MermaidWebView2").wstring();
        }
        WNDCLASSW type{};type.hInstance=GetModuleHandleW(nullptr);type.lpszClassName=L"MMViewer.MermaidRenderer";type.lpfnWndProc=Proc;
        if(!RegisterClassW(&type)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS){Fail(L"Mermaid描画ウィンドウを作成できません。");return;}
        host=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,type.lpszClassName,L"",WS_POPUP,-30000,-30000,1024,768,nullptr,nullptr,type.hInstance,this);
        if(!host){Fail(L"Mermaid描画ウィンドウを作成できません。");return;}
        ShowWindow(host,SW_SHOWNOACTIVATE);SetTimer(host,1,30000,nullptr);
        auto weak=weak_from_this();
        auto hr=CreateCoreWebView2EnvironmentWithOptions(nullptr,profile.c_str(),nullptr,
          Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>([weak](HRESULT status,ICoreWebView2Environment* env)->HRESULT{
            auto self=weak.lock();if(!self||self->closed)return S_OK;
            if(FAILED(status)||!env){self->Fail(L"Mermaidの表示にはMicrosoft Edge WebView2 Runtimeが必要です。");return S_OK;}
            self->environment=env;
            auto created=env->CreateCoreWebView2Controller(self->host,
              Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>([weak](HRESULT status,ICoreWebView2Controller* control)->HRESULT{
                auto self=weak.lock();if(!self||self->closed){if(control)control->Close();return S_OK;}
                if(FAILED(status)||!control){self->Fail(L"WebView2の描画を開始できません。");return S_OK;}
                self->controller=control;control->get_CoreWebView2(&self->web);if(!self->web){self->Fail(L"WebView2を取得できません。");return S_OK;}
                ComPtr<ICoreWebView2Controller3> dpiControl;
                if(SUCCEEDED(control->QueryInterface(IID_PPV_ARGS(&dpiControl)))){dpiControl->put_ShouldDetectMonitorScaleChanges(FALSE);dpiControl->put_RasterizationScale(1);}
                control->put_ZoomFactor(1);control->put_Bounds({0,0,1024,768});control->put_IsVisible(TRUE);
                self->Configure();return S_OK;
              }).Get());
            if(FAILED(created))self->Fail(L"WebView2の初期化に失敗しました。");return S_OK;
          }).Get());
        if(FAILED(hr))Fail(L"Mermaidの表示にはMicrosoft Edge WebView2 Runtimeが必要です。");
    }
    void Configure() {
        auto weak=weak_from_this();EventRegistrationToken token{};
        ComPtr<ICoreWebView2Settings> settings;web->get_Settings(&settings);
        if(settings){settings->put_AreDefaultContextMenusEnabled(FALSE);settings->put_AreDevToolsEnabled(FALSE);settings->put_IsStatusBarEnabled(FALSE);settings->put_AreDefaultScriptDialogsEnabled(FALSE);settings->put_IsZoomControlEnabled(FALSE);}
        web->AddWebResourceRequestedFilter(L"*",COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
        web->add_WebResourceRequested(Callback<ICoreWebView2WebResourceRequestedEventHandler>([weak](ICoreWebView2*,ICoreWebView2WebResourceRequestedEventArgs* args)->HRESULT{
            auto self=weak.lock();if(!self||self->closed)return S_OK;
            ComPtr<ICoreWebView2WebResourceRequest> request;args->get_Request(&request);LPWSTR uri=nullptr;request->get_Uri(&uri);std::wstring path=uri?uri:L"";CoTaskMemFree(uri);
            int id=path==Page?IDR_MERMAID_HTML:path==L"https://mmviewer.invalid/renderer.js"?IDR_MERMAID_JS:0;
            auto stream=id?ResourceStream(id):ComPtr<IStream>{};ComPtr<ICoreWebView2WebResourceResponse> response;
            const wchar_t* headers=id==IDR_MERMAID_HTML?L"Content-Type: text/html; charset=utf-8\r\nCache-Control: no-store":L"Content-Type: text/javascript; charset=utf-8\r\nCache-Control: no-store";
            self->environment->CreateWebResourceResponse(stream.Get(),stream?200:403,stream?L"OK":L"Blocked",headers,&response);args->put_Response(response.Get());return S_OK;
        }).Get(),&token);
        web->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>([](ICoreWebView2*,ICoreWebView2NavigationStartingEventArgs* args)->HRESULT{
            LPWSTR uri=nullptr;args->get_Uri(&uri);if(!uri||wcscmp(uri,Page)!=0)args->put_Cancel(TRUE);CoTaskMemFree(uri);return S_OK;
        }).Get(),&token);
        web->add_NewWindowRequested(Callback<ICoreWebView2NewWindowRequestedEventHandler>([](ICoreWebView2*,ICoreWebView2NewWindowRequestedEventArgs* args)->HRESULT{args->put_Handled(TRUE);return S_OK;}).Get(),&token);
        web->add_PermissionRequested(Callback<ICoreWebView2PermissionRequestedEventHandler>([](ICoreWebView2*,ICoreWebView2PermissionRequestedEventArgs* args)->HRESULT{args->put_State(COREWEBVIEW2_PERMISSION_STATE_DENY);return S_OK;}).Get(),&token);
        web->add_ProcessFailed(Callback<ICoreWebView2ProcessFailedEventHandler>([weak](ICoreWebView2*,ICoreWebView2ProcessFailedEventArgs*)->HRESULT{if(auto self=weak.lock())self->Fail(L"Mermaid描画プロセスが終了しました。MMViewerを再起動してください。");return S_OK;}).Get(),&token);
        web->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>([weak](ICoreWebView2*,ICoreWebView2WebMessageReceivedEventArgs* args)->HRESULT{
            auto self=weak.lock();if(!self||self->closed)return S_OK;LPWSTR message=nullptr;
            if(SUCCEEDED(args->TryGetWebMessageAsString(&message))&&message){std::wstring text=message;CoTaskMemFree(message);self->Message(text);}return S_OK;
        }).Get(),&token);
        if(FAILED(web->Navigate(Page)))Fail(L"同梱したMermaidエンジンを読み込めません。");
    }
    void Message(const std::wstring& text) {
        if(text==L"ready"){ready=true;KillTimer(host,1);Next();return;}
        if(text.starts_with(L"fatal\n")){Fail(text.substr(6));return;}
        std::wistringstream input(text);std::wstring kind;unsigned long long id=0;input>>kind>>id;
        if(!active||id!=active->id)return;
        if(kind==L"error"){const auto at=text.find(L'\n',text.find(L'\n')+1);Complete(at==std::wstring::npos?L"Mermaidの構文を確認してください。":text.substr(at+1));return;}
        if(kind==L"size"){
            double width=0,height=0;input>>width>>height;
            if(!std::isfinite(width)||!std::isfinite(height)||width<=0||height<=0||width>100000||height>100000){Complete(L"図のサイズが上限を超えています。");return;}
            active->image->width=float(width);active->image->height=float(height);
            double scale=std::min({2.0,8192.0/width,8192.0/height,std::sqrt(16000000.0/(width*height))});
            LONG w=std::max(1L,LONG(std::floor(width*scale))),h=std::max(1L,LONG(std::floor(height*scale)));
            SetWindowPos(host,nullptr,-30000,-30000,w,h,SWP_NOACTIVATE|SWP_NOZORDER);controller->put_Bounds({0,0,w,h});
            auto request=L"capture\n"+std::to_wstring(id)+L"\n"+std::to_wstring(w)+L"\n"+std::to_wstring(h);
            if(FAILED(web->PostWebMessageAsString(request.c_str())))Complete(L"描画サイズを設定できません。");return;
        }
        if(kind!=L"capture")return;
        ComPtr<IStream> stream;if(FAILED(CreateStreamOnHGlobal(nullptr,TRUE,&stream))){Complete(L"図の画像メモリを確保できません。");return;}
        auto weak=weak_from_this();
        auto hr=web->CapturePreview(COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG,stream.Get(),
          Callback<ICoreWebView2CapturePreviewCompletedHandler>([weak,id,stream](HRESULT status)->HRESULT{
            auto self=weak.lock();if(!self||self->closed||!self->active||self->active->id!=id)return S_OK;
            if(FAILED(status)){self->Complete(L"Mermaidの画像を取得できません。");return S_OK;}
            LARGE_INTEGER zero{};stream->Seek(zero,STREAM_SEEK_SET,nullptr);Gdiplus::Bitmap decoded(stream.Get());
            if(decoded.GetLastStatus()!=Gdiplus::Ok){self->Complete(L"Mermaidの画像を読み込めません。");return S_OK;}
            auto bitmap=std::make_shared<Gdiplus::Bitmap>(decoded.GetWidth(),decoded.GetHeight(),PixelFormat32bppPARGB);
            {Gdiplus::Graphics graphics(bitmap.get());graphics.DrawImage(&decoded,0,0);}
            self->active->image->bitmap=std::move(bitmap);self->Complete();return S_OK;
          }).Get());
        if(FAILED(hr))Complete(L"Mermaidの画像取得を開始できません。");
    }
    void Next() {
        if(!ready||active||closed||!fatal.empty())return;
        while(!pending.empty()){
            auto job=std::move(pending.front());pending.pop_front();if(job.image.use_count()==1)continue;
            active=std::make_unique<Job>(std::move(job));break;
        }
        if(!active)return;
        auto source=WideSource(active->source);
        if(source.empty()&&!active->source.empty()){Complete(L"Mermaidの文字コードが不正です。");return;}
        auto message=L"render\n"+std::to_wstring(active->id)+L"\n"+(active->image->dark?L"dark":L"default")+L"\n"+source;
        SetTimer(host,1,30000,nullptr);
        if(FAILED(web->PostWebMessageAsString(message.c_str())))Complete(L"Mermaidのソースを渡せません。");
    }
};
MermaidRenderer::MermaidRenderer(std::wstring profile):state_(std::make_shared<State>(std::move(profile))){}
MermaidRenderer::~MermaidRenderer(){state_->Shutdown();}
std::shared_ptr<MermaidImage> MermaidRenderer::Render(const std::string& source,bool dark,std::function<void()> changed){
    auto key=(dark?"dark\n":"light\n")+source;
    if(auto found=state_->cache.find(key);found!=state_->cache.end())if(auto image=found->second.lock())return image;
    if(state_->cache.size()>256)std::erase_if(state_->cache,[](const auto& item){return item.second.expired();});
    auto image=std::make_shared<MermaidImage>();image->dark=dark;
    if(!state_->fatal.empty()||source.size()>1000000){image->complete=true;image->error=state_->fatal.empty()?L"Mermaidソースが上限の1 MBを超えています。":state_->fatal;return image;}
    state_->cache[key]=image;state_->pending.push_back({state_->nextId++,source,image,std::move(changed)});state_->Start();state_->Next();return image;
}
}
