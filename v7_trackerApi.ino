/*
====================================================================================
  SMART STUDENT TRACKER v6.3 - FULL TELEMETRY SNAPSHOT AT 165s

  Hardware:
  - ESP32-C3 Mini (MCU)
  - ADXL345 (I2C 0x53) - Motion detection, GPIO2 interrupt  [ALWAYS ON in sleep]
  - DS3231  (I2C 0x68) - RTC                                [ALWAYS ON in sleep]
  - EC200U  - LTE + GPS modem                               [POWERED OFF at 170s]
  - W25Q    - SPI Flash                                     [POWERED OFF at 170s]

  Active-cycle timeline (resets on every motion event):
  +--------------------------------------------------------------------------+
  |  0s ────────────────────── 165s ──── 170s ─────────────────────── 180s  |
  |  Normal operation            |         |                           |     |
  |  Modem + GPS running         |         |                           |     |
  |  Full telemetry every 15s    |         |                           |     |
  |                        SAVE FULL    POWER OFF               DEEP SLEEP  |
  |                        TELEMETRY    EC200U +                    |        |
  |                        to W25Q:     W25Q                        |        |
  |                        - RTC time   (graceful AT+QPOWD=1        |        |
  |                        - GPS fix     + PWRKEY fallback)         |        |
  |                        - Lat/Lon/Alt W25Q 0xB9 deep             |        |
  |                        - Sats/HDOP   power-down + SPI.end)      |        |
  |                        - CSQ signal                             |        |
  |                        - Carrier                                |        |
  |                        - MCC/MNC                                |        |
  |                        - Cell ID / TAC                          |        |
  +--------------------------------------------------------------------------+

  W25Q storage: each TelemetryRecord = 1 x 256-byte page.
    16Mbit (2MB) chip holds 8192 records = 8192 wake cycles before wrap.

  --- CHANGE LOG ---
  v6.0->v6.1  TX/RX swap fix, READY terminator, FLASH_CS boot fix, PWRKEY fix.
  v6.1->v6.2  Proper power-down at 170s: EC200U AT+QPOWD=1 + PWRKEY fallback,
              W25Q Deep Power-Down (0xB9) + SPI.end().
  v6.2->v6.3  165s checkpoint saves FULL telemetry (was GPS+CSQ only).
              TelemetryRecord now stores: RTC unix + h/m/s/d/m/y, GPS lat/lon/
              alt/HDOP/sats, LTE CSQ/carrier/network type/MCC/MNC/Cell ID/TAC.
              collectFullTelemetry() replaces fetchGPSData/fetchSignalStrength.
====================================================================================
*/

#include <Wire.h>
#include <SPI.h>
#include <RTClib.h>
#include <esp_sleep.h>

// ===== PIN DEFINITIONS =====
#define I2C_SDA      8
#define I2C_SCL      9
#define SPI_CLK      4
#define SPI_MISO     5
#define SPI_MOSI     6
#define FLASH_CS     7
#define MODEM_TX     20      // Modem's TX  → ESP RX
#define MODEM_RX     21      // Modem's RX  → ESP TX
#define MODEM_PWRKEY 10
#define ADXL_INT     GPIO_NUM_2
#define ADXL_ADDR    0x53

// ===== TIMING CONSTANTS =====
#define INACTIVITY_TIMEOUT  180000UL   // 180s – enter deep sleep
#define GPS_LOG_CHECKPOINT  165000UL   // 165s – save GPS/signal to flash
#define MODEM_SHUTDOWN_TIME 170000UL   // 170s – power off EC200U + W25Q
#define MOTION_THRESHOLD    35         // ADXL delta threshold (proven)

// ===== API CONFIGURATION =====
// IMPORTANT: The EC200U is on Airtel LTE (100.84.x.x) — it CANNOT reach
// 192.168.x.x (your local LAN). You need a publicly reachable address.
//
// Easiest option — ngrok tunnel (free, 5 min setup):
//   1. Download: https://ngrok.com/download
//   2. Run alongside your Node server: ngrok http 3000
//   3. Copy the Forwarding hostname e.g. "abc123.ngrok-free.app"
//   4. Paste it below (no https://) and set port to 80
//
// Alternatives: deploy to Railway/Render, or router port-forward + public IP.
// ---- OPTION A: webhook.site (connectivity test, no server needed) ----
// 1. Go to https://webhook.site  — copy your unique URL token
// 2. Set API_SERVER_ADDR to "webhook.site", API_SERVER_PORT to 80
// 3. Set API_ENDPOINT_PATH to "/your-unique-token"
//
// ---- OPTION B: Railway / Render (permanent deploy of your Node server) ----
// 1. Push your server folder to GitHub
// 2. Connect to https://railway.app  — deploys automatically, gives public URL
// 3. Set API_SERVER_ADDR to "your-app.railway.app", port 80
//
#define API_SERVER_ADDR     "trackerservertest-production.up.railway.app"
#define API_SERVER_PORT     8081
#define API_ENDPOINT_PATH   "/api/tracker/telemetry"
#define API_REQUEST_TIMEOUT 15000UL                   // 15s timeout for LTE

// ===== GLOBALS =====
int16_t x, y, z;
int16_t lastX, lastY, lastZ;
unsigned long lastMotionTime  = 0;
unsigned long lastGPSLogTime  = 0;
uint32_t flashWriteAddress    = 0x000000;
bool modemInitialized         = false;
bool modemShutdownTriggered   = false;
bool apiDataSentFlag          = false;  // Track if last telemetry was sent via API

RTC_DS3231 rtc;
bool rtcReady   = false;
bool modemReady = false;

// ===== FULL TELEMETRY RECORD (saved to W25Q at 165s) =====
// Everything that was visible on the live dashboard, packed into flash.
struct TelemetryRecord {
  // --- RTC timestamp ---
  uint32_t timestamp;          // Unix epoch (seconds)
  uint8_t  rtc_hour;
  uint8_t  rtc_min;
  uint8_t  rtc_sec;
  uint8_t  rtc_day;
  uint8_t  rtc_month;
  uint16_t rtc_year;

  // --- GPS / GNSS ---
  float    latitude;           // Decimal degrees
  float    longitude;          // Decimal degrees
  float    altitude;           // Metres (float — EC200U returns decimals)
  uint8_t  satellite_count;    // Active satellites
  float    hdop;               // Horizontal dilution of precision

  // --- LTE cellular ---
  uint8_t  signal_csq;         // CSQ 0-31 (99 = unknown)
  uint16_t mcc;                // Mobile Country Code  e.g. 404
  uint16_t mnc;                // Mobile Network Code  e.g. 97
  uint32_t cell_id;            // Cell ID (hex string parsed to uint32)
  uint16_t tac;                // Tracking Area Code (hex parsed to uint16)
  char     carrier[16];        // Operator name  e.g. "IND airtel\0"
  char     network_type[8];    // "LTE\0", "NR\0", etc.

  uint8_t  _pad[2];            // keep struct size a multiple of 4 bytes
};

// ===== STRING PARSER HELPER =====
String getValue(String data, char separator, int index) {
  int found = 0;
  int strIndex[] = {0, -1};
  int maxIndex = data.length() - 1;
  for (int i = 0; i <= maxIndex && found <= index; i++) {
    if (data.charAt(i) == separator || i == maxIndex) {
      found++;
      strIndex[0] = strIndex[1] + 1;
      strIndex[1] = (i == maxIndex) ? i + 1 : i;
    }
  }
  return found > index ? data.substring(strIndex[0], strIndex[1]) : "";
}

// ===== AT COMMAND HELPER =====
String sendATCommand(String command, const int timeout) {
  // Flush any leftover data in the buffer
  while (Serial1.available()) {
    Serial1.read();
    delayMicroseconds(100);
  }
  
  String response = "";
  delay(50);  // Ensure modem is ready
  
  // Send command
  Serial1.println(command);
  
  long int time = millis();
  while ((time + timeout) > millis()) {
    while (Serial1.available()) {
      char c = (char)Serial1.read();
      response += c;
      delayMicroseconds(50);
    }
    
    // Look for any response terminator
    if (response.indexOf("\r\nOK\r\n") != -1 ||
        response.indexOf("\nOK\n") != -1 ||
        response.indexOf("OK\r\n") != -1 ||
        response.indexOf("ERROR\r\n") != -1 ||
        response.indexOf("ERROR\n") != -1 ||
        response.indexOf("READY\r\n") != -1 ||
        response.indexOf("+CME ERROR") != -1) {
      break;
    }
    delay(5);
  }
  
  return response;
}

// ===== W25Q FLASH OPERATIONS =====
void flashWriteEnable() {
  digitalWrite(FLASH_CS, LOW);
  SPI.transfer(0x06);   // WREN
  digitalWrite(FLASH_CS, HIGH);
}

void flashWritePage(uint32_t address, uint8_t* data, uint16_t length) {
  flashWriteEnable();
  digitalWrite(FLASH_CS, LOW);
  SPI.transfer(0x02);                           // Page Program
  SPI.transfer((address >> 16) & 0xFF);
  SPI.transfer((address >>  8) & 0xFF);
  SPI.transfer( address        & 0xFF);
  for (uint16_t i = 0; i < length; i++) SPI.transfer(data[i]);
  digitalWrite(FLASH_CS, HIGH);
  delay(5);
}

void saveTelemetryToFlash(TelemetryRecord& rec) {
  // W25Q page = 256 bytes. TelemetryRecord must fit in one page.
  static_assert(sizeof(TelemetryRecord) <= 256,
                "TelemetryRecord exceeds one W25Q page (256 bytes)!");

  uint8_t buffer[sizeof(TelemetryRecord)];
  memcpy(buffer, &rec, sizeof(TelemetryRecord));
  flashWritePage(flashWriteAddress, buffer, sizeof(TelemetryRecord));
  Serial.printf("[W25Q] Record saved at 0x%06X  (%u bytes used / 256 per page)\n",
                flashWriteAddress, (unsigned)sizeof(TelemetryRecord));
  flashWriteAddress += 256;   // advance one full page regardless of record size
}

// ===== W25Q INIT / SHUTDOWN =====
void initFlash() {
  // Note: FLASH_CS is already HIGH from setup() pre-init.
  // SPI.begin() is safe to call here (or after a SPI.end()).
  pinMode(FLASH_CS, OUTPUT);
  digitalWrite(FLASH_CS, HIGH);
  SPI.begin(SPI_CLK, SPI_MISO, SPI_MOSI, -1);
  Serial.println("[W25Q] Ready");
}

// Proper W25Q power-down: send Deep Power-Down command (0xB9),
// then deassert CS and release the SPI bus. Current drops to <1µA.
void shutdownFlash() {
  Serial.println("[W25Q] Sending Deep Power-Down command (0xB9)...");
  digitalWrite(FLASH_CS, LOW);
  SPI.transfer(0xB9);           // Deep Power-Down instruction
  digitalWrite(FLASH_CS, HIGH);
  delay(5);                     // tDP = 3µs typical, 5ms is very safe
  SPI.end();                    // release MOSI/MISO/CLK pins
  // Hold CS HIGH so the chip stays in deep power-down
  pinMode(FLASH_CS, OUTPUT);
  digitalWrite(FLASH_CS, HIGH);
  Serial.println("[W25Q] Deep power-down — SPI bus released");
}

// ===== EC200U BOOT =====
void bootModem() {
  Serial.println("\n>>> Connecting to Cellular Modem...");

  // Reset pins to standard state if they were reconfigured by shutdown
  pinMode(MODEM_RX, INPUT);
  delay(50);

  // Serial1.begin(baud, config, RX_pin, TX_pin)
  // MODEM_TX (GPIO20) is modem's TX → ESP's RX → goes in RX slot
  // MODEM_RX (GPIO21) is modem's RX → ESP's TX → goes in TX slot
  Serial1.begin(115200, SERIAL_8N1, MODEM_TX, MODEM_RX);
  delay(100);
  while (Serial1.available()) Serial1.read();   // flush garbage

  // Smart-boot: check if modem is already awake
  String atTest = sendATCommand("AT", 1500);
  if (atTest.indexOf("OK") != -1) {
    Serial.println("    [ OK ] Modem already awake!");
    modemReady = true;
  } else {
    // Send PWRKEY LOW pulse to wake modem from power-down
    // EC200U: pulse PWRKEY LOW for ~500-1000ms to wake from power-down
    Serial.println("    Modem asleep/powered-down. Sending wake pulse (PWRKEY LOW)...");
    pinMode(MODEM_PWRKEY, OUTPUT);
    digitalWrite(MODEM_PWRKEY, HIGH);  // ensure HIGH first
    delay(100);
    digitalWrite(MODEM_PWRKEY, LOW);   // pulse LOW to wake
    delay(800);                        // EC200U requires ~500-800ms LOW pulse
    digitalWrite(MODEM_PWRKEY, HIGH);  // release back HIGH

    Serial.println("    Waiting 20s for modem OS to fully boot...");
    delay(20000);  // Increased from 15s to allow full service initialization

    // Retry AT command after boot with multiple attempts
    int atRetries = 0;
    while (atRetries < 5) {
      atTest = sendATCommand("AT", 2000);  // Increased timeout to 2s
      if (atTest.indexOf("OK") != -1) {
        Serial.println("    [ OK ] Modem booted successfully.");
        modemReady = true;
        break;
      }
      atRetries++;
      if (atRetries < 5) {
        Serial.printf("    [Retry %d/5] Modem not responding yet...\n", atRetries);
        delay(500);
      }
    }
    
    if (!modemReady) {
      Serial.println("    [FAIL] Modem not responding after boot pulse.");
      Serial.printf("    [DEBUG] AT response: %s\n", atTest.c_str());
    }
  }

  if (modemReady) {
    // Disable echo mode to ensure clean AT responses
    Serial.println("    Disabling echo mode...");
    sendATCommand("ATE0", 1000);
    delay(200);
    
    // SIM check with retry
    String simCheck = sendATCommand("AT+CPIN?", 2500);
    Serial.println(simCheck.indexOf("READY") != -1
                   ? "    [ OK ] SIM ready."
                   : "    [WARN] SIM missing or locked.");

    // Extra wait to allow modem to fully stabilize LTE connection
    Serial.println("    Stabilizing modem services (5s wait)...");
    delay(5000);

    // Start GPS engine (may fail initially if modem not fully ready)
    String gpsSetup = sendATCommand("AT+QGPS=1", 3000);
    if (gpsSetup.indexOf("OK") != -1 || gpsSetup.indexOf("504") != -1) {
      Serial.println("    [ OK ] GPS engine on.");
    } else {
      Serial.println("    [WARN] GPS engine failed to start — will retry.");
      delay(2000);
      String gpsRetry = sendATCommand("AT+QGPS=1", 3000);
      Serial.println(gpsRetry.indexOf("OK") != -1
                     ? "    [ OK ] GPS engine on (retry succeeded)."
                     : "    [WARN] GPS engine still failing.");
    }
  }

  modemShutdownTriggered = false;
}

// ===== EC200U PROPER POWER-DOWN =====
// Two-stage: graceful AT command first, PWRKEY hard-cut as fallback.
void shutdownModem() {
  if (!modemReady && modemShutdownTriggered) {
    Serial.println("[EC200U] Already shut down, skipping.");
    return;
  }

  Serial.println("[EC200U] Initiating graceful shutdown (AT+QPOWD=1)...");

  // Stage 1: graceful OS shutdown via AT command
  // EC200U responds with "POWERED DOWN" then goes silent
  Serial1.println("AT+QPOWD=1");
  unsigned long t = millis();
  bool graceful = false;
  String buf = "";
  while (millis() - t < 5000) {           // wait up to 5s
    while (Serial1.available()) buf += (char)Serial1.read();
    if (buf.indexOf("POWERED DOWN") != -1) {
      graceful = true;
      break;
    }
  }

  if (graceful) {
    Serial.println("[EC200U] Graceful shutdown confirmed (POWERED DOWN received).");
  } else {
    Serial.println("[EC200U] No response to AT+QPOWD=1 — using PWRKEY hard-cut...");
    // Stage 2: PWRKEY hard power-cut
    // EC200U: hold PWRKEY HIGH for ≥2000ms to force off
    pinMode(MODEM_PWRKEY, OUTPUT);
    digitalWrite(MODEM_PWRKEY, LOW);
    delay(50);
    digitalWrite(MODEM_PWRKEY, HIGH);
    delay(2500);                          // >2000ms = force off
    digitalWrite(MODEM_PWRKEY, LOW);
    Serial.println("[EC200U] PWRKEY hard-cut complete.");
  }

  // Release UART — free GPIO20/21 (no floating drive during deep sleep)
  Serial1.end();
  // Drive TX pin LOW so it doesn't float and tickle the modem
  pinMode(MODEM_RX, OUTPUT);
  digitalWrite(MODEM_RX, LOW);

  modemReady             = false;
  modemShutdownTriggered = true;
  Serial.println("[EC200U] UART released. Modem is OFF.");
}

// ===== FULL TELEMETRY COLLECTION — fills every field of TelemetryRecord =====

// -- RTC --
void collectRTC(TelemetryRecord& rec) {
  if (!rtcReady) return;
  DateTime now      = rtc.now();
  rec.timestamp     = now.unixtime();
  rec.rtc_hour      = now.hour();
  rec.rtc_min       = now.minute();
  rec.rtc_sec       = now.second();
  rec.rtc_day       = now.day();
  rec.rtc_month     = now.month();
  rec.rtc_year      = now.year();
  Serial.printf("[RTC]    %04d-%02d-%02d %02d:%02d:%02d  (unix %lu)\n",
                rec.rtc_year, rec.rtc_month, rec.rtc_day,
                rec.rtc_hour, rec.rtc_min,   rec.rtc_sec,
                (unsigned long)rec.timestamp);
}

// -- GPS --
void collectGPS(TelemetryRecord& rec) {
  if (!modemReady) return;
  
  // Retry logic: GPS may need multiple attempts after modem wake
  int gpsRetries = 0;
  while (gpsRetries < 3) {
    String resp = sendATCommand("AT+QGPSLOC=2", 4000);  // Increased timeout to 4s

    if (resp.indexOf("+CME ERROR: 516") != -1) {
      Serial.printf("[GPS]    Attempt %d/3 - Searching — no fix yet.\n", gpsRetries + 1);
      gpsRetries++;
      if (gpsRetries < 3) delay(2000);  // Wait 2s before retry
      continue;
    }
    
    if (resp.indexOf("+QGPSLOC:") == -1) {
      Serial.printf("[GPS]    Attempt %d/3 - No response from GPS engine.\n", gpsRetries + 1);
      gpsRetries++;
      if (gpsRetries < 3) delay(1000);  // Wait 1s before retry
      continue;
    }

    // Successfully got GPS data
    // +QGPSLOC: <UTC>,<lat>,<lon>,<hdop>,<alt>,<fix>,<cog>,<spkm>,<spkn>,<date>,<nsat>
    String d = resp.substring(resp.indexOf("+QGPSLOC:") + 9);
    String lat  = getValue(d, ',', 1);
    String lon  = getValue(d, ',', 2);
    String hdop = getValue(d, ',', 3);
    String alt  = getValue(d, ',', 4);
    String sats = getValue(d, ',', 10);
    sats.replace("\r",""); sats.replace("\n",""); sats.replace("OK",""); sats.trim();

    rec.latitude        = lat.toFloat();
    rec.longitude       = lon.toFloat();
    rec.altitude        = alt.toFloat();
    rec.hdop            = hdop.toFloat();
    rec.satellite_count = (uint8_t)sats.toInt();

    Serial.printf("[GPS]    Fix: %.6f, %.6f  Alt: %.1fm  HDOP: %.2f  Sats: %u\n",
                  rec.latitude, rec.longitude, rec.altitude, rec.hdop, rec.satellite_count);
    return;  // Success
  }
  
  // If all retries failed, log it
  Serial.println("[GPS]    Failed to get GPS fix after 3 attempts.");
}

// -- LTE signal + cell info --
void collectLTE(TelemetryRecord& rec) {
  if (!modemReady) return;

  // Signal strength (CSQ) with retry for post-wake stability
  String csqResp = sendATCommand("AT+CSQ", 2000);
  if (csqResp.indexOf("+CSQ:") != -1) {
    int s = csqResp.indexOf("+CSQ:") + 5;
    String v = csqResp.substring(s, csqResp.indexOf(",", s));
    v.trim();
    uint8_t csq_val = (uint8_t)v.toInt();
    
    // If CSQ is 99 (unknown) after wake, retry once
    if (csq_val == 99) {
      Serial.println("[LTE]    CSQ unknown (99) — retrying...");
      delay(1000);
      csqResp = sendATCommand("AT+CSQ", 2000);
      if (csqResp.indexOf("+CSQ:") != -1) {
        int s2 = csqResp.indexOf("+CSQ:") + 5;
        String v2 = csqResp.substring(s2, csqResp.indexOf(",", s2));
        v2.trim();
        csq_val = (uint8_t)v2.toInt();
      }
    }
    
    rec.signal_csq = csq_val;
    Serial.printf("[LTE]    CSQ: %u/31\n", rec.signal_csq);
  }

  // Carrier name with retry
  String cops = sendATCommand("AT+COPS?", 2500);
  int attempts = 0;
  while ((cops.indexOf("+COPS:") == -1 || cops.indexOf("\"\"") != -1) && attempts < 2) {
    Serial.printf("[LTE]    Carrier query attempt %d - retrying...\n", attempts + 1);
    delay(1500);
    cops = sendATCommand("AT+COPS?", 2500);
    attempts++;
  }
  
  int q1 = cops.indexOf("\""), q2 = cops.indexOf("\"", q1 + 1);
  if (q1 != -1 && q2 != -1) {
    String carrier = cops.substring(q1 + 1, q2);
    carrier.trim();
    strncpy(rec.carrier, carrier.c_str(), sizeof(rec.carrier) - 1);
    rec.carrier[sizeof(rec.carrier) - 1] = '\0';
    Serial.printf("[LTE]    Carrier: %s\n", rec.carrier);
  }

  // Serving cell engineering data (MCC, MNC, Cell ID, TAC) with retry
  String qeng = sendATCommand("AT+QENG=\"servingcell\"", 3000);
  attempts = 0;
  while ((qeng.indexOf("+QENG:") == -1 || qeng.indexOf("4,0") != -1) && attempts < 2) {
    // "4,0" means not registered - retry
    Serial.printf("[LTE]    Cell info attempt %d - not registered yet, retrying...\n", attempts + 1);
    delay(2000);
    qeng = sendATCommand("AT+QENG=\"servingcell\"", 3000);
    attempts++;
  }
  
  if (qeng.indexOf("+QENG:") != -1) {
    String cell = qeng.substring(qeng.indexOf("+QENG:"));

    // Field layout for LTE:
    // +QENG: "servingcell",<state>,<RAT>,<is_tdd>,<MCC>,<MNC>,<cellID>,<PCID>,
    //         <earfcn>,<freq_band>,<ul_bw>,<dl_bw>,<TAC>,<RSRP>,<RSRQ>,<RSSI>,
    //         <SINR>,<srxlev>
    String tech = getValue(cell, ',', 2);
    tech.replace("\"", ""); tech.trim();
    strncpy(rec.network_type, tech.c_str(), sizeof(rec.network_type) - 1);
    rec.network_type[sizeof(rec.network_type) - 1] = '\0';

    if (tech == "LTE") {
      String mcc_s      = getValue(cell, ',', 4);  mcc_s.trim();
      String mnc_s      = getValue(cell, ',', 5);  mnc_s.trim();
      String cellid_s   = getValue(cell, ',', 6);  cellid_s.trim();
      String tac_s      = getValue(cell, ',', 12); tac_s.trim();

      rec.mcc     = (uint16_t)mcc_s.toInt();
      rec.mnc     = (uint16_t)mnc_s.toInt();
      rec.cell_id = (uint32_t)strtoul(cellid_s.c_str(), nullptr, 16);  // hex string
      rec.tac     = (uint16_t)strtoul(tac_s.c_str(),    nullptr, 16);  // hex string

      Serial.printf("[LTE]    %s  MCC/MNC: %u/%u  Cell: 0x%X  TAC: 0x%X\n",
                    rec.network_type, rec.mcc, rec.mnc, rec.cell_id, rec.tac);
    }
  }
}

// Master collector — call this at 165s
void collectFullTelemetry(TelemetryRecord& rec) {
  memset(&rec, 0, sizeof(rec));
  collectRTC(rec);
  collectGPS(rec);
  collectLTE(rec);
}

// ===== API / HTTP FUNCTIONS =====

// Convert TelemetryRecord to JSON string (ArduinoJSON style, manual building for memory efficiency)
String telemetryToJSON(const TelemetryRecord& rec) {
  String json = "{";
  json += "\"timestamp\":" + String(rec.timestamp) + ",";
  json += "\"rtc_datetime\":\"" + String(rec.rtc_year) + "-" 
                                + String(rec.rtc_month < 10 ? "0" : "") + String(rec.rtc_month) + "-"
                                + String(rec.rtc_day < 10 ? "0" : "") + String(rec.rtc_day) + "T"
                                + String(rec.rtc_hour < 10 ? "0" : "") + String(rec.rtc_hour) + ":"
                                + String(rec.rtc_min < 10 ? "0" : "") + String(rec.rtc_min) + ":"
                                + String(rec.rtc_sec < 10 ? "0" : "") + String(rec.rtc_sec) + "Z\",";
  json += "\"gps\":{";
  json += "\"latitude\":" + String(rec.latitude, 6) + ",";
  json += "\"longitude\":" + String(rec.longitude, 6) + ",";
  json += "\"altitude\":" + String(rec.altitude, 2) + ",";
  json += "\"hdop\":" + String(rec.hdop, 2) + ",";
  json += "\"satellites\":" + String(rec.satellite_count);
  json += "},";
  json += "\"lte\":{";
  json += "\"signal_csq\":" + String(rec.signal_csq) + ",";
  json += "\"carrier\":\"" + String(rec.carrier) + "\",";
  json += "\"network_type\":\"" + String(rec.network_type) + "\",";
  json += "\"mcc\":" + String(rec.mcc) + ",";
  json += "\"mnc\":" + String(rec.mnc) + ",";
  json += "\"cell_id\":\"0x" + String(rec.cell_id, HEX) + "\",";
  json += "\"tac\":\"0x" + String(rec.tac, HEX) + "\"";
  json += "}";
  json += "}";
  return json;
}

// Send telemetry via socket-based HTTP (avoids CME ERROR: 58 conflicts)
bool sendTelemetryViaHTTP(const TelemetryRecord& rec) {
  if (!modemReady) {
    Serial.println("[API] ERROR: Modem not ready");
    return false;
  }

  Serial.println("\n[API] Sending telemetry via socket...");
  
  // Step 1: Check current PDP context status
  delay(200);
  Serial.println("[API] Checking PDP context status...");
  String pdpCheck = sendATCommand("AT+QIACT?", 2000);
  Serial.printf("[API] PDP status: %s\n", pdpCheck.c_str());
  
  // Step 2: Only activate PDP context if not already active.
  // +QIACT: 1,1,1,"x.x.x.x" means context 1 is up — re-activating returns ERROR.
  if (pdpCheck.indexOf("+QIACT: 1,1") != -1) {
    Serial.println("[API] PDP context already active — skipping activation.");
  } else {
    Serial.println("[API] Activating PDP context (ID 1)...");
    String pdpResp = sendATCommand("AT+QIACT=1", 5000);
    Serial.printf("[API] PDP activate response: %s\n", pdpResp.c_str());
    if (pdpResp.indexOf("OK") == -1) {
      Serial.println("[API] PDP activation failed");
      return false;
    }
    delay(1000);
  }
  
  // Step 3: Verify PDP context is active
  String pdpVerify = sendATCommand("AT+QIACT?", 2000);
  Serial.printf("[API] PDP verify: %s\n", pdpVerify.c_str());
  delay(500);
  
  // Step 4: Open TCP connection using EC200U's AT+QIOPEN API.
  // (AT+QSOCKCREATE is BG96/EC21 only — not supported on EC200U.)
  // AT+QIOPEN=<ctxID>,<connID>,"TCP","<host>",<port>,<localport>,<accessmode>
  //   ctxID=1 matches our PDP context, connID=0, localport=0(auto), accessmode=1(buffer)
  delay(200);
  sendATCommand("AT+QICLOSE=0,1", 1000);  // close slot 0 if lingering; ignore error
  delay(300);

  Serial.println("[API] Opening TCP connection...");
  String openCmd = "AT+QIOPEN=1,0,\"TCP\",\"" + String(API_SERVER_ADDR) + "\"," +
                   String(API_SERVER_PORT) + ",0,1";
  Serial.printf("[API] Open command: %s\n", openCmd.c_str());

  // AT+QIOPEN returns OK immediately, then sends "+QIOPEN: 0,<err>" URC asynchronously.
  // Wait for URC with extended timeout
  while (Serial1.available()) Serial1.read();  // flush
  Serial1.println(openCmd);
  String openResp = "";
  unsigned long t0 = millis();
  bool gotOK = false;
  
  while (millis() - t0 < 20000) {  // wait up to 20s for URC
    while (Serial1.available()) openResp += (char)Serial1.read();
    
    if (openResp.indexOf("OK") != -1) gotOK = true;
    if (openResp.indexOf("+QIOPEN: 0,0") != -1) break;       // success
    if (openResp.indexOf("+QIOPEN: 0,") != -1) break;         // error (non-zero code)
    delay(100);
  }
  Serial.printf("[API] Open response: %s\n", openResp.c_str());

  // If we got OK but no URC, try querying connection status
  if (gotOK && openResp.indexOf("+QIOPEN:") == -1) {
    Serial.println("[API] Got OK but no URC - checking connection status...");
    delay(1000);
    String statusCheck = sendATCommand("AT+QISTATE?", 2000);
    Serial.printf("[API] Connection state: %s\n", statusCheck.c_str());
    if (statusCheck.indexOf("0,0,1") != -1 || statusCheck.indexOf("0,1,1") != -1) {
      Serial.println("[API] Connection is open (state query confirmed)");
      delay(200);
    } else {
      Serial.println("[API] TCP open failed");
      sendATCommand("AT+QICLOSE=0,1", 1000);
      return false;
    }
  } else if (openResp.indexOf("+QIOPEN: 0,0") == -1) {
    Serial.println("[API] TCP open failed");
    sendATCommand("AT+QICLOSE=0,1", 1000);
    return false;
  }
  delay(200);

  // Step 5: Build HTTP POST request.
  // Use a static char buffer — String concatenation on ESP32 can silently
  // truncate or corrupt when heap is fragmented, producing a malformed request.
  String json = telemetryToJSON(rec);
  int jsonLen = json.length();

  // Headers (without body) — build into a fixed buffer
  char headerBuf[512];
  int headerLen = snprintf(headerBuf, sizeof(headerBuf),
    "POST %s HTTP/1.1\r\n"
    "Host: %s:%d\r\n"
    "Content-Type: application/json\r\n"
    "Content-Length: %d\r\n"
    "Connection: close\r\n"
    "\r\n",
    API_ENDPOINT_PATH, API_SERVER_ADDR, API_SERVER_PORT, jsonLen);

  int totalLen = headerLen + jsonLen;
  Serial.printf("[API] Header: %d bytes  JSON: %d bytes  Total: %d bytes\n",
                headerLen, jsonLen, totalLen);
  Serial.println("[API] --- HEADERS ---");
  Serial.print(headerBuf);
  Serial.println("[API] --- JSON BODY ---");
  Serial.println(json);
  Serial.println("[API] --- END ---");

  // Step 6: AT+QISEND=<connID>,<totalLen> → '>' prompt → headers → JSON → SEND OK
  delay(100);
  String sendCmd = "AT+QISEND=0," + String(totalLen);
  Serial.printf("[API] Send command: %s\n", sendCmd.c_str());

  while (Serial1.available()) Serial1.read();  // flush
  Serial1.println(sendCmd);
  String promptResp = "";
  unsigned long tp = millis();
  while (millis() - tp < 5000) {
    while (Serial1.available()) promptResp += (char)Serial1.read();
    if (promptResp.indexOf(">") != -1) break;
    delay(20);
  }
  Serial.printf("[API] Prompt: %s\n", promptResp.c_str());

  if (promptResp.indexOf(">") == -1) {
    Serial.println("[API] No '>' prompt — aborting send");
    sendATCommand("AT+QICLOSE=0,1", 1000);
    return false;
  }

  // Send headers then JSON body as separate writes (avoids one giant String alloc)
  Serial1.write((uint8_t*)headerBuf, headerLen);
  Serial1.print(json);

  // Wait for SEND OK / SEND FAIL
  String sendOk = "";
  unsigned long ts = millis();
  while (millis() - ts < 8000) {
    while (Serial1.available()) sendOk += (char)Serial1.read();
    if (sendOk.indexOf("SEND OK") != -1 || sendOk.indexOf("SEND FAIL") != -1) break;
    delay(50);
  }
  Serial.printf("[API] Send result: %s\n", sendOk.c_str());

  if (sendOk.indexOf("SEND OK") == -1) {
    Serial.println("[API] Send failed");
    sendATCommand("AT+QICLOSE=0,1", 1000);
    return false;
  }

  // Step 7: Read server response.
  // Poll AT+QIRD in a loop — ngrok adds latency and the modem only buffers
  // data after the full response arrives. AT+QIRD returns ERROR (not empty)
  // when no data is ready yet, so we retry until we get content or time out.
  String readResp = "";
  unsigned long tr = millis();
  while (millis() - tr < 10000) {
    delay(500);
    String chunk = sendATCommand("AT+QIRD=0,1500", 4000);
    // A successful read looks like: +QIRD: <len>\r\n<data>\r\nOK
    // An empty buffer returns ERROR — keep waiting.
    if (chunk.indexOf("+QIRD:") != -1) {
      readResp = chunk;
      break;
    }
  }
  Serial.printf("[API] Server response: %s\n", readResp.c_str());

  bool success = (readResp.indexOf("HTTP/1.1 200") != -1 ||
                  readResp.indexOf("HTTP/1.0 200") != -1 ||
                  (readResp.indexOf("+QIRD:") != -1 && readResp.indexOf("200") != -1));
  if (success) {
    Serial.println("[API] ✓ HTTP 200 OK - Telemetry sent!");
  } else {
    Serial.println("[API] ✗ No HTTP 200 received");
  }

  // Step 8: Close connection
  delay(200);
  sendATCommand("AT+QICLOSE=0,1", 2000);
  return success;
}

bool sendTelemetryViaSimpleHTTP(const TelemetryRecord& rec) {
  return sendTelemetryViaHTTP(rec);
}

bool sendTelemetryViaURL(const TelemetryRecord& rec) {
  return sendTelemetryViaHTTP(rec);
}

// ===== TELEMETRY DASHBOARD =====
void printNetworkStatus() {
  Serial.println("\n=================================================");
  Serial.println("           LIVE TELEMETRY & NETWORK LBS          ");
  Serial.println("=================================================");

  Serial.println("\n[ HARDWARE STATUS ]");
  if (rtcReady) {
    DateTime now = rtc.now();
    Serial.printf("  System Time : %02d:%02d:%02d\n", now.hour(), now.minute(), now.second());
  }
  Serial.printf("  Motion (mg) : X:%d | Y:%d | Z:%d\n", lastX, lastY, lastZ);
  Serial.printf("  ADXL345     : ARMED (XY-only)\n");

  if (modemReady && !modemShutdownTriggered) {
    Serial.println("\n[ CELLULAR NETWORK ]");
    String cops = sendATCommand("AT+COPS?", 1500);
    int q1 = cops.indexOf("\""), q2 = cops.indexOf("\"", q1 + 1);
    if (q1 != -1 && q2 != -1)
      Serial.printf("  Carrier     : %s\n", cops.substring(q1 + 1, q2).c_str());

    String csq    = sendATCommand("AT+CSQ", 1000);
    String sigNum = getValue(csq.substring(csq.indexOf(":") + 1), ',', 0);
    sigNum.trim();
    Serial.printf("  Signal (CSQ): %s / 31\n", sigNum.c_str());

    String qeng = sendATCommand("AT+QENG=\"servingcell\"", 2000);
    if (qeng.indexOf("+QENG:") != -1) {
      String cellData = qeng.substring(qeng.indexOf("+QENG:"));
      String tech = getValue(cellData, ',', 2);
      tech.replace("\"", "");
      if (tech == "LTE") {
        String mcc       = getValue(cellData, ',', 4);
        String mnc       = getValue(cellData, ',', 5);
        String cellIdHex = getValue(cellData, ',', 6);
        String tacHex    = getValue(cellData, ',', 12);
        Serial.printf("  Network Type: %s\n",            tech.c_str());
        Serial.printf("  MCC / MNC   : %s / %s\n",       mcc.c_str(), mnc.c_str());
        Serial.printf("  Cell ID(Hex): %s  |  TAC: %s\n", cellIdHex.c_str(), tacHex.c_str());
      }
    }

    Serial.println("\n[ SATELLITE GPS ]");
    String gpsResponse = sendATCommand("AT+QGPSLOC=2", 2000);
    if (gpsResponse.indexOf("+CME ERROR: 516") != -1) {
      Serial.println("  Status      : SEARCHING (Acquiring Satellites...)");
    } else if (gpsResponse.indexOf("+QGPSLOC:") != -1) {
      String gpsData = gpsResponse.substring(gpsResponse.indexOf("+QGPSLOC:") + 9);
      String lat  = getValue(gpsData, ',', 1);
      String lon  = getValue(gpsData, ',', 2);
      String alt  = getValue(gpsData, ',', 4);
      String sats = getValue(gpsData, ',', 10);
      sats.replace("\r", ""); sats.replace("\n", ""); sats.replace("OK", ""); sats.trim();
      Serial.println ("  Status      : 3D FIX SECURED");
      Serial.printf  ("  Satellites  : %s Active\n",     sats.c_str());
      Serial.printf  ("  Coordinates : %s, %s\n",         lat.c_str(), lon.c_str());
      Serial.printf  ("  Altitude    : %s Meters\n",      alt.c_str());
    }
  } else if (modemShutdownTriggered) {
    Serial.println("\n[ MODEM ] Powered off (pre-sleep shutdown active)");
  }
  Serial.println("=================================================\n");
}

// ===== ADXL345 REGISTER OPS =====
void writeRegister(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(ADXL_ADDR);
  Wire.write(reg); Wire.write(value);
  Wire.endTransmission();
}

uint8_t readRegister(uint8_t reg) {
  Wire.beginTransmission(ADXL_ADDR);
  Wire.write(reg);
  Wire.endTransmission(false);
  Wire.requestFrom(ADXL_ADDR, 1);
  return Wire.available() ? Wire.read() : 0;
}

void readXYZ() {
  Wire.beginTransmission(ADXL_ADDR);
  Wire.write(0x32);
  Wire.endTransmission(false);
  Wire.requestFrom(ADXL_ADDR, 6);
  if (Wire.available() == 6) {
    x = Wire.read() | (Wire.read() << 8);
    y = Wire.read() | (Wire.read() << 8);
    z = Wire.read() | (Wire.read() << 8);
  }
}

void clearInterrupt() { readRegister(0x30); }

// ===== ADXL345 SETUP =====
void setupADXL345() {
  Serial.println("[ADXL] Configuring...");
  writeRegister(0x2D, 0x00); delay(10);  // reset
  writeRegister(0x31, 0x0B);             // DATA_FORMAT: full res, ±16g
  writeRegister(0x2C, 0x08);             // BW_RATE: 25Hz
  writeRegister(0x24, 18);               // THRESH_ACT: ~1.1g
  writeRegister(0x27, 0x70);             // ACT_INACT_CTL: X Y Z
  writeRegister(0x2F, 0x00);             // INT_MAP: ACTIVITY → INT1
  writeRegister(0x2E, 0x10);             // INT_ENABLE: ACTIVITY
  writeRegister(0x2D, 0x08);             // POWER_CTL: measure
  delay(50);
  clearInterrupt();
  Serial.println("[ADXL] Ready");
}

// ===== RTC SETUP =====
void setupRTC() {
  if (rtc.begin(&Wire)) {
    rtcReady = true;
    if (rtc.lostPower()) {
      Serial.println("[RTC] Lost power — adjusting to compile time");
      rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    }
    Serial.println("[RTC] Ready");
  } else {
    Serial.println("[RTC] FAILED");
  }
}

// ===== MOTION DETECTION =====
bool detectMotion() {
  readXYZ();
  int dx = abs(x - lastX), dy = abs(y - lastY), dz = abs(z - lastZ);
  if (dx > MOTION_THRESHOLD || dy > MOTION_THRESHOLD || dz > MOTION_THRESHOLD) {
    lastX = x; lastY = y; lastZ = z;
    lastMotionTime = millis();
    clearInterrupt();
    Serial.printf("[MOTION] X:%d Y:%d Z:%d | dX:%d dY:%d dZ:%d\n", x, y, z, dx, dy, dz);
    return true;
  }
  return false;
}

// ===== CHECKPOINT 165s — SAVE FULL TELEMETRY TO W25Q =====
void checkpointGPSLog(unsigned long elapsed) {
  if (elapsed >= GPS_LOG_CHECKPOINT && elapsed < GPS_LOG_CHECKPOINT + 500) {

    Serial.println("\n╔═══════════════════════════════════════════════════════╗");
    Serial.println("║   [165s] FULL TELEMETRY SNAPSHOT — Saving to W25Q    ║");
    Serial.println("╚═══════════════════════════════════════════════════════╝");

    if (!modemReady) {
      Serial.println("[165s] Modem not ready — saving RTC timestamp only.");
    }

    TelemetryRecord rec;
    collectFullTelemetry(rec);   // fills RTC + GPS + LTE in one call

    // Print the complete snapshot that will be written to flash
    Serial.println("\n[ SNAPSHOT SUMMARY ]");

    // RTC
    if (rtcReady) {
      Serial.printf("  Timestamp    : %04u-%02u-%02u %02u:%02u:%02u  (unix: %lu)\n",
                    rec.rtc_year, rec.rtc_month,  rec.rtc_day,
                    rec.rtc_hour, rec.rtc_min,    rec.rtc_sec,
                    (unsigned long)rec.timestamp);
    } else {
      Serial.println("  Timestamp    : RTC unavailable");
    }

    // GPS
    if (rec.latitude != 0.0f || rec.longitude != 0.0f) {
      Serial.printf("  GPS Fix      : 3D FIX SECURED\n");
      Serial.printf("  Coordinates  : %.6f, %.6f\n", rec.latitude, rec.longitude);
      Serial.printf("  Altitude     : %.1f m\n",     rec.altitude);
      Serial.printf("  HDOP         : %.2f\n",        rec.hdop);
      Serial.printf("  Satellites   : %u active\n",   rec.satellite_count);
    } else {
      Serial.println("  GPS Fix      : No fix (saved zeros)");
    }

    // LTE
    if (rec.signal_csq != 0 || rec.mcc != 0) {
      Serial.printf("  Carrier      : %s\n",          rec.carrier);
      Serial.printf("  Network      : %s\n",           rec.network_type);
      Serial.printf("  Signal (CSQ) : %u / 31\n",     rec.signal_csq);
      Serial.printf("  MCC / MNC    : %u / %u\n",     rec.mcc, rec.mnc);
      Serial.printf("  Cell ID      : 0x%08X\n",       rec.cell_id);
      Serial.printf("  TAC          : 0x%04X\n",       rec.tac);
    } else {
      Serial.println("  LTE          : No network data (modem offline)");
    }

    // Write to flash
    saveTelemetryToFlash(rec);

    Serial.println("\n[165s] Full telemetry snapshot saved to W25Q.");
    Serial.printf ("[165s] Flash pointer now at 0x%06X\n", flashWriteAddress);

    // --- NEW: Send telemetry to API (if modem ready and API configured) ---
    if (modemReady && String(API_SERVER_ADDR) != "your-server.com") {
      Serial.println("\n[165s] Attempting to send telemetry via HTTP API...");
      delay(1000);
      
      bool apiSuccess = sendTelemetryViaHTTP(rec);
      
      if (apiSuccess) {
        apiDataSentFlag = true;
        Serial.println("[165s] ✓ API transmission successful!");
      } else {
        apiDataSentFlag = false;
        Serial.println("[165s] ⚠ API transmission failed - data saved to W25Q only");
      }
    } else if (String(API_SERVER_ADDR) == "your-server.com") {
      Serial.println("[165s] ℹ API endpoint not configured. Update API_SERVER_ADDR to enable.");
    }
    
    Serial.println("═══════════════════════════════════════════════════════\n");

    lastGPSLogTime = elapsed;
  }
}

// ===== CHECKPOINT 170s — POWER OFF EC200U + W25Q =====
void checkpointShutdown(unsigned long elapsed) {
  if (elapsed >= MODEM_SHUTDOWN_TIME && elapsed < MODEM_SHUTDOWN_TIME + 500) {
    Serial.println("\n[170s] Pre-sleep power-down sequence...");

    // 1. Power off EC200U (graceful then hard-cut)
    shutdownModem();

    // 2. Deep power-down W25Q flash + release SPI bus
    shutdownFlash();

    Serial.println("[170s] EC200U + W25Q powered off. Waiting for 180s sleep...\n");
  }
}

// ===== DEEP SLEEP ENTRY =====
void enterDeepSleep() {
  Serial.println("\n>>> ENTERING DEEP SLEEP <<<");
  Serial.printf ("Motion baseline: X=%d Y=%d Z=%d\n", lastX, lastY, lastZ);
  Serial.println("Wakeup source  : GPIO2 HIGH (ADXL345 activity interrupt)");
  Serial.flush();
  delay(100);

  esp_deep_sleep_enable_gpio_wakeup(1ULL << ADXL_INT, ESP_GPIO_WAKEUP_GPIO_HIGH);
  Serial.flush();
  delay(50);
  esp_deep_sleep_start();
  // never reached
}

// ===== DEVELOPER MENU =====
void printSystemStatus() {
  Serial.println("\n================================================== ");
  Serial.println("     SYSTEM STATUS & CONFIGURATION (v6.3)         ");
  Serial.println("================================================== \n");

  Serial.println("[ HARDWARE DEVICES ]");
  Serial.printf("  RTC (DS3231 0x68)      : %s\n",  rtcReady   ? "✓ OK"    : "✗ FAILED");
  Serial.printf("  ADXL345 (0x53)         : ✓ OK (ARMED)\n");
  Serial.printf("  EC200U Modem           : %s\n",  modemReady ? "✓ READY" : "✗ OFFLINE");
  Serial.printf("  W25Q Flash             : ✓ Ready (CS=GPIO%d)\n", FLASH_CS);

  if (rtcReady) {
    DateTime now = rtc.now();
    Serial.printf("\n[ RTC TIME ]\n");
    Serial.printf("  %04d-%02d-%02d %02d:%02d:%02d  (Unix: %lu)\n",
                  now.year(), now.month(), now.day(),
                  now.hour(), now.minute(), now.second(), now.unixtime());
  }

  Serial.println("\n[ POWER CYCLE TIMELINE ]");
  Serial.printf ("  0s   – 165s  : Normal operation (modem + GPS active)\n");
  Serial.printf ("  165s         : Save GPS + signal to W25Q flash\n");
  Serial.printf ("  170s         : EC200U graceful off (AT+QPOWD=1) + W25Q deep power-down\n");
  Serial.printf ("  180s         : ESP32-C3 enters deep sleep (~10µA)\n");
  Serial.printf ("  Wake         : ADXL345 GPIO2 HIGH → ESP32 hard-reset → full reinit\n");

  Serial.println("\n[ ACTIVE CYCLE ]");
  unsigned long elapsed   = millis() - lastMotionTime;
  unsigned long remaining = (INACTIVITY_TIMEOUT > elapsed) ? INACTIVITY_TIMEOUT - elapsed : 0;
  Serial.printf("  Elapsed   : %.1f s\n", elapsed   / 1000.0);
  Serial.printf("  Remaining : %.1f s\n", remaining / 1000.0);
  Serial.printf("  X/Y/Z     : %d / %d / %d\n", x, y, z);
  Serial.printf("  Flash ptr : 0x%06X\n", flashWriteAddress);
  Serial.println("================================================== \n");
}

void printDevMenu() {
  Serial.println("\n╔═══════════════════════════════════════════════════════╗");
  Serial.println("║     SMART STUDENT TRACKER v6.2 - DEV MENU             ║");
  Serial.println("╚═══════════════════════════════════════════════════════╝");
  Serial.println("\n[ POWER & SLEEP ]");
  Serial.println("  1.  force_sleep     -> Enter deep sleep NOW (5s countdown)");
  Serial.println("  2.  skip_170s       -> Jump to 170s checkpoint (power-off EC200U+W25Q)");
  Serial.println("  3.  skip_165s       -> Jump to 165s checkpoint (save GPS)");
  Serial.println("  4.  sleep_timer     -> Show time remaining until sleep");
  Serial.println("\n[ MOTION & SENSORS ]");
  Serial.println("  5.  get_baseline    -> Update motion baseline");
  Serial.println("  6.  test_adxl       -> Read ADXL 10 samples");
  Serial.println("  7.  force_motion    -> Simulate motion (reset timer)");
  Serial.println("\n[ GPS & MODEM ]");
  Serial.println("  12. gps_location    -> Fetch GPS NOW");
  Serial.println("  13. signal          -> Check signal strength");
  Serial.println("  14. test_gps_save   -> Fetch + save GPS to W25Q");
  Serial.println("  15. modem_boot      -> Reinitialise modem");
  Serial.println("  15b. modem_debug    -> Debug modem boot (verbose)");
  Serial.println("  16. telemetry       -> Full live telemetry dashboard");
  Serial.println("  20. modem_off       -> Power off EC200U NOW (test shutdown)");
  Serial.println("  21. flash_off       -> Deep power-down W25Q NOW (test shutdown)");
  Serial.println("\n[ API & CLOUD ]");
  Serial.println("  30. api_config      -> Show current API configuration");
  Serial.println("  31. api_test        -> Send test telemetry via HTTP POST");
  Serial.println("  32. api_test_get    -> Send test telemetry via HTTP GET");
  Serial.println("  33. api_json        -> Show JSON format of current data");
  Serial.println("\n[ DIAGNOSTICS ]");
  Serial.println("  8.  rtc_status      -> RTC time");
  Serial.println("  9.  modem_ping      -> AT test");
  Serial.println("  10. int_pin         -> GPIO2 state");
  Serial.println("  17. sys_status      -> Full system status");
  Serial.println("  18. modem_diag      -> Modem UART diagnostics");
  Serial.println("  19. uart_echo       -> UART echo test");
  Serial.println("  11. help            -> This menu\n");
}

void handleSerialCommands() {
  if (!Serial.available()) return;
  String input = Serial.readStringUntil('\n');
  input.trim();

  if (input == "" || input == "11" || input == "help") { printDevMenu(); }

  else if (input == "1" || input == "force_sleep") {
    Serial.println("\n>>> FORCING DEEP SLEEP IN 5 SECONDS <<<");
    delay(5000);
    lastMotionTime = millis() - INACTIVITY_TIMEOUT;
  }
  else if (input == "2" || input == "skip_170s") {
    lastMotionTime = millis() - MODEM_SHUTDOWN_TIME;
    Serial.println("Jumped to 170s — shutdown will trigger next loop.");
  }
  else if (input == "3" || input == "skip_165s") {
    lastMotionTime = millis() - GPS_LOG_CHECKPOINT;
    Serial.println("Jumped to 165s — GPS save will trigger next loop.");
  }
  else if (input == "4" || input == "sleep_timer") {
    unsigned long el  = millis() - lastMotionTime;
    unsigned long rem = (INACTIVITY_TIMEOUT > el) ? INACTIVITY_TIMEOUT - el : 0;
    Serial.printf("Elapsed: %.1fs  Remaining: %.1fs\n", el/1000.0, rem/1000.0);
  }
  else if (input == "5" || input == "get_baseline") {
    for (int i = 0; i < 10; i++) { readXYZ(); delay(50); }
    lastX = x; lastY = y; lastZ = z;
    Serial.printf("Baseline: X=%d Y=%d Z=%d\n", lastX, lastY, lastZ);
  }
  else if (input == "6" || input == "test_adxl") {
    for (int i = 1; i <= 10; i++) {
      readXYZ();
      Serial.printf("  Sample %d: X=%6d Y=%6d Z=%6d\n", i, x, y, z);
      delay(100);
    }
  }
  else if (input == "7" || input == "force_motion") {
    lastMotionTime = millis();
    Serial.println("Motion simulated — cycle reset.");
  }
  else if (input == "8" || input == "rtc_status") {
    if (rtcReady) {
      DateTime now = rtc.now();
      Serial.printf("RTC: %04d-%02d-%02d %02d:%02d:%02d\n",
                    now.year(), now.month(), now.day(),
                    now.hour(), now.minute(), now.second());
    } else { Serial.println("RTC: NOT READY"); }
  }
  else if (input == "9" || input == "modem_ping") {
    String r = sendATCommand("AT", 2000);
    Serial.println(r.length() > 0 ? r : "No response.");
  }
  else if (input == "10" || input == "int_pin") {
    int s = digitalRead((int)ADXL_INT);
    Serial.printf("GPIO2: %d (%s)\n", s, s == HIGH ? "HIGH" : "LOW");
  }
  else if (input == "12" || input == "gps_location") {
    Serial.println("\n>>> FETCHING GPS <<<");
    TelemetryRecord rec; memset(&rec, 0, sizeof(rec));
    collectGPS(rec);
    Serial.printf("Lat: %.6f  Lon: %.6f  Alt: %.1fm  HDOP: %.2f  Sats: %u\n",
                  rec.latitude, rec.longitude, rec.altitude, rec.hdop, rec.satellite_count);
  }
  else if (input == "13" || input == "signal") {
    Serial.println("\n>>> CHECKING LTE SIGNAL + CELL <<<");
    TelemetryRecord rec; memset(&rec, 0, sizeof(rec));
    collectLTE(rec);
    Serial.printf("Carrier: %s  %s  CSQ: %u/31  MCC/MNC: %u/%u  Cell: 0x%X  TAC: 0x%X\n",
                  rec.carrier, rec.network_type, rec.signal_csq,
                  rec.mcc, rec.mnc, rec.cell_id, rec.tac);
  }
  else if (input == "14" || input == "test_gps_save") {
    if (!modemReady) { Serial.println("ERROR: modem not ready"); return; }
    Serial.println("Collecting full telemetry snapshot and saving to W25Q...");
    TelemetryRecord rec;
    collectFullTelemetry(rec);
    saveTelemetryToFlash(rec);
    Serial.println("Full telemetry saved to W25Q.");
  }
  else if (input == "15" || input == "modem_boot") {
    Serial.println("[ Reinitializing Modem & Flash ]");
    modemReady = false;
    modemShutdownTriggered = false;
    bootModem();
    initFlash();
    if (modemReady) {
      Serial.println("[ OK ] Modem and Flash reinitialized successfully.");
    } else {
      Serial.println("[ WARN ] Modem did not respond - may still be booting.");
    }
  }
  else if (input == "15b" || input == "modem_debug") {
    Serial.println("\n╔═══════════════════════════════════════════════════════╗");
    Serial.println("║  [DEBUG] Modem Boot Sequence - Verbose Logging        ║");
    Serial.println("╚═══════════════════════════════════════════════════════╝");
    Serial.printf("\n[Pre-Boot] PWRKEY pin state: %d\n", digitalRead(MODEM_PWRKEY));
    Serial.printf("[Pre-Boot] modemReady: %d, modemShutdownTriggered: %d\n", 
                  modemReady, modemShutdownTriggered);
    
    Serial.println("\n[Step 1] Resetting MODEM_RX pin...");
    pinMode(MODEM_RX, INPUT);
    delay(50);
    
    Serial.println("[Step 2] Starting Serial1 (115200 baud)...");
    Serial1.begin(115200, SERIAL_8N1, MODEM_TX, MODEM_RX);
    delay(100);
    
    Serial.println("[Step 3] Flushing garbage from UART...");
    while (Serial1.available()) {
      char c = Serial1.read();
      Serial.printf("  Flushed: 0x%02X ('%c')\n", c, (c >= 32 && c < 127) ? c : '?');
    }
    
    Serial.println("[Step 4] Testing if modem is already awake...");
    String testAT = sendATCommand("AT", 1500);
    Serial.printf("  Response: %s\n", testAT.c_str());
    
    if (testAT.indexOf("OK") != -1) {
      Serial.println("[Result] ✓ Modem already awake!");
      modemReady = true;
    } else {
      Serial.println("[Result] Modem not responding - attempting wake pulse...");
      
      Serial.println("\n[Step 5] PWRKEY wake sequence:");
      Serial.println("  - Set PWRKEY HIGH (ensure released)");
      pinMode(MODEM_PWRKEY, OUTPUT);
      digitalWrite(MODEM_PWRKEY, HIGH);
      delay(100);
      Serial.printf("    PWRKEY: %d\n", digitalRead(MODEM_PWRKEY));
      
      Serial.println("  - Pulse PWRKEY LOW for 800ms");
      digitalWrite(MODEM_PWRKEY, LOW);
      Serial.printf("    PWRKEY: %d\n", digitalRead(MODEM_PWRKEY));
      delay(800);
      
      Serial.println("  - Release PWRKEY HIGH");
      digitalWrite(MODEM_PWRKEY, HIGH);
      Serial.printf("    PWRKEY: %d\n", digitalRead(MODEM_PWRKEY));
      
      Serial.println("\n[Step 6] Waiting 15s for modem OS to boot...");
      for (int i = 0; i < 15; i++) {
        Serial.printf("  %ds... ", i+1);
        delay(1000);
      }
      Serial.println("\n");
      
      Serial.println("[Step 7] Retesting AT command...");
      testAT = sendATCommand("AT", 1500);
      Serial.printf("  Response: %s\n", testAT.c_str());
      
      if (testAT.indexOf("OK") != -1) {
        Serial.println("[Result] ✓ Modem booted successfully!");
        modemReady = true;
      } else {
        Serial.println("[Result] ✗ Modem still not responding.");
        Serial.println("\n[Troubleshooting Tips]");
        Serial.println("  1. Check PWRKEY wiring (GPIO10)");
        Serial.println("  2. Check modem power supply");
        Serial.println("  3. Check UART wiring (GPIO20/21)");
        Serial.println("  4. Try command 19 (uart_echo) to test UART");
      }
    }
    
    Serial.println("\n[Reinitializing Flash]");
    initFlash();
    
    Serial.println("\n═══════════════════════════════════════════════════════\n");
  }
  else if (input == "16" || input == "telemetry") {
    printNetworkStatus();
  }
  else if (input == "17" || input == "sys_status") {
    printSystemStatus();
  }
  else if (input == "18" || input == "modem_diag") {
    Serial.println("\n[ UART CONFIG ]");
    Serial.printf("  Serial1 RX=GPIO%d  TX=GPIO%d  PWRKEY=GPIO%d\n",
                  MODEM_TX, MODEM_RX, MODEM_PWRKEY);
    Serial.printf("  Status: %s\n", modemReady ? "ONLINE" : "OFFLINE");
    String r = sendATCommand("AT", 2000);
    Serial.println(r.length() > 0 ? r : "No response to AT.");
  }
  else if (input == "19" || input == "uart_echo") {
    Serial.println("Sending AT and listening 3s...");
    Serial1.print("AT\r\n");
    unsigned long t = millis(); String buf = "";
    while (millis() - t < 3000) {
      if (Serial1.available()) { char c = Serial1.read(); buf += c; Serial.write(c); }
      delay(10);
    }
    Serial.println(buf.indexOf("OK") != -1 ? "\n✓ OK received." : "\n✗ No OK — check wiring.");
  }
  else if (input == "20" || input == "modem_off") {
    Serial.println("Testing EC200U shutdown sequence...");
    shutdownModem();
  }
  else if (input == "21" || input == "flash_off") {
    Serial.println("Testing W25Q deep power-down...");
    shutdownFlash();
  }
  else if (input == "30" || input == "api_config") {
    Serial.println("\n╔═══════════════════════════════════════════════════════╗");
    Serial.println("║         API CONFIGURATION & STATUS                   ║");
    Serial.println("╚═══════════════════════════════════════════════════════╝\n");
    Serial.println("[ CURRENT CONFIGURATION ]");
    Serial.printf("  Server Address  : %s\n", API_SERVER_ADDR);
    Serial.printf("  Server Port     : %d\n", API_SERVER_PORT);
    Serial.printf("  API Endpoint    : %s\n", API_ENDPOINT_PATH);
    Serial.printf("  Request Timeout : %lu ms\n\n", API_REQUEST_TIMEOUT);
    Serial.println("[ TO CHANGE SETTINGS ]");
    Serial.println("  1. Open v7_trackerApi.ino");
    Serial.println("  2. Edit these defines at the top:");
    Serial.println("     #define API_SERVER_ADDR     \"your-ip-or-domain\"");
    Serial.println("     #define API_SERVER_PORT     80");
    Serial.println("     #define API_ENDPOINT_PATH   \"/api/tracker/telemetry\"");
    Serial.println("  3. Recompile and upload\n");
    Serial.printf("[ STATUS ]\n");
    Serial.printf("  Modem Ready    : %s\n", modemReady ? "Yes" : "No");
    Serial.printf("  Last API Send  : %s\n", apiDataSentFlag ? "Success" : "Not sent yet");
    Serial.println("═══════════════════════════════════════════════════════\n");
  }
  else if (input == "31" || input == "api_test") {
    if (!modemReady) {
      Serial.println("ERROR: Modem not ready. Use command 15 to boot modem first.");
      return;
    }
    Serial.println("\n>>> TESTING API - HTTP POST <<<");
    TelemetryRecord testRec;
    memset(&testRec, 0, sizeof(testRec));
    collectFullTelemetry(testRec);
    
    bool success = sendTelemetryViaHTTP(testRec);
    if (success) {
      Serial.println("\n✓ API test completed successfully!");
    } else {
      Serial.println("\n✗ API test failed - check server configuration");
    }
  }
  else if (input == "32" || input == "api_test_get") {
    if (!modemReady) {
      Serial.println("ERROR: Modem not ready. Use command 15 to boot modem first.");
      return;
    }
    Serial.println("\n>>> TESTING API - HTTP POST (Full Telemetry) <<<");
    TelemetryRecord testRec;
    memset(&testRec, 0, sizeof(testRec));
    collectFullTelemetry(testRec);
    
    bool success = sendTelemetryViaHTTP(testRec);
    if (success) {
      Serial.println("\n✓ API test (POST) completed successfully!");
    } else {
      Serial.println("\n✗ API test (POST) failed - check server configuration");
    }
  }
  else if (input == "33" || input == "api_json") {
    Serial.println("\n╔═══════════════════════════════════════════════════════╗");
    Serial.println("║          JSON TELEMETRY FORMAT (for API)             ║");
    Serial.println("╚═══════════════════════════════════════════════════════╝\n");
    
    TelemetryRecord rec;
    memset(&rec, 0, sizeof(rec));
    collectFullTelemetry(rec);
    
    String json = telemetryToJSON(rec);
    
    Serial.println("[ PRETTY PRINTED JSON ]\n");
    Serial.println("{");
    Serial.printf("  \"timestamp\": %u,\n", rec.timestamp);
    Serial.printf("  \"rtc_datetime\": \"20XX-XX-XXTXX:XX:XXZ\",\n");
    Serial.println("  \"gps\": {");
    Serial.printf("    \"latitude\": %.6f,\n", rec.latitude);
    Serial.printf("    \"longitude\": %.6f,\n", rec.longitude);
    Serial.printf("    \"altitude\": %.2f,\n", rec.altitude);
    Serial.printf("    \"hdop\": %.2f,\n", rec.hdop);
    Serial.printf("    \"satellites\": %u\n", rec.satellite_count);
    Serial.println("  },");
    Serial.println("  \"lte\": {");
    Serial.printf("    \"signal_csq\": %u,\n", rec.signal_csq);
    Serial.printf("    \"carrier\": \"%s\",\n", rec.carrier);
    Serial.printf("    \"network_type\": \"%s\",\n", rec.network_type);
    Serial.printf("    \"mcc\": %u,\n", rec.mcc);
    Serial.printf("    \"mnc\": %u,\n", rec.mnc);
    Serial.printf("    \"cell_id\": \"0x%X\",\n", rec.cell_id);
    Serial.printf("    \"tac\": \"0x%X\"\n", rec.tac);
    Serial.println("  }");
    Serial.println("}\n");
    
    Serial.println("[ COMPACT JSON (as sent) ]\n");
    Serial.println(json);
    Serial.printf("\nJSON Size: %u bytes\n\n", json.length());
  }

  else if (input == "34" || input == "http_diag") {
    Serial.println("\n╔═══════════════════════════════════════════════════════╗");
    Serial.println("║  [HTTP DIAGNOSTICS] Testing modem HTTP service       ║");
    Serial.println("╚═══════════════════════════════════════════════════════╝\n");
    
    if (!modemReady) {
      Serial.println("ERROR: Modem not ready");
      return;
    }
    
    Serial.println("[Test 1] Checking PDP context status...");
    String pdpStatus = sendATCommand("AT+QIACT?", 2000);
    Serial.printf("Response: %s\n\n", pdpStatus.c_str());
    
    Serial.println("[Test 2] Activating PDP context (AT+QIACT=1)...");
    String pdpActivate = sendATCommand("AT+QIACT=1", 3000);
    Serial.printf("Response: %s\n\n", pdpActivate.c_str());
    
    delay(1000);
    
    Serial.println("[Test 3] Checking PDP context after activation...");
    String pdpStatus2 = sendATCommand("AT+QIACT?", 2000);
    Serial.printf("Response: %s\n\n", pdpStatus2.c_str());
    
    Serial.println("[Test 4] Stopping HTTP context (AT+QHTTPSTOP)...");
    String httpStop = sendATCommand("AT+QHTTPSTOP", 1000);
    Serial.printf("Response: %s\n", httpStop.c_str());
    delay(1000);
    
    Serial.println("[Test 5] Starting HTTP context (AT+QHTTPSTART)...");
    String httpStart = sendATCommand("AT+QHTTPSTART", 2000);
    Serial.printf("Response: %s\n\n", httpStart.c_str());
    
    if (httpStart.indexOf("OK") != -1) {
      Serial.println("✓ HTTP context started successfully!");
      sendATCommand("AT+QHTTPSTOP", 1000);
    } else {
      Serial.println("✗ HTTP context start failed");
      Serial.println("\nPossible causes:");
      Serial.println("1. PDP context not activated");
      Serial.println("2. Modem HTTP service not available");
      Serial.println("3. Modem needs reboot");
    }
    
    Serial.println("\n═══════════════════════════════════════════════════════\n");
  }

  else {
    Serial.println("Unknown command. Type 'help' or '11'.");
  }
}

// ===== SETUP =====
void setup() {
  Serial.begin(115200);
  delay(2000);

  Serial.println("\n=================================================");
  Serial.println("  SMART STUDENT TRACKER v6.2 (Deep Sleep)       ");
  Serial.println("=================================================\n");

  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  Serial.println(wakeup_reason == ESP_SLEEP_WAKEUP_GPIO
                 ? ">>> WOKE FROM MOTION (GPIO2) <<<"
                 : ">>> COLD BOOT <<<");

  // --- I2C ---
  Wire.begin(I2C_SDA, I2C_SCL);
  delay(10);

  // --- FLASH CS safe HIGH before anything else touches SPI/UART ---
  // This prevents CS floating LOW during the 15s modem boot window.
  pinMode(FLASH_CS, OUTPUT);
  digitalWrite(FLASH_CS, HIGH);
  delay(10);

  // --- Sensors ---
  setupRTC();
  setupADXL345();

  // --- Modem ---
  bootModem();
  delay(100);

  // --- Flash SPI fully initialised ---
  initFlash();

  // --- ADXL INT pin ---
  pinMode((int)ADXL_INT, INPUT);

  // --- Motion baseline ---
  readXYZ();
  lastX = x; lastY = y; lastZ = z;
  lastMotionTime = millis();

  Serial.println("\n[INIT] All systems ready.");
  Serial.println("[CYCLE] 180s until deep sleep (motion resets timer)\n");
  printDevMenu();
}

// ===== MAIN LOOP =====
void loop() {
  handleSerialCommands();

  unsigned long currentTime = millis();
  unsigned long elapsed     = currentTime - lastMotionTime;

  // Motion check — resets the whole cycle
  if (detectMotion()) {
    Serial.println("[RESET] Motion detected — inactivity cycle reset.");
    lastMotionTime = millis();
    lastGPSLogTime = 0;
    // If modem was shut down by a previous checkpoint, reboot it
    if (modemShutdownTriggered) {
      Serial.println("[RESET] Modem was shut down — rebooting for new cycle...");
      modemShutdownTriggered = false;
      bootModem();
      initFlash();
    }
    return;
  }

  // Telemetry every 15s (only while modem is alive)
  static unsigned long lastTelemetryTime = 0;
  if (currentTime - lastTelemetryTime >= 15000) {
    printNetworkStatus();
    lastTelemetryTime = currentTime;
  }

  // 165s — save GPS to flash
  checkpointGPSLog(elapsed);

  // 170s — power off EC200U + W25Q
  checkpointShutdown(elapsed);

  // 180s — deep sleep
  if (elapsed >= INACTIVITY_TIMEOUT) {
    Serial.println("\nInactivity timeout — preparing deep sleep...");

    clearInterrupt();
    delay(200);

    int state = digitalRead((int)ADXL_INT);
    Serial.printf("INT pin state: %d\n", state);

    if (state == HIGH) {
      // Motion just happened — abort sleep, reset cycle
      Serial.println("INT still HIGH — aborting sleep, resetting cycle.");
      lastMotionTime = millis();
      return;
    }

    // Ensure peripherals are down (in case checkpointShutdown was missed)
    if (!modemShutdownTriggered) {
      Serial.println("Running emergency peripheral shutdown before sleep...");
      shutdownModem();
      shutdownFlash();
    }

    enterDeepSleep();   // does not return
  }

  delay(100);
}
