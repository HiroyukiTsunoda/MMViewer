# 起動と障害調査

## 通常デスクトップで開く

GUI起動は共通ユーザールールに従い、ユーザーが操作する `Winsta0\Default` に表示する。Windowsの `exec_command` を使う場合は初回から `sandbox_permissions: "require_escalated"` を指定する。GUIアプリ本体を隠さず、補助コンソールを表示しない。

ユーザー向けMarkdownは、別の表示方法を指定されていなければ次の方法で開く。パスは絶対パスの1引数として渡す。

```powershell
Start-Process -FilePath 'C:\git\MMviewer\dist\MMViewer.exe' -ArgumentList '"C:\path with spaces\資料.md"'
```

既存インスタンスがあればファイルが転送される。起動後は対象ウィンドウが通常デスクトップ上で可視・画面内・応答ありであることと、対象ファイルが実画面に開かれたことを確認する。送信側プロセスの正常終了だけではファイル転送の成功を判断しない。

単一起動や引数処理を変更した場合は、既存画面でファイルが開き、必要な親フォルダが登録され、ツリーの選択位置が開いたファイルに合うことを確認する。

## 画面消失・ハング

現象が出ているプロセスを終了する前に、リポジトリルートで状態を採取する。

```powershell
powershell -NoProfile -File .\scripts\collect-hang.ps1 -NoDump
```

PIDを限定する場合は `-ProcessId` を指定する。スタック調査が必要なら `-NoDump` を外してミニダンプも採取する。結果は `artifacts/hang-*/capture.json` とプロセス別JSONに保存される。スクリプトはアプリを終了・再起動・移動しない。

JSONの `TargetDesktop`、`Enumerated`、`Error` と、列挙されたウィンドウの `Visible`、`OnMonitor`、`Responding`、`Ready`、`Closing` を確認する。列挙失敗や権限不足でウィンドウが見えない場合は、「ウィンドウなし」と断定せず、許可された通常デスクトップ側で確認する。

`CodexSandboxDesktop-*` の残存インスタンスがmutexを保持している場合は、通常デスクトップの新しい起動から見つけられないことがある。プロセスとデスクトップを特定し、復旧操作の対象を絞る。実ユーザーのセッションや登録フォルダを消して初期化する手段は、通常の起動調査に含めない。

再起動で現象が消えた場合は復旧として報告する。原因修正を報告するには、原因を裏付ける状態・コードと、元の発生条件に対応する検証が必要。
