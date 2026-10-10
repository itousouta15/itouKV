# 0.3.0：原子批次驗收與測量

日期：2026-10-10。環境：Windows x64、Intel Core i5-12450HX、MSVC 19.51 Release。
Linux 驗證使用 WSL Ubuntu 24.04、GCC 13.3.0。

## 驗收

- Windows：**17 項 C++ 核心測試、33 項 Node.js 程序／格式測試通過**。
- Linux：同一組 **17 項核心測試在 ASan + UBSan 下通過**，本次未報告錯誤。
- 新增 300 批隨機資料與參考模型比對，包含 batch 內重複 key、重啟及 compaction。
- 每個 batch 尾筆截斷位置都驗證整批不發布；完整紀錄每個 byte 的 bit flip 都會拒絕開啟。
- 六個直接終止程序的故障點，在有／無 SSTable 的條件下各驗證一次。
- 同時執行 writer 與 snapshot reader，檢查兩個 key 不出現半批更新。
- 最大 **32 MiB encoded payload**、copy/move ownership、移動後 builder 重用都有實測。
- 使用 Node Buffer + zlib 獨立組裝 v2 frame；CRC 正確但 entry 無效的紀錄仍會拒絕。

這些結果涵蓋程序中斷與明確故障注入，不等同實機硬斷電或所有 filesystem 的驗證。
完整持久化邊界仍以 README / WRITE_BATCH 規格為準。

## 相同 durability、不同 batch size

```powershell
.\build\Release\itoukv_batch_bench.exe 1000
```

每組都使用新資料庫、相同 1,000 個 key、128-byte value。每個 commit 都同步 WAL，
寫完後重新開啟並比對全部資料。三組依序執行；這是一般開發機單次測量，沒有固定
CPU 頻率或隔離背景應用程式。

| 每批操作數 | Commit 數 | 寫入耗時 | Operations/s | WAL bytes |
| --- | --- | --- | --- | --- |
| 1（既有單筆 put） | 1,000 | 4,285.5 ms | 233.345 | 170,890 |
| 10 | 100 | 351.823 ms | 2,842.34 | 147,490 |
| 100 | 10 | 55.3628 ms | 18,062.7 | 144,250 |

本次 100 筆 batch 的更新吞吐量約為單筆模式的 **77.4 倍**。durable flush 次數
由 1,000 次降為 10 次；編碼、系統呼叫與 WAL framing 也不同，不能將全部差異
只歸因於 flush。

這個 workload 的資料可立即組成 batch，**沒有計入等待湊批的延遲**，也不是多客戶端
自動 group commit。單次結果不代表服務穩定容量，亦不應與先前不同時段的 SSTable
benchmark 直接相比。

```json
{"operations":1000,"value_bytes":128,"sync_per_commit":true,"results":[{"batch_size":1,"commits":1000,"elapsed_ms":4285.5,"operations_per_second":233.345,"wal_bytes":170890},{"batch_size":10,"commits":100,"elapsed_ms":351.823,"operations_per_second":2842.34,"wal_bytes":147490},{"batch_size":100,"commits":10,"elapsed_ms":55.3628,"operations_per_second":18062.7,"wal_bytes":144250}]}
```
