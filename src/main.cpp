#include <Arduino.h>
#include <WiFi.h>
#include <SPIFFS.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <BleKeyboard.h>

// --- WiFi AP (trustworthy name) ---
const char *ap_ssid = "AppleDevice-Setup";
const char *ap_pass = "Setup1234";

// --- BLE Keyboard (trustworthy name) ---
BleKeyboard bleKeyboard("Apple Keyboard", "Apple Inc.", 100);

// --- Web Server ---
AsyncWebServer server(80);
String pendingText;
volatile bool textPending = false;
String pendingShortcut;
volatile bool shortcutPending = false;

// --- LED ---
const int ledPin = 2;
bool clientConnected = false;
unsigned long ledLastToggle = 0;
const int ledInterval = 500;

// --- Setup WiFi AP ---
void setupAP()
{
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ap_ssid, ap_pass);
  Serial.printf("AP started: %s\nIP: %s\n", ap_ssid, WiFi.softAPIP().toString().c_str());
}

// --- Setup Web Server ---
void setupWeb()
{
  if (!SPIFFS.begin(true))
  {
    Serial.println("SPIFFS mount failed!");
    return;
  }

  // Serve the web interface
  server.serveStatic("/", SPIFFS, "/").setDefaultFile("index.html");

  // POST /sendText -> type text
  server.on("/sendText", HTTP_POST, [](AsyncWebServerRequest *req)
            {
    if (req->hasParam("data", true)) {
      pendingText = req->getParam("data", true)->value();
      textPending = true;
      req->send(200, "text/plain", "OK");
    } else req->send(400, "text/plain", "Missing data"); });

  // POST /shortcut -> send shortcut
  server.on("/shortcut", HTTP_POST, [](AsyncWebServerRequest *req)
            {
    if (req->hasParam("action", true)) {
      pendingShortcut = req->getParam("action", true)->value();
      shortcutPending = true;
      req->send(200, "text/plain", "OK");
    } else req->send(400, "text/plain", "Missing action"); });

  server.begin();
  Serial.println("WebServer started");
}

// --- Setup ---
void setup()
{
  Serial.begin(115200);
  delay(500);

  pinMode(ledPin, OUTPUT);
  digitalWrite(ledPin, LOW);

  setupAP();
  setupWeb();

  Serial.println("Starting BLE Keyboard...");
  bleKeyboard.begin();
}

// --- Loop ---
void loop()
{
  // Handle BLE connection
  static bool clientConnected = false;

  if (bleKeyboard.isConnected() && !clientConnected)
  {
    clientConnected = true;
    Serial.println("BLE client connected!");

    // Blink LED once quickly
    digitalWrite(ledPin, HIGH);
    delay(200); // short blink
    digitalWrite(ledPin, LOW);
  }
  else if (!bleKeyboard.isConnected() && clientConnected)
  {
    clientConnected = false;
    Serial.println("BLE client disconnected!");
  }

  // Handle text input
  if (textPending && bleKeyboard.isConnected())
  {
    bleKeyboard.print(pendingText);
    Serial.printf("[Keyboard] Sent Text: %s\n", pendingText.c_str());
    textPending = false;
  }

  // Handle shortcuts
  if (shortcutPending && bleKeyboard.isConnected())
  {
    Serial.printf("[Keyboard] Shortcut: %s\n", pendingShortcut.c_str());

    if (pendingShortcut == "openSafari")
    {
      bleKeyboard.press(KEY_LEFT_GUI);
      bleKeyboard.press(' ');
      bleKeyboard.releaseAll();
      delay(300);
      bleKeyboard.print("Safari");
      bleKeyboard.write(KEY_RETURN);
    }
    else if (pendingShortcut == "switchTab")
    {
      bleKeyboard.press(KEY_LEFT_GUI);
      bleKeyboard.press(KEY_RIGHT_ARROW);
      bleKeyboard.releaseAll();
    }
    else if (pendingShortcut == "closeApp")
    {
      bleKeyboard.press(KEY_LEFT_GUI);
      bleKeyboard.press('q');
      bleKeyboard.releaseAll();
    }
    else if (pendingShortcut == "minimize")
    {
      bleKeyboard.press(KEY_LEFT_GUI);
      bleKeyboard.press('h');
      bleKeyboard.releaseAll();
    }

    shortcutPending = false;
  }

  delay(10);
}
