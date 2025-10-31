/*
 * --- THIS CODE GOES IN main.cpp ---
 * * It assumes Phase 1 and Phase 2 are complete
 * and you have a working "adin_netif_init()" function
 * that starts the network.
 */

#include <Arduino.h>
#include <WebServer.h> // The standard Arduino WebServer library
#include "SPIDevice.h"
#include "ADIN1110.h"

// --- Your objects from before ---
const int ADIN_CS_PIN = 5;
SPIDevice adinSPI(ADIN_CS_PIN, 1000000, MSBFIRST, SPI_MODE0);
ADIN1110 adin(adinSPI);

// --- The standard Web Server object ---
WebServer server(80); // Create a server on port 80

// --- Your custom network init function (from Phase 2) ---
// This function will initialize the ADIN1110 and esp_netif
// and connect to the network.
void my_network_init() {
  // 1. Initialize the ADIN1110 chip
  if (!adin.begin()) {
    Serial.println("Failed to init ADIN1110");
    while(true) delay(1);
  }

  // 2. TODO: Start the esp_netif glue driver (The hard part)
  //    ... This would involve:
  //    ... esp_netif_init()
  //    ... create_my_netif_driver()
  //    ... attach_the_isr()
  //    ... create_the_rx_task()
  //    ... esp_netif_attach()

  Serial.println("Network driver started. Waiting for link...");

  // 3. Wait for the 10BASE-T1L link to be active
  while (!adin.isLinkUp()) {
    delay(100);
  }
  Serial.println("Network Link is UP.");

  // 4. TODO: Wait for lwIP to get an IP address
  //    ... (code to wait for GOT_IP event)
  // Serial.print("IP Address: ");
  // Serial.println(Ethernet.localIP()); // (Or the esp_netif equivalent)
}

// --- Web server "handler" functions ---
void handleRoot() {
  String html = "<html><head><title>ESP32 SPE Server</title></head>";
  html += "<body style='font-family: sans-serif;'>";
  html += "<h1>Hello from 10BASE-T1L!</h1>";
  html += "<p>This page is hosted on an ESP32 over Single Pair Ethernet.</p>";
  html += "</body></html>";
  
  server.send(200, "text/html", html);
}

void handleNotFound() {
  server.send(404, "text/plain", "Not Found");
}


// --- Main Setup and Loop ---
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("--- Starting ADIN1110 Web Server ---");

  // This one function call does all the complex network setup
  my_network_init(); 

  // --- Web Server Setup ---
  // Once the network is up, this is all standard code
  server.on("/", handleRoot);
  server.onNotFound(handleNotFound);
  server.begin(); // Start the web server
  Serial.println("Web server started. Open the IP address in a browser.");
}

void loop() {
  // This function must be called in the loop
  // to allow the server to handle incoming client requests.
  server.handleClient();
}