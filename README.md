# HikariSoundEngine

Windows x64 的無介面音效常駐程式，將 FxSound 的 `audiopassthru` 與 `dsp` 包成可以單獨使用的命令列工具。宿主以 AGPL-3.0-or-later 提供；與 1U 透過具名管線交換 JSON，1U 不連結這個倉庫的程式碼。它不安裝驅動、不建立啟動項、不連網，需要先有 FxSound 虛擬播放端點。

目前是本機實作與離線驗證階段，尚未公開、簽章或發行；真實音訊的虛擬機驗收與 A4 量測均為 **NOT RUN**。沒有虛擬裝置時 `run` 在建立宿主、寫檔、寫登錄檔或切換預設裝置之前回 `VIRTUAL_DEVICE_MISSING`。開發機禁止安裝驅動、切換預設播放裝置、向實體裝置播放測試音及建立啟動項。

## 建置

需要 Git、Node.js 24、Visual Studio 2022 MSVC v143 14.44.35207、Windows SDK 與 CMake 3.25 以上。上游子模組固定在 `d8e7a23d37ed5939c2a3090a1c1756c7f2500b17`，必須具有完整提交歷史且沒有本機變更。這個提交包含最後實體輸出消失時的重試處理與 1.2.15 裝置音量修正；這是原始碼證據，硬體行為仍待驗證。

```powershell
git submodule update --init
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 -Clean
```

腳本複製兩個上游函式庫到 `build/upstream`，核對並套用 `patches/`，編譯 x64 `/MT`、GUI subsystem、asInvoker、`/Brepro`，執行離線自測後才交付 `dist/HikariSoundEngine.exe` 與 `SHA256SUMS.txt`。自測失敗或超時會使建置失敗。`dist/drivers/` 的三個檔案是原上游已簽章檔案，只複製、不安裝、不修改或另行簽章。

## 單獨使用

GUI subsystem 不開黑色視窗；JSON 寫入呼叫者已提供的標準輸出。使用 PowerShell `Start-Process -WindowStyle Hidden -RedirectStandardOutput <檔案>` 可擷取輸出，等待時取得行程物件並呼叫 `.WaitForExit()`。

```text
HikariSoundEngine.exe --version
HikariSoundEngine.exe --self-test
HikariSoundEngine.exe --probe-devices
HikariSoundEngine.exe measure --params <JSON檔> [--rate 48000]
HikariSoundEngine.exe run [--data-dir <絕對目錄>]
HikariSoundEngine.exe stop
HikariSoundEngine.exe status
HikariSoundEngine.exe devices
HikariSoundEngine.exe run --capture-endpoint <虛擬端點ID> --output-endpoint <實體端點ID> --no-default-switch
HikariSoundEngine.exe guard --pid <pid>
```

`--probe-devices` 是額外的唯讀診斷入口，可在引擎未執行時列出端點、格式與三個預設角色，用於安全退出前後比對。`devices` 與 `status` 經管線查詢，沒有執行個體則回 `running:false`。除錯擷取端點必須是唯一的 FxSound 虛擬端點；不接受任意實體擷取裝置。

預設資料目錄是 `%APPDATA%/HikariSoundEngine`。1U 可傳 `%APPDATA%/Hikari-Toolkit/sound-engine`。宿主只寫 `state.json` 與 `logs/engine.log`；讀取一次的 `launch-hint.json` 由呼叫者寫，v1 格式為 `{"writtenAtMs":<UTC Unix毫秒>,"outputId":"<實體端點ID>"}`，只接受 60 秒內且端點目前存在的提示。日誌每份 256 KB，保留兩份輪替；不記錄使用者路徑、端點識別碼或請求參數。

## 管線協定 v1

名稱：`\\.\pipe\Hikari1U.SoundEngine.<工作階段編號>`。JSON Lines，每行最多 65,536 bytes；可選 `id` 是安全整數或最多 128 bytes 的字串，回應保留它。管線拒絕遠端連線、僅本人與 SYSTEM 可存取，名稱已被占用就拒絕啟動；用戶端以 identification SQOS 開啟，伺服器不 impersonate。

```json
{"id":1,"cmd":"hello"}
{"id":2,"cmd":"status"}
{"id":3,"cmd":"apply","params":{"bypass":false,"eq":{"mode":"parametric","preamp":0,"filters":[{"type":"PK","freq":1000,"gain":3,"q":1.4}]} ,"effects":{"clarity":4,"ambience":0,"surround":2,"dynamicBoost":5,"bass":5,"treble":0}}}
{"id":4,"cmd":"set-output","mode":"follow"}
{"id":5,"cmd":"set-output","mode":"fixed","deviceId":"<id>"}
{"id":6,"cmd":"set-buffer","ms":40}
{"id":7,"cmd":"devices"}
{"id":8,"cmd":"subscribe"}
{"id":9,"cmd":"quit"}
```

命令回 `ok` 與相同 `id`，失敗另有 `code` 與 `message`；`apply` 回 `applied` 的實際夾限值。`hello` 回 name、version、protocol、pid。`status` 包含 state、output、virtual、bufferMs、stats、conflict、applied、lastError。訂閱 ACK 後推 `event:"state"`；未變的狀態不重複推播。未知命令、不合法 JSON／UTF-8／重複键／過深巢狀資料回 BAD_REQUEST；半行請求與寫入有時間上限。

效果五項為 0–10，高音 `treble` 為 −100–100（對應 6 kHz、Q 0.7、±8 dB）；preamp ±20 dB。圖形 EQ 使用 `eq.points` 的 1–256 個點，頻率 20–20,000 Hz 且嚴格遞增，增益 ±12 dB；舊欄位 `bands` 拒絕。參數 EQ 最多 128 條，頻率 20–20,000 Hz、增益 ±20 dB、Q 0.1–10。非有限數值與錯誤型別拒絕；未知參數欄位丟棄。`bypass` 仍轉送音訊但跳過兩段 DSP。

處理順序：自有 preamp／圖形或參數 EQ／高音 → DfxDsp 五項效果／原限幅器；DfxDsp 自己的 EQ 永遠關閉。圖形曲線在對數頻率軸上線性插值 dB，兩端外維持端點增益；以自有 16,384 tap minimum-phase FIR 與 256 frame 分區卷積重現，48 kHz 時額外區塊延遲 5.333 ms，常數曲線走純增益、零曲線不增加延遲。參數 EQ 14 種名稱完全相容目前 1U 預覽；**BP、NO、AP、LSQ、HSQ 刻意沿用 PK 近似**，LS／HS 與預覽相同，依 §16 第二次修訂及使用者最後確認的選擇。向量實際執行唯讀的 1U TypeScript 後匯出並保存來源 SHA256，沒有把私有原始碼編進小程式。

`measure` 使用低電平脈衝，在記憶體中走完整處理鏈並回傳 512 個頻點的實際 `gainDb`，不開音訊端點。效果全零與 13 個內置圖形預設分別以 ±0.1 dB 與 30 Hz–16 kHz 最大偏差 ≤1 dB 驗收；實際數字見驗證報告。非線性效果啟用時，脈衝量測只代表所列輸入電平的響應。`latencyFrames` 是離線脈衝起點，不能當成硬體端到端延遲。

## 裝置與退出

宿主沿用上游音訊迴圈，透過掛勾直接指定實體輸出並序列化 DSP 參數與音訊處理。跟隨模式使用最近的系統預設／使用者选择順序；固定輸出消失時等待该端點，跟隨模式可選下一個實體裝置。十秒內三次外部切換預設裝置會退讓，睡眠或解鎖不解除退讓。官方 `FxSound.exe` 執行中時拒絕接管；在執行中出現也停止處理。

接管前保存還原證據並啟動守護；守護核對行程建立時間與映像、等持有的行程控制代碼，新守護已確認就緒才交棒，未完成交棒的異常退出保留復原責任。正常退出、stop、quit、登出與關機會還原仍屬這次接管的預設與音量／靜音；完整成功後不重做，重新接管時記錄新快照，任何角色還原失敗都不標為乾淨退出。未處理例外由守護處理，無法保證 Windows 強制關機來得及完成 COM 還原，需虛擬機驗證。

上游目前只接管 `eConsole`，`eMultimedia` 是否也要接管須依 VM 播放器／遊戲結果決定；通訊角色保持不接管。支援 32-bit float 的 2/4/6/8 聲道與 44.1/48 kHz 整數倍輸出：2 聲道 legacy float 或 mask 0x3、Quad 0x33、5.1 0x3f／0x60f、7.1 0x63f；其他 layout、PCM 及不一致的實際格式會拒絕。上游沒有真正的重取樣器，其他取樣率也拒絕，沒有 ARM64 支援。

## 尚需實測

| 檢查 | 結果 |
| --- | --- |
| 20 / 40 / 80 ms 实際延遲與 underrun | NOT RUN |
| 超過一小時播放的時脈漂移 | NOT RUN |
| 兩分鐘無音訊後 `powercfg /requests` | NOT RUN |
| 只接管 eConsole 是否涵蓋播放器與遊戲 | NOT RUN |
| 插拔、睡眠、远端桌面、關機／登出、異常退出還原 | NOT RUN |
| Win10 / Win11、HVCI、藍牙、7.1、空間音效與反作弊 | NOT RUN |

40 ms 只是 plan 的暫定標準檔；沒有根據離線結果改延遲、漂移、MMCSS 或音訊迴圈。`stats.underruns` 在尚無可靠量測掛勾時回 `null`，`underrunMeasurementAvailable:false`，不是零次的證明。靜音超過十秒是否需要停實體輸出流，必須由 A4 的睡眠請求實測决定，目前未加入。

## 來源與授權

這是 FxSound 的獨立衍生程式，不是 FxSound 官方產品；FxSound 是 FxSound LLC 的商標。沒有沿用它的產品名稱或圖示；虛擬裝置仍顯示 FxSound 是因為驅動原檔不改。詳見 LICENSE、THIRD-PARTY.md、MODIFICATIONS.md、SOURCE.txt 與 MS-LPL 通知。上游 HRTF 表及舊 DSP 機密聲明的出處不确定性照實保留。

公開倉庫、推送、對外 tag、Release 與簽章各需使用者逐次授權。散布二進位之前必须公開对应完整原始碼。VM 驗收清單、量測格式與會話 B 的接線說明见 `docs/`。
