/*
  =====================================================
   SWARMFORGE - MASTER  (ESP8266, ESP-NOW + Web server)
  =====================================================
  Board: NodeMCU 1.0 (ESP-12E Module)  | Core: esp8266 3.1.2

  What it does:
   - Creates Wi-Fi network "SwarmForge" (password swarm1234)
   - Serves a tiny JSON API for dashboard.html
        /status          -> current state as JSON
        /cmd?c=COMMAND   -> send a command to the slave
   - Sends commands to the SLAVE with ESP-NOW and waits for an ACK
   - Pings the slave every 2 s so the dashboard knows it is online
*/

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <espnow.h>

// =====================================================
//  !!! EDIT THIS: the SLAVE's MAC address !!!
//  Upload Slave.ino first, open its Serial Monitor,
//  and copy the MAC address it prints.
// =====================================================
uint8_t slaveMac[6] = {0x10, 0x52, 0x1C, 0xE9, 0x8C, 0x3D};

// ---------- Settings ----------
#define WIFI_CHANNEL      1          // MUST be the same in Slave.ino
const char* AP_SSID = "SwarmForge";
const char* AP_PASS = "swarm1234";   // at least 8 characters
#define ACK_TIMEOUT_MS    2000UL     // wait this long for an ACK
#define PING_INTERVAL_MS  2000UL     // heartbeat to slave
#define OFFLINE_AFTER_MS  7000UL     // no reply for this long = offline

// ---------- Packets (must be identical in Slave.ino) ----------
struct CommandPacket {
  uint32_t packetId;      // number of this command (1,2,3...). 0 = heartbeat PING
  char     command[24];   // text such as "GO_TO_ZONE_A"
};

struct StatusPacket {
  uint32_t packetId;      // copy of the command's packetId (so we know which command this answers)
  bool     acknowledged;  // true = "I understood and accepted the command"
  char     status[24];    // slave's current state, e.g. "GOING TO ZONE A"
};

// ---------- Valid commands ----------
const char* VALID_COMMANDS[] = {
  "MOVE_FORWARD", "MOVE_BACKWARD", "TURN_LEFT", "TURN_RIGHT",
  "STOP", "GO_TO_ZONE_A", "GO_TO_ZONE_B"
};
const int NUM_VALID = 7;

bool isValidCommand(const char* c) {
  for (int i = 0; i < NUM_VALID; i++) {
    if (strcmp(c, VALID_COMMANDS[i]) == 0) return true;
  }
  return false;
}

// ---------- Event log (sent to dashboard) ----------
// type letters: o=online c=command s=packet sent a=ack x=status t=timeout f=failure/offline i=info e=error
#define EVENT_COUNT 14
struct Event {
  uint32_t n;          // event number 1,2,3...
  char     type;
  char     msg[40];
};
Event    events[EVENT_COUNT];
uint32_t eventSeq = 0;

void addEvent(char type, const char* msg) {
  eventSeq++;
  Event &e = events[eventSeq % EVENT_COUNT];
  e.n = eventSeq;
  e.type = type;
  strncpy(e.msg, msg, sizeof(e.msg) - 1);
  e.msg[sizeof(e.msg) - 1] = 0;
}

// ---------- State ----------
enum LinkState { LS_IDLE, LS_WAITING, LS_ACKED, LS_TIMEOUT };
LinkState linkState = LS_IDLE;

char     lastCommand[24] = "NONE";
char     slaveStatus[24] = "UNKNOWN";
uint32_t packetCounter = 0;   // increments with every command
uint32_t pendingId     = 0;   // command we are waiting for an ACK for
uint32_t cmdSent = 0, acksRcv = 0, cmdFailed = 0;
unsigned long cmdSentAt = 0;
unsigned long lastSlaveSeen = 0;   // last time ANY packet came from the slave
unsigned long lastPing = 0;
bool slaveOnline = false;

ESP8266WebServer server(80);

// ---------- Receive queue (callback only stores, loop() processes) ----------
#define RX_Q 4
volatile uint8_t rxHead = 0, rxTail = 0;
StatusPacket rxQueue[RX_Q];

void onDataRecv(uint8_t *mac, uint8_t *data, uint8_t len) {
  if (len != sizeof(StatusPacket)) return;
  uint8_t next = (rxHead + 1) % RX_Q;
  if (next == rxTail) return;                    // queue full, drop
  memcpy(&rxQueue[rxHead], data, sizeof(StatusPacket));
  rxHead = next;
}

// ---------- Helpers ----------
String macToString(const uint8_t* m) {
  char b[18];
  snprintf(b, sizeof(b), "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
  return String(b);
}

const char* stateName() {
  switch (linkState) {
    case LS_WAITING: return "WAITING";
    case LS_ACKED:   return "ACKED";
    case LS_TIMEOUT: return "TIMEOUT";
    default:         return "IDLE";
  }
}

void printBanner() {
  Serial.println();
  Serial.println("================================");
  Serial.println("       SWARMFORGE MASTER");
  Serial.println("================================");
  Serial.println();
}

// ---------- Send a command to the slave ----------
void startCommand(const char* cmd) {
  packetCounter++;
  pendingId = packetCounter;

  CommandPacket p;
  memset(&p, 0, sizeof(p));
  p.packetId = pendingId;
  strncpy(p.command, cmd, sizeof(p.command) - 1);
  strncpy(lastCommand, cmd, sizeof(lastCommand) - 1);

  Serial.println();
  Serial.println("Sending:");
  Serial.println(cmd);

  int result = esp_now_send(slaveMac, (uint8_t*)&p, sizeof(p));

  cmdSent++;
  cmdSentAt = millis();
  linkState = LS_WAITING;

  char msg[40];
  snprintf(msg, sizeof(msg), "COMMAND → %s", cmd);
  addEvent('c', msg);

  if (result == 0) {
    Serial.println();
    Serial.println("Packet sent ✓");
    addEvent('s', "PACKET SENT");
  } else {
    Serial.println();
    Serial.println("Packet send error ✗ (will time out)");
    addEvent('e', "PACKET SEND ERROR");
  }
  Serial.println();
  Serial.println("Waiting for ACK...");
}

// ---------- Heartbeat ping ----------
void sendPing() {
  CommandPacket p;
  memset(&p, 0, sizeof(p));
  p.packetId = 0;
  strcpy(p.command, "PING");
  esp_now_send(slaveMac, (uint8_t*)&p, sizeof(p));
}

// ---------- Handle a packet that came from the slave ----------
void handleStatus(StatusPacket &s) {
  s.status[sizeof(s.status) - 1] = 0;
  lastSlaveSeen = millis();
  if (lastSlaveSeen == 0) lastSlaveSeen = 1;

  bool statusChanged = (strcmp(slaveStatus, s.status) != 0);
  strncpy(slaveStatus, s.status, sizeof(slaveStatus) - 1);

  if (s.packetId != 0 && linkState == LS_WAITING && s.packetId == pendingId) {
    // This is the ACK for our command
    acksRcv++;
    linkState = LS_ACKED;

    Serial.println();
    Serial.println("ACK RECEIVED ✓");
    Serial.println();
    Serial.println("Slave status:");
    Serial.println(slaveStatus);

    addEvent('a', "ACK ← SLAVE");
    char msg[40];
    snprintf(msg, sizeof(msg), "STATUS → %s", slaveStatus);
    addEvent('x', msg);
  }
  else if (s.packetId == 0 && statusChanged) {
    // Heartbeat reply / slave changed state by itself (e.g. finished moving -> IDLE)
    Serial.println();
    Serial.print("Slave status update: ");
    Serial.println(slaveStatus);
    char msg[40];
    snprintf(msg, sizeof(msg), "STATUS → %s", slaveStatus);
    addEvent('x', msg);
  }
}

// ---------- Web handlers ----------
void sendCors() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Cache-Control", "no-store");
}

void handleRoot() {
  sendCors();
  server.send(200, "text/plain",
              "SwarmForge MASTER is running.\nOpen dashboard.html on your laptop while connected to this Wi-Fi.");
}

void handleStatus() {
  String json;
  json.reserve(1800);
  json += "{\"slaveOnline\":";
  json += slaveOnline ? "true" : "false";
  json += ",\"state\":\"";       json += stateName();
  json += "\",\"lastCommand\":\""; json += lastCommand;
  json += "\",\"slaveStatus\":\""; json += slaveStatus;
  json += "\",\"sent\":";        json += cmdSent;
  json += ",\"acks\":";          json += acksRcv;
  json += ",\"failed\":";        json += cmdFailed;
  json += ",\"packetId\":";      json += packetCounter;
  json += ",\"events\":[";

  uint32_t first = (eventSeq > EVENT_COUNT) ? (eventSeq - EVENT_COUNT + 1) : 1;
  bool comma = false;
  for (uint32_t i = first; i <= eventSeq; i++) {
    Event &e = events[i % EVENT_COUNT];
    if (e.n != i) continue;
    if (comma) json += ",";
    json += "{\"n\":"; json += i;
    json += ",\"t\":\""; json += e.type;
    json += "\",\"m\":\""; json += e.msg;
    json += "\"}";
    comma = true;
  }
  json += "]}";

  sendCors();
  server.send(200, "application/json; charset=utf-8", json);
}

void handleCmd() {
  sendCors();
  String c = server.arg("c");
  if (!isValidCommand(c.c_str())) {
    server.send(400, "application/json", "{\"ok\":false,\"msg\":\"unknown command\"}");
    return;
  }
  if (linkState == LS_WAITING) {
    server.send(409, "application/json", "{\"ok\":false,\"msg\":\"busy, waiting for ACK\"}");
    return;
  }
  startCommand(c.c_str());
  server.send(200, "application/json", "{\"ok\":true}");
}

// =====================================================
void setup() {
  Serial.begin(115200);
  delay(500);
  printBanner();
  Serial.println("ESP8266 INITIALIZED");

  // Wi-Fi: Access Point (for the browser) + Station (needed by ESP-NOW)
  WiFi.persistent(false);
  WiFi.setAutoConnect(false);
  WiFi.setAutoReconnect(false);
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASS, WIFI_CHANNEL);
  WiFi.disconnect();

  // ESP-NOW
  if (esp_now_init() != 0) {
    Serial.println("ESP-NOW INIT FAILED ✗  (reset the board)");
    while (true) delay(1000);
  }
  esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
  esp_now_register_recv_cb(onDataRecv);
  esp_now_add_peer(slaveMac, ESP_NOW_ROLE_COMBO, WIFI_CHANNEL, NULL, 0);
  Serial.println("ESP-NOW READY");

  // Web server
  server.on("/", handleRoot);
  server.on("/status", []() { handleStatus(); });
  server.on("/cmd", handleCmd);
  server.begin();

  Serial.println();
  Serial.print("Wi-Fi network : "); Serial.println(AP_SSID);
  Serial.print("Password      : "); Serial.println(AP_PASS);
  Serial.print("Dashboard IP  : "); Serial.println(WiFi.softAPIP());
  Serial.print("Slave MAC     : "); Serial.println(macToString(slaveMac));
  Serial.println();
  Serial.println("Looking for slave...");

  addEvent('o', "MASTER ONLINE");
}

// =====================================================
void loop() {
  server.handleClient();

  // 1) Process packets received from the slave
  while (rxTail != rxHead) {
    StatusPacket s = rxQueue[rxTail];
    rxTail = (rxTail + 1) % RX_Q;
    handleStatus(s);
  }

  // 2) ACK timeout
  if (linkState == LS_WAITING && millis() - cmdSentAt >= ACK_TIMEOUT_MS) {
    linkState = LS_TIMEOUT;
    cmdFailed++;
    lastSlaveSeen = 0;                       // treat slave as gone
    Serial.println();
    Serial.println("TIMEOUT ✗");
    Serial.println();
    Serial.println("SLAVE NOT RESPONDING ✗");
    addEvent('t', "TIMEOUT ✗");
    addEvent('f', "SLAVE NOT RESPONDING");
  }

  // 3) Online / offline detection
  bool nowOnline = (lastSlaveSeen != 0) && (millis() - lastSlaveSeen < OFFLINE_AFTER_MS);
  if (nowOnline && !slaveOnline) {
    slaveOnline = true;
    Serial.println();
    Serial.println("SLAVE FOUND");
    addEvent('o', "SLAVE ONLINE");
  } else if (!nowOnline && slaveOnline) {
    slaveOnline = false;
    Serial.println();
    Serial.println("SLAVE OFFLINE ✗");
    addEvent('f', "SLAVE OFFLINE");
  }

  // 4) Heartbeat (not while waiting for a command ACK)
  if (linkState != LS_WAITING && millis() - lastPing >= PING_INTERVAL_MS) {
    lastPing = millis();
    sendPing();
  }

  // 5) Optional: type a command name in the Serial Monitor to send it without the dashboard
  if (Serial.available()) {
    String line = Serial.readStringUntil('\n');
    line.trim();
    line.toUpperCase();
    if (isValidCommand(line.c_str()) && linkState != LS_WAITING) startCommand(line.c_str());
  }
}