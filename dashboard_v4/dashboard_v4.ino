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
SoftwareSerial sds(14, 12); // TX = D5 (GPIO14), RX = D6 (GPIO12)

WiFiServer server(80);

// ---------- Duty-cycle config (mutable via UI) ----------
unsigned long measurePeriodMs = 5UL * 60UL * 1000UL;  // full cycle length (5 minutes)
unsigned long warmupMs        = 30UL * 1000UL;        // warmup after wake (30s)
unsigned long sampleWindowMs  = 10UL * 1000UL;        // sampling duration (~10s)

// --------------------------------------------------------
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

const char* stateName(SensorState st) {
  switch (st) {
    case STATE_WARMING:  return "Warming up";
    case STATE_SAMPLING: return "Sampling";
    case STATE_SLEEPING: return "Sleeping";
    default:             return "Unknown";
  }
}

// ------------- SDS011 Sleep / Wake commands -------------

void sensorSleep() {
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

// ------------- HTTP responses -------------

void sendJson(WiFiClient &client) {
  unsigned long now = millis();

  int aqi = indianAQI_PM25(lastPm25);
  const char *cat = aqiCategory(aqi);
  const char *stName = stateName(sensorState);

  long nextInSec = -1;
  if (sensorState == STATE_SLEEPING) {
    unsigned long nextStart = cycleStartMs + measurePeriodMs;
    if ((long)(nextStart - now) > 0) {
      nextInSec = (nextStart - now) / 1000;
    } else {
      nextInSec = 0;
    }
  }

  long lastAgoSec = -1;
  if (lastUpdateMs > 0) {
    lastAgoSec = (now - lastUpdateMs) / 1000;
  }

  client.println(F("HTTP/1.1 200 OK"));
  client.println(F("Content-Type: application/json"));
  client.println(F("Cache-Control: no-store"));
  client.println(F("Connection: close"));
  client.println();

  String json = "{";

  json += "\"pm25\":";
  if (isnan(lastPm25)) json += "null"; else json += String(lastPm25, 1);

  json += ",\"pm10\":";
  if (isnan(lastPm10)) json += "null"; else json += String(lastPm10, 1);

  json += ",\"aqi\":";
  if (aqi < 0) json += "null"; else json += String(aqi);

  json += ",\"category\":\"";
  json += cat;
  json += "\"";

  json += ",\"state\":\"";
  json += stName;
  json += "\"";

  json += ",\"next_in\":";
  json += String(nextInSec);

  json += ",\"last_update_ago\":";
  json += String(lastAgoSec);

  // current config in seconds
  json += ",\"period\":";
  json += String(measurePeriodMs / 1000);

  json += ",\"warmup\":";
  json += String(warmupMs / 1000);

  json += ",\"sample\":";
  json += String(sampleWindowMs / 1000);

  json += "}";

  client.print(json);
}

// Force a new measurement cycle immediately
void sendForce(WiFiClient &client) {
  sensorWake();
  sensorState   = STATE_WARMING;
  stateStartMs  = millis();
  cycleStartMs  = millis();
  Serial.println("Force refresh requested: starting new cycle.");

  client.println(F("HTTP/1.1 200 OK"));
  client.println(F("Content-Type: application/json"));
  client.println(F("Cache-Control: no-store"));
  client.println(F("Connection: close"));
  client.println();
  client.print(F("{\"status\":\"ok\"}"));
}

// parse & apply config from /config?period=..&warmup=..&sample=..
void applyConfigFromUrl(const String &url) {
  int qm = url.indexOf('?');
  if (qm < 0) return;
  String qs = url.substring(qm + 1);

  long periodSec = (long)(measurePeriodMs / 1000);
  long warmupSec = (long)(warmupMs / 1000);
  long sampleSec = (long)(sampleWindowMs / 1000);

  while (qs.length() > 0) {
    int amp = qs.indexOf('&');
    String kv;
    if (amp >= 0) {
      kv = qs.substring(0, amp);
      qs = qs.substring(amp + 1);
    } else {
      kv = qs;
      qs = "";
    }

    int eq = kv.indexOf('=');
    if (eq < 0) continue;
    String key = kv.substring(0, eq);
    String val = kv.substring(eq + 1);
    key.toLowerCase();

    long v = val.toInt();
    if (key == "period") {
      if (v >= 10 && v <= 3600) periodSec = v;          // 10s – 60 min
    } else if (key == "warmup") {
      if (v >= 5 && v <= 300) warmupSec = v;            // 5–300 s
    } else if (key == "sample") {
      if (v >= 5 && v <= 300) sampleSec = v;            // 5–300 s
    }
  }

  // ensure sane relationship: period > warmup + sample + 5s
  long minPeriod = warmupSec + sampleSec + 5;
  if (periodSec < minPeriod) periodSec = minPeriod;

  measurePeriodMs = (unsigned long)periodSec * 1000UL;
  warmupMs        = (unsigned long)warmupSec * 1000UL;
  sampleWindowMs  = (unsigned long)sampleSec * 1000UL;

  Serial.print("New config -> period=");
  Serial.print(periodSec);
  Serial.print("s, warmup=");
  Serial.print(warmupSec);
  Serial.print("s, sample=");
  Serial.print(sampleSec);
  Serial.println("s");

  // restart cycle with new timings
  sensorWake();
  sensorState   = STATE_WARMING;
  stateStartMs  = millis();
  cycleStartMs  = millis();
}

void sendConfigResp(WiFiClient &client, const String &url) {
  applyConfigFromUrl(url);

  client.println(F("HTTP/1.1 200 OK"));
  client.println(F("Content-Type: application/json"));
  client.println(F("Cache-Control: no-store"));
  client.println(F("Connection: close"));
  client.println();

  client.print(F("{\"status\":\"ok\"}"));
}

void sendIndexHtml(WiFiClient &client) {
  client.println(F("HTTP/1.1 200 OK"));
  client.println(F("Content-Type: text/html"));
  client.println(F("Connection: close"));
  client.println();

  client.print(F(
    "<!DOCTYPE html><html><head>"
    "<meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width, initial-scale=1'>"
    "<title>AQI Monitor</title>"
    "<style>"
    "body{margin:0;font-family:-apple-system,BlinkMacSystemFont,Segoe UI,Arial,sans-serif;"
      "background:radial-gradient(circle at top,#1f2933,#020617);color:#e5e7eb;"
      "display:flex;justify-content:center;align-items:flex-start;min-height:100vh;padding:20px;}"
    ".container{max-width:420px;width:100%;}"
    "h1{margin:0 0 8px;font-size:26px;font-weight:600;text-align:left;color:#f9fafb;}"
    ".subtitle-row{display:flex;align-items:center;justify-content:space-between;gap:6px;margin-bottom:16px;}"
    ".subtitle{font-size:13px;color:#9ca3af;}"
    ".btn{font-size:11px;border:none;border-radius:999px;padding:6px 12px;"
      "background:#e5e7eb;color:#020617;font-weight:600;cursor:pointer;"
      "display:inline-flex;align-items:center;gap:6px;box-shadow:0 6px 14px rgba(0,0,0,0.35);}"
    ".btn:disabled{opacity:0.5;cursor:default;box-shadow:none;}"
    ".btn span.icon{font-size:12px;}"
    ".card{background:rgba(15,23,42,0.9);border-radius:18px;padding:16px 18px;margin-bottom:14px;"
      "box-shadow:0 18px 40px rgba(0,0,0,0.5);backdrop-filter:blur(12px);border:1px solid rgba(148,163,184,0.25);}"
    ".aqi-card{position:relative;overflow:hidden;color:#f9fafb;}"
    ".aqi-overlay{position:absolute;inset:0;background:linear-gradient(160deg,rgba(0,0,0,0.55),rgba(0,0,0,0.25));}"
    ".aqi-inner{position:relative;}"
    ".aqi-label{font-size:12px;text-transform:uppercase;letter-spacing:0.16em;color:#e5e7eb;opacity:0.9;}"
    ".aqi-value{font-size:44px;font-weight:700;line-height:1.1;text-shadow:0 2px 10px rgba(0,0,0,0.55);}"
    ".aqi-category{font-size:15px;margin-top:4px;font-weight:600;color:#fefce8;"
      "text-shadow:0 2px 6px rgba(0,0,0,0.75);}"
    ".chip{display:inline-block;padding:3px 9px;border-radius:999px;font-size:11px;"
      "border:1px solid rgba(15,23,42,0.7);margin-left:6px;background:rgba(248,250,252,0.95);color:#020617;font-weight:600;}"
    ".pill{font-size:11px;padding:4px 10px;border-radius:999px;background:rgba(15,23,42,0.9);"
      "border:1px solid rgba(148,163,184,0.5);display:inline-flex;align-items:center;gap:6px;}"
    ".dot{width:8px;height:8px;border-radius:999px;background:#6b7280;}"
    ".dot.running{background:#22c55e;}"
    ".dot.sleep{background:#6b7280;}"
    ".grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:10px;}"
    ".metric-label{font-size:11px;text-transform:uppercase;letter-spacing:0.12em;color:#9ca3af;margin-bottom:6px;}"
    ".metric-value{font-size:20px;font-weight:600;}"
    ".metric-unit{font-size:11px;color:#9ca3af;margin-left:4px;}"
    ".status-line{font-size:12px;color:#9ca3af;margin-bottom:3px;}"
    ".value-strong{color:#e5e7eb;font-weight:500;}"
    ".aqi-good{background:linear-gradient(135deg,#16a34a,#14532d);} "
    ".aqi-satisfactory{background:linear-gradient(135deg,#22c55e,#166534);} "
    ".aqi-moderate{background:linear-gradient(135deg,#eab308,#b45309);} "
    ".aqi-poor{background:linear-gradient(135deg,#f97316,#c2410c);} "
    ".aqi-verypoor{background:linear-gradient(135deg,#ef4444,#991b1b);} "
    ".aqi-severe{background:linear-gradient(135deg,#7f1d1d,#450a0a);} "
    "#aqiChart{width:100%;height:60px;margin-top:10px;}"
    "#aqiPath{fill:none;stroke:#e5e7eb;stroke-width:1.8;stroke-linecap:round;stroke-linejoin:round;"
      "filter:drop-shadow(0 2px 4px rgba(0,0,0,0.6));}"
    ".config-grid{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:8px;margin-top:8px;}"
    ".config-label{font-size:10px;text-transform:uppercase;letter-spacing:0.12em;color:#9ca3af;margin-bottom:4px;}"
    ".config-input{width:100%;padding:4px 6px;border-radius:10px;border:1px solid rgba(148,163,184,0.5);"
      "background:#020617;color:#e5e7eb;font-size:11px;box-sizing:border-box;}"
    ".config-note{font-size:11px;color:#9ca3af;margin-top:6px;}"
    "</style>"
    "</head><body><div class='container'>"
    "<h1>AQI Monitor</h1>"
    "<div class='subtitle-row'>"
      "<div class='subtitle'>Live PM2.5 / PM10 from SDS011</div>"
      "<button id='refreshBtn' class='btn'><span class='icon'>⟳</span><span>Refresh now</span></button>"
    "</div>"

    "<div id='aqiCard' class='card aqi-card'>"
      "<div class='aqi-overlay'></div>"
      "<div class='aqi-inner'>"
        "<div class='aqi-label'>Air Quality Index"
          "<span id='aqiChip' class='chip'>--</span>"
        "</div>"
        "<div id='aqiValue' class='aqi-value'>--</div>"
        "<div id='aqiCategory' class='aqi-category'>Waiting for first reading...</div>"
        "<svg id='aqiChart' viewBox='0 0 100 40' preserveAspectRatio='none'>"
          "<path id='aqiPath' d='' />"
        "</svg>"
      "</div>"
    "</div>"

    "<div class='grid'>"
      "<div class='card'>"
        "<div class='metric-label'>PM2.5</div>"
        "<div><span id='pm25Value' class='metric-value'>--</span>"
          "<span class='metric-unit'>µg/m³</span></div>"
      "</div>"
      "<div class='card'>"
        "<div class='metric-label'>PM10</div>"
        "<div><span id='pm10Value' class='metric-value'>--</span>"
          "<span class='metric-unit'>µg/m³</span></div>"
      "</div>"
    "</div>"

    "<div class='card'>"
      "<div class='status-line'>"
        "<span class='pill'><span id='stateDot' class='dot'></span>"
        "<span id='stateText'>State: --</span></span>"
      "</div>"
      "<div class='status-line'>Next reading: "
        "<span id='nextText' class='value-strong'>--</span></div>"
      "<div class='status-line'>Last update: "
        "<span id='lastText' class='value-strong'>--</span></div>"
    "</div>"

    "<div class='card'>"
      "<div class='metric-label'>Measurement settings</div>"
      "<div class='config-grid'>"
        "<div><div class='config-label'>Interval (s)</div>"
          "<input id='periodInput' class='config-input' type='number' min='30' max='3600' value='300'></div>"
        "<div><div class='config-label'>Warmup (s)</div>"
          "<input id='warmupInput' class='config-input' type='number' min='5' max='300' value='30'></div>"
        "<div><div class='config-label'>Sample (s)</div>"
          "<input id='sampleInput' class='config-input' type='number' min='5' max='300' value='10'></div>"
      "</div>"
      "<div style='margin-top:8px;display:flex;justify-content:space-between;align-items:center;'>"
        "<div class='config-note'>Applies immediately · Not saved across power loss</div>"
        "<button id='saveConfigBtn' class='btn' style='padding:5px 10px;font-size:10px;box-shadow:none;'>Save</button>"
      "</div>"
    "</div>"

    "</div>"
    "<script>"
    "let aqiHistory=[];const MAX_POINTS=60;let configInit=false;"

    "function categoryToClass(cat){"
      "if(!cat) return '';"
      "cat = cat.toLowerCase();"
      "if(cat==='good') return 'aqi-good';"
      "if(cat==='satisfactory') return 'aqi-satisfactory';"
      "if(cat==='moderate') return 'aqi-moderate';"
      "if(cat==='poor') return 'aqi-poor';"
      "if(cat==='very poor') return 'aqi-verypoor';"
      "if(cat==='severe') return 'aqi-severe';"
      "return '';"
    "}"
    "function formatSecs(s){"
      "if(s<0) return '—';"
      "if(s<60) return s.toFixed(0)+' s';"
      "var m=Math.floor(s/60);"
      "var r=s%60;"
      "if(m<60) return m+' min' + (r>5?(' '+r.toFixed(0)+' s'):'');"
      "var h=Math.floor(m/60);"
      "m=m%60;"
      "return h+' h '+m+' min';"
    "}"
    "function updateSparkline(){"
      "const path=document.getElementById('aqiPath');"
      "if(!path) return;"
      "if(aqiHistory.length<2){path.setAttribute('d','');return;}"
      "let min=Infinity,max=-Infinity;"
      "for(let v of aqiHistory){if(v<min)min=v;if(v>max)max=v;}"
      "if(max===min){max=min+1;}"
      "let d='';"
      "for(let i=0;i<aqiHistory.length;i++){"
        "let x=(i/(aqiHistory.length-1))*100;"
        "let norm=(aqiHistory[i]-min)/(max-min);"
        "let y=40-(norm*34+3);"
        "d+=(i===0?'M ':' L ')+x.toFixed(1)+' '+y.toFixed(1);"
      "}"
      "path.setAttribute('d',d);"
    "}"
    "function updateUI(data){"
      "var aqiVal=document.getElementById('aqiValue');"
      "var aqiCat=document.getElementById('aqiCategory');"
      "var aqiCard=document.getElementById('aqiCard');"
      "var aqiChip=document.getElementById('aqiChip');"
      "var pm25=document.getElementById('pm25Value');"
      "var pm10=document.getElementById('pm10Value');"
      "var stateText=document.getElementById('stateText');"
      "var nextText=document.getElementById('nextText');"
      "var lastText=document.getElementById('lastText');"
      "var stateDot=document.getElementById('stateDot');"

      "if(data.aqi===null){"
        "aqiVal.textContent='--';"
        "aqiCat.textContent='Waiting for data...';"
        "aqiChip.textContent='--';"
      "}else{"
        "aqiVal.textContent=data.aqi;"
        "aqiCat.textContent=data.category;"
        "aqiChip.textContent=data.category;"
        "aqiHistory.push(data.aqi);"
        "if(aqiHistory.length>MAX_POINTS) aqiHistory.shift();"
        "updateSparkline();"
      "}"

      "pm25.textContent=(data.pm25===null?'--':data.pm25.toFixed(1));"
      "pm10.textContent=(data.pm10===null?'--':data.pm10.toFixed(1));"

      "stateText.textContent='State: '+data.state;"

      "if(data.state==='Sleeping'){"
        "stateDot.classList.add('sleep');"
        "stateDot.classList.remove('running');"
      "}else{"
        "stateDot.classList.add('running');"
        "stateDot.classList.remove('sleep');"
      "}"

      "nextText.textContent=formatSecs(data.next_in);"
      "if(data.last_update_ago<0){"
        "lastText.textContent='—';"
      "}else if(data.last_update_ago<2){"
        "lastText.textContent='just now';"
      "}else{"
        "lastText.textContent=formatSecs(data.last_update_ago)+' ago';"
      "}"

      "aqiCard.className='card aqi-card';"
      "var cls=categoryToClass(data.category);"
      "if(cls) aqiCard.className+=' '+cls;"

      "if(!configInit && data.period && data.warmup && data.sample){"
        "document.getElementById('periodInput').value=data.period;"
        "document.getElementById('warmupInput').value=data.warmup;"
        "document.getElementById('sampleInput').value=data.sample;"
        "configInit=true;"
      "}"
    "}"
    "async function poll(){"
      "try{"
        "const r=await fetch('/data');"
        "if(!r.ok){throw new Error('HTTP '+r.status);}"
        "const j=await r.json();"
        "updateUI(j);"
      "}catch(e){console.log('Poll error',e);}"
      "setTimeout(poll,2000);"
    "}"
    "async function forceRefresh(){"
      "const btn=document.getElementById('refreshBtn');"
      "btn.disabled=true;"
      "try{"
        "await fetch('/force');"
      "}catch(e){console.log('force error',e);}"
      "setTimeout(()=>{btn.disabled=false;},5000);"
    "}"
    "async function saveConfig(){"
      "const btn=document.getElementById('saveConfigBtn');"
      "const p=document.getElementById('periodInput').value;"
      "const w=document.getElementById('warmupInput').value;"
      "const s=document.getElementById('sampleInput').value;"
      "btn.disabled=true;"
      "try{"
        "const url='/config?period='+encodeURIComponent(p)+'&warmup='+encodeURIComponent(w)+'&sample='+encodeURIComponent(s);"
        "await fetch(url);"
      "}catch(e){console.log('config error',e);}"
      "setTimeout(()=>{btn.disabled=false;},3000);"
    "}"
    "document.getElementById('refreshBtn').addEventListener('click',forceRefresh);"
    "document.getElementById('saveConfigBtn').addEventListener('click',saveConfig);"
    "poll();"
    "</script>"
    "</body></html>"
  ));
}

// Read full HTTP request header until blank line
void readRequestHeaders(WiFiClient &client, String &firstLine) {
  firstLine = client.readStringUntil('\r');
  client.readStringUntil('\n'); // consume \n

  while (client.connected()) {
    String line = client.readStringUntil('\r');
    client.readStringUntil('\n');
    if (line.length() == 0) {
      break;
    }
  }
}

void handleClient() {
  WiFiClient client = server.available();
  if (!client) return;

  unsigned long start = millis();
  while (!client.available() && millis() - start < 1000) {
    delay(1);
  }
  if (!client.available()) {
    client.stop();
    return;
  }

  String reqLine;
  readRequestHeaders(client, reqLine);
  reqLine.trim();
  if (reqLine != "GET /data HTTP/1.1"){
    Serial.print("HTTP: ");
    Serial.println(reqLine);
  }
  // extract URL path+query part
  String url = "/";
  int sp1 = reqLine.indexOf(' ');
  int sp2 = reqLine.indexOf(' ', sp1 + 1);
  if (sp1 >= 0 && sp2 > sp1) {
    url = reqLine.substring(sp1 + 1, sp2); // e.g. /data, /force, /config?...
  }

  if (url.startsWith("/data")) {
    sendJson(client);
  } else if (url.startsWith("/force")) {
    sendForce(client);
  } else if (url.startsWith("/config")) {
    sendConfigResp(client, url);
  } else {
    sendIndexHtml(client);
  }

  client.flush();
  client.stop();
}

// ------------- Setup & Loop -------------

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println();
  Serial.println("Booting...");

  sds.begin(9600);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nConnected!");
  Serial.println("================================");
  Serial.print("  AQI Dashboard -> http://");
  Serial.println(WiFi.localIP());
  Serial.println("================================");

  server.begin();
  Serial.println("Web server started.");

  sensorWake();
  cycleStartMs = millis();
  stateStartMs = millis();
  sensorState = STATE_WARMING;
}

void loop() {
  handleClient();

  unsigned long now = millis();

  switch (sensorState) {
    case STATE_WARMING:
      if (now - stateStartMs >= warmupMs) {
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

      if (now - stateStartMs >= sampleWindowMs && sampleCount > 0) {
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
      if (now - cycleStartMs >= measurePeriodMs) {
        Serial.println("Starting new measurement cycle...");
        sensorWake();
        sensorState = STATE_WARMING;
        stateStartMs = now;
        cycleStartMs = now;
      }
      break;
  }
}
