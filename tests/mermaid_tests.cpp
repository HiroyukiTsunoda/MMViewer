#include "mermaid_renderer.h"
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <vector>
namespace fs=std::filesystem;
void Check(bool value,const char* text){if(!value)throw std::runtime_error(text);}
void Wait(const std::shared_ptr<mm::MermaidImage>& image){
    const auto end=GetTickCount64()+45000;
    while(!image->complete&&GetTickCount64()<end){MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}MsgWaitForMultipleObjectsEx(0,nullptr,10,QS_ALLINPUT,MWMO_INPUTAVAILABLE);}
    Check(image->complete,"Mermaid rendering did not finish");
}
int main(){
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    Gdiplus::GdiplusStartupInput startup;ULONG_PTR token=0;Gdiplus::GdiplusStartup(&token,&startup,nullptr);
    int result=0;try{
        auto artifacts=fs::absolute(fs::path(__FILE__)).parent_path().parent_path()/L"artifacts"/L"official-mermaid";
        fs::create_directories(artifacts);
        mm::MermaidRenderer renderer((artifacts/(L"profile-"+std::to_wstring(GetCurrentProcessId()))).wstring());
        std::vector<std::pair<std::string,std::string>> samples={
          {"frame-subgraphs",R"(flowchart LR
  subgraph frameN["Frame N"]
    direction LR
    writeN["値を設定する Updater"] -->|アクセサで書き込む| dataN["T 型の FrameData"]
    dataN -->|アクセサで読み取る| readN["値を利用する Updater"]
  end
  subgraph frameNext["Frame N+1"]
    direction LR
    writeNext["値を設定する Updater"] -->|アクセサで書き込む| dataNext["T 型の FrameData"]
    dataNext -->|アクセサで読み取る| readNext["値を利用する Updater"]
  end)"},
          {"mixed-direction","flowchart TB\nsubgraph one[One]\ndirection LR\nA-->B\nend\nsubgraph two[Two]\ndirection RL\nC-->D\nend"},
          {"sequence","sequenceDiagram\nparticipant A as 利用者\nparticipant B as サーバー\nrect rgb(220,240,255)\nA->>+B: Request\nB-->>-A: Response\nend"},
          {"class","classDiagram\nAnimal <|-- Duck\nAnimal : +int age"},
          {"state","stateDiagram-v2\n[*] --> Idle\nIdle --> Running\nRunning --> [*]"},
          {"er","erDiagram\nCUSTOMER ||--o{ ORDER : places"},
          {"gantt","gantt\ntitle Schedule\ndateFormat YYYY-MM-DD\nsection Work\nBuild :a, 2026-01-01, 5d"},
          {"pie","pie title Share\n\"A\" : 40\n\"B\" : 60"},
          {"journey","journey\ntitle Journey\nsection Work\nBuild: 5: Me"},
          {"mindmap","mindmap\n  root((Root))\n    One\n    Two"},
          {"timeline","timeline\ntitle History\n2025 : Start\n2026 : Release"},
          {"git","gitGraph\ncommit\nbranch develop\ncheckout develop\ncommit"},
          {"requirement","requirementDiagram\nrequirement viewer_req {\nid: 1\ntext: Render diagrams\nrisk: low\nverifymethod: test\n}"},
          {"quadrant","quadrantChart\ntitle Priorities\nx-axis Low --> High\ny-axis Low --> High\nTask: [0.3, 0.6]"},
          {"c4","C4Context\nPerson(user, \"User\", \"Reader\")\nSystem(app, \"App\", \"Viewer\")\nRel(user, app, \"Uses\")"},
          {"sankey","sankey-beta\n\nA,B,10\nB,C,5"},
          {"xy","xychart-beta\nx-axis [a,b,c]\ny-axis \"Count\" 0 --> 10\nbar [2,5,8]"},
          {"block","block-beta\ncolumns 2\nA B\nA-->B"},
          {"packet","packet-beta\n0-7: \"Header\"\n8-15: \"Data\""},
          {"kanban","kanban\n  todo[Todo]\n    one[One]\n  done[Done]\n    two[Two]"},
          {"architecture","architecture-beta\ngroup api(cloud)[API]\nservice db(database)[Database] in api\nservice server(server)[Server] in api\ndb:R -- L:server"},
          {"radar","radar-beta\naxis a, b, c\ncurve x{3,4,5}"},
          {"treemap","treemap-beta\n\"Group\"\n  \"One\": 12\n  \"Two\": 20"},
          {"treeview","treeView-beta\n\"src\"\n  \"main.cpp\"\n  \"viewer.cpp\""},
          {"swimlanes","swimlane-beta LR\nsubgraph Reader\nA[Open]\nend\nsubgraph Viewer\nB[Render]\nend\nA-->B"},
          {"eventmodeling","eventmodeling\ntf 01 ui Viewer\ntf 02 cmd OpenFile\ntf 03 evt FileOpened"},
          {"venn","venn-beta\nset A\nset B\nunion A,B[\"Shared\"]"},
          {"ishikawa","ishikawa-beta\n  Failure\n  Input\n    Invalid syntax\n  Runtime\n    Missing dependency"},
          {"wardley","wardley-beta\nanchor Reader [0.9, 0.8]\ncomponent Viewer [0.6, 0.7]\nReader -> Viewer"},
          {"cynefin","cynefin-beta\ncomplex\n\"Explore\"\nclear\n\"Known fix\""},
          {"railroad","railroad-beta\nbit = choice(terminal(\"0\"), terminal(\"1\"));"},
          {"railroad-ebnf","railroad-ebnf-beta\nbit = \"0\" | \"1\" ;"},
          {"railroad-abnf","railroad-abnf-beta\nbit = \"0\" / \"1\" ;"},
          {"railroad-peg","railroad-peg-beta\nbit <- \"0\" / \"1\" ;"},
          {"elk","---\nconfig:\n  layout: elk\n---\nflowchart LR\nA-->B\nA-->C"},
          {"zenuml","zenuml\nClient->Server: hello"},
          {"math","flowchart LR\nA[\"$$x^2 + y^2$$\"] --> B[\"結果\"]"}
        };
        CLSID png{};CLSIDFromString(L"{557cf406-1a04-11d3-9a73-0000f81ef32e}",&png);
        for(auto& [name,source]:samples){
            auto image=renderer.Render(source,false,{});Wait(image);
            if(!image->error.empty()){std::wcerr<<L"ERROR: "<<image->error<<L"\n";throw std::runtime_error(name+" failed");}
            Check(image->bitmap&&image->bitmap->GetWidth()>0&&image->bitmap->GetHeight()>0,"No rendered image");
            Gdiplus::Color base;image->bitmap->GetPixel(0,0,&base);size_t different=0;
            for(UINT y=0;y<image->bitmap->GetHeight();y+=3)for(UINT x=0;x<image->bitmap->GetWidth();x+=3){Gdiplus::Color pixel;image->bitmap->GetPixel(x,y,&pixel);if(pixel.GetValue()!=base.GetValue())++different;}
            Check(different>20,"Mermaid capture was blank");
            Check(image->bitmap->Save((artifacts/(name+".png")).c_str(),&png)==Gdiplus::Ok,"Cannot save diagram snapshot");
            std::cout<<"PASS "<<name<<" "<<image->width<<"x"<<image->height<<std::endl;
        }
        auto dark=renderer.Render(samples[0].second,true,{});Wait(dark);Check(dark->bitmap&&dark->error.empty(),"Dark theme failed");
        dark->bitmap->Save((artifacts/L"frame-dark.png").c_str(),&png);
        auto invalid=renderer.Render("lowchart LR\nA --> B",false,{});Wait(invalid);Check(!invalid->error.empty()&&!invalid->bitmap,"Invalid syntax was silently accepted");
        auto recovery=renderer.Render("flowchart LR\nAfter-->Error",false,{});Wait(recovery);Check(recovery->bitmap&&recovery->error.empty(),"Rendering did not recover after a syntax error");
        std::cout<<"PASS all official Mermaid families, dark theme, syntax errors and recovery\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<std::endl;result=1;}
    Gdiplus::GdiplusShutdown(token);CoUninitialize();return result;
}
