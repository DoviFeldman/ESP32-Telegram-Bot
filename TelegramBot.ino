/*
  ============================================================
  ESP32 Telegram Message Scheduler Bot
  ============================================================
  HOW IT WORKS:
  1. Send messages to your bot anytime — it stores up to 50
  2. Every day at 3 set times, it sends you 3 stored messages
  3. Messages are sent in order (oldest first)
  
  SETUP STEPS:
  1. Install "Universal Telegram Bot" library (Arduino Library Manager)
  2. Install "ArduinoJson" library (Arduino Library Manager)
  3. Fill in your WiFi and Telegram details below
  4. Flash to ESP32
  ============================================================
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>

// ============================================================
//  *** EDIT THESE VALUES ***
// ============================================================

// Your WiFi credentials
const char* WIFI_SSID     = "YourWiFiName";
const char* WIFI_PASSWORD = "YourWiFiPassword";

// Get your bot token from @BotFather on Telegram
const char* BOT_TOKEN = "1234567890:ABCDefghIJKlmnoPQRsTUVwxyz";

// Get your chat ID from @userinfobot on Telegram
const String CHAT_ID = "123456789";

// Times to send messages each day (24-hour format)
const int SEND_HOUR_1   = 8;    // 8:00 AM
const int SEND_MINUTE_1 = 0;

const int SEND_HOUR_2   = 13;   // 1:00 PM
const int SEND_MINUTE_2 = 0;

const int SEND_HOUR_3   = 20;   // 8:00 PM
const int SEND_MINUTE_3 = 0;

// How often to check for new Telegram messages (milliseconds)
const int CHECK_INTERVAL_MS = 3000;   // 3 seconds

// Maximum number of messages to store
const int MAX_MESSAGES = 50;

// ============================================================
//  END OF SETTINGS — no need to edit below this line
// ============================================================

WiFiClientSecure client;
UniversalTelegramBot bot(BOT_TOKEN, client);

// Message storage
String messageQueue[MAX_MESSAGES];
int messageCount   = 0;
int nextSendIndex  = 0;   // which message to send next

// Time tracking
unsigned long lastBotCheck = 0;

// Track which send slots have fired today
bool slot1Sent = false;
bool slot2Sent = false;
bool slot3Sent = false;
int  lastDay   = -1;      // used to reset slots at midnight

// Simple NTP time (no extra library needed)
const char* NTP_SERVER    = "pool.ntp.org";
const long  GMT_OFFSET_S  = 0;       // Change to your UTC offset in seconds
                                      // e.g. UTC+1 = 3600, UTC-5 = -18000
const int   DST_OFFSET_S  = 0;       // Daylight saving offset (3600 or 0)

// ============================================================
void setup() {
  Serial.begin(115200);
  Serial.println("\n=== ESP32 Telegram Scheduler ===");

  // Connect to WiFi
  Serial.print("Connecting to WiFi");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected! IP: " + WiFi.localIP().toString());

  // Sync time via NTP
  configTime(GMT_OFFSET_S, DST_OFFSET_S, NTP_SERVER);
  Serial.print("Syncing time");
  struct tm t;
  while (!getLocalTime(&t)) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nTime synced!");

  // Allow insecure TLS (needed for Telegram on ESP32)
  client.setInsecure();

  Serial.println("Bot ready. Send me messages and I'll schedule them!");
  Serial.println("------------------------------------------------");
}

// ============================================================
void loop() {
  // 1. Check for incoming Telegram messages
  if (millis() - lastBotCheck > CHECK_INTERVAL_MS) {
    lastBotCheck = millis();
    checkForNewMessages();
  }

  // 2. Check if it's time to send a scheduled message
  checkSendTimes();

  delay(100);
}

// ============================================================
// Receive messages from Telegram and store them
// ============================================================
void checkForNewMessages() {
  int numMessages = bot.getUpdates(bot.last_message_received + 1);

  for (int i = 0; i < numMessages; i++) {
    String text   = bot.messages[i].text;
    String fromId = bot.messages[i].chat_id;

    Serial.println("Received: " + text);

    // Only accept messages from your own chat ID
    if (fromId != CHAT_ID) {
      bot.sendMessage(fromId, "Sorry, I only accept messages from my owner.", "");
      continue;
    }

    // Handle /status command
    if (text == "/status") {
      sendStatus();
      continue;
    }

    // Handle /clear command — wipes all stored messages
    if (text == "/clear") {
      messageCount  = 0;
      nextSendIndex = 0;
      bot.sendMessage(CHAT_ID, "All messages cleared!", "");
      continue;
    }

    // Handle /list command — shows stored messages
    if (text == "/list") {
      sendList();
      continue;
    }

    // Store the message
    if (messageCount < MAX_MESSAGES) {
      messageQueue[messageCount] = text;
      messageCount++;
      String reply = "✅ Stored! (" + String(messageCount) + "/" + String(MAX_MESSAGES) + " messages queued)";
      bot.sendMessage(CHAT_ID, reply, "");
      Serial.println("Stored message #" + String(messageCount));
    } else {
      bot.sendMessage(CHAT_ID, "❌ Queue full! (" + String(MAX_MESSAGES) + " max). Use /clear to reset.", "");
    }
  }
}

// ============================================================
// Check if current time matches any send slot
// ============================================================
void checkSendTimes() {
  struct tm t;
  if (!getLocalTime(&t)) return;

  int currentHour   = t.tm_hour;
  int currentMinute = t.tm_min;
  int currentDay    = t.tm_yday;   // day of year (resets slots daily)

  // Reset send-slot flags at midnight (new day)
  if (currentDay != lastDay) {
    slot1Sent = false;
    slot2Sent = false;
    slot3Sent = false;
    lastDay   = currentDay;
    Serial.println("New day — send slots reset.");
  }

  // Check slot 1
  if (!slot1Sent && currentHour == SEND_HOUR_1 && currentMinute == SEND_MINUTE_1) {
    slot1Sent = true;
    sendNextMessage("Morning");
  }

  // Check slot 2
  if (!slot2Sent && currentHour == SEND_HOUR_2 && currentMinute == SEND_MINUTE_2) {
    slot2Sent = true;
    sendNextMessage("Afternoon");
  }

  // Check slot 3
  if (!slot3Sent && currentHour == SEND_HOUR_3 && currentMinute == SEND_MINUTE_3) {
    slot3Sent = true;
    sendNextMessage("Evening");
  }
}

// ============================================================
// Send the next queued message
// ============================================================
void sendNextMessage(String slotName) {
  if (nextSendIndex >= messageCount) {
    Serial.println(slotName + " slot fired but no messages left.");
    bot.sendMessage(CHAT_ID, "📭 No more messages in queue for " + slotName + " slot!", "");
    return;
  }

  String msg = messageQueue[nextSendIndex];
  bot.sendMessage(CHAT_ID, "🕐 " + slotName + " message:\n\n" + msg, "");
  Serial.println("Sent " + slotName + " message #" + String(nextSendIndex + 1) + ": " + msg);
  nextSendIndex++;
}

// ============================================================
// /status command — show queue info and current time
// ============================================================
void sendStatus() {
  struct tm t;
  getLocalTime(&t);

  char timeStr[32];
  strftime(timeStr, sizeof(timeStr), "%H:%M:%S  %d/%m/%Y", &t);

  int remaining = messageCount - nextSendIndex;

  String status = "📊 *Bot Status*\n";
  status += "─────────────────\n";
  status += "🕐 Current time: " + String(timeStr) + "\n";
  status += "📬 Total stored: " + String(messageCount) + "\n";
  status += "✅ Already sent: " + String(nextSendIndex) + "\n";
  status += "⏳ Remaining:    " + String(remaining) + "\n\n";
  status += "📅 *Daily send times:*\n";
  status += "  Slot 1: " + pad(SEND_HOUR_1) + ":" + pad(SEND_MINUTE_1) + (slot1Sent ? " ✅" : " ⏳") + "\n";
  status += "  Slot 2: " + pad(SEND_HOUR_2) + ":" + pad(SEND_MINUTE_2) + (slot2Sent ? " ✅" : " ⏳") + "\n";
  status += "  Slot 3: " + pad(SEND_HOUR_3) + ":" + pad(SEND_MINUTE_3) + (slot3Sent ? " ✅" : " ⏳") + "\n\n";
  status += "Commands: /status /list /clear";

  bot.sendMessage(CHAT_ID, status, "Markdown");
}

// ============================================================
// /list command — show all stored messages
// ============================================================
void sendList() {
  if (messageCount == 0) {
    bot.sendMessage(CHAT_ID, "📭 No messages stored yet.", "");
    return;
  }

  String list = "📋 *Stored Messages*\n─────────────────\n";
  for (int i = 0; i < messageCount; i++) {
    String prefix = (i < nextSendIndex) ? "✅ " : "⏳ ";
    list += prefix + String(i + 1) + ". " + messageQueue[i] + "\n";
    // Send in chunks to avoid Telegram message length limit
    if ((i + 1) % 10 == 0) {
      bot.sendMessage(CHAT_ID, list, "Markdown");
      list = "";
    }
  }
  if (list.length() > 0) {
    bot.sendMessage(CHAT_ID, list, "Markdown");
  }
}

// ============================================================
// Helper: zero-pad single digit numbers (e.g. 8 -> "08")
// ============================================================
String pad(int n) {
  return (n < 10) ? "0" + String(n) : String(n);
}
