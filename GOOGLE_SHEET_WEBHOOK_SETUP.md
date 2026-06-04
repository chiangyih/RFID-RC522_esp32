# RFID UID 上傳 Google Sheet 詳細操作手冊

本文件說明如何建立 `SHEET_WEBHOOK_URL`，並完成從 ESP32 到 Google Sheet 的資料寫入流程。

目前韌體版本已最佳化為：

- 讀卡後先更新 OLED，再將上傳事件放入背景佇列。
- 佇列會背景送出，不阻塞主讀卡流程。
- 上傳失敗會重試（預設 2 次），同 UID 短時間會去抖動。

## 1. 目標與前置條件

### 1.1 目標

每次刷卡成功後，ESP32 會送出以下欄位到 Google Sheet：

- datetime
- uid_raw
- uid_big_endian

### 1.2 前置條件

- 已有 Google 帳號
- 已可上網
- 專案可正常編譯與上傳
- 專案已有 `include/secrets.h`（且已被 `.gitignore` 排除）

## 2. 建立 Google Sheet

1. 開啟 Google 雲端硬碟，新增 Google 試算表。
2. 將檔名改為你容易辨識的名稱，例如：`RFID_UID_Log`。
3. 在第一列建立欄位名稱：
   - A1: datetime
   - B1: uid_raw
   - C1: uid_big_endian
4. 確認目前工作表是你要寫入的分頁（預設通常是 Sheet1）。

## 3. 建立 Apps Script 專案

1. 在該試算表點選上方選單：擴充功能 > Apps Script。
2. 進入編輯器後，將預設內容全部刪除。
3. 從試算表網址複製 Spreadsheet ID（`/d/` 與 `/edit` 之間那段）。
4. 貼上以下程式碼（將 `SPREADSHEET_ID` 與 `SHEET_NAME` 改成你的設定）。

```javascript
// Webhook 入口：ESP32 會以 HTTP POST(JSON) 呼叫此函式
function doPost(e) {
  try {
    // 1) 基本設定
    // SPREADSHEET_ID 來自試算表網址 /d/ 與 /edit 之間
    // SHEET_NAME 是要寫入的分頁名稱
    var SPREADSHEET_ID = "請填你的試算表ID";
    var SHEET_NAME = "Sheet1";

    // 2) 開啟試算表並取得目標分頁
    // 使用 openById 比 getActiveSpreadsheet 更穩定，較不受執行上下文影響
    var ss = SpreadsheetApp.openById(SPREADSHEET_ID);
    var sheet = ss.getSheetByName(SHEET_NAME);
    if (!sheet) {
      // 目標分頁不存在時，直接拋錯供除錯
      throw new Error("Sheet not found: " + SHEET_NAME);
    }

    // 3) 取得 POST 內容
    // e.postData.contents 會是 ESP32 送來的 JSON 字串
    // 若內容異常，預設為空 JSON，避免 JSON.parse 直接崩潰
    var body = "{}";
    if (e && e.postData && e.postData.contents) {
      body = e.postData.contents;
    }

    // 4) 解析 JSON 欄位
    var data = JSON.parse(body);

    // 5) 準備寫入資料
    // datetime: 使用裝置傳入時間；若未提供則回退為伺服器當下時間
    // uid_raw: UID HEX 格式（例如 08 CF E7 42）
    // uid_big_endian: UID 十進制（Big Endian）
    var dt = data.datetime ? data.datetime : new Date();
    var uidRaw = data.uid_raw ? data.uid_raw : "";
    var uidBig = data.uid_big_endian ? data.uid_big_endian : "";

    // 6) 寫入一列到 Sheet
    // 欄位順序固定：datetime, uid_raw, uid_big_endian
    sheet.appendRow([dt, uidRaw, uidBig]);

    // 7) 回傳成功 JSON
    return ContentService
      .createTextOutput(JSON.stringify({ ok: true }))
      .setMimeType(ContentService.MimeType.JSON);
  } catch (err) {
    // 發生錯誤時回傳錯誤訊息，便於在 ESP32 或 Apps Script Logs 追查
    return ContentService
      .createTextOutput(JSON.stringify({ ok: false, error: String(err) }))
      .setMimeType(ContentService.MimeType.JSON);
  }
}
```

5. 點選儲存，專案名稱可設為 `RFID_UID_Logger`。

## 4. 部署 Web App 並取得 SHEET_WEBHOOK_URL

1. 點選右上角：部署 > 新增部署。
2. 類型選擇：網頁應用程式。
3. 設定如下：
   - 執行身分：我自己
   - 存取權限：知道連結的任何人
4. 點選部署。
5. 首次部署會要求授權，按流程完成授權。
6. 部署完成後，複製「網頁應用程式網址」。
7. 這個網址就是 `SHEET_WEBHOOK_URL`，通常會以 `/exec` 結尾。

## 5. 將 URL 填入專案設定

1. 打開 `include/secrets.h`。
2. 找到以下設定：

```c
#define SHEET_WEBHOOK_URL "https://script.google.com/macros/s/replace-me/exec"
```

3. 把網址替換為你剛剛部署取得的 Web App URL。
4. 確認 `include/secrets.h` 仍在 `.gitignore` 中，避免機密上傳。

## 6. 韌體上傳與功能驗證

1. 重新編譯並上傳韌體。
2. 開啟 Serial Monitor。
3. 刷一張卡，應看到：
   - UID HEX
   - UID DEC
  - `Sheet upload HTTP code: 200`（或其他 2xx）
  - 若有轉址，可能顯示 `Sheet upload HTTP code: 302 -> 200`
  - 若不是 2xx，會多看到 `Sheet upload response: ...` 伺服器回應片段
  - `Sheet redirect to: ...` 預設不顯示（僅 `SHEET_UPLOAD_DEBUG = true` 時顯示）
4. 回到 Google Sheet，確認有新增一列資料。

### 6.1 重要檢查（避免 302/500）

1. 確認韌體已使用最新程式碼（已內建 30x 轉址處理，301/302/303 會改以 GET 追蹤）。
2. 確認 `SHEET_WEBHOOK_URL` 使用 `/exec`，不要用 `/dev`。
3. 每次修改 Apps Script 程式碼後，需重新部署 Web App 版本。
4. 若不想序列埠過多訊息，可維持 `SHEET_UPLOAD_DEBUG = false`（預設）。
5. 背景佇列會持續送出資料，短暫斷網恢復後可自動重試送達。

## 7. 端到端測試建議

### 7.1 正常流程測試

- 連上 WiFi
- 刷卡
- Serial 顯示成功
- Sheet 新增一列

### 7.2 斷網流程測試

- 關閉 AP 或讓設備離線
- 刷卡
- Serial 應顯示上傳失敗或跳過訊息
- 程式應繼續運作，不當機

### 7.3 恢復網路測試

- 恢復 WiFi
- 再刷卡
- 新刷卡資料可正常寫入 Sheet

## 8. 常見問題與排除

### 問題 1：HTTP 403 / 401

可能原因：
- Web App 權限未開放給知道連結的人
- 使用了錯誤部署版本

排除方式：
- 重新部署，檢查存取權限
- 確認使用的是最新部署

### 問題 2：HTTP 302 或重導

可能原因：
- Google Apps Script 進行轉址（script.google.com -> script.googleusercontent.com）
- 使用到錯誤網址或非 `/exec` 版本

排除方式：
- 重新從部署頁複製 Web App URL
- 確認 URL 末段是 `/exec`
- 確認已上傳本專案最新版韌體（已內建 30x 轉址處理）

### 問題 3：HTTP 500（Apps Script 內部錯誤）

可能原因：
- Script 找不到目標分頁
- `getActiveSpreadsheet()` 取得不到預期試算表
- JSON 解析或欄位處理例外

排除方式：
- 改用本文件提供的 `openById()` 寫法
- 確認 `SPREADSHEET_ID` 與 `SHEET_NAME` 正確
- 在 Apps Script 編輯器查看「執行作業」錯誤細節

### 問題 4：Sheet 沒有新增資料

可能原因：
- `doPost(e)` 程式未儲存或不是最新部署
- 寫入的工作表不是預期分頁

排除方式：
- 儲存後重新部署
- 在程式中指定目標分頁，例如 `getSheetByName("Sheet1")`

### 問題 5：ESP32 顯示 WiFi 已連線但上傳失敗

可能原因：
- Webhook URL 未設定或仍是 `replace-me`
- TLS 或網路品質問題

排除方式：
- 檢查 `include/secrets.h` 的 URL
- 先用瀏覽器或 Postman 測試 webhook 是否可達

### 問題 6：快速刷卡時發現有少量資料未寫入

可能原因：
- 佇列已滿（目前策略為丟棄最舊資料，保留最新事件）
- 單筆資料連續重試失敗後被丟棄

排除方式：
- 改善 WiFi 品質，降低上傳失敗率
- 依需求調大佇列容量與重試次數（需修改程式常數）

## 9. 安全性建議（可選）

目前設定是最小可用版本，建議後續加強：

1. 在 payload 加 token（例如 `api_key`），Apps Script 驗證後才寫入。
2. 限制可接受來源，避免公開網址被濫用。
3. 定期輪替 webhook 與 token。

## 10. 版本更新注意事項

只要修改 Apps Script 程式碼，就建議重新部署新版 Web App，確保新邏輯生效。

## 11. 快速除錯流程（建議照順序）

1. 先看 Serial 的 `Sheet upload HTTP code`。
2. 若非 2xx，再看 `Sheet upload response` 內容。
3. `302`：先確認是最新版韌體 + `/exec` URL。
4. `403`：檢查 Web App 權限是否為「知道連結的任何人」。
5. `500`：進 Apps Script「執行作業」查看堆疊，優先檢查 `SPREADSHEET_ID` / `SHEET_NAME`。
6. 若出現 `302 -> 200`，屬正常轉址後成功，不需額外處理。

---

若你要，我可以再補一份「含 token 驗證」的 Apps Script 與 ESP32 對應設定版本。