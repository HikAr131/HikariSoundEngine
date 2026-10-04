# 會話 B 交接

倉庫：`G:/Dev/1U/HikariSoundEngine`。目前版本 1.0.1，protocol 1。只在這個獨立倉庫開發，沒有修改 1U 本體。上游釘值 `d8e7a23d37ed5939c2a3090a1c1756c7f2500b17`；補丁套到 build copy，原子模組保持乾淨。

## 1.0.1 給會話 B 的變更摘要

### 版本與產物

| 項目 | 1.0.0（已散布） | 1.0.1（本次） |
| --- | --- | --- |
| 原始碼 | 公開 tag `v1.0.0` → `a3cd408`，Release 已掛來源 ZIP | tag `v1.0.1`（推送與 Release 需使用者同意；狀態見總結） |
| `hello.version` | `1.0.0` | `1.0.1`（protocol 仍為 1） |
| 已簽章 exe | 1U 憑證簽章，SHA256 `8ce4557754303426efdaf69a2344b24fc90ce139435110d20eed59b0a42272cf`；已釘進 1U 的 `COMPONENT_VERSION 1.0.0` 並上傳三個網域 | 尚未簽章；未簽章 exe 雜湊見下方「最終產物」，簽章後要重算 |
| 另存位置 | `.scratch/v1.0.0-signed-dist/`（gitignore 範圍內，含簽章 exe、`sign.log` 與當時的 ZIP） | `dist/` |

1.0.0 的 tag、Release 與已簽章執行檔都不動：它們是已散布 1.0.0 的對應原始碼與產物。1.0.1 的簽章由 1U 倉庫的 `scripts/sign-sound-engine-helper.cmd` 處理，簽完要重新算 `dist/HikariSoundEngine.exe` 的大小與 SHA256，再釘進 1U。

### 協定新增（只加不改）

- **`OUTPUT_EXCLUSIVE_LOCKED` 開始會出現**。實體輸出被另一個程式獨佔時，`status.state` 是既有的 `idle-no-device`（沒有新增 state 值），`status.lastError` 是：
  `{"ok":false,"code":"OUTPUT_EXCLUSIVE_LOCKED","message":"Output endpoint is in exclusive use by another application","hresult":"0x8889000A","device":"<裝置顯示名>"}`
  1U 端請用 `lastError.code` 判斷，顯示 plan §8 的 `SOUND_ENGINE_OUTPUT_EXCLUSIVE_LOCKED`（B 類），`{device}` 取 `lastError.device`。訂閱者會收到 state 事件；恢復後 `lastError` 變回 `null`、state 回到 `starting`／`processing`。
- **獨佔期間的重試與恢復節奏**：helper 每 2 秒以共享模式探測一次該輸出（只 Initialize、不播放、不改音量）。探到可用時只觸發一次上游重新初始化，所以對方放開後約 2 秒內自動恢復，不需要 1U 介入或重開 helper。1U 不必自己輪詢或重啟引擎。
- **`lastError.hresult` 與 `lastError.device`**：其餘播放端初始化失敗維持 `code:"INTERNAL"`（message `Audio output initialization failed`），同樣帶 `hresult`（八位大寫十六進位，如 `0x80004005`）與 `device`。之後以 2、4、8、16、30 秒退避自動重試；上游停住卻沒有任何回報時也這樣退避，但不產生 `lastError`。這些欄位給診斷判讀用；1U 的對應表沒有登記的碼照舊退到 `SOUND_ENGINE_DOWN`。日誌只寫碼與 HRESULT，不寫裝置 ID 或路徑。
- **`status.stats.applyLockMaxUs`**：本次音訊啟動以來 apply 最長的持鎖時間（微秒，向上取整），沒有音訊物件時為 `null`。
- **新增狀態值 `ready`**：輸出已在這次音訊工作階段成功初始化、上游正在執行（沒有停住）、虛擬裝置是系統預設（`--no-default-switch` 時不要求），但還沒處理過任何音訊。沒有播放聲音時 loopback 不送封包，`audioFrames` 一直是 0，就會停在 `ready`；一有 frames 就轉成 `processing`。1U 的開啟流程請把 `ready` 與 `processing` 都當成成功，`sound-engine.js` 只認 `processing` 的地方要改，否則沒放聲音時按「开启」必定失敗。優先順序：輸出失敗 > `bypassed` > `processing` > `ready` > `starting`。
- **`bypass` 不寫進 `state.json`**：「对比原声」只在按住時生效；helper 寫狀態檔時一律寫 `bypass:false`，讀回時也強制為 false（包含 1.0.0 寫下的狀態檔）。`status.applied` 與 `apply` 回應仍反映當下真實的 bypass。1U 端不用改。
- **`virtual.isDefault`**：沒有虛擬端點時改為 `false`（1.0.0 在「沒有虛擬端點、也沒有預設裝置」時會誤回 `true`）。

### 行為改變

- **相同參數的 apply 不再重建**：與目前套用值逐欄完全相同（夾限之後比）時，helper 直接回 `ok` 與 `applied`，不建濾波器、不碰音訊、不重寫 `state.json`。1U 重送同一組參數是安全的。
- **淡化時長 20 ms**：EQ（preamp／圖形／參數／高音）改變時，新濾波器先用最近的輸入預熱，再與舊的交叉淡化 20 ms；連續操作時新目標排隊、只留最新的，改回原參數會從對稱位置折返。`bypass`（「对比原声」）也以 20 ms 淡入淡出；淡化完成後輸出與輸入逐位元組相同，上游處理鏈在背後照常運算。
- **上游 setter 只在值有變時呼叫**。最長持鎖時間：只改 EQ 0.5 µs；改一個效果約 102 µs；五個效果一次全改約 543 µs（上游 setter 會寫登錄檔，必須與音訊處理互斥）。新濾波器的追趕一律在鎖外，鎖只用來確認它已追上。
- 上游五個效果自己在數值改變時會不會咔嗒，屬於上游行為，列在 `vm-validation.md` 第三輪記錄。
- **DLL 載入**：靜態匯入只剩 KnownDLL（ADVAPI32、KERNEL32、ole32、SHELL32、USER32），CFGMGR32 與 WTSAPI32 改為延遲載入，`wWinMain` 第一件事是 `SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32)`。元件目錄旁被放進同名 DLL 也不會被載入。每次建置都由 `tools/verify-distribution.mjs` 檢查。

### 來源封存

`dist/HikariSoundEngine-source.zip` 改為瘦身版（檔名不變）：上游保留全部原始碼與建置檔，只省略與建置無關的預編二進位（官方安裝程式、除錯符號、舊驅動與工具），三個驅動原檔保留。manifest `SOURCE-ARCHIVE.json` 的 `format` 改為 2，多了 `omissionPolicy` 與 `omitted`（路徑、大小、git blob ID）。ZIP 內路徑改為正斜線。大小與雜湊見下方。

### 最終產物

| 檔案 | 大小 | SHA256 |
| --- | ---: | --- |
| `dist/HikariSoundEngine.exe`（未簽章） | 1,285,120 | `71af520fb7315fa82ecf7ed6ff5404074d98b9440a0b5be6e1bdb4d7c66c15bc` |
| `dist/drivers/fxvad.sys` | 326,656 | `425629b6309000013e8cd1a9b827bee365d21c9f743873aadd0c3bc96a999d2a` |
| `dist/drivers/fxvad.inf` | 5,170 | `b7049bfce3bd60ede027518785d3087c48f546e0ff082af634eb9d819c81d273` |
| `dist/drivers/fxvadntamd64.cat` | 10,590 | `25c8dae186155d20f74feedefb4f84161e4215925b8fd0c898f68f3e50ebcd7d` |

兩次乾淨建置逐位元組相同。來源 ZIP 與其他檔案的雜湊以 `dist/SHA256SUMS.txt` 為準。簽章由 1U 倉庫的 `scripts/sign-sound-engine-helper.cmd` 處理：簽完 `dist/HikariSoundEngine.exe` 的大小與雜湊都會變，要重算 `SHA256SUMS.txt` 再釘進 1U 的元件清單；驅動三檔絕不簽、絕不改。

## 協定與接線（沿用 1.0.0）

- 工作階段 pipe：`Hikari1U.SoundEngine.<sessionId>`，JSON Lines 上限 64 KB；hello/status/apply/set-output/set-buffer/devices/subscribe/quit，id 回傳配對。新欄位只增加。
- apply.eq 使用修訂 §16 的 `points`：1–256 個有限數值點、頻率嚴格遞增、增益 ±12 dB。圖形與參數 EQ 都在自有段；DfxDsp EQ 永遠關閉。
- 使用者在會話 A 的決定是「採 §16，但保留參數 EQ 預覽相容」，因此 BP／NO／AP／LSQ／HSQ 仍用 PK 近似，LS／HS 與預覽相同，不做 corner-frequency 轉換；共享 plan 的 §16 第二次修訂已採相同規則，圖形保真度仍照 §16 驗收。
- launch-hint.json 正規形狀：`{"writtenAtMs":<UTC Unix毫秒>,"outputId":"<實體端點ID>"}`。寫入距離啟動不超過 60 秒；小程式讀一次，不覆寫這個檔。
- 1U 是偏好檔的唯一寫入者；helper 是 state.json 的唯一寫入者，保存 applied／output mode／buffer／端點排名／還原與執行個體證據。異常退出後快速重啟會繼承舊還原證據。
- `stats.underruns:null` 與 `underrunMeasurementAvailable:false` 表示沒有可靠量測來源；不可當作零次。`processing` 需要上游已處理 frames 且播放端點可用，不能只靠安裝或初始化成功。
- 只有確知 pipe 不存在時 CLI status/devices 回 running:false；協定或逾時錯誤回 INTERNAL，不能宣稱未運行。
- 原生中繼的 identification SQOS 與管線 v1 相容；helper 自己的 CLI client 也使用 SECURITY_IDENTIFICATION，不 impersonate。

## measure

`HikariSoundEngine.exe measure --params <JSON檔> [--rate 48000]` 在記憶體中走同一個 DspAdapter、低電平脈衝與 512 個對數頻點，完全不開音訊端點。回應含 `frequencies`、`gainDb`、`sampleRate`、`channels`、`latencyFrames`、`impulseFrames`、`inputAmplitude`；gainDb 是整條處理鏈的實際幅頻響應。latencyFrames 只是這段離線處理的脈衝起點，不是 WASAPI／驅動／硬體端到端延遲。1.0.1 的 measure 數字與 1.0.0 逐個相同。

請 B 的保真度腳本將所有五項效果與高音設 0，bypass:false；參數 EQ 按目前 preview 參考函式，圖形 EQ 按 log-frequency linear dB 目標。公開社區預設只在本機唯讀量測，不帶進公開 helper 倉庫。

## 原始碼核對與限制

EqualizerAPO 的圖形 EQ 在第一點以下／最後一點以上維持端點增益；PK／LSC／HSC／LP／HP 的係數與 Q 解讀符合 RBJ。LS／HS 舊元件用 corner frequency 而預覽當 center frequency；LSQ／HSQ 不在舊元件支援表。原始碼只讀核對，沒有抄入本 helper。

核對來源為官方 SVN r100 的固定歷史 mirror commit `53d885f7f1a097b457e17a5206b7d60f647877a8`（git-svn-id 指向 trunk@100，2024-09-27）；SourceForge 直接 URL 當下回 404，沒有宣稱 mirror 是最新版本。圖形插值與兩端行為見 [GainIterator.cpp:73–92](https://github.com/mirror/equalizerapo/blob/53d885f7f1a097b457e17a5206b7d60f647877a8/helpers/GainIterator.cpp#L73-L92)；RBJ 與 Q 見 [BiQuad.cpp:35–110](https://github.com/mirror/equalizerapo/blob/53d885f7f1a097b457e17a5206b7d60f647877a8/filters/BiQuad.cpp#L35-L110)。LS／HS 的 corner 轉換見 [BiQuadFilter.cpp:44–59](https://github.com/mirror/equalizerapo/blob/53d885f7f1a097b457e17a5206b7d60f647877a8/filters/BiQuadFilter.cpp#L44-L59)；有效型別表見 [BiQuadFilterFactory.cpp:41–54](https://github.com/mirror/equalizerapo/blob/53d885f7f1a097b457e17a5206b7d60f647877a8/filters/BiQuadFilterFactory.cpp#L41-L54)。

上游只設 eConsole；是否也需 eMultimedia，要依 VM 播放器／遊戲結果決定。保留原音訊迴圈、time-critical thread、Sleep(1) polling，沒有加入 MMCSS／重取樣／漂移補償／靜音停流。輸出限支持格式，避免上游非整數倍取樣率的音高問題。A4 所有真實量測目前 NOT RUN，暫定 40 ms 不代表已驗證。

驅動為原 `Version14/win10/x64` 三檔，只複製到 dist/drivers；SHA256 見 dist/SHA256SUMS.txt 和驗證報告。它們不能另行簽章，原始碼與簽章二進位對應尚未證明。正式元件包要映射成 B plan 的檔案布局，不能把未簽章 exe 的雜湊當正式釘值。

## 尚需使用者／測試機

A4：20／40／80 ms 實際延遲、underrun、65 分鐘漂移、兩分鐘靜音後 powercfg requests；A1／A3 裝置接管、還原、退讓與 session/power 清單，以及 1.0.1 的實聽咔嗒與獨佔恢復，見 `vm-validation.md`。
