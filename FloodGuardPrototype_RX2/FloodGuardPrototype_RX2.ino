#include <painlessMesh.h>

///////////////////////////////// Mesh (match TX / RX1)//////////////////////////////////////
#define  MESH_PREFIX   "FloodGuardMesh"
#define  MESH_PASSWORD "floodguard2025"
#define  MESH_PORT     5555

#ifndef LED_BUILTIN
#define LED_BUILTIN 2   
#endif
#define BTN_PIN     D5  
#define EXT_LED_PIN D7  

const unsigned long RX_HEARTBEAT_INTERVAL_MS = 7000;   // HELLO:RX2:<id>
const unsigned long RX_HEARTBEAT_TIMEOUT_MS  = 12000;  // LED on if heartbeat less than 12s ago to show its connected to mesh
const unsigned long REBROADCAST_MIN_INTERVAL = 10000;
const unsigned long BTN_DEBOUNCE_MS          = 50;
const unsigned long BTN_LED_HOLD_MS          = 3000;

painlessMesh mesh;
uint32_t myId=0;
unsigned long lastRelayAlertMs=0, lastRelayWarningMs=0;
unsigned long lastHeardOtherRXMs=0;

bool btnStable=true, lastBtnStable=true;
unsigned long lastBtnChangeMs=0, extLedHoldUntil=0;

void setBuiltinLed(bool on){ digitalWrite(LED_BUILTIN, on?LOW:HIGH); }
void setExternalLed(bool on){ digitalWrite(EXT_LED_PIN, on?HIGH:LOW); }

void changedConnections(){ Serial.print("[mesh] CHANGED peers: "); for(auto &id: mesh.getNodeList(true)) Serial.print(String(id)+" "); Serial.println(); }
void onNewConnection(uint32_t nid){ Serial.printf("[mesh] NEW connection: %u\n", nid); changedConnections(); }
void onDroppedConnection(uint32_t nid){ Serial.printf("[mesh] DROPPED: %u\n", nid); changedConnections(); }

void received(uint32_t from, String &msg){
  if ((msg.startsWith("HELLO:RX1") || msg.startsWith("HELLO:RX2")) && from!=myId){
    lastHeardOtherRXMs = millis();
    Serial.printf("[rx2] Heard heartbeat from %u: %s\n", from, msg.c_str());
  }

  String up=msg; up.toUpperCase();
  bool isWarn=(up.indexOf("WARNING")!=-1);
  bool isAlert=(up.indexOf("ALERT")!=-1);
  bool alreadyRelayed=(up.indexOf("[RPT:")!=-1)||(up.indexOf("[BTN:")!=-1);

  if((isWarn||isAlert) && !alreadyRelayed){
    unsigned long now=millis(); bool allow=false;
    if(isAlert && now-lastRelayAlertMs>=REBROADCAST_MIN_INTERVAL){ allow=true; lastRelayAlertMs=now; }
    if(isWarn  && now-lastRelayWarningMs>=REBROADCAST_MIN_INTERVAL){ allow=true; lastRelayWarningMs=now; }
    if(allow){
      String relay="[RPT:"+String(myId)+"] "+msg;
      mesh.sendBroadcast(relay);
      Serial.printf("[rx2] Rebroadcast: %s\n", relay.c_str());
    }
  }
}

void sendManualAlert(){
  String tagged="[BTN:"+String(myId)+"] ALERT";
  mesh.sendBroadcast(tagged);
  Serial.printf("[rx2] Manual ALERT (tagged): %s\n", tagged.c_str());
  mesh.sendBroadcast("ALERT");
  Serial.println("[rx2] Manual ALERT (plain): ALERT");
  extLedHoldUntil = millis() + BTN_LED_HOLD_MS;
}

void setup(){
  Serial.begin(115200); delay(100);
  Serial.println("\n=== RX Type-2 (mesh-only repeater + button) ===");
  pinMode(LED_BUILTIN,OUTPUT); setBuiltinLed(false);
  pinMode(EXT_LED_PIN,OUTPUT); setExternalLed(false);
  pinMode(BTN_PIN,INPUT_PULLUP);

  mesh.setDebugMsgTypes(ERROR | STARTUP | CONNECTION | COMMUNICATION);
  mesh.init(MESH_PREFIX, MESH_PASSWORD, MESH_PORT);
  mesh.onReceive(&received);
  mesh.onChangedConnections(&changedConnections);
  mesh.onNewConnection(&onNewConnection);
  mesh.onDroppedConnection(&onDroppedConnection);
  myId=mesh.getNodeId();
  Serial.printf("[rx2] NodeID=%u\n", myId);
}

void loop(){
  mesh.update();

  unsigned long now=millis();

  static unsigned long lastBeat=0;
  if(now-lastBeat>=RX_HEARTBEAT_INTERVAL_MS){
    lastBeat=now;
    mesh.sendBroadcast(String("HELLO:RX2:")+myId);
    Serial.println("[rx2] Sent heartbeat");
  }

  bool connectedToAnotherRX=(now-lastHeardOtherRXMs)<=RX_HEARTBEAT_TIMEOUT_MS;
  setBuiltinLed(connectedToAnotherRX);

  bool raw=digitalRead(BTN_PIN); 
  if(raw!=btnStable && (now-lastBtnChangeMs)>=BTN_DEBOUNCE_MS){ lastBtnChangeMs=now; btnStable=raw; }
  if(lastBtnStable==HIGH && btnStable==LOW){ sendManualAlert(); }
  lastBtnStable=btnStable;

  setExternalLed(now<extLedHoldUntil);

  static unsigned long lastDbg=0;
  if(now-lastDbg>=2000){
    lastDbg=now;
    Serial.printf("[dbg] t=%lu ms peerRX=%s peers=%u btn=%s extLED=%s\n",
      now, connectedToAnotherRX?"YES":"NO",
      (unsigned)mesh.getNodeList().size(),
      btnStable?"UP":"DOWN",
      (now<extLedHoldUntil)?"ON":"OFF");
  }
}
