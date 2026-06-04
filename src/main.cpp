#include <Arduino.h>
#include <SPI.h>
#include <MFRC522.h>
#include <U8g2lib.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>
#include <stdio.h>
#include "secrets.h"

// ============================================================== 
// 專案邏輯總覽
// --------------------------------------------------------------
// [資料流]
// RC522 讀卡 -> UID 轉碼 (HEX/DEC) -> Serial/OLED 顯示 -> 事件入佇列 -> 背景上傳 Google Sheet
//
// [顯示策略]
// 1) 開機先顯示待機畫面。
// 2) 偵測到新卡後顯示卡片資料。
// 3) 卡片移開後不清畫面，保留最後一次結果。
// 4) 下一張卡進入時才刷新顯示內容。
//
// [效能策略]
// 1) 使用固定緩衝區，避免 String 動態配置。
// 2) HEX/DEC 字串在 loop 內只計算一次，供 Serial 與 OLED 共用。
// 3) 上傳採佇列與重試，減少網路波動對刷卡反應速度的影響。
// 4) 同 UID 短時間去抖動，避免重複感應造成重複寫入與畫面閃動。
// ============================================================== 

// ── RC522 接腳定義 ──────────────────────────────────────────
// SS 與 RST 可依實際接線調整，需與硬體一致。
#define RC522_SS_PIN  5
#define RC522_RST_PIN 4

// ── OLED SH1106 (I2C 硬體) ──────────────────────────────────
// 模組背面跳線焊在 0x78 側 → 7-bit 位址 0x3C (U8g2 預設值)
// 若跳線改焊至 0x7A 側，需呼叫 u8g2.setI2CAddress(0x7A)
// 本專案採用全緩衝模式（_F_），每次先在記憶體繪圖，再一次送到螢幕。
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

// ── MFRC522 物件 ────────────────────────────────────────────
// 建立 RC522 讀卡器物件，並帶入 SS / RST 腳位。
MFRC522 rfid(RC522_SS_PIN, RC522_RST_PIN);

// ── 常數 ────────────────────────────────────────────────────
// 序列埠鮑率，需與 Serial Monitor 設定一致。
static const uint32_t SERIAL_BAUD   = 115200;
// UID 字串緩衝區大小：10-byte UID 之 HEX 需最多 30 字元（含空格與結尾字元）。
static const size_t UID_HEX_BUF_LEN = 32;
// 10-byte UID 十進制最大約 25 位數 + 字串結尾字元。
static const size_t UID_DEC_BUF_LEN = 26;
// Google Sheet 上傳 payload 緩衝區。
static const size_t JSON_PAYLOAD_LEN = 192;
// 上傳除錯輸出開關：true 顯示 redirect 與額外診斷，false 僅保留必要資訊。
static const bool SHEET_UPLOAD_DEBUG = false;
// 上傳佇列大小（避免讀卡流程被網路延遲阻塞）。
static const uint8_t UPLOAD_QUEUE_CAPACITY = 8;
// 上傳失敗重試次數（不含第一次）。
static const uint8_t UPLOAD_MAX_RETRY = 2;
// 重試等待毫秒數。
static const uint32_t UPLOAD_RETRY_DELAY_MS = 1500;
// HTTP 連線/回應逾時。
static const uint16_t HTTP_CONNECT_TIMEOUT_MS = 1500;
static const uint16_t HTTP_READ_TIMEOUT_MS = 2500;
// 同一張卡短時間重複感應去抖（避免重複處理）。
static const uint32_t UID_DEBOUNCE_MS = 350;

// NTP 設定（台灣時區 UTC+8）。
static const long GMT_OFFSET_SEC = 8 * 3600;
static const int DST_OFFSET_SEC  = 0;

struct UploadItem {
    // 產生事件當下的本地時間字串（已格式化）。
    char dateTime[32];
    // UID HEX（原始值，如 "08 CF E7 42"）。
    char uidHex[UID_HEX_BUF_LEN];
    // UID DEC（大端序十進制值）。
    char uidDec[UID_DEC_BUF_LEN];
    // 當前已重試次數。
    uint8_t retryCount;
    // 下一次允許重試的時間點（millis）。
    uint32_t nextRetryAt;
};

// 環形佇列：head 指向待送首筆，tail 指向下一個可寫入位置。
static UploadItem uploadQueue[UPLOAD_QUEUE_CAPACITY];
static uint8_t uploadHead = 0;
static uint8_t uploadTail = 0;
static uint8_t uploadCount = 0;

// 去抖動狀態：記住最近一次 UID 與時間，抑制過快重複觸發。
static char lastUidHex[UID_HEX_BUF_LEN] = {0};
static uint32_t lastUidReadAt = 0;

// ── 函式宣告 ────────────────────────────────────────────────
// showWaiting()：顯示開機待機畫面。
// showUID()：顯示卡片 UID（HEX + DEC）與底部等待提示。
// uidToHexCString()：將 UID 位元組陣列轉為大寫 HEX C 字串。
// uidToDecCString()：將 UID 視為 big-endian 無號整數轉十進制 C 字串。
void showWaiting();
void showUID(const char *hexStr, const char *decStr);
void drawWiFiStatus();
void uidToHexCString(const MFRC522::Uid &uid, char *out, size_t outSize);
void uidToDecCString(const MFRC522::Uid &uid, char *out, size_t outSize);
void connectWiFi();
void syncTimeByNTP();
void getCurrentDateTime(char *out, size_t outSize);
bool uploadUIDToSheet(const char *dateTimeStr, const char *hexStr, const char *decStr);
void enqueueUpload(const char *hexStr, const char *decStr);
void processUploadQueue();

// ════════════════════════════════════════════════════════════
void setup() {
    // 初始化序列埠，用於除錯與顯示讀卡結果。
    Serial.begin(SERIAL_BAUD);

    // 啟動 SPI 匯流排，提供 RC522 通訊。
    SPI.begin();

    // 初始化 RC522 晶片，設定為可讀卡狀態。
    rfid.PCD_Init();

    // 初始化 OLED 顯示器。
    u8g2.begin();

    // 設定預設字型（後續會在不同區塊切換字型）。
    u8g2.setFont(u8g2_font_6x12_tf);

    // 連線 WiFi
    connectWiFi();

    // 同步 NTP 時間，供 UID 上傳時附帶日期時間。
    syncTimeByNTP();

    Serial.println("RFID Reader ready.");

    // 開機先顯示待機畫面，提示使用者可開始刷卡。
    showWaiting();
}

// ════════════════════════════════════════════════════════════
// WiFi 連線
// 嘗試連線至 secrets.h 中定義的 SSID，最多等待 10 秒。
void connectWiFi() {
    Serial.printf("Connecting to WiFi: %s\n", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    const uint32_t timeout = 10000;
    const uint32_t start   = millis();

    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - start >= timeout) {
            Serial.println("WiFi connection timed out.");
            return;
        }
        delay(500);
        Serial.print('.');
    }

    Serial.println();
    Serial.print("WiFi connected. IP: ");
    Serial.println(WiFi.localIP());
}

// ────────────────────────────────────────────────────────────
// NTP 同步（嘗試最多 8 秒），成功後可用 localtime() 取得本地時間。
void syncTimeByNTP() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("Skip NTP sync: WiFi not connected.");
        return;
    }

    configTime(GMT_OFFSET_SEC, DST_OFFSET_SEC, "pool.ntp.org", "time.nist.gov");

    time_t now = 0;
    const uint32_t start = millis();
    while (now < 1700000000) {
        now = time(nullptr);
        if (millis() - start > 8000) {
            Serial.println("NTP sync timed out.");
            return;
        }
        delay(200);
    }
    Serial.println("NTP time synced.");
}

// ────────────────────────────────────────────────────────────
// 產生日期時間字串（格式：yyyy/mm/dd hh:mm:ss）。
void getCurrentDateTime(char *out, size_t outSize) {
    if (outSize == 0) {
        return;
    }

    time_t now = time(nullptr);
    if (now < 1700000000) {
        snprintf(out, outSize, "1970/01/01 00:00:00");
        return;
    }

    struct tm localTm;
    localtime_r(&now, &localTm);
    strftime(out, outSize, "%Y/%m/%d %H:%M:%S", &localTm);
}

// ────────────────────────────────────────────────────────────
// 單次上傳執行器：嘗試將一筆資料送到 Google Apps Script Webhook。
// 欄位：datetime、uid_raw、uid_big_endian。
// 回傳值：true=本次送達成功(2xx)，false=失敗（可交由佇列重試）。
bool uploadUIDToSheet(const char *dateTimeStr, const char *hexStr, const char *decStr) {
    if (WiFi.status() != WL_CONNECTED) {
        return false;
    }

    if (strlen(SHEET_WEBHOOK_URL) == 0 || strstr(SHEET_WEBHOOK_URL, "replace-me") != nullptr) {
        Serial.println("Sheet upload skipped: SHEET_WEBHOOK_URL not configured.");
        return false;
    }

    char payload[JSON_PAYLOAD_LEN];
    snprintf(
        payload,
        sizeof(payload),
        "{\"datetime\":\"%s\",\"uid_raw\":\"%s\",\"uid_big_endian\":\"%s\"}",
        dateTimeStr,
        hexStr,
        decStr
    );

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    if (!http.begin(client, SHEET_WEBHOOK_URL)) {
        Serial.println("Sheet upload failed: begin() failed.");
        return false;
    }
    http.setConnectTimeout(HTTP_CONNECT_TIMEOUT_MS);
    http.setTimeout(HTTP_READ_TIMEOUT_MS);

    // Google Apps Script 可能先回 30x。
    // 301/302/303 依常見行為改為 GET；307/308 才保留原 POST。
    // 為了正確判讀流程，會分別記錄 initial 與 follow 狀態碼。
    const char *headerKeys[] = {"Location"};
    http.collectHeaders(headerKeys, 1);

    http.addHeader("Content-Type", "application/json");
    int initialHttpCode = http.POST(reinterpret_cast<uint8_t *>(payload), strlen(payload));

    if (initialHttpCode == 301 || initialHttpCode == 302 || initialHttpCode == 303 || initialHttpCode == 307 || initialHttpCode == 308) {
        String redirectUrl = http.header("Location");
        if (SHEET_UPLOAD_DEBUG) {
            Serial.print("Sheet redirect to: ");
            Serial.println(redirectUrl);
        }

        http.end();

        if (redirectUrl.length() > 0) {
            WiFiClientSecure redirectedClient;
            redirectedClient.setInsecure();

            HTTPClient redirectedHttp;
            if (!redirectedHttp.begin(redirectedClient, redirectUrl)) {
                Serial.println("Sheet upload failed: redirect begin() failed.");
                return false;
            }
            redirectedHttp.setConnectTimeout(HTTP_CONNECT_TIMEOUT_MS);
            redirectedHttp.setTimeout(HTTP_READ_TIMEOUT_MS);

            int followHttpCode = 0;
            if (initialHttpCode == 307 || initialHttpCode == 308) {
                redirectedHttp.addHeader("Content-Type", "application/json");
                followHttpCode = redirectedHttp.POST(reinterpret_cast<uint8_t *>(payload), strlen(payload));
            } else {
                followHttpCode = redirectedHttp.GET();
            }

            if (followHttpCode > 0) {
                Serial.print("Sheet upload HTTP code: ");
                Serial.print(initialHttpCode);
                Serial.print(" -> ");
                Serial.println(followHttpCode);
                if (followHttpCode < 200 || followHttpCode >= 300) {
                    String response = redirectedHttp.getString();
                    response.replace('\n', ' ');
                    if (response.length() > 180) {
                        response = response.substring(0, 180) + "...";
                    }
                    Serial.print("Sheet upload response: ");
                    Serial.println(response);
                }
            } else {
                Serial.print("Sheet upload failed: ");
                Serial.print("initial=");
                Serial.print(initialHttpCode);
                Serial.print(", follow=");
                Serial.println(redirectedHttp.errorToString(followHttpCode));
            }

            redirectedHttp.end();
            return (followHttpCode >= 200 && followHttpCode < 300);
        }

        return false;
    }

    if (initialHttpCode > 0) {
        Serial.print("Sheet upload HTTP code: ");
        Serial.println(initialHttpCode);
        if (initialHttpCode < 200 || initialHttpCode >= 300) {
            String response = http.getString();
            response.replace('\n', ' ');
            if (response.length() > 180) {
                response = response.substring(0, 180) + "...";
            }
            Serial.print("Sheet upload response: ");
            Serial.println(response);
        }
    } else {
        Serial.print("Sheet upload failed: ");
        Serial.println(http.errorToString(initialHttpCode));
    }

    http.end();
    return (initialHttpCode >= 200 && initialHttpCode < 300);
}

// ────────────────────────────────────────────────────────────
// 將 UID 讀取事件加入上傳佇列，避免當前刷卡流程被網路請求阻塞。
void enqueueUpload(const char *hexStr, const char *decStr) {
    if (uploadCount >= UPLOAD_QUEUE_CAPACITY) {
        // 佇列滿時丟棄最舊資料，保留最新刷卡事件。
        uploadHead = (uploadHead + 1) % UPLOAD_QUEUE_CAPACITY;
        uploadCount--;
        if (SHEET_UPLOAD_DEBUG) {
            Serial.println("Sheet queue full, drop oldest item.");
        }
    }

    UploadItem &item = uploadQueue[uploadTail];
    getCurrentDateTime(item.dateTime, sizeof(item.dateTime));
    strncpy(item.uidHex, hexStr, sizeof(item.uidHex) - 1);
    item.uidHex[sizeof(item.uidHex) - 1] = '\0';
    strncpy(item.uidDec, decStr, sizeof(item.uidDec) - 1);
    item.uidDec[sizeof(item.uidDec) - 1] = '\0';
    item.retryCount = 0;
    item.nextRetryAt = 0;

    uploadTail = (uploadTail + 1) % UPLOAD_QUEUE_CAPACITY;
    uploadCount++;
}

// ────────────────────────────────────────────────────────────
// 逐筆處理上傳佇列：成功即出列，失敗則按次數重試。
void processUploadQueue() {
    if (uploadCount == 0) {
        return;
    }

    UploadItem &item = uploadQueue[uploadHead];
    if (item.nextRetryAt != 0 && static_cast<int32_t>(millis() - item.nextRetryAt) < 0) {
        return;
    }

    const bool success = uploadUIDToSheet(item.dateTime, item.uidHex, item.uidDec);
    if (success) {
        uploadHead = (uploadHead + 1) % UPLOAD_QUEUE_CAPACITY;
        uploadCount--;
        return;
    }

    if (item.retryCount < UPLOAD_MAX_RETRY) {
        item.retryCount++;
        item.nextRetryAt = millis() + UPLOAD_RETRY_DELAY_MS;
        if (SHEET_UPLOAD_DEBUG) {
            Serial.print("Sheet retry queued, attempt: ");
            Serial.println(item.retryCount);
        }
    } else {
        Serial.println("Sheet upload dropped after retries.");
        uploadHead = (uploadHead + 1) % UPLOAD_QUEUE_CAPACITY;
        uploadCount--;
    }
}

// ════════════════════════════════════════════════════════════
void loop() {
    // 非阻塞背景上傳：即使當前沒有刷卡，也持續嘗試送出佇列資料。
    // 這可把網路延遲從「讀卡主路徑」抽離，減少刷卡時卡頓。
    processUploadQueue();

    // 流程說明：
    // 1) PICC_IsNewCardPresent()：檢查是否有「新卡」進入天線範圍。
    // 2) PICC_ReadCardSerial()：若有卡，再嘗試讀取 UID。
    // 只要任一步驟失敗就直接返回，避免無效處理。
    if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) {
        return;
    }

    // UID 轉碼只做一次：
    // 後續 Serial 與 OLED 共用同一份結果，避免重複運算。
    char hexStr[UID_HEX_BUF_LEN];
    char decStr[UID_DEC_BUF_LEN];
    uidToHexCString(rfid.uid, hexStr, sizeof(hexStr));
    uidToDecCString(rfid.uid, decStr, sizeof(decStr));

    const uint32_t nowMs = millis();
    // 去抖動：同一 UID 在極短時間內重複觸發時，直接跳過後續流程。
    // 目的：
    // 1) 降低同卡停留在天線區時的重複顯示與重複上傳
    // 2) 提升高頻刷卡時的有效吞吐
    if (strcmp(hexStr, lastUidHex) == 0 && (nowMs - lastUidReadAt) < UID_DEBOUNCE_MS) {
        rfid.PICC_HaltA();
        rfid.PCD_StopCrypto1();
        return;
    }
    strncpy(lastUidHex, hexStr, sizeof(lastUidHex) - 1);
    lastUidHex[sizeof(lastUidHex) - 1] = '\0';
    lastUidReadAt = nowMs;

    // Serial 輸出：提供開發/除錯時觀察資料。
    Serial.println("Card detected!");
    Serial.print("UID HEX: "); Serial.println(hexStr);
    Serial.print("UID DEC:(Big Endian) "); Serial.println(decStr);

    // OLED 顯示（含底部等待提示）。
    // showUID() 內部會先 clearBuffer()，因此每次新卡都會覆蓋舊資訊。
    showUID(hexStr, decStr);

    // 將每一筆 UID 讀取結果放入上傳佇列（立即返回，不阻塞感應流程）。
    enqueueUpload(hexStr, decStr);

    // 讓目前卡片進入 Halt，結束本次交易流程，避免重複觸發。
    rfid.PICC_HaltA();

    // 關閉加密狀態（若有啟用驗證流程時更重要），讓讀卡器回到乾淨狀態。
    rfid.PCD_StopCrypto1();

    // 不清除畫面，保持顯示直到下一張卡感應
}

// ════════════════════════════════════════════════════════════
// 待機畫面
void showWaiting() {
    // 清除緩衝區，避免舊畫面殘留。
    u8g2.clearBuffer();

    // 顯示標題（粗體較醒目）。
    u8g2.setFont(u8g2_font_8x13B_tf);
    u8g2.drawStr(10, 20, "RFID Reader");

    // 顯示待機提示。
    u8g2.setFont(u8g2_font_6x12_tf);
    u8g2.drawStr(20, 42, "Waiting...");

    // 右上角第一行顯示 WiFi 狀態。
    drawWiFiStatus();

    // 將緩衝區一次送出到 OLED。
    u8g2.sendBuffer();
}

// ────────────────────────────────────────────────────────────
// UID 顯示畫面（HEX + DEC 大端序），底部固定顯示等待提示
void showUID(const char *hexStr, const char *decStr) {
    // 每次顯示新卡前先清空緩衝，確保完整覆蓋前一筆資料。
    u8g2.clearBuffer();

    // 第 1 區：HEX 標籤（小字）。
    u8g2.setFont(u8g2_font_5x7_tf);
    u8g2.drawStr(0, 8, "HEX:");

    // 第 2 區：HEX 值（較大字，提升可讀性）。
    u8g2.setFont(u8g2_font_7x13B_tf);
    u8g2.drawStr(0, 21, hexStr);

    // 第 3 區：DEC 標籤（註明 Big Endian）。
    u8g2.setFont(u8g2_font_5x7_tf);
    u8g2.drawStr(0, 31, "DEC:(Big Endian)");

    // 第 4 區：DEC 值。
    u8g2.setFont(u8g2_font_7x13B_tf);
    u8g2.drawStr(0, 44, decStr);

    // 第 5 區：分隔線，將資料區與狀態提示區分開。
    u8g2.drawHLine(0, 50, 128);

    // 底部狀態列：提示使用者可繼續刷下一張卡。
    u8g2.setFont(u8g2_font_5x7_tf);
    u8g2.drawStr(0, 62, "Wait another card:...");

    // 右上角第一行顯示 WiFi 狀態。
    drawWiFiStatus();

    // 完成所有繪圖後再一次更新螢幕，避免閃爍。
    u8g2.sendBuffer();
}

// ────────────────────────────────────────────────────────────
// 在 OLED 右上角第一行（y=7）繪製 WiFi 連線狀態。
// 已連線：「wifi:<IP>」；未連線：「wifi:failure」。
// 使用 getStrWidth() 動態右對齊，不依賴固定像素寬度。
void drawWiFiStatus() {
    u8g2.setFont(u8g2_font_5x7_tf);

    char label[32];
    if (WiFi.status() == WL_CONNECTED) {
        // 取得 IP 字串（格式如 192.168.1.100）
        IPAddress ip = WiFi.localIP();
        snprintf(label, sizeof(label), "WiFi:%d.%d.%d.%d",
                 ip[0], ip[1], ip[2], ip[3]);
    } else {
        snprintf(label, sizeof(label), "WiFi:failure");
    }

    int16_t x = static_cast<int16_t>(128 - u8g2.getStrWidth(label));
    if (x < 0) {
        x = 0;
    }
    u8g2.drawStr(x, 7, label);
}

// ────────────────────────────────────────────────────────────
// 將 UID 轉換為大寫 HEX 字串，bytes 以空格分隔。
// 例：A3 F2 01 5B
void uidToHexCString(const MFRC522::Uid &uid, char *out, size_t outSize) {
    if (outSize == 0) {
        return;
    }

    out[0] = '\0';
    size_t pos = 0;
    for (uint8_t i = 0; i < uid.size; i++) {
        int written = snprintf(
            out + pos,
            outSize - pos,
            (i == 0) ? "%02X" : " %02X",
            uid.uidByte[i]
        );

        if (written < 0) {
            out[pos] = '\0';
            return;
        }

        size_t w = static_cast<size_t>(written);
        if (w >= (outSize - pos)) {
            // 緩衝區不足時：
            // 1) 保留目前可寫入內容
            // 2) 強制加上字串結尾字元，避免越界或髒資料
            out[outSize - 1] = '\0';
            return;
        }

        pos += w;
    }
}

// ────────────────────────────────────────────────────────────
// 將 UID 轉換為大端序十進制字串。
// 實作採用「十進制位數陣列」而非 uint64_t，原因如下：
// 1) MFRC522 可能讀到 10-byte UID。
// 2) 10-byte 整數可能超出 64-bit 範圍。
// 3) 以位數陣列運算可避免溢位，並保留正確十進制結果。
void uidToDecCString(const MFRC522::Uid &uid, char *out, size_t outSize) {
    if (outSize == 0) {
        return;
    }
    if (outSize == 1) {
        out[0] = '\0';
        return;
    }

    // 以十進制位數陣列表示結果（低位在前）。
    // 每處理一個 byte 就執行：result = result * 256 + byte。
    // 10-byte UID 最大約 25 位十進制，這裡保留 32 位作安全裕度。
    uint8_t digits[32] = {0};
    size_t digitsLen = 1;

    for (uint8_t i = 0; i < uid.size; i++) {
        uint16_t carry = uid.uidByte[i];

        for (size_t j = 0; j < digitsLen; j++) {
            // value 範圍安全：digit(0~9) * 256 + carry，使用 uint16_t 足夠承載。
            uint16_t value = static_cast<uint16_t>(digits[j]) * 256u + carry;
            digits[j] = static_cast<uint8_t>(value % 10u);
            carry = value / 10u;
        }

        // 將剩餘進位轉成新的十進制位數。
        while (carry > 0 && digitsLen < (sizeof(digits) / sizeof(digits[0]))) {
            digits[digitsLen++] = static_cast<uint8_t>(carry % 10u);
            carry /= 10u;
        }
    }

    // 反向輸出（最高位到最低位）。
    size_t outPos = 0;
    for (size_t k = digitsLen; k > 0; k--) {
        if (outPos >= outSize - 1) {
            break;
        }
        out[outPos++] = static_cast<char>('0' + digits[k - 1]);
    }
    out[outPos] = '\0';
}