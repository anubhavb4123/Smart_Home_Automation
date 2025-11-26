// =========================================================
// ESP8266 — Full merged code (updated)
// Features:
//  - Safe serial parsing using <...> packets
//  - Telegram bot (admin + guest login system)
//  - Commands: /status /ledon /ledoff /fanon /fanoff /switchon /switchoff /whoami /help /logins
//  - Power-source detection with debounce and admin alert
//  - WiFi auto-reconnect
//  - Broadcast helper
//  - New admin-only commands: /remove <chat_id>, /addguest <chat_id> <name>
// =========================================================

#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <SoftwareSerial.h>
#include <map>

// ====== WiFi & Telegram Config ======
const char* ssid = "BAJPAI_2.4Ghz";
const char* password = "44444422";

#define BOT_TOKEN "8001347460:AxOg4j-1vmbM52hSsK72E"
#define ADMIN_CHAT_ID "18352"
#define LOGIN_PASSWORD "423"

// ====== Objects ======
WiFiClientSecure secured_client;
UniversalTelegramBot bot(BOT_TOKEN, secured_client);

// ====== Serial Communication ======
#define RX_PIN D6  // RX for ESP8266 (connect to TX of Arduino)
#define TX_PIN D5  // TX for ESP8266 (not always used)
SoftwareSerial Serial2(RX_PIN, TX_PIN);

// ====== GPIO ======
#define LED_PIN D1
#define FAN_PIN D2
#define SWITCH_PIN D3
#define POWER_SOURCE_PIN D7  // HIGH: supply; LOW: battery

enum PowerSource { UNKNOWN, BATTERY, SUPPLY };
PowerSource currentPowerSource = UNKNOWN;
PowerSource lastPowerSource = UNKNOWN;

unsigned long lastChangeTime = 0;   // when a potential change was first detected
const unsigned long debounceDelay = 2000; // 2 seconds stability check

// ====== Sensor Data (globals used across functions) ======
String serialData = "";
bool receivingPacket = false;

int hourVal = 0, minuteVal = 0, secondVal = 0;
int dayVal = 0, monthVal = 0, yearVal = 0;
float tDHT = 0.0, h = 0.0, tBHP = 0.0, pVal = 0.0;
float mqVal = 0.0;

// ====== Morning broadcast tracking ======
int lastMorningSentDay = -1;    // day number when 6:30 message was last sent (prevents repeats)
int lastNightSentDay = -1;    // day number when 12:00 AM message was last sent (prevents repeats)
bool morningBroadcastEnabled = true; // flip to false to disable feature quickly
bool nightBroadcastEnabled = true; // flip to false to disable feature quickly

// ====== Alerts / Flags from sensor =========
bool sentPoorAlert = false;

// ====== Timers ======
unsigned long lastBotCheck = 0;
unsigned long lastWiFiCheck = 0;
const unsigned long botInterval = 800;      // check bot every ~0.8s
const unsigned long wifiCheckInterval = 10000; // check wifi every 10s

// Track logged-in guests and their friendly names
std::map<String, bool> guestLoggedIn;
std::map<String, String> guestNames;

// Forward declarations
void parseSensorData(String data);
void handleMessages(int num);
void processCommand(String chat_id, String text);
String getSenderName(int msgIndex, const String &fallbackChatId);
void broadcastToLoggedIn(String msg);
bool isValidChatId(const String &s);

// ====== Setup ======
void setup() {
  Serial.begin(115200);
  Serial2.begin(9600); // from Arduino
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH); // LED OFF (active LOW)

  pinMode(LED_PIN, OUTPUT);
  pinMode(FAN_PIN, OUTPUT);
  pinMode(SWITCH_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW); // LED off
  digitalWrite(FAN_PIN, LOW);
  digitalWrite(SWITCH_PIN, LOW);

  Serial.println("\n📶 Connecting to WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  secured_client.setInsecure(); // for TLS without certificate validation

  // LED blink while connecting
  while (WiFi.status() != WL_CONNECTED) {
    digitalWrite(LED_BUILTIN, LOW);
    delay(200);
    digitalWrite(LED_BUILTIN, HIGH);
    delay(200);
    Serial.print(".");
  }

  Serial.println("\n✅ WiFi Connected!");
  Serial.print("📡 IP: ");
  Serial.println(WiFi.localIP());

  digitalWrite(LED_BUILTIN, LOW); // LED ON (system running)
  bot.sendMessage(ADMIN_CHAT_ID, "🚀 ESP8266 is online & ready!", "");

  // Read initial power source
  pinMode(POWER_SOURCE_PIN, INPUT); // HIGH: supply; LOW: battery
  currentPowerSource = digitalRead(POWER_SOURCE_PIN) ? BATTERY : SUPPLY;
  lastPowerSource = currentPowerSource;
  if (currentPowerSource == SUPPLY) Serial.println("🔌 Power source: SUPPLY (external)");
  else Serial.println("🔋 Power source: BATTERY");
}

// ====== Main Loop ======
void loop() {
  // --- Read sensor data from Serial2 using <...> markers ---
  while (Serial2.available()) {
    char c = Serial2.read();

    if (c == '<') {
      receivingPacket = true;
      serialData = "";
    }
    else if (c == '>') {
      receivingPacket = false;
      serialData.trim();
      if (serialData.length() > 0) {
        parseSensorData(serialData);
      }
      serialData = "";
    }
    else if (receivingPacket) {
      // append only printable chars to be safer
      if (c >= 32 && c <= 126) serialData += c;
    }
  }

  // --- Telegram Updates ---
  if (millis() - lastBotCheck > botInterval) {
    int newMessages = bot.getUpdates(bot.last_message_received + 1);
    while (newMessages) {
      handleMessages(newMessages);
      newMessages = bot.getUpdates(bot.last_message_received + 1);
    }
    lastBotCheck = millis();
  }

  // --- WiFi Auto-Reconnect ---
  if (millis() - lastWiFiCheck > wifiCheckInterval) {
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("⚠️ WiFi Lost! Reconnecting...");
      digitalWrite(LED_BUILTIN, HIGH); // LED OFF while reconnecting
      WiFi.begin(ssid, password);
      int retry = 0;
      while (WiFi.status() != WL_CONNECTED && retry < 20) {
        delay(500);
        Serial.print(".");
        retry++;
      }
      if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\n✅ Reconnected!");
        bot.sendMessage(ADMIN_CHAT_ID, "🔁 WiFi reconnected!", "");
        digitalWrite(LED_BUILTIN, LOW); // LED ON
      }
    }
    lastWiFiCheck = millis();

    // Debug print of latest sensor values (useful for serial monitor)
    Serial.println("🕒 Time: " + String(hourVal) + ":" + String(minuteVal) + ":" + String(secondVal));
    Serial.println("📅 Date: " + String(dayVal) + "/" + String(monthVal) + "/" + String(yearVal));
    Serial.println("🌡️ DHT Temp: " + String(tDHT, 1) + " °C");
    Serial.println("💧 Humidity: " + String(h, 1) + " %");
    Serial.println("🌡️ BMP Temp: " + String(tBHP, 1) + " °C");
    Serial.println("🧭 Pressure: " + String(pVal, 1) + " hPa");
    Serial.println("🌫️ MQ135 Value: " + String((int)mqVal));
  }

  // --- Power Source Change Detection ---
 // --- Power Source Change Detection ---
 PowerSource newPower = digitalRead(POWER_SOURCE_PIN) ? BATTERY : SUPPLY;

if (newPower != currentPowerSource) {

  if (lastChangeTime == 0)
    lastChangeTime = millis();

  if (millis() - lastChangeTime > debounceDelay) {

    lastPowerSource = currentPowerSource;
    currentPowerSource = newPower;
    lastChangeTime = 0;

    String msg;
    if (currentPowerSource == SUPPLY)
      msg = "🔌 Power source switched: Now HOME on *GRID* !";
    else
      msg = "🔋 Power source switched: Now HOME on *INVERTER* !";

    Serial.println(msg);
    bot.sendMessage(ADMIN_CHAT_ID, msg, "Markdown");

    // Send alert to logged-in guests
    for (auto &u : guestLoggedIn) {
      if (u.second) {
        bot.sendMessage(u.first, msg, "Markdown");
      }
    }
  }
}
else {
  lastChangeTime = 0;
}

  // --- Daily 6:30 AM Morning Broadcast ---
  // Send once when time reaches 06:30 and hasn't been sent today
  if (morningBroadcastEnabled && yearVal > 2000) { // ensure we have a valid parsed date/time
    if (hourVal == 6 && minuteVal == 30) {
      // If lastMorningSentDay differs from today's dayVal -> send
      if (lastMorningSentDay != dayVal) {
        sendMorningSensorBroadcast();
        lastMorningSentDay = dayVal;
      }
    } else {
      // reset guard when minute passes (keeps it ready for next day)
      // (optional) we don't clear lastMorningSentDay here because it holds the day we sent
    }
  }
  // --- Daily 00:00 AM Morning Broadcast ---
  if (nightBroadcastEnabled && yearVal > 2000) { // ensure we have a valid parsed date/time
    if (hourVal == 0 && minuteVal == 00) {
      // If lastNightSentDay differs from today's dayVal -> send
      if (lastNightSentDay != dayVal) {
        sendNightSensorBroadcast();
        lastNightSentDay = dayVal;
      }
    } else {
      // reset guard when minute passes (keeps it ready for next day)
      // (optional) we don't clear lastNightSentDay here because it holds the day we sent
    }
  }


}

// ====== Message Handling ======
String getSenderName(int msgIndex, const String &fallbackChatId) {
  String name = "";
  if (bot.messages[msgIndex].from_name.length() > 0) name = bot.messages[msgIndex].from_name;
  else name = "User " + fallbackChatId;
  return name;
}

void handleMessages(int num) {
  for (int i = 0; i < num; i++) {
    String chat_id = bot.messages[i].chat_id;
    String text = bot.messages[i].text; 

    // Get a friendly name for the sender (fallback to chat id)
    String senderName = getSenderName(i, chat_id);

    bool isAdmin = (chat_id == ADMIN_CHAT_ID);
    bool loggedIn = (guestLoggedIn.count(chat_id) ? guestLoggedIn[chat_id] : false);

    if (text.startsWith("/login ")) {
      String pwd = text.substring(7);

      if (isAdmin) {
        bot.sendMessage(chat_id, "👑 Admin already has full access.", "Markdown");
      }
      else if (pwd == LOGIN_PASSWORD) {
        guestLoggedIn[chat_id] = true;
        guestNames[chat_id] = senderName;
        bot.sendMessage(chat_id, "✅ *Login successful!*", "Markdown");

        // Notify admin when a guest logs in
        String adminMsg = "🔐 Guest logged in:\n";
        adminMsg += senderName + " (" + chat_id + ")";
        bot.sendMessage(ADMIN_CHAT_ID, adminMsg, "");
        Serial.println("Admin notified: " + adminMsg);
      }
      else {
        bot.sendMessage(chat_id, "❌ *Wrong password!* Try again.", "Markdown");
      }
      continue;
    }

    //----------- Logout ----------
    if (text == "/logout") {
      // Capture name before clearing
      if (!isAdmin) {
        String nameBefore = guestNames.count(chat_id) ? guestNames[chat_id] : senderName;

        guestLoggedIn[chat_id] = false;
        guestNames[chat_id] = "";
        bot.sendMessage(chat_id, "🔒 You are now logged out.", "Markdown");

        // Notify admin when a guest (non-admin) logs out
        String adminMsg = "🔓 Guest logged out:\n";
        adminMsg += nameBefore + " (" + chat_id + ")";
        bot.sendMessage(ADMIN_CHAT_ID, adminMsg, "");
        Serial.println("Admin notified: " + adminMsg);
      } else {
        bot.sendMessage(chat_id, "👑 Admin cannot logout (always has access).", "Markdown");
      }
      continue;
    }

    // ----------- ADMIN / GUEST COMMANDS ----------
    if (isAdmin || loggedIn) {
      processCommand(chat_id, text);
      continue;
    }
    else {
      bot.sendMessage(chat_id, "🚫 Access Denied!\nLogin with `/login your_password`", "Markdown");
    }
  }
}

// ====== Validate chat_id format (digits with optional leading '-') ======
bool isValidChatId(const String &s) {
  if (s.length() == 0) return false;
  int start = 0;
  if (s.charAt(0) == '-') {
    if (s.length() == 1) return false;
    start = 1;
  }
  for (int i = start; i < s.length(); i++) {
    if (!isDigit(s.charAt(i))) return false;
  }
  return true;
}

// ====== Command Processor ======
void processCommand(String chat_id, String text) {
  if (text == "/status") {
    String msg = "📊 *Sensor Readings:*\n";
    msg += "Time: " + String(hourVal) + ":" + String(minuteVal) + ":" + String(secondVal) + "\n";
    msg += "Date: " + String(dayVal) + "/" + String(monthVal) + "/" + String(yearVal) + "\n";
    msg += "DHT11 -> Temp: " + String(tDHT, 1) + " °C, Humidity: " + String(h, 1) + " %\n";
    msg += "BMP180 -> Temp: " + String(tBHP, 1) + " °C, Pressure: " + String(pVal, 1) + " hPa\n";
    msg += "MQ135 -> Air Quality Value: " + String((int)mqVal) + "\n";
    msg += "HOME Power Source: ";
    if (currentPowerSource == SUPPLY)
      msg += "🔌 GRID (external)\n";
    else if (currentPowerSource == BATTERY)
      msg += "🔋 INVERTER\n";
    else
      msg += "❓ UNKNOWN\n";
      bot.sendChatAction(chat_id, "typing");
      bot.sendMessage(chat_id, msg, "Markdown");
  }
  else if (text == "/ledon") {
    digitalWrite(LED_PIN, HIGH);
    bot.sendChatAction(chat_id, "typing");
    bot.sendMessage(chat_id, "💡 LED *ON*", "Markdown");
  }
  else if (text == "/ledoff") {
    digitalWrite(LED_PIN, LOW);
    bot.sendChatAction(chat_id, "typing");
    bot.sendMessage(chat_id, "💡 LED *OFF*", "Markdown");
  }
  else if (text == "/fanon") {
    digitalWrite(FAN_PIN, HIGH);
    bot.sendChatAction(chat_id, "typing");
    bot.sendMessage(chat_id, "Fan *ON*", "Markdown");
  }
  else if (text == "/fanoff") {
    digitalWrite(FAN_PIN, LOW);
    bot.sendChatAction(chat_id, "typing");
    bot.sendMessage(chat_id, "Fan *OFF*", "Markdown");
  }
  else if (text == "/switchon") {
    digitalWrite(SWITCH_PIN, HIGH);
    bot.sendChatAction(chat_id, "typing");
    bot.sendMessage(chat_id, "Switch *ON*", "Markdown");
  }
  else if (text == "/switchoff") {
    digitalWrite(SWITCH_PIN, LOW);
    bot.sendChatAction(chat_id, "typing");
    bot.sendMessage(chat_id, "Switch *OFF*", "Markdown");
  }
  else if (text == "/whoami") {
    if (chat_id == ADMIN_CHAT_ID){
        bot.sendChatAction(chat_id, "typing");
      bot.sendMessage(chat_id, "👑 You are *Admin*", "Markdown");}
    else if (guestLoggedIn.count(chat_id) && guestLoggedIn[chat_id]){
         bot.sendChatAction(chat_id, "typing");
         bot.sendMessage(chat_id, "🧑‍🚀 You are *Guest*", "Markdown");
        }
    else {
        bot.sendChatAction(chat_id, "typing");
        bot.sendMessage(chat_id, "🕵️‍♂️ Not logged in", "Markdown");}
  }
  else if (text == "/help") {
     bot.sendChatAction(chat_id, "typing");
     bot.sendMessage(chat_id,
      "🤖 *Commands:*\n"
     "/status → Sensor data\n"
     "/ledon → Turn LED on\n"
     "/ledoff → Turn LED off\n"
     "/fanon → Turn FAN on\n"
     "/fanoff → Turn FAN off\n"
     "/switchon → Turn main switch on\n"
     "/switchoff → Turn main switch off\n"
     "/login <password> → Guest login\n"
     "/logout → Logout from guest mode\n"
     "/whoami → Check whether you are Admin/Guest/Unknown\n"
     "/help → Show all commands\n"
     "/logins → Show currently logged-in guests (Admin only)\n"
     "/listlogins → Same as /logins (Admin only)\n"
     "/remove <chat_id> → Remove/log out a guest (Admin only)\n"
     "/addguest <chat_id> <name> → Add and auto-login a guest (Admin only)\n"
     "/broadcast <message> → Broadcast message to all logged-in guests (Admin only)",
      "Markdown");
    }
  // ----- New admin-only command: list current logins -----
  else if (text == "/logins" || text == "/listlogins") {
    // Ensure only admin can call this
    if (chat_id != ADMIN_CHAT_ID) {
        bot.sendChatAction(chat_id, "typing");
      bot.sendMessage(chat_id, "🚫 Only the admin can use this command.", "Markdown");
      return;
    }

    String msg = "🔐 *Currently logged-in guests:*\n";
    int count = 0;
    for (auto &entry : guestLoggedIn) {
      if (entry.second) {
        String id = entry.first;
        String name = guestNames.count(id) ? guestNames[id] : ("User " + id);
        msg += "- " + name + " (" + id + ")\n";
        count++;
      }
    }
    if (count == 0) {
      msg += "_None_\n";
    }
    msg += "\n*Total:* " + String(count);
    bot.sendChatAction(chat_id, "typing");
    bot.sendMessage(chat_id, msg, "Markdown");
  }
  // ----- Admin-only: remove a guest by chat_id -----
  else if (text.startsWith("/remove")) {
    if (chat_id != ADMIN_CHAT_ID) {
        bot.sendChatAction(chat_id, "typing");
      bot.sendMessage(chat_id, "🚫 Only the admin can use this command.", "Markdown");
      return;
    }

    // Usage: /remove <chat_id>
    String param = "";
    if (text.length() > 7) {
      // could be "/remove " + id
      param = text.substring(7);
    }
    param.trim();

    if (param.length() == 0) {
        bot.sendChatAction(chat_id, "typing");
      bot.sendMessage(chat_id, "❗ Usage: /remove <chat_id>", "Markdown");
      return;
    }

    if (!isValidChatId(param)) {
        bot.sendChatAction(chat_id, "typing");
      bot.sendMessage(chat_id, "❗ Invalid chat_id format.", "Markdown");
      return;
    }

    String target = param;
    bool wasLoggedIn = guestLoggedIn.count(target) ? guestLoggedIn[target] : false;

    if (wasLoggedIn) {
      guestLoggedIn[target] = false;
      guestNames[target] = "";
      bot.sendChatAction(chat_id, "typing");
      bot.sendMessage(chat_id, "Guest removed successfully.", "Markdown");

      // Attempt to notify the guest
      bot.sendChatAction(target, "typing");
      bot.sendMessage(target, "🔒 You have been logged out by admin.", "Markdown");
    } else {
        bot.sendChatAction(chat_id, "typing");
      bot.sendMessage(chat_id, "Guest is not logged in.", "Markdown");
    }
  }
  // ----- Admin-only: add guest (id + name) -----
  else if (text.startsWith("/addguest")) {
    if (chat_id != ADMIN_CHAT_ID) {
        bot.sendChatAction(chat_id, "typing");
      bot.sendMessage(chat_id, "🚫 Only the admin can use this command.", "Markdown");
      return;
    }

    // Expected: /addguest <chat_id> <name...>
    String rest = "";
    if (text.length() > 9) {
      rest = text.substring(9);
    }
    rest.trim();

    if (rest.length() == 0) {
        bot.sendChatAction(chat_id, "typing");
      bot.sendMessage(chat_id, "❗ Usage: /addguest <chat_id> <name>", "Markdown");
      return;
    }

    int sp = rest.indexOf(' ');
    if (sp == -1) {
        bot.sendChatAction(chat_id, "typing");
        bot.sendMessage(chat_id, "❗ Usage: /addguest <chat_id> <name>", "Markdown");
        return;
    }

    String target = rest.substring(0, sp);
    String name = rest.substring(sp + 1);
    target.trim();
    name.trim();

    if (!isValidChatId(target)) {
      bot.sendChatAction(chat_id, "typing");
      bot.sendMessage(chat_id, "❗ Invalid chat_id format.", "Markdown");
      return;
    }

    if (name.length() == 0) {
        bot.sendChatAction(chat_id, "typing");
      bot.sendMessage(chat_id, "❗ Missing name. Usage: /addguest <chat_id> <name>", "Markdown");
      return;
    }

    guestLoggedIn[target] = true;
    guestNames[target] = name;
    bot.sendChatAction(chat_id, "typing");
    bot.sendMessage(chat_id, "Guest added and logged in successfully.", "Markdown");
    // Notify the guest
    bot.sendChatAction(target, "typing");
    bot.sendMessage(target, "👋 You were added as a guest by the admin.", "Markdown");
  }
  else if (text.startsWith("/broadcast ")) {

    if (chat_id != ADMIN_CHAT_ID) {
      bot.sendMessage(chat_id, "🚫 Only admin can broadcast.", "Markdown");
      return;
    }

    String msg = text.substring(11);
    msg.trim();

    if (msg.length() == 0) {
      bot.sendMessage(chat_id, "❗ Usage: /broadcast <message>", "Markdown");
      return;
    }

    int sent = 0;
    for (auto &entry : guestLoggedIn) {
      if (entry.second) {
        bot.sendMessage(entry.first, msg, "Markdown");
        sent++;
      }
    }

    bot.sendMessage(chat_id, "📣 Broadcast sent to " + String(sent) + " guests.", "Markdown");
  }
  // ===== Guests send message to Admin =====
else if (text.startsWith("/send ")) {

    // Only guests (NOT admin)
    if (chat_id == ADMIN_CHAT_ID) {
        bot.sendMessage(chat_id, "👑 Admin cannot use /send. This is for guests only.", "Markdown");
        return;
    }

    // Must be logged in
    if (!(guestLoggedIn.count(chat_id) && guestLoggedIn[chat_id])) {
        bot.sendMessage(chat_id, "🚫 You must login to send a message to admin.\nUse /login <password>", "Markdown");
        return;
    }

    bot.sendChatAction(chat_id, "typing");
    delay(300);

    // Extract message after /send 
    String guestMsg = text.substring(6);
    guestMsg.trim();

    if (guestMsg.length() == 0) {
        bot.sendMessage(chat_id, "❗ Usage: /send <your message>", "Markdown");
        return;
    }

    // Prepare message for admin
    String finalMsg = 
        "📨 *New message from Guest*\n\n"
        "👤 Name: " + (guestNames.count(chat_id) ? guestNames[chat_id] : "Unknown") + "\n"
        "🆔 Chat ID: " + chat_id + "\n\n"
        "💬 *Message:*\n" + guestMsg;

    // Send to admin
    bot.sendMessage(ADMIN_CHAT_ID, finalMsg, "Markdown");

    // Confirm to guest
    bot.sendMessage(chat_id, "📩 Your message has been sent to Admin!", "Markdown");
}
// ===== Admin reply to specific guest =====
else if (text.startsWith("/reply ")) {

    // Only admin allowed
    if (chat_id != ADMIN_CHAT_ID) {
        bot.sendMessage(chat_id, "🚫 Only Admin can use /reply.", "Markdown");
        return;
    }

    // Extract arguments: /reply <chat_id> <message>
    String rest = text.substring(7);
    rest.trim();

    int sp = rest.indexOf(' ');
    if (sp == -1) {
        bot.sendMessage(chat_id, "❗ Usage: /reply <chat_id> <message>", "Markdown");
        return;
    }

    String targetId = rest.substring(0, sp);
    String messageToGuest = rest.substring(sp + 1);

    targetId.trim();
    messageToGuest.trim();

    // Validate chat_id
    if (!isValidChatId(targetId)) {
        bot.sendMessage(chat_id, "❗ Invalid chat_id format.", "Markdown");
        return;
    }

    if (messageToGuest.length() == 0) {
        bot.sendMessage(chat_id, "❗ Message cannot be empty.\nUse: /reply <id> <message>", "Markdown");
        return;
    }

    bot.sendChatAction(chat_id, "typing");
    delay(250);

    // Send message to guest
    bot.sendMessage(targetId, "📩 *Admin Message:*\n" + messageToGuest, "Markdown");

    // Confirmation for admin
    String conf = 
        "✅ Message sent to guest:\n"
        "🆔 " + targetId + "\n\n"
        "💬 *Message:*\n" + messageToGuest;

    bot.sendMessage(chat_id, conf, "Markdown");
}
  else {
    bot.sendChatAction(chat_id, "typing");
    bot.sendMessage(chat_id, "❓ Unknown command! Send /help");
  }
}

// ====== Parse Sensor Data (safe, validated) ======
void parseSensorData(String data) {
  Serial.println("📩 Received Packet: " + data);

  // ==============================
  // 1️⃣ VALIDATION – COUNT ';'
  // ==============================
  int sepCount = 0;
  for (int i = 0; i < data.length(); i++) {
    if (data[i] == ';') sepCount++;
  }

  // Expecting EXACTLY 10 semicolons (11 values)
  if (sepCount < 10) {
    Serial.println("❌ ERROR: Corrupted/Incomplete packet. Ignored.");
    return;
  }

  // ==============================
  // 2️⃣ SPLIT PACKET SAFELY
  // ==============================
  String parts[20];
  int idx = 0;

  while (data.length() && idx < 20) {
    int p = data.indexOf(';');

    if (p == -1) {
      parts[idx++] = data;
      break;
    }

    parts[idx++] = data.substring(0, p);
    data = data.substring(p + 1);
  }

  if (idx < 11) {
    Serial.println("❌ ERROR: Not enough data fields!");
    return;
  }

  // ==============================
  // 3️⃣ ASSIGN VALUES CLEANLY
  // ==============================
  hourVal   = parts[0].toInt();
  minuteVal = parts[1].toInt();
  secondVal = parts[2].toInt();
  dayVal    = parts[3].toInt();
  monthVal  = parts[4].toInt();
  yearVal   = parts[5].toInt();
  tDHT      = parts[6].toFloat();
  h         = parts[7].toFloat();
  tBHP      = parts[8].toFloat();
  pVal      = parts[9].toFloat();
  mqVal     = parts[10].toFloat();

  // ==============================
  // 4️⃣ PRINT UPDATED VALUES
  // ==============================
  Serial.println("🕒 Time: " + String(hourVal) + ":" + String(minuteVal) + ":" + String(secondVal));
  Serial.println("📅 Date: " + String(dayVal) + "/" + String(monthVal) + "/" + String(yearVal));
  Serial.println("🌡️ DHT Temp: " + String(tDHT, 1) + " °C");
  Serial.println("💧 Humidity: " + String(h, 1) + " %");
  Serial.println("🌡️ BMP Temp: " + String(tBHP, 1) + " °C");
  Serial.println("🧭 Pressure: " + String(pVal, 1) + " hPa");
  Serial.println("🌫️ MQ135 Value: " + String(mqVal));

  // ==============================
  // 5️⃣ AIR QUALITY ALERT
  // ==============================
  if (mqVal >= 700) {
    if (!sentPoorAlert) {

      String msg =
        "⚠️ *Poor Air Quality Detected!*\n"
        "MQ135 Value: " + String((int)mqVal) + "\n"
        "Status: *Poor*";

      // Send alert to admin
      bot.sendMessage(ADMIN_CHAT_ID, msg, "Markdown");

      // Send alert to logged-in guests
      for (auto &u : guestLoggedIn) {
        if (u.second) {
          bot.sendMessage(u.first, msg, "Markdown");
        }
      }

      Serial.println("🚨 Poor air quality alert sent!");
      sentPoorAlert = true;
    }
  } 
  else {
    sentPoorAlert = false;  // reset for next alert
  }
}

// ====== Show 'typing...' action ======
void showTyping(String chat_id) {
  bot.sendChatAction(chat_id, "typing");
  delay(350);  // best balance for visible typing
}

// ====== Broadcast Message to Admin + All logged-in guests ======
void broadcastToLoggedIn(String msg) {
  bot.sendMessage(ADMIN_CHAT_ID, msg, "");
  for (auto& u : guestLoggedIn) {
    if (u.second) {
      bot.sendMessage(u.first, msg, "");
    }
  }
}
// Build sensor summary string and broadcast at 6:30
void sendMorningSensorBroadcast() {
  // Build sensor snapshot string (single-line friendly)
  String snapshot = "🌅 Good morning!\n\n";
  snapshot += "📊 Today's sensor reading is:\n";
  snapshot += "Time: " + String(hourVal) + ":" + String(minuteVal) + ":"+ "00" + "\n";
  snapshot += "Date: " + String(dayVal) + "/" + String(monthVal) + "/" + String(yearVal) + "\n";
  snapshot += "DHT11 -> Temp: " + String(tDHT, 1) + " °C, Humidity: " + String(h, 1) + " %\n";
  snapshot += "BMP180 -> Temp: " + String(tBHP, 1) + " °C, Pressure: " + String(pVal, 1) + " hPa\n";
  snapshot += "MQ135 -> Air Quality Value: " + String((int)mqVal) + "\n";
  snapshot += "HOME Power Source: ";
    if (currentPowerSource == SUPPLY)
      snapshot += "🔌 GRID (external)\n";
    else if (currentPowerSource == BATTERY)
      snapshot += "🔋 INVERTER\n";
    else
      snapshot += "❓ UNKNOWN\n";
      snapshot += "Have a good day!";

  // Use existing broadcast helper to send to admin + guests
  broadcastToLoggedIn(snapshot);
  Serial.println("✅ Morning broadcast sent.");
}

// Build sensor summary string and broadcast at 12:00 AM
void sendNightSensorBroadcast() {
  String snapshot = "GOOD NIGHT!\n\n";
  snapshot += "📊 Today's sensor reading is:\n";
  snapshot += "Time: " + String(hourVal) + ":" + String(minuteVal) + ":"+ "00" + "\n";
  snapshot += "Date: " + String(dayVal) + "/" + String(monthVal) + "/" + String(yearVal) + "\n";
  snapshot += "DHT11 -> Temp: " + String(tDHT, 1) + " °C, Humidity: " + String(h, 1) + " %\n";
  snapshot += "BMP180 -> Temp: " + String(tBHP, 1) + " °C, Pressure: " + String(pVal, 1) + " hPa\n";
  snapshot += "MQ135 -> Air Quality Value: " + String((int)mqVal) + "\n";
  snapshot += "HOME Power Source: ";
    if (currentPowerSource == SUPPLY)
      snapshot += "🔌 GRID (external)\n";
    else if (currentPowerSource == BATTERY)
      snapshot += "🔋 INVERTER\n";
    else
      snapshot += "❓ UNKNOWN\n";
      snapshot += "Have a good night!";

  // Use existing broadcast helper to send to admin + guests
  broadcastToLoggedIn(snapshot);
  Serial.println("✅ Night broadcast sent.");
}
