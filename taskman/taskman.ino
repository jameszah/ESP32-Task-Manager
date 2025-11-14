/*
  ESP32 Task Manager -- https://github.com/jameszah/ESP32-Task-Manager
  - display the cpu usage of all esp32 tasks on both cores (above 2%) on a website hosted by the esp32 itself
  - uses port 81 on the ip address (so port 80 left free for other uses)

  by James Zahary Oct 25, 2025 jamzah.plc@gmail.com
   
  https://github.com/jameszah/ESP32-Task-Manager is licensed under the GNU General Public License v3.0


*/

/*

See taskman.h for more information

*/


#include <Arduino.h>
#include <WiFi.h>
#include "esp_http_server.h"
#include <ArduinoOTA.h>

#define PROGRAM_NAME "taskman7.0"  //  <--- the important bit
//#define SAMPLE_RATE_HZ 1    // default is 1, or 2,4,8 for samples per second

#include "taskman.h"               //  <--- the important bit

#include "esp_http_server.h"

esp_err_t root_get_handler(httpd_req_t *req) {
  const char *html =
    "<!DOCTYPE html>"
    "<html>"
    "<head><title>ESP32 Task Manager</title></head>"
    "<body>"
    "<h2>ESP32 Task Manager</h2>"
    "<ul>"
    "<li><a href=\"/taskman\">/taskman</a></li>"
    "<li><a href=\"/network\">/network</a></li>"
    "<li><a href=\"/data\">/data</a></li>"
    "<li><a href=\"/dataInfo\">/dataInfo</a></li>"
    "</ul>"
    "</body>"
    "</html>";

  httpd_resp_set_type(req, "text/html");
  httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}


void setup() {

  taskman_setup();  //  <--- the important bit

  /////////// this section starts wifi and ota
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

  /////////// this section starts wifi and ota

  httpd_handle_t test_server;

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.ctrl_port = 32769;
  config.stack_size = 6 * 1024;    // optional tweak
  config.lru_purge_enable = true;  // auto-clean old sockets
  config.max_open_sockets = 8;     // safer defaults
  config.recv_wait_timeout = 5;
  config.send_wait_timeout = 5;

  esp_err_t err = httpd_start(&test_server, &config);
  if (err != ESP_OK) {
    Serial.printf("❌ Failed to start Taskman server (err=%d)\n", err);
    return;
  } else {
    Serial.println("✅ Taskman server started");
  }

  // see taskman.h Option 1
  taskman_server_setup(test_server);  //  <--- the important bit

  httpd_uri_t uri_root = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = root_get_handler,
    .user_ctx = NULL
  };
  httpd_register_uri_handler(test_server, &uri_root);

  // see taskman.h Option 2
  //taskman_server_setup();  //  <--- the important bit

  taskman_setup_fake_load_tasks();  // remove for actual use
}

void loop() {

  //ArduinoOTA.handle();  // just for convienience in testing

  taskman_fake_loop_load();  // remove for actual use

}

