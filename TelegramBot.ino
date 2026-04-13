
/*
  ============================================================
  ESP32 Telegram Message Scheduler Bot  — V2
  ============================================================
  HOW IT WORKS:
  - Send any text to the bot and it gets queued (up to 50 msgs)
  - Every other week, at a configurable time, it sends 3 queued
    messages back to you
  - All settings (time, day, week interval, WiFi) can be changed
    directly from the Telegram chat
  - Queue and settings survive power-offs (stored in NVS flash)

  COMMANDS:
    /status              show time, queue, and next send info
    /list                show all queued messages
    /clear               delete all queued messages
    /settime HH MM       e.g. /settime 14 00  sets send time to 2:00 PM ET
    /setday D            1=Sun  2=Mon  3=Tue  4=Wed  5=Thu  6=Fri  7=Sat
    /setweeks N          send every N weeks  (default 2)
    /addwifi SSID PASS   save a WiFi network (PASS optional for open networks)
    /listwifi            list saved networks
    /deletewifi SSID     remove one saved network
    /clearwifi           remove ALL saved networks (stays on current WiFi)
    /help                show all commands

  SETUP:
  1. Install "Universal Telegram Bot" library (Arduino Library Manager)
  2. Install "ArduinoJson" library v6 (Arduino Library Manager)
  3. Fill in BOT_TOKEN and CHAT_ID below
  4. Optionally set DEFAULT_SSID / DEFAULT_PASSWORD as a fallback WiFi
  5. Flash to ESP32
  ============================================================
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>
#include <Preferences.h>   // built-in ESP32 NVS (non-volatile storage)
#include <time.h>

// ============================================================
//  *** EDIT THESE ***
// ============================================================


// Your WiFi credentials
const char* WIFI_SSID     = "YourWiFiName";
const char* WIFI_PASSWORD = "YourWiFiPassword";

// Get your bot token from @BotFather on Telegram
const char* BOT_TOKEN = "1234567890:ABCDefghIJKlmnoPQRsTUVwxyz";

// Get your chat ID from @userinfobot on Telegram
const String CHAT_ID = "123456789";


// ============================================================
//  END OF SETTINGS
// ============================================================

// Eastern Time (handles EST/EDT automatically)
const char* TZ_RULE    = "EST5EDT,M3.2.0,M11.1.0";
const char* NTP_SERVER = "pool.ntp.org";

// Schedule defaults (all stored in NVS, changeable via chat)
int schedHour        = 14;  // 2:00 PM
int schedMinute      = 0;
int schedWeekday     = 2;   // 1=Sun … 7=Sat  (2 = Monday)
int schedEveryNWeeks = 2;

// Message queue
const int MAX_MESSAGES = 50;
String    messageQueue[MAX_MESSAGES];
int       messageCount  = 0;
int       nextSendIndex = 0;

// Send-slot tracking
bool slotSentThisWeek = false;
int  lastCheckedDay   = -1;
int  referenceWeek    = -1;   // ISO week number of last send

// Misc
WiFiClientSecure     secureClient;
UniversalTelegramBot bot(BOT_TOKEN, secureClient);
Preferences          prefs;
unsigned long        lastBotCheck = 0;
const int            CHECK_MS     = 3000;   // poll Telegram every 3 s

// ============================================================
//  SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  Serial.println("\n=== ESP32 Telegram Scheduler V2 ===");

  prefs.begin("botdata", false);
  loadPrefs();
  connectWiFi();

  // Sync NTP time with automatic EST/EDT handling
  configTzTime(TZ_RULE, NTP_SERVER);
  Serial.print("Syncing time");
  struct tm t;
  while (!getLocalTime(&t)) { delay(500); Serial.print("."); }
  Serial.printf("\nTime: %02d:%02d ET\n", t.tm_hour, t.tm_min);

  secureClient.setInsecure();   // required for Telegram HTTPS on ESP32
  Serial.println("Bot ready.");
}

// ============================================================
//  LOOP
// ============================================================
void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi lost, reconnecting...");
    connectWiFi();
  }

  if (millis() - lastBotCheck > CHECK_MS) {
    lastBotCheck = millis();
    checkForNewMessages();
  }

  checkSendTime();
  delay(100);
}

// ============================================================
//  WiFi — try all saved networks, then the hardcoded default
// ============================================================
void connectWiFi() {
  int netCount = prefs.getInt("wifiCount", 0);

  // Collect networks to try
  String ssids[11], passes[11];
  int total = 0;
  for (int i = 0; i < netCount && total < 10; i++) {
    ssids[total]  = prefs.getString(("ssid" + String(i)).c_str(), "");
    passes[total] = prefs.getString(("pass" + String(i)).c_str(), "");
    if (ssids[total].length() > 0) total++;
  }
  ssids[total]  = DEFAULT_SSID;
  passes[total] = DEFAULT_PASSWORD;
  total++;

  for (int a = 0; a < total; a++) {
    Serial.printf("Trying: %s\n", ssids[a].c_str());
    WiFi.begin(ssids[a].c_str(), passes[a].c_str());
    for (int t = 0; t < 20 && WiFi.status() != WL_CONNECTED; t++) {
      delay(500); Serial.print(".");
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("\nConnected to %s  IP: %s\n",
                    ssids[a].c_str(), WiFi.localIP().toString().c_str());
      return;
    }
    Serial.println("\nFailed.");
    WiFi.disconnect();
    delay(300);
  }
  Serial.println("Could not connect to any WiFi.");
}

// ============================================================
//  NVS — load and save all persistent data
// ============================================================
void loadPrefs() {
  schedHour        = prefs.getInt("sHour",    14);
  schedMinute      = prefs.getInt("sMin",      0);
  schedWeekday     = prefs.getInt("sWday",     2);
  schedEveryNWeeks = prefs.getInt("sNWeeks",   2);
  messageCount     = prefs.getInt("mqCount",   0);
  nextSendIndex    = prefs.getInt("mqNext",    0);
  referenceWeek    = prefs.getInt("refWeek",  -1);

  for (int i = 0; i < messageCount && i < MAX_MESSAGES; i++) {
    messageQueue[i] = prefs.getString(("mq" + String(i)).c_str(), "");
  }
  Serial.printf("Loaded %d queued messages.\n", messageCount);
}

void savePrefs() {
  prefs.putInt("sHour",   schedHour);
  prefs.putInt("sMin",    schedMinute);
  prefs.putInt("sWday",   schedWeekday);
  prefs.putInt("sNWeeks", schedEveryNWeeks);
  prefs.putInt("mqCount", messageCount);
  prefs.putInt("mqNext",  nextSendIndex);
  prefs.putInt("refWeek", referenceWeek);
  for (int i = 0; i < messageCount && i < MAX_MESSAGES; i++) {
    prefs.putString(("mq" + String(i)).c_str(), messageQueue[i]);
  }
}

// ============================================================
//  TELEGRAM — receive and dispatch
// ============================================================
void checkForNewMessages() {
  int n = bot.getUpdates(bot.last_message_received + 1);
  for (int i = 0; i < n; i++) {
    String text   = bot.messages[i].text;
    String fromId = bot.messages[i].chat_id;
    Serial.println("Recv: " + text);
    if (fromId != CHAT_ID) {
      bot.sendMessage(fromId, "Unauthorized.", "");
      continue;
    }
    handleCommand(text);
  }
}

void handleCommand(String text) {
  text.trim();

  // --- simple commands ---
  if (text == "/status")   { sendStatus();  return; }
  if (text == "/list")     { sendList();    return; }
  if (text == "/listwifi") { cmdListWifi(); return; }

  if (text == "/clear") {
    messageCount = nextSendIndex = 0;
    savePrefs();
    bot.sendMessage(CHAT_ID, "Queue cleared.", "");
    return;
  }
  if (text == "/clearwifi") { cmdClearWifi(); return; }

  if (text == "/help" || text == "/start") {
    bot.sendMessage(CHAT_ID,
      "Commands:\n"
      "/status\n"
      "/list\n"
      "/clear\n"
      "/settime HH MM  (24h, ET)\n"
      "/setday D  (1=Sun 2=Mon 3=Tue 4=Wed 5=Thu 6=Fri 7=Sat)\n"
      "/setweeks N\n"
      "/addwifi SSID PASSWORD\n"
      "/listwifi\n"
      "/deletewifi SSID\n"
      "/clearwifi\n\n"
      "Any other text is queued as a message.", "");
    return;
  }

  // --- parameterised commands ---

  // /settime HH MM
  if (text.startsWith("/settime ")) {
    String a = text.substring(9); a.trim();
    int sp = a.indexOf(' ');
    if (sp < 0) { bot.sendMessage(CHAT_ID, "Usage: /settime HH MM", ""); return; }
    int h = a.substring(0, sp).toInt();
    int m = a.substring(sp + 1).toInt();
    if (h < 0 || h > 23 || m < 0 || m > 59) {
      bot.sendMessage(CHAT_ID, "Invalid. Hours 0-23, minutes 0-59.", ""); return;
    }
    schedHour = h; schedMinute = m; savePrefs();
    bot.sendMessage(CHAT_ID, "Send time set to " + formatAMPM(h, m) + " ET.", "");
    return;
  }

  // /setday D
  if (text.startsWith("/setday ")) {
    int d = text.substring(8).toInt();
    if (d < 1 || d > 7) {
      bot.sendMessage(CHAT_ID, "Usage: /setday D  (1=Sun 2=Mon ... 7=Sat)", ""); return;
    }
    schedWeekday = d; savePrefs();
    String names[] = {"", "Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"};
    bot.sendMessage(CHAT_ID, "Send day set to " + names[d] + ".", "");
    return;
  }

  // /setweeks N
  if (text.startsWith("/setweeks ")) {
    int n = text.substring(10).toInt();
    if (n < 1 || n > 52) {
      bot.sendMessage(CHAT_ID, "Usage: /setweeks N  (1-52)", ""); return;
    }
    schedEveryNWeeks = n; savePrefs();
    bot.sendMessage(CHAT_ID, "Interval set to every " + String(n) + " week(s).", "");
    return;
  }

  // /addwifi SSID [PASSWORD]
  if (text.startsWith("/addwifi ")) {
    cmdAddWifi(text.substring(9)); return;
  }

  // /deletewifi SSID
  if (text.startsWith("/deletewifi ")) {
    cmdDeleteWifi(text.substring(12)); return;
  }

  // --- store as queued message ---
  if (messageCount < MAX_MESSAGES) {
    messageQueue[messageCount++] = text;
    savePrefs();
    bot.sendMessage(CHAT_ID,
      "Stored! (" + String(messageCount) + "/" + String(MAX_MESSAGES) + " queued)", "");
  } else {
    bot.sendMessage(CHAT_ID,
      "Queue full (" + String(MAX_MESSAGES) + " max). Use /clear to reset.", "");
  }
}

// ============================================================
//  SCHEDULED SEND — fire 3 messages on the right weekday/week/time
// ============================================================
void checkSendTime() {
  struct tm t;
  if (!getLocalTime(&t)) return;

  // Reset flag at midnight
  if (t.tm_yday != lastCheckedDay) {
    slotSentThisWeek = false;
    lastCheckedDay   = t.tm_yday;
  }
  if (slotSentThisWeek) return;

  // Weekday match?  (tm_wday 0=Sun; schedWeekday 1=Sun so +1)
  if ((t.tm_wday + 1) != schedWeekday) return;

  // Time match?
  if (t.tm_hour != schedHour || t.tm_min != schedMinute) return;

  // Week cadence match?
  int isoW = getISOWeek(&t);
  if (referenceWeek == -1) referenceWeek = isoW;          // first-boot anchor
  if ((isoW - referenceWeek) % schedEveryNWeeks != 0) return;

  // Fire!
  slotSentThisWeek = true;
  referenceWeek    = isoW;
  savePrefs();
  sendThreeMessages();
}

void sendThreeMessages() {
  for (int i = 0; i < 3; i++) {
    if (nextSendIndex >= messageCount) {
      bot.sendMessage(CHAT_ID,
        "Queue empty after " + String(i) + " message(s).", "");
      savePrefs();
      return;
    }
    bot.sendMessage(CHAT_ID, messageQueue[nextSendIndex], "");
    Serial.println("Sent msg #" + String(nextSendIndex + 1));
    nextSendIndex++;
    delay(500);
  }
  savePrefs();
}

// ============================================================
//  /status
// ============================================================
void sendStatus() {
  struct tm t;
  if (!getLocalTime(&t)) { bot.sendMessage(CHAT_ID, "Could not get time.", ""); return; }

  char dateBuf[16];
  snprintf(dateBuf, sizeof(dateBuf), "%02d/%02d/%04d",
           t.tm_mon + 1, t.tm_mday, t.tm_year + 1900);

  String dayNames[] = {"","Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"};

  String s;
  s  = "Bot Status\n";
  s += "Time: " + formatAMPM(t.tm_hour, t.tm_min) + ":" + pad(t.tm_sec) + " ET\n";
  s += "Date: " + String(dateBuf) + "\n\n";
  s += "Queue: " + String(messageCount - nextSendIndex) + " remaining";
  s += " (" + String(nextSendIndex) + " already sent)\n\n";
  s += "Schedule:\n";
  s += "  Day:      " + dayNames[schedWeekday] + "\n";
  s += "  Time:     " + formatAMPM(schedHour, schedMinute) + " ET\n";
  s += "  Interval: every " + String(schedEveryNWeeks) + " week(s)\n\n";
  s += "Type /help for all commands.";

  bot.sendMessage(CHAT_ID, s, "");
}

// ============================================================
//  /list
// ============================================================
void sendList() {
  if (messageCount == 0) {
    bot.sendMessage(CHAT_ID, "No messages stored yet.", "");
    return;
  }
  String list = "Stored messages:\n";
  for (int i = 0; i < messageCount; i++) {
    list += String(i + 1) + ". ";
    list += (i < nextSendIndex ? "[sent] " : "[queued] ");
    list += messageQueue[i] + "\n";
    if ((i + 1) % 10 == 0) {   // chunk to avoid 4096-char Telegram limit
      bot.sendMessage(CHAT_ID, list, "");
      list = "";
    }
  }
  if (list.length() > 0) bot.sendMessage(CHAT_ID, list, "");
}

// ============================================================
//  WiFi management
// ============================================================
void cmdAddWifi(String args) {
  args.trim();
  int   sp   = args.lastIndexOf(' ');
  String ssid = (sp < 0) ? args : args.substring(0, sp);
  String pass = (sp < 0) ? ""   : args.substring(sp + 1);

  if (ssid.length() == 0) {
    bot.sendMessage(CHAT_ID, "Usage: /addwifi SSID PASSWORD", ""); return;
  }
  int netCount = prefs.getInt("wifiCount", 0);

  // Update if already stored
  for (int i = 0; i < netCount; i++) {
    if (prefs.getString(("ssid" + String(i)).c_str(), "") == ssid) {
      prefs.putString(("pass" + String(i)).c_str(), pass);
      bot.sendMessage(CHAT_ID, "Updated: " + ssid, ""); return;
    }
  }
  if (netCount >= 10) {
    bot.sendMessage(CHAT_ID, "WiFi list full (10 max). Delete one first.", ""); return;
  }
  prefs.putString(("ssid" + String(netCount)).c_str(), ssid);
  prefs.putString(("pass" + String(netCount)).c_str(), pass);
  prefs.putInt("wifiCount", netCount + 1);
  bot.sendMessage(CHAT_ID, "Saved WiFi: " + ssid +
    " (" + String(netCount + 1) + " total)", "");
}

void cmdListWifi() {
  int n = prefs.getInt("wifiCount", 0);
  if (n == 0) { bot.sendMessage(CHAT_ID, "No WiFi networks saved.", ""); return; }
  String list = "Saved WiFi networks:\n";
  for (int i = 0; i < n; i++) {
    list += String(i + 1) + ". " + prefs.getString(("ssid" + String(i)).c_str(), "") + "\n";
  }
  list += "(Passwords hidden)";
  bot.sendMessage(CHAT_ID, list, "");
}

void cmdDeleteWifi(String args) {
  args.trim();
  int n = prefs.getInt("wifiCount", 0);
  int found = -1;
  for (int i = 0; i < n; i++) {
    if (prefs.getString(("ssid" + String(i)).c_str(), "") == args) { found = i; break; }
  }
  if (found < 0) { bot.sendMessage(CHAT_ID, "Not found: " + args, ""); return; }

  for (int i = found; i < n - 1; i++) {
    prefs.putString(("ssid" + String(i)).c_str(),
                    prefs.getString(("ssid" + String(i + 1)).c_str(), ""));
    prefs.putString(("pass" + String(i)).c_str(),
                    prefs.getString(("pass" + String(i + 1)).c_str(), ""));
  }
  prefs.remove(("ssid" + String(n - 1)).c_str());
  prefs.remove(("pass" + String(n - 1)).c_str());
  prefs.putInt("wifiCount", n - 1);
  bot.sendMessage(CHAT_ID, "Deleted: " + args, "");
}

void cmdClearWifi() {
  int n = prefs.getInt("wifiCount", 0);
  for (int i = 0; i < n; i++) {
    prefs.remove(("ssid" + String(i)).c_str());
    prefs.remove(("pass" + String(i)).c_str());
  }
  prefs.putInt("wifiCount", 0);
  bot.sendMessage(CHAT_ID,
    "All saved networks deleted. Staying on current connection.", "");
}

// ============================================================
//  Helpers
// ============================================================

// "14:05" -> "2:05 PM"
String formatAMPM(int h, int m) {
  int dh = h % 12;
  if (dh == 0) dh = 12;
  return String(dh) + ":" + pad(m) + (h < 12 ? " AM" : " PM");
}

String pad(int n) {
  return (n < 10) ? "0" + String(n) : String(n);
}

// ISO 8601 week number
int getISOWeek(struct tm* t) {
  int isoWday = (t->tm_wday == 0) ? 6 : t->tm_wday - 1;   // Mon=0
  int week    = (t->tm_yday - isoWday + 10) / 7;
  if (week < 1)  week = 53;
  if (week > 53) week = 1;
  return week;
}
