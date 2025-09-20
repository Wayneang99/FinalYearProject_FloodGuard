#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <painlessMesh.h>
#include <time.h>  // timestamps

/////////////////// Wi-Fi (web UI)///////////////////////////////
const char* WIFI_SSID = "xxxxxxxxxxxxxxxx"; ///////////REPLACE WITH YOUR OWN WIFI INFORMATION!!!!
const char* WIFI_PASS = "xxxxxxxxxxxxxxxxx";

//////////////////////////////// Telegram bot ////////////////////////
const char* TG_BOT_TOKEN = "xxxxxxxxxxxxxxxxxxxxx";  ////// AVAILABLE IN COURSERA SUBMISSION!!!!
const char* TG_CHAT_ID   = "xxxxxxxxxxxxxxxxxxxxxx"; /// REPLACE WITH YOUR OWN CHAT ID!!!             

//////////////////////////////////////// Mesh (match TX / RX2) ///////////////////////////////////////
#define  MESH_PREFIX   "FloodGuardMesh"
#define  MESH_PASSWORD "floodguard2025"
#define  MESH_PORT     5555

#ifndef LED_BUILTIN
#define LED_BUILTIN 2   
#endif

/////////////////////////////I2C pins & addresses///////////////////////////////////
#define I2C_SDA D2
#define I2C_SCL D1
#define LCD_ADDR_1 0x27
#define LCD_ADDR_2 0x3F

/////////////////////////// External LEDs////////////////////////////////////////
#define RED_LED_PIN D6   // Red LED pulses on any ALERT
#define YEL_LED_PIN D5   // Yellow LED pulses on WARNING from TX (HTTP ingest)
const unsigned long LED_HOLD_MS = 3000;

///////////////////////////////////// Auth////////////////////////////////////////////
const char* AUTH_USER = "admin";
const char* AUTH_PASS = "admin";

//////////////////////////////Timers, buffers///////////////////////////////////
const unsigned long RX_HEARTBEAT_INTERVAL_MS = 7000;
const unsigned long LCD_SHOW_MS              = 5000;
const size_t        MAX_LOGS                 = 20;

//////////////////////////////Time / NTP (Asia/Singapore UTC+8, no DST)/////////////////////////////////////////
const long GMT_OFFSET_SEC = 8 * 3600;
const int  DST_OFFSET_SEC = 0;
bool       timeSynced     = false;
unsigned long lastTimeAttemptMs = 0;

//////////////////////////////// Telegram throttle & debug /////////////////////////////////
const unsigned long TG_MIN_INTERVAL_MS = 2000;
unsigned long lastTgSentMs = 0;
int    tg_last_http = -999;
String tg_last_resp;
String tg_last_err;
String tg_last_dns = "n/a";
String tg_last_step = "n/a";
long   tg_last_rssi = 0;
const unsigned long TG_DEDUPE_WINDOW_MS = 3000;
String tg_last_msg; unsigned long tg_last_msg_time = 0;

//////////////////////////Telegram queue (do heavy work in loop, not callbacks!!!!!!!!!!)/////////////////////////////
const size_t TGQ_CAP = 6;
String tg_queue[TGQ_CAP];
uint8_t tgq_head = 0, tgq_count = 0;
bool tgq_sending = false;

void tgq_enqueue(const String& m){
  // overwrite oldest if full
  if (tgq_count < TGQ_CAP) {
    tg_queue[(tgq_head + tgq_count) % TGQ_CAP] = m;
    tgq_count++;
  } else {
    tg_queue[tgq_head] = m;
    tgq_head = (tgq_head + 1) % TGQ_CAP;
  }
}
bool tgq_peek(String& out){
  if (!tgq_count) return false;
  out = tg_queue[tgq_head];
  return true;
}
void tgq_pop(){
  if (!tgq_count) return;
  tgq_head = (tgq_head + 1) % TGQ_CAP;
  tgq_count--;
}

/////////////////////////////////////////////////////////////////////////////////////
painlessMesh mesh;
ESP8266WebServer server(80);

String allLogs[MAX_LOGS]; size_t allHead=0, allCount=0;
String txLogs [MAX_LOGS]; size_t txHead =0, txCount =0;
String rxLogs [MAX_LOGS]; size_t rxHead =0, rxCount =0;

LiquidCrystal_I2C lcd27(LCD_ADDR_1, 16, 2);
LiquidCrystal_I2C lcd3F(LCD_ADDR_2, 16, 2);
LiquidCrystal_I2C* lcd = nullptr;

struct { bool active=false; unsigned long until=0; String l1,l2; } lcdState;

// LED state
unsigned long redUntil = 0;
unsigned long yelUntil = 0;

///////////////////////////////////////////// time helpers /////////////////////////////////////////////////////
String fmtNow() {
  time_t now = time(nullptr);
  if (now < 1609459200) return String(millis()) + "ms"; //  if not synced yet then ms
  struct tm tmnow; localtime_r(&now, &tmnow);
  char buf[20]; strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmnow);
  return String(buf);
}
void ensureTimeSyncNonBlocking(){
  if (timeSynced || WiFi.status() != WL_CONNECTED) return;
  unsigned long nowMs = millis();
  if (nowMs - lastTimeAttemptMs < 5000) return; // every 5s
  lastTimeAttemptMs = nowMs;
  configTime(GMT_OFFSET_SEC, DST_OFFSET_SEC, "pool.ntp.org", "time.google.com", "time.windows.com");
  time_t now = time(nullptr);
  if (now >= 1609459200) { timeSynced = true; Serial.printf("[time] Synced: %s\n", fmtNow().c_str()); }
}

////////////////////////////////////////////// small utils//////////////////////////////////////////////////////////////////
void push(String arr[], size_t& head, size_t& count, const String& line){ arr[head]=line; head=(head+1)%MAX_LOGS; if(count<MAX_LOGS)count++; }
String nowTag(){ return fmtNow(); }
void addAll(const String& l){ push(allLogs,allHead,allCount,l); Serial.println("[ALL] "+l); }
void addTX (const String& l){ push(txLogs ,txHead ,txCount ,l); Serial.println("[TX ] "+l); }
void addRX (const String& l){ push(rxLogs ,rxHead ,rxCount ,l); Serial.println("[RX ] "+l); }

bool i2cProbe(uint8_t a){ Wire.beginTransmission(a); return (Wire.endTransmission()==0); }
String fit16(const String& s){ return s.length()<=16 ? s : s.substring(0,16); }
void lcdShowLines(const String& l1,const String& l2,unsigned long hold){ if(!lcd)return; lcdState.active=true; lcdState.until=millis()+hold; lcdState.l1=fit16(l1); lcdState.l2=fit16(l2); lcd->clear(); lcd->setCursor(0,0); lcd->print(lcdState.l1); lcd->setCursor(0,1); lcd->print(lcdState.l2); }
void lcdShowIdle(){ if(!lcd)return; lcdState.active=false; lcd->clear(); lcd->setCursor(0,0); lcd->print("RX1 Ready"); lcd->setCursor(0,1); lcd->print((WiFi.status()==WL_CONNECTED)?"WiFi OK":"WiFi ..."); }
void onAlertOrWarningLCD(const String& type,const String& details){ lcdShowLines(type=="ALERT"?"ALERT! TAKE CARE":"WARNING RECEIVED", details, LCD_SHOW_MS); }

inline void setRed(bool on){ digitalWrite(RED_LED_PIN, on?HIGH:LOW); }
inline void setYel(bool on){ digitalWrite(YEL_LED_PIN, on?HIGH:LOW); }
inline void pulseRed(){ redUntil = millis() + LED_HOLD_MS; setRed(true); }
inline void pulseYel(){ yelUntil = millis() + LED_HOLD_MS; setYel(true); }

// URL-encode for Telegram
String urlEncode(const String &x){
  String o; const char *hex="0123456789ABCDEF";
  for (size_t i=0;i<x.length();++i){ uint8_t c=(uint8_t)x[i];
    if ((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c=='~') o+=(char)c;
    else { o+='%'; o+=hex[(c>>4)&0xF]; o+=hex[c&0xF]; }
  }
  return o;
}

///////////////////////////////////////// Telegram send (DNS + POST→GET + RAW TLS) ///////////////////////////////////////////////////////
bool telegramSend(const String& text){
  if (text == tg_last_msg && millis() - tg_last_msg_time < TG_DEDUPE_WINDOW_MS) return true;
  tg_last_msg = text; tg_last_msg_time = millis();

  tg_last_http = -999; tg_last_resp=""; tg_last_err=""; tg_last_dns="n/a"; tg_last_step="start"; tg_last_rssi = WiFi.RSSI();

  if (WiFi.status() != WL_CONNECTED) { tg_last_err = "WiFi not connected"; return false; }
  if (!TG_BOT_TOKEN || String(TG_BOT_TOKEN).startsWith("YOUR_")) { tg_last_err = "Bot token not set"; return false; }
  if (!TG_CHAT_ID   || String(TG_CHAT_ID).length()==0 || String(TG_CHAT_ID).startsWith("YOUR_")) { tg_last_err = "Chat ID not set"; return false; }

  IPAddress apiIP; tg_last_step="dns";
  if (!WiFi.hostByName("api.telegram.org", apiIP)) { tg_last_err="DNS resolution failed"; tg_last_dns="fail"; return false; }
  tg_last_dns = apiIP.toString();

  unsigned long nowMs = millis();
  if (nowMs - lastTgSentMs < TG_MIN_INTERVAL_MS) { tg_last_err="Rate-limited"; return false; }
  lastTgSentMs = nowMs;

  { // POST
    tg_last_step="post";
    WiFiClientSecure client; client.setInsecure(); client.setTimeout(15000);
    client.setBufferSizes(512,512); // smaller to reduce RAM spike
    HTTPClient https; String url = String("https://api.telegram.org/bot") + TG_BOT_TOKEN + "/sendMessage";
    if (https.begin(client, url)) {
      https.addHeader("Content-Type","application/x-www-form-urlencoded");
      String body = "chat_id=" + String(TG_CHAT_ID) + "&text=" + urlEncode(text);
      tg_last_http = https.POST(body); tg_last_resp = https.getString(); https.end();
      if (tg_last_http==200 && tg_last_resp.indexOf("\"ok\":true")!=-1) return true;
    } else tg_last_err="HTTPS begin(POST) failed";
  }
  { // GET fallback
    tg_last_step="get";
    WiFiClientSecure client; client.setInsecure(); client.setTimeout(15000);
    client.setBufferSizes(512,512);
    HTTPClient https; String url = String("https://api.telegram.org/bot")+TG_BOT_TOKEN+"/sendMessage?chat_id="+TG_CHAT_ID+"&text="+urlEncode(text);
    if (https.begin(client, url)) {
      tg_last_http = https.GET(); tg_last_resp = https.getString(); https.end();
      if (tg_last_http==200 && tg_last_resp.indexOf("\"ok\":true")!=-1) return true;
    } else tg_last_err="HTTPS begin(GET) failed";
  }
  { // RAW TLS to IP
    tg_last_step="raw";
    WiFiClientSecure client; client.setInsecure(); client.setTimeout(15000);
    client.setBufferSizes(512,512);
    if (!client.connect(apiIP, 443)) { tg_last_err="TLS connect() failed"; tg_last_http=-1; return false; }
    String path = String("/bot")+TG_BOT_TOKEN+"/sendMessage?chat_id="+TG_CHAT_ID+"&text="+urlEncode(text);
    String req  = String("GET ")+path+" HTTP/1.1\r\nHost: api.telegram.org\r\nUser-Agent: FloodGuard/1.0\r\nConnection: close\r\n\r\n";
    client.print(req);
    String status = client.readStringUntil('\n');
    if (status.startsWith("HTTP/1.1 "))      tg_last_http = status.substring(9,12).toInt();
    else if (status.startsWith("HTTP/2 "))   tg_last_http = status.substring(7,10).toInt();
    else                                     tg_last_http = -2;
    unsigned long t0=millis(); while(client.connected() && client.available()==0 && millis()-t0<3000){ delay(10); }
    String body=""; while(client.available() && body.length()<512) body+=(char)client.read();
    tg_last_resp=body;
    if (tg_last_http==200 && body.indexOf("\"ok\":true")!=-1) { tg_last_err=""; return true; }
    tg_last_err="RAW fail"; return false;
  }
}

////////////////////////////////////////////Queue flusher (call from loop)///////////////////////////////////////////////////////
void tgq_flush(){
  if (tgq_sending) return;
  if (!tgq_count) return;
  if (WiFi.status()!=WL_CONNECTED) return;

  // simple pacing
  if (millis() - lastTgSentMs < TG_MIN_INTERVAL_MS) return;

  String m;
  if (!tgq_peek(m)) return;
  tgq_sending = true;
  bool ok = telegramSend(m);
  if (ok) tgq_pop();
  tgq_sending = false;
}

//////////////////////////////////////////////THEMED HTML helpers (light blue UI + icons)////////////////////////////////////////////////////////
////////////////////////////////// This part of HTML styling was copied from one of my personal projects previously when i was teaching myself HTML/CSS////////////////////////////////////////////////////////
String header(const String& t){
  String s = F(
"<!doctype html><meta charset='utf-8'><meta name=viewport content='width=device-width,initial-scale=1'>"
"<style>"
":root{--bg:#EAF6FF;--bg-2:#F6FBFF;--text:#0D1B2A;--muted:#5C6B7A;--primary:#1976D2;--primary-600:#1565C0;--primary-050:#E3F2FD;--line:#CFE8FF;--ok:#1B5E20;--warn:#E65100;--alert:#B71C1C}"
"html,body{height:100%}body{margin:0;background:var(--bg);color:var(--text);font:14px/1.45 -apple-system,BlinkMacSystemFont,Segoe UI,Roboto,Helvetica,Arial,sans-serif}"
".wrap{max-width:980px;margin:0 auto;padding:16px}"
".card{background:#fff;border:1px solid var(--line);box-shadow:0 6px 24px rgba(16,38,73,.06);border-radius:14px;padding:16px}"
"header.top{position:sticky;top:0;z-index:10;background:linear-gradient(180deg,#dff2ff,rgba(223,242,255,.6));border-bottom:1px solid var(--line)}"
"header .inner{max-width:980px;margin:0 auto;padding:12px 16px;display:flex;align-items:center;gap:12px;flex-wrap:wrap}"
"h1{font-size:18px;margin:0;color:var(--primary)}"
"nav{display:flex;gap:8px;flex-wrap:wrap}"
".btn{display:inline-flex;align-items:center;gap:6px;padding:8px 12px;border-radius:10px;border:1px solid var(--line);background:var(--primary-050);color:var(--primary);text-decoration:none;font-weight:600;transition:.15s;outline:0}"
".btn:hover{transform:translateY(-1px);box-shadow:0 4px 18px rgba(25,118,210,.18)}"
".btn:focus{box-shadow:0 0 0 3px rgba(25,118,210,.25)}"
".btn.primary{background:var(--primary);border-color:var(--primary);color:#fff}"
".btn.primary:hover{background:var(--primary-600)}"
".btn.ghost{background:#fff;color:var(--primary)}"
".btn.pill{border-radius:999px;padding:6px 12px;font-weight:600}"
".btn.small{padding:6px 10px;font-size:12px}"
".btn.active{outline:0;box-shadow:0 0 0 3px rgba(25,118,210,.25)}"
".btn svg{width:16px;height:16px;display:inline-block;fill:currentColor}"
".spacer{flex:1}"
".toolbar{display:flex;gap:8px;align-items:center;flex-wrap:wrap}"
".switch{position:relative;display:inline-flex;align-items:center;gap:8px;color:var(--muted);font-weight:600}"
".switch input{appearance:none;width:44px;height:24px;border-radius:20px;background:#cfe8ff;position:relative;outline:none;cursor:pointer;transition:.2s;border:1px solid var(--line)}"
".switch input:checked{background:var(--primary)}"
".switch input::after{content:'';position:absolute;top:2px;left:2px;width:18px;height:18px;background:#fff;border-radius:50%;transition:.2s;box-shadow:0 1px 2px rgba(0,0,0,.2)}"
".switch input:checked::after{left:22px}"
"table{border-collapse:separate;border-spacing:0;width:100%;overflow:hidden;border:1px solid var(--line);border-radius:12px}"
"th,td{padding:10px 12px;border-bottom:1px solid var(--line);vertical-align:top}"
"th{background:#dff2ff;text-align:left;color:#0b4c8c;font-weight:700}"
"tbody tr:nth-child(even){background:var(--bg-2)}"
"tbody tr:hover{background:#dff2ff66}"
"code{font-family:ui-monospace,Menlo,Consolas,monospace;white-space:pre-wrap;word-break:break-word}"
".hint{color:var(--muted);font-size:12px}"
".chips{display:flex;gap:6px;flex-wrap:wrap}"
".chip{display:inline-block;padding:4px 8px;border-radius:999px;font-size:12px;border:1px solid var(--line);background:#fff;color:var(--muted)}"
".chip.ok{border-color:#A5D6A7;color:var(--ok);background:#E8F5E9}"
".chip.warn{border-color:#FFCC80;color:var(--warn);background:#FFF3E0}"
".chip.alert{border-color:#EF9A9A;color:var(--alert);background:#FFEBEE}"
"footer{margin-top:16px;color:var(--muted);font-size:12px}"
"</style>"
"<header class='top'><div class='inner'>"
"<h1>FloodGuard</h1>"
"<nav>"
"<a class='btn primary' data-path='/' href='/' aria-label='All logs'>"
"<svg viewBox='0 0 24 24' aria-hidden='true'><path d='M4 6h16v2H4V6zm0 5h16v2H4v-2zm0 5h16v2H4v-2z'/></svg>All</a>"
"<a class='btn' data-path='/tx' href='/tx' aria-label='TX logs'>"
"<svg viewBox='0 0 24 24' aria-hidden='true'><path d='M5 20h14v-2H5v2zM12 3 7 8h3v4h4V8h3l-5-5z'/></svg>TX</a>"
"<a class='btn' data-path='/rx' href='/rx' aria-label='RX logs'>"
"<svg viewBox='0 0 24 24' aria-hidden='true'><path d='M12 2v10l-4-4-1.4 1.4L12 16l5.4-6.6L16 8l-4 4V2zM5 20h14v-2H5v2z'/></svg>RX</a>"
"<a class='btn ghost' data-path='/diag' href='/diag' aria-label='Diagnostics'>"
"<svg viewBox='0 0 24 24' aria-hidden='true'><path d='M12 2a10 10 0 1 0 0 20 10 10 0 0 0 0-20zm1 15h-2v-6h2v6zm0-8h-2V7h2v2z'/></svg>Diag</a>"
"</nav>"
"<div class='spacer'></div>"
"<div class='toolbar'>"
"<a class='btn pill small' href='/export.csv?view=all' title='Download all logs (CSV)' aria-label='CSV All'>"
"<svg viewBox='0 0 24 24' aria-hidden='true'><path d='M3 5h18v14H3V5zm2 2v2h4V7H5zm6 0v2h4V7h-4zm6 0v2h2V7h-2zM5 11v2h4v-2H5zm6 0v2h4v-2h-4zm6 0v2h2v-2h-2zM5 15v2h4v-2H5zm6 0v2h4v-2h-4zm6 0v2h2v-2h-2z'/></svg>CSV All</a>"
"<a class='btn pill small' href='/export.csv?view=tx'  title='Download TX logs (CSV)' aria-label='CSV TX'>"
"<svg viewBox='0 0 24 24' aria-hidden='true'><path d='M3 5h18v14H3V5zm2 2v2h4V7H5zm6 0v2h4V7h-4zm6 0v2h2V7h-2zM5 11v2h4v-2H5zm6 0v2h4v-2h-4zm6 0v2h2v-2h-2zM5 15v2h4v-2H5zm6 0v2h4v-2h-4zm6 0v2h2v-2h-2z'/></svg>CSV TX</a>"
"<a class='btn pill small' href='/export.csv?view=rx'  title='Download RX logs (CSV)' aria-label='CSV RX'>"
"<svg viewBox='0 0 24 24' aria-hidden='true'><path d='M3 5h18v14H3V5zm2 2v2h4V7H5zm6 0v2h4V7h-4zm6 0v2h2V7h-2zM5 11v2h4v-2H5zm6 0v2h4v-2h-4zm6 0v2h2v-2h-2z'/></svg>CSV RX</a>"
"<a class='btn pill small' href='/tg_test'  title='Send Telegram test' aria-label='Telegram test'>"
"<svg viewBox='0 0 24 24' aria-hidden='true'><path d='M2 21l21-9L2 3v7l15 2-15 2v7z'/></svg>TG Test</a>"
"<a class='btn pill small' href='/tg_debug' title='Telegram debug' aria-label='Telegram debug'>"
"<svg viewBox='0 0 24 24' aria-hidden='true'><path d='M3 6h14v2H3V6zm0 5h18v2H3v-2zm0 5h10v2H3v-2z'/></svg>TG Debug</a>"
"<label class='switch' title='Auto refresh pages'><input id='ar' type='checkbox' aria-label='Auto refresh'><span>Auto&nbsp;refresh</span></label>"
"</div>"
"</div></header>"
"<div class='wrap'><div class='card'>"
);
  s += F(
"<script>(function(){"
"var params=new URLSearchParams(location.search);"
"var on=params.get('autorefresh'); on=(on===null||on==='1');"
"var ar=document.getElementById('ar'); if(ar){ ar.checked=on; ar.addEventListener('change',function(){params.set('autorefresh', ar.checked?'1':'0'); location.search=params.toString();}); }"
"if(on){ setTimeout(function(){ location.reload(); },3000); }"
"document.querySelectorAll('nav a[data-path]').forEach(function(a){ if(a.getAttribute('data-path')===location.pathname){ a.classList.add('active'); } });"
"})();</script>"
  );
  s += "<h2 style='margin:8px 0 14px 0;'>" + t + "</h2>";
  return s;
}

String table(const String& t, String a[], size_t h, size_t c){
  String s = header(t);
  s += "<table><thead><tr><th style='width:60px'>#</th><th>Entry</th></tr></thead><tbody>";
  for(size_t i=0;i<c;i++){
    size_t idx=(h+MAX_LOGS-c+i)%MAX_LOGS;
    s += "<tr><td>"+String(i+1)+"</td><td><code>"+a[idx]+"</code></td></tr>";
  }
  s += "</tbody></table><footer><div class='chips'>"
       "<span class='chip ok'>Wi-Fi "+String(WiFi.status()==WL_CONNECTED?"OK":"…")+"</span>"
       "<span class='chip'>Peers "+String(mesh.getNodeList().size())+"</span>"
       "<span class='chip'>Node "+String(mesh.getNodeId())+"</span>"
       "<span class='chip'>Time "+fmtNow()+"</span>"
       "</div><div class='hint'>Tip: pause Auto refresh to inspect logs without refresh jumps.</div></footer>"
       "</div></div>";
  return s;
}

bool authGuard(){ if(!server.authenticate(AUTH_USER,AUTH_PASS)){ server.requestAuthentication(); return false; } return true; }

//////////////////////////////////////////////////// CSV //////////////////////////////////////////////////////
String csvEscape(const String& in){ String out="\""; for(size_t i=0;i<in.length();++i){ char c=in[i]; if(c=='\"') out+="\"\""; else if(c=='\r'||c=='\n') out+=' '; else out+=c; } return out+"\""; }
void buildCSV(String& out,const char* view,String a[],size_t h,size_t c){ out.reserve(2048); out="index,view,entry\r\n"; for(size_t i=0;i<c;i++){ size_t idx=(h+MAX_LOGS-c+i)%MAX_LOGS; out+=String(i+1); out+=","; out+=view; out+=","; out+=csvEscape(a[idx]); out+="\r\n"; } }

/////////////////////////////////////////////////////////// HTTP handlers ///////////////////////////////////////////////
void hAll(){ if(!authGuard())return; server.send(200,"text/html",table("All Logs",allLogs,allHead,allCount)); }
void hTX (){ if(!authGuard())return; server.send(200,"text/html",table("TX Logs", txLogs, txHead, txCount)); }
void hRX (){ if(!authGuard())return; server.send(200,"text/html",table("RX Logs", rxLogs, rxHead, rxCount)); }
void hDiag(){
  if(!authGuard())return;
  String s = header("Diagnostics");
  s += "<table><tbody>";
  s += "<tr><th style='width:220px'>IP</th><td><code>"+WiFi.localIP().toString()+"</code></td></tr>";
  s += "<tr><th>NodeID</th><td><code>"+String(mesh.getNodeId())+"</code></td></tr>";
  s += "<tr><th>Peers</th><td><code>"+String(mesh.getNodeList().size())+"</code></td></tr>";
  s += "<tr><th>Max logs/page</th><td><code>"+String(MAX_LOGS)+"</code></td></tr>";
  s += "<tr><th>LCD</th><td><code>"+String(lcd?"OK":"NOT FOUND")+"</code></td></tr>";
  s += "<tr><th>Time synced</th><td><code>"+String(timeSynced?"yes":"no")+"</code></td></tr>";
  s += "<tr><th>Now</th><td><code>"+fmtNow()+"</code></td></tr>";
  s += "<tr><th>TG queue</th><td><code>"+String(tgq_count)+" pending</code></td></tr>";
  s += "</tbody></table><footer class='hint'>Theme: Light Blue • Buttons have icons + hover/focus states • Telegram sends are queued in loop() to prevent resets.</footer></div></div>";
  server.send(200,"text/html",s);
}
void hExportCSV(){
  if(!authGuard())return; String v=server.hasArg("view")?server.arg("view"):"all"; String csv;
  if(v=="tx") buildCSV(csv,"tx",txLogs,txHead,txCount);
  else if(v=="rx") buildCSV(csv,"rx",rxLogs,rxHead,rxCount);
  else buildCSV(csv,"all",allLogs,allHead,allCount);
  server.sendHeader("Content-Disposition","attachment; filename=\"floodguard_"+v+".csv\"");
  server.send(200,"text/csv",csv);
}

// Telegram test/debug (these use direct send because they are manual)
void hTgTest(){
  if(!authGuard()) return;
  bool ok = telegramSend("FloodGuard RX1: /tg_test @ " + fmtNow());
  String out;
  out += ok ? "ok\n" : "failed\n";
  out += "WiFi: " + String(WiFi.status()==WL_CONNECTED?"up":"down") + " (RSSI " + String(WiFi.RSSI()) + " dBm)\n";
  out += "HTTP: " + String(tg_last_http) + "\n";
  out += "Step: " + tg_last_step + "\n";
  out += "DNS:  " + tg_last_dns + "\n";
  out += "Resp: " + tg_last_resp + "\n";
  out += "Err:  " + tg_last_err + "\n";
  server.send(200, "text/plain", out);
}
void hTgDebug(){
  if(!authGuard()) return;
  String masked = String(TG_BOT_TOKEN);
  if (masked.length() > 12) masked = masked.substring(0,6) + "..." + masked.substring(masked.length()-6);
  String out;
  out += "WiFi: " + String(WiFi.status()==WL_CONNECTED?"up":"down") + " (RSSI " + String(WiFi.RSSI()) + " dBm)\n";
  out += "BotToken: " + masked + "\n";
  out += "ChatID: " + String(TG_CHAT_ID) + "\n";
  out += "LastHTTP: " + String(tg_last_http) + "\n";
  out += "LastStep: " + String(tg_last_step) + "\n";
  out += "LastDNS:  " + tg_last_dns + "\n";
  out += "LastResp: " + tg_last_resp + "\n";
  out += "LastErr:  " + tg_last_err + "\n";
  out += "Now: " + fmtNow() + "\n";
  server.send(200, "text/plain", out);
} 

// Accept GET/POST from TX: /ingest?node=tx&type=ALERT|WARNING&msg=...
void hIngest(){
  String node = server.hasArg("node") ? server.arg("node") : "";
  String type = server.hasArg("type") ? server.arg("type") : "";
  String msg  = server.hasArg("msg")  ? server.arg("msg")  : "";
  if((!server.hasArg("node")||!server.hasArg("type")) && server.hasArg("plain")){
    String b=server.arg("plain"); int a,e;
    a=b.indexOf("node="); if(a>=0){ e=b.indexOf('&',a); node=b.substring(a+5, e==-1?b.length():e); }
    a=b.indexOf("type="); if(a>=0){ e=b.indexOf('&',a); type=b.substring(a+5, e==-1?b.length():e); }
    a=b.indexOf("msg=" ); if(a>=0){ e=b.indexOf('&',a); msg =b.substring(a+4, e==-1?b.length():e); }
  }
  if(node=="") node="unknown"; if(type=="") type="INFO";

  String line="["+nowTag()+"] "+node+" "+type+" :: "+msg;
  addAll(line); if(node=="tx") addTX(line); if(node=="rx"||node=="rx2") addRX(line);

  String up=type; up.toUpperCase();
  if(up.indexOf("ALERT")!=-1){
    pulseRed(); onAlertOrWarningLCD("ALERT", msg);
    tgq_enqueue("ALERT from "+node+" @ "+fmtNow()+": "+msg);   // queue instead of direct send
  } else if(up.indexOf("WARNING")!=-1){
    if(node=="tx"){ pulseYel(); }
    onAlertOrWarningLCD("WARNING", msg);
    tgq_enqueue("WARNING from "+node+" @ "+fmtNow()+": "+msg); // queue instead of direct send
  }
  server.send(200,"text/plain","ok");
}

// Blink built-in LED briefly on any mesh msg
void blink(){ digitalWrite(LED_BUILTIN, HIGH); delay(30); digitalWrite(LED_BUILTIN, LOW); }

//////////////////////////////Mesh callbacks /////////////////////////////////////////////////////////
void onNewConnection(uint32_t nodeId){ Serial.printf("[mesh] NEW connection: %u\n", nodeId); }
void onDroppedConnection(uint32_t nodeId){ Serial.printf("[mesh] DROPPED: %u\n", nodeId); }
void changedConnections(){ Serial.print("[mesh] CHANGED peers: "); for(auto &id: mesh.getNodeList(true)) Serial.print(String(id)+" "); Serial.println(); }

// Normalize & log mesh messages (from both RX2 and TX), then queue Telegram afterwards!!!!!
void received(uint32_t from, String &msg){
  blink();
  String up=msg; up.toUpperCase();
  String type="INFO"; if(up.indexOf("ALERT")!=-1) type="ALERT"; else if(up.indexOf("WARNING")!=-1) type="WARNING";
  String src="rx"; if(up.startsWith("HELLO:RX2")||up.indexOf("[RPT:")!=-1||up.indexOf("[BTN:")!=-1) src="rx2";
  String line="["+nowTag()+"] "+src+" "+type+" :: "+msg;
  addAll(line); addRX(line);
  if(type=="ALERT"){ pulseRed(); onAlertOrWarningLCD("ALERT", msg); tgq_enqueue("ALERT via mesh ("+src+") @ "+fmtNow()+": "+msg); }
  else if(type=="WARNING"){ onAlertOrWarningLCD("WARNING", msg); tgq_enqueue("WARNING via mesh ("+src+") @ "+fmtNow()+": "+msg); }
}

void setup(){
  Serial.begin(115200); delay(100);
  Serial.println("\n=== RX Type-1 (root + LCD + LEDs + logs + Telegram QUEUED + Themed UI + Icons) ===");
  pinMode(LED_BUILTIN, OUTPUT); digitalWrite(LED_BUILTIN, LOW); // ON
  pinMode(RED_LED_PIN, OUTPUT); setRed(false);
  pinMode(YEL_LED_PIN, OUTPUT); setYel(false);

  for(size_t i=0;i<MAX_LOGS;i++){ allLogs[i].reserve(160); txLogs[i].reserve(160); rxLogs[i].reserve(160); }

  Wire.begin(I2C_SDA,I2C_SCL);
  if(i2cProbe(LCD_ADDR_1)) lcd=&lcd27; else if(i2cProbe(LCD_ADDR_2)) lcd=&lcd3F; else lcd=nullptr;
  if(lcd){ lcd->init(); lcd->backlight(); lcd->clear(); lcd->setCursor(0,0); lcd->print("FloodGuard RX1"); lcd->setCursor(0,1); lcd->print("LCD Ready"); }
  else { Serial.println("[lcd] Not found (0x27/0x3F)"); }

  mesh.setDebugMsgTypes(ERROR | STARTUP | CONNECTION | COMMUNICATION);
  mesh.init(MESH_PREFIX, MESH_PASSWORD, MESH_PORT);
  mesh.setRoot(true); mesh.setContainsRoot(true);
  mesh.onReceive(&received); mesh.onNewConnection(&onNewConnection);
  mesh.onDroppedConnection(&onDroppedConnection); mesh.onChangedConnections(&changedConnections);

  mesh.stationManual(WIFI_SSID, WIFI_PASS);
  mesh.setHostname("FloodGuard-RX1");

  server.on("/",HTTP_GET, hAll);
  server.on("/tx",HTTP_GET, hTX);
  server.on("/rx",HTTP_GET, hRX);
  server.on("/diag",HTTP_GET, hDiag);
  server.on("/ingest",HTTP_ANY, hIngest);
  server.on("/export.csv",HTTP_GET, hExportCSV);
  server.on("/tg_test",HTTP_GET, hTgTest);
  server.on("/tg_debug",HTTP_GET, hTgDebug);
  server.begin();
  Serial.println("[http] Server started on :80");
}

void loop(){
  mesh.update();
  server.handleClient();
  ensureTimeSyncNonBlocking();

  // Flush Telegram queue from loop (safe, non-blocking pacing)
  tgq_flush();

  static bool shown=false;
  if(!shown && WiFi.status()==WL_CONNECTED){ Serial.print("[wifi] IP: "); Serial.println(WiFi.localIP()); shown=true; }

  static unsigned long lastBeat=0, nowMs=0; nowMs=millis();
  if(nowMs-lastBeat>=RX_HEARTBEAT_INTERVAL_MS){ lastBeat=nowMs; mesh.sendBroadcast(String("HELLO:RX1:")+mesh.getNodeId()); }

  static unsigned long lastDbg=0;
  if(nowMs-lastDbg>=2000){
    lastDbg=nowMs;
    Serial.printf("[dbg] %s peers=%u wifi=%s RSSI=%lddBm logs(all/tx/rx)=%u/%u/%u lcd=%s red=%s yel=%s tgq=%u\n",
      fmtNow().c_str(), (unsigned)mesh.getNodeList().size(),
      (WiFi.status()==WL_CONNECTED?"up":"down"), WiFi.RSSI(),
      (unsigned)allCount,(unsigned)txCount,(unsigned)rxCount,
      (lcd?"ok":"none"), (nowMs<redUntil?"ON":"OFF"), (nowMs<yelUntil?"ON":"OFF"),
      tgq_count);
    if (lcd && lcdState.active && nowMs > lcdState.until) lcdShowIdle();
  }
  if(nowMs >= redUntil) setRed(false);
  if(nowMs >= yelUntil) setYel(false);
}
