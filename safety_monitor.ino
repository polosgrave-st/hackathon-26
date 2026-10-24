/*
  ZONEGUARD - Machine Zone Safety Monitor - HowdyHack 2026
  ESP32-S3 + HC-SR04 ultrasonic + 3 LED stack light + speaker/amp
  + servo barrier arm + maintenance button + OLED + phone dashboard

  NEW:
    - MAINTENANCE MODE: press the button (or tap it on the phone) to disable
      the alarm + barrier while someone works on the machine. Logged.
    - INCIDENT REPORT: every danger incident is logged with real clock time,
      how long it lasted, closest distance, and whether it was acknowledged.
      Download the full report as a spreadsheet (CSV) from the phone.

  PHONE DASHBOARD:
    1. On your phone, join Wi-Fi "ZoneGuard" (password: howdyhack26)
    2. Open http://192.168.4.1 in your browser

  Libraries: Adafruit SSD1306 (+ Adafruit GFX)
  Board: ESP32S3 Dev Module (esp32 by Espressif, version 3.x)
*/

#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ---------------- PINS ----------------
const int TRIG_PIN    = 4;
const int ECHO_PIN    = 5;
const int SERVO_PIN   = 6;
const int BUTTON_PIN  = 7;   // maintenance mode button
const int SDA_PIN     = 8;
const int SCL_PIN     = 9;
const int RED_PIN     = 15;
const int YELLOW_PIN  = 16;
const int GREEN_PIN   = 17;
const int SPEAKER_PIN = 18;

// ---------------- SETTINGS ----------------
const float WARNING_CM = 50.0;
const float DANGER_CM  = 20.0;
const int BARRIER_UP   = 0;
const int BARRIER_DOWN = 90;
const char* WIFI_NAME  = "ZoneGuard";
const char* WIFI_PASS  = "howdyhack26";

const int SERVO_CH   = 0;
const int SPEAKER_CH = 2;

// ---------------- GLOBALS ----------------
Adafruit_SSD1306 display(128, 64, &Wire, -1);
bool oledOK = false;
WebServer server(80);

enum Zone { SAFE, WARNING, DANGER };
Zone zone = SAFE;
float distanceCm = 999;
unsigned long nearMisses = 0;
bool muted = false;
bool maintenance = false;

// Incident log (newest first)
// type: 0 = danger incident, 1 = maintenance ON, 2 = maintenance OFF
struct Incident {
  uint16_t id;
  uint8_t type;
  unsigned long start;
  unsigned long end;     // 0 = still happening
  float minDist;
  bool acked;
};
const int LOG_SIZE = 50;
Incident logs[LOG_SIZE];
int logCount = 0;
uint16_t nextId = 1;
bool dangerActive = false;   // when true, logs[0] is the ongoing danger incident

unsigned long lastRead = 0, lastBeep = 0, lastOled = 0, lastButton = 0, lastBlink = 0;
bool beepOn = false, blinkOn = false, lastButtonDown = false;

// ---------------- PHONE DASHBOARD ----------------
const char PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ZoneGuard</title>
<style>
body{margin:0;font-family:system-ui,sans-serif;background:#111;color:#eee;padding:16px}
h1{font-size:20px;margin:0 0 12px}
h2{font-size:17px;margin:22px 0 8px}
#card{border-radius:16px;padding:24px;text-align:center;background:#333;transition:background .2s}
#zone{font-size:38px;font-weight:800}
#dist{font-size:18px;margin-top:6px}
.row{display:flex;gap:10px;margin-top:12px}
.box{flex:1;background:#222;border-radius:12px;padding:12px;text-align:center;font-size:13px}
.big{font-size:28px;font-weight:800}
button{width:100%;margin-top:10px;padding:14px;font-size:16px;border:0;border-radius:12px;background:#333;color:#eee}
#maintBtn{background:#1a73e8;color:#fff}
#dl{background:#1e8e3e;color:#fff}
ul{list-style:none;padding:0;margin:0}
li{background:#222;border-radius:10px;padding:10px;margin-bottom:6px;font-size:14px;line-height:1.4}
.tag{font-weight:800}
.danger{color:#ff6b5e}.maint{color:#7ab4ff}.muted{color:#999;font-size:12px}
</style></head><body>
<h1>&#9888;&#65039; ZoneGuard Safety Monitor</h1>
<div id="card"><div id="zone">--</div><div id="dist">connecting...</div></div>
<div class="row">
  <div class="box"><div class="big" id="n">0</div>near-misses</div>
  <div class="box"><div class="big" id="up">0:00</div>shift time</div>
  <div class="box"><div class="big" id="last">--</div>last incident</div>
</div>
<button onclick="fetch('/mute')">Acknowledge alarm</button>
<button id="maintBtn" onclick="fetch('/maint')">Start maintenance mode</button>
<h2>Incident report</h2>
<ul id="log"><li class="muted">No incidents yet</li></ul>
<button id="dl" onclick="downloadCSV()">Download report (CSV)</button>
<button onclick="if(confirm('Reset shift data?'))fetch('/reset')">Reset shift data</button>
<script>
const colors={SAFE:'#1e8e3e',WARNING:'#e0a800',DANGER:'#d93025'};
let last=null;
function fmt(ms){let s=Math.floor(ms/1000),m=Math.floor(s/60),h=Math.floor(m/60);
  return (h?h+':'+String(m%60).padStart(2,'0'):m)+':'+String(s%60).padStart(2,'0')}
function clock(d,ms){return new Date(Date.now()-(d.up-ms))}
function t(d,ms){return clock(d,ms).toLocaleTimeString([],{hour:'numeric',minute:'2-digit',second:'2-digit'})}
function dur(e,d){return (((e.e||d.up)-e.s)/1000).toFixed(1)+' s'}
function render(d){
  const z=d.mt?'MAINTENANCE':d.z;
  document.getElementById('zone').textContent=z+(d.m&&!d.mt?' (ack)':'');
  document.getElementById('card').style.background=d.mt?'#1a73e8':colors[d.z];
  document.getElementById('dist').textContent=d.mt?'Safety alarm disabled for maintenance':
    (d.d<0?'Nobody nearby':d.d.toFixed(0)+' cm from machine');
  document.getElementById('n').textContent=d.n;
  document.getElementById('up').textContent=fmt(d.up);
  const lastDanger=d.log.find(e=>e.y==0);
  document.getElementById('last').textContent=lastDanger?fmt(d.up-lastDanger.s)+' ago':'--';
  document.getElementById('maintBtn').textContent=d.mt?'End maintenance mode':'Start maintenance mode';
  document.getElementById('log').innerHTML=d.log.length?d.log.map(e=>{
    if(e.y==0) return '<li><span class="tag danger">#'+e.i+' DANGER</span> at '+t(d,e.s)+
      '<br>Closest: '+e.d.toFixed(0)+' cm &middot; Lasted: '+dur(e,d)+(e.e?'':' (ongoing)')+
      '<br><span class="muted">'+(e.a?'Alarm acknowledged':'Not acknowledged')+'</span></li>';
    return '<li><span class="tag maint">&#128295; Maintenance '+(e.y==1?'STARTED':'ENDED')+'</span> at '+t(d,e.s)+'</li>';
  }).join(''):'<li class="muted">No incidents yet</li>';
}
async function tick(){try{last=await (await fetch('/data')).json();render(last)}catch(e){}}
function downloadCSV(){
  if(!last)return;
  const d=last;
  let rows=[['Incident #','Type','Start time','End time','Duration (s)','Closest distance (cm)','Acknowledged']];
  [...d.log].reverse().forEach(e=>{
    const type=e.y==0?'Danger zone entry':(e.y==1?'Maintenance started':'Maintenance ended');
    rows.push([e.i,type,clock(d,e.s).toLocaleString(),e.y==0&&e.e?clock(d,e.e).toLocaleString():'',
      e.y==0?(((e.e||d.up)-e.s)/1000).toFixed(1):'',e.y==0?e.d.toFixed(0):'',e.y==0?(e.a?'Yes':'No'):'']);
  });
  rows.push([]);rows.push(['Total near-misses',d.n]);rows.push(['Report generated',new Date().toLocaleString()]);
  const csv=rows.map(r=>r.map(c=>'"'+String(c).replace(/"/g,'""')+'"').join(',')).join('\n');
  const a=document.createElement('a');
  a.href=URL.createObjectURL(new Blob([csv],{type:'text/csv'}));
  a.download='zoneguard_incident_report.csv';
  document.body.appendChild(a);a.click();a.remove();
}
setInterval(tick,500);tick();
</script></body></html>
)rawliteral";

// ---------------- HELPERS ----------------
const char* zoneName(Zone z) {
  if (z == DANGER)  return "DANGER";
  if (z == WARNING) return "WARNING";
  return "SAFE";
}

void setServo(int angle) {
  int us = map(angle, 0, 180, 500, 2400);
  ledcWrite(SERVO_PIN, (uint32_t)us * 16383 / 20000);
}

void speaker(int freq) {
  ledcWriteTone(SPEAKER_PIN, freq);
}

float readOnce() {
  digitalWrite(TRIG_PIN, LOW);  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH); delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  unsigned long us = pulseIn(ECHO_PIN, HIGH, 25000);
  if (us == 0) return 999;
  return us / 58.0;
}

float readDistance() {
  float a = readOnce(); delay(5);
  float b = readOnce(); delay(5);
  float c = readOnce();
  return max(min(a, b), min(max(a, b), c));
}

void addIncident(uint8_t type, float d) {
  for (int i = LOG_SIZE - 1; i > 0; i--) logs[i] = logs[i - 1];
  logs[0].id = nextId++;
  logs[0].type = type;
  logs[0].start = millis();
  logs[0].end = (type == 0) ? 0 : millis();
  logs[0].minDist = d;
  logs[0].acked = false;
  if (logCount < LOG_SIZE) logCount++;
}

void endDanger() {
  if (dangerActive) {
    logs[0].end = millis();
    dangerActive = false;
    Serial.printf("Incident #%u ended, lasted %.1f s\n", logs[0].id, (logs[0].end - logs[0].start) / 1000.0);
  }
}

void acknowledge() {
  if (zone == DANGER && !maintenance) {
    muted = true;
    if (dangerActive) logs[0].acked = true;
    Serial.println("Alarm acknowledged");
  }
}

void toggleMaintenance() {
  endDanger();
  maintenance = !maintenance;
  addIncident(maintenance ? 1 : 2, distanceCm);
  muted = false;
  zone = SAFE;               // re-check the zone fresh
  speaker(0); beepOn = false;
  setServo(BARRIER_UP);
  Serial.println(maintenance ? "MAINTENANCE MODE ON" : "MAINTENANCE MODE OFF");
}

void updateLights() {
  if (maintenance) {
    // slow yellow blink = maintenance
    if (millis() - lastBlink > 500) { lastBlink = millis(); blinkOn = !blinkOn; }
    digitalWrite(GREEN_PIN, LOW);
    digitalWrite(RED_PIN, LOW);
    digitalWrite(YELLOW_PIN, blinkOn);
    return;
  }
  digitalWrite(GREEN_PIN,  zone == SAFE);
  digitalWrite(YELLOW_PIN, zone == WARNING);
  digitalWrite(RED_PIN,    zone == DANGER);
}

void updateAlarm() {
  unsigned long now = millis();
  if (maintenance) {
    if (beepOn) { beepOn = false; speaker(0); }
    return;
  }
  if (zone == DANGER && !muted) {
    if (now - lastBeep > 150) {
      lastBeep = now;
      beepOn = !beepOn;
      speaker(beepOn ? 2500 : 0);
    }
  } else if (zone == WARNING) {
    if (!beepOn && now - lastBeep > 1000) { lastBeep = now; beepOn = true; speaker(1200); }
    else if (beepOn && now - lastBeep > 60) { beepOn = false; speaker(0); }
  } else if (beepOn) {
    beepOn = false;
    speaker(0);
  }
}

void updateOled() {
  if (!oledOK) return;
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(2);
  display.setCursor(0, 0);
  if (maintenance) {
    display.println("MAINT");
    display.setTextSize(1);
    display.setCursor(0, 24);
    display.println("Safety alarm OFF");
    display.println("Press button to exit");
    display.setCursor(0, 54);
    display.print("WiFi: ZoneGuard");
    display.display();
    return;
  }
  display.println(zoneName(zone));
  display.setTextSize(1);
  display.setCursor(0, 24);
  if (distanceCm >= 999) display.println("Distance: --");
  else { display.print("Distance: "); display.print(distanceCm, 0); display.println(" cm"); }
  display.print("Near-misses: ");
  display.println(nearMisses);
  display.setCursor(0, 54);
  display.print(muted ? "ALARM ACKNOWLEDGED" : "WiFi: ZoneGuard");
  display.display();
}

void checkButton() {
  bool down = (digitalRead(BUTTON_PIN) == LOW);
  if (down && !lastButtonDown && millis() - lastButton > 300) {
    lastButton = millis();
    toggleMaintenance();
  }
  lastButtonDown = down;
}

void handleData() {
  String j;
  j.reserve(4000);
  j = "{\"d\":" + String(distanceCm >= 999 ? -1.0f : distanceCm, 1);
  j += ",\"z\":\"" + String(zoneName(zone)) + "\"";
  j += ",\"n\":" + String(nearMisses);
  j += ",\"m\":" + String(muted ? "true" : "false");
  j += ",\"mt\":" + String(maintenance ? "true" : "false");
  j += ",\"up\":" + String(millis());
  j += ",\"log\":[";
  for (int i = 0; i < logCount; i++) {
    if (i) j += ",";
    j += "{\"i\":" + String((unsigned int)logs[i].id);
    j += ",\"y\":" + String((unsigned int)logs[i].type);
    j += ",\"s\":" + String(logs[i].start);
    j += ",\"e\":" + String(logs[i].end);
    j += ",\"d\":" + String(logs[i].minDist >= 999 ? 0.0f : logs[i].minDist, 1);
    j += ",\"a\":" + String(logs[i].acked ? 1 : 0) + "}";
  }
  j += "]}";
  server.send(200, "application/json", j);
}

// ---------------- SETUP ----------------
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("ZoneGuard starting...");

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(RED_PIN, OUTPUT);
  pinMode(YELLOW_PIN, OUTPUT);
  pinMode(GREEN_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  ledcAttachChannel(SERVO_PIN, 50, 14, SERVO_CH);
  ledcAttachChannel(SPEAKER_PIN, 2000, 8, SPEAKER_CH);
  speaker(0);
  setServo(BARRIER_UP);

  Wire.begin(SDA_PIN, SCL_PIN);
  oledOK = display.begin(SSD1306_SWITCHCAPVCC, 0x3C);

  // Wi-Fi: lower transmit power so it doesn't brown out the shared 3.3V rail
  WiFi.mode(WIFI_AP);
  bool apOK = WiFi.softAP(WIFI_NAME, WIFI_PASS, 6);
  WiFi.setTxPower(WIFI_POWER_11dBm);
  Serial.printf("Wi-Fi '%s' %s. Dashboard: http://", WIFI_NAME, apOK ? "started" : "FAILED");
  Serial.println(WiFi.softAPIP());

  server.on("/", []() { server.send_P(200, "text/html", PAGE); });
  server.on("/data", handleData);
  server.on("/mute", []() { acknowledge(); server.send(200, "text/plain", "ok"); });
  server.on("/maint", []() { toggleMaintenance(); server.send(200, "text/plain", "ok"); });
  server.on("/reset", []() {
    nearMisses = 0; logCount = 0; nextId = 1; dangerActive = false;
    server.send(200, "text/plain", "ok");
  });
  server.begin();

  // Startup self-test
  digitalWrite(GREEN_PIN, HIGH);  delay(200);
  digitalWrite(YELLOW_PIN, HIGH); delay(200);
  digitalWrite(RED_PIN, HIGH);
  speaker(1500); delay(150); speaker(0);
  setServo(BARRIER_DOWN); delay(500);
  setServo(BARRIER_UP);   delay(300);
  digitalWrite(GREEN_PIN, LOW);
  digitalWrite(YELLOW_PIN, LOW);
  digitalWrite(RED_PIN, LOW);
}

// ---------------- LOOP ----------------
void loop() {
  server.handleClient();
  checkButton();

  if (millis() - lastRead > 100) {
    lastRead = millis();
    distanceCm = readDistance();

    Zone newZone = SAFE;
    if (distanceCm < DANGER_CM || (zone == DANGER && distanceCm < DANGER_CM + 5)) newZone = DANGER;
    else if (distanceCm < WARNING_CM || (zone == WARNING && distanceCm < WARNING_CM + 5)) newZone = WARNING;

    if (!maintenance) {
      if (newZone == DANGER && zone != DANGER) {
        nearMisses++;
        addIncident(0, distanceCm);
        dangerActive = true;
        Serial.printf("INCIDENT #%u: danger zone entered at %.0f cm\n", logs[0].id, distanceCm);
      }
      if (newZone == DANGER && dangerActive && distanceCm < logs[0].minDist) {
        logs[0].minDist = distanceCm;   // track closest approach
      }
      if (newZone != DANGER && zone == DANGER) endDanger();
      if (newZone != DANGER) muted = false;
      if (newZone != zone) setServo(newZone == DANGER ? BARRIER_DOWN : BARRIER_UP);
    }
    zone = newZone;
  }

  updateLights();
  updateAlarm();

  if (millis() - lastOled > 250) {
    lastOled = millis();
    updateOled();
  }
}
