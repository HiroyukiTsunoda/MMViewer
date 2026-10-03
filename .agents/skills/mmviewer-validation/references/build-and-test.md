# ビルドとテスト

以下のコマンドはリポジトリルートで実行する。現在のビルド設定とテスト登録は `CMakeLists.txt`、配布までの処理は `scripts/build.ps1` が基準。

## ビルドと配布

Visual StudioのC++ Build Tools、同梱CMake / Ninjaを使用する。標準スクリプトは `vswhere.exe` で環境を探し、`vcvars64.bat` を読み込んでReleaseビルドする。

```powershell
powershell -NoProfile -File .\scripts\build.ps1 -Test
```

このコマンドはビルド、CTest全件、`dist/MMViewer.exe` へのコピーまで行う。`-Test` を省くとテストは実行されない。ビルド失敗の詳細は `build-native/build.log` にある。

配布exeが使用中ならコピーが失敗する。配置が依頼に含まれる場合は、対象の既存画面を通常終了してから配置する。ハングしている場合は [起動と障害調査](runtime.md) に従って先に証拠を採取する。配置後は `build-native/MMViewer.exe` と `dist/MMViewer.exe` のSHA-256を比較する。

配布を伴わない変更検証では、構成済みの `build-native` に対する必要なターゲットのビルドとCTestを使える。C++コンパイラが必要なビルドはVisual Studioの開発環境で実行する。例の `cmake` / `ctest` がPATHにない場合は、`scripts/build.ps1` と同じVisual Studio同梱の実行ファイルを使う。

```powershell
cmake --build build-native --target core_tests startup_tests --parallel 4
ctest --test-dir build-native -R '^(core|startup)$' --output-on-failure
```

サンドボックスのプロセス起動・権限エラーと、コンパイル・テストの失敗は区別する。環境の制約は、許可された実行経路で解消してからコードの原因を判断する。

## 変更に応じたテスト

必要な実行ファイルを今回のソースからビルドしてからテストする。対象名は `ctest --test-dir build-native -N` と `CMakeLists.txt` で確認できる。

| 変更対象 | 関連するCTest名の入口 |
| --- | --- |
| 文字コード、パス、走査、起動時の存在確認 | `core`, `startup` |
| mutex、起動・終了、ファイル転送 | `single_instance` と関連する `host` |
| 本文、検索、選択、図・画像、キャッシュ | `viewer`, `diagram`, `image_lifecycle` |
| 公式Mermaid、テーマ、非同期描画 | `mermaid`, `mermaid_viewer` |
| ツリー、タブ、セッション、監視 | `host`, `host_tree_scroll`, `session_bulk`, `encoding_reentry`, `watch_recovery`, `expansion_history` から変更経路に対応するもの |

Mermaidの実描画テストにはWebView2 Runtimeが必要。Runtime不足による失敗を描画成功と報告しない。

`tests/host_tests.cpp` の `Host` は隔離したセッションを使い、実ユーザーの状態を読む `App::Restore` を呼ばない。新しいGUIテストでもこの隔離を保つ。既存のテストとfixtureを使える検証は、実装依頼の範囲で都度の確認を求めず進めてよい。環境が求める権限確認は別途尊重する。

`viewer_benchmark`、`soak_tests`、`soak_heap_tests` は通常ビルドから除外されている。性能比較や長時間耐久が依頼された場合、または関連する不具合の確認に必要な場合に、対象・時間を定めて使う。過去の計測値やテスト件数は今回の結果に流用しない。

## Mermaid資産を変更する場合だけ

通常のビルドは同梱済み資産を使用する。資産の修正・再生成が必要な場合にだけ `scripts/mermaid` で実行する。

```powershell
Push-Location scripts/mermaid
try {
    npm ci --ignore-scripts
    if ($LASTEXITCODE -ne 0) { throw 'npm ci failed' }
    npm run build
    if ($LASTEXITCODE -ne 0) { throw 'Mermaid asset build failed' }
} finally {
    Pop-Location
}
```

依存バージョンは `package-lock.json`、生成処理は `build.mjs` を確認する。生成物と第三者ライセンスの整合を確認し、C++側へ取り込んだうえで関連する実描画テストを実行する。
