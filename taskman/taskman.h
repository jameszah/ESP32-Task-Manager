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
  Ver 4.5 - Oct 27
  - few fixes and more info
  Ver 4.7 - Oct 30
  - business with multi-core tasks - bug in uxTaskGetSystemState
  - shut off graph when not visible
  Ver 4.9 - Oct 31
  - fix 32bit rollover on the timers

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

#define SAMPLE_RATE_HZ 1
#define SAMPLE_INTERVAL (1000 / SAMPLE_RATE_HZ)
#define SAMPLE_COUNT 100

httpd_handle_t taskman_server = NULL;

// ─── STRUCTS ─────────────────────────────────────────────
struct TaskSample {
  String name;
  float usage[SAMPLE_COUNT];
  int index = 0;
  bool active = false;

  // from FreeRTOS TaskStatus_t
  UBaseType_t taskNumber = 0;
  eTaskState state;
  UBaseType_t currentPrio = 0;
  UBaseType_t basePrio = 0;
  uint32_t runTime = 0;
  uint32_t stackHighWater = 0;
  int core = -1;

  uint32_t prevRunTime = 0;
  bool over2 = false;

  // 🆕 Track how long since we last saw it alive
  int missingCount = 0;
};

constexpr int MAX_TASKS = 30;
TaskSample tasks[MAX_TASKS];
TaskStatus_t* taskStatusArray = nullptr;
uint32_t prevTotalRunTime = 0;
int maxtaskCount = 0;

void cpuMonitorTask(void* param) {
  Serial.println("cpuMonitor started ...");

  // Allocate system state array once
  if (!taskStatusArray) {
    taskStatusArray = (TaskStatus_t*)malloc(sizeof(TaskStatus_t) * MAX_TASKS);
    if (!taskStatusArray) {
      Serial.println("Failed to allocate taskStatusArray");
      vTaskDelete(nullptr);
      return;
    }
  }

  for (;;) {
    uint32_t totalRunTime;
    UBaseType_t numReturned = uxTaskGetSystemState(taskStatusArray, MAX_TASKS, &totalRunTime);

    if (numReturned == 0 || totalRunTime == prevTotalRunTime) {
      vTaskDelay(pdMS_TO_TICKS(SAMPLE_INTERVAL));
      continue;
    }

    uint32_t deltaTotal = totalRunTime - prevTotalRunTime;
    prevTotalRunTime = totalRunTime;

    // Mark all tasks as unseen this cycle
    bool seen[MAX_TASKS] = { false };

    // ── Process current system tasks ───────────────────────────────
    for (uint32_t i = 0; i < numReturned; i++) {
      TaskStatus_t* t = &taskStatusArray[i];
      if (!t->pcTaskName) continue;

      // Find existing task
      int idx = -1;
      for (int j = 0; j < maxtaskCount; j++) {
        if (tasks[j].name == t->pcTaskName) {
          idx = j;
          break;
        }
      }

      // If not found, add new one (if space)
      if (idx == -1 && maxtaskCount < MAX_TASKS) {
        idx = maxtaskCount++;
        tasks[idx].name = t->pcTaskName;
        tasks[idx].active = true;
        tasks[idx].prevRunTime = t->ulRunTimeCounter;
        memset(tasks[idx].usage, 0, sizeof(tasks[idx].usage));
        //Serial.printf("New task observed: %s\n", t->pcTaskName);
      }

      if (idx == -1) continue;  // no free slot available
      seen[idx] = true;

      /*
            // Skip if runtime goes backward
            if (t->ulRunTimeCounter < tasks[idx].prevRunTime) {
              if (tasks[idx].prevRunTime - t->ulRunTimeCounter > 0x0FFFFFFF){

                // do nothing - this is a ulRunTimeCounter rolled over the 32 bit number
                
              } else continue; // this is faulty data reported in systemstate
            }
      */

      uint32_t curr = t->ulRunTimeCounter;
      uint32_t prev = tasks[idx].prevRunTime;

      // If runtime goes backward
      if (curr < prev) {
        uint32_t diff = prev - curr;

        if (diff > 0x0FFFFFFF) {
          // ✅ Valid 32-bit counter rollover (huge backward jump)
          //    Let unsigned math handle it normally below
        } else {
          // 🚫 Small backward jump = bogus data from other core or race condition
          continue;
        }
      }

      // Compute delta normally — unsigned arithmetic handles rollover correctly
      uint32_t delta = curr - prev;

      // 🚫 Filter out absurdly large deltas (corrupted data)
      if (delta > 0x0FFFFFFF) continue;

      uint32_t deltaTask = delta; // t->ulRunTimeCounter - tasks[idx].prevRunTime;
      tasks[idx].prevRunTime = curr; // t->ulRunTimeCounter;

      float usage = (deltaTotal > 0) ? (float)deltaTask / deltaTotal * 100.0f : 0.0f;
      tasks[idx].usage[tasks[idx].index] = usage;
      tasks[idx].index = (tasks[idx].index + 1) % SAMPLE_COUNT;

      // Update system info
      tasks[idx].taskNumber = t->xTaskNumber;
      tasks[idx].state = t->eCurrentState;
      tasks[idx].currentPrio = t->uxCurrentPriority;
      tasks[idx].basePrio = t->uxBasePriority;
      tasks[idx].runTime = t->ulRunTimeCounter;
      tasks[idx].stackHighWater = t->usStackHighWaterMark;
      tasks[idx].core = t->xCoreID;
      //tasks[idx].over2          = (usage > 2.0f);
      if (usage > 2.0f) tasks[idx].over2 = true;
    }

    // ── Roll zeros for missing tasks ───────────────────────────────
    for (int j = 0; j < maxtaskCount; j++) {
      if (!seen[j]) {
        // Task not observed this round → roll in zero usage
        tasks[j].usage[tasks[j].index] = 0.0f;
        tasks[j].index = (tasks[j].index + 1) % SAMPLE_COUNT;
      }
    }

    vTaskDelay(pdMS_TO_TICKS(SAMPLE_INTERVAL));
  }
}

void FakeLoad1(void* pv) {
  uint32_t idleMs = 10000UL;
  Serial.printf("FakeLoad1: Core 1, idle for %lu ms... Delete fake_load in setup to get rid of these\n", idleMs);
  vTaskDelay(pdMS_TO_TICKS(idleMs));

  for (;;) {
    uint32_t runSecs = random(1, 10);
    //Serial.printf("FakeLoad1: Core 1, large  load for %d seconds...\n", runSecs);

    uint32_t start = millis();
    while ((millis() - start) < (runSecs * 1000UL)) {
      int j = 0;
      for (int i = 0; i < 200000; i++) {
        j = (j + 1) * -1;  // compiler deletes this without volatile for some reason
      }
      if (j < 0) Serial.println("Fake Load 1!");
      vTaskDelay(1);  // yield to other tasks
    }

    uint32_t idleMs = random(5, 20) * 1000UL;
    //Serial.printf("FakeLoad1: Core 1, idle for %lu ms...\n", idleMs);
    vTaskDelay(pdMS_TO_TICKS(idleMs));
  }
}


void FakeLoad0(void* pv) {
  const uint32_t cycleMs = 30000UL;  // full sine wave cycle = 30 seconds
  const float minLoad = 0.05;        // 5% minimum load
  const float maxLoad = 0.50;        // 50% maximum load
  const uint32_t stepMs = 200;       // time step resolution

  Serial.printf("FakeLoad0: Core %d, sine-wave fake load (%.0f–%.0f%%, %lus cycle)\n",
                xPortGetCoreID(), minLoad * 100, maxLoad * 100, cycleMs / 1000);

  uint32_t startCycle = millis();
  uint32_t lastReport = millis();

  for (;;) {
    uint32_t elapsed = (millis() - startCycle) % cycleMs;

    // Sine wave 0→2π
    float phase = (2.0f * PI * elapsed) / cycleMs;
    float loadFrac = minLoad + (maxLoad - minLoad) * (0.5f * (sinf(phase) + 1.0f));

    uint32_t busyMs = (uint32_t)(stepMs * loadFrac);
    uint32_t idleMs = stepMs - busyMs;

    // --- Busy loop ---
    uint32_t t0 = millis();
    while ((millis() - t0) < busyMs) {
      float f = 0.0f;
      for (int i = 0; i < 500; i++) f += 3.14f / 2.71f;
      if (f < 0) Serial.println("Fake Load 0!");
    }

    // --- Idle for remainder ---
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
    body {
      font-family: sans-serif;
      margin: 0;
      background: #fff;
      padding: 1em;
    }
    #cpuChart {
      width: 100%;
      max-height: 400px;
    }
    table {
      width: 100%;
      border-collapse: collapse;
      margin-top: 20px;
    }
    th, td {
      border: 1px solid #ccc;
      padding: 6px 10px;
      text-align: left;
    }
    th {
      background: #eee;
    }
  </style>
  </head>
<body>
<h2>ESP32 Task Manager </h2>
  <canvas id="cpuChart" width="900" height="400"></canvas>
<div style="
  background: #fff;
  padding: 12px 16px;
  border-radius: 10px;
  box-shadow: 0 2px 6px rgba(0,0,0,0.1);
  max-width: 900px;
  margin-top: 15px;
  line-height: 1.6;
  font-size: 14px;
  color: #333;
">
  <ul style="margin: 0 0 10px 20px; padding: 0;">
    <li>Click task names in legend to hide or restore lines</li>
    <li>Hover over a line to see task name and current CPU usage</li>
    <li>Refresh the page to load full 100 seconds of data from the ESP32, otherwise you're seeing live 1-second updates</li>
  </ul>
  <p style="margin: 0;">
    <a href="https://github.com/jameszah/ESP32-Task-Manager" target="_blank" 
       style="color:#0078d4; text-decoration:none;">
       Source Code on GitHub: <b>ESP32-Task-Manager 4.9</b>
    </a>
  </p>
</div>
<h3>Task Info - updates every 30 sec</h3>
<table id="taskTable" border="1" style="margin-top:10px; border-collapse:collapse; width:100%; background:white;">
  <thead>
    <tr style="background:#eee;">
      <th>Task Name</th>
      <th>Core</th>
      <th>Priority</th>
      <th>Stack HW</th>
      <th>State</th>
    </tr>
  </thead>
  <tbody></tbody>
</table>

  
  <script>
let chart;
let sampleCount = 100; // number of samples to keep on screen  SAMPLE_COUNT !!!!


function createChart() {
  const ctx = document.getElementById('cpuChart').getContext('2d');

  chart = new Chart(ctx, {
    type: 'line',
    data: {
      labels: Array.from({ length: sampleCount }, (_, i) => i - sampleCount + 1),
      datasets: [] // start empty — we’ll fill it later
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

async function updateChartData() {
  try {
    const res = await fetch('/data');
    const json = await res.json();

    if (!chart) return; // chart not ready yet

    Object.entries(json).forEach(([name, data], i) => {
      let dataset = chart.data.datasets.find(d => d.label === name);

      // Add new dataset if not found
      if (!dataset) {
        dataset = {
          label: name,
          data: data,
          borderColor: `hsl(${i * 70 % 360}, 70%, 50%)`,
          borderWidth: 1.5,
          fill: false,
          tension: 0.4,
          pointRadius: 0,
          pointHoverRadius: 0
        };
        chart.data.datasets.push(dataset);
      } else {
        dataset.data = data;
      }
    });

    chart.update('none'); // quick refresh, no animation
  } catch (err) {
    console.error('updateChartData failed:', err);
  }
}


// ─── INITIAL LOAD ─────────────────────────────────────────────

let charttimer;
let tabletimer;

// Main startup function
async function startWork() {
  console.log("Starting work (tab visible)");

  updateChartData();

  // Load table immediately
  updateTable();

  // Start periodic updates
  charttimer = setInterval(updateChart, 1000);
  tabletimer = setInterval(updateTable, 30000);
}

function stopWork() {
  console.log("Stopping work (tab hidden)");
  clearInterval(charttimer);
  clearInterval(tabletimer);
}

document.addEventListener("visibilitychange", async () => {
  if (document.visibilityState === "hidden") {
    stopWork();
  } else {
    await startWork(); // re-fetch fresh data when returning
  }
});

function init(){
  createChart();
  startWork();
}

// Start immediately when page first loads
window.addEventListener("load", init);

// ─── UPDATE WITH NEW SAMPLE ───────────────────────────────────
async function updateChart() {

    // If chart doesn't exist yet, load full dataset once
  if (!chart) {
    createChart();
    updateChartData();
    return;
  }

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

// Keep x-axis labels aligned: -n … 0
chart.data.labels = Array.from({ length: sampleCount }, (_, i) => i - sampleCount + 1);

  chart.update('none');
}

 async function updateTable() {
  try {
    const res = await fetch('/dataInfo');
    if (!res.ok) throw new Error("Fetch failed");
    const json = await res.json();

    const tbody = document.querySelector('#taskTable tbody');
    tbody.innerHTML = '';

    const stateNames = {
      0: 'Running',
      1: 'Ready',
      2: 'Blocked',
      3: 'Suspended',
      4: 'Deleted'
    };

    for (const [name, info] of Object.entries(json)) {
      const row = document.createElement('tr');
      row.innerHTML = `
        <td>${name}</td>
        <td>${info.core == 2147483647 ? '-' : info.core}</td>
        <td>${info.prio}</td>
        <td>${info.stackHW}</td>
        <td>${stateNames[info.state] ?? info.state}</td>
      `;
      tbody.appendChild(row);
    }
  } catch (e) {
    console.error("updateTable error:", e);
  }
}


//window.addEventListener('load', init);

  </script>
</body>
</html>
)rawliteral";

  httpd_resp_set_type(req, "text/html");
  httpd_resp_send(req, html.c_str(), html.length());
  return ESP_OK;
}

esp_err_t taskman_handleDataInfo(httpd_req_t* req) {
  String json = "{";
  bool firstTask = true;

  for (int i = 0; i < maxtaskCount; i++) {
    //if (!tasks[i].active) continue;
    if (!firstTask) json += ",";
    firstTask = false;

    json += "\"" + tasks[i].name + "\":{";
    json += "\"core\":" + String(tasks[i].core);
    json += ",\"prio\":" + String(tasks[i].currentPrio);
    json += ",\"stackHW\":" + String(tasks[i].stackHighWater);
    json += ",\"state\":" + String(tasks[i].state);
    json += "}";
  }

  json += "}";
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json.c_str(), json.length());
  return ESP_OK;
}

esp_err_t taskman_handleData(httpd_req_t* req) {
  //Serial.println("... full data req");

  String json = "{";

  bool firstTask = true;
  for (int i = 0; i < maxtaskCount; i++) {
    //if (!tasks[i].active) continue;
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

  for (int i = 0; i < maxtaskCount; i++) {
    //if (!tasks[i].active) continue;
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
    httpd_uri_t uri_dataInfo = { .uri = "/dataInfo", .method = HTTP_GET, .handler = taskman_handleDataInfo };
    httpd_uri_t uri_dataCurrent = { .uri = "/dataCurrent", .method = HTTP_GET, .handler = taskman_handleDataCurrent };


    httpd_register_uri_handler(taskman_server, &uri_root);
    httpd_register_uri_handler(taskman_server, &uri_dataInfo);
    httpd_register_uri_handler(taskman_server, &uri_data);
    httpd_register_uri_handler(taskman_server, &uri_dataCurrent);
  }
  xTaskCreatePinnedToCore(cpuMonitorTask, "CPU_Monitor", 1250, nullptr, 7, nullptr, 0);
  vTaskDelay(pdMS_TO_TICKS(3));
  Serial.printf("Taskman setup complete, used %d bytes of ram, current free %d\n", start_free - ESP.getFreeHeap(), ESP.getFreeHeap());
}

void taskman_setup_fake_load_tasks() {
  int start_free = ESP.getFreeHeap();
  xTaskCreatePinnedToCore(FakeLoad1, "FakeLoad1", 2000, nullptr, 1, nullptr, 1);
  xTaskCreatePinnedToCore(FakeLoad0, "FakeLoad0", 2500, nullptr, 1, nullptr, 0);
  Serial.printf("Fake load tasks setup complete, used %d bytes of ram, current free %d\n", start_free - ESP.getFreeHeap(), ESP.getFreeHeap());
}

void taskman_fake_loop_load() {
  int j = 0;
  for (int i = 0; i < 500; i++) {
    j = j + 1;
  }
  if (j < 0) Serial.println("fake load loop!");
  vTaskDelay(pdMS_TO_TICKS(1));
  ;
}