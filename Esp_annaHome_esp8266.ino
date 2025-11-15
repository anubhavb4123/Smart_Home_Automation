// ESP8266 Telegram Bot with guest login tracking (corrected)
// Make sure you have UniversalTelegramBot and SoftwareSerial installed

#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <SoftwareSerial.h>
#include <map>

// ====== WiFi & Telegram Config ======
const char* ssid = "BAJPAI_2.4Ghz";
const char* password = "44444422";

#define BOT_TOKEN "8001347460:AAEfxOg4j-1vmLdZ35qYabMqPJ52hSsK72E"
#define ADMIN_CHAT_ID "1839775992"
#define LOGIN_PASSWORD "4123"

// ====== Objects ======
WiFiClientSecure secured_client;
UniversalTelegramBot bot(BOT_TOKEN, secured_client);

// ====== Serial Communication ======
#define RX_PIN D6  // RX for ESP8266 (connect to TX of sensor board)
#define TX_PIN D5  // TX for ESP8266 (connect to RX of sensor board)

SoftwareSerial Serial2(RX_PIN, TX_PIN);

// ====== LED Indicator ======
#define LED_PIN 2
#define FAN_PIN 15
#define SWITCH_PIN 4 
#define POWER_SOURCE_PIN D3  // choose a free pin connected to your detection circuit

enum PowerSource { UNKNOWN, BATTERY, SUPPLY };
PowerSource currentPowerSource = UNKNOWN;
PowerSource lastPowerSource = UNKNOWN;

unsigned long lastChangeTime = 0;   // when a potential change was first detected
const unsigned long debounceDelay = 2000; // 2 seconds stability check

// ====== Sensor Data (globals used across functions) ======
String serialData = "";
int hourVal = 0, minuteVal = 0, secondVal = 0, dayVal = 0, monthVal = 0, yearVal = 0;
float tDHT = 0.0, h = 0.0, tBHP = 0.0, p = 0.0, mqVal = 0.0;

// ====== Alerts / Flags from sensor =========
int LastHour = 0;
int lowBatteryWarning = 0;
int lowBatteryPercentage = 0;
bool sentPoorAlert = false;

// ====== Timers ======
unsigned long lastBotCheck = 0;
unsigned long lastWiFiCheck = 0;
const unsigned long botInterval = 800;      // check bot every ~0.8s
const unsigned long wifiCheckInterval = 10000; // check wifi every 10s

// Track logged-in guests and their friendly names
std::map<String, bool> guestLoggedIn;
std::map<String, String> guestNames;

// ====== Setup ======
void setup() {
  Serial.begin(115200);
  Serial2.begin(9600); // from sensor board
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH); // LED OFF (active LOW)
  pinMode(LED_PIN, OUTPUT);
  pinMode(FAN_PIN, OUTPUT);
  pinMode(SWITCH_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);
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

  // Read initial power source
  currentPowerSource = digitalRead(POWER_SOURCE_PIN) ? SUPPLY : BATTERY;
  lastPowerSource = currentPowerSource;

  // Log initial state
  if (currentPowerSource == SUPPLY)
    Serial.println("🔌 Power source: SUPPLY (external)");
  else
    Serial.println("🔋 Power source: BATTERY");
}

// ====== Helpers ======
// Attempt to get a friendly sender name from the incoming message.
// Uses from_name if present, otherwise returns "User <chatid>"
String getSenderName(int msgIndex, const String &fallbackChatId) {
  String name = "";

  if (bot.messages[msgIndex].from_name.length() > 0) {
    name = bot.messages[msgIndex].from_name;
  } else {
    name = "User " + fallbackChatId;
  }
  return name;
}

// ====== Main Loop ======
void loop() {
  // --- Read sensor data from Serial2 ---
  while (Serial2.available()) {
    char c = Serial2.read();
    // accumulate until newline
    if (c == '\n') {
      serialData.trim();
      if (serialData.length() > 0) {
        parseSensorData(serialData);
      }
      serialData = "";
    } else if (c != '\r') {
      serialData += c;
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
  Serial.println("🕒 Time: " + String(hourVal) + ":" + String(minuteVal) + ":" + String(secondVal));
  Serial.println("📅 Date: " + String(dayVal) + "/" + String(monthVal) + "/" + String(yearVal));
  Serial.println("🌡️ DHT Temp: " + String(tDHT, 1) + " °C");
  Serial.println("💧 Humidity: " + String(h, 1) + " %");
  Serial.println("🌡️ BMP Temp: " + String(tBHP, 1) + " °C");
  Serial.println("🧭 Pressure: " + String(p, 1) + " hPa");
  Serial.println("🌫️ MQ135 Value: " + String((int)mqVal));
  }

  // --- Notifications from sensor flags ---
  if (lowBatteryWarning == 1) {
    broadcastToLoggedIn("⚠️ Low Battery! " + String(lowBatteryPercentage) + "%");
    lowBatteryWarning = 0;
  }

  if (LastHour != 0) {
    broadcastToLoggedIn("⏰ Hour " + String(LastHour) + " started!");
    LastHour = 0;
  }

  // --- Power Source Change Detection ---
    PowerSource newPower = digitalRead(POWER_SOURCE_PIN) ? SUPPLY : BATTERY;

  // Check if reading is different from current
  if (newPower != currentPowerSource) {
    // Possible change detected — start timing
    if (lastChangeTime == 0) {
      lastChangeTime = millis();
    }

    // If stable for longer than debounce delay
    if (millis() - lastChangeTime > debounceDelay) {
      lastPowerSource = currentPowerSource;
      currentPowerSource = newPower;
      lastChangeTime = 0; // reset timer

      // Prepare message
      String msg;
      if (currentPowerSource == SUPPLY)
        msg = "🔌 Power source switched: Now running on SUPPLY (external power)!";
      else
        msg = "🔋 Power source switched: Now running on BATTERY!";

      Serial.println(msg);          // Log to Serial
      bot.sendMessage(ADMIN_CHAT_ID, msg, "");  // Send Telegram alert
    }
  } 
  else {
    // If reading matches current state, reset timer (no change)
    lastChangeTime = 0;
  }

}

// ====== Message Handling ======
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
      if (!isAdmin){
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

// ====== Command Processor ======
void processCommand(String chat_id, String text) {
  if (text == "/status") {
    String msg = "📊 *Sensor Readings:*\n";
    msg += "Time: " + String(hourVal) + ":" + String(minuteVal) + ":" + String(secondVal) + "\n";
    msg += "Date: " + String(dayVal) + "/" + String(monthVal) + "/" + String(yearVal) + "\n";
    msg += "DHT11 -> Temp: " + String(tDHT, 1) + " °C, Humidity: " + String(h, 1) + " %\n";
    msg += "BMP180 -> Temp: " + String(tBHP, 1) + " °C, Pressure: " + String(p, 1) + " hPa\n";
    msg += "MQ135 -> Air Quality Value: " + String(mqVal) + "\n";
    bot.sendMessage(chat_id, msg, "Markdown");
  }
  else if (text == "/ledon") {
    digitalWrite(LED_PIN, LOW);
    bot.sendMessage(chat_id, "💡 LED *ON*", "Markdown");
  }
  else if (text == "/ledoff") {
    digitalWrite(LED_PIN, HIGH);
    bot.sendMessage(chat_id, "💡 LED *OFF*", "Markdown");
  }
  else if (text =="/fanon"){
    digitalWrite(FAN_PIN,HIGH);
    bot.sendMessage(chat_id, " Fan *ON*", "Markdown");
  }
  else if (text =="/fanoff"){
    digitalWrite(FAN_PIN,LOW);
    bot.sendMessage(chat_id, "Fan *OFF*", "Markdown");
  }
  else if (text =="/switchon"){
    digitalWrite(SWITCH_PIN, HIGH);
    bot.sendMessage(chat_id, " Switch *ON*", "Markdown");
  }
  else if (text =="/switchoff"){
    digitalWrite(SWITCH_PIN,LOW);
    bot.sendMessage(chat_id, "switch *OFF*", "Markdown");
  }
  else if (text == "/whoami") {
    if (chat_id == ADMIN_CHAT_ID)
      bot.sendMessage(chat_id, "👑 You are *Admin*", "Markdown");
    else if (guestLoggedIn.count(chat_id) && guestLoggedIn[chat_id])
      bot.sendMessage(chat_id, "🧑‍🚀 You are *Guest*", "Markdown");
    else
      bot.sendMessage(chat_id, "🕵️‍♂️ Not logged in", "Markdown");
  }
  else if (text == "/help") {
    bot.sendMessage(chat_id,
      "🤖 *Commands:*\n"
      "/status → Sensor data\n"
      "/ledon → Turn LED on\n"
      "/ledoff → Turn LED off\n"
      "/fanon → Turn FAN on\n"
      "/fanoff → Turn FAN off\n"
      "/switchon → Turn SWITCH on\n"
      "/switchoff → Turn SWITCH off\n"
      "/login pwd → Guest login\n"
      "/logout → Logout\n"
      "/whoami → Check role\n"
      "/logins → Show currently logged-in guests",
      "Markdown");
  }
  // ----- New admin-only command: list current logins -----
  else if (text == "/logins" || text == "/listlogins") {
    // Ensure only admin can call this
    if (chat_id != ADMIN_CHAT_ID) {
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
    bot.sendMessage(chat_id, msg, "Markdown");
  }
  else {
    bot.sendMessage(chat_id, "❓ Unknown command! Send /help");
  }
}

// ====== Parse Sensor Data ======
void parseSensorData(String data) {
  Serial.println("📩 Received: " + data);

  // Split data by ';'
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

  // Assign values safely
  if (idx >= 11) {
    hourVal   = parts[0].toInt();
    minuteVal = parts[1].toInt();
    secondVal = parts[2].toInt();
    dayVal    = parts[3].toInt();
    monthVal  = parts[4].toInt();
    yearVal   = parts[5].toInt();

    tDHT  = parts[6].toFloat();
    h     = parts[7].toFloat();
    tBHP  = parts[8].toFloat();
    p     = parts[9].toFloat();

    mqVal = parts[10].toInt();

    // Optional flags (Arduino may or may not send them)
    lowBatteryWarning    = (idx > 11) ? parts[11].toInt() : 0;
    lowBatteryPercentage = (idx > 12) ? parts[12].toInt() : 0;
    LastHour             = (idx > 13) ? parts[13].toInt() : 0;
  }

  // Print parsed data
  Serial.println("🕒 Time: " + String(hourVal) + ":" + String(minuteVal) + ":" + String(secondVal));
  Serial.println("📅 Date: " + String(dayVal) + "/" + String(monthVal) + "/" + String(yearVal));
  Serial.println("🌡️ DHT Temp: " + String(tDHT, 1) + " °C");
  Serial.println("💧 Humidity: " + String(h, 1) + " %");
  Serial.println("🌡️ BMP Temp: " + String(tBHP, 1) + " °C");
  Serial.println("🧭 Pressure: " + String(p, 1) + " hPa");
  Serial.println("🌫️ MQ135 Value: " + String((int)mqVal));

  if (lowBatteryWarning)
    Serial.println("⚠️ Low Battery: " + String(lowBatteryPercentage) + "%");

  if (LastHour)
    Serial.println("⏰ Hour Change: " + String(LastHour));
  // ===== Air Quality Check =====
  if (mqVal >= 700) {

    // Send alert only once
    if (!sentPoorAlert) {
     String msg = "⚠️ *Poor Air Quality Detected!*\n"
                 "MQ135 Value: " + String((int)mqVal) + "\n"
                 "Status: *Poor*";

      // Send to admin
     bot.sendMessage(ADMIN_CHAT_ID, msg, "Markdown");

      // Send to all logged-in guests
      for (auto &u : guestLoggedIn) {
        if (u.second) {
        bot.sendMessage(u.first, msg, "Markdown");
     }
    }

      Serial.println("🚨 Poor air quality alert sent!");
      sentPoorAlert = true;  // stop repeated alerts
    }
  }
  else {
    // Reset alert so we can send again when it becomes poor next time
    sentPoorAlert = false;
  }
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
