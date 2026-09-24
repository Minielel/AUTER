#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <Preferences.h>
#include "DHT.h"
#include <time.h>
#include <string.h>
#include <stdlib.h>

// ===== WLAN =====
const char* ssid     = "";
const char* password = "";

// ===== Telegram =====
#define BOT_TOKEN ""
#define CHAT_ID   ""
#define TG_CHECK_MS 2000UL

WiFiClientSecure tlsClient;
UniversalTelegramBot tgBot(BOT_TOKEN, tlsClient);
unsigned long lastTgCheck = 0;

// ===== Webserver =====
WiFiServer server(80);

// ===== Pins =====
const int ledPin  = 32;
const int pumpPin = 33;

// ===== Sensoren =====
#define DHTPIN   26
#define DHTTYPE  DHT22
#define SOIL_PIN 34

const int dry = 3200;
const int wet = 1400;

DHT dht(DHTPIN, DHTTYPE);

// ===== Speicher =====
Preferences prefs;

// ===== Schwellwerte =====
int ledOnHour  = 8;
int ledOffHour = 20;
int soilMin    = 30;
int humMin     = 40;

// ===== Sensorwerte =====
float currentTemp = 0.0f;
float currentHum  = 0.0f;
int   currentSoil = 0;

// ===== Steuerung =====
// mode: 0 = auto, 1 = manuell AN, 2 = manuell AUS
int ledMode  = 0;
int pumpMode = 0;

// ===== Pumpensteuerung =====
const unsigned long PUMP_ON_MS   = 5000UL;
const unsigned long PUMP_WAIT_MS = 30000UL;
const int           SOIL_DELTA   = 5;
const float         HUM_DELTA    = 3.0f;

bool  pumpRunning  = false;
bool  pumpWaiting  = false;
bool  botBlocked   = false;

int   soilBeforePump    = 0;
float humBeforePump     = 0.0f;
unsigned long pumpStart = 0;
unsigned long waitStart = 0;

// ===== Preferences =====
void savePrefs() {
  prefs.putInt("ledOn",   ledOnHour);
  prefs.putInt("ledOff",  ledOffHour);
  prefs.putInt("soilMin", soilMin);
  prefs.putInt("humMin",  humMin);
}

void loadPrefs() {
  ledOnHour  = prefs.getInt("ledOn",    8);
  ledOffHour = prefs.getInt("ledOff",  20);
  soilMin    = prefs.getInt("soilMin", 30);
  humMin     = prefs.getInt("humMin",  40);
}

// ===== Hilfsfunktionen =====
int getCurrentHour() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return -1;
  return timeinfo.tm_hour;
}

bool isLedOn(int h) {
  if (h < 0) return false;
  if (ledOnHour < ledOffHour)
    return (h >= ledOnHour && h < ledOffHour);
  else
    return (h >= ledOnHour || h < ledOffHour);
}

int getParam(const char* header, const char* key) {
  const char* pos = strstr(header, key);
  if (!pos) return -1;
  pos += strlen(key);
  if (*pos != '=') return -1;
  pos++;
  char tmp[8];
  int i = 0;
  while (*pos && *pos != '&' && *pos != ' ' && *pos != '\r' && i < 7)
    tmp[i++] = *pos++;
  tmp[i] = '\0';
  return atoi(tmp);
}

// ===== LED =====
void updateLed() {
  if      (ledMode == 1) digitalWrite(ledPin, LOW);
  else if (ledMode == 2) digitalWrite(ledPin, HIGH);
  else                   digitalWrite(ledPin, isLedOn(getCurrentHour()) ? LOW : HIGH);
}

// ===== Bot =====
void bot() {
  Serial.println("[BOT] Pumpe gesperrt.");
  botBlocked = true;
  char msg[160];
  snprintf(msg, sizeof(msg),
    "Pumpe gesperrt!\nKein Anstieg nach Pumpen.\n"
    "Boden: %d%% (Min: %d%%)\nLuft: %.1f%% (Min: %d%%)\n"
    "Bitte Wasser nachfullen!\nDanach: /passtso",
    currentSoil, soilMin, currentHum, humMin
  );
  tgBot.sendMessage(CHAT_ID, msg);
}

void botRelease() {
  botBlocked = false;
  Serial.println("[BOT] Pumpe freigegeben.");
  tgBot.sendMessage(CHAT_ID, "Pumpe freigegeben.");
}

// ===== Telegram =====
void handleTelegram() {
  if (millis() - lastTgCheck < TG_CHECK_MS) return;
  lastTgCheck = millis();

  int n = tgBot.getUpdates(tgBot.last_message_received + 1);
  while (n) {
    for (int i = 0; i < n; i++) {
      char text[64];
      tgBot.messages[i].text.toCharArray(text, sizeof(text));
      Serial.print("[TG] "); Serial.println(text);

      if (strcmp(text, "/passtso") == 0) {
        if (botBlocked) {
          botRelease();
        } else {
          tgBot.sendMessage(CHAT_ID, "Pumpe ist nicht gesperrt.");
        }
      } else if (strcmp(text, "/status") == 0) {
        char status[160];
        snprintf(status, sizeof(status),
          "Status:\nTemp:  %.1f C\nLuft:  %.1f%%\nBoden: %d%%\n"
          "Pumpe: %s\nLED:   %s\nBot:   %s",
          currentTemp, currentHum, currentSoil,
          pumpRunning ? "AN" : (pumpWaiting ? "Warte..." : "AUS"),
          isLedOn(getCurrentHour()) ? "AN" : "AUS",
          botBlocked ? "GESPERRT" : "OK"
        );
        tgBot.sendMessage(CHAT_ID, status);
      }
    }
    n = tgBot.getUpdates(tgBot.last_message_received + 1);
  }
}

// ===== Pumpenlogik =====
void updatePump() {
  if (pumpMode == 1) { digitalWrite(pumpPin, LOW);  return; }
  if (pumpMode == 2) { digitalWrite(pumpPin, HIGH); return; }
  if (botBlocked)    { digitalWrite(pumpPin, HIGH); return; }

  if (pumpRunning) {
    if (millis() - pumpStart >= PUMP_ON_MS) {
      digitalWrite(pumpPin, HIGH);
      pumpRunning = false;
      pumpWaiting = true;
      waitStart   = millis();
      Serial.println("[PUMPE] Gestoppt. Warte 30s...");
    }
    return;
  }

  if (pumpWaiting) {
    if (millis() - waitStart >= PUMP_WAIT_MS) {
      pumpWaiting = false;
      int   soilDelta = currentSoil - soilBeforePump;
      float humDelta  = currentHum  - humBeforePump;

      char lb[64]; snprintf(lb, sizeof(lb), "[PUMPE] Boden: %d->%d%% (d=%d)", soilBeforePump, currentSoil, soilDelta);
      Serial.println(lb);
      char lb2[64]; snprintf(lb2, sizeof(lb2), "[PUMPE] Luft: %.1f->%.1f%% (d=%.1f)", humBeforePump, currentHum, humDelta);
      Serial.println(lb2);

      bool blockSoil = (currentSoil < soilMin) && (soilDelta < SOIL_DELTA);
      bool blockHum  = ((int)currentHum < humMin) && (humDelta < HUM_DELTA);

      if (blockSoil || blockHum) bot();
      else Serial.println("[PUMPE] Anstieg OK.");
    }
    return;
  }

  if (currentSoil < soilMin || (int)currentHum < humMin) {
    soilBeforePump = currentSoil;
    humBeforePump  = currentHum;
    pumpStart      = millis();
    pumpRunning    = true;
    digitalWrite(pumpPin, LOW);
    Serial.println("[PUMPE] Gestartet.");
  }
}

// ===== Hauptseite =====
void sendPage(WiFiClient &client) {
  char buf[512];

  client.println("HTTP/1.1 200 OK");
  client.println("Content-type:text/html");
  client.println();

  client.print(
    "<!DOCTYPE html><html><head>"
    "<meta charset='UTF-8'>"
    "<title>ESP32</title>"
    "<script src='https://cdn.jsdelivr.net/npm/gaugeJS/dist/gauge.min.js'></script>"
    "<style>"
    "*{box-sizing:border-box;margin:0;padding:0}"
    "body{font-family:'Segoe UI',sans-serif;background:#f5f0eb;color:#4a4a4a;"
    "max-width:560px;margin:0 auto;padding:24px 16px}"
    "h1{font-size:1.2em;font-weight:600;color:#7a7a8a;text-align:center;margin-bottom:20px;"
    "letter-spacing:2px;text-transform:uppercase}"
    "h2{font-size:0.75em;font-weight:600;color:#9a9aaa;letter-spacing:1px;"
    "text-transform:uppercase;margin-bottom:12px}"
    ".card{background:#fff;border-radius:16px;padding:20px;margin-bottom:14px;"
    "box-shadow:0 2px 12px rgba(0,0,0,0.06)}"
    ".gauges{display:flex;justify-content:space-around;align-items:flex-end;gap:8px}"
    ".gw{text-align:center;flex:1}"
    ".gw canvas{width:120px!important;height:72px!important}"
    ".gval{font-size:1em;font-weight:600;color:#6a8fbf;margin-top:2px}"
    ".glbl{font-size:0.72em;color:#aaa;margin-top:2px}"
    // Toggle
    ".row{display:flex;align-items:center;justify-content:space-between;"
    "padding:10px 0;border-bottom:1px solid #f0f0f0}"
    ".row:last-child{border-bottom:none}"
    ".rlbl{font-size:0.88em;color:#5a5a6a}"
    ".switch{position:relative;display:inline-block;width:48px;height:26px}"
    ".switch input{opacity:0;width:0;height:0}"
    ".sl{position:absolute;cursor:pointer;inset:0;background:#dde0e8;"
    "border-radius:26px;transition:.25s}"
    ".sl:before{content:'';position:absolute;width:20px;height:20px;left:3px;top:3px;"
    "background:#fff;border-radius:50%;transition:.25s;"
    "box-shadow:0 1px 4px rgba(0,0,0,0.15)}"
    "input:checked+.sl{background:#a8c5e8}"
    "input:checked+.sl:before{transform:translateX(22px)}"
    // Auto button
    ".abtn{font-size:0.78em;padding:4px 12px;border:1px solid #c8d8ee;"
    "border-radius:20px;background:#eef4fb;color:#6a8fbf;cursor:pointer;"
    "transition:.2s}"
    ".abtn:hover{background:#ddeaf8}"
    ".abtn.active{background:#a8c5e8;color:#fff;border-color:#a8c5e8}"
    // Status badges
    ".badges{display:flex;gap:10px;flex-wrap:wrap;margin-top:4px}"
    ".badge{font-size:0.78em;padding:4px 12px;border-radius:20px;font-weight:600}"
    ".bon{background:#d4ecd4;color:#5a9a5a}"
    ".boff{background:#ebebeb;color:#999}"
    ".bblock{background:#fadadd;color:#c05a5a}"
    ".bwait{background:#fef3d0;color:#b07a20}"
    // Form
    "label.fl{font-size:0.82em;color:#7a7a8a;display:block;margin:10px 0 4px}"
    "input[type=number]{width:72px;padding:6px 8px;border:1px solid #dde0e8;"
    "border-radius:8px;font-size:0.9em;color:#4a4a4a;background:#fafafa}"
    ".sbtn{margin-top:12px;padding:8px 20px;background:#a8c5e8;color:#fff;"
    "border:none;border-radius:20px;cursor:pointer;font-size:0.88em}"
    ".sbtn:hover{background:#8ab4d8}"
    ".sep{height:1px;background:#f0f0f0;margin:10px 0}"
    "</style>"

    "<script>"
    "var gT,gH,gS;"
    "var gOpts={"
    "angle:0.05,lineWidth:0.25,radiusScale:0.95,"
    "pointer:{length:0.5,strokeWidth:0.035,color:'#6a8fbf'},"
    "limitMax:false,limitMin:false,"
    "colorStart:'#a8c5e8',colorStop:'#6a8fbf',"
    "strokeColor:'#e8edf5',generateGradient:false,highDpiSupport:true"
    "};"

    "window.onload=function(){"
    "gT=new Gauge(document.getElementById('cT')).setOptions(gOpts);"
    "gT.maxValue=50;gT.setMinValue(-10);gT.animationSpeed=25;"
    "gH=new Gauge(document.getElementById('cH')).setOptions(gOpts);"
    "gH.maxValue=100;gH.setMinValue(0);gH.animationSpeed=25;"
    "gS=new Gauge(document.getElementById('cS')).setOptions(gOpts);"
    "gS.maxValue=100;gS.setMinValue(0);gS.animationSpeed=25;"
    "load();"
    "};"

    "function load(){"
    "var x=new XMLHttpRequest();"
    "x.onload=function(){"
    "var d=JSON.parse(x.responseText);"
    "gT.set(parseFloat(d.temp));document.getElementById('vT').innerText=d.temp+' C';"
    "gH.set(parseFloat(d.hum));document.getElementById('vH').innerText=d.hum+' %';"
    "gS.set(parseFloat(d.soil));document.getElementById('vS').innerText=d.soil+' %';"

    // LED toggle + auto button sync
    "document.getElementById('tLed').checked=(d.ledMode==='1');"
    "var ab=document.getElementById('aLed');"
    "ab.className='abtn'+(d.ledMode==='0'?' active':'');"

    // Pump toggle + auto button sync
    "document.getElementById('tPump').checked=(d.pumpMode==='1');"
    "var pb=document.getElementById('aPump');"
    "pb.className='abtn'+(d.pumpMode==='0'?' active':'');"

    // Badges
    "var lbg=document.getElementById('lbadge');"
    "lbg.innerText=d.ledTxt;lbg.className='badge '+(d.ledMode==='1'?'bon':'boff');"
    "var pbg=document.getElementById('pbadge');"
    "pbg.innerText=d.pumpTxt;"
    "pbg.className='badge '+(d.pumpTxt==='AN'?'bon':d.pumpTxt==='Gesperrt'?'bblock':d.pumpTxt==='Warte...'?'bwait':'boff');"
    "var bbg=document.getElementById('bbadge');"
    "bbg.innerText=d.bot;bbg.className='badge '+(d.bot==='GESPERRT'?'bblock':'bon');"
    "};"
    "x.open('GET','/data');x.send();"
    "}"
    "setInterval(load,3000);"

    // LED toggle: manuell AN -> mode=1, aus -> mode=2
    "function setLed(v){"
    "var x=new XMLHttpRequest();"
    "x.open('GET','/ledmode?v='+(v?1:2));x.send();}"

    // Pumpe toggle: manuell AN -> mode=1, aus -> mode=2
    "function setPump(v){"
    "var x=new XMLHttpRequest();"
    "x.open('GET','/pumpmode?v='+(v?1:2));x.send();}"

    // Auto buttons -> mode=0
    "function autoLed(){"
    "var x=new XMLHttpRequest();"
    "x.open('GET','/ledmode?v=0');x.send();"
    "document.getElementById('tLed').checked=false;}"

    "function autoPump(){"
    "var x=new XMLHttpRequest();"
    "x.open('GET','/pumpmode?v=0');x.send();"
    "document.getElementById('tPump').checked=false;}"

    "</script></head><body>"
    "<h1>ESP32 Steuerung</h1>"
  );

  // ===== Gauges =====
  client.print(
    "<div class='card'>"
    "<h2>Sensorwerte</h2>"
    "<div class='gauges'>"
    "<div class='gw'><canvas id='cT'></canvas>"
    "<div class='gval' id='vT'>...</div>"
    "<div class='glbl'>Temperatur</div></div>"
    "<div class='gw'><canvas id='cH'></canvas>"
    "<div class='gval' id='vH'>...</div>"
    "<div class='glbl'>Luftfeuchte</div></div>"
    "<div class='gw'><canvas id='cS'></canvas>"
    "<div class='gval' id='vS'>...</div>"
    "<div class='glbl'>Bodenfeuchte</div></div>"
    "</div></div>"
  );

  // ===== Status =====
  client.print(
    "<div class='card'>"
    "<h2>Status</h2>"
    "<div class='badges'>"
    "<span id='lbadge' class='badge boff'>...</span>"
    "<span id='pbadge' class='badge boff'>...</span>"
    "<span id='bbadge' class='badge bon'>...</span>"
    "</div></div>"
  );

  // ===== Steuerung =====
  client.print(
    "<div class='card'>"
    "<h2>Steuerung</h2>"

    "<div class='row'>"
    "<span class='rlbl'>Licht</span>"
    "<div style='display:flex;align-items:center;gap:10px'>"
    "<button class='abtn' id='aLed' onclick='autoLed()'>Auto</button>"
    "<label class='switch'>"
    "<input type='checkbox' id='tLed' onchange='setLed(this.checked)'>"
    "<span class='sl'></span></label>"
    "</div></div>"

    "<div class='row'>"
    "<span class='rlbl'>Pumpe</span>"
    "<div style='display:flex;align-items:center;gap:10px'>"
    "<button class='abtn' id='aPump' onclick='autoPump()'>Auto</button>"
    "<label class='switch'>"
    "<input type='checkbox' id='tPump' onchange='setPump(this.checked)'>"
    "<span class='sl'></span></label>"
    "</div></div>"
    "</div>"
  );

  // ===== LED Schwellwerte =====
  snprintf(buf, sizeof(buf),
    "<div class='card'>"
    "<h2>Licht Zeitsteuerung</h2>"
    "<form action='/setled' method='get'>"
    "<label class='fl'>Von Stunde</label>"
    "<input type='number' name='ledOn'  min='0' max='23' value='%d'>"
    "<label class='fl'>Bis Stunde</label>"
    "<input type='number' name='ledOff' min='0' max='23' value='%d'><br>"
    "<button class='sbtn' type='submit'>Speichern</button>"
    "</form></div>",
    ledOnHour, ledOffHour
  );
  client.print(buf);

  // ===== Pumpe Schwellwerte =====
  snprintf(buf, sizeof(buf),
    "<div class='card'>"
    "<h2>Pumpe Schwellwerte</h2>"
    "<form action='/setpump' method='get'>"
    "<label class='fl'>Bodenfeuchte Minimum (%%)</label>"
    "<input type='number' name='soilMin' min='0' max='100' value='%d'>"
    "<label class='fl'>Luftfeuchte Minimum (%%)</label>"
    "<input type='number' name='humMin'  min='0' max='100' value='%d'><br>"
    "<button class='sbtn' type='submit'>Speichern</button>"
    "</form></div>"
    "</body></html>",
    soilMin, humMin
  );
  client.print(buf);
}

// ===== JSON =====
void sendData(WiFiClient &client) {
  char pumpTxt[16];
  if      (pumpMode == 1)  snprintf(pumpTxt, sizeof(pumpTxt), "AN");
  else if (pumpMode == 2)  snprintf(pumpTxt, sizeof(pumpTxt), "AUS");
  else if (pumpRunning)    snprintf(pumpTxt, sizeof(pumpTxt), "AN");
  else if (pumpWaiting)    snprintf(pumpTxt, sizeof(pumpTxt), "Warte...");
  else if (botBlocked)     snprintf(pumpTxt, sizeof(pumpTxt), "Gesperrt");
  else                     snprintf(pumpTxt, sizeof(pumpTxt), "AUS");

  char ledTxt[8];
  snprintf(ledTxt, sizeof(ledTxt), "%s",
    (ledMode == 1 || (ledMode == 0 && isLedOn(getCurrentHour()))) ? "AN" : "AUS"
  );

  char buf[256];
  snprintf(buf, sizeof(buf),
    "{\"temp\":\"%.1f\",\"hum\":\"%.1f\",\"soil\":\"%d\","
    "\"ledMode\":\"%d\",\"pumpMode\":\"%d\","
    "\"ledTxt\":\"%s\",\"pumpTxt\":\"%s\",\"bot\":\"%s\"}",
    currentTemp, currentHum, currentSoil,
    ledMode, pumpMode,
    ledTxt, pumpTxt,
    botBlocked ? "GESPERRT" : "OK"
  );

  client.println("HTTP/1.1 200 OK");
  client.println("Content-type:application/json");
  client.println();
  client.print(buf);
}

// ===== Redirect =====
void sendRedirect(WiFiClient &client) {
  client.println("HTTP/1.1 303 See Other");
  client.println("Location: /");
  client.println();
}

// ===== Setup =====
void setup() {
  Serial.begin(115200);

  pinMode(ledPin,  OUTPUT);
  pinMode(pumpPin, OUTPUT);
  digitalWrite(ledPin,  HIGH);
  digitalWrite(pumpPin, HIGH);

  dht.begin();

  WiFi.begin(ssid, password);
  Serial.print("Verbinde WLAN");
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println();
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());

  tlsClient.setInsecure();
  tgBot.sendMessage(CHAT_ID, "ESP32 gestartet.");

  configTime(3600, 3600, "pool.ntp.org");
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) Serial.println("NTP ok.");

  prefs.begin("ctrl", false);
  loadPrefs();

  server.begin();
  Serial.println("Webserver gestartet.");
}

// ===== Loop =====
void loop() {
  handleTelegram();

  static unsigned long lastRead = 0;
  if (millis() - lastRead > 3000UL) {
    lastRead = millis();
    float temp = dht.readTemperature();
    float hum  = dht.readHumidity();
    int   raw  = analogRead(SOIL_PIN);
    int   soil = constrain(map(raw, dry, wet, 0, 100), 0, 100);
    if (!isnan(temp) && !isnan(hum)) {
      currentTemp = temp;
      currentHum  = hum;
      currentSoil = soil;
    }
    updateLed();
  }

  updatePump();

  WiFiClient client = server.available();
  if (!client) return;

  char header[512];
  int  headerLen = 0;
  bool lineBlank = true;
  memset(header, 0, sizeof(header));

  while (client.connected()) {
    if (!client.available()) continue;
    char c = client.read();
    if (headerLen < (int)sizeof(header) - 1) {
      header[headerLen++] = c;
      header[headerLen]   = '\0';
    }

    if (c == '\n' && lineBlank) {

      if (strstr(header, "GET /data ")) {
        sendData(client);

      } else if (strstr(header, "GET /ledmode?")) {
        int v = getParam(header, "v");
        if (v >= 0 && v <= 2) { ledMode = v; updateLed(); }
        client.println("HTTP/1.1 200 OK"); client.println();

      } else if (strstr(header, "GET /pumpmode?")) {
        int v = getParam(header, "v");
        if (v >= 0 && v <= 2) {
          pumpMode = v;
          if (v == 0) { pumpRunning = false; pumpWaiting = false; digitalWrite(pumpPin, HIGH); }
        }
        client.println("HTTP/1.1 200 OK"); client.println();

      } else if (strstr(header, "GET /setled?")) {
        int v;
        v = getParam(header, "ledOn");  if (v >= 0 && v <= 23)  ledOnHour  = v;
        v = getParam(header, "ledOff"); if (v >= 0 && v <= 23)  ledOffHour = v;
        savePrefs(); updateLed(); sendRedirect(client);

      } else if (strstr(header, "GET /setpump?")) {
        int v;
        v = getParam(header, "soilMin"); if (v >= 0 && v <= 100) soilMin = v;
        v = getParam(header, "humMin");  if (v >= 0 && v <= 100) humMin  = v;
        savePrefs(); sendRedirect(client);

      } else {
        sendPage(client);
      }
      break;
    }

    if      (c == '\n') lineBlank = true;
    else if (c != '\r') lineBlank = false;
  }

  client.stop();
}