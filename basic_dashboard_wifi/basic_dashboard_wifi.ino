#include <ESP8266WiFi.h>
#include <SoftwareSerial.h>

// ------------ WiFi CONFIG: CHANGE THESE -------------
const char* ssid     = "Airtel_AstroNet";
const char* password = "Magic@Sky_88";
// ----------------------------------------------------

// SDS011 wiring:
//   SDS011 5V  -> ESP VIN (5V)
//   SDS011 GND -> ESP GND
//   SDS011 TX  -> ESP D5 (GPIO14)
//   SDS011 RX  -> ESP D6 (GPIO12)

// SoftwareSerial(rxPin, txPin) -> RX first, TX second
SoftwareSerial sds(14, 12); // RX=D5 (GPIO14), TX=D6 (GPIO12)

WiFiServer server(80);

// ---------- Duty-cycle config (tune if you want) ----------
const unsigned long MEASURE_PERIOD_MS = 5UL * 60UL * 1000UL;  // full cycle length (5 minutes)
const unsigned long WARMUP_MS         = 30UL * 1000UL;        // warmup after wake (30s)
const unsigned long SAMPLE_WINDOW_MS  = 10UL * 1000UL;        // sampling duration (~10s)

// ---------------------------------------------------------
float lastPm25   = NAN;
float lastPm10   = NAN;
int   lastAqi    = -1;
unsigned long lastUpdateMs = 0;

enum SensorState {
  STATE_WARMING,
  STATE_SAMPLING,
  STATE_SLEEPING
};

SensorState sensorState = STATE_WARMING;
unsigned long stateStartMs = 0;
unsigned long cycleStartMs = 0;

// running sums for one measurement window
float pm25Sum = 0;
float pm10Sum = 0;
uint16_t sampleCount = 0;

// ------------- Indian AQI for PM2.5 -------------
int indianAQI_PM25(float pm) {
  if (isnan(pm)) return -1;

  float c_low, c_high;
  int i_low, i_high;

  if (pm <= 30) {
    c_low = 0;   c_high = 30;  i_low = 0;   i_high = 50;
  } else if (pm <= 60) {
    c_low = 30;  c_high = 60;  i_low = 51;  i_high = 100;
  } else if (pm <= 90) {
    c_low = 60;  c_high = 90;  i_low = 101; i_high = 200;
  } else if (pm <= 120) {
    c_low = 90;  c_high = 120; i_low = 201; i_high = 300;
  } else if (pm <= 250) {
    c_low = 120; c_high = 250; i_low = 301; i_high = 400;
  } else {
    return 500; // Severe
  }

  float aqi = (i_high - i_low) / (c_high - c_low) * (pm - c_low) + i_low;
  return (int)aqi;
}

const char* aqiCategory(int aqi) {
  if (aqi < 0) return "Unknown";
  if (aqi <= 50)   return "Good";
  if (aqi <= 100)  return "Satisfactory";
  if (aqi <= 200)  return "Moderate";
  if (aqi <= 300)  return "Poor";
  if (aqi <= 400)  return "Very Poor";
  return "Severe";
}

// ------------- SDS011 Sleep / Wake commands -------------

void sensorSleep() {
  // Sleep command: fan + laser off
  const uint8_t sleepCmd[19] = {
    0xAA, 0xB4, 0x06, 0x01, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0xFF, 0xFF, 0x05, 0xAB
  };
  sds.write(sleepCmd, 19);
  sds.flush();
}

void sensorWake() {
  // Wake command: fan + laser on
  const uint8_t wakeCmd[19] = {
    0xAA, 0xB4, 0x06, 0x01, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0xFF, 0xFF, 0x06, 0xAB
  };
  sds.write(wakeCmd, 19);
  sds.flush();
}

// ------------- SDS011 frame reading -------------

bool readSDS011Frame(float &pm25, float &pm10) {
  if (sds.available() < 10) return false;

  uint8_t buf[10];
  sds.readBytes(buf, 10);

  if (buf[0] == 0xAA && buf[1] == 0xC0 && buf[9] == 0xAB) {
    pm25 = (buf[2] + buf[3] * 256) / 10.0f;
    pm10 = (buf[4] + buf[5] * 256) / 10.0f;
    return true;
  }

  return false;
}

// ------------- Web Server -------------

void handleClient() {
  WiFiClient client = server.available();
  if (!client) return;

  // wait for request line
  unsigned long start = millis();
  while (!client.available() && millis() - start < 1000) {
    delay(1);
  }
  if (!client.available()) {
    client.stop();
    return;
  }

  String req = client.readStringUntil('\r');
  client.readStringUntil('\n'); // consume

  // prepare some status info
  unsigned long now = millis();
  const char* stateStr = "Unknown";
  if (sensorState == STATE_WARMING)  stateStr = "Warming up";
  else if (sensorState == STATE_SAMPLING) stateStr = "Sampling";
  else if (sensorState == STATE_SLEEPING) stateStr = "Sleeping";

  long nextInSec = -1;
  if (sensorState == STATE_SLEEPING) {
    unsigned long nextStart = cycleStartMs + MEASURE_PERIOD_MS;
    if ((long)(nextStart - now) > 0) {
      nextInSec = (nextStart - now) / 1000;
    } else {
      nextInSec = 0;
    }
  }

  int aqi = indianAQI_PM25(lastPm25);
  const char* category = aqiCategory(aqi);

  // basic HTML page
  String html = F(
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/html\r\n"
    "Connection: close\r\n\r\n"
    "<!DOCTYPE html><html><head>"
    "<meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width, initial-scale=1'>"
    "<meta http-equiv='refresh' content='5'>"
    "<title>AQI Monitor</title>"
    "<style>"
    "body{font-family:Arial,Helvetica,sans-serif;background:#111;color:#eee;text-align:center;padding:20px;}"
    ".card{background:#222;padding:16px;border-radius:12px;margin:10px auto;max-width:360px;}"
    ".aqi{font-size:48px;font-weight:bold;margin:10px 0;}"
    ".pm{font-size:20px;}"
    ".status{font-size:14px;color:#aaa;}"
    "</style>"
    "</head><body><h1>AQI Monitor</h1>"
  );

  html += "<div class='card'><div class='aqi'>";
  if (!isnan(lastPm25)) {
    html += String(aqi);
    html += "</div><div>";
    html += category;
    html += "</div>";
  } else {
    html += "—</div><div>No data yet</div>";
  }
  html += "</div>";

  html += "<div class='card pm'>";
  html += "PM2.5: ";
  html += (isnan(lastPm25) ? String("—") : String(lastPm25, 1));
  html += " µg/m³<br>PM10: ";
  html += (isnan(lastPm10) ? String("—") : String(lastPm10, 1));
  html += " µg/m³</div>";

  html += "<div class='card status'>";
  html += "State: ";
  html += stateStr;
  if (sensorState == STATE_SLEEPING && nextInSec >= 0) {
    html += "<br>Next reading in ~";
    html += String(nextInSec);
    html += " s";
  }
  if (lastUpdateMs > 0) {
    html += "<br>Last update ";
    html += String((now - lastUpdateMs) / 1000);
    html += " s ago";
  }
  html += "</div>";

  html += "</body></html>";

  client.print(html);
  client.stop();
}

// ------------- Setup & Loop -------------

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println();
  Serial.println("Booting...");

  sds.begin(9600);

  // WiFi
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nConnected!");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());

  server.begin();
  Serial.println("Web server started.");

  // Start first measurement cycle
  sensorWake();
  cycleStartMs = millis();
  stateStartMs = millis();
  sensorState = STATE_WARMING;
}

void loop() {
  handleClient();  // keep serving page

  unsigned long now = millis();

  switch (sensorState) {
    case STATE_WARMING:
      if (now - stateStartMs >= WARMUP_MS) {
        // start sampling window
        pm25Sum = 0;
        pm10Sum = 0;
        sampleCount = 0;
        sensorState = STATE_SAMPLING;
        stateStartMs = now;
        Serial.println("Warmup done, sampling...");
      }
      break;

    case STATE_SAMPLING: {
      float pm25, pm10;
      if (readSDS011Frame(pm25, pm10)) {
        pm25Sum += pm25;
        pm10Sum += pm10;
        sampleCount++;
        Serial.print("Sample ");
        Serial.print(sampleCount);
        Serial.print(": PM2.5=");
        Serial.print(pm25);
        Serial.print(" PM10=");
        Serial.println(pm10);
      }

      if (now - stateStartMs >= SAMPLE_WINDOW_MS && sampleCount > 0) {
        // finalize this cycle
        lastPm25 = pm25Sum / sampleCount;
        lastPm10 = pm10Sum / sampleCount;
        lastAqi  = indianAQI_PM25(lastPm25);
        lastUpdateMs = now;

        Serial.print("Cycle done. Avg PM2.5=");
        Serial.print(lastPm25);
        Serial.print(" PM10=");
        Serial.print(lastPm10);
        Serial.print(" AQI=");
        Serial.println(lastAqi);

        sensorSleep();
        sensorState = STATE_SLEEPING;
        stateStartMs = now;
        Serial.println("Sensor sleeping.");
      }
      break;
    }

    case STATE_SLEEPING:
      // time to start next cycle?
      if (now - cycleStartMs >= MEASURE_PERIOD_MS) {
        Serial.println("Starting new measurement cycle...");
        sensorWake();
        sensorState = STATE_WARMING;
        stateStartMs = now;
        cycleStartMs = now;
      }
      break;
  }
}