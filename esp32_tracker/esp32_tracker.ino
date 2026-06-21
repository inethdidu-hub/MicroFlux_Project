#include <Arduino.h>
#include <TinyGPS++.h>
#include <Preferences.h>       
#include "soc/soc.h"           
#include "soc/rtc_cntl_reg.h"  

// Deployed Render proxy URL (Testing plain HTTP)
const String FB_URL = "http://microflux-project.onrender.com/";
const int buzzerPin = 13, ledPin = 14, onboardLED = 2, reedPin = 26, voltPin = 34;

HardwareSerial A9G(2); 
TinyGPSPlus gps;
Preferences prefs;

String userPhone = "", apn = "mobitel", lastSmsSender = "", a9gBuffer = "", lastCallerPhone = "";
bool rcOption = false, gprsConnected = false, incomingSmsFlag = false, callPending = false;
bool ussdPending = false;
unsigned long lastSync = 0, ringTime = 0, ussdSentTime = 0;

// Forward Declarations to ensure clean compilation
bool sendCmd(String cmd, String expected, unsigned long timeoutMs);
bool initGPRS();
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

void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); 
  Serial.begin(115200);
  pinMode(buzzerPin, OUTPUT); pinMode(ledPin, OUTPUT); pinMode(onboardLED, OUTPUT); pinMode(reedPin, INPUT_PULLUP);

  prefs.begin("system-config", false);
  userPhone = prefs.getString("userPhone", "");
  rcOption = prefs.getBool("rc", false);
  apn = prefs.getString("apn", "mobitel");
  if (apn == "dialogbb") {
    apn = "mobitel";
    prefs.putString("apn", apn);
  }
  prefs.end();

  A9G.begin(115200, SERIAL_8N1, 16, 17); delay(3000);
  sendCmd("AT+GPS=1", "OK", 1000);
  sendCmd("AT+GPSRD=10", "OK", 1000);
  sendCmd("AT+CMGF=1", "OK", 1000);
  sendCmd("AT+CLIP=1", "OK", 1000);
  sendCmd("AT+CSCS=\"GSM\"", "OK", 1000);
  sendCmd("AT+CNMI=2,2,0,0,0", "OK", 1000);

  gprsConnected = initGPRS();
}

bool sendCmd(String cmd, String expected, unsigned long timeoutMs) {
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
          Serial.println("[A9G-RAW] " + response);
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

bool initGPRS() {
  Serial.println("[GPRS] Connecting to mobile internet...");
  sendCmd("AT", "OK", 1000);
  
  // Cellular diagnostics
  sendCmd("AT+CSQ", "OK", 1000);    // Read Signal Quality (ideal is 15-31, 99 is no signal)
  sendCmd("AT+CREG?", "OK", 1000);   // Network Registration status
  sendCmd("AT+CGREG?", "OK", 1000);  // GPRS Registration status
  sendCmd("AT+CGATT?", "OK", 1000);  // Check GPRS attachment status
  
  sendCmd("AT+CGACT=0,1", "OK", 2000); // Deactivate PDP to reset context
  sendCmd("AT+CGATT=1", "OK", 5000);
  sendCmd("AT+CGDCONT=1,\"IP\",\"" + apn + "\"", "OK", 3000);
  if (!sendCmd("AT+CGACT=1,1", "OK", 8000)) {
    Serial.println("[GPRS] PDP context activation failed!");
    return false;
  }
  
  // Set Google DNS to resolve "Dns,fail,try url" errors
  sendCmd("AT+CDNSCFG=\"8.8.8.8\",\"8.8.4.4\"", "OK", 2000);
  
  // Verify assigned IP address
  sendCmd("AT+CGPADDR=1", "OK", 2000);
  
  // Trigger initial balance check on boot
  triggerUSSD();
  return true;
}

void sendSMS(String phone, String text) {
  if (phone.length() == 0) return;
  A9G.printf("AT+CMGS=\"%s\"\r\n", phone.c_str());
  delay(500);
  A9G.print(text);
  A9G.write(0x1A);
  delay(500);
}

void sendLocationSMS(String phone) {
  String mapUrl = "Microflux Location: https://www.google.com/maps?q=" + String(gps.location.lat(), 6) + "," + String(gps.location.lng(), 6);
  sendSMS(phone, mapUrl);
}

void httpPatch(String path, String payload) {
  // Use direct HTTPPOST command with escaped quotes in payload
  String url = FB_URL + path + "?x-http-method-override=PATCH";
  String escapedPayload = escapeQuotes(payload);
  A9G.printf("AT+HTTPPOST=\"%s\",\"application/json\",\"%s\"\r\n", url.c_str(), escapedPayload.c_str());
  Serial.println("[HTTP] Sent PATCH payload: " + payload);
}

void loop() {
  while (A9G.available() > 0) {
    char c = A9G.read();
    gps.encode(c);
    if (c == '\n') {
      a9gBuffer.trim();
      if (a9gBuffer.length() > 0) handleA9GLine(a9gBuffer);
      a9gBuffer = "";
    } else if (c != '\r') a9gBuffer += c;
  }

  if (ussdPending && (millis() - ussdSentTime > 15000)) {
    Serial.println("[USSD] Timeout! Cancelling USSD session...");
    A9G.println("AT+CUSD=2");
    delay(100);
    A9G.println("AT+CSCS=\"GSM\"");
    ussdPending = false;
  }

  digitalWrite(onboardLED, gprsConnected ? digitalRead(reedPin) : ((millis() / 200) % 2 == 0));

  if (callPending && (millis() - ringTime > 2500)) {
    A9G.println("ATH"); delay(500);
    sendLocationSMS(lastCallerPhone != "" ? lastCallerPhone : userPhone);
    callPending = false; lastCallerPhone = "";
  }

  if (millis() - lastSync > 10000) {
    lastSync = millis();
    if (gprsConnected) {
      // Direct GET commands via HTTPGET
      A9G.println("AT+HTTPGET=\"" + FB_URL + "commands.json\"");
      Serial.println("[HTTP] Triggered HTTPGET for commands");
      delay(2000); // Allow some time for GET response
      
      // POST telemetry via direct HTTPPOST
      int rawADC = analogRead(voltPin);
      int batPct = constrain(map((rawADC / 4095.0) * 16.5 * 100, 330, 420, 0, 100), 0, 100);
      String lat = gps.location.isValid() ? String(gps.location.lat(), 6) : "0.0";
      String lon = gps.location.isValid() ? String(gps.location.lng(), 6) : "0.0";
      String payload = "{\"lat\":" + lat + ",\"lon\":" + lon + ",\"battery\":" + String(batPct) + ",\"tamper\":" + (digitalRead(reedPin) == HIGH ? "true" : "false") + "}";
      httpPatch("bag_data.json", payload);
    } else {
      gprsConnected = initGPRS();
    }
  }
}

void handleA9GLine(String line) {
  Serial.println("[A9G-OUT] " + line);

  if (line.indexOf("RING") != -1) {
    if (rcOption && !callPending) { callPending = true; ringTime = millis(); }
  }

  if (line.indexOf("+CLIP:") != -1) {
    int start = line.indexOf('\"');
    if (start != -1) {
      int end = line.indexOf('\"', start + 1);
      if (end != -1) {
        lastCallerPhone = line.substring(start + 1, end);
        if (rcOption && callPending) {
          A9G.println("ATH"); delay(500);
          sendLocationSMS(lastCallerPhone);
          callPending = false; lastCallerPhone = "";
        }
      }
    }
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

  if (line.startsWith("{") && line.indexOf("\"alarm\"") != -1) {
    processSecureFirstLogic(line);
  }

  if (line.indexOf("+CUSD:") != -1) {
    int start = line.indexOf('\"');
    if (start != -1) {
      int end = line.indexOf('\"', start + 1);
      if (end != -1) {
        String balHex = line.substring(start + 1, end);
        String bal = decodeHex(balHex);
        Serial.println("[BALANCE] " + bal);
        if (lastSmsSender != "") { sendSMS(lastSmsSender, "SIM Balance:\n" + bal); lastSmsSender = ""; }
        bal.replace("\"", "'");
        httpPatch("bag_data.json", "{\"balance\":\"" + bal + "\"}");
      }
    }
    // Restore character set to GSM
    A9G.println("AT+CSCS=\"GSM\"");
    ussdPending = false;
  }
}

void processSecureFirstLogic(String payload) {
  bool magnetAway = (digitalRead(reedPin) == HIGH); 
  bool riskZone = (payload.indexOf("PROXIMITY VIOLATION") != -1);
  bool appAlarm = (payload.indexOf("\"alarm\":true") != -1 || payload.indexOf("\"b\":true") != -1);
  bool rcOptionNew = (payload.indexOf("\"rc\":true") != -1);

  if (rcOptionNew != rcOption) {
    rcOption = rcOptionNew;
    prefs.begin("system-config", false);
    prefs.putBool("rc", rcOption);
    prefs.end();
  }

  if (!magnetAway && !riskZone) {
    digitalWrite(buzzerPin, LOW); digitalWrite(ledPin, LOW);
  } else {
    digitalWrite(buzzerPin, (magnetAway || riskZone || appAlarm) ? HIGH : LOW);
    digitalWrite(ledPin, (magnetAway || riskZone || rcOption) ? HIGH : LOW);
  }

  if (payload.indexOf("\"send_sms_location\":true") != -1 && gps.location.isValid()) {
    sendLocationSMS(userPhone);
    httpPatch("commands.json", "{\"send_sms_location\":false}");
  }
}

void processIncomingSMS(String msg, String sender) {
  msg.trim();
  if (msg.startsWith("SIM:") || msg.startsWith("sim:")) {
    userPhone = msg.substring(msg.indexOf(':') + 1); userPhone.trim();
    prefs.begin("system-config", false); prefs.putString("userPhone", userPhone); prefs.end();
    sendSMS(sender, "SIM updated: " + userPhone);
  }
  else if (msg.equalsIgnoreCase("STATUS")) {
    String statusText = "Microflux status:\nGPS: " + String(gps.location.lat(),6) + "," + String(gps.location.lng(),6) +
                        "\nMaps: https://www.google.com/maps?q=" + String(gps.location.lat(),6) + "," + String(gps.location.lng(),6) +
                        "\nBattery: " + String(constrain(map((analogRead(voltPin)/4095.0)*16.5*100, 330, 420, 0, 100), 0, 100)) + "%" +
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
  else if (msg.equalsIgnoreCase("RC:ON") || msg.equalsIgnoreCase("RC:1")) {
    rcOption = true;
    prefs.begin("system-config", false); prefs.putBool("rc", rcOption); prefs.end();
    sendSMS(sender, "Ringcut is now ARMED.");
  }
  else if (msg.equalsIgnoreCase("RC:OFF") || msg.equalsIgnoreCase("RC:0")) {
    rcOption = false;
    prefs.begin("system-config", false); prefs.putBool("rc", rcOption); prefs.end();
    sendSMS(sender, "Ringcut is now DISARMED.");
  }
  else if (msg.startsWith("APN:") || msg.startsWith("apn:")) {
    apn = msg.substring(msg.indexOf(':') + 1); apn.trim();
    prefs.begin("system-config", false); prefs.putString("apn", apn); prefs.end();
    sendSMS(sender, "APN updated: " + apn);
    gprsConnected = initGPRS();
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
  A9G.println("AT+CSCS=\"GSM\""); // Ensure we are in GSM mode to send plain text USSD
  delay(200);
  String ussdCode = (apn.indexOf("mobitel") != -1) ? "*100#" : ((apn.indexOf("hutch") != -1) ? "*111#" : "*123#");
  A9G.printf("AT+CUSD=1,\"%s\",15\r\n", ussdCode.c_str());
  delay(500);
  A9G.println("AT+CSCS=\"HEX\""); // Immediately switch to HEX mode to capture response in HEX
  ussdPending = true;
  ussdSentTime = millis();
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
