/*
  =====================================================
   SWARMFORGE - SLAVE  (ESP8266, ESP-NOW)
  =====================================================
  Board: NodeMCU 1.0 (ESP-12E Module)  | Core: esp8266 3.1.2

  What it does:
   - Receives commands from the MASTER via ESP-NOW
   - Changes a SIMULATED robot state (no motors!)
   - Sends an ACK + status back, using the same packetId
   - Returns to IDLE a few seconds after a movement command
*/

#include <ESP8266WiFi.h>
#include <espnow.h>
extern "C" {
  #include <user_interface.h>     // for wifi_set_channel()
}

#define WIFI_CHANNEL     1        // MUST be the same in Master.ino
#define TASK_DURATION_MS 5000UL   // simulated movement lasts 5 s, then IDLE

// ---------- Packets (must be identical in Master.ino) ----------
struct CommandPacket {
  uint32_t packetId;
  char     command[24];
};

struct StatusPacket {
  uint32_t packetId;
  bool     acknowledged;
  char     status[24];
};

// ---------- Robot state ----------
char          state[24] = "IDLE";
bool          timedState = false;   // true while "moving" (will return to IDLE)
unsigned long stateStart = 0;

// ---------- Remember the master (so we can send updates by ourselves) ----------
uint8_t masterMac[6];
bool    haveMaster = false;

// ---------- Receive queue ----------
#define RX_Q 4
struct RxItem {
  uint8_t       mac[6];
  CommandPacket pkt;
};
volatile uint8_t rxHead = 0, rxTail = 0;
RxItem rxQueue[RX_Q];

void onDataRecv(uint8_t *mac, uint8_t *data, uint8_t len) {
  if (len != sizeof(CommandPacket)) return;
  uint8_t next = (rxHead + 1) % RX_Q;
  if (next == rxTail) return;                     // queue full, drop
  memcpy(rxQueue[rxHead].mac, mac, 6);
  memcpy(&rxQueue[rxHead].pkt, data, sizeof(CommandPacket));
  rxHead = next;
}

// ---------- Helpers ----------
void printBanner() {
  Serial.println();
  Serial.println("================================");
  Serial.println("        SWARMFORGE SLAVE");
  Serial.println("================================");
  Serial.println();
}

void ensurePeer(const uint8_t* mac) {
  if (!esp_now_is_peer_exist((uint8_t*)mac)) {
    esp_now_add_peer((uint8_t*)mac, ESP_NOW_ROLE_COMBO, WIFI_CHANNEL, NULL, 0);
  }
}

void sendStatus(const uint8_t* mac, uint32_t id, bool ok) {
  StatusPacket s;
  memset(&s, 0, sizeof(s));
  s.packetId = id;
  s.acknowledged = ok;
  strncpy(s.status, state, sizeof(s.status) - 1);
  esp_now_send((uint8_t*)mac, (uint8_t*)&s, sizeof(s));
}

void setState(const char* newState, bool timed) {
  strncpy(state, newState, sizeof(state) - 1);
  timedState = timed;
  stateStart = millis();
}

// Convert a command to a simulated robot state. Returns false if unknown.
bool applyCommand(const char* cmd) {
  if (strcmp(cmd, "MOVE_FORWARD")  == 0) { setState("MOVING FORWARD",  true);  return true; }
  if (strcmp(cmd, "MOVE_BACKWARD") == 0) { setState("MOVING BACKWARD", true);  return true; }
  if (strcmp(cmd, "TURN_LEFT")     == 0) { setState("TURNING LEFT",    true);  return true; }
  if (strcmp(cmd, "TURN_RIGHT")    == 0) { setState("TURNING RIGHT",   true);  return true; }
  if (strcmp(cmd, "GO_TO_ZONE_A")  == 0) { setState("GOING TO ZONE A", true);  return true; }
  if (strcmp(cmd, "GO_TO_ZONE_B")  == 0) { setState("GOING TO ZONE B", true);  return true; }
  if (strcmp(cmd, "STOP")          == 0) { setState("STOPPED",         false); return true; }
  return false;
}

void processPacket(RxItem &it) {
  it.pkt.command[sizeof(it.pkt.command) - 1] = 0;
  memcpy(masterMac, it.mac, 6);
  haveMaster = true;
  ensurePeer(it.mac);

  // Heartbeat: answer silently with our current state
  if (strcmp(it.pkt.command, "PING") == 0) {
    sendStatus(it.mac, 0, true);
    return;
  }

  Serial.println("COMMAND RECEIVED:");
  Serial.println(it.pkt.command);

  bool ok = applyCommand(it.pkt.command);
  if (!ok) strcpy(state, "UNKNOWN COMMAND");

  Serial.println();
  Serial.println("State:");
  Serial.println(state);
  Serial.println();
  Serial.println("Sending ACK...");

  sendStatus(it.mac, it.pkt.packetId, ok);

  Serial.println();
  Serial.println("ACK SENT ✓");
  Serial.println();
  Serial.println("Waiting for commands...");
  Serial.println();
}

// =====================================================
void setup() {
  Serial.begin(115200);
  delay(500);
  printBanner();
  Serial.println("ESP8266 INITIALIZED");

  WiFi.persistent(false);
  WiFi.setAutoConnect(false);
  WiFi.setAutoReconnect(false);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  wifi_set_channel(WIFI_CHANNEL);

  if (esp_now_init() != 0) {
    Serial.println("ESP-NOW INIT FAILED ✗  (reset the board)");
    while (true) delay(1000);
  }
  esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
  esp_now_register_recv_cb(onDataRecv);
  Serial.println("ESP-NOW READY");

  Serial.println();
  Serial.println("*** COPY THIS MAC ADDRESS INTO Master.ino ***");
  Serial.print("SLAVE MAC: ");
  Serial.println(WiFi.macAddress());

  Serial.println();
  Serial.println("Waiting for commands...");
  Serial.println();
}

// =====================================================
void loop() {
  // 1) Handle received commands
  while (rxTail != rxHead) {
    RxItem it = rxQueue[rxTail];
    rxTail = (rxTail + 1) % RX_Q;
    processPacket(it);
  }

  // 2) Simulated movement finished -> back to IDLE, tell the master
  if (timedState && millis() - stateStart >= TASK_DURATION_MS) {
    setState("IDLE", false);
    Serial.println("Task finished. State: IDLE");
    Serial.println();
    if (haveMaster) sendStatus(masterMac, 0, true);
  }
}