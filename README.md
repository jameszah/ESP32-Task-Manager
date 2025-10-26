# ESP32-Task-Manager
Display a rolling graph of Task CPU usage inside an ESP32
- any Arduino program with wifi, just include the taskman.h, call taskman_setup() in the setup(), and it will serve a graph at port 81 of your ip as below with 100 seconds of 1 second averages of all the tasks above 2% utilization on your esp32
  

<img width="757" height="471" alt="image" src="https://github.com/user-attachments/assets/1584f631-f67e-4046-9ba9-c6e75f259b0d" />


https://www.reddit.com/r/esp32/comments/1oeq3v6/whats_happening_inside_my_esp32/

---
Good stuff not included
- killing tasks
- changing priorities
  
