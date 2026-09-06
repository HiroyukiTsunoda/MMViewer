#include "diagram.h"
#include <iostream>
#include <stdexcept>
#include <vector>
#include <filesystem>

static void Check(bool condition,const char* message) { if(!condition)throw std::runtime_error(message); }
static std::shared_ptr<mm::Diagram> Valid(const std::string& source) {
    auto d=mm::ParseDiagram(source);if(!d->valid)std::wcerr<<d->error<<L"\n";
    Check(d->valid,"Expected valid diagram");Check(d->width>0&&d->height>0,"Expected positive dimensions");return d;
}
static unsigned long long RasterHash(const mm::Diagram& d) {
    Gdiplus::Bitmap bitmap((INT)d.width+20,(INT)d.height+20,PixelFormat32bppARGB);
    {Gdiplus::Graphics g(&bitmap);mm::DiagramTheme theme{Gdiplus::Color(255,255,255),Gdiplus::Color(30,40,55),Gdiplus::Color(90,105,125),Gdiplus::Color(237,244,255),Gdiplus::Color(72,107,173)};
    g.Clear(theme.background);mm::DrawDiagram(g,d,10,10,1,theme);Check(g.GetLastStatus()==Gdiplus::Ok,"Raster fixture draw");}
    Gdiplus::BitmapData pixels;Gdiplus::Rect area(0,0,bitmap.GetWidth(),bitmap.GetHeight());
    Check(bitmap.LockBits(&area,Gdiplus::ImageLockModeRead,PixelFormat32bppARGB,&pixels)==Gdiplus::Ok,"Raster lock");
    unsigned long long hash=1469598103934665603ULL;
    for(UINT y=0;y<pixels.Height;++y){const auto* row=static_cast<const unsigned char*>(pixels.Scan0)+(ptrdiff_t)y*pixels.Stride;for(UINT x=0;x<pixels.Width*4;++x){hash^=row[x];hash*=1099511628211ULL;}}
    bitmap.UnlockBits(&pixels);return hash;
}
int main(int argc,char** argv) {
    ULONG_PTR token;Gdiplus::GdiplusStartupInput startup;Gdiplus::GdiplusStartup(&token,&startup,nullptr);
    try {
        auto base=Valid("flowchart TD\nA[\"開始\"] --> B{判定}\nB -->|はい| C([完了])\nB -.->|いいえ| A\n");
        auto repeat=Valid("flowchart TD\nA[\"開始\"] --> B{判定}\nB -->|はい| C([完了])\nB -.->|いいえ| A\n");
        Check(base->width==repeat->width&&base->height==repeat->height,"Layout must be deterministic");
        auto horizontal=Valid("graph LR; A --> B --> C");auto vertical=Valid("graph TB; A --> B --> C");
        Check(horizontal->width>horizontal->height,"LR orientation");Check(vertical->height>vertical->width,"TB orientation");
        Valid("graph RL; A --> B");Valid("graph BT; A --> B");
        Valid("flowchart TD\nA[四角] --- B(角丸)\nB ==> C([スタジアム])\nC --> D{ひし形}\nD --> E((円))\nE --> F(((二重円)))\nF --> G{{六角形}}\nG --> H[[サブルーチン]]\nH --> I[(データベース)]\n");
        Valid("flowchart LR\nsubgraph Input[入力]\nA[\"ファイル;改行<br/>日本語\"] --> B[処理]\nend\nB --> C[出力]\nclassDef selected fill:#e0f2fe,stroke:#0284c7,color:#0c4a6e\nclass A,B selected\nstyle C fill:#fff,stroke:#333,stroke-width:2px\nlinkStyle 0 stroke:#008800\n");
        Valid("graph TD\n%% comment\nA --> A\nA -- 確認 --> B\nB -. 再試行 .-> A\n");
        auto design=Valid("flowchart TD\nUI[\"ネイティブ：タブ・テーマ・操作\"] --> DM[\"DocumentManager\"]\nDM --> IO[\"ファイル読込・更新監視\"]\nDM --> MD[\"Markdown：本文・図・参照を抽出\"]\nMD --> VW[\"ネイティブ：本文表示\"]\nVW --> MM[\"Mermaid：図描画\"]\nVW --> STATE[\"文字サイズ・読書位置\"]\nSTATE --> UI\nIO --> DM");
        auto sequence=Valid("sequenceDiagram\nautonumber\nactor U as 利用者\nparticipant A as アプリ\nparticipant F as ファイル\nU->>+A: Markdownを開く\nA->>+F: 読み込み\nF-->>-A: 本文\nalt Mermaidがある\nA->>A: 図を描画\nelse 本文のみ\nNote over A,F: オフラインで表示\nend\nA-->>-U: 表示完了\n");
        Valid("sequenceDiagram\nparticipant A\nparticipant B\nloop 繰り返し\nopt 必要な場合\nA->>B: 通知\nend\nend\nNote left of A: メモ\nNote right of B: 完了\n");
        Valid("sequenceDiagram\nA->>B: 引数 function( と配列 [ の説明\nB-->>A: 引用符 \" も通常のラベル\n");
        auto percentage=Valid("sequenceDiagram\nA->>B: 50%% complete\n");
        auto truncated=Valid("sequenceDiagram\nA->>B: 50\n");
        Check(RasterHash(*percentage)!=RasterHash(*truncated),"Percent signs inside message labels must not become comments");
        auto combined=Valid("graph TD\nA[Node]:::hot\nclassDef hot stroke:#0284c7,stroke-width:5,stroke-dasharray:5 5\nstyle A fill:#ffeeee\n");
        auto explicitStyle=Valid("graph TD\nA[Node]\nstyle A stroke:#0284c7,stroke-width:5,stroke-dasharray:5 5,fill:#ffeeee\n");
        Check(RasterHash(*combined)==RasterHash(*explicitStyle),"Local fill must preserve class line width and dashes");
        auto classWidth=Valid("graph TD\nA[Node]:::hot\nclassDef hot stroke:#0284c7,stroke-width:5\nstyle A stroke-width:2\n");
        auto explicitWidth=Valid("graph TD\nA[Node]\nstyle A stroke:#0284c7,stroke-width:2\n");
        Check(RasterHash(*classWidth)==RasterHash(*explicitWidth),"Local width must override class width");
        std::string nested="sequenceDiagram\nparticipant A\n";for(int i=0;i<12;++i)nested+="loop\n";nested+="A->>A: 繰り返し\n";for(int i=0;i<12;++i)nested+="end\n";
        auto nestedDiagram=Valid(nested);RasterHash(*nestedDiagram);
        auto shapes=Valid("flowchart LR\nsubgraph Nodes[ノードの形状]\nA[四角] --> B(角丸)\nB --> C([スタジアム])\nend\nC -->|判断する| D{判定}\nD -->|はい| E[(データベース)]\nD -.->|いいえ| F((終了))\nclassDef blue fill:#e0f2fe,stroke:#0284c7,color:#0c4a6e\nclass A,B,C blue\nstyle D fill:#fef3c7,stroke:#d97706,color:#78350f");
        for(const auto& source:std::vector<std::string>{"", "pie\n\"A\":10", "graph TD\nA[broken", "flowchart TD\nA --> B\nclick A callback", "flowchart TD\nsubgraph Foo\nA", "sequenceDiagram\nA->>B: ok\nunknown statement", "sequenceDiagram\ndeactivate A", "sequenceDiagram\nelse invalid", "graph TD\nA-->B\nstyle A opacity:0.5", "%%{init:{}}%%\ngraph TD\nA-->B"}) {
            auto d=mm::ParseDiagram(source);Check(!d->valid&&!d->error.empty(),"Invalid syntax must report an explicit error");
        }
        Check(!mm::ParseDiagram(std::string(50001,'x'))->valid,"Size limit");
        // Large diagrams exercise the shared measurer and the static patterns.
        std::string bigSequence="sequenceDiagram\n";
        for(int i=0;i<1500;++i)bigSequence+="P"+std::to_string(i%40)+"->>P"+std::to_string((i+1)%40)+": メッセージ"+std::to_string(i)+"\n";
        auto sequenceA=Valid(bigSequence),sequenceB=Valid(bigSequence);
        Check(sequenceA->width==sequenceB->width&&sequenceA->height==sequenceB->height,"Large sequence layout must be deterministic");
        std::string bigFlow="graph TD\n";
        for(int i=0;i<300;++i)bigFlow+="N"+std::to_string(i)+"[ノード"+std::to_string(i)+"] --> N"+std::to_string((i+1)%300)+"\n";
        auto flowA=Valid(bigFlow),flowB=Valid(bigFlow);
        Check(flowA->width==flowB->width&&flowA->height==flowB->height,"Large flowchart layout must be deterministic");
        Check(RasterHash(*flowA)==RasterHash(*flowB),"Cached draw fonts must not change the rendering");
        {
            // A document view paints only its client area and the drawing skips
            // elements outside it; every pixel of such a partial view must still
            // match the full rendering.
            const int width=(INT)base->width+20,height=(INT)base->height+20;
            mm::DiagramTheme theme{Gdiplus::Color(255,255,255),Gdiplus::Color(30,40,55),Gdiplus::Color(90,105,125),Gdiplus::Color(237,244,255),Gdiplus::Color(72,107,173)};
            Gdiplus::Bitmap full(width,height,PixelFormat32bppARGB);
            {Gdiplus::Graphics g(&full);g.Clear(theme.background);mm::DrawDiagram(g,*base,10,10,1,theme);Check(g.GetLastStatus()==Gdiplus::Ok,"Full rendering");}
            const int cropX=width/3,cropY=height/3,cropW=std::max(1,std::min(300,width-cropX)),cropH=std::max(1,std::min(240,height-cropY));
            Gdiplus::Bitmap crop(cropW,cropH,PixelFormat32bppARGB);
            {Gdiplus::Graphics g(&crop);g.Clear(theme.background);mm::DrawDiagram(g,*base,10.0f-cropX,10.0f-cropY,1,theme);Check(g.GetLastStatus()==Gdiplus::Ok,"Partial rendering");}
            size_t mismatches=0;
            for(int y=0;y<cropH;++y)for(int x=0;x<cropW;++x){Gdiplus::Color a,b;full.GetPixel(x+cropX,y+cropY,&a);crop.GetPixel(x,y,&b);if(a.GetValue()!=b.GetValue())++mismatches;}
            Check(mismatches==0,"A partial view renders differently from the full diagram");
            Gdiplus::Bitmap tiny(64,48,PixelFormat32bppARGB);
            {Gdiplus::Graphics g(&tiny);g.Clear(theme.background);mm::DrawDiagram(g,*flowA,-3000,-4000,1,theme);Check(g.GetLastStatus()==Gdiplus::Ok,"Mostly off-screen rendering");}
        }
        // Exercise GDI+ paths in both themes and at two document font scales.
        for(const auto& d:{base,sequence})for(float scale:{1.0f,1.5f}) {
            Gdiplus::Bitmap bitmap((INT)(d->width*scale+20),(INT)(d->height*scale+20),PixelFormat32bppARGB);
            Gdiplus::Graphics g(&bitmap);mm::DiagramTheme theme{Gdiplus::Color(255,255,255),Gdiplus::Color(30,40,55),Gdiplus::Color(90,105,125),Gdiplus::Color(237,244,255),Gdiplus::Color(72,107,173)};
            g.Clear(theme.background);mm::DrawDiagram(g,*d,10,10,scale,theme);Check(g.GetLastStatus()==Gdiplus::Ok,"GDI+ rendering");
            theme={Gdiplus::Color(24,28,36),Gdiplus::Color(231,235,244),Gdiplus::Color(150,163,186),Gdiplus::Color(39,51,72),Gdiplus::Color(123,162,227)};
            g.Clear(theme.background);mm::DrawDiagram(g,*d,10,10,scale,theme);Check(g.GetLastStatus()==Gdiplus::Ok,"Dark GDI+ rendering");
        }
        if(argc>1&&std::string(argv[1])=="--snapshots") {
            std::filesystem::path directory=argc>2?std::filesystem::path(argv[2]):std::filesystem::path("artifacts/diagrams");
            std::filesystem::create_directories(directory);
            const CLSID png={0x557cf406,0x1a04,0x11d3,{0x9a,0x73,0x00,0x00,0xf8,0x1e,0xf3,0x2e}};
            std::vector<std::pair<std::string,std::shared_ptr<mm::Diagram>>> fixtures={{"flow",base},{"sequence",sequence},{"architecture",design},{"shapes",shapes}};
            for(const auto&[name,d]:fixtures)for(bool dark:{false,true}) {
                mm::DiagramTheme theme=dark
                    ?mm::DiagramTheme{Gdiplus::Color(24,28,36),Gdiplus::Color(231,235,244),Gdiplus::Color(150,163,186),Gdiplus::Color(39,51,72),Gdiplus::Color(123,162,227)}
                    :mm::DiagramTheme{Gdiplus::Color(255,255,255),Gdiplus::Color(30,40,55),Gdiplus::Color(90,105,125),Gdiplus::Color(237,244,255),Gdiplus::Color(72,107,173)};
                Gdiplus::Bitmap bitmap((INT)d->width+20,(INT)d->height+20,PixelFormat32bppARGB);Gdiplus::Graphics g(&bitmap);
                g.Clear(theme.background);mm::DrawDiagram(g,*d,10,10,1,theme);
                auto path=directory/(name+(dark?"-dark.png":"-light.png"));Check(bitmap.Save(path.c_str(),&png,nullptr)==Gdiplus::Ok,"PNG snapshot save");
            }
        }
        std::cout<<"diagram_tests: all checks passed\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<"\n";Gdiplus::GdiplusShutdown(token);return 1;}
    Gdiplus::GdiplusShutdown(token);return 0;
}
