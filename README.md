# sysprogramming-midterm

這個 C 專案透過 TCP 查詢 Ubuntu 的系統資訊。server 監聽本機 `127.0.0.1`，每條連線由一個 `fork()` 建立的子行程處理。程式支援 `SYSINFO` 和 `PING`。請求及回應都以換行結尾，每條連線處理一次查詢。

## 程式結構

- `src/server.c` 接受 TCP 連線、建立子行程，並處理讀取逾時、斷線及子行程回收。
- `src/commands.c` 負責回應系統資訊查詢和 PING。
- `src/client.c` 解析命令列，送出一行請求並顯示回覆。
- `src/common.c` 提供兩端共用的收送、參數解析與除錯日誌，建置成 `libsysprogram.so`。
- `tests/test_suite.c` 驗證10 條同時連線及三組異常對照。

## 在 Ubuntu 執行

在第一個終端機編譯並啟動 server：

```sh
cd ~/sysprogramming-midterm
make
./build/debug1/server --port 39085 --debug
```

在第二個終端機查詢：

```sh
cd ~/sysprogramming-midterm
./build/debug1/client --port 39085 SYSINFO
./build/debug1/client --port 39085 PING
```

`SYSINFO` 會回傳作業系統、核心版本、架構、開機秒數、記憶體總量與空閒量（MiB），以及服務這次請求的子行程 PID。

| 選項 | 行為 |
| --- | --- |
| `--port 0`（server） | 由作業系統分配連接埠，從 `LISTEN` 訊息取得實際值。 |
| `--timeout-ms 5000`（server） | 等待完整請求的期限，預設 5 秒。 |
| `--baseline`（server） | 關閉三組要對照的防護。 |
| `--reply-delay-ms 650`（server） | 先送出 `OK `，間隔 650 ms 後送出回應內容，供斷線實驗使用。預設 0。 |
| `--debug`（兩端） | 執行時開啟 Debug 日誌。 |

## 建置與驗證

```sh
make test
make DEBUG=0
readelf -d build/debug1/server build/debug1/client | grep libsysprogram.so
```

`make test` 會編譯並執行 C 測試程式，使用動態配置的 loopback 連接埠，自動啟動與停止各測試 server。`DEBUG=1` 是預設值，可透過 `--debug` 開啟日誌。`DEBUG=0` 會在編譯時移除日誌程式碼，產物放在 `build/debug0/`。

## 報告與證據

- [期中專案報告](REPORT.md)
- [C 測試實測輸出](evidence/test-run.txt)
- [10 條連線的實測輸出圖](evidence/concurrency.png)
- [三組異常的實測輸出圖](evidence/robustness.png)
- [Debug 開關檢查](evidence/debug-check.txt)
- [封包擷取檔](evidence/sysinfo.pcap)與[封包清單](evidence/packet-summary.txt)
