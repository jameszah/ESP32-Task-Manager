/*
  ESP32 Task Manager -- https://github.com/jameszah/ESP32-Task-Manager
  - display the cpu usage of all esp32 tasks on both cores (above 2%) on a website hosted by the esp32 itself
  - uses port 81 or port 80 (see below) on the ip address 

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
 Ver 5.0 - Nov 1
 - put the filename of program in the title
 Ver 6.2 - Nov 3
 - another graph of free ram and psram
 Ver 7.0 - Nov 13
 - another page of network information
 - reuse existing httpd server to save 10kb ram
 - drop 1-second updates and just print entire graph
 
More info:

  https://www.reddit.com/r/esp32/comments/1oeq3v6/whats_happening_inside_my_esp32/

*/

/*
Your own code needs wifi, and these lines:

#include "taskman.h"       //  <--- the important bit
#define PROGRAM_NAME "your program" 
#define SAMPLE_RATE_HZ 1    // default is 1, or 2,4,8 for samples per second

void setup(){
  taskman_setup();         //  <--- the important bit
  
  // Two options for the webserver
  // 1. You start wifi, and then taskman_server_setup();, 
  //    then 192.168.1.111:81/taskman
  // 2. You start wifi, and a httpd web server for your own stuff, 
  //    and then taskman_server_setup(mainServer), and it will use your existing server - saving 10kb of ram
  //    then 192.168.1.111/taskman (port 80 is assumed)

  // Option 1 -- taskman on port 81
  taskman_server_setup();  //  <--- the important bit
  
  // Option 2 - taskman on port 80 along with all your own endpoints
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  httpd_handle_t mainServer = NULL; 
  httpd_start(&mainServer, &config);
  taskman_server_setup(mainServer);  <--- the important bit
}


*/

#include <Arduino.h>
#include <WiFi.h>
#include "esp_http_server.h"

#ifndef PROGRAM_NAME
#define PROGRAM_NAME "replace with your name"
#endif

// how many samples per second 1, 2, 4, or 8
#ifndef SAMPLE_RATE_HZ
#define SAMPLE_RATE_HZ 1
#endif

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

  //  Track how long since we last saw it alive
  int missingCount = 0;
};

// ---- System-wide sampling ----
struct SystemSample {
  uint32_t freeRam[SAMPLE_COUNT];    // free RAM in KB
  uint32_t freePSRam[SAMPLE_COUNT];  // free PSRAM in KB
  int index = 0;                     // rolling index for samples
};

// Global instance
SystemSample sysSamples;

constexpr int MAX_TASKS = 25;
TaskSample tasks[MAX_TASKS];
TaskStatus_t* taskStatusArray = nullptr;
uint32_t prevTotalRunTime = 0;
int maxtaskCount = 0;

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <lwip/sockets.h>

#define MAX_URI 16
#define MAX_URI_LEN 32

struct UriStats {
  char uri[MAX_URI_LEN];  // copy URI here
  uint32_t hits;
  uint64_t totalDurationUs;
  uint64_t maxDurationUs;
  uint64_t lastHitUs;
};

UriStats uriStats[MAX_URI];

UriStats* getUriStats(const char* uri) {
  // Look for existing
  for (int i = 0; i < MAX_URI; i++) {
    if (uriStats[i].uri && strcmp(uriStats[i].uri, uri) == 0) {
      return &uriStats[i];
    }
  }

  // Find empty slot
  for (int i = 0; i < MAX_URI; i++) {
    if (uriStats[i].uri[0] == '\0') {  // empty slot
      strncpy(uriStats[i].uri, uri, MAX_URI_LEN - 1);
      uriStats[i].uri[MAX_URI_LEN - 1] = '\0';  // ensure null termination
      uriStats[i].hits = 0;
      uriStats[i].totalDurationUs = 0;
      uriStats[i].maxDurationUs = 0;
      uriStats[i].lastHitUs = 0;
      return &uriStats[i];
    }
  }

  // No room
  return nullptr;
}

static uint64_t nowUs() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (uint64_t)tv.tv_sec * 1000000ULL + tv.tv_usec;
}

// ====== Session Tracking ======

#include <string.h>

#define MAX_ACTIVE_SESS 8

struct ActiveSessionInfo {
  bool in_use;
  int sock;
  uint64_t start_us;
  uint64_t duration_us;
  char uri[32];
};

static ActiveSessionInfo sessions[MAX_ACTIVE_SESS];

// Call once at boot
void initSessions() {
  for (int i = 0; i < MAX_ACTIVE_SESS; i++) {
    sessions[i].in_use = false;
    sessions[i].uri[0] = 0;
  }
}

void recordSessionStart(int sock, const char* uri) {
  for (int i = 0; i < MAX_ACTIVE_SESS; i++) {
    if (!sessions[i].in_use) {
      sessions[i].in_use = true;
      sessions[i].sock = sock;
      sessions[i].start_us = nowUs();

      strncpy(sessions[i].uri, uri, sizeof(sessions[i].uri));
      sessions[i].uri[sizeof(sessions[i].uri) - 1] = 0;
      return;
    }
  }
}

void recordSessionEnd(int sock, uint64_t duration) {
  for (int i = 0; i < MAX_ACTIVE_SESS; i++) {
    if (sessions[i].in_use && sessions[i].sock == sock) {
      sessions[i].duration_us = duration;
      sessions[i].in_use = false;
      return;
    }
  }
}


// ====== Wrapper Handler ======

typedef esp_err_t (*actual_handler_t)(httpd_req_t*);

esp_err_t tracked_handler(httpd_req_t* req) {
  actual_handler_t fn = (actual_handler_t)req->user_ctx;
  const char* uriText = req->uri;

  int sock = httpd_req_to_sockfd(req);

  recordSessionStart(sock, uriText);

  uint64_t start = nowUs();
  esp_err_t res = fn(req);
  uint64_t dur = nowUs() - start;

  recordSessionEnd(sock, dur);

  // Update stats for the correct URI
  UriStats* s = getUriStats(uriText);
  if (s) {
    s->hits++;
    s->totalDurationUs += dur;
    if (dur > s->maxDurationUs) s->maxDurationUs = dur;
    s->lastHitUs = nowUs();
  }

  return res;
}

#include "esp_http_server.h"
#include "lwip/sockets.h"
#include "lwip/tcp.h"
#include "lwip/priv/tcp_priv.h"  // access internal tcp_pcb list
#include "lwip/inet.h"
#include "lwip/ip_addr.h"

// Convert lwIP tcp_state enum to readable text
static const char* tcpStateName(enum tcp_state st) {
  switch (st) {
    case CLOSED: return "CLOSED";
    case LISTEN: return "LISTEN";
    case SYN_SENT: return "SYN_SENT";
    case SYN_RCVD: return "SYN_RCVD";
    case ESTABLISHED: return "ESTABLISHED";
    case FIN_WAIT_1: return "FIN_WAIT_1";
    case FIN_WAIT_2: return "FIN_WAIT_2";
    case CLOSE_WAIT: return "CLOSE_WAIT";
    case CLOSING: return "CLOSING";
    case LAST_ACK: return "LAST_ACK";
    case TIME_WAIT: return "TIME_WAIT";
    default: return "?";
  }
}

#include "lwip/sockets.h"
#include "lwip/priv/tcp_priv.h"

#include "lwip/sockets.h"
#include "lwip/api.h"
#include "lwip/priv/tcp_priv.h"
#include "lwip/priv/sockets_priv.h"

#include "lwip/tcp.h"
#include "lwip/sockets.h"
#include "lwip/ip4_addr.h"
#include "lwip/ip6_addr.h"

// Normalize sockaddr_storage into IPv4 + port
static bool sockaddrToIPv4(const struct sockaddr_storage* sa, uint32_t* out_ip, uint16_t* out_port) {
  if (sa->ss_family == AF_INET) {
    const sockaddr_in* a = (const sockaddr_in*)sa;
    *out_ip = a->sin_addr.s_addr;
    *out_port = ntohs(a->sin_port);
    return true;
  }

  if (sa->ss_family == AF_INET6) {
    const sockaddr_in6* a6 = (const sockaddr_in6*)sa;

    // Check for IPv4 mapped ::ffff:a.b.c.d
    if (IN6_IS_ADDR_V4MAPPED(&a6->sin6_addr)) {
      uint32_t ip4;
      memcpy(&ip4, &a6->sin6_addr.s6_addr[12], 4);
      *out_ip = ip4;
      *out_port = ntohs(a6->sin6_port);
      return true;
    }
  }

  return false;
}

static tcp_pcb* matchPcbToSock(tcp_pcb* pcb, const struct sockaddr_storage* sa) {
  if (!pcb) return nullptr;

  // Compare local port
  uint16_t lport = 0, rport = 0;
  ip_addr_t lip = pcb->local_ip;
  ip_addr_t rip = pcb->remote_ip;

  lport = pcb->local_port;
  rport = pcb->remote_port;

  if (sa->ss_family == AF_INET) {
    const sockaddr_in* a = (const sockaddr_in*)sa;
    if (ntohs(a->sin_port) != rport) return nullptr;
    if (rip.u_addr.ip4.addr != a->sin_addr.s_addr) return nullptr;
    return pcb;  // match
  }

  if (sa->ss_family == AF_INET6) {
    const sockaddr_in6* a6 = (const sockaddr_in6*)sa;
    if (ntohs(a6->sin6_port) != rport) return nullptr;

    // Compare full IPv6 address
    if (memcmp(&rip.u_addr.ip6, &a6->sin6_addr, sizeof(ip6_addr_t)) != 0)
      return nullptr;

    return pcb;
  }
  return nullptr;
}

tcp_pcb* pcbForSocket(int fd) {
  struct sockaddr_storage sa;
  socklen_t sl = sizeof(sa);

  if (getpeername(fd, (struct sockaddr*)&sa, &sl) < 0)
    return nullptr;

  uint32_t peer_ip4;
  uint16_t peer_port4;

  if (!sockaddrToIPv4(&sa, &peer_ip4, &peer_port4))
    return nullptr;

  // Scan active connections
  for (tcp_pcb* pcb = tcp_active_pcbs; pcb; pcb = pcb->next) {
    if (pcb->remote_ip.type == IPADDR_TYPE_V4 && pcb->remote_ip.u_addr.ip4.addr == peer_ip4 && pcb->remote_port == peer_port4) {
      return pcb;
    }
  }

  // Scan TIME_WAIT
  for (tcp_pcb* pcb = tcp_tw_pcbs; pcb; pcb = pcb->next) {
    if (pcb->remote_ip.type == IPADDR_TYPE_V4 && pcb->remote_ip.u_addr.ip4.addr == peer_ip4 && pcb->remote_port == peer_port4) {
      return pcb;
    }
  }

  return nullptr;
}

// Get TCP state for an FD
static const char* getTcpState(int fd) {
  struct tcp_pcb* pcb = pcbForSocket(fd);
  if (!pcb) return "unknown";
  return tcpStateName(pcb->state);
}

// Get pending bytes for an FD
static int getPendingBytes(int fd) {
  int pending = 0;
  ioctl(fd, FIONREAD, &pending);
  return pending;
}

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_system.h"
#include "esp_partition.h"
#include "esp_heap_caps.h"

#include "lwip/tcp.h"
#include "lwip/ip_addr.h"

String appendTcpPcbsHtml() {
  String html = "";
  html += "<h2>TCP Active PCBs</h2>";
  html += "<table border='1' cellpadding='4'>";
  html += "<tr>"
          "<th>#</th>"
          "<th>PCB</th>"
          "<th>State</th>"
          "<th>Local</th>"
          "<th>Remote</th>"
          "<th>Recv_Q</th>"
          "<th>Send_Q</th>"
          "</tr>";

  int idx = 0;
  struct tcp_pcb* pcb = tcp_active_pcbs;

  if (!pcb) {
    html += "<tr><td colspan='7'>(none)</td></tr>";
    html += "</table>";
    return html;
  }

  while (pcb) {
    char localStr[64];
    char remoteStr[64];

    snprintf(localStr, sizeof(localStr), "%s:%d",
             ipaddr_ntoa(&pcb->local_ip),
             pcb->local_port);

    snprintf(remoteStr, sizeof(remoteStr), "%s:%d",
             ipaddr_ntoa(&pcb->remote_ip),
             pcb->remote_port);

    // queue lengths
    u16_t recv_q = pcb->rcv_wnd;
    u16_t send_q = pcb->snd_queuelen;

    html += "<tr>";
    html += "<td>" + String(idx) + "</td>";
    html += "<td>0x" + String((uintptr_t)pcb, HEX) + "</td>";
    html += "<td>" + String(tcpStateName(pcb->state)) + "</td>";
    html += "<td>" + String(localStr) + "</td>";
    html += "<td>" + String(remoteStr) + "</td>";
    html += "<td>" + String(recv_q) + "</td>";
    html += "<td>" + String(send_q) + "</td>";
    html += "</tr>";

    pcb = pcb->next;
    idx++;
  }
  html += "</table>";
  return html;
}

String getDiagnostics() {
  String out;

  // Chip info
  esp_chip_info_t chip;
  esp_chip_info(&chip);

  // Internal RAM
  multi_heap_info_t info;
  heap_caps_get_info(&info, MALLOC_CAP_INTERNAL);

  out += "Internal RAM:\n";
  out += "------------------------------------------\n";
  out += "  Total Size        : " + String(info.total_free_bytes + info.total_allocated_bytes) + "\n";
  out += "  Free Bytes        : " + String(info.total_free_bytes) + "\n";
  out += "  Allocated Bytes   : " + String(info.total_allocated_bytes) + "\n";
  out += "  Minimum Free Bytes: " + String(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)) + "\n";
  out += "------------------------------------------\n";

  // PSRAM
  if (psramFound()) {
    multi_heap_info_t pinfo;
    heap_caps_get_info(&pinfo, MALLOC_CAP_SPIRAM);

    out += "PSRAM:\n";
    out += "------------------------------------------\n";
    out += "  Total Size        : " + String(pinfo.total_free_bytes + pinfo.total_allocated_bytes) + "\n";
    out += "  Free Bytes        : " + String(pinfo.total_free_bytes) + "\n";
    out += "  Allocated Bytes   : " + String(pinfo.total_allocated_bytes) + "\n";
    out += "  Minimum Free Bytes: " + String(heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM)) + "\n";
    out += "------------------------------------------\n";
  }

  // Flash size (uint32_t*)
  uint32_t flashSize = 0;
  esp_flash_get_size(NULL, &flashSize);

  out += "Flash:\n";
  out += "------------------------------------------\n";
  out += "  Flash Size        : " + String(flashSize) + "\n";
  out += "------------------------------------------\n";

  return out;
}

#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_clk_tree.h"

String getChipInfoBlock() {
  String out;

  esp_chip_info_t chip;
  esp_chip_info(&chip);

  // CPU frequency
  uint32_t cpuHz = 0;
  esp_clk_tree_src_get_freq_hz(SOC_MOD_CLK_CPU, ESP_CLK_TREE_SRC_FREQ_PRECISION_EXACT, &cpuHz);

  // XTAL frequency
  uint32_t xtalHz = 0;
  esp_clk_tree_src_get_freq_hz(SOC_MOD_CLK_XTAL, ESP_CLK_TREE_SRC_FREQ_PRECISION_EXACT, &xtalHz);

  out += "Chip Info:\n";
  out += "------------------------------------------\n";

  // Model
  const char* model = "ESP32";
  out += "  Model             : " + String(model) + "\n";

  // Package (not directly exposed in IDF v5; keep user-supplied string)
  out += "  Package           : D0WD-Q6\n";

  // Revision
  out += "  Revision          : " + String(chip.revision) + ".00\n";

  // Cores
  out += "  Cores             : " + String(chip.cores) + "\n";

  // CPU freq
  out += "  CPU Frequency     : " + String(cpuHz / 1000000) + " MHz\n";

  // XTAL freq
  out += "  XTAL Frequency    : " + String(xtalHz / 1000000) + " MHz\n";

  // Raw features
  out += "  Features Bitfield : 0x" + String(chip.features, HEX) + "\n";

  // Feature parsing
  out += "  Embedded Flash    : " + String((chip.features & CHIP_FEATURE_EMB_FLASH) ? "Yes" : "No") + "\n";
  out += "  Embedded PSRAM    : " + String((chip.features & CHIP_FEATURE_EMB_PSRAM) ? "Yes" : "No") + "\n";
  out += "  2.4GHz WiFi       : " + String((chip.features & CHIP_FEATURE_WIFI_BGN) ? "Yes" : "No") + "\n";
  out += "  Classic BT        : " + String((chip.features & CHIP_FEATURE_BT) ? "Yes" : "No") + "\n";
  out += "  BT Low Energy     : " + String((chip.features & CHIP_FEATURE_BLE) ? "Yes" : "No") + "\n";
  out += "  IEEE 802.15.4     : No\n";  // ESP32 classic does not include this

  return out;
}

esp_err_t taskman_handleNetwork(httpd_req_t* req) {
  String html;
  html.reserve(4096);

  html += "<html><head><title>ESP32 Task Manager - Network</title>";
  String progName = PROGRAM_NAME;
  
  html +=
  "<style>"
    "body {"
      "font-family: sans-serif;"
      "margin: 0;"
      "background: #fff;"
      "padding: 1em;"
    "}"
    "#cpuChart {"
      "width: 100%;"
      "max-height: 400px;"
    "}"
    "table {"
      "width: 100%;"
      "border-collapse: collapse;"
      "margin-top: 20px;"
    "}"
    "th, td {"
      "border: 1px solid #ccc;"
      "padding: 6px 10px;"
      "text-align: left;"
    "}"
    "th {"
      "background: #eee;"
    "}"
  "</style>"
  "</head>";

  html += "<h2>ESP32 Task Manager - ";
  html += progName;
  html += "</h2>";

  html += "<style>"
          "table { border-collapse: collapse; width: 100%; }"
          "td, th { border:1px solid #ccc; padding:4px; text-align:left; }"
          "</style></head><body>";

  html += "<h2>Active Sessions</h2>";
  html += "<table>";
  html += "<tr><th>#</th><th>In Use</th><th>Socket</th><th>IPv4</th><th>IPv6</th><th>Port</th><th>URI</th>"
          "<th>Start </th><th>Duration (us)</th></tr>";

  for (int i = 0; i < MAX_ACTIVE_SESS; i++) {
    const ActiveSessionInfo& s = sessions[i];
    uint64_t dur = s.in_use ? (nowUs() - s.start_us) : s.duration_us;

    // --- New: convert start_us → "seconds ago" ---
    double secAgo = (nowUs() - s.start_us) / 1'000'000.0;
    char startedStr[32];
    snprintf(startedStr, sizeof(startedStr), "%.2f sec ago", secAgo);

    char ipv4str[INET_ADDRSTRLEN] = "(none)";
    char ipv6str[INET6_ADDRSTRLEN] = "(none)";
    uint16_t port = 0;
    if (s.sock == 0) continue;
    if (s.sock > 0) {
      struct sockaddr_storage addr;
      socklen_t alen = sizeof(addr);

      if (getpeername(s.sock, (struct sockaddr*)&addr, &alen) == 0) {
        if (addr.ss_family == AF_INET) {
          // IPv4
          struct sockaddr_in* a = (struct sockaddr_in*)&addr;
          inet_ntop(AF_INET, &a->sin_addr, ipv4str, sizeof(ipv4str));
          port = ntohs(a->sin_port);
        } else if (addr.ss_family == AF_INET6) {
          struct sockaddr_in6* a6 = (struct sockaddr_in6*)&addr;
          if (IN6_IS_ADDR_V4MAPPED(&a6->sin6_addr)) {
            // IPv4-mapped IPv6 -> store in IPv4 column
            struct in_addr a4;
            memcpy(&a4, &a6->sin6_addr.s6_addr[12], sizeof(a4));
            inet_ntop(AF_INET, &a4, ipv4str, sizeof(ipv4str));
            port = ntohs(a6->sin6_port);
          } else {
            // pure IPv6
            inet_ntop(AF_INET6, &a6->sin6_addr, ipv6str, sizeof(ipv6str));
            port = ntohs(a6->sin6_port);
          }
        }
      }
    }

    html += "<tr>";
    html += "<td>" + String(i) + "</td>";
    html += "<td>" + String(s.in_use ? "yes" : "no") + "</td>";
    html += "<td>" + String(s.sock) + "</td>";
    html += "<td>" + String(ipv4str) + "</td>";
    html += "<td>" + String(ipv6str) + "</td>";
    html += "<td>" + String(port) + "</td>";
    html += "<td>" + String(s.uri ? s.uri : "(none)") + "</td>";
    html += "<td>" + String(startedStr) + "</td>";   
    html += "<td>" + String(dur) + "</td>";
    html += "</tr>";
  }

  html += "</table>";

  html += "<h2>Endpoint Stats</h2>";
  html += "<table border='1' cellpadding='4'>";
  html += "<tr><th>URI</th><th>Hits</th><th>Avg (ms)</th><th>Max (ms)</th><th>Last (Seconds ago)</th></tr>";

  uint64_t now = nowUs();

  for (int i = 0; i < MAX_URI; i++) {
    if (!uriStats[i].hits) continue;

    uint64_t avgUs = uriStats[i].totalDurationUs / uriStats[i].hits;
    uint64_t maxUs = uriStats[i].maxDurationUs;

    uint64_t avgMs = avgUs / 1000;
    uint64_t maxMs = maxUs / 1000;

    uint64_t lastAgoSec = (now - uriStats[i].lastHitUs) / 1000000ULL;

    html += "<tr>";
    html += "<td>" + String(uriStats[i].uri) + "</td>";
    html += "<td>" + String(uriStats[i].hits) + "</td>";
    html += "<td>" + String(avgMs) + "</td>";
    html += "<td>" + String(maxMs) + "</td>";
    html += "<td>" + String(lastAgoSec) + "</td>";
    html += "</tr>";
  }
  html += "</table>";

  //////////////////////////////////////////////
  // HTTPD Client List With TCP State + Pending
  //////////////////////////////////////////////

#define HTTPD_MAX_CLIENTS 8

  size_t client_count = HTTPD_MAX_CLIENTS;
  int client_fds[HTTPD_MAX_CLIENTS] = { 0 };

  // Pull active sockets from ESP-IDF HTTPD
  esp_err_t client_err = httpd_get_client_list(taskman_server, &client_count, client_fds);

  html += "<h2>HTTPD Client List</h2>";
  html += "<table border='1' cellpadding='4'>";
  html += "<tr>"
          "<th>#</th>"
          "<th>Socket</th>"
          "<th>IPv4</th>"
          "<th>IPv6</th>"
          "<th>Port</th>"
          "<th>Tracked?</th>"
          "<th>TCP</th>"
          "<th>Pending</th>"
          "</tr>";

  if (client_err != ESP_OK) {
    html += "<tr><td colspan='8'>httpd_get_client_list failed</td></tr>";
  } else if (client_count == 0) {
    html += "<tr><td colspan='8'>(none)</td></tr>";
  } else {
    for (int i = 0; i < client_count; i++) {

      int sock = client_fds[i];

      char ipv4str[INET_ADDRSTRLEN] = "(none)";
      char ipv6str[INET6_ADDRSTRLEN] = "(none)";
      uint16_t port = 0;

      struct sockaddr_storage addr;
      socklen_t alen = sizeof(addr);

      if (getpeername(sock, (struct sockaddr*)&addr, &alen) == 0) {
        if (addr.ss_family == AF_INET) {
          struct sockaddr_in* a = (struct sockaddr_in*)&addr;
          inet_ntop(AF_INET, &a->sin_addr, ipv4str, sizeof(ipv4str));
          port = ntohs(a->sin_port);

          ipv6str[0] = 0;  // hide IPv6

        } else if (addr.ss_family == AF_INET6) {
          struct sockaddr_in6* a6 = (struct sockaddr_in6*)&addr;

          if (IN6_IS_ADDR_V4MAPPED(&a6->sin6_addr)) {
            // Convert mapped IPv6 to IPv4 cleanly
            struct in_addr a4;
            memcpy(&a4, &a6->sin6_addr.s6_addr[12], sizeof(a4));
            inet_ntop(AF_INET, &a4, ipv4str, sizeof(ipv4str));
            port = ntohs(a6->sin6_port);
            ipv6str[0] = 0;

          } else {
            // Real IPv6
            inet_ntop(AF_INET6, &a6->sin6_addr, ipv6str, sizeof(ipv6str));
            port = ntohs(a6->sin6_port);
            ipv4str[0] = 0;
          }
        }
      }

      // Check if tracked in your session manager
      const char* tracked = "no";
      for (int j = 0; j < MAX_ACTIVE_SESS; j++) {
        if (sessions[j].in_use && sessions[j].sock == sock) {
          tracked = "yes";
          break;
        }
      }

      // New: pull TCP state and unread buffer size
      const char* tcpState = getTcpState(sock);
      int pending = getPendingBytes(sock);

      html += "<tr>";
      html += "<td>" + String(i) + "</td>";
      html += "<td>" + String(sock) + "</td>";
      html += "<td>" + String(ipv4str) + "</td>";
      html += "<td>" + String(ipv6str) + "</td>";
      html += "<td>" + String(port) + "</td>";
      html += "<td>" + String(tracked) + "</td>";
      html += "<td>" + String(tcpState) + "</td>";
      html += "<td>" + String(pending) + "</td>";
      html += "</tr>";
    }
  }

  html += "</table>";

  html += appendTcpPcbsHtml();

  html += "<div style='display:flex; gap:20px; align-items:flex-start;'>";

  html += "<div style='flex:1; min-width:300px;'>";
  html += "<h2>Chip Info</h2><pre>";
  html += getChipInfoBlock();
  html += "</pre>";
  html += "</div>";

  html += "<div style='flex:1; min-width:300px;'>";
  html += "<h2>Memory</h2><pre>";
  html += getDiagnostics();
  html += "</pre>";
  html += "</div>";

  html += "</div>";

  html += "</body></html>";
  
  html += 
  "<li><a href=\"/taskman\">Task Manager</a></li><br>"
  "<p style=\"margin: 0;\">"
    "<a href=\"https://github.com/jameszah/ESP32-Task-Manager\" target=\"_blank\" "
       "style=\"color:#0078d4; text-decoration:none;\">"
       "Source Code on GitHub: <b>ESP32-Task-Manager 7.0</b>"
    "</a>"
  "</p>";


  httpd_resp_set_type(req, "text/html");
  return httpd_resp_sendstr(req, html.c_str());
}


//////////////////////////////////////////

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

      uint32_t curr = t->ulRunTimeCounter;
      uint32_t prev = tasks[idx].prevRunTime;

      // If runtime goes backward
      if (curr < prev) {
        uint32_t diff = prev - curr;

        if (diff > 0x0FFFFFFF) {
          //  Valid 32-bit counter rollover (huge backward jump)
          //    Let unsigned math handle it normally below
        } else {
          //  Small backward jump = bogus data from other core or race condition
          continue;
        }
      }

      // Compute delta normally — unsigned arithmetic handles rollover correctly
      uint32_t delta = curr - prev;

      //  Filter out absurdly large deltas (corrupted data)
      if (delta > 0x0FFFFFFF) continue;

      uint32_t deltaTask = delta;     // t->ulRunTimeCounter - tasks[idx].prevRunTime;
      tasks[idx].prevRunTime = curr;  // t->ulRunTimeCounter;

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

    sysSamples.freeRam[sysSamples.index] = ESP.getFreeHeap() / 1024;
    sysSamples.freePSRam[sysSamples.index] = ESP.getFreePsram() / 1024;
    sysSamples.index = (sysSamples.index + 1) % SAMPLE_COUNT;
  }
}

void FakeLoad1(void* pv) {
  uint32_t idleMs = 10000UL;
  //Serial.printf("FakeLoad1: Core 1, idle for %lu ms...\n", idleMs);
  vTaskDelay(pdMS_TO_TICKS(idleMs));

  for (;;) {
    uint32_t runSecs = random(1, 10);
    //Serial.printf("FakeLoad1: Core 1, running for %lu seconds...\n", runSecs);

    // Allocate PSRAM once per load phase (optional)
    uint8_t* psramBuffer = (uint8_t*)ps_malloc(2 * 1024 * 1024);  // 1 MB PSRAM
    if (psramBuffer) memset(psramBuffer, 0xAA, 2 * 1024 * 1024);

    uint8_t* ramBuffer = nullptr;
    size_t ramSize = 0;

    uint32_t start = millis();
    uint32_t lastIncrease = start;

    while ((millis() - start) < (runSecs * 1000UL)) {
      int j = 0;
      for (int i = 0; i < 200000; i++) {
        j = (j + 1) * -1;
      }
      if (j < 0) Serial.println("Fake Load 1!");
      // every 1 second: increase RAM allocation by 2 KB
      if (millis() - lastIncrease >= 1000) {
        lastIncrease = millis();
        ramSize += 5 * 1024;  // +2 KB
        uint8_t* newBuffer = (uint8_t*)heap_caps_realloc(ramBuffer, ramSize, MALLOC_CAP_INTERNAL);
        //uint8_t* newBuffer = (uint8_t*)realloc(ramBuffer, ramSize);
        if (newBuffer) {
          ramBuffer = newBuffer;
          memset(ramBuffer + ramSize - 5 * 1024, 0xAA, 5 * 1024);  // touch new region
          //Serial.printf("FakeLoad1: increased RAM to %u KB\n", ramSize / 1024);
        } else {
          Serial.println("FakeLoad1: realloc failed");
          break;  // stop increasing on failure
        }
      }

      vTaskDelay(1);
    }

    if (ramBuffer) free(ramBuffer);
    if (psramBuffer) free(psramBuffer);

    uint32_t idleMs = random(5, 20) * 1000UL;
    //Serial.printf("FakeLoad1: idle for %lu ms...\n", idleMs);
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

String getProgramName() {
  String path = __FILE__;
  int slash = path.lastIndexOf('/');
  if (slash < 0) slash = path.lastIndexOf('\\');  // handle Windows
  int dot = path.lastIndexOf('.');
  if (dot < 0) dot = path.length();
  return path.substring(slash + 1, dot);
}

esp_err_t taskman_handleRoot(httpd_req_t* req) {
  //String progName = getProgramName();
  String progName = PROGRAM_NAME;

  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <title>)rawliteral";
  html += progName;
  html += R"rawliteral( - ESP32 Task Manager</title>
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
<h2>ESP32 Task Manager - )rawliteral";
  html += progName;
  html += R"rawliteral(</h2>
  
  <canvas id="memChart" width="900" height="200" style="margin-top: 20px;"></canvas>
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
    <li><a href="/network">Network Info</a></li>
  </ul>
  <p style="margin: 0;">
    <a href="https://github.com/jameszah/ESP32-Task-Manager" target="_blank" 
       style="color:#0078d4; text-decoration:none;">
       Source Code on GitHub: <b>ESP32-Task-Manager 7.0</b>
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

let cpuChart, memChart;

let sampleCount = 100; // number of samples to keep on screen

function createChart() {
  // === CPU Chart ===
  const ctx = document.getElementById('cpuChart').getContext('2d');
  cpuChart = new Chart(ctx, {
    type: 'line',
    data: {
      labels: Array.from({ length: sampleCount }, (_, i) => i - sampleCount + 1),
      datasets: [] // You can push CPU datasets dynamically
    },
    options: {
      animation: false,
      responsive: true,
      layout: {
      padding: {
        right: 67   
      }
    },
        
  interaction: {
    mode: 'nearest',    // hover on nearest point, not just direct hit
    intersect: false,   // don't require pointer to be exactly on a line
    axis: 'x'           // hover works along x-axis
  },
      scales: {
        x: { title: { display: true, text: 'Seconds Ago' } },
        y: {
          beginAtZero: true,
          max: 100,
          title: { display: true, text: 'CPU %' }
        }
      },
      plugins: {
        legend: { position: 'bottom', labels: { boxWidth: 12 } }
      }
    }
  });

  // === Memory Chart ===
  const memCtx = document.getElementById('memChart').getContext('2d');
  memChart = new Chart(memCtx, {
    type: 'line',
    data: {
      labels: Array.from({ length: sampleCount }, (_, i) => i - sampleCount + 1),
      datasets: [
        {
          label: 'free RAM',
          yAxisID: 'yRam',
          borderColor: 'rgb(54, 162, 235)',
          backgroundColor: 'rgba(54, 162, 235, 0.2)',
          data: Array(sampleCount).fill(null),
          pointRadius: 0,       // <-- no points
          tension: 0.4      // <-- add this for smooth curves
        },
        {
          label: 'free PSRAM',
          yAxisID: 'yPsram',
          borderColor: 'rgb(255, 99, 132)',
          backgroundColor: 'rgba(255, 99, 132, 0.2)',
          data: Array(sampleCount).fill(null),
          pointRadius: 0,
          tension: 0.4      // <-- add this for smooth curves
        }
      ]
    },
    options: {
      animation: false,
      responsive: true,
      layout: {
      padding: {
        right: 0   
      }
    },
        
  interaction: {
    mode: 'nearest',    // hover on nearest point, not just direct hit
    intersect: false,   // don't require pointer to be exactly on a line
    axis: 'x'           // hover works along x-axis
  },
      scales: {
        x: { position: 'top',
        title: { display: true, text: 'Seconds Ago' } },
        yRam: {
          type: 'linear',
          position: 'left',
          beginAtZero: true,
          max: 300, // KB
          title: { display: true, text: 'RAM (KB)' }
        },
        yPsram: {
          type: 'linear',
          position: 'right',
          beginAtZero: true,
          max: 5000, 
          title: { display: true, text: 'PSRAM (KB)' },
          grid: { drawOnChartArea: false } // keep right axis separate
        }
      },
      plugins: {
        legend: { position: 'top', labels: { boxWidth: 12 } }
      }
    }
  });
}

let updating = false;
let stopCharts = false;

async function updateChartData() {
    if (updating) {
    //setTimeout(updateChartData, 1000);
    return;
  }

  if (stopCharts) return;      // <-- Do not run if stopped

  updating = true;
  
  try {
    const res = await fetch('/data');
    const json = await res.json();

    if (!cpuChart || !memChart) return;

    // ---- Update CPU chart ----
    Object.entries(json).forEach(([name, data], i) => {
      if (name === 'ram' || name === 'psram') return; // skip memory for now

      let ds = cpuChart.data.datasets.find(d => d.label === name);
      if (!ds) {
        ds = {
          label: name,
          data: data,
          borderColor: `hsl(${i * 70 % 360}, 70%, 50%)`,
          borderWidth: 1.5,
          fill: false,
          tension: 0.4,
          pointRadius: 0  
        };
        cpuChart.data.datasets.push(ds);
      } else {
        ds.data = data;
      }
    });
    cpuChart.update('none');

    // ---- Update memory chart ----
    if (json.ram && json.psram) {
      memChart.data.datasets[0].data = json.ram;
      memChart.data.datasets[1].data = json.psram;
      memChart.update('none');
    }

  } catch (err) {
    console.error('updateChartData failed:', err);
  }
    updating = false;
  setTimeout(updateChartData, 1000); // next tick
}


let charttimer;
let tabletimer;

async function startWork() {
  console.log("Starting work (tab visible)");
  stopCharts = false;
  updateChartData();
  updateTable();
  
  tabletimer = setInterval(updateTable, 30000);
}

function stopWork() {
  console.log("Stopping work (tab hidden)");
  //clearInterval(charttimer);
  stopCharts = true;        // <-- prevents future scheduling
  clearInterval(tabletimer);
}

document.addEventListener("visibilitychange", async () => {
  if (document.visibilityState === "hidden") stopWork();
  else await startWork();
});

function init(){
  createChart();
  startWork();
}

window.addEventListener("load", init);

async function updateTable() {
  try {
    const res = await fetch('/dataInfo');
    if (!res.ok) throw new Error("Fetch failed");
    const json = await res.json();
    const tbody = document.querySelector('#taskTable tbody');
    tbody.innerHTML = '';
    const stateNames = { 0: 'Running', 1: 'Ready', 2: 'Blocked', 3: 'Suspended', 4: 'Deleted' };
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
</script>
</body>
</html>
)rawliteral";

  httpd_resp_send(req, html.c_str(), HTTPD_RESP_USE_STRLEN);
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

///
esp_err_t taskman_handleData(httpd_req_t* req) {
  httpd_resp_set_type(req, "application/json");

  char buf[1024];
  size_t off = 0;

// Helper: append and flush when needed
#define FLUSH_IF_FULL() \
  if (off >= sizeof(buf) - 64) { \
    httpd_resp_send_chunk(req, buf, off); \
    off = 0; \
  }

#define APPEND(fmt, ...) \
  do { \
    off += snprintf(buf + off, sizeof(buf) - off, fmt, ##__VA_ARGS__); \
    FLUSH_IF_FULL(); \
  } while (0)

  // Start JSON
  APPEND("{");

  bool firstItem = true;

  // ---- Per-task CPU history ----
  for (int i = 0; i < maxtaskCount; i++) {
    if (!tasks[i].over2) continue;

    if (!firstItem) APPEND(",");
    firstItem = false;

    // Task name
    APPEND("\"%.64s\":[", tasks[i].name);

    // History samples
    int pos = tasks[i].index;
    for (int j = 0; j < SAMPLE_COUNT; j++) {
      APPEND("%.1f", tasks[i].usage[pos]);
      if (j < SAMPLE_COUNT - 1) APPEND(",");
      pos = (pos + 1) % SAMPLE_COUNT;
    }

    APPEND("]");
  }

  // ---- System RAM history ----
  if (!firstItem) APPEND(",");
  APPEND("\"ram\":[");

  for (int i = 0; i < SAMPLE_COUNT; i++) {
    int pos = (sysSamples.index + i) % SAMPLE_COUNT;
    APPEND("%u", sysSamples.freeRam[pos]);
    if (i < SAMPLE_COUNT - 1) APPEND(",");
  }

  APPEND("],");

  // ---- System PSRAM history ----
  APPEND("\"psram\":[");

  for (int i = 0; i < SAMPLE_COUNT; i++) {
    int pos = (sysSamples.index + i) % SAMPLE_COUNT;
    APPEND("%u", sysSamples.freePSRam[pos]);
    if (i < SAMPLE_COUNT - 1) APPEND(",");
  }

  APPEND("]");

  // End JSON
  APPEND("}");

  // Flush leftover
  if (off) httpd_resp_send_chunk(req, buf, off);

  // Terminate chunks
  httpd_resp_send_chunk(req, NULL, 0);

  return ESP_OK;
}

/////////////
void printTopTasksOneLine() {
  struct Item {
    const char* name;
    float usage;
    int core;
  };

  Item list[maxtaskCount];
  int count = 0;

  // ---- Collect tasks ----
  for (int i = 0; i < maxtaskCount; i++) {
    if (!tasks[i].over2) continue;

    // ✅ Skip IDLE tasks
    if (tasks[i].name == "IDLE0" || tasks[i].name == "IDLE1") continue;

    int pos = (tasks[i].index - 1 + SAMPLE_COUNT) % SAMPLE_COUNT;
    float u = tasks[i].usage[pos];

    list[count].name = tasks[i].name.c_str();
    list[count].usage = u;
    list[count].core = tasks[i].core;
    count++;
  }

  // ---- Sort by usage descending ----
  for (int i = 0; i < count - 1; i++) {
    for (int j = i + 1; j < count; j++) {
      if (list[j].usage > list[i].usage) {
        Item tmp = list[i];
        list[i] = list[j];
        list[j] = tmp;
      }
    }
  }

  // ---- Free RAM ----
  uint32_t ramKB = ESP.getFreeHeap() / 1024;
  uint32_t psramKB = ESP.getFreePsram() / 1024;

  // ---- Print single line ----
  Serial.printf("RAM=%uKB PSRAM=%uKB | ", ramKB, psramKB);

  int limit = (count < 4) ? count : 4;
  for (int i = 0; i < limit; i++) {
    if (list[i].usage <= 1.0) continue;
    Serial.printf("%s:%.1f[%d]", list[i].name, list[i].usage, list[i].core);
    if (i < limit - 1) Serial.print(", ");
  }

  Serial.println();
}

void taskman_setup() {
  int start_free = ESP.getFreeHeap();
  xTaskCreatePinnedToCore(cpuMonitorTask, "CPU_Monitor", 1000, nullptr, 7, nullptr, 0);
  vTaskDelay(pdMS_TO_TICKS(10));

  Serial.println("\nhttps://github.com/jameszah/ESP32-Task-Manager\n");

  Serial.printf("Taskman setup complete, used %d bytes of ram, current free %d\n", start_free - ESP.getFreeHeap(), ESP.getFreeHeap());
}

void taskman_server_setup(httpd_handle_t existing_server = nullptr) {
  int start_free = ESP.getFreeHeap();

  httpd_handle_t server = existing_server;

  if (server == nullptr) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 81;
    config.ctrl_port = 32770;
    config.stack_size = 6 * 1024;    // optional tweak
    config.lru_purge_enable = true;  // auto-clean old sockets
    config.max_open_sockets = 8;     // safer defaults
    config.recv_wait_timeout = 5;
    config.send_wait_timeout = 5;

    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
      Serial.printf("❌ Failed to start Taskman server (err=%d)\n", err);
      return;
    } else {
      Serial.println("✅ Taskman server started");
    }
  } else {
    Serial.println("♻️  Reusing existing Taskman server handle");
  }
  
/*
  httpd_uri_t uri_data = { .uri = "/data", .method = HTTP_GET, .handler = taskman_handleData, .user_ctx = nullptr };
  httpd_register_uri_handler(server, &uri_data);
*/

#define REGISTER_TRACKED(uri_str, fn) \
  do { \
    static httpd_uri_t u = { \
      .uri = uri_str, \
      .method = HTTP_GET, \
      .handler = tracked_handler, \
      .user_ctx = (void*)fn \
    }; \
    httpd_register_uri_handler(server, &u); \
  } while (0)

  REGISTER_TRACKED("/network", taskman_handleNetwork);
  REGISTER_TRACKED("/data", taskman_handleData);
  REGISTER_TRACKED("/taskman", taskman_handleRoot);
  REGISTER_TRACKED("/dataInfo", taskman_handleDataInfo);

/*
httpd_uri_t uri_data = {.uri = "/data",  .method = HTTP_GET, .handler = tracked_handler, .user_ctx = (void*)taskman_handleData };
httpd_register_uri_handler(server, &uri_data);
*/

  // --- Save or return handle ---
  taskman_server = server;

  Serial.printf("Taskman server setup complete, used %d bytes of RAM, free: %d\n",
                start_free - ESP.getFreeHeap(), ESP.getFreeHeap());
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
}