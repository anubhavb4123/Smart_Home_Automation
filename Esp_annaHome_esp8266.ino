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

#define BOT_TOKEN "800134746"
#define ADMIN_CHAT_ID "1592"
#define LOGIN_PASSWORD "4123"

// ====== Objects ======
WiFiClientSecure secured_client;
UniversalTelegramBot bot(BOT_TOKEN, secured_client);

// ====== Serial Communication ======
#define RX_PIN D6  // RX for ESP8266 (connect to TX of sensor board)
#define TX_PIN D5  // TX for ESP8266 (connect to RX of sensor board)
SoftwareSerial Serial2(RX_PIN, TX_PIN);

// ====== LED Indicator ======
#define LED_BUILTIN  // NodeMCU LED (Active LOW)
#define LED_PIN 15
#define FAN_PIN 16
#define SWITCH_PIN 17 

// ====== Sensor Data (globals used across functions) ======
String serialData = "";
int hourVal = 0, minuteVal = 0, secondVal = 0, dayVal = 0, monthVal = 0, yearVal = 0;
float tDHT = 0.0, h = 0.0, tBHP = 0.0, p = 0.0, mqVal = 0.0;

// ====== Alerts / Flags from sensor =========
int LastHour = 0;
int lowBatteryWarning = 0;
int lowBatteryPercentage = 0;

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
    bot.sendMessage(chat_id, " Switch *ON*", "Markdown")
  }
  else if (text =="/switchoff"){
    digitalWrite(SWITCH_PIN,LOW);
    bot.sendMessage(chat_id, "switch *OFF*", "Markdown")
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

  // Temporary parsed variables
  int phour=0, pminute=0, psecond=0, pday=0, pmonth=0, pyear=0;
  float ptDHT=0, phum=0, ptBMP=0, ppress=0;
  int pmq = 0;
  int plowBatteryFlag = 0;
  int plowBatteryPct = 0;
  int pLastHour = 0;

  // Example expected incoming format (semicolon separated):
  // hour;minute;second;day;month;year;dhtTemp;humidity;bmpTemp;pressure;mq;lowBatteryFlag;lowBatteryPct;lastHour
  // But your original sscanf had 11 fields. Adjust as per what your sensor sends.
  // We'll try to parse up to 11-14 values safely using sscanf.
  // This sscanf expects at least 11 fields. If you send extra fields, they'll be parsed too if format matches.
  int parsed = sscanf(data.c_str(),
                      "%d;%d;%d;%d;%d;%d;%f;%f;%f;%f;%d;%d;%d;%d",
                      &phour, &pminute, &psecond, &pday, &pmonth, &pyear,
                      &ptDHT, &phum, &ptBMP, &ppress, &pmq,
                      &plowBatteryFlag, &plowBatteryPct, &pLastHour);

  // If parse failed (parsed < 11) try a fallback with only first 11 fields
  if (parsed < 11) {
    parsed = sscanf(data.c_str(),
                    "%d;%d;%d;%d;%d;%d;%f;%f;%f;%f;%d",
                    &phour, &pminute, &psecond, &pday, &pmonth, &pyear,
                    &ptDHT, &phum, &ptBMP, &ppress, &pmq);
    // reset optional flags if not provided
    plowBatteryFlag = 0;
    plowBatteryPct = 0;
    pLastHour = 0;
  }

  // Assign parsed values into globals
  hourVal = phour;
  minuteVal = pminute;
  secondVal = psecond;
  dayVal = pday;
  monthVal = pmonth;
  yearVal = pyear;

  tDHT = ptDHT;
  h = phum;
  tBHP = ptBMP;
  p = ppress;
  mqVal = pmq;

  // Optional flags
  lowBatteryWarning = plowBatteryFlag;
  lowBatteryPercentage = plowBatteryPct;
  LastHour = pLastHour;

  // Print parsed data for confirmation
  Serial.println("🕒 Time: " + String(hourVal) + ":" + String(minuteVal) + ":" + String(secondVal));
  Serial.println("📅 Date: " + String(dayVal) + "/" + String(monthVal) + "/" + String(yearVal));
  Serial.println("🌡️ DHT Temp: " + String(tDHT, 1) + " °C");
  Serial.println("💧 Humidity: " + String(h, 1) + " %");
  Serial.println("🌡️ BMP Temp: " + String(tBHP, 1) + " °C");
  Serial.println("🧭 Pressure: " + String(p, 1) + " hPa");
  Serial.println("🌫️ MQ135 Value: " + String((int)mqVal));
  if (lowBatteryWarning) Serial.println("⚠️ Low battery flag from sensor: " + String(lowBatteryPercentage) + "%");
  if (LastHour) Serial.println("⏰ Sensor sent LastHour: " + String(LastHour));
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
