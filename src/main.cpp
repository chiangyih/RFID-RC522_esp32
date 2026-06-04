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
// RC522 讀卡 -> UID 轉碼 (HEX/DEC) -> Serial/OLED 同步輸出
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

// NTP 設定（台灣時區 UTC+8）。
static const long GMT_OFFSET_SEC = 8 * 3600;
static const int DST_OFFSET_SEC  = 0;

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
void uploadUIDToSheet(const char *hexStr, const char *decStr);

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
// 每次讀到 UID 後發送到 Google Apps Script Webhook。
// 欄位：datetime、uid_raw、uid_big_endian。
void uploadUIDToSheet(const char *hexStr, const char *decStr) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("Sheet upload skipped: WiFi disconnected.");
        return;
    }

    if (strlen(SHEET_WEBHOOK_URL) == 0 || strstr(SHEET_WEBHOOK_URL, "replace-me") != nullptr) {
        Serial.println("Sheet upload skipped: SHEET_WEBHOOK_URL not configured.");
        return;
    }

    char dateTimeStr[32];
    getCurrentDateTime(dateTimeStr, sizeof(dateTimeStr));

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
        return;
    }

    // Google Apps Script 可能回傳 30x。
    // 301/302/303 依慣例改用 GET；307/308 才保留原方法（POST）。
    const char *headerKeys[] = {"Location"};
    http.collectHeaders(headerKeys, 1);

    http.addHeader("Content-Type", "application/json");
    int httpCode = http.POST(reinterpret_cast<uint8_t *>(payload), strlen(payload));

    if (httpCode == 301 || httpCode == 302 || httpCode == 303 || httpCode == 307 || httpCode == 308) {
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
                return;
            }

            if (httpCode == 307 || httpCode == 308) {
                redirectedHttp.addHeader("Content-Type", "application/json");
                httpCode = redirectedHttp.POST(reinterpret_cast<uint8_t *>(payload), strlen(payload));
            } else {
                httpCode = redirectedHttp.GET();
            }

            if (httpCode > 0) {
                Serial.print("Sheet upload HTTP code: ");
                Serial.println(httpCode);
                if (httpCode < 200 || httpCode >= 300) {
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
                Serial.println(redirectedHttp.errorToString(httpCode));
            }

            redirectedHttp.end();
            return;
        }
    }

    if (httpCode > 0) {
        Serial.print("Sheet upload HTTP code: ");
        Serial.println(httpCode);
        if (httpCode < 200 || httpCode >= 300) {
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
        Serial.println(http.errorToString(httpCode));
    }

    http.end();
}

// ════════════════════════════════════════════════════════════
void loop() {
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

    // Serial 輸出：提供開發/除錯時觀察資料。
    Serial.println("Card detected!");
    Serial.print("UID HEX: "); Serial.println(hexStr);
    Serial.print("UID DEC:(Big Endian) "); Serial.println(decStr);

    // OLED 顯示（含底部等待提示）。
    // showUID() 內部會先 clearBuffer()，因此每次新卡都會覆蓋舊資訊。
    showUID(hexStr, decStr);

    // 將每一筆 UID 讀取結果上傳至 Google Sheet。
    uploadUIDToSheet(hexStr, decStr);

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