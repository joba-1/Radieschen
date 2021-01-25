# Radieschen

firmware for ESP8266 to count radiation events with a geiger-müller-counter

## Hardware
Connect RadiationD-v1.1 (CAJOE) header P3-Gnd with ESP-Gnd, P3-5V with ESP-5V and P3-Vin with ESP-D2

Note: P3-Vin has 3.3V level on my board. Ideal for ESP8266, no level shifter needed. Strange pin name, but it really is the interrupt line (falling edge).

STL for 3d-printing a support for a D1mini will follow soon.

## Database
Connect to InfluxDB is easy: modify influx server and port in platformio.ini and create the database on the server:

    job4:~ > influx
    Connected to http://localhost:8086 version 1.8.0
    InfluxDB shell version: 1.8.0
    > create database Radieschen
