# 第一份驗收與效能紀錄

日期：2026-10-10。版本：itouKV 0.2.0。

## 環境

- CPU：Intel Core i5-12450HX，8 cores / 12 logical processors。
- Windows x64、MSVC 19.51、CMake Release 建置。
- 程序／格式 oracle：Node.js v24.16.0。
- 第二個平台：WSL Ubuntu 24.04、GCC 13.3.0；另以 `-O1 -g -fsanitize=address,undefined` 驗證核心。

這是一般開發機上的微型測量，沒有固定 CPU 頻率、清空 OS file cache 或關閉其他應用程式。下列兩個專案的 benchmark 依序執行，避免它們彼此爭用 CPU；仍不代表隔離的效能實驗室。

## 正確性驗收

| 項目 | 本次結果 |
| --- | --- |
| Windows C++ 核心測試 | 12 / 12 通過 |
| Windows Node.js 程序／格式測試 | 19 / 19 通過 |
| Linux 核心測試 | 通過；最終 12 項也在 ASan / UBSan 下全部通過 |
| ASan / UBSan | 本次執行未報告錯誤 |
| 參考模型 | 1,200 次操作，固定 seed，穿插重啟與 compaction |
| WAL 故障點 | 5 個階段，分別在有／無 SSTable 下直接終止程序 |
| Compaction 故障點 | 5 個階段，包含 manifest 發布前／後與清理後 |
| 二進位格式 | 由 Node Buffer + zlib CRC32 獨立組裝／檢查 |
| 邊界 | 最大 64 KiB key、16 MiB value，跨多次 OS I/O，重啟及 compaction 後比對 |

上述測試證據針對已測案例與故障模型；實際硬斷電、所有檔案系統與多層 LSM 不在這次驗收內。CI 已加入 Windows／Ubuntu 及 sanitizer 工作流程，遠端 CI 結果需在推送後另行確認。

## 基準 workload

```powershell
.\build\Release\itoukv_bench.exe 1000
```

1. 寫入 1,000 個不同 key，每個 value 128 bytes，每一筆都 flush。
2. 從 MemTable 讀回全部 key，確認值正確。
3. 關閉、重開，量測 WAL 重放。
4. 執行一次全量 compaction。
5. 從 SSTable 讀回全部 key；再查詢 1,000 個確定不存在的 key。
6. 再重開，量測 manifest + SSTable 驗證／索引載入。

## 一次實測

| 指標 | 結果 |
| --- | --- |
| 每筆同步寫入 throughput | 538.415 ops/s |
| 寫入 p50 / p95 | 1.585 / 3.2299 ms |
| 1,000 次 MemTable 讀取 | 0.7607 ms |
| WAL 重放開啟 | 24.9929 ms |
| Compaction | 55.3534 ms |
| 1,000 次 SSTable 命中查詢 | 45.6737 ms |
| SSTable 開啟／驗證 | 5.7969 ms |
| 1,000 次不存在 key 查詢 | 0.4034 ms |
| Bloom negative / false positive | 993 / 7（此組 absent keys 的 false-positive rate 為 0.7%） |
| 查詢階段讀取 table rows | 8,524 |
| Logical live bytes | 134,890 |
| Compaction 前 WAL bytes | 170,890 |
| SSTable bytes | 149,412 |
| 近似初次寫入放大 | 2.37478× |

寫入放大定義為 `(初始 WAL + SSTable + 32-byte manifest) / logical live bytes`，不包含檔案系統 journal、metadata、讀取、或後續 compaction。這組資料沒有大量覆寫，不能代表寫入密集 workload 的長期放大。

SSTable 寫完後已被 checksum 驗證讀過，量測的是 **warm file cache**。MemTable 與 SSTable 查詢路徑也不同：前者在 RAM 找值；後者經過稀疏索引後逐列讀取／驗證。8,524 rows 是查詢中實際走訪的資料列，不是磁碟實體 I/O 次數。

## 原始輸出

```json
{"operations":1000,"value_bytes":128,"sync_per_write":true,"write_ms":1857.3,"write_ops_per_second":538.415,"write_p50_ms":1.585,"write_p95_ms":3.2299,"memtable_read_ms":0.7607,"wal_recovery_ms":24.9929,"compact_ms":55.3534,"sstable_lookup_ms":45.6737,"sstable_recovery_ms":5.7969,"missing_lookup_ms":0.4034,"bloom_negatives":993,"bloom_false_positives":7,"table_rows_read":8524,"logical_live_bytes":134890,"initial_wal_bytes":170890,"sstable_bytes":149412,"approx_write_amplification":2.37478,"warm_file_cache":true}
```

## 下一個有意義的實驗

- 固定相同 durability，對照單筆 flush 與 group commit。
- 固定 live data 大小，改變 overwrite ratio，觀察全量 compaction 的寫入放大。
- 調整 index stride 與 Bloom bits/key，量測記憶體、查詢走訪 rows 及 false positives。
- 將 startup 全檔 checksum 改為可選的 lazy block verification，測量啟動延遲與損毀偵測時機。
- 在自有測試 VM 中加入儲存層的掉電／重新排序模型，擴大到程序終止之外的故障場景。
