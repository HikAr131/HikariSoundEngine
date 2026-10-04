<!-- SPDX-License-Identifier: AGPL-3.0-or-later -->
# 本機對應來源封存

`node tools/archive-source.mjs --list` 顯示確切檔案清單；`node tools/archive-source.mjs` 產生 `dist/HikariSoundEngine-source.zip`。正式來源封存要求 helper 已提交且工作區乾淨、上游內容維持釘選提交；本機尚未提交的開發快照可加 `--draft`，其 manifest 明確記錄 draft 狀態，不得當成已發布的來源。

工具只建立本機 ZIP，不建 GitHub 倉庫、不推送、不建立 tag 或 Release。`SOURCE.txt` 的公開狀態與來源取得說明隨原始檔案一起封存；公開來源與二進位散布仍需另外完成授權的發布程序。

## 明確來源範圍

- helper 的根建置檔案、manifest、AGPL／MS-LPL 授權與來源聲明，以及 `src/`、`tests/`、`patches/`、`tools/`、`docs/`。測試向量及實際套用的 patch 都在封存內。
- `upstream/fxsound-app` 在 `d8e7a23d37ed5939c2a3090a1c1756c7f2500b17` 的完整 `git archive` 內容，包含上游原本追蹤的二進位與三個未修改的已簽章驅動檔案；不把 upstream 裁成只有 DSP／audio 子目錄。
- 本機 `.scratch/fxsound-driver` 存在時，另將 `c78fc6d031d16bd0a5dbbdff4871cfb8715d343d` 的完整來源封入 `upstream/fxsound-driver-source/`。這是從明確釘選提交匯出的公開上游來源，`.scratch` 目錄本身不入包；原簽章驅動與該參考來源的可重現關係仍為 **NOT VERIFIED**。
- app 的 `Resources` nested gitlink 只用於官方 GUI，未參與 helper 的建置。它的路徑與 commit 記錄在 manifest；未初始化的 GUI 資源內容不冒稱已包含。

排除 `.git/`、`build/`、`dist/`、`.scratch/`、`plan/`、`node_modules/`、私有設定目錄、`.env`、憑證與金鑰。helper 的來源範圍採明確根檔案／目錄清單，不遍歷整個工作區。

## 解壓驗證與離線建置

解壓後執行 `node tools/archive-source.mjs --verify-extracted`，逐檔核對 `SOURCE-ARCHIVE.json` 的大小與 SHA-256，拒絕額外來源檔案、符號連結及遺失的必要輸入；建置生成的根 `build/`、`dist/` 不參與來源核對，允許重複建置。

原 `build.ps1` 保留不變，供有完整 Git 檢出的工作區使用；去除 `.git` 的來源快照改用 ZIP 內生成的 `build-from-source.ps1`：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build-from-source.ps1
```

這個入口只將原建置腳本的 Git history／HEAD 前置檢查改為完整來源雜湊核對，其餘建置與離線自測步驟一致。需要既有的 MSVC v143 x64、CMake、Node.js 與 Git（只用於本機套 patch），不需要下載上游或初始化 submodule；工具本身不安裝任何相依套件。

補丁以明確隔離的 git-dir 與 `--no-index` 套用；即使解壓在別的 Git 倉庫之內，也不依賴其 HEAD、index 或子目錄範圍。隔離路徑必須不存在，補丁後的來源驗證仍是必要步驟，成功退出碼不能取代它。

封存工具在交付 ZIP 前會實際完整解壓、重新核對全部雜湊、逐一核對上游 `.vcxproj` 編譯來源存在、套用 patch 並執行 `tools/verify-upstream.mjs`；此驗證證明來源與建置輸入完整，不取代實際編譯或音訊裝置驗收。
