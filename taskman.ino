/*
  ESP32 Task Manager -- https://github.com/jameszah/ESP32-Task-Manager
  - display the cpu usage of all esp32 tasks on both cores (above 2%) on a website hosted by the esp32 itself
  - uses port 81 on the ip address (so port 80 left free for other uses)

  by James Zahary Oct 25, 2025 jamzah.plc@gmail.com
   
  https://github.com/jameszah/ESP32-Task-Manager is licensed under the GNU General Public License v3.0

  Arduino Code for ESP32, tested with 
  - Arduino IDE 2.3.6                   https://github.com/arduino/arduino-ide/releases/tag/2.3.6
  - arduino-esp32 v3.3.2                https://github.com/espressif/arduino-esp32/releases/tag/3.3.2
  - ai-thinker esp32-cam board hardware https://www.espboards.dev/esp32/esp32cam/

  Ver 4.3 - Oct 25, 2025
  - Initial release

More info:

  https://www.reddit.com/r/esp32/comments/1oeq3v6/whats_happening_inside_my_esp32/

*/

/*
Your own code needs wifi, and these two lines:

#include "taskman.h"       //  <--- the important bit

void setup(){
  taskman_setup();         //  <--- the important bit
}

And then access the taskmanager display with 192.168.1.111:81 
Your ip address, and PORT 81

*/


#include <Arduino.h>
#include <WiFi.h>
#include "esp_http_server.h"  
//#include <ArduinoOTA.h>

#include "taskman.h"       //  <--- the important bit

void setup() {

  Serial.begin(115200);
  WiFi.mode(WIFI_STA);
  WiFi.begin("ssid", "password");
  Serial.print("Connecting");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nConnected! IP: " + WiFi.localIP().toString());

  //ArduinoOTA.setHostname("taskman");  // just for convienience in testing
  //ArduinoOTA.begin();                 // just for convienience in testing

  taskman_setup();    //  <--- the important bit

  taskman_setup_fake_load_tasks();   // remove for actual use
}

void loop() {

  //ArduinoOTA.handle();               // just for convienience in testing

  taskman_fake_loop_load();          // remove for actual use

}

