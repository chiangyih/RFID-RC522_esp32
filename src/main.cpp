#include <Arduino.h>
#include <SPI.h>
#include <MFRC522.h>
#include <U8g2lib.h>
#include <stdio.h>

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

// ── 函式宣告 ────────────────────────────────────────────────
// showWaiting()：顯示開機待機畫面。
// showUID()：顯示卡片 UID（HEX + DEC）與底部等待提示。
// uidToHexCString()：將 UID 位元組陣列轉為大寫 HEX C 字串。
// uidToDecCString()：將 UID 視為 big-endian 無號整數轉十進制 C 字串。
void showWaiting();
void showUID(const char *hexStr, const char *decStr);
void uidToHexCString(const MFRC522::Uid &uid, char *out, size_t outSize);
void uidToDecCString(const MFRC522::Uid &uid, char *out, size_t outSize);

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

    Serial.println("RFID Reader ready.");

    // 開機先顯示待機畫面，提示使用者可開始刷卡。
    showWaiting();
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

    // 完成所有繪圖後再一次更新螢幕，避免閃爍。
    u8g2.sendBuffer();
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