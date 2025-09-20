#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <painlessMesh.h>

//////////////Wi-Fi (for HTTP logging to RX-Type-1)/////////////////////////////
const char* WIFI_SSID   = "xxxxxxxxxxxxxxx";      //////////REPLACE WITH YOUR OWN WIFI INFORMATION!!!!!
const char* WIFI_PASS   = "xxxxxxxxxxxxxxxxxxxxx";
const char* SERVER_HOST = "xxxxxxxxxxxxxxxxxxxx";   
const uint16_t SERVER_PORT = 80;

#define  MESH_PREFIX   "FloodGuardMesh"
#define  MESH_PASSWORD "floodguard2025"
#define  MESH_PORT     5555

#ifndef LED_BUILTIN
#define LED_BUILTIN 2              // GPIO2/D4, active-LOW on ESP8266
#endif

// Pins
#define TRIG_PIN D5
#define ECHO_PIN D6               
#define LED_PIN  D7                // external alert LED

// Logic
#define MAX_DISTANCE_CM            300
#define ALERT_ON_CM                20
#define ALERT_OFF_CM               25
#define CONSEC_REQUIRED            2
#define BROADCAST_MIN_INTERVAL_MS  15000UL
#define WARNING_WINDOW_CM          5
#define WARNING_MIN_INTERVAL_MS    15000UL

painlessMesh mesh;
bool          localAlert = false;
unsigned long lastAlertBroadcastMs   = 0;
unsigned long lastWarningBroadcastMs = 0;

//////////////////////////////////////////////// Utilities////////////////////////////////////////////////
int singlePingCm(){
  digitalWrite(TRIG_PIN, LOW);  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH); delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  unsigned long us = pulseIn(ECHO_PIN, HIGH, 30000UL);
  if (us == 0) return MAX_DISTANCE_CM;
  int cm = (int)(us/58);
  if (cm < 0) cm = MAX_DISTANCE_CM;
  if (cm > MAX_DISTANCE_CM) cm = MAX_DISTANCE_CM;
  return cm;
}
int median5Cm(){
  int s[5]; for(int i=0;i<5;i++){ s[i]=singlePingCm(); delay(20); yield(); }
  for(int i=1;i<5;i++){ int k=s[i], j=i-1; while(j>=0 && s[j]>k){ s[j+1]=s[j]; j--; } s[j+1]=k; }
  return s[2];
}
String urlEncode(const String &x){
  String o; const char *hex="0123456789ABCDEF";
  for (size_t i=0;i<x.length();++i){ char c=x[i];
    if (isalnum(c)||c=='-'||c=='_'||c=='.'||c=='~') o+=c;
    else { o+='%'; o+=hex[(c>>4)&0xF]; o+=hex[c&0xF]; }
  } return o;
}
void httpPost(const String& type, const String& msg){
  if (WiFi.status()!=WL_CONNECTED) return;
  WiFiClient c; HTTPClient http;
  String url = String("http://")+SERVER_HOST+":"+SERVER_PORT+"/ingest";
  if (http.begin(c, url)){
    http.addHeader("Content-Type","application/x-www-form-urlencoded");
    String body = "node=tx&type="+type+"&msg="+urlEncode(msg);
    int code = http.POST(body);
    Serial.printf("[http] POST %s (%s) -> %d\n", type.c_str(), msg.c_str(), code);
    http.end();
  }
}


void receivedCallback(uint32_t from, String &msg){
  Serial.printf("[mesh] RX from %u: %s\n", from, msg.c_str());
}

////////////////////////////////////////Setup / Loop////////////////////////////////////////
void setup(){
  Serial.begin(115200);
  delay(100);
  Serial.println();
  Serial.println("=== FloodGuard TX (debug) ===");
  Serial.printf("[boot] SDK:%s  ChipID:%08X  Reset:%s\n",
                ESP.getSdkVersion(), ESP.getChipId(), ESP.getResetReason().c_str());

  pinMode(LED_BUILTIN, OUTPUT); digitalWrite(LED_BUILTIN, LOW); // to power LED ON
  pinMode(TRIG_PIN, OUTPUT); pinMode(ECHO_PIN, INPUT);
  pinMode(LED_PIN, OUTPUT);  digitalWrite(LED_PIN, LOW);

  mesh.setDebugMsgTypes(ERROR | STARTUP);
  mesh.init(MESH_PREFIX, MESH_PASSWORD, MESH_PORT);
  mesh.onReceive(&receivedCallback);
  mesh.stationManual(WIFI_SSID, WIFI_PASS);   // non-blocking
  mesh.setHostname("FloodGuard-TX");
}

void loop(){
  mesh.update();

  // show IP address once
  static bool ipShown=false;
  if (!ipShown && WiFi.status()==WL_CONNECTED){
    Serial.print("[wifi] IP: "); Serial.println(WiFi.localIP()); ipShown=true;
  }

  // Sensing + FSM
  int cm = median5Cm();
  static int below=0, above=0;
  bool wasAlert = localAlert;

  if (!localAlert){
    if (cm < ALERT_ON_CM){ below++; above=0; } else below=0;
    if (below >= CONSEC_REQUIRED){ localAlert = true; below=0; }
  } else {
    if (cm > ALERT_OFF_CM){ above++; below=0; } else above=0;
    if (above >= CONSEC_REQUIRED){ localAlert = false; above=0; }
  }

  unsigned long now = millis();

  // ALERT broadcast + optional HTTP log
  if (wasAlert != localAlert || (localAlert && (now - lastAlertBroadcastMs) >= BROADCAST_MIN_INTERVAL_MS)){
    mesh.sendBroadcast("ALERT");
    lastAlertBroadcastMs = now;
    Serial.println("[mesh] Broadcast: ALERT");
    httpPost("ALERT", "ALERT");
  }

  // WARNING broadcast near threshold (rate-limited)
  if (!localAlert){
    bool inWarn = (cm > ALERT_ON_CM) && (cm <= ALERT_ON_CM + WARNING_WINDOW_CM);
    if (inWarn && (now - lastWarningBroadcastMs) >= WARNING_MIN_INTERVAL_MS){
      int diff = cm - ALERT_ON_CM; if (diff < 0) diff = 0;
      String w = "WARNING!! Water level is currently " + String(diff) + " cm away from Dangerous Level";
      mesh.sendBroadcast(w);
      lastWarningBroadcastMs = now;
      Serial.println(String("[mesh] Broadcast: ")+w);
      httpPost("WARNING", w);
    }
  }

  // Local alert LED
  digitalWrite(LED_PIN, localAlert ? HIGH : LOW);

  static unsigned long lastDbg=0;
  if (now - lastDbg >= 2000){
    lastDbg = now;
    int peers = mesh.getNodeList().size();   // number of known peers (excludes self)
    Serial.printf("[dbg] cm=%d  alert=%d  wifi=%s  peers=%d\n",
                  cm, localAlert, (WiFi.status()==WL_CONNECTED?"up":"down"), peers);
  }

  delay(300);
}
