/*
  A9G Serial Communication Test (115200 Baud Mode)
  --------------------------------------------------
  This code tests communication at 115200 baud.
  Set your Serial Monitor to 115200 baud!
  
  TO TEST:
  1. Flash this code.
  2. Disconnect the programmer's TX wire.
  3. Turn ON A9G manually.
*/

#include <Arduino.h>
#include <driver/uart.h>

unsigned long lastSendTime = 0;
const unsigned long interval = 2000; // Send AT every 2 seconds

// Helper function to print clean debug messages to PC Serial Monitor
void printToPC(String msg) {
  // 1. Temporarily turn OFF inversion for PC compatibility
  uart_set_line_inverse(UART_NUM_0, 0);
  
  // 2. Print message
  Serial.println(msg);
  Serial.flush(); // Wait for data to send
  
  // 3. Immediately turn ON inversion to restore A9G RX idle state (HIGH)
  uart_set_line_inverse(UART_NUM_0, UART_SIGNAL_TXD_INV);
}

void setup() {
  // Start Serial at 115200 baud
  Serial.begin(115200);
  delay(1000);
  
  // Keep Pin 15 HIGH permanently to enable the RX pull-up resistor R9
  pinMode(15, OUTPUT);
  digitalWrite(15, HIGH); 
  
  // Keep TX inverted by default to keep A9G RX HIGH when idle
  uart_set_line_inverse(UART_NUM_0, UART_SIGNAL_TXD_INV);
  
  printToPC("\n=============================================");
  printToPC("   A9G Serial Test (115200 Baud Mode)        ");
  printToPC("=============================================");
  printToPC("Using Pin 15 for PWR pull-up control.");
  printToPC("Set your Serial Monitor to 115200 baud!");
  printToPC("---------------------------------------------");
  printToPC("IMPORTANT: Disconnect the programmer's TX wire");
  printToPC("after flashing to prevent signal conflict!");
  printToPC("---------------------------------------------");
}

void loop() {
  // 1. Read and print any incoming characters from the A9G module
  String response = "";
  while (Serial.available() > 0) {
    char c = Serial.read();
    response += c;
  }
  
  if (response.length() > 0) {
    printToPC("[A9G Response]: " + response);
    if (response.indexOf("OK") != -1) {
      printToPC("-> STATUS: A9G Communication SUCCESS! [OK received]");
    } else {
      printToPC("-> STATUS: Received response, but no 'OK' found.");
    }
  }

  // 2. Automatically send "AT" command every 2 seconds
  if (millis() - lastSendTime >= interval) {
    lastSendTime = millis();
    
    // Inversion is already ON, so A9G receives clean "AT"
    Serial.print("AT\r\n");
    Serial.flush(); 
  }
}
