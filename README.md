# ESP32-Task-Manager
Display a rolling graph of Task CPU usage inside an ESP32
- any Arduino program with wifi, just include the taskman.h, call taskman_setup() in the setup(), and it will serve a graph at port 81 of your ip as below with 100 seconds of 1 second averages of all the tasks above 2% utilization on your esp32
- with 2 cores on the esp32-s (on the ai thinker esp32-cam), the percentages will add to 200%
- it records 100 seconds inside the esp32, but only sends 1-second updates to the graph.
- you can hit refresh to get the full 100 seconds and get rid of any wifi delays on a 1 second update, such as that wobble if the sinewave of fake load below
- also below the moving graph is a snapshot of your tasks, priority, heap highwater mark
- the cpu_monitor runs at prio 7 on core 0 (with wifi and arduino housekeeping), then the web server is prio 5, and wifi 23, so tasks above prio 7 can interfere with data collection if they don't let cpu_monitor run

<img  alt="image" src="https://github.com/jameszah/ESP32-Task-Manager/blob/main/taskman4.5.png" />

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
### Other info
Good stuff not added yet
- killing tasks
- changing priorities

### Endpoints
The endpoints are below - the esp32 keeps track of 100 points, and will deliver that entire series for every task that every exceeded 2% of its core, or for the current data you can just get the last second snapshot of every 2% plus task.  The data collector only runs once per second, so 2 fetchs in a second will give you the same data. 

http://192.168.0.152:81/dataInfo

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

http://192.168.1.111:81/dataCurrent  
{
  "loopTask": 3.1,
  "IDLE1": 96.6,
  "IDLE0": 97.8,
  "BusyTask1": 0,
  "BusyTask0": 0,
  "httpd": 0.3,
  "wifi": 1.4
}

http://192.168.1.111:81/data  
{
  "loopTask": [3.1, 3.1, 3.1, 3.1, 3.1, 3.1, 2.6, 1.8, 1.8, 2.3, 3.1, 3.1, 3.1, 3.1, 3.1, 3.1, 2.9, 2.7, 2.7, 2.7, 2.7, 2.7, 2.7, 2.9, 3.1, 3.1, 3.1, 3.1, 3.1, 3.1, 3.1, 3.1, 3.1, 3.1, 3.1, 3.1, 3.1, 2.9, 2.7, 2.7, 2.7, 2.3, 1.8, 1.8, 1.8, 1.8, 2.5, 3.1, 3.1, 3.1, 3.1, 3.1, 3.1, 3.1, 3.1, 3.1, 3.1, 3.1, 3.1, 3.1, 3.1, 2.9, 2.7, 2.7, 2.7, 2.3, 2.6, 3.1, 3.1, 3.1, 3.1, 2.3, 2.6, 3.1, 3.1, 3.1, 3.1, 3, 3.1, 2.3, 1.8, 1.8, 2.3, 2.7, 2.7, 2.7, 2.9, 3.1, 3.1, 3.1, 3.1, 3.1, 3.1, 3.1, 3.1, 3.1, 3.1, 3.1, 3.1, 3.1],
  "IDLE1": [96.9, 96.9, 96.9, 96.5, 96.9, 96.9, 58.7, 2.4, 2.3, 40.9, 96.9, 96.9, 96.9, 96.9, 96.9, 96.9, 61, 13.1, 13.5, 13.5, 13.5, 13.5, 13.5, 50.9, 96.9, 96.9, 96.9, 96.9, 96.9, 96.9, 96.9, 96.9, 96.9, 96.9, 96.9, 96.9, 96.6, 56, 13.1, 13.2, 13.2, 8.9, 2.3, 2.6, 2.4, 2.4, 51.9, 96.9, 96.9, 96.9, 96.9, 96.9, 96.9, 96.9, 96.9, 96.9, 96.9, 96.9, 96.9, 96.9, 96.9, 49.8, 13.2, 13.2, 13.2, 8.1, 56.7, 96.9, 96.5, 96.3, 96.6, 41.3, 57.9, 96.3, 96.6, 96.6, 96.1, 93.4, 96.6, 39.3, 2.7, 2.4, 8.6, 13.1, 13.2, 13.1, 65.9, 96.6, 96.6, 96.6, 96.6, 96.6, 96.6, 96.6, 96.6, 96.6, 96.6, 96.6, 96.6, 96.6],
  ... etc ...
  "wifi": [1.8, 1.6, 1.4, 2.8, 1.8, 1.2, 1.3, 1.4, 1.4, 1.5, 1.3, 1.2, 1.4, 1.7, 2.4, 1.9, 1.7, 1.4, 1.2, 1.9, 1.7, 1.5, 1.8, 1.3, 1.2, 1.3, 1.3, 1.5, 1.6, 1.7, 1.4, 1.2, 1.1, 1.4, 1.1, 1, 1.8, 1.1, 0.9, 1.4, 1.1, 1.1, 1.5, 1.1, 1.1, 1.3, 1.1, 1.2, 1.2, 1.6, 1.4, 1.2, 1.3, 1.4, 1.2, 1.3, 1.3, 1.1, 1, 1.1, 1.3, 1.1, 0.9, 1.4, 1.1, 1.2, 1.2, 1.1, 1.7, 2.1, 1.8, 1.6, 1.8, 2.1, 1.7, 1.8, 2.2, 1.4, 1.9, 2.4, 1.9, 2, 1.8, 1.6, 1.7, 1.8, 1.7, 1.7, 2, 2.5, 2, 1.6, 1.9, 2, 1.6, 1.5, 1.7, 1.6, 2.5, 3]
}
