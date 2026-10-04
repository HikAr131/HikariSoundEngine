# 本機驗證報告（2026-10-04）

使用者已確認先交付本機實作與驗證，A4 保留待測。這份報告不把離線自測當成音訊裝置、驅動或遊戲驗證；本次未安裝驅動、切換預設、播放實體測試音、建立啟動項、簽章或發布。

## 本機完成標準

| 項目 | 驗證與結果 |
| --- | --- |
| x64 /MT /GUI | `tools/verify-distribution.mjs` 驗 PE x64、GUI subsystem，匯入表只含 Windows 系統 DLL，沒有動態 VC runtime |
| 完整建置與自測 | `powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 -Clean` 退出碼 0；管線、參數與圖形 EQ、生命週期、狀態檔、假守護子行程、DSP、measure 均通過 |
| 參數 EQ | 14,336 個唯讀執行原 TypeScript 產生的參考點，≤0.1 dB；BP／NO／AP／LSQ／HSQ 保留 PK 預覽近似 |
| 全零處理鏈 | 20 Hz–20 kHz，48 kHz、512 點低電平脈衝，最大偏差 `8.51406e-7 dB`；五個穩態正弦案例也通過 ±0.1 dB |
| 圖形 EQ | 13 個內置預設、30 Hz–16 kHz、512 點，完整 DspAdapter 處理链最大偏差 `0.105548 dB`，門檻 ≤1 dB |
| 效果與限制器 | 五個效果各自 10 與 0 的实际波形不同；16 種取樣率／聲道組合、2／4／6／8 聲道全幅噪音限制器、bypass 通過；128 條高增益濾波器的 8 個極端全鏈案例也要求 finite 且 ≤1 |
| 圖形串流 | 完整 IR 與實際卷積逐 tap 相差 <1e-6；多聲道隔離、137 frame 分塊一致、256 點與任意 cwd 測試向量查找通過 |
| 無裝置安全退出 | 執行前唯讀 probe 確認無虛擬端點；`run` 實際退出碼 2、`VIRTUAL_DEVICE_MISSING`；三個預設角色與端點清單前後相同，指定狀態目錄與登錄根未建立 |
| 上游保護 | 固定子模組乾淨；建置副本套用補丁後驗證；33 個補丁驗證變異均被拒絕，還原後通過 |
| 位元組檢查 | 來源非法控制字元為 0，交付 PowerShell 純 ASCII；原始驅動三檔逐位元組相同 |

`measure` 使用 131,072 frame、256 frame 區塊、雙聲道、輸入脈衝振幅 0.0001，並先送 32 個零區塊。這些數字只代表此電平下的離線頻響；啟用非線性效果時，不能推成所有音量的線性響應。圖形 FIR 的區塊延遲是 256 frames，48 kHz 時 5.333 ms；這不是整套系統的硬體延遲。

## 內置曲線完整鏈量測

| 預設 | 最大偏差 dB |
| --- | ---: |
| 1UGeneral | 0.00352145 |
| 1UMusic | 0.0166779 |
| 1UVoice | 0.0342991 |
| 1UVolumeBoost | 0.0248925 |
| 1UGaming | 0.0070016 |
| 1UClassic | 0.000000851406 |
| 1ULight | 0.0102161 |
| 1UBassBoost | 0.0248685 |
| 1UStreamingVideo | 0.00100895 |
| 1UMovies | 0.0124351 |
| 1UTV | 0.00618329 |
| 1UTranscription | 0.105548 |
| 1UDefault | 0.000000851406 |

| 合成案例 | 完整鏈最大偏差 dB | 自有 FIR RMS 偏差 dB |
| --- | ---: | ---: |
| 全 +12 | 0.000000141375 | 0 |
| 全 −12 | 0.000000437456 | 0 |
| 隔段 ±12 | 0.226075 | 0.0164001 |
| 單段 +12 | 0.0117593 | 0.000521620 |
| 高低階梯 | 0.000879501 | 0.0000535373 |

合成案例只記數字，沒有把它們當成內置預設門檻；內置預設門檻沒有放寬。另以自有 FIR 核對 44.1／96／192 kHz，13 個內置預設的最大偏差為 0.0988046／0.258187／0.488456 dB；完整鏈正式表格採 48 kHz。

## 最終產物與重建證據

最後兩次 `build.ps1 -Clean` 均退出 0，完整自測通過，執行檔逐位元組一致。最終未簽章執行檔為 1,163,264 bytes，SHA256 為 `fb199f6a5ba82c15f8713a3148330f07778eb20d2cac0b13641c71982abb6f81`；PE 匯入只有 ADVAPI32、CFGMGR32、KERNEL32、SHELL32、USER32、WTSAPI32、ole32 七個 Windows 系統 DLL。

同一執行檔再通過無虛擬裝置安全退出，以及三批、每批六個並行的 `measure`，18／18 退出 0、512 個量測值均為有限數，平直最大偏差 `8.51405742669052e-7 dB`。離線來源 ZIP 在另一個 Git 倉庫內解壓成無 `.git` 的來源樹，921 個檔案的 manifest 核對通過；`build-from-source.ps1 -Clean` 實際編譯與完整自測均退出 0，輸出 `OFFLINE_SOURCE_COMPILE_AND_SELF_TEST_PASSED`。

來源重建驗證使用最終程式碼與建置入口，最終封存只再更新本報告與交接文件。最終 ZIP 另重新解壓核對全部來源雜湊、補丁與編譯輸入；來源 manifest 保存本機提交釘值，ZIP 與其他產物的雜湊見 `dist/SHA256SUMS.txt`。

守護交接、重新接管音量快照、初始暫停繼承復原、格式與硬體識別均有離線合成測試；真實 fxvad adapter 的 PnP property、Windows 通知時序及裝置還原結果仍需依 VM 清單驗證。外部審查與逐條採納記錄見 `review-results.md`。

## 驅動原件雜湊

| 檔案 | SHA256 |
| --- | --- |
| fxvad.sys | `425629b6309000013e8cd1a9b827bee365d21c9f743873aadd0c3bc96a999d2a` |
| fxvad.inf | `b7049bfce3bd60ede027518785d3087c48f546e0ff082af634eb9d819c81d273` |
| fxvadntamd64.cat | `25c8dae186155d20f74feedefb4f84161e4215925b8fd0c898f68f3e50ebcd7d` |

以上來自固定 app 子模組的 Version14/win10/x64；只複製，沒有修改或另簽。它們與參考驅動原始碼能否重建成相同簽章二進位仍為 NOT VERIFIED。

## 待測與交付界線

A4 三檔端到端延遲、underrun、65 分鐘漂移與靜音兩分鐘後 power requests 均為 NOT RUN，暫定 40 ms 沒有升格為已驗證設定。裝置接管／還原、eConsole 覆蓋、睡眠、RDP、HVCI、藍牙、7.1、遊戲與反作弊依 `vm-validation.md` 驗收。公開建庫、push、tag、Release、關 Actions、簽章都沒有執行；目前執行檔是未簽章本機產物。

完整來源封存的 manifest 保存每個源檔雜湊與固定上游提交，包含實際編譯來源、補丁、測試向量、授權文件與原始驅動原件；解壓後以 `build-from-source.ps1` 重建。來源 ZIP 的最終雜湊以 `dist/SHA256SUMS.txt` 為準，不將自引用 ZIP 雜湊写進 ZIP 本身。

## 驗證暫存殘留

自動批准審查拒絕清除下列兩個驗證目錄，原回報理由均為 `blocked by policy`。它們不參與建置或分發，也不影響程式使用；沒有改用其他刪除方式繞過拒絕。

- `C:/Users/Administrator/AppData/Local/Temp/hikari-source-mutation-3333bee66bed4c159614f500a629cb1a`：三個來源封存變異驗證檔案。
- `G:/Dev/1U/HikariSoundEngine/.scratch/offline-source-build`：第一次離線重建失敗的解壓來源副本，未編譯成產物。
