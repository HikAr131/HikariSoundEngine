# 會話 B 交接（本機準備版）

倉庫：`G:/Dev/1U/HikariSoundEngine`；版本 1.0.0，protocol 1。只在這個獨立倉庫開發，沒有修改 1U 本體。上游釘值 `d8e7a23d37ed5939c2a3090a1c1756c7f2500b17`；補丁套到 build copy，原子模組保持乾淨。公開、tag、Release、簽章均未執行，正式釘值必須等簽章後重算。

## 協定與接線

本機交付產物：`dist/HikariSoundEngine.exe`，1,163,264 bytes，SHA256 `fb199f6a5ba82c15f8713a3148330f07778eb20d2cac0b13641c71982abb6f81`；兩次乾淨建置逐位元組相同，完整自測與 18 次並行／重複 measure 通過。完整來源為 `dist/HikariSoundEngine-source.zip`，含全部編譯輸入與離線入口，實際解壓編譯自測通過；本機提交與來源 manifest 對應，檔案雜湊以 `dist/SHA256SUMS.txt` 為準。

以上釘值只識別本機未簽章版本；B 的正式安全清單依既定規則保持 fail closed，直到另行取得正式來源公開及簽章授權並驗證產物。A4 依使用者決定保留待測，不能將離線測量或本機提交當成公開發布完成。

- 工作階段 pipe：`Hikari1U.SoundEngine.<sessionId>`，JSON Lines 上限 64 KB；hello/status/apply/set-output/set-buffer/devices/subscribe/quit，id 回傳配對。新欄位只增加。
- apply.eq 使用修訂 §16 的 `points`：1–256 個有限數值點、頻率嚴格遞增、增益 ±12 dB。圖形與參數 EQ 都在自有段；DfxDsp EQ 永遠關閉。
- 使用者在本會話的最新決定是「採 §16，但保留參數 EQ 預覽相容」，因此 BP／NO／AP／LSQ／HSQ 仍用 PK 近似，LS／HS 與預覽相同，不做 corner-frequency 轉換；共享 plan 的 §16 第二次修訂已採相同規則，圖形保真度仍照 §16 驗收。
- launch-hint.json 正規形狀：`{"writtenAtMs":<UTC Unix毫秒>,"outputId":"<實體端點ID>"}`。寫入距離啟動不超過 60 秒；小程式讀一次，不覆寫這個檔。
- 1U 是偏好檔的唯一寫入者；helper 是 state.json 的唯一寫入者，保存 applied／output mode／buffer／端點排名／還原與執行個體證據。異常退出後快速重啟會繼承舊還原證據。
- `stats.underruns:null` 與 `underrunMeasurementAvailable:false` 表示沒有可靠量測來源；不可當作零次。`processing` 需要上游已處理 frames 且播放端點可用，不能只靠安裝或初始化成功。
- 只有確知 pipe 不存在時 CLI status/devices 回 running:false；協定或逾時錯誤回 INTERNAL，不能宣稱未运行。
- 原生中繼的 identification SQOS 與管線 v1 相容；helper 自己的 CLI client 也使用 SECURITY_IDENTIFICATION，不 impersonate。

## measure

`HikariSoundEngine.exe measure --params <JSON檔> [--rate 48000]` 在記憶體中走同一個 DspAdapter、低電平脈衝與 512 個對數頻點，完全不開音訊端點。回應含 `frequencies`、`gainDb`、`sampleRate`、`channels`、`latencyFrames`、`impulseFrames`、`inputAmplitude`；gainDb 是整條處理鏈的實際幅频響應。latencyFrames 只是這段離線處理的脈衝起點，不是 WASAPI／驅動／硬體端到端延遲。

請 B 的保真度腳本將所有五項效果與高音設 0，bypass:false；參數 EQ 按目前 preview 參考函式，圖形 EQ 按 log-frequency linear dB 目標。公開社區預設只在本機唯讀量測，不帶進公開 helper 倉庫。

## 原始碼核對與限制

EqualizerAPO 的圖形 EQ 在第一點以下／最後一點以上維持端點增益；PK／LSC／HSC／LP／HP 的係數與 Q 解讀符合 RBJ。LS／HS 舊元件用 corner frequency 而預覽當 center frequency；LSQ／HSQ 不在舊元件支援表。原始碼只讀核對，沒有抄入本 helper。

核對來源為官方 SVN r100 的固定歷史 mirror commit `53d885f7f1a097b457e17a5206b7d60f647877a8`（git-svn-id 指向 trunk@100，2024-09-27）；SourceForge 直接 URL 當下回 404，沒有宣稱 mirror 是最新版本。圖形插值與兩端行為見 [GainIterator.cpp:73–92](https://github.com/mirror/equalizerapo/blob/53d885f7f1a097b457e17a5206b7d60f647877a8/helpers/GainIterator.cpp#L73-L92)；RBJ 與 Q 見 [BiQuad.cpp:35–110](https://github.com/mirror/equalizerapo/blob/53d885f7f1a097b457e17a5206b7d60f647877a8/filters/BiQuad.cpp#L35-L110)。LS／HS 的 corner 轉換見 [BiQuadFilter.cpp:44–59](https://github.com/mirror/equalizerapo/blob/53d885f7f1a097b457e17a5206b7d60f647877a8/filters/BiQuadFilter.cpp#L44-L59)；有效型別表見 [BiQuadFilterFactory.cpp:41–54](https://github.com/mirror/equalizerapo/blob/53d885f7f1a097b457e17a5206b7d60f647877a8/filters/BiQuadFilterFactory.cpp#L41-L54)。

上游只設 eConsole；是否也需 eMultimedia，要依 VM 播放器／遊戲結果决定。保留原音訊迴圈、time-critical thread、Sleep(1) polling，沒有加入 MMCSS／重取樣／漂移補償／靜音停流。輸出限支持格式，避免上游非整數倍取樣率的音高問題。A4 所有真實量測目前 NOT RUN，暫定 40 ms 不代表已驗證。

驅動為原 `Version14/win10/x64` 三檔，只複製到 dist/drivers；SHA256 见 dist/SHA256SUMS.txt 和驗證報告。它們不能另行簽章，原始碼與簽章二進位對應尚未證明。正式元件包要映射成 B plan 的檔案布局，不能把本機未簽章 exe 的雜湊當正式釘值。

## 尚需使用者／測試機

A4：20／40／80 ms 實際延遲、underrun、65 分鐘漂移、兩分鐘靜音後 powercfg requests；A1／A3 裝置接管、還原、退讓與 session/power 清單见 vm-validation.md。A5：公開倉庫建立／push／對外 tag／Release／簽章，每一項都要另行明確授權；散布測試包前必须公開對應完整源碼。
