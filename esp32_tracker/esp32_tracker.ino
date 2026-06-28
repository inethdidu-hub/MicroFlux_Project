#include <Arduino.h>
#include <TinyGPS++.h>
#include <Preferences.h>       
#include "soc/soc.h"           
#include "soc/rtc_cntl_reg.h"  
#include <driver/uart.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

// ThingSpeak ThingHTTP Proxy configuration
const String TS_URL = "http://api.thingspeak.com/apps/thinghttp/send_request";
String tsGetCommandsKey = "CKJ2JJ1UUD3ED45G";
String tsWriteTelemetryKey = "ERAUMAMO68Y7GXSM";
String tsResetCommandsKey = "0H7FWFTR8CTF5Y59";

// ThingSpeak MQTT Configuration
String tsMqttHost = "mqtt3.thingspeak.com";
const int tsMqttPort = 1883;
String tsChannelID = "3418086";                  // ThingSpeak Channel ID
String tsMqttClientID = "DDYqNyUwKjQVCyguAwwVHhY";  // ThingSpeak MQTT Client ID
String tsMqttUsername = "DDYqNyUwKjQVCyguAwwVHhY";  // ThingSpeak MQTT Username
String tsMqttPassword = "DwINN09LTBCMgesANiAIbRi7";  // ThingSpeak MQTT Password
bool mqttActive = false;

const int ledPin = 14, buzzerPin = 13, onboardLED = 2, reedPin = 26, voltPin = 34;

// Map A9G directly to Serial (UART0) to match the custom PCB routing
#define A9G Serial

TinyGPSPlus gps;
Preferences prefs;

String userPhone = "", apn = "mobitel", lastSmsSender = "", a9gBuffer = "", lastCallerPhone = "";
bool gprsConnected = false, incomingSmsFlag = false;
bool ussdPending = false, riskZone = false, appAlarm = false, appLight = false, lastMagnetAway = false;
unsigned long lastConnectAttemptTime = 0, ussdSentTime = 0;
const unsigned long RECONNECT_INTERVAL = 60000;

// Variables for fast GPS fix, low latency non-blocking polling, and LBS fallback
bool isLbsLocation = false;
double lbsLat = 0.0, lbsLon = 0.0;
enum HTTPState { HTTP_IDLE, HTTP_GET_PENDING, HTTP_POST_PENDING };
HTTPState httpState = HTTP_IDLE;
unsigned long httpStartTime = 0;
unsigned long lastCommandPoll = 0;
unsigned long lastTelemetryUpload = 0;
unsigned long lastLbsQuery = 0;
int httpFailCount = 0;
unsigned long lastA9GActivity = 0;

// Variables for persistent preferences and real-time local alert state
bool appBuzzer = false, appLed = false;
bool rcOption = false;
bool appAlarmActive = false, riskZoneActive = false, appLightActive = false;
unsigned long lastGprsAttempt = 0;

// Variables for GPS stationary drift and accuracy filtering
double lastUploadedLat = 0.0, lastUploadedLon = 0.0; // Default: 0.0 to indicate no location available yet
unsigned long lastCoordinateUpdateTime = 0;

// Wi-Fi Geolocation API configuration (Unwired Labs - FREE, no credit card required)
String unwiredToken = "pk.165ed003b34e8fe7a8c4cbb035d6269b"; // User's Unwired Labs Token
bool isWifiLocation = false;
double wifiLat = 0.0, wifiLon = 0.0;
unsigned long lastWifiLocQuery = 0;

// Battery query variables (uses A9G internal PMU via AT+CBC, no external voltage sensor needed)
int a9gBatteryLevel = 100;
unsigned long lastBatteryCheck = 0;
int csqVal = 0;

// Variables for conditional telemetry upload (limits GPRS transmission to prevent brownout reboots)
bool lastUploadedTamper = false;

// Wi-Fi and Hybrid Network Manager variables
enum NetworkMode { NET_WIFI, NET_GPRS };
NetworkMode currentNetMode = NET_GPRS;
String wifiSSID = "";
String wifiPassword = "";
unsigned long lastWiFiScanTime = 0;

// Forward Declarations to ensure clean compilation
void printPC(String msg);
void performWiFiGetCommands();
void queryWiFiLocation();
double parseJsonDouble(String json, String key);
void learnWiFiLocation(double lat, double lon);
String cleanMac(String mac);
bool sendCmd(String cmd, String expected, unsigned long timeoutMs);
bool initGPRS();
bool waitNetworkRegister();
void updateLocalAlerts();
void sendSMS(String phone, String text);
void sendLocationSMS(String phone);
void httpPatch(String path, String payload);
void handleA9GLine(String line);
void processSecureFirstLogic(String payload);
void processIncomingSMS(String msg, String sender);
String textToHex(String text);
bool isHexString(String s);
String decodeHex(String hexStr);
void triggerUSSD();
String escapeQuotes(String input);
String urlEncode(String str);
String sanitizeJsonString(String s);
double parseNmeaCoord(String term, String dir);
void hardwarePowerOnA9G(bool forceRestart = false);
bool parseJsonBool(String json, String key);
String parseJsonString(String json, String key);
int readLocalBattery();

// Helper function to print clean debug messages to PC Serial Monitor (also ignored by A9G)
void printPC(String msg) {
  Serial.println(msg);
  Serial.flush();
}

// Pulse Pin 15 to ground PWR_KEY via transistor Q2 for 2 seconds to hardware-boot A9G
// Only toggles PWR_KEY if the module is unresponsive (OFF), restores GPS stream directly if ON
void hardwarePowerOnA9G(bool forceRestart) {
  bool responsive = false;
  
  if (!forceRestart) {
    printPC("[SYSTEM] Checking if A9G is responsive...");
    // Flush any startup garbage from serial buffer
    while (A9G.available() > 0) A9G.read();
    
    // Send simple AT test commands directly
    for (int i = 0; i < 3; i++) {
      A9G.println("AT");
      unsigned long start = millis();
      while (millis() - start < 400) {
        if (A9G.available() > 0) {
          String r = A9G.readString();
          if (r.indexOf("OK") != -1 || r.indexOf("$GN") != -1 || r.indexOf("$GP") != -1 || 
              r.indexOf("+GPSRD") != -1 || r.indexOf("CME ERROR") != -1 || r.indexOf("READY") != -1) {
            responsive = true;
            break;
          }
        }
      }
      if (responsive) break;
      delay(200);
    }
  }
  
  if (responsive && !forceRestart) {
    printPC("[SYSTEM] A9G is responsive. Restoring GPS stream...");
    A9G.println("AT+GPS=1");
    delay(100);
    A9G.println("AT+GPSRD=2");
  } else {
    int retries = 0;
    while (!responsive && retries < 3) {
      retries++;
      if (forceRestart) {
        printPC("[SYSTEM] GPS stream silent: Forcing A9G hardware reset (Attempt " + String(retries) + "/3)...");
      } else {
        printPC("[SYSTEM] A9G is unresponsive/OFF. Pulsing PWR_KEY (Pin 15) to turn ON... (Attempt " + String(retries) + "/3)");
      }
      pinMode(15, OUTPUT);
      digitalWrite(15, HIGH); // Ground PWR_KEY
      delay(2000);            // Hold for 2 seconds
      digitalWrite(15, LOW);   // Release PWR_KEY
      
      printPC("[SYSTEM] Waiting 8s for A9G boot...");
      delay(8000);            // Allow A9G to start up
      
      // Verify responsiveness after pulse
      printPC("[SYSTEM] Checking A9G responsiveness after boot pulse...");
      while (A9G.available() > 0) A9G.read(); // Flush garbage
      for (int i = 0; i < 3; i++) {
        A9G.println("AT");
        unsigned long start = millis();
        while (millis() - start < 400) {
          if (A9G.available() > 0) {
            String r = A9G.readString();
            if (r.indexOf("OK") != -1 || r.indexOf("$GN") != -1 || r.indexOf("$GP") != -1 || 
                r.indexOf("+GPSRD") != -1 || r.indexOf("CME ERROR") != -1 || r.indexOf("READY") != -1) {
              responsive = true;
              break;
            }
          }
        }
        if (responsive) break;
        delay(200);
      }
      
      if (responsive) {
        printPC("[SYSTEM] A9G booted successfully and is responsive!");
        break;
      } else {
        printPC("[SYSTEM] A9G boot verification failed! Retrying PWR_KEY pulse...");
      }
    }
    
    // Initialize GPS once responsive
    A9G.println("AT+GPS=1");
    delay(100);
    A9G.println("AT+GPSRD=2");
  }
}

void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); 
  
  // Set larger RX buffer size BEFORE beginning Serial to prevent packet loss / truncation
  Serial.setRxBufferSize(1024);
  Serial.begin(115200);
  delay(1000);
  
  printPC("[SYSTEM] ESP32 Booted. Waiting 5s for A9G initialization...");
  delay(5000); // Allow A9G CPU to start up before checking responsiveness
  
  // Auto-boot A9G on startup
  hardwarePowerOnA9G();

  pinMode(ledPin, OUTPUT); 
  pinMode(buzzerPin, OUTPUT); 
  pinMode(onboardLED, OUTPUT); 
  pinMode(reedPin, INPUT_PULLUP);

  // Startup beep test to verify buzzer hardware wiring on Pin 13
  printPC("[SYSTEM] Running startup buzzer beep test on Pin 13...");
  digitalWrite(buzzerPin, HIGH);
  delay(200);
  digitalWrite(buzzerPin, LOW);
  delay(100);
  digitalWrite(buzzerPin, HIGH);
  delay(200);
  digitalWrite(buzzerPin, LOW);

  prefs.begin("system-config", false);
  userPhone = prefs.getString("userPhone", "");
  rcOption = prefs.getBool("rc", false);
  appBuzzer = prefs.getBool("appBuzzer", false);
  appLed = prefs.getBool("appLed", false);
  apn = prefs.getString("apn", "mobitel");
  wifiSSID = prefs.getString("wifiSSID", "Ineth's Network");
  wifiPassword = prefs.getString("wifiPassword", "fvfl5152");
  wifiLat = prefs.getDouble("wifiLat", 0.0);
  wifiLon = prefs.getDouble("wifiLon", 0.0);
  if (wifiLat != 0.0 && wifiLon != 0.0) {
    isWifiLocation = true;
  }
  if (apn == "dialogbb") {
    apn = "mobitel";
    prefs.putString("apn", apn);
  }
  prefs.end();

  delay(2000);

  // Initialize Wi-Fi connection if credentials are configured
  if (wifiSSID.length() > 0) {
    printPC("[SYSTEM] Wi-Fi SSID configured: " + wifiSSID + ". Attempting connection...");
    WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str());
    
    unsigned long startAttempt = millis();
    bool connected = false;
    while (millis() - startAttempt < 15000) {
      if (WiFi.status() == WL_CONNECTED) {
        connected = true;
        break;
      }
      delay(500);
      printPC(".");
    }
    
    if (connected) {
      currentNetMode = NET_WIFI;
      printPC("\n[SYSTEM] Wi-Fi connected successfully! IP: " + WiFi.localIP().toString());
      gprsConnected = false;
      // Configure A9G for GPS operation only (no GPRS context active)
      A9G.println("AT+GPS=1");
      delay(100);
      A9G.println("AT+GPSRD=2");
    } else {
      printPC("\n[SYSTEM] Wi-Fi connection timed out. Falling back to GPRS...");
      WiFi.disconnect(true);
      currentNetMode = NET_GPRS;
      gprsConnected = initGPRS();
    }
  } else {
    printPC("[SYSTEM] No Wi-Fi SSID configured. Starting GPRS...");
    currentNetMode = NET_GPRS;
    gprsConnected = initGPRS();
  }
  
  lastA9GActivity = millis();
}

bool sendCmd(String cmd, String expected, unsigned long timeoutMs) {
  lastA9GActivity = millis(); // Refresh activity watchdog when sending commands
  // Flush serial RX buffer first to prevent backlog congestion
  while (A9G.available() > 0) A9G.read();
  A9G.println(cmd);
  unsigned long start = millis();
  String response = "";
  while (millis() - start < timeoutMs) {
    while (A9G.available() > 0) {
      char c = A9G.read();
      gps.encode(c);
      if (c == '\n') {
        response.trim();
        if (response.length() > 0) {
          printPC("[A9G-RAW] " + response);
          if (response.indexOf(expected) != -1) return true;
          if (response.indexOf("ERROR") != -1) return false;
          handleA9GLine(response);
        }
        response = "";
      } else if (c != '\r') response += c;
    }
    delay(5);
  }
  return false;
}

bool waitNetworkRegister() {
  printPC("[GPRS] Checking network registration...");
  for (int i = 0; i < 20; i++) {
    // Flush NMEA data before sending CREG query
    while (A9G.available() > 0) {
      A9G.read();
    }
    
    A9G.println("AT+CREG?");
    unsigned long start = millis();
    String resp = "";
    while (millis() - start < 1000) {
      while (A9G.available() > 0) {
        char c = A9G.read();
        gps.encode(c);
        resp += c;
      }
      delay(5);
    }
    resp.trim();
    if (resp.length() > 0) {
      printPC("[A9G-REG] Response: " + resp);
      if (resp.indexOf("+CREG: 1,1") != -1 || resp.indexOf("+CREG: 1,5") != -1 || 
          resp.indexOf("+CREG: 0,1") != -1 || resp.indexOf("+CREG: 0,5") != -1 ||
          resp.indexOf("+CREG: 2,1") != -1 || resp.indexOf("+CREG: 2,5") != -1 ||
          resp.indexOf("+CREG: 1") != -1 || resp.indexOf("+CREG: 5") != -1) {
        printPC("[GPRS] Registered on network successfully!");
        return true;
      }
    } else {
      printPC("[A9G-REG] No response. Retrying AT handshake to sync baudrate...");
      A9G.println("AT");
    }
    delay(500);
  }
  return false;
}

bool initGPRS() {
  printPC("[GPRS] Connecting to mobile internet...");
  lastGprsAttempt = millis();
  
  // Auto-baud lock handshake and responsiveness check
  bool responsive = false;
  for (int i = 0; i < 5; i++) {
    if (sendCmd("AT", "OK", 500)) {
      responsive = true;
    }
  }
  
  if (!responsive) {
    printPC("[GPRS] A9G is unresponsive/OFF. Powering ON first...");
    hardwarePowerOnA9G(false); // Boot A9G
    // Retry handshake once more after boot
    for (int i = 0; i < 5; i++) {
      sendCmd("AT", "OK", 500);
    }
  }
  
  // Turn off GPS stream immediately during connection to prevent serial UART congestion
  sendCmd("AT+GPSRD=0", "OK", 1000);
  
  sendCmd("ATE0", "OK", 1000); // Turn off local echo
  
  // Reset and restore fundamental GSM/SMS/GPS settings
  sendCmd("AT+GPS=1", "OK", 1000);
  sendCmd("AT+CMGF=1", "OK", 1000);
  sendCmd("AT+CPMS=\"SM\",\"SM\",\"SM\"", "OK", 1500); // Select SIM memory
  sendCmd("AT+CMGD=1,4", "OK", 2000);                 // Clear SMS memory
  sendCmd("AT+CSCS=\"GSM\"", "OK", 1000);
  sendCmd("AT+CNMI=2,2,0,0,0", "OK", 1000);
  
  // Wait for network registration first to prevent GPRS attachment errors
  if (!waitNetworkRegister()) {
    printPC("[GPRS] Network registration failed!");
    return false;
  }
  
  sendCmd("AT+CSQ", "OK", 1000);    
  
  // 1. Attach GPRS
  bool attached = false;
  for (int i = 0; i < 3; i++) {
    printPC("[GPRS] Attempting GPRS attach (AT+CGATT=1) " + String(i + 1) + "/3...");
    while (A9G.available() > 0) A9G.read(); // Flush garbage
    A9G.println("AT+CGATT=1");
    unsigned long start = millis();
    while (millis() - start < 10000) { // Wait up to 10 seconds
      if (A9G.available() > 0) {
        String line = A9G.readStringUntil('\n');
        line.trim();
        if (line.length() > 0) {
          printPC("[A9G-RAW] " + line);
          if (line.indexOf("OK") != -1 || line.indexOf("+CGATT:1") != -1 || line.indexOf("+CGATT: 1") != -1) {
            attached = true;
            break;
          }
        }
      }
      delay(5);
    }
    if (attached) break;
    delay(2000); // Wait 2s before retry
  }
  if (!attached) {
    printPC("[GPRS] GPRS attachment failed!");
    return false;
  }
  
  // 2. Set APN parameters
  sendCmd("AT+CGDCONT=1,\"IP\",\"" + apn + "\"", "OK", 3000);
  
  // 3. Activate PDP context
  bool contextActive = false;
  for (int i = 0; i < 3; i++) {
    printPC("[GPRS] Activating PDP Context (AT+CGACT=1,1) " + String(i + 1) + "/3...");
    while (A9G.available() > 0) A9G.read(); // Flush garbage
    A9G.println("AT+CGACT=1,1");
    unsigned long start = millis();
    while (millis() - start < 10000) { // Wait up to 10 seconds
      if (A9G.available() > 0) {
        String line = A9G.readStringUntil('\n');
        line.trim();
        if (line.length() > 0) {
          printPC("[A9G-RAW] " + line);
          if (line.indexOf("OK") != -1) {
            contextActive = true;
            break;
          }
        }
      }
      delay(5);
    }
    if (contextActive) break;
    A9G.println("AT+CGACT=0,1");
    delay(2000); // Wait 2s before retry
  }
  if (!contextActive) {
    printPC("[GPRS] PDP context activation failed!");
    return false;
  }
  
  // Use default operator DNS (Mobitel/Dialog) to prevent Google DNS block issues
  // sendCmd("AT+CDNSCFG=\"8.8.8.8\",\"8.8.4.4\"", "OK", 2000);
  
  // Wait for valid IP address allocation before enabling AGPS
  printPC("[GPRS] Waiting for IP address allocation...");
  bool hasIp = false;
  for (int i = 0; i < 15; i++) { // Wait up to 15 seconds
    delay(1000);
    while (A9G.available() > 0) A9G.read(); // Flush buffer
    A9G.println("AT+CGPADDR=1");
    unsigned long start = millis();
    String r = "";
    while (millis() - start < 1500) {
      if (A9G.available() > 0) {
        char c = A9G.read();
        r += c;
      }
      delay(5);
    }
    r.trim();
    printPC("[GPRS] IP Check: " + r);
    if (r.indexOf("+CGPADDR:") != -1 && r.indexOf("0.0.0.0") == -1 && r.indexOf("ERROR") == -1) {
      printPC("[GPRS] IP Allocated successfully.");
      hasIp = true;
      break;
    }
  }
  if (!hasIp) {
    printPC("[GPRS] IP allocation failed!");
    return false;
  }
  
  // Enable AGPS (Assisted GPS) for low SNR satellite tracking once GPRS is connected
  printPC("[GPS] GPRS active. Enabling AGPS (Assisted GPS) for low SNR sensitivity...");
  sendCmd("AT+GPS=0", "OK", 1000);
  delay(500);
  if (sendCmd("AT+AGPS=1", "OK", 8000)) {
    printPC("[GPS] AGPS enabled successfully!");
  } else {
    printPC("[GPS] AGPS enabling failed. Initializing standard GPS...");
    sendCmd("AT+GPS=1", "OK", 2000);
  }
  sendCmd("AT+GPSRD=2", "OK", 1000); // 10-second rate for stable NMEA stream support
  
  // 4. Connect to MQTT Broker (ThingSpeak) if credentials are configured
  if (tsChannelID == "YOUR_CHANNEL_ID" || tsMqttClientID == "YOUR_CLIENT_ID" || 
      tsMqttUsername == "YOUR_MQTT_USERNAME" || tsMqttPassword == "YOUR_MQTT_PASSWORD") {
    printPC("[MQTT] Credentials not configured. Bypassing MQTT and falling back to ThingHTTP.");
    mqttActive = false;
  } else {
    bool mqttConnected = false;
    printPC("[MQTT] Connecting to ThingSpeak MQTT Broker (" + tsMqttHost + ")...");
    for (int i = 0; i < 3; i++) {
      while (A9G.available() > 0) A9G.read(); // Flush buffer
      A9G.println("AT+MQTTCONN=\"" + tsMqttHost + "\"," + String(tsMqttPort) + ",\"" + tsMqttClientID + "\",120,0,\"" + tsMqttUsername + "\",\"" + tsMqttPassword + "\"");
      unsigned long start = millis();
      while (millis() - start < 10000) { // Wait up to 10s for broker connection
        if (A9G.available() > 0) {
          String line = A9G.readStringUntil('\n');
          line.trim();
          if (line.length() > 0) {
            printPC("[A9G-RAW] " + line);
            if (line.indexOf("OK") != -1 || line.indexOf("ALREADY") != -1) {
              mqttConnected = true;
              break;
            }
          }
        }
        delay(5);
      }
      if (mqttConnected) break;
      delay(2000);
    }

    if (mqttConnected) {
      printPC("[MQTT] Connected to broker successfully!");
      mqttActive = true;
      
      // Subscribe to commands topic: channels/<channelID>/subscribe/json
      String subTopic = "channels/" + tsChannelID + "/subscribe/json";
      while (A9G.available() > 0) A9G.read(); // Flush buffer
      A9G.println("AT+MQTTSUB=\"" + subTopic + "\",0");
      unsigned long start = millis();
      while (millis() - start < 3000) {
        if (A9G.available() > 0) {
          String line = A9G.readStringUntil('\n');
          line.trim();
          if (line.length() > 0) {
            printPC("[A9G-RAW] " + line);
          }
        }
        delay(5);
      }
    } else {
      printPC("[MQTT] Connection failed! Falling back to ThingHTTP.");
      mqttActive = false;
    }
  }
  
  return true;
}

void sendSMS(String phone, String text) {
  if (phone.length() == 0) return;
  A9G.print("AT+CMGS=\"");
  A9G.print(phone);
  A9G.print("\"\r\n");
  delay(500);
  A9G.print(text);
  A9G.write(0x1A);
  delay(500);
}

void sendLocationSMS(String phone) {
  double lat = 0.0;
  double lon = 0.0;
  bool isLbsUsed = false;
  bool isWifiUsed = false;
  bool hasLocation = gps.location.isValid() || isWifiLocation || isLbsLocation || (lastUploadedLat != 0.0 && lastUploadedLon != 0.0);
  
  if (gps.location.isValid()) {
    lat = gps.location.lat();
    lon = gps.location.lng();
  } else if (isWifiLocation) {
    lat = wifiLat;
    lon = wifiLon;
    isWifiUsed = true;
  } else if (isLbsLocation) {
    lat = lbsLat;
    lon = lbsLon;
    isLbsUsed = true;
  } else if (lastUploadedLat != 0.0 && lastUploadedLon != 0.0) {
    lat = lastUploadedLat;
    lon = lastUploadedLon;
  }
  
  String mapUrl;
  if (!hasLocation) {
    mapUrl = "Microflux: GPS/Wi-Fi/LBS location not valid yet (searching for network/satellites).";
  } else {
    mapUrl = "Microflux Location: https://www.google.com/maps?q=" + String(lat, 6) + "," + String(lon, 6);
    if (isWifiUsed) {
      mapUrl += " (Wi-Fi Zone)";
    } else if (isLbsUsed) {
      mapUrl += " (Cell-Tower LBS)";
    }
  }
  sendSMS(phone, mapUrl);
}

void httpPatch(String path, String payload) {
  if (currentNetMode == NET_WIFI) {
    WiFiClientSecure client;
    client.setInsecure(); // Bypass certificate validation for simplicity
    HTTPClient http;
    String url = "https://microplux-anti-theft-app-2026-default-rtdb.firebaseio.com/" + path;
    http.begin(client, url);
    http.addHeader("Content-Type", "application/json");
    int httpCode = http.sendRequest("PATCH", payload);
    if (httpCode > 0) {
      printPC("[HTTP-WIFI] PATCH successful. Code: " + String(httpCode) + " | Payload: " + payload);
    } else {
      printPC("[HTTP-WIFI] PATCH failed. Error: " + http.errorToString(httpCode));
    }
    http.end();
  } else {
    if (mqttActive && path.indexOf("bag_data") != -1) {
      String latVal = parseJsonString(payload, "lat");
      String lonVal = parseJsonString(payload, "lon");
      String batVal = parseJsonString(payload, "battery");
      String tamperVal = parseJsonString(payload, "tamper");
      String lbsVal = parseJsonString(payload, "lbs");
      String distVal = parseJsonString(payload, "wifi_dist");
      String gpsVal = parseJsonString(payload, "gps_locked");
      String csqValStr = parseJsonString(payload, "csq");
      
      // Map telemetry parameters to ThingSpeak channel fields
      // field1=lat, field2=lon, field3=battery, field4=tamper, field5=lbs, field6=wifi_dist, field7=gps_locked, field8=csq
      String fieldTamper = (tamperVal == "true") ? "1" : "0";
      String fieldLbs = (lbsVal == "true") ? "1" : "0";
      String fieldGps = (gpsVal == "true") ? "1" : "0";
      
      String pubPayload = "field1=" + latVal + "&field2=" + lonVal + "&field3=" + batVal + "&field4=" + fieldTamper + "&field5=" + fieldLbs + "&field6=" + distVal + "&field7=" + fieldGps + "&field8=" + csqValStr;
      String pubTopic = "channels/" + tsChannelID + "/publish";
      
      while (A9G.available() > 0) A9G.read(); // Flush buffer
      A9G.println("AT+MQTTPUB=\"" + pubTopic + "\",\"" + pubPayload + "\",0,0");
      printPC("[MQTT] Published telemetry: " + pubPayload);
    } else {
      String apiKey = (path.indexOf("commands") != -1) ? tsResetCommandsKey : tsWriteTelemetryKey;
      String url = TS_URL;
      String postData = "api_key=" + apiKey + "&payload=" + urlEncode(payload);
      
      A9G.println("AT+GPSRD=0"); // Disable NMEA stream during HTTP transmission to prevent data corruption
      delay(50);
      
      A9G.print("AT+HTTPPOST=\"");
      A9G.print(url);
      A9G.print("\",\"application/x-www-form-urlencoded\",\"");
      A9G.print(postData);
      A9G.print("\"\r\n");
      printPC("[HTTP] Sent PATCH payload via ThingHTTP POST (" + path + "): " + payload);
    }
  }
}

void loop() {
  unsigned long now = millis();

  // 1. Read A9G serial data and pass to parser/GPS
  while (A9G.available() > 0) {
    char c = A9G.read();
    gps.encode(c);
    if (c == '\n') {
      a9gBuffer.trim();
      if (a9gBuffer.length() > 0) handleA9GLine(a9gBuffer);
      a9gBuffer = "";
    } else if (c != '\r') a9gBuffer += c;
  }

  // 2. HTTP timeout watchdog (GPRS only - clear busy state if request takes too long)
  if (currentNetMode == NET_GPRS && httpState != HTTP_IDLE && (now - httpStartTime > 20000)) {
    printPC("[HTTP] Watchdog timeout! Resetting HTTP state to IDLE...");
    httpState = HTTP_IDLE;
    httpFailCount++;
    A9G.println("AT+GPSRD=2"); // Restore NMEA stream on watchdog timeout
  }

  // 3. USSD timeout watchdog
  if (ussdPending && (now - ussdSentTime > 15000)) {
    printPC("[USSD] Timeout! Cancelling USSD session...");
    A9G.println("AT+CUSD=2");
    delay(100);
    A9G.println("AT+CSCS=\"GSM\"");
    ussdPending = false;
  }

  // 4. Onboard LED status indicator
  // If Wi-Fi is connected, it turns ON if secure, or pulses if tamper alert
  bool isOnline = (currentNetMode == NET_WIFI) ? (WiFi.status() == WL_CONNECTED) : gprsConnected;
  digitalWrite(onboardLED, isOnline ? digitalRead(reedPin) : ((now / 200) % 2 == 0));

  // 5. Watchdog to check if A9G has crashed or powered OFF (no activity for 45 seconds - GPRS only)
  if (currentNetMode == NET_GPRS) {
    if (gprsConnected && (millis() - lastA9GActivity > 45000)) {
      printPC("[SYSTEM] A9G Watchdog (GPRS): No activity for 45s. Powering ON...");
      hardwarePowerOnA9G();
      gprsConnected = false;
      httpState = HTTP_IDLE;
      lastA9GActivity = millis();
    }
  } else if (currentNetMode == NET_WIFI) {
    // Super-Safe OFF-Only Watchdog for Wi-Fi Mode
    // Only runs if GPS stream is silent for more than 45 seconds
    if (millis() - lastA9GActivity > 45000) {
      static unsigned long lastA9GResponsivenessCheck = 0;
      if (now - lastA9GResponsivenessCheck > 15000) { // Throttle AT checks to once every 15 seconds
        lastA9GResponsivenessCheck = now;
        printPC("[SYSTEM] WiFi Watchdog: GPS stream silent for 45s. Checking if A9G is OFF...");
        
        bool responsive = false;
        for (int i = 0; i < 3; i++) {
          while (A9G.available() > 0) A9G.read(); // Flush garbage
          A9G.println("AT");
          unsigned long start = millis();
          while (millis() - start < 500) {
            if (A9G.available() > 0) {
              String r = A9G.readString();
              if (r.indexOf("OK") != -1 || r.indexOf("$GN") != -1 || r.indexOf("$GP") != -1 || 
                  r.indexOf("+GPSRD") != -1 || r.indexOf("CME ERROR") != -1 || r.indexOf("READY") != -1) {
                responsive = true;
                break;
              }
            }
          }
          if (responsive) break;
          delay(200);
        }
        
        if (responsive) {
          printPC("[SYSTEM] A9G is ON but GPS stream was silent. Restarting GPS receiver...");
          A9G.println("AT+GPS=1");
          delay(100);
          A9G.println("AT+GPSRD=2");
          lastA9GActivity = now; // Reset watchdog timer
        } else {
          printPC("[SYSTEM] A9G is OFF/Frozen. Initializing hardware recovery...");
          hardwarePowerOnA9G(true);
          lastA9GActivity = now;
        }
      }
    }
  }

  // 6. Network connectivity self-healing with fallback/switching
  if (currentNetMode == NET_WIFI) {
    if (WiFi.status() != WL_CONNECTED) {
      printPC("[SYSTEM] Wi-Fi connection lost! Falling back to GPRS...");
      WiFi.disconnect(true);
      currentNetMode = NET_GPRS;
      gprsConnected = initGPRS();
      lastA9GActivity = millis();
    }
  } else {
    // GPRS Mode
    if (!gprsConnected) {
      if (now - lastGprsAttempt > 30000) { // Retry GPRS every 30 seconds (non-blocking)
        lastGprsAttempt = now;
        gprsConnected = initGPRS();
        if (gprsConnected) {
          lastA9GActivity = millis();
        }
      }
    } else if (wifiSSID.length() > 0 && (now - lastWiFiScanTime > 300000)) {
      // Every 5 minutes, scan for the configured Wi-Fi network to switch back
      lastWiFiScanTime = now;
      printPC("[SYSTEM] Scanning for configured Wi-Fi SSID: " + wifiSSID + "...");
      int n = WiFi.scanNetworks();
      bool wifiDetected = false;
      for (int i = 0; i < n; i++) {
        if (WiFi.SSID(i) == wifiSSID) {
          wifiDetected = true;
          break;
        }
      }
      WiFi.scanDelete();
      
      if (wifiDetected) {
        printPC("[SYSTEM] Configured Wi-Fi network detected! Switching from GPRS to Wi-Fi...");
        // Deactivate GPRS context on A9G to save power
        A9G.println("AT+CGACT=0,1");
        delay(100);
        A9G.println("AT+CGATT=0");
        delay(2000); // Wait 2s for cellular detach completion to avoid UART serial congestion
        gprsConnected = false;
        httpState = HTTP_IDLE;
        
        // Flush serial buffer to remove GPRS detach response garbage
        while (A9G.available() > 0) A9G.read();
        
        WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str());
        unsigned long startAttempt = millis();
        bool connected = false;
        while (millis() - startAttempt < 10000) {
          if (WiFi.status() == WL_CONNECTED) {
            connected = true;
            break;
          }
          delay(500);
        }
        if (connected) {
          currentNetMode = NET_WIFI;
          printPC("[SYSTEM] Connected to Wi-Fi successfully! Disabling A9G GPRS.");
          // Assure A9G is powered ON and responsive on switchback
          printPC("[SYSTEM] Switchback to WiFi: Assuring A9G is ON...");
          hardwarePowerOnA9G(false); // If A9G is OFF, boots it ON and restores GPS stream
          lastA9GActivity = millis();
        } else {
          printPC("[SYSTEM] Wi-Fi connection failed. Retaining GPRS.");
          WiFi.disconnect(true);
          gprsConnected = initGPRS();
        }
      }
    }
  }

  // 7. Non-blocking polling and telemetry scheduler (Runs in Wi-Fi mode OR active GPRS mode)
  if (currentNetMode == NET_WIFI || gprsConnected) {
    // Determine if we have a robust GPS fix (at least 3 satellites required for accuracy)
    bool hasGpsFix = (gps.location.isValid() && gps.satellites.value() >= 3);

    // Apply accuracy filter (HDOP check)
    if (hasGpsFix && gps.hdop.isValid() && gps.hdop.hdop() > 5.0) {
      hasGpsFix = false;
    }

    // Query LBS location if we don't have a valid GPS fix (every 30 seconds - GPRS mode only)
    if (currentNetMode == NET_GPRS && !hasGpsFix && (now - lastLbsQuery > 30000)) {
      lastLbsQuery = now;
      A9G.println("AT+LOCATION=1");
      printPC("[GPS] Requesting Cell-Tower LBS location...");
    }

    // Query Wi-Fi Geolocation if we don't have a valid GPS fix and no custom Wi-Fi location is set (every 30 seconds - Wi-Fi mode only)
    if (currentNetMode == NET_WIFI && !hasGpsFix && !isWifiLocation && (now - lastWifiLocQuery > 30000)) {
      lastWifiLocQuery = now;
      queryWiFiLocation();
    }

    // Query battery level via local ESP32 ADC (Pin 34) every 10 seconds for live updates
    // Runs immediately on startup/connection, then every 10 seconds
    static bool firstBatteryCheckDone = false;
    if (!firstBatteryCheckDone || (now - lastBatteryCheck > 10000)) {
      lastBatteryCheck = now;
      firstBatteryCheckDone = true;
      a9gBatteryLevel = readLocalBattery();
    }

    // Query GSM signal strength (CSQ) every 30 seconds (runs in both Wi-Fi and GPRS modes)
    static unsigned long lastCsqCheck = 0;
    if (now - lastCsqCheck > 30000) {
      lastCsqCheck = now;
      A9G.println("AT+CSQ");
    }

    // Poll Firebase commands (every 8 seconds for Wi-Fi / fallback GPRS)
    if (now - lastCommandPoll > 8000) {
      lastCommandPoll = now;
      if (currentNetMode == NET_WIFI) {
        performWiFiGetCommands();
      } else if (!mqttActive && httpState == HTTP_IDLE) { // Only poll HTTP when MQTT is inactive
        httpState = HTTP_GET_PENDING;
        httpStartTime = now;
        A9G.println("AT+GPSRD=0"); // Disable NMEA stream during HTTP transmission to prevent data corruption
        delay(50);
        A9G.println("AT+HTTPGET=\"" + TS_URL + "?api_key=" + tsGetCommandsKey + "\"");
        printPC("[HTTP] Sent GET request for commands.json");
      }
    }

    // Smart Conditional Telemetry Upload: Send data on event change or 60s heartbeat
    double currentLat = gps.location.lat();
    double currentLon = gps.location.lng();

    bool currentTamper = (digitalRead(reedPin) == HIGH);
    bool tamperChanged = (currentTamper != lastUploadedTamper);
    bool locationMoved = false;

    if (currentNetMode == NET_WIFI) {
      if (isWifiLocation) {
        if (lastUploadedLat != 0.0 && lastUploadedLon != 0.0) {
          double distanceMoved = TinyGPSPlus::distanceBetween(wifiLat, wifiLon, lastUploadedLat, lastUploadedLon);
          if (distanceMoved >= 25.0) {
            locationMoved = true;
          }
        } else {
          locationMoved = true;
        }
      } else if (hasGpsFix) {
        if (lastUploadedLat != 0.0 && lastUploadedLon != 0.0) {
          double distanceMoved = TinyGPSPlus::distanceBetween(currentLat, currentLon, lastUploadedLat, lastUploadedLon);
          double speedKmH = gps.speed.isValid() ? gps.speed.kmph() : 0.0;
          if (distanceMoved >= 6.0 && speedKmH >= 1.8) {
            locationMoved = true;
          }
        } else {
          locationMoved = true;
        }

        // Auto-Learn Wi-Fi fingerprints when GPS has a solid lock (Wi-Fi mode only)
        static unsigned long lastWifiLearnTime = 0;
        if (now - lastWifiLearnTime > 300000) { // Every 5 minutes
          lastWifiLearnTime = now;
          learnWiFiLocation(currentLat, currentLon);
        }
      }
    } else {
      isWifiLocation = false; // Detach Wi-Fi location once disconnected
      if (hasGpsFix) {
        if (lastUploadedLat != 0.0 && lastUploadedLon != 0.0) {
          double distanceMoved = TinyGPSPlus::distanceBetween(currentLat, currentLon, lastUploadedLat, lastUploadedLon);
          double speedKmH = gps.speed.isValid() ? gps.speed.kmph() : 0.0;
          if (distanceMoved >= 6.0 && speedKmH >= 1.8) {
            locationMoved = true;
          }
        } else {
          locationMoved = true;
        }
      }
    }

    bool shouldUpload = tamperChanged || locationMoved || (now - lastTelemetryUpload > (currentNetMode == NET_WIFI ? 2000 : 60000));

    if (shouldUpload) {
      String lat = "0.0";
      String lon = "0.0";
      bool lbsFlag = false;

      if (currentNetMode == NET_WIFI) {
        if (isWifiLocation) {
          if (locationMoved || lastUploadedLat == 0.0) {
            lastUploadedLat = wifiLat;
            lastUploadedLon = wifiLon;
            lastCoordinateUpdateTime = now;
          } else {
            wifiLat = lastUploadedLat;
            wifiLon = lastUploadedLon;
          }
          lat = String(wifiLat, 6);
          lon = String(wifiLon, 6);
          lbsFlag = false;
        } else if (hasGpsFix) {
          if (locationMoved || lastUploadedLat == 0.0) {
            lastUploadedLat = currentLat;
            lastUploadedLon = currentLon;
            lastCoordinateUpdateTime = now;
          } else {
            currentLat = lastUploadedLat;
            currentLon = lastUploadedLon;
          }
          lat = String(currentLat, 6);
          lon = String(currentLon, 6);
          lbsFlag = false;
        }
      } else {
        if (hasGpsFix) {
          if (locationMoved || lastUploadedLat == 0.0) {
            lastUploadedLat = currentLat;
            lastUploadedLon = currentLon;
            lastCoordinateUpdateTime = now;
          } else {
            currentLat = lastUploadedLat;
            currentLon = lastUploadedLon;
          }
          lat = String(currentLat, 6);
          lon = String(currentLon, 6);
          lbsFlag = false;
        } else if (isLbsLocation) {
          lat = String(lbsLat, 6);
          lon = String(lbsLon, 6);
          lbsFlag = true;
        } else if (lastUploadedLat != 0.0) {
          lat = String(lastUploadedLat, 6);
          lon = String(lastUploadedLon, 6);
        }
      }

      int rssi = (currentNetMode == NET_WIFI) ? WiFi.RSSI() : 0;
      double wifiDist = (rssi < 0) ? pow(10.0, (-45.0 - (double)rssi) / 28.0) : -1.0;

      String payload = "{\"lat\":" + lat + ",\"lon\":" + lon + ",\"battery\":" + String(a9gBatteryLevel) + ",\"tamper\":" + (currentTamper ? "true" : "false") + ",\"lbs\":" + (lbsFlag ? "true" : "false") + ",\"wifi_dist\":" + (wifiDist > 0 ? String(wifiDist, 1) : "-1") + ",\"gps_locked\":" + (hasGpsFix ? "true" : "false") + ",\"csq\":" + String(csqVal) + "}";

      if (currentNetMode == NET_WIFI) {
        lastTelemetryUpload = now;
        lastUploadedTamper = currentTamper;
        httpPatch("bag_data.json", payload);
      } else if (httpState == HTTP_IDLE) {
        lastTelemetryUpload = now;
        httpState = HTTP_POST_PENDING;
        httpStartTime = now;
        lastUploadedTamper = currentTamper;
        httpPatch("bag_data.json", payload);
      }
    }
  }

  // 8. Update physical alerts locally (real-time offline/online evaluation)
  updateLocalAlerts();

  // Self-healing from GPRS dropouts
  if (currentNetMode == NET_GPRS && httpFailCount >= 3) {
    printPC("[HTTP] Too many failures. Re-initializing GPRS...");
    gprsConnected = false;
    httpFailCount = 0;
  }
}

void handleA9GLine(String line) {
  printPC("[A9G-OUT] " + line);
  lastA9GActivity = millis(); // Refresh activity watchdog

  // Parse unsolicited incoming MQTT command push notifications
  if (line.indexOf("+MQTTPUBLISH:") != -1 || line.indexOf("+MQTTPUB:") != -1) {
    printPC("[MQTT] Unsolicited publish received!");
    int braceIndex = line.indexOf('{');
    if (braceIndex != -1) {
      String json = line.substring(braceIndex);
      // Clean trailing quotes if present
      json.trim();
      if (json.endsWith("\"")) {
        json = json.substring(0, json.length() - 1);
      }
      printPC("[MQTT] Forwarding payload to parser: " + json);
      processSecureFirstLogic(json);
    }
  }

  if (gps.location.isValid()) {
    isLbsLocation = false;
  }

  // Detect A9G sudden reboot or bootloader messages
  if (line.indexOf("READY") != -1 || line.indexOf("Init...") != -1 || line.indexOf("Ai_Thinker") != -1 || line.indexOf("V02.02.") != -1) {
    printPC("[SYSTEM] A9G sudden reboot detected! Re-initializing GPS stream...");
    gprsConnected = false;
    mqttActive = false; // Reset MQTT status on module reboot
    httpState = HTTP_IDLE;
    
    if (currentNetMode == NET_WIFI) {
      // Allow A9G to boot up, then re-enable GPS stream
      delay(8000);
      A9G.println("AT+GPS=1");
      delay(100);
      A9G.println("AT+GPSRD=2");
      printPC("[SYSTEM] GPS stream re-enabled after sudden reboot.");
    }
    return;
  }

  // Parse location fallback from cell-tower LBS
  if (line.indexOf("+LOCATION:") != -1) {
    int commaIndex = line.indexOf(',');
    int colonIndex = line.indexOf(':');
    if (commaIndex != -1 && colonIndex != -1) {
      String latStr = line.substring(colonIndex + 1, commaIndex);
      String lonStr = line.substring(commaIndex + 1);
      latStr.trim();
      lonStr.trim();
      double tempLat = latStr.toDouble();
      double tempLon = lonStr.toDouble();
      if (tempLat != 0.0 && tempLon != 0.0) {
        lbsLat = tempLat;
        lbsLon = tempLon;
        isLbsLocation = true;
        printPC("[GPS] LBS Fallback updated: " + String(lbsLat, 6) + ", " + String(lbsLon, 6));
      }
    }
  }

  // Parse battery level from A9G PMU (+CBC: 0,85)
  if (line.indexOf("+CBC:") != -1) {
    int commaIndex = line.indexOf(',');
    if (commaIndex != -1) {
      String batStr = line.substring(commaIndex + 1);
      batStr.trim();
      int tempBat = batStr.toInt();
      if (tempBat >= 0 && tempBat <= 100) {
        a9gBatteryLevel = tempBat;
        printPC("[SYSTEM] Battery status updated (CBC): " + String(a9gBatteryLevel) + "%");
      }
    }
  }

  // Parse battery level from unsolicited +CIEV charging reports (+CIEV: "Charging",80%)
  if (line.indexOf("+CIEV: \"Charging\",") != -1) {
    int commaIndex = line.indexOf(',');
    if (commaIndex != -1) {
      String batStr = line.substring(commaIndex + 1);
      batStr.trim();
      if (batStr.endsWith("%")) {
        batStr = batStr.substring(0, batStr.length() - 1);
      }
      int tempBat = batStr.toInt();
      if (tempBat >= 0 && tempBat <= 100) {
        a9gBatteryLevel = tempBat;
        printPC("[SYSTEM] Battery status updated (CIEV): " + String(a9gBatteryLevel) + "%");
      }
    }
  }

  // Parse GSM Signal Strength (CSQ) from +CSQ: 10,99
  if (line.indexOf("+CSQ:") != -1) {
    int colonIndex = line.indexOf(':');
    int commaIndex = line.indexOf(',');
    if (colonIndex != -1 && commaIndex != -1) {
      String csqStr = line.substring(colonIndex + 1, commaIndex);
      csqStr.trim();
      int tempCsq = csqStr.toInt();
      if (tempCsq >= 0 && tempCsq <= 31) {
        csqVal = tempCsq;
        printPC("[SYSTEM] CSQ Signal Strength updated: " + String(csqVal));
      }
    }
  }

  // Parse HTTP responses and clear lock state
  if (line.startsWith("{") && line.indexOf("\"alarm\"") != -1) {
    processSecureFirstLogic(line);
  }

  if (line.indexOf("+CMT:") != -1) {
    incomingSmsFlag = true;
    int start = line.indexOf('\"');
    if (start != -1) {
      int end = line.indexOf('\"', start + 1);
      if (end != -1) lastCallerPhone = line.substring(start + 1, end);
    }
    return;
  }

  if (incomingSmsFlag) {
    incomingSmsFlag = false;
    processIncomingSMS(line, lastCallerPhone);
    lastCallerPhone = "";
  }

  if (line.indexOf("+CUSD:") != -1) {
    int start = line.indexOf('\"');
    if (start != -1) {
      int end = line.indexOf('\"', start + 1);
      if (end != -1) {
        String balHex = line.substring(start + 1, end);
        String bal = decodeHex(balHex);
        printPC("[BALANCE] " + bal);
        if (lastSmsSender != "") { sendSMS(lastSmsSender, "SIM Balance:\n" + bal); lastSmsSender = ""; }
        String cleanBal = sanitizeJsonString(bal);
        httpPatch("bag_data.json", "{\"balance\":\"" + cleanBal + "\"}");
      }
    }
    // Restore character set to GSM
    A9G.println("AT+CSCS=\"GSM\"");
    ussdPending = false;
  }

  // Clear HTTP busy flag on response or error
  if (line.startsWith("{") || line == "ERROR" || line.indexOf("failure") != -1 || line.indexOf("CLOSED") != -1) {
    if (httpState != HTTP_IDLE) {
      httpState = HTTP_IDLE;
      printPC("[HTTP] Request completed or errored, clearing busy flag.");
      A9G.println("AT+GPSRD=2"); // Restore NMEA stream when HTTP finishes
    }
  }
}

bool parseJsonBool(String json, String key) {
  int idx = json.indexOf("\"" + key + "\"");
  if (idx == -1) return false;
  int colon = json.indexOf(":", idx);
  if (colon == -1) return false;
  int nextComma = json.indexOf(",", colon);
  int nextBrace = json.indexOf("}", colon);
  int end = (nextComma != -1 && (nextBrace == -1 || nextComma < nextBrace)) ? nextComma : nextBrace;
  if (end == -1) return false;
  String val = json.substring(colon + 1, end);
  val.replace("\"", "");
  val.replace(" ", "");
  val.trim();
  return (val.equalsIgnoreCase("true") || val.equals("1"));
}

void processSecureFirstLogic(String payload) {
  // Parse owner_phone if present in commands payload
  int keyIdx = payload.indexOf("\"owner_phone\"");
  if (keyIdx != -1) {
    int colonIdx = payload.indexOf(':', keyIdx);
    if (colonIdx != -1) {
      int quoteStart = payload.indexOf('\"', colonIdx);
      if (quoteStart != -1) {
        int quoteEnd = payload.indexOf('\"', quoteStart + 1);
        if (quoteEnd != -1) {
          String newPhone = payload.substring(quoteStart + 1, quoteEnd);
          newPhone.trim();
          if (newPhone.startsWith("0")) {
            newPhone = "+94" + newPhone.substring(1);
          }
          if (newPhone.length() > 0 && newPhone != userPhone) {
            userPhone = newPhone;
            prefs.begin("system-config", false);
            prefs.putString("userPhone", userPhone);
            prefs.end();
            printPC("[SYSTEM] Sync userPhone: " + userPhone);
          }
        }
      }
    }
  }

  bool appBuzzerNew = parseJsonBool(payload, "b");
  bool appLedNew = parseJsonBool(payload, "l");
  bool rcOptionNew = parseJsonBool(payload, "rc");

  appAlarmActive = parseJsonBool(payload, "alarm");
  appLightActive = appLedNew; // Map LED option setting to light state
  riskZoneActive = (payload.indexOf("PROXIMITY VIOLATION") != -1 || payload.indexOf("exceeded") != -1 || payload.indexOf("beyond") != -1);

  if (appBuzzerNew != appBuzzer || appLedNew != appLed || rcOptionNew != rcOption) {
    appBuzzer = appBuzzerNew;
    appLed = appLedNew;
    rcOption = rcOptionNew;
    prefs.begin("system-config", false);
    prefs.putBool("appBuzzer", appBuzzer);
    prefs.putBool("appLed", appLed);
    prefs.putBool("rc", rcOption);
    prefs.end();
    printPC("[SYSTEM] Preferences updated and saved.");
  }

  // Parse wifi_lat and wifi_lon if present in commands payload (sent by the user app)
  double wifiLatNew = parseJsonDouble(payload, "wifi_lat");
  double wifiLonNew = parseJsonDouble(payload, "wifi_lon");
  if (wifiLatNew != 0.0 && wifiLonNew != 0.0 && (wifiLatNew != wifiLat || wifiLonNew != wifiLon)) {
    wifiLat = wifiLatNew;
    wifiLon = wifiLonNew;
    isWifiLocation = true;
    prefs.begin("system-config", false);
    prefs.putDouble("wifiLat", wifiLat);
    prefs.putDouble("wifiLon", wifiLon);
    prefs.end();
    printPC("[SYSTEM] Saved new Wi-Fi coordinates: " + String(wifiLat, 6) + "," + String(wifiLon, 6));
  }

  // Run immediate alert update
  updateLocalAlerts();

  if (parseJsonBool(payload, "send_sms_location")) {
    // Reset Firebase command first to prevent infinite loops
    httpPatch("commands.json", "{\"send_sms_location\":false}");
    delay(200);
    
    if (userPhone != "") {
      sendLocationSMS(userPhone);
    }
  }
}

void updateLocalAlerts() {
  bool magnetAway = (digitalRead(reedPin) == HIGH);
  
  // The alarm trigger is active if:
  // 1. Physical magnet is away (tampered)
  // 2. Risk zone proximity breach has occurred
  // 3. User clicked force alarm in the app
  bool alarmTrigger = (magnetAway || riskZoneActive || appAlarmActive);

  // The buzzer pin 13 outputs HIGH if:
  // - appBuzzer option is enabled AND alarm is triggered
  // The LED pin 14 outputs HIGH if:
  // - appLed option is enabled AND alarm is triggered
  bool shouldSoundBuzzer = (appBuzzer && alarmTrigger);
  bool shouldLightLed = (appLed && alarmTrigger);

  // Non-blocking double-beep buzzer pattern matching startup tone (200ms beeps, 100ms gap, 1000ms total cycle)
  if (shouldSoundBuzzer) {
    unsigned long cycleTime = millis() % 1000; // 1000ms (1 second) total cycle time
    if (cycleTime < 200) {
      digitalWrite(buzzerPin, HIGH); // First beep for 200ms
    } else if (cycleTime >= 200 && cycleTime < 300) {
      digitalWrite(buzzerPin, LOW);  // Short silence for 100ms
    } else if (cycleTime >= 300 && cycleTime < 500) {
      digitalWrite(buzzerPin, HIGH); // Second beep for 200ms
    } else {
      digitalWrite(buzzerPin, LOW);  // Silence gap for 500ms
    }
  } else {
    digitalWrite(buzzerPin, LOW);
  }

  digitalWrite(ledPin, shouldLightLed ? HIGH : LOW);

  // Periodic debugger to print internal states to PC console monitor
  static unsigned long lastAlertDebug = 0;
  if (millis() - lastAlertDebug > 3000) {
    lastAlertDebug = millis();
    printPC("[ALERT-DEBUG] magnetAway (Tamper): " + String(magnetAway ? "YES" : "NO") + 
            " | appBuzzer (Setting): " + String(appBuzzer ? "ON" : "OFF") + 
            " | appLed (Setting): " + String(appLed ? "ON" : "OFF") + 
            " | appAlarmActive (Forced): " + String(appAlarmActive ? "YES" : "NO") + 
            " | Pin 14 (LED) output: " + String(digitalRead(ledPin) == HIGH ? "HIGH" : "LOW") +
            " | Pin 13 (Buzzer) output: " + String(digitalRead(buzzerPin) == HIGH ? "HIGH" : "LOW"));
  }
}

double parseJsonDouble(String json, String key) {
  int index = json.indexOf("\"" + key + "\"");
  if (index == -1) return 0.0;
  int colonIndex = json.indexOf(":", index);
  if (colonIndex == -1) return 0.0;
  int commaIndex = json.indexOf(",", colonIndex);
  int braceIndex = json.indexOf("}", colonIndex);
  int endIndex = (commaIndex != -1 && commaIndex < braceIndex) ? commaIndex : braceIndex;
  String val = json.substring(colonIndex + 1, endIndex);
  val.trim();
  return val.toDouble();
}

String parseJsonString(String json, String key) {
  int index = json.indexOf("\"" + key + "\"");
  if (index == -1) return "";
  int colonIndex = json.indexOf(":", index);
  if (colonIndex == -1) return "";
  int commaIndex = json.indexOf(",", colonIndex);
  int braceIndex = json.indexOf("}", colonIndex);
  int endIndex = (commaIndex != -1 && commaIndex < braceIndex) ? commaIndex : braceIndex;
  String val = json.substring(colonIndex + 1, endIndex);
  val.trim();
  // Remove surrounding quotes if present
  if (val.startsWith("\"") && val.endsWith("\"")) {
    val = val.substring(1, val.length() - 1);
  }
  return val;
}

String cleanMac(String mac) {
  String clean = "";
  for (int i = 0; i < mac.length(); i++) {
    char c = mac.charAt(i);
    if (c != ':') {
      clean += c;
    }
  }
  clean.toLowerCase();
  return clean;
}

void learnWiFiLocation(double lat, double lon) {
  if (lat == 0.0 || lon == 0.0) return;
  
  printPC("[GPS-WIFI] Learning nearby Wi-Fi AP for Firebase mapping...");
  int n = WiFi.scanNetworks();
  if (n <= 0) return;

  // Select the strongest access point (index 0)
  String rawMac = WiFi.BSSIDstr(0);
  String ssid = WiFi.SSID(0);
  WiFi.scanDelete(); // Delete scan results from memory
  
  // Exclude common mobile hotspots to keep the database clean
  String lowerSSID = ssid;
  lowerSSID.toLowerCase();
  if (lowerSSID.indexOf("iphone") != -1 || lowerSSID.indexOf("android") != -1 || 
      lowerSSID.indexOf("hotspot") != -1 || lowerSSID.indexOf("galaxy") != -1 ||
      rawMac == "" || rawMac == "00:00:00:00:00:00") {
    printPC("[GPS-WIFI] Strongest AP is a mobile hotspot or invalid (" + ssid + "). Skipping learning.");
    return;
  }

  String cleanMacAddress = cleanMac(rawMac);
  
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  
  // Check if MAC is already registered in Firebase wifi_db to prevent redundant writes
  String url = "https://microplux-anti-theft-app-2026-default-rtdb.firebaseio.com/wifi_db/" + cleanMacAddress + ".json";
  http.begin(client, url);
  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    String response = http.getString();
    if (response != "null") {
      printPC("[GPS-WIFI] MAC " + cleanMacAddress + " is already mapped. Skipping.");
      http.end();
      return;
    }
  }
  http.end();

  // Perform PUT write request to Firebase RTDB
  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");
  String payload = "{\"lat\":" + String(lat, 6) + ",\"lon\":" + String(lon, 6) + "}";
  printPC("[GPS-WIFI] Mapping MAC " + cleanMacAddress + " (" + ssid + ") -> Firebase...");
  int putCode = http.PUT(payload);
  if (putCode == HTTP_CODE_OK) {
    printPC("[GPS-WIFI] Successfully mapped MAC to Firebase: " + cleanMacAddress);
  } else {
    printPC("[GPS-WIFI] Firebase PUT mapping failed: " + String(putCode));
  }
  http.end();
}

void queryWiFiLocation() {
  printPC("[GPS-WIFI] Scanning Wi-Fi networks for Geolocation fallback...");
  int n = WiFi.scanNetworks();
  if (n <= 0) {
    printPC("[GPS-WIFI] No Wi-Fi networks found. Geolocation scan aborted.");
    return;
  }

  // Select strongest AP (index 0)
  String rawMac = WiFi.BSSIDstr(0);
  String ssid = WiFi.SSID(0);
  int rssiVal = WiFi.RSSI(0);

  printPC("[GPS-WIFI] Strongest AP: " + ssid + " [" + rawMac + "] (" + String(rssiVal) + " dBm)");

  // Clean MAC to get safe alphanumeric key
  String cleanMacAddress = cleanMac(rawMac);
  if (cleanMacAddress == "" || cleanMacAddress == "000000000000") {
    printPC("[GPS-WIFI] Invalid MAC address. Geolocation aborted.");
    WiFi.scanDelete();
    return;
  }

  // Step 1: Query Firebase RTDB Cache first (100% FREE, Unlimited)
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  String firebaseResult = "null";
  String url = "https://microplux-anti-theft-app-2026-default-rtdb.firebaseio.com/wifi_db/" + cleanMacAddress + ".json";
  
  http.begin(client, url);
  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    firebaseResult = http.getString();
  }
  http.end();

  // Step 2: Evaluate Firebase lookup
  if (firebaseResult != "null" && firebaseResult != "") {
    double latVal = parseJsonDouble(firebaseResult, "lat");
    double lonVal = parseJsonDouble(firebaseResult, "lon");
    
    if (latVal != 0.0 && lonVal != 0.0) {
      wifiLat = latVal;
      wifiLon = lonVal;
      isWifiLocation = true;
      printPC("[GPS-WIFI] Cache HIT (Firebase): Lat=" + String(wifiLat, 6) + ", Lon=" + String(wifiLon, 6));
      WiFi.scanDelete();
      return;
    }
  }

  printPC("[GPS-WIFI] Cache MISS. Querying Unwired Labs API as backup...");

  // Step 3: Unwired Labs Fallback (Daily Free quota backup)
  if (unwiredToken == "YOUR_UNWIRED_TOKEN" || unwiredToken == "") {
    printPC("[GPS-WIFI] Unwired Labs Geolocation Token is not configured. Skipping backup query.");
    WiFi.scanDelete();
    return;
  }

  // Format payload for Unwired Labs API
  String json = "{\"token\":\"" + unwiredToken + "\",\"wifi\":[";
  int addedCount = 0;
  for (int i = 0; i < n; i++) {
    String mac = WiFi.BSSIDstr(i);
    int rssi = WiFi.RSSI(i);
    if (mac != "" && mac != "00:00:00:00:00:00") {
      if (addedCount > 0) json += ",";
      json += "{\"bssid\":\"" + mac + "\",\"signal\":" + String(rssi) + "}";
      addedCount++;
    }
  }
  json += "],\"address\":0}";
  WiFi.scanDelete(); // Delete scan results to free heap memory

  String unwiredUrl = "https://us1.unwiredlabs.com/v2/process.php";
  http.begin(client, unwiredUrl);
  http.addHeader("Content-Type", "application/json");

  int postCode = http.POST(json);
  if (postCode == HTTP_CODE_OK) {
    String response = http.getString();
    printPC("[GPS-WIFI] Unwired Labs Response: " + response);
    
    if (response.indexOf("\"status\":\"ok\"") != -1 || response.indexOf("\"status\": \"ok\"") != -1) {
      double latVal = parseJsonDouble(response, "lat");
      double lonVal = parseJsonDouble(response, "lon");
      
      if (latVal != 0.0 && lonVal != 0.0) {
        wifiLat = latVal;
        wifiLon = lonVal;
        isWifiLocation = true;
        printPC("[GPS-WIFI] Backup Geolocation successful: Lat=" + String(wifiLat, 6) + ", Lon=" + String(wifiLon, 6));

        // Step 4: Cache resolved coordinates into Firebase to make future lookups free
        http.end();
        http.begin(client, url);
        http.addHeader("Content-Type", "application/json");
        String cachePayload = "{\"lat\":" + String(wifiLat, 6) + ",\"lon\":" + String(wifiLon, 6) + "}";
        http.PUT(cachePayload);
        printPC("[GPS-WIFI] Cached new coordinates in Firebase wifi_db for future free lookups.");
      } else {
        printPC("[GPS-WIFI] Geolocation failed: Parsed coordinates are zero.");
      }
    } else {
      printPC("[GPS-WIFI] Geolocation failed. API Status not OK.");
    }
  } else {
    printPC("[GPS-WIFI] Backup API call failed. HTTP Code: " + String(postCode));
  }
  http.end();
}

void performWiFiGetCommands() {
  WiFiClientSecure client;
  client.setInsecure(); // Bypass certificate validation for simplicity
  HTTPClient http;
  String url = "https://microplux-anti-theft-app-2026-default-rtdb.firebaseio.com/commands.json";
  http.begin(client, url);
  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    printPC("[HTTP-WIFI] GET commands: " + payload);
    processSecureFirstLogic(payload);
  } else {
    printPC("[HTTP-WIFI] GET commands failed. Error: " + http.errorToString(httpCode));
  }
  http.end();
}

void processIncomingSMS(String msg, String sender) {
  msg.trim();
  
  // Auto-save owner number from incoming SMS if userPhone is not set
  if (userPhone == "" && sender.length() > 0) {
    userPhone = sender;
    prefs.begin("system-config", false);
    prefs.putString("userPhone", userPhone);
    prefs.end();
    printPC("[SMS] Auto-saved owner phone: " + userPhone);
  }

  if (msg.startsWith("SIM:") || msg.startsWith("sim:")) {
    userPhone = msg.substring(msg.indexOf(':') + 1); userPhone.trim();
    prefs.begin("system-config", false); prefs.putString("userPhone", userPhone); prefs.end();
    sendSMS(sender, "SIM updated: " + userPhone);
  }
  else if (msg.equalsIgnoreCase("STATUS")) {
    String statusText = "Microflux status:\nGPS: " + (gps.location.isValid() ? (String(gps.location.lat(), 6) + "," + String(gps.location.lng(), 6)) : "No Fix") +
                        "\nMaps: " + (gps.location.isValid() ? ("https://www.google.com/maps?q=" + String(gps.location.lat(), 6) + "," + String(gps.location.lng(), 6)) : "No Fix") +
                        "\nBattery: " + String(a9gBatteryLevel) + "%" +
                        "\nTamper: " + (digitalRead(reedPin) == HIGH ? "ALERT" : "SECURE") +
                        "\nGPRS: " + (gprsConnected ? "Online" : "Offline") +
                        "\nRingcut: " + (rcOption ? "ARMED" : "OFF");
    sendSMS(sender, statusText);
    lastSmsSender = sender;
    triggerUSSD();
  }
  else if (msg.equalsIgnoreCase("BALANCE") || msg.equalsIgnoreCase("bal")) {
    lastSmsSender = sender;
    triggerUSSD();
  }
  else if (msg.startsWith("APN:") || msg.startsWith("apn:")) {
    apn = msg.substring(msg.indexOf(':') + 1); apn.trim();
    prefs.begin("system-config", false); prefs.putString("apn", apn); prefs.end();
    sendSMS(sender, "APN updated: " + apn);
    gprsConnected = initGPRS();
  }
  else if (msg.startsWith("WIFI:") || msg.startsWith("wifi:")) {
    String wifiCmd = msg.substring(msg.indexOf(':') + 1);
    wifiCmd.trim();
    
    if (wifiCmd.equalsIgnoreCase("RESET") || wifiCmd.equalsIgnoreCase("CLEAR")) {
      prefs.begin("system-config", false);
      prefs.putString("wifiSSID", "");
      prefs.putString("wifiPassword", "");
      prefs.end();
      sendSMS(sender, "Wi-Fi settings cleared. Restarting into GPRS mode...");
      delay(2000);
      ESP.restart();
    } else {
      int commaIdx = msg.indexOf(',');
      if (commaIdx != -1) {
        String newSsid = msg.substring(msg.indexOf(':') + 1, commaIdx);
        newSsid.trim();
        String newPass = msg.substring(commaIdx + 1);
        newPass.trim();
        
        prefs.begin("system-config", false);
        prefs.putString("wifiSSID", newSsid);
        prefs.putString("wifiPassword", newPass);
        prefs.end();
        
        sendSMS(sender, "Wi-Fi updated: " + newSsid + ". Restarting...");
        delay(2000);
        ESP.restart();
      } else {
        sendSMS(sender, "Invalid format! Use: WIFI:SSID,Password");
      }
    }
  }
}

// Conversion and decoding helper functions
String textToHex(String text) {
  String hex = "";
  for (int i = 0; i < text.length(); i++) {
    char c = text.charAt(i);
    char buf[3];
    sprintf(buf, "%02X", c);
    hex += buf;
  }
  return hex;
}

bool isHexString(String s) {
  if (s.length() == 0) return false;
  for (int i = 0; i < s.length(); i++) {
    char c = s.charAt(i);
    if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))) {
      return false;
    }
  }
  return true;
}

String decodeHex(String hexStr) {
  if (!isHexString(hexStr)) return hexStr;
  
  bool isUCS2 = false;
  if (hexStr.length() % 4 == 0) {
    int ucs2Count = 0;
    int totalBlocks = hexStr.length() / 4;
    for (int i = 0; i < hexStr.length(); i += 4) {
      String highByte = hexStr.substring(i, i + 2);
      if (highByte == "00" || highByte == "0D" || highByte == "0b" || highByte == "0B") {
        ucs2Count++;
      }
    }
    if (ucs2Count >= totalBlocks / 2 || totalBlocks == 0) {
      isUCS2 = true;
    }
  }
  
  String decoded = "";
  if (isUCS2) {
    for (int i = 0; i < hexStr.length(); i += 4) {
      String charHex = hexStr.substring(i, i + 4);
      long code = strtol(charHex.c_str(), NULL, 16);
      if (code < 128) {
        decoded += (char)code;
      } else {
        if (code < 0x800) {
          decoded += (char)(0xC0 | (code >> 6));
          decoded += (char)(0x80 | (code & 0x3F));
        } else {
          decoded += (char)(0xE0 | (code >> 12));
          decoded += (char)(0x80 | ((code >> 6) & 0x3F));
          decoded += (char)(0x80 | (code & 0x3F));
        }
      }
    }
  } else {
    for (int i = 0; i < hexStr.length(); i += 2) {
      String charHex = hexStr.substring(i, i + 2);
      long code = strtol(charHex.c_str(), NULL, 16);
      decoded += (char)code;
    }
  }
  return decoded;
}

void triggerUSSD() {
  // USSD Balance check is disabled to prevent high power bursts on battery
  printPC("[USSD] USSD balance query is disabled.");
}

String escapeQuotes(String input) {
  String output = "";
  for (int i = 0; i < input.length(); i++) {
    char c = input.charAt(i);
    if (c == '"') {
      output += "\\\"";
    } else {
      output += c;
    }
  }
  return output;
}

String urlEncode(String str) {
  String encodedString = "";
  char c;
  char code0;
  char code1;
  for (int i = 0; i < str.length(); i++) {
    c = str.charAt(i);
    if (isalnum(c)) {
      encodedString += c;
    } else if (c == ' ') {
      encodedString += '+';
    } else {
      code1 = (c & 0xf) + '0';
      if ((c & 0xf) > 9) {
        code1 = (c & 0xf) - 10 + 'A';
      }
      c = (c >> 4) & 0xf;
      code0 = c + '0';
      if (c > 9) {
        code0 = c - 10 + 'A';
      }
      encodedString += '%';
      encodedString += code0;
      encodedString += code1;
    }
  }
  return encodedString;
}

// Convert NMEA DDMM.MMMM or DDDMM.MMMM format to decimal degrees
double parseNmeaCoord(String term, String dir) {
  if (term.length() == 0 || dir.length() == 0) return 0.0;
  int dotIndex = term.indexOf('.');
  if (dotIndex == -1) return 0.0;
  
  int minStart = dotIndex - 2;
  if (minStart < 0) return 0.0;
  
  String degStr = term.substring(0, minStart);
  String minStr = term.substring(minStart);
  
  double degrees = degStr.toDouble();
  double minutes = minStr.toDouble();
  double decDegrees = degrees + (minutes / 60.0);
  
  if (dir == "S" || dir == "W") {
    decDegrees = -decDegrees;
  }
  return decDegrees;
}

String sanitizeJsonString(String s) {
  String clean = "";
  for (int i = 0; i < s.length(); i++) {
    char c = s.charAt(i);
    if (c == '\\') {
      continue; // Skip backslashes to prevent invalid escape sequences
    } else if (c == '\"') {
      clean += '\''; // Replace double quotes with single quotes
    } else if (c >= 32 && c <= 126) {
      clean += c; // Allow only printable ASCII characters
    }
  }
  return clean;
}

int readLocalBattery() {
  long sum = 0;
  for (int i = 0; i < 20; i++) {
    sum += analogRead(voltPin);
    delay(1);
  }
  float avgAdc = sum / 20.0;
  if (avgAdc < 100) return 0; // Pin floating or grounded
  
  // Convert ESP32 ADC reading to voltage.
  // Full-scale ADC range is 3.3V at 11dB attenuation.
  // Assumes a standard 1:1 voltage divider (100k/100k) on voltPin (GPIO 34).
  float batteryVolts = (avgAdc / 4095.0) * 3.3 * 2.0;
  int pct = (int)((batteryVolts - 3.3) / (4.2 - 3.3) * 100.0);
  if (pct > 100) pct = 100;
  if (pct < 0) pct = 0;
  
  printPC("[BATTERY-ADC] Raw ADC: " + String(avgAdc) + " | Volts: " + String(batteryVolts, 2) + "V | Level: " + String(pct) + "%");
  return pct;
}
