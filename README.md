# itouKV

一個從零實作的 **C++20 持久化 key-value 儲存引擎**。提供可嵌入的函式庫與 CLI，支援任意位元組的 key/value、同步 WAL、重啟恢復、不可變 SSTable、稀疏索引、Bloom filter、範圍查詢及跨程序排他鎖。沒有外部 C++ 套件依賴。

目前版本：**0.3.0，原子 WriteBatch + WAL + MemTable + 單層 SSTable**。單筆操作或整個 batch 各用一次 durable flush；手動 compaction 串流合併新舊資料，以原子 manifest 切換資料世代，再清除舊檔案。

## 建置與使用

需求：CMake 3.20+、C++20 編譯器（MSVC / GCC / Clang）。程序故障／格式測試另需 Node.js 22.15+（使用 zlib CRC32 獨立驗證格式）。

```sh
cmake -S . -B build
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

Windows 請在 Visual Studio Developer PowerShell 執行；執行檔在 `build/Release/`。Linux 的單組態 generator 可加 `-DCMAKE_BUILD_TYPE=Release`，執行檔在 `build/`。

```powershell
.\build\Release\itoukv.exe .\demo.db put name itou
.\build\Release\itoukv.exe .\demo.db get name
.\build\Release\itoukv.exe .\demo.db put language C++
.\build\Release\itoukv.exe .\demo.db scan
.\build\Release\itoukv.exe .\demo.db stats
.\build\Release\itoukv.exe .\demo.db delete name
.\build\Release\itoukv.exe .\demo.db compact
.\build\Release\itoukv.exe .\demo.db stats
.\build\Release\itoukv.exe .\demo.db batch .\examples\settings.tsv
```

每個 CLI 命令都重新開啟資料庫，因此上述 `put → get` 本身就會走恢復流程。`compact` 把 live data 寫入新 SSTable，清空 MemTable 並切換到新 WAL。`get` 回傳 value 加一個 LF；不存在時 exit code 為 2。`delete` 即使 key 不存在也寫入一筆持久化刪除紀錄，回傳 2。使用錯誤為 64，儲存／資料格式錯誤為 1。

### 二進位資料與掃描

```sh
itoukv --hex ./demo.db put 00ff 000180ff
itoukv --hex ./demo.db get 00ff
itoukv ./demo.db scan "" 20
```

`--hex` 解碼 key/value 參數；`get` 改輸出十六進位。`scan [start] [limit]` 包含起始 key，依**無號位元組字典序**排列；預設最多 100 筆。結果固定用 JSON 的 `key_hex` / `value_hex`，可無損表示 NUL、非 UTF-8 等資料。空 key、空 value 都合法；最大 key 64 KiB，最大 value 16 MiB。

### 嵌入自己的程式

```cpp
#include <itoukv/store.hpp>

itoukv::Store db("./example.db");
db.put("name", "itou");                 // 回傳前同步 WAL
auto value = db.get("name");            // optional<string>
auto page = db.scan("name", 10);         // 複製的查詢快照
db.compact();                           // 合併成不可變 SSTable，切換 WAL
bool existed = db.erase("name");
```

CMake：`add_subdirectory(path/to/itouKV)`，再 `target_link_libraries(your_app PRIVATE itoukv)`。同一個 `Store` 可供多執行緒呼叫；同一資料夾只能由一個 `Store` 持有，包括唯讀查詢。鎖由作業系統管理，程序死亡就會釋放，請勿刪除 `LOCK`。

### 原子批次寫入

```cpp
itoukv::WriteBatch batch;
batch.put("version", "0.3").put("enabled", "true").erase("obsolete");
db.write(batch); // 整批一筆 WAL、一個 commit 序號、一次 durable flush
```

支援重複 key、空 batch、binary data、copy/move 後重用。最多 4,096 筆、32 MiB
編碼後 payload。恢復時整批存在或整批不存在，讀取不會看到部分發布。`sequence`
計算 commit 數，不是 batch 內的操作數。完整契約、CLI TSV 格式與 WAL v2 見
[docs/WRITE_BATCH.md](docs/WRITE_BATCH.md)。

### 範例：離線筆記 CLI

`itoukv_notes` 是獨立的範例執行檔，使用 `Store` 儲存筆記、以 `WriteBatch`
原子更新筆記與標籤索引，不影響通用的 `itoukv` CLI。Windows 路徑如下；Linux
單組態建置時將 `build/Release/` 改為 `build/`。

```powershell
"First note" | .\build\Release\itoukv_notes.exe .\notes.db add intro "我的筆記" - work
.\build\Release\itoukv_notes.exe .\notes.db show intro
.\build\Release\itoukv_notes.exe .\notes.db list work
.\build\Release\itoukv_notes.exe .\notes.db edit intro "更新標題" .\note.txt personal
.\build\Release\itoukv_notes.exe .\notes.db export .\notes.tsv
.\build\Release\itoukv_notes.exe .\notes-copy.db import .\notes.tsv
.\build\Release\itoukv_notes.exe .\notes.db delete intro
.\build\Release\itoukv_notes.exe .\notes.db compact
```

介面為 `itoukv_notes <資料庫目錄> <命令> ...`：`add/edit <id> <title> <file|-> [tag ...]`、
`show <id>`、`list [tag]`、`delete <id>`、`export/import <file>` 和 `compact`。
`-` 可從 stdin 讀取內文；`edit` 的標籤清單會取代原有標籤。ID／標籤只接受
1～80 個 ASCII 英數字、`_` 或 `-`；標題最多 4 KiB，內文可以包含換行及
任意位元組，完整筆記仍受 16 MiB value 上限限制。`show` 先輸出標題和標籤，
再原樣輸出內文；`list` 以 ID 排序、每行顯示 ID、標題與標籤。每個命令都會
重新開啟資料庫，找不到筆記時 exit code 為 2。

匯出檔是 `ITOUKV_NOTES_V1` 標頭加上逐行
`id<TAB>title_hex<TAB>body_hex<TAB>comma-separated-tags`；hex 可無損保存
換行、NUL 或非 UTF-8 內文。匯入會先驗證整份檔案並拒絕重複／已存在的 ID，
然後依 `WriteBatch` 上限分批提交；**每批原子提交，但整份匯入不是單一交易**。
匯入只接受檔案路徑，以便在寫入前完成驗證。請勿在匯入期間改動該檔案。

## 持久化契約

每個 commit（單筆或整批）依序執行：

```text
驗證參數 → 編碼紀錄 → 寫 header → 寫 payload → 寫 checksum
                                                   ↓
                      回傳成功 ← 更新記憶體 ← fsync / FlushFileBuffers
```

- **已回覆成功的修改**：完整 WAL 已交給作業系統的持久化 flush，重啟後應可恢復。
- **尚未回覆的修改**：可能存在，也可能不存在。尤其 flush 完成、回覆送出之前死亡，修改仍會存在。
- **寫入期間拋出例外**：提交結果不明；此 `Store` 進入 poisoned 狀態，拒絕讀寫，必須關閉並重新開啟。
- **不完整尾筆**：最後一筆 header 不足 32 bytes，或有效 header 宣告的 body 尚未寫完，恢復時截掉該筆並同步截斷。
- **完整紀錄損毀**：checksum、序號、長度或操作碼不合法就拒絕開啟，保留原始檔案。包含最後一筆完整紀錄，避免把損毀誤當成可丟棄的尾筆。

這份契約假設底層檔案系統／裝置遵守 flush，且沒有外部程式修改資料庫。POSIX 另同步新建目錄的父目錄與 WAL 所在目錄。Windows 同步檔案內容，但沒有一般化的 POSIX directory-fsync 對應，因此**新建目錄／檔名在硬斷電後的保留不在本版的跨平台保證內**。測試包含程序直接終止與人工截斷，不代表實測過硬斷電、控制器快取或所有檔案系統。

完整格式見 [docs/WAL_FORMAT.md](docs/WAL_FORMAT.md)。

## SSTable、查詢與 compaction

```text
put/delete → 同步 WAL → MemTable（新值或 tombstone）
get        → MemTable → Bloom filter → 稀疏索引 → SSTable row
scan       → MemTable + SSTable 有序串流合併，較新的版本優先
compact    → 合併 live data → 新 SSTable + 新空 WAL → 發布 CURRENT
```

SSTable 的值留在磁碟，RAM 保存每 16 筆一個索引項目、約 10 bits/key 的 Bloom filter，以及上次 compaction 後的 MemTable。查詢命中 Bloom negative 就不用讀取 row；filter 允許 false positive。Row、header 和整個 SSTable 各有 checksum。開啟時串流驗證整個 SSTable checksum，因此啟動仍需讀取該檔案全部位元組，並非 O(1)。

Compaction 持有 Store mutex，依序：

1. 串流寫入完整新 SSTable、flush 並重新驗證。
2. 建立新空 WAL，flush，POSIX 同步所在目錄。
3. 寫入帶 checksum 的 `CURRENT.next` 並 flush。
4. 同目錄 rename 原子替換 `CURRENT`，POSIX 再同步目錄。
5. 切換記憶體狀態與檔案 handle，最後才刪除舊世代檔案。

發布前死亡會繼續讀舊 SSTable + 舊 WAL；發布後死亡會讀新 SSTable + 新 WAL。兩者代表相同的已確認 logical state。未被 manifest 指向的候選檔案不參與恢復，下次相同世代 compaction 可覆寫候選檔；發生在發布後的 crash 也可能留下無用舊檔，不影響查詢，本版沒有通用 orphan 掃除器。詳細格式與故障矩陣見 [docs/SSTABLE_FORMAT.md](docs/SSTABLE_FORMAT.md)。

`stats` 顯示 sequence、checkpoint、generation、WAL/SSTable 大小、MemTable entry 數量、Bloom checks/negatives 及 table rows read。查詢計數自本次開啟起算，也包含重放 WAL 或更新 live-state 計數時的內部 lookup。

## 驗證

`ctest` 包含兩個 suite：

- C++ 儲存測試（17 項）：1,200 次單筆操作及 300 批操作與參考模型比對、重啟／compaction、binary data、排序、逐 byte 截斷／損毀、sequence、最大 key/value／32 MiB batch、poisoned 狀態、多執行緒與批次可見性、Bloom、range merge、tombstone 回收、batch copy/move。
- Node.js 程序測試（33 項）：真實 CLI、UTF-8、binary data、獨立 Node/zlib 格式 oracle、超限與 CRC 正確但格式錯誤的紀錄、單筆／批次／compaction 故障階段、跨程序鎖與強制終止後恢復。

Fault hook 由測試顯式注入，正式 CLI 沒有環境變數觸發的隱藏故障模式。

```sh
# Linux 無 CMake 時也能驗證核心
g++ -std=c++20 -O2 -pthread -Iinclude src/store.cpp tests/storage_test.cpp -o itoukv_tests
./itoukv_tests

# GCC / Clang 的 ASan + UBSan 建置
cmake -S . -B build-sanitize -DCMAKE_BUILD_TYPE=Debug -DITOUKV_SANITIZE=ON
cmake --build build-sanitize --parallel
ctest --test-dir build-sanitize --output-on-failure
```

## Benchmark

```powershell
.\build\Release\itoukv_bench.exe 1000
.\build\Release\itoukv_batch_bench.exe 1000
```

建立獨立暫存資料庫，量測 128-byte value、每次同步的寫入 throughput / p50 / p95、MemTable 讀取、WAL 恢復、compaction、SSTable 查詢／恢復、Bloom false positives，輸出 JSON，結束後清除自己的資料。另提供不含檔案系統 journal／metadata 的近似寫入放大。

SSTable 查詢使用 warm OS file cache，MemTable 查詢是記憶體操作；兩者皆不代表冷磁碟隨機讀取。不同同步模式、資料量、檔案系統或測量階段的結果不宜直接比較。SSTable 基線見 [docs/EXPERIMENTS.md](docs/EXPERIMENTS.md)，批次寫入比較見 [docs/BATCH_EXPERIMENTS.md](docs/BATCH_EXPERIMENTS.md)。

## 接下來的里程碑

1. 多 SSTable／level 的查詢合併，降低每次全量 compaction 的寫入放大。
2. MemTable 大小門檻與背景 compaction，縮短持有鎖的時間。
3. Block cache、block 級 checksum 與 lazy verification 的可選取捨。
4. 跨請求 group commit、MVCC 與更完整的混合 workload。

目前每次 compaction 全量重寫一個 SSTable，所有操作以同一把 mutex 序列化；MemTable／WAL 直到手動 `compact` 才縮小。WriteBatch 提供原子寫入，但沒有 read-modify-write transaction、MVCC、多層 LSM 或背景 compaction。

## 結構

```text
include/itoukv/store.hpp  公開 API 與故障點
src/store.cpp            OS I/O、CRC32、WAL、SSTable、Bloom、manifest、合併與恢復
src/cli.cpp              UTF-8 / hex CLI
tests/                   模型、損毀、程序故障驗證
bench/                   JSON benchmark
docs/                    格式與設計
```

MIT License.
