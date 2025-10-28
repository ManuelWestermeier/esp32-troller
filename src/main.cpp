#include <Arduino.h>
#include <WiFi.h>
#include <SPIFFS.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <BleKeyboard.h>
#include <DNSServer.h>
#include <ArduinoJson.h>

#include <vector>

using namespace std;

// --- DNS captive portal ---
const byte DNS_PORT = 53;
DNSServer dnsServer;

// --- WiFi AP params ---
const char *ap_ssid = "AppleDevice-Setup";
const char *ap_pass = "Setup1234";

// --- BLE Keyboard ---
BleKeyboard bleKeyboard("Apple Keyboard", "Apple Inc.", 100);

// --- Web Server ---
AsyncWebServer server(80);

// --- SPIFFS chains file ---
const char *CHAINS_FILE = "/chains.json";

struct Chain
{
  String name;
  std::vector<String> commands; // each is a line/command
};
std::vector<Chain> chains;

// --- Pending actions from web UI ---
String pendingText = "";
volatile bool textPending = false;
String pendingShortcut = "";
volatile bool shortcutPending = false;

// LED for connection indicator
const int ledPin = 2;
bool clientConnected = false;
unsigned long ledLastToggle = 0;
const int ledInterval = 500;

// --- Helpers: trimming
static inline String trim(const String &s)
{
  int i = 0, j = (int)s.length() - 1;
  while (i <= j && isspace(s[i]))
    i++;
  while (j >= i && isspace(s[j]))
    j--;
  if (i == 0 && j == (int)s.length() - 1)
    return s;
  return s.substring(i, j + 1);
}

// --- Load / Save Chains ---
void loadChains()
{
  chains.clear();
  if (!SPIFFS.exists(CHAINS_FILE))
    return;
  File f = SPIFFS.open(CHAINS_FILE, "r");
  if (!f)
    return;

  DynamicJsonDocument doc(8192);
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err)
  {
    Serial.println("Failed to parse chains.json");
    return;
  }
  if (!doc.is<JsonArray>())
    return;

  for (JsonVariant v : doc.as<JsonArray>())
  {
    if (!v.is<JsonObject>())
      continue;
    JsonObject obj = v.as<JsonObject>();
    Chain c;
    c.name = obj["name"].as<const char *>();

    if (obj.containsKey("commands") && obj["commands"].is<JsonArray>())
    {
      for (JsonVariant cmd : obj["commands"].as<JsonArray>())
      {
        c.commands.push_back(String(cmd.as<const char *>()));
      }
    }
    chains.push_back(c);
  }
  Serial.printf("Loaded %u chains\n", (unsigned)chains.size());
}

void saveChains()
{
  DynamicJsonDocument doc(8192);
  JsonArray arr = doc.to<JsonArray>();
  for (auto &c : chains)
  {
    JsonObject obj = arr.createNestedObject();
    obj["name"] = c.name;
    JsonArray cmds = obj.createNestedArray("commands");
    for (auto &cmd : c.commands)
      cmds.add(cmd);
  }
  File f = SPIFFS.open(CHAINS_FILE, "w");
  if (!f)
  {
    Serial.println("Failed to open chains file for writing");
    return;
  }
  serializeJson(doc, f);
  f.close();
  Serial.println("Chains saved");
}

// --- Utility: find chain index by name or -1 ---
int findChainIndex(const String &name)
{
  for (size_t i = 0; i < chains.size(); ++i)
  {
    if (chains[i].name == name)
      return (int)i;
  }
  return -1;
}

// --- Parse commands string into vector (split on newline or semicolon) ---
std::vector<String> parseCommandsString(const String &s)
{
  std::vector<String> out;
  String temp;
  for (size_t i = 0; i < s.length(); ++i)
  {
    char c = s[i];
    if (c == '\n' || c == '\r' || c == ';')
    {
      if (temp.length())
      {
        out.push_back(trim(temp));
        temp = "";
      }
      // skip extra separators
    }
    else
    {
      temp += c;
    }
  }
  if (temp.length())
    out.push_back(trim(temp));
  return out;
}

// --- Map named key words to HID codes (returns 0 if unsupported) ---
uint8_t namedKeyToHID(const String &k)
{
  String key = k;
  key.toLowerCase();
  if (key == "enter" || key == "return")
    return KEY_RETURN;
  if (key == "tab")
    return KEY_TAB;
  if (key == "escape" || key == "esc")
    return KEY_ESC;
  if (key == "backspace" || key == "bs")
    return KEY_BACKSPACE;
  if (key == "left")
    return KEY_LEFT_ARROW;
  if (key == "right")
    return KEY_RIGHT_ARROW;
  if (key == "up")
    return KEY_UP_ARROW;
  if (key == "down")
    return KEY_DOWN_ARROW;
  if (key == "space")
    return ' ';
  // Add more if needed
  return 0;
}

// --- Send a single command line: either plain text or special combo like {ctrl+shift}+t or {enter} ---
void sendCommandLine(const String &line)
{
  if (line.length() == 0)
    return;
  // If format uses {} for modifiers/named keys
  if (line.startsWith("{"))
  {
    int end = line.indexOf('}');
    if (end != -1)
    {
      String inside = line.substring(1, end); // e.g. "ctrl+shift" or "enter"
      String rest = "";
      if (end + 1 < (int)line.length())
        rest = line.substring(end + 1); // maybe +x or +text
      // If rest starts with '+', remove it:
      if (rest.startsWith("+"))
        rest = rest.substring(1);

      // parse tokens inside braces
      bool pressCtrl = false, pressShift = false, pressAlt = false, pressMeta = false;
      String insideLower = inside;
      insideLower.toLowerCase();

      // tokens separated by '+'
      std::vector<String> tokens;
      String tmp;
      for (size_t i = 0; i <= insideLower.length(); ++i)
      {
        if (i == insideLower.length() || insideLower[i] == '+')
        {
          if (tmp.length())
          {
            tokens.push_back(trim(tmp));
            tmp = "";
          }
        }
        else
          tmp += insideLower[i];
      }
      String namedKey = "";
      for (auto &t : tokens)
      {
        if (t == "ctrl" || t == "control")
          pressCtrl = true;
        else if (t == "shift")
          pressShift = true;
        else if (t == "alt")
          pressAlt = true;
        else if (t == "meta" || t == "cmd" || t == "gui")
          pressMeta = true;
        else
        {
          // if it's not a modifier, treat as named key
          namedKey = t;
        }
      }

      // Apply modifier presses
      if (pressCtrl)
        bleKeyboard.press(KEY_LEFT_CTRL);
      if (pressShift)
        bleKeyboard.press(KEY_LEFT_SHIFT);
      if (pressAlt)
        bleKeyboard.press(KEY_LEFT_ALT);
      if (pressMeta)
        bleKeyboard.press(KEY_LEFT_GUI);

      delay(10);

      if (rest.length() > 0)
      {
        // use rest as text or single char
        if (rest.length() == 1)
        {
          bleKeyboard.press(rest.charAt(0));
          delay(20);
          bleKeyboard.releaseAll();
        }
        else
        {
          // longer text
          bleKeyboard.print(rest);
        }
      }
      else if (namedKey.length() > 0)
      {
        uint8_t code = namedKeyToHID(namedKey);
        if (code != 0)
        {
          bleKeyboard.press(code);
          delay(20);
          bleKeyboard.releaseAll();
        }
        else
        {
          // unknown named key, try typing its text
          bleKeyboard.print(namedKey);
        }
      }
      else
      {
        // just modifiers (e.g., "{ctrl}")
        // press & release a tiny delay to send modifier-only? We will press+release all
        delay(50);
      }

      // ensure everything released
      delay(10);
      bleKeyboard.releaseAll();
      delay(50);
      return;
    }
  }

  // Not a bracketed command — send as normal text
  bleKeyboard.print(line);
  delay(50);
}

// --- run a chain by name ---
void runChain(const String &name)
{
  int idx = findChainIndex(name);
  if (idx < 0)
  {
    Serial.printf("Chain not found: %s\n", name.c_str());
    return;
  }
  Serial.printf("Running chain: %s\n", name.c_str());
  Chain &c = chains[idx];
  for (auto &line : c.commands)
  {
    sendCommandLine(line);
    delay(150); // small delay between lines; adjust if needed
  }
  Serial.println("Chain finished");
}

// --- Setup DNS to redirect all DNS names to AP IP (captive portal) ---
void setupDNS()
{
  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
}

// --- Web endpoints setup ---
void setupWeb()
{
  // Serve static files from SPIFFS root. Place index.html in data/ folder for upload.
  server.serveStatic("/", SPIFFS, "/").setDefaultFile("index.html");

  // sendText endpoint (existing)
  server.on("/sendText", HTTP_POST, [](AsyncWebServerRequest *req)
            {
    if (req->hasParam("data", true)) {
      pendingText = req->getParam("data", true)->value();
      textPending = true;
      req->send(200, "text/plain", "OK");
    } else {
      req->send(400, "text/plain", "Missing data");
    } });

  // shortcut endpoint (existing simple actions)
  server.on("/shortcut", HTTP_POST, [](AsyncWebServerRequest *req)
            {
    if (req->hasParam("action", true)) {
      pendingShortcut = req->getParam("action", true)->value();
      shortcutPending = true;
      req->send(200, "text/plain", "OK");
    } else {
      req->send(400, "text/plain", "Missing action");
    } });

  // --- Chains API ---
  // GET /chains -> list all chains (JSON)
  server.on("/chains", HTTP_GET, [](AsyncWebServerRequest *req)
            {
    DynamicJsonDocument doc(8192);
    JsonArray arr = doc.to<JsonArray>();
    for (auto &c : chains) {
      JsonObject obj = arr.createNestedObject();
      obj["name"] = c.name;
      JsonArray ca = obj.createNestedArray("commands");
      for (auto &cmd : c.commands) ca.add(cmd);
    }
    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out); });

  // GET /chain?name= -> single chain
  server.on("/chain", HTTP_GET, [](AsyncWebServerRequest *req)
            {
    if (!req->hasParam("name")) {
      req->send(400, "text/plain", "Missing name");
      return;
    }
    String name = req->getParam("name")->value();
    int idx = findChainIndex(name);
    if (idx < 0) {
      req->send(404, "text/plain", "Not found");
      return;
    }
    DynamicJsonDocument doc(2048);
    JsonObject obj = doc.to<JsonObject>();
    obj["name"] = chains[idx].name;
    JsonArray ca = obj.createNestedArray("commands");
    for (auto &cmd : chains[idx].commands) ca.add(cmd);
    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out); });

  // POST /chain -> create or update chain
  // expects form fields: name (string), commands (text block: newline or ; separated)
  server.on("/chain", HTTP_POST, [](AsyncWebServerRequest *req)
            {
    if (!req->hasParam("name", true) || !req->hasParam("commands", true)) {
      req->send(400, "text/plain", "Missing params");
      return;
    }
    String name = req->getParam("name", true)->value();
    String commandsBlob = req->getParam("commands", true)->value();

    std::vector<String> cmds = parseCommandsString(commandsBlob);

    int idx = findChainIndex(name);
    if (idx >= 0) {
      // update
      chains[idx].commands.clear();
      for (auto &c : cmds) chains[idx].commands.push_back(c);
    } else {
      Chain c;
      c.name = name;
      for (auto &it : cmds) c.commands.push_back(it);
      chains.push_back(c);
    }
    saveChains();
    req->send(200, "text/plain", "Saved"); });

  // POST /runChain : name param
  server.on("/runChain", HTTP_POST, [](AsyncWebServerRequest *req)
            {
    if (!req->hasParam("name", true)) {
      req->send(400, "text/plain", "Missing name");
      return;
    }
    String name = req->getParam("name", true)->value();
    req->send(200, "text/plain", "Executing");
    // run in loop context (we'll store a requested-run? — but here we'll run directly if ble connected)
    // To avoid long work inside async callback, schedule via a small lambda that runs asynchronously via Task (but keep simple here).
    // We'll run immediately but only if BLE connected:
    if (bleKeyboard.isConnected()) {
      runChain(name);
    } else {
      Serial.println("BLE not connected; cannot run chain now.");
    } });

  // POST /deleteChain : name param
  server.on("/deleteChain", HTTP_POST, [](AsyncWebServerRequest *req)
            {
    if (!req->hasParam("name", true)) {
      req->send(400, "text/plain", "Missing name");
      return;
    }
    String name = req->getParam("name", true)->value();
    int idx = findChainIndex(name);
    if (idx >= 0) {
      chains.erase(chains.begin() + idx);
      saveChains();
      req->send(200, "text/plain", "Deleted");
    } else {
      req->send(404, "text/plain", "Not found");
    } });

  server.begin();
  Serial.println("WebServer started");
}

// --- Setup AP & WiFi ---
void setupAP()
{
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ap_ssid, ap_pass);
  Serial.printf("AP started: %s\nIP: %s\n", ap_ssid, WiFi.softAPIP().toString().c_str());
}

// --- Process DNS in loop ---
void handleDNS()
{
  dnsServer.processNextRequest();
}

// --- Initial setup ---
void setup()
{
  Serial.begin(115200);
  delay(500);

  pinMode(ledPin, OUTPUT);
  digitalWrite(ledPin, LOW);

  if (!SPIFFS.begin(true))
  {
    Serial.println("SPIFFS mount failed!");
    // continue anyway
  }
  else
  {
    Serial.println("SPIFFS mounted");
  }

  setupAP();
  setupDNS();

  loadChains();

  setupWeb();

  Serial.println("Starting BLE Keyboard...");
  bleKeyboard.begin();
}

// --- Main loop ---
void loop()
{
  // captive DNS processing
  handleDNS();

  // handle BLE connection events for LED
  static bool wasConnected = false;
  if (bleKeyboard.isConnected() && !wasConnected)
  {
    wasConnected = true;
    Serial.println("BLE client connected!");
    // quick blink
    digitalWrite(ledPin, HIGH);
    delay(200);
    digitalWrite(ledPin, LOW);
  }
  else if (!bleKeyboard.isConnected() && wasConnected)
  {
    wasConnected = false;
    Serial.println("BLE client disconnected!");
  }

  // handle pending text from web
  if (textPending && bleKeyboard.isConnected())
  {
    // copy to local to avoid concurrency issues
    noInterrupts();
    String txt = pendingText;
    textPending = false;
    interrupts();

    bleKeyboard.print(txt);
    Serial.printf("[Keyboard] Sent Text: %s\n", txt.c_str());
  }

  // handle pending shortcut actions
  if (shortcutPending && bleKeyboard.isConnected())
  {
    noInterrupts();
    String action = pendingShortcut;
    shortcutPending = false;
    interrupts();

    Serial.printf("[Keyboard] Shortcut: %s\n", action.c_str());
    if (action == "openSafari")
    {
      // Cmd + Space, type "Safari", Enter (iPad/macOS style, but behavior depends)
      bleKeyboard.press(KEY_LEFT_GUI);
      bleKeyboard.press(' ');
      bleKeyboard.releaseAll();
      delay(300);
      bleKeyboard.print("Safari");
      bleKeyboard.write(KEY_RETURN);
    }
    else if (action == "switchTab")
    {
      bleKeyboard.press(KEY_LEFT_GUI);
      bleKeyboard.press(KEY_RIGHT_ARROW);
      bleKeyboard.releaseAll();
    }
    else if (action == "closeApp")
    {
      bleKeyboard.press(KEY_LEFT_GUI);
      bleKeyboard.press('q');
      bleKeyboard.releaseAll();
    }
    else if (action == "minimize")
    {
      bleKeyboard.press(KEY_LEFT_GUI);
      bleKeyboard.press('h');
      bleKeyboard.releaseAll();
    }
    else if (action == "copy")
    {
      bleKeyboard.press(KEY_LEFT_CTRL);
      bleKeyboard.press('c');
      bleKeyboard.releaseAll();
    }
    else if (action == "paste")
    {
      bleKeyboard.press(KEY_LEFT_CTRL);
      bleKeyboard.press('v');
      bleKeyboard.releaseAll();
    }
    else if (action == "undo")
    {
      bleKeyboard.press(KEY_LEFT_CTRL);
      bleKeyboard.press('z');
      bleKeyboard.releaseAll();
    }
    else if (action == "redo")
    {
      bleKeyboard.press(KEY_LEFT_CTRL);
      bleKeyboard.press(KEY_LEFT_SHIFT);
      bleKeyboard.press('z');
      bleKeyboard.releaseAll();
    }
  }

  delay(10);
}
