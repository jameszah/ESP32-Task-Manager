# ESP32-Task-Manager
Display a rolling graph of Task CPU usage inside an ESP32
- any Arduino program with wifi, just include the taskman.h, call taskman_setup() and taskman_server_setup() in the setup(), and it will serve a graph at port 80 or 81 of your ip as below with 100 seconds of 1 second averages of all the tasks above 2% utilization on your esp32
- you can call taskman_server_setup() and it will setup a webserver you can access at 192.168.1.111:81/taskman
- OR you can call taskman_server_setup(sever), and it we use your existing httpd server at 192.168.1.111/taskman (saving 10kb or memory)
- also 192.168.1.111/network gives some internal information about the webserver, sockets, memory, etc (see below)
- with 2 cores on the esp32-s (on the ai thinker esp32-cam), the percentages will add to 200%
- it records 100 seconds inside the esp32 and updates the web graph every second
- also below the moving graph is a snapshot of your tasks, priority, heap highwater mark, update every 30 seconds
- the cpu_monitor runs at prio 7 on core 0 (with wifi and arduino housekeeping), then the web server is prio 5, and wifi 23, so tasks above prio 7 can interfere with data collection if they don't let cpu_monitor run
- the /dataCurrnet endpoint was removed as the 1 second predictabily of the wifi was a little randon, so now everything is recorded quickly inside the esp32 memory, and you get a full graph every second
- a second graph was added to keep track of ram and psram
- taskman_setup() starts the recording so you can put at beginning of setup() to keep track of memory and cpu while the setup is running, and only run taskman_server_setup() later when you have the wifi and the webserver turned on, but you can look backwards 10 or 30 seconds to see how your setup() behaves
- it can also run at 1 sample per second, or with #define SAMPLE_RATE_HZ, you can change that to 2, 4, or 8 samples per second, still with 100 samples
- I find the averaging over a 1 second smooths out graphs, as even activity on a 1 second frequency will happen with an 1/8th of a second, so graphs as constantly moving 100% to 0% and back

<img  alt="image" src="https://github.com/jameszah/ESP32-Task-Manager/blob/main/taskman7.png" />
<img  alt="image" src="https://github.com/jameszah/ESP32-Task-Manager/blob/main/taskman7net.png" />

### Initial Mention
https://www.reddit.com/r/esp32/comments/1oeq3v6/whats_happening_inside_my_esp32/

---
### Get Started
- download the .ino and the .h
- edit your ssid and password into the .ino file
- install on esp32
- goto your ip address with port 81, like 192.168.1.111:81
- then add your own code, and delete the lines that produce the fake load on core 0, core 1, and inside the loop
---
### Add to an existing project
Your own code needs wifi, and these two lines:

```
#define PROGRAM_NAME "replace with program name"
#include "taskman.h"       //  <--- the important bit

void setup(){
  taskman_setup();         //  <--- the important bit

  // Option 1 -- taskman on port 81
  taskman_server_setup();  //  <--- the important bit
  
  // Option 2 - taskman on port 80 along with all your own endpoints
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  httpd_handle_t mainServer = NULL; 
  httpd_start(&mainServer, &config);
  taskman_server_setup(mainServer);  <--- the important bit
}
```
And then access the taskmanager display with 192.168.1.111:81/taskman or 192.168.1.111:80/taskman
Your ip address, and PORT 81 or 80

---
### Other info
Good stuff not added yet
- killing tasks
- changing priorities

### Endpoints
The endpoints are below - the esp32 keeps track of 100 points, and will deliver that entire series for every task that every exceeded 2% of its core, or for the current data you can just get the last second snapshot of every 2% plus task.  The data collector only runs once per second, so 2 fetchs in a second will give you the same data. 

http://192.168.1.111:81/dataInfo

{
  "CPU_Monitor": {
    "core": 0,
    "prio": 7,
    "stackHW": 620,
    "state": 0
  },
  "IDLE1": {
    "core": 1,
    "prio": 0,
    "stackHW": 572,
    "state": 1
  },
  "IDLE0": {
    "core": 0,
    "prio": 0,
    "stackHW": 464,
    "state": 1
 } ... etc ...
  "Tmr Svc": {
    "core": 2147483647,
    "prio": 1,
    "stackHW": 3608,
    "state": 2
  }
}


http://192.168.1.111:81/data  
{
  "loopTask": [3.1, 3.1, 3.1, 3.1, 3.1, 3.1, 2.6, 1.8, 1.8, 2.3, 3.1, 3.1, 3.1, 3.1, 3.1, 3.1, 2.9, 2.7, 2.7, 2.7, 2.7, 2.7, 2.7, 2.9, 3.1, 3.1, 3.1, 3.1, 3.1, 3.1, 3.1, 3.1, 3.1, 3.1, 3.1, 3.1, 3.1, 2.9, 2.7, 2.7, 2.7, 2.3, 1.8, 1.8, 1.8, 1.8, 2.5, 3.1, 3.1, 3.1, 3.1, 3.1, 3.1, 3.1, 3.1, 3.1, 3.1, 3.1, 3.1, 3.1, 3.1, 2.9, 2.7, 2.7, 2.7, 2.3, 2.6, 3.1, 3.1, 3.1, 3.1, 2.3, 2.6, 3.1, 3.1, 3.1, 3.1, 3, 3.1, 2.3, 1.8, 1.8, 2.3, 2.7, 2.7, 2.7, 2.9, 3.1, 3.1, 3.1, 3.1, 3.1, 3.1, 3.1, 3.1, 3.1, 3.1, 3.1, 3.1, 3.1],
  "IDLE1": [96.9, 96.9, 96.9, 96.5, 96.9, 96.9, 58.7, 2.4, 2.3, 40.9, 96.9, 96.9, 96.9, 96.9, 96.9, 96.9, 61, 13.1, 13.5, 13.5, 13.5, 13.5, 13.5, 50.9, 96.9, 96.9, 96.9, 96.9, 96.9, 96.9, 96.9, 96.9, 96.9, 96.9, 96.9, 96.9, 96.6, 56, 13.1, 13.2, 13.2, 8.9, 2.3, 2.6, 2.4, 2.4, 51.9, 96.9, 96.9, 96.9, 96.9, 96.9, 96.9, 96.9, 96.9, 96.9, 96.9, 96.9, 96.9, 96.9, 96.9, 49.8, 13.2, 13.2, 13.2, 8.1, 56.7, 96.9, 96.5, 96.3, 96.6, 41.3, 57.9, 96.3, 96.6, 96.6, 96.1, 93.4, 96.6, 39.3, 2.7, 2.4, 8.6, 13.1, 13.2, 13.1, 65.9, 96.6, 96.6, 96.6, 96.6, 96.6, 96.6, 96.6, 96.6, 96.6, 96.6, 96.6, 96.6, 96.6],
  ... etc ...
  "wifi": [1.8, 1.6, 1.4, 2.8, 1.8, 1.2, 1.3, 1.4, 1.4, 1.5, 1.3, 1.2, 1.4, 1.7, 2.4, 1.9, 1.7, 1.4, 1.2, 1.9, 1.7, 1.5, 1.8, 1.3, 1.2, 1.3, 1.3, 1.5, 1.6, 1.7, 1.4, 1.2, 1.1, 1.4, 1.1, 1, 1.8, 1.1, 0.9, 1.4, 1.1, 1.1, 1.5, 1.1, 1.1, 1.3, 1.1, 1.2, 1.2, 1.6, 1.4, 1.2, 1.3, 1.4, 1.2, 1.3, 1.3, 1.1, 1, 1.1, 1.3, 1.1, 0.9, 1.4, 1.1, 1.2, 1.2, 1.1, 1.7, 2.1, 1.8, 1.6, 1.8, 2.1, 1.7, 1.8, 2.2, 1.4, 1.9, 2.4, 1.9, 2, 1.8, 1.6, 1.7, 1.8, 1.7, 1.7, 2, 2.5, 2, 1.6, 1.9, 2, 1.6, 1.5, 1.7, 1.6, 2.5, 3]
}
