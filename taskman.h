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
#include <ArduinoOTA.h>

#define SAMPLE_RATE_HZ 1
#define SAMPLE_INTERVAL (1000 / SAMPLE_RATE_HZ)
#define SAMPLE_COUNT 100
#define MAX_TASKS 20

httpd_handle_t taskman_server = NULL;

// ─── STRUCTS ─────────────────────────────────────────────
struct TaskSample {
  String name;
  float usage[SAMPLE_COUNT];
  int index = 0;
  bool active = false;
  uint32_t prevRunTime = 0;
  bool over2 = false;
};

// ─── GLOBALS ─────────────────────────────────────────────
TaskSample* tasks = nullptr;
TaskStatus_t* taskStatusArray = nullptr;
int taskCount = 0;
uint32_t prevTotalRunTime = 0;

// ─── CPU MONITOR TASK ───────────────────────────────────
void cpuMonitorTask(void* param) {
  Serial.println("cpuMonitor started ...");
  for (;;) {
    int numTasks = uxTaskGetNumberOfTasks();

    // Allocate or resize if needed
    if (tasks == nullptr || numTasks != taskCount) {
      if (tasks) delete[] tasks;
      if (taskStatusArray) free(taskStatusArray);

      taskCount = numTasks;
      tasks = new TaskSample[taskCount];
      taskStatusArray = (TaskStatus_t*)malloc(sizeof(TaskStatus_t) * taskCount);
    }

    uint32_t totalRunTime;
    UBaseType_t numReturned = uxTaskGetSystemState(taskStatusArray, taskCount, &totalRunTime);
    if (numReturned == 0 || totalRunTime == prevTotalRunTime) {
      vTaskDelay(pdMS_TO_TICKS(SAMPLE_INTERVAL));
      continue;
    }

    uint32_t deltaTotal = totalRunTime - prevTotalRunTime;
    prevTotalRunTime = totalRunTime;

    for (uint32_t i = 0; i < numReturned; i++) {
      TaskStatus_t* t = &taskStatusArray[i];
      if (!t->pcTaskName) continue;

      // Find or create entry
      int idx = -1;
      for (int j = 0; j < taskCount; j++) {
        if (tasks[j].active && tasks[j].name == t->pcTaskName) {
          idx = j;
          break;
        }
      }
      if (idx == -1) {
        for (int j = 0; j < taskCount; j++) {
          if (!tasks[j].active) {
            idx = j;
            tasks[j].name = t->pcTaskName;
            //Serial.println(tasks[j].name);
            tasks[j].active = true;
            tasks[j].prevRunTime = t->ulRunTimeCounter;
            memset(tasks[j].usage, 0, sizeof(tasks[j].usage));
            break;
          }
        }
      }
      if (idx == -1) continue;

      uint32_t deltaTask = (t->ulRunTimeCounter >= tasks[idx].prevRunTime)
                             ? t->ulRunTimeCounter - tasks[idx].prevRunTime
                             : 0;
      tasks[idx].prevRunTime = t->ulRunTimeCounter;

      float usage = (deltaTotal > 0) ? (float)deltaTask / deltaTotal * 100.0f : 0;
      tasks[idx].usage[tasks[idx].index] = usage;
      tasks[idx].index = (tasks[idx].index + 1) % SAMPLE_COUNT;

      //Serial.printf("%s: %.2f%%\n", t->pcTaskName, usage);

      if (usage > 1.0f) tasks[idx].over2 = true;
    }

    vTaskDelay(pdMS_TO_TICKS(SAMPLE_INTERVAL));
  }
}

void BusyTask1(void* pv) {
  uint32_t idleMs = 10000UL;
  Serial.printf("BusyTask1: Core 1, idle for %lu ms... Delete fake_load in setup to get rid of these\n", idleMs);
  vTaskDelay(pdMS_TO_TICKS(idleMs));

  for (;;) {
    uint32_t runSecs = random(1, 10);
    Serial.printf("BusyTask1: Core 1, large  load for %d seconds...\n", runSecs);

    uint32_t start = millis();
    while ((millis() - start) < (runSecs * 1000UL)) {
      int j = 0;
      for ( volatile int i = 0; i < 80000; i++) {
        j = j + 1;               // compiler deletes this without volatile for some reason
      }
      if (j < 0) Serial.println("Busy1!");
      vTaskDelay(1);  // yield to other tasks
    }

    uint32_t idleMs = random(5, 20) * 1000UL;
    //Serial.printf("BusyTask1: Core 1, idle for %lu ms...\n", idleMs);
    vTaskDelay(pdMS_TO_TICKS(idleMs));
  }
}

void BusyTask0(void* pv) {
  uint32_t idleMs = 20000UL;
  Serial.printf("BusyTask0: Core 0, idle for %lu ms... Delete fake_load in setup to get rid of these\n", idleMs);
  vTaskDelay(pdMS_TO_TICKS(idleMs));

  for (;;) {
    uint32_t runSecs = random(1, 10);
    Serial.printf("BusyTask0: Core 0, medium load for %d seconds...\n", runSecs);

    uint32_t start = millis();
    while ((millis() - start) < (runSecs * 1000UL)) {
      float f = 0.0;
      for (int i = 0; i < 900; i++) {
        f = f + 3.14 / 2.71;
      }
      if (f < 0) Serial.println("Busy0!");
      vTaskDelay(1);  // yield to other tasks
    }

    uint32_t idleMs = random(5, 20) * 1000UL;
    //Serial.printf("BusyTask0: Core 0, idle for %lu ms...\n", idleMs);
    vTaskDelay(pdMS_TO_TICKS(idleMs));
  }
}

esp_err_t taskman_handleRoot(httpd_req_t* req) {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <title>ESP32 Task Monitor</title>
  <script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
  <style>
    body { font-family: sans-serif; background:#f4f4f4; margin:20px; }
    canvas { background:white; border-radius:10px; box-shadow:0 0 10px rgba(0,0,0,0.1); }
  </style>
</head>
<body>
  <h2>ESP32 Task Manager 4.3 </h2>
  <canvas id="cpuChart" width="900" height="400"></canvas>
  <p>- Click names to remove/restore lines<br>
     - Hover over line to find line name and value<br>
     - Refresh page to get full 100 seconds on data from esp32, otherwise getting 1 second updates<br>
     <a href="https://github.com/jameszah/ESP32-Task-Manager" target="_blank">Source Code here: https://github.com/jameszah/ESP32-Task-Manager</a>
  </p>
  <script>
let chart;
let sampleCount = 100; // number of samples to keep on screen  SAMPLE_COUNT !!!!

function createChart(initialJson) {
  const ctx = document.getElementById('cpuChart').getContext('2d');

  const datasets = Object.entries(initialJson).map(([name, data], i) => ({
    label: name,
    data: data,
    borderColor: `hsl(${i * 70 % 360}, 70%, 50%)`,
    borderWidth: 1.5,
    fill: false,
    tension: 0.4,
    pointRadius: 0,
    pointHoverRadius: 0
  }));

  chart = new Chart(ctx, {
    type: 'line',
    data: {
      labels: Array.from({ length: sampleCount }, (_, i) => i - sampleCount + 1),
      datasets: datasets
    },
    options: {
      animation: false,
      responsive: true,
scales: {
  x: {
    title: { display: true, text: 'Seconds Ago' }
  },
  y: {
    beginAtZero: true,
    max: 100,
    title: { display: true, text: 'CPU %' }
  }
},
      plugins: {
        legend: {
          position: 'bottom',
          labels: { boxWidth: 12 }
        }
      }
    }
  });
}

// ─── INITIAL LOAD ─────────────────────────────────────────────
async function init() {
  const res = await fetch('/data');
  const json = await res.json();
  createChart(json);

   // Start updates only AFTER chart is ready
  setInterval(updateChart, 1000);
}

// ─── UPDATE WITH NEW SAMPLE ───────────────────────────────────
async function updateChart() {

    // If chart doesn't exist yet, load full dataset once
  if (!chart) {
    const res = await fetch('/data');
    const json = await res.json();
    createChart(json);
    return;
  }
    // Save which datasets are hidden
  const hiddenMap = {};
  chart.data.datasets.forEach((d, i) => {
    hiddenMap[d.label] = chart.getDatasetMeta(i).hidden;
  });

  const res = await fetch('/dataCurrent');
  const json = await res.json();

  if (!chart) return;

  for (const [name, value] of Object.entries(json)) {
    let ds = chart.data.datasets.find(d => d.label === name);
    if (!ds) {
          // Task appeared for the first time — create a new dataset
    ds = {
      label: name,
      data: Array(chart.data.labels.length - 1).fill(0), // fill history with 0
      borderColor: `hsl(${chart.data.datasets.length * 70 % 360}, 70%, 50%)`,
      borderWidth: 1.5,
      fill: false,
      tension: 0.4,
      pointRadius: 0,
      pointHoverRadius: 0
    };
    chart.data.datasets.push(ds);
    }

    ds.data.push(value);            // add new point
    if (ds.data.length > sampleCount) ds.data.shift(); // keep fixed length
  }

  // shift labels too
  chart.data.labels.push(chart.data.labels.length);
  if (chart.data.labels.length > sampleCount) chart.data.labels.shift();

  // Restore hidden state
  chart.data.datasets.forEach((d, i) => {
    const meta = chart.getDatasetMeta(i);
    if (hiddenMap.hasOwnProperty(d.label)) {
      meta.hidden = hiddenMap[d.label];
    }
  });

// Keep x-axis labels aligned: -n … 0
chart.data.labels = Array.from({ length: sampleCount }, (_, i) => i - sampleCount + 1);

  chart.update('none');
}

window.addEventListener('load', init);

  </script>
</body>
</html>
)rawliteral";

  httpd_resp_set_type(req, "text/html");
  httpd_resp_send(req, html.c_str(), html.length());
  return ESP_OK;
}


esp_err_t taskman_handleData(httpd_req_t* req) {
  //Serial.println("... full data req");

  String json = "{";

  bool firstTask = true;
  for (int i = 0; i < taskCount; i++) {
    if (!tasks[i].active) continue;
    if (!tasks[i].over2) continue;

    if (!firstTask) json += ",";
    firstTask = false;

    json += "\"" + tasks[i].name + "\":[";

    int pos = tasks[i].index;
    for (int j = 0; j < SAMPLE_COUNT; j++) {
      json += String(tasks[i].usage[pos], 1);
      if (j < SAMPLE_COUNT - 1) json += ",";
      pos = (pos + 1) % SAMPLE_COUNT;
    }
    json += "]";
  }
  json += "}";
  //Serial.println(json);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json.c_str(), json.length());
  return ESP_OK;
}

esp_err_t taskman_handleDataCurrent(httpd_req_t* req) {
  String json = "{";
  bool firstTask = true;

  for (int i = 0; i < taskCount; i++) {
    if (!tasks[i].active) continue;
    if (!tasks[i].over2) continue;

    if (!firstTask) json += ",";
    firstTask = false;

    int pos = (tasks[i].index - 1 + SAMPLE_COUNT) % SAMPLE_COUNT;
    json += "\"" + tasks[i].name + "\":" + String(tasks[i].usage[pos], 1);
  }

  json += "}";
  //Serial.println(json);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json.c_str(), json.length());
  return ESP_OK;
}

void taskman_setup() {
  int start_free = ESP.getFreeHeap();
  
  Serial.println("\nhttps://github.com/jameszah/ESP32-Task-Manager\n");
  
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 81;   // 80;
  config.ctrl_port = 32770;  // 32769;   // control port

  if (httpd_start(&taskman_server, &config) == ESP_OK) {
    httpd_uri_t uri_root = { .uri = "/", .method = HTTP_GET, .handler = taskman_handleRoot };
    httpd_uri_t uri_data = { .uri = "/data", .method = HTTP_GET, .handler = taskman_handleData };
    httpd_uri_t uri_dataCurrent = { .uri = "/dataCurrent", .method = HTTP_GET, .handler = taskman_handleDataCurrent };

    httpd_register_uri_handler(taskman_server, &uri_root);
    httpd_register_uri_handler(taskman_server, &uri_data);
    httpd_register_uri_handler(taskman_server, &uri_dataCurrent);
  }
  xTaskCreatePinnedToCore(cpuMonitorTask, "CPU_Monitor", 1250, nullptr, 7, nullptr, 0);
  delay(3);
  Serial.printf("Taskman setup complete, used %d bytes of ram, current free %d\n",start_free-ESP.getFreeHeap(), ESP.getFreeHeap());
}

void taskman_setup_fake_load_tasks() {
  int start_free = ESP.getFreeHeap();
  xTaskCreatePinnedToCore(BusyTask1, "BusyTask1", 2000, nullptr, 1, nullptr, 1);
  xTaskCreatePinnedToCore(BusyTask0, "BusyTask0", 2000, nullptr, 1, nullptr, 0);
  Serial.printf("Fake load tasks setup complete, used %d bytes of ram, current free %d\n",start_free-ESP.getFreeHeap(), ESP.getFreeHeap());
}

void taskman_fake_loop_load() {
  int j = 0;
  for (int i = 0; i < 500; i++) {
    j = j + 1;
  }
  if (j < 0) Serial.println("busyloop!");
  delay(1);
}
