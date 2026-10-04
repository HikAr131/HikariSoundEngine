# 外部審查與採納記錄（2026-10-04）

使用者選擇 DeepSeek 冷讀＋codex 找反證。DeepSeek 主渠道實跑逾時後，以備援渠道 `deepseek-v4-pro` 完成；codex 額度探針通過後，以 read-only／high 執行找反證。兩份輸出引用分別核對 3／3、9／9，有效引用全部吻合；沒有將缺少 VM 實測列成程式碼缺陷。

下表合併重複問題。原始審查输出保留於本機 `.scratch/`，不封入公開來源；引文核對在修正前執行，修正後行號會改變。

| 目前程式碼問題 | 採納與修正範圍 | 驗證界線 |
| --- | --- | --- |
| 舊守護在新執行個體持有 mutex 時直接退出，留下新守護尚未就緒的空窗 | 採納；以持久化守護就緒證據交接，未完成交接的舊守護繼續等待 | 合成決策與假子行程；實際裝置還原待 VM |
| 舊守護復原時，新執行個體以零等待搶 mutex 而被拒絕 | 採納；啟動以有限等待容許復原完成 | 本機控制代碼／決策；真正快速重啟待 VM |
| 最後一次 mix format 或 closest match 與 cached format 不同，音訊迴圈仍使用舊聲道數 | 採納；所有四個 WASAPI Initialize 呼叫前核對格式，不吻合則拒絕 | 合成格式與逐呼叫補丁變異；熱變更待 VM |
| 實體端點改名 FxSound 可繞過無虛擬裝置檢查 | 採納；查 DeviceTopology 對應 adapter 的 PnP hardware ID 與 fxvad service | 硬體識別解析及無驅動安全退出；實際 fxvad 拓樸待 VM |
| 官方 FxSound 持續執行時，每秒重做還原 | 採納；衝突狀態保持退讓，已完成的復原不再次覆寫系統狀態 | 合成狀態判據；官方程式共存待 VM |
| 暫停時自己的還原通知算成外部切換，以及上下游雙回呼跨 tick 重複計數 | 採納；使用單一持久通知來源，成功自身寫入按角色／端點分類 | 合成通知帳本與跨批次案例；Windows 通知排程待 VM |
| shutdown 後仍派送入列 apply，將 cleanExit 改回 false | 採納；停止判據在派送前生效，closing 狀態拒絕變更命令 | 合成停止判據；登出／關機待 VM |
| set-output／set-buffer 初始化失敗後，記憶體／狀態檔分叉，仍保留未初始化音訊物件 | 採納；失敗停止物件、復原並保存實際狀態，初始化成功後才持有可處理物件 | 結構核對與離線自測；真裝置失敗注入待 VM |
| 四聲道 mask 0x000f 被當 Quad，中心／低音與環繞送錯 | 採納；只接受符合上游位置排列的 float32 layout，不改聲道轉換迴圈 | 合成 2／4／6／8 聲道格式案例；實際 USB／7.1 待 VM |

codex 沒有找到能推翻 `preset_list_handle_` 未初始化診斷的原始碼反證；constructor 的 NULL 初始化保留。沒有找到 helper 管線 client impersonation 或來源封存確定阻斷；這些結論不替代真機或解壓編譯證據。

裝置身分核對依據 Microsoft 的 [Device Topologies](https://learn.microsoft.com/en-us/windows/win32/coreaudio/device-topologies)、[GetDeviceId](https://learn.microsoft.com/en-us/windows/win32/api/devicetopology/nf-devicetopology-idevicetopology-getdeviceid) 與 [CM_Get_DevNode_Registry_PropertyW](https://learn.microsoft.com/en-us/windows/win32/api/cfgmgr32/nf-cfgmgr32-cm_get_devnode_registry_propertyw)。顯示名稱仍可用來展示，不參與虛擬驅動判定。

守護交接與音訊格式邊界的修法增加了判據，因此修正後另做限定範圍的設計核對。原報告兩列的欄位順序不符機械模板，由原審查者只重排欄位、不改引文後，引用核對 2／2 完全吻合；採納暫停後重新接管仍沿用舊音量快照、以及初始 RDP 暫停時漏掉繼承復原這兩條，分別以新接管保存新快照、初始暫停先完成繼承復原修正，未另做全倉庫複審。

來源 ZIP 的實際離線重建另發現：當解壓目錄位於別的 Git 倉庫下，Git 自動尋找上層倉庫，`git apply` 可能略過不在其子目錄範圍的檔案卻返回成功。後續補丁驗證正確拒絕了該次建置；建置改以明確不存在的 git-dir 搭配 no-index 套用，避免依賴或觸碰上層倉庫，原入口與 ZIP 生成入口共用此修正。

最終建置與來源解壓编譯證據見 `validation-report.md`；裝置身分 API 的實際 adapter property 值仍列 VM 待測。
