# Atomic WriteBatch（0.3.0）

`WriteBatch` 把一串有順序的 put/delete 當成**一筆 WAL commit**。整批只有一次
`fsync` / `FlushFileBuffers`；成功回覆後，重啟會恢復整批修改。讀取 API 不會
看到批次中間狀態。

```cpp
itoukv::Store db("./settings.db");
itoukv::WriteBatch batch;
batch.put("version", "0.3")
     .put("enabled", "true")
     .erase("obsolete");
db.write(batch);
```

## 精確語意

- Builder 擁有自己的 key/value 副本，建立或修改 batch 不執行 I/O。
- 操作依加入順序執行；同一 key 最後一筆有效操作決定結果。
- 每個非空 batch 的 `sequence` **只增加 1**。既有單筆 put/delete 仍各增加 1。
- 空 batch 是 no-op，不產生 WAL 紀錄，也不增加 sequence。
- Batch 可重用、複製、移動、`clear()`；移動後的來源會是可再次使用的空 batch。
- 單筆 key/value 上限沿用 64 KiB / 16 MiB。每批最多 4,096 筆、32 MiB
  編碼後 payload（包含每筆 9-byte metadata）。超限的加入不改變原有 batch。
- 同一 Store 的 mutex 涵蓋寫入、flush 與整批 MemTable 更新。`scan` 回傳某一
  時點的多 key 快照；兩次分開的 `get` 呼叫仍可能夾著另一筆 commit。
- Batch builder 本身沒有執行緒鎖；呼叫 `write(batch)` 時不得在另一執行緒修改它。

這是原子寫入批次，不包含 read-modify-write transaction、條件更新、rollback、
snapshot isolation 或自動跨請求 group commit。

## 故障契約

| 中斷時機 | 重新開啟後 |
| --- | --- |
| Header / 部分 entries / 缺少 checksum | 截掉整筆尾端 batch，保留先前 commits |
| 完整 batch 已寫入、尚未 flush／回覆 | 整批可能存在或不存在，不接受部分存在 |
| Flush 完成、尚未回覆 | 整批存在；呼叫端仍可能不知道結果 |
| 完整紀錄 checksum 或內部 entry 不合法 | 拒絕開啟並保留檔案，不靜默略過 |
| 寫入／flush／記憶體發布期間例外 | Store poisoned，必須重開；結果可能已提交 |

Decoder 先驗證整筆 checksum、每個 entry 的長度與 tag、entry 數量與 payload
邊界，再套用任何 entry。記憶體配置若在套用時失敗，該建構／寫入不會暴露一個
可繼續讀取的部分狀態；重新開啟後以完整 WAL 為準。

## CLI

```powershell
.\build\Release\itoukv.exe .\demo.db batch .\examples\settings.tsv
.\build\Release\itoukv.exe .\demo.db scan

# stdin，同樣是 ASCII hex 欄位；PowerShell 的 `t 是 tab、`n 是 newline。
"put`t61`t31`nput`t62`t32" | .\build\Release\itoukv.exe .\demo.db batch -
```

TSV 格式：

```text
put<TAB>key_hex<TAB>value_hex
delete<TAB>key_hex
```

空 key/value 欄位合法。支援 CRLF、LF、空白行及以 `#` 開頭的註解。所有欄位都
使用 hex；不依賴全域 `--hex` 旗標。CLI 在開啟／修改資料庫前驗證整份輸入，
無效行會報出行號。整份文字輸入有約 64 MiB 上限，防止無界讀取。

## WAL v2 batch record

v1 的單筆紀錄格式保持原樣；0.3 decoder 允許 v1 / v2 交錯。含 v2 紀錄的 WAL
需要 0.3+ 讀取。SSTable 與 manifest 格式不變，checkpoint sequence 現在明確
表示 commit 序號，不是批次內的 mutation 數量。

| Offset | Bytes | Meaning |
| --- | --- | --- |
| 0 | 4 | `IKV2` |
| 4 | 4 | Payload bytes，最多 32 MiB |
| 8 | 8 | Commit sequence |
| 16 | 4 | Entry count，1..4096 |
| 20 | 4 | Reserved zero |
| 24 | 1 | Tag = 3（batch） |
| 25 | 3 | Reserved zero |
| 28 | 4 | CRC32 of bytes 0..27 |
| 32 | variable | Entries |
| last | 4 | CRC32 of complete header + payload |

每個 entry：`tag:u8 | key_length:u32 | value_length:u32 | key | value`。
Tag 1 = put，2 = delete；delete 的 value length 必須為 0。所有整數為
little-endian。v1/v2 共用連續 commit 序號及 IEEE CRC32。

## 驗證與效能

- C++：batch ownership、copy/move 後重用、重複 key、空 batch、大小限制。
- 逐一截斷 batch 的每個 byte 邊界；完整 batch 的每個 byte 做 bit flip。
- 300 批固定 seed 隨機操作對照參考模型，混合重啟與 compaction。
- Writer 更新多個 key，同時 reader 使用 `scan` 驗證原子可見性。
- 六個故障階段，在有／無 SSTable 的情況下直接終止真實程序。
- Node Buffer + zlib 獨立驗證 v2 格式，也測試 CRC 正確但 entry 格式錯誤的紀錄。

```powershell
.\build\Release\itoukv_batch_bench.exe 1000
```

比較同樣 1,000 次更新、同樣每個 commit 同步的單筆 put、10 筆 batch、100 筆
batch。每組使用新資料庫並重啟比對所有值。結果包含總耗時、ops/s、commit
數量與 WAL bytes。Batch=1 使用既有 v1 put 路徑，其他使用 v2；量測也包含編碼與
builder 成本，不應將所有差異都歸因於 flush 次數。
