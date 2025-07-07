| Supported Targets | ESP32 | ESP32-C3 | ESP32-C6 | ESP32-S2 | ESP32-S3 |
| ----------------- | ----- | -------- | -------- | -------- | -------- |

# ESP-WIFI-MESH TCP Traffic High Datarate Throughput Test

This Branch implements an TCP protocol Throughput Test for the ESP-WIFI-MESH network. This was part of the final project for my graduation in Electrical Engineering Bachelor Course.
More info about the network can be found at Espressif ESP-IDF Docs, mainly at ESP-WIFI-MESH and ESP Netif.
A .pdf file is included at the main branch with the complete Thesis in portuguese.

## Functionality

The setup included 3 esp32 nodes, with one beeing a root fixed node UDP/TCP server and the others beeing layer 2 and layer 3 client nodes. The nodes send/receive packets to/from the server for 120 seconds. After that, the information of how many Bytes were sent/received at each node is displayed at the root. The project also includes a PING function to aquire the latency from the network links.

### Configure the project

Open the project configuration menu (`idf.py menuconfig`) to configure the mesh network channel, router SSID, router password and mesh softAP settings.
The communication ports and layer of the node can be edited here as well. The node can be configured as an UDP/TCP server or client.

### Build and Flash

Build the project and flash it to multiple boards forming a mesh network, then run monitor tool to view serial output:

```
idf.py -p PORT flash monitor
```

(To exit the serial monitor, type ``Ctrl-]``.)

See the Getting Started Guide for full steps to configure and use ESP-IDF to build projects.
