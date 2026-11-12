This was an attempt to implement a nice CC1101 driver library for my Somfy blinds, using  ESP-IDF. An attempt was also made with Arduino ,but I could not get a CC1101 to receive data using this code, so I eventually gave up. 


Inspiration/example code from several projects:

- Somfy: https://github.com/rstrouse/ESPSomfy-RTS (If you need a full-featured Somfy on ESP32, use this. It's a solid project, frequently updated)

- https://github.com/PBearson/ESP32-With-ESP-PROG-Demo 

CC1101 implementations: 

- https://github.com/LSatan/SmartRC-CC1101-Driver-Lib

- https://github.com/mfurga/cc1101/blob/main/cc1101.cc

- https://github.com/nopnop2002/esp-idf-cc1101/blob/main/components/cc1101/cc1101.c

and the TI datasheet at https://www.ti.com/lit/ds/symlink/cc1101.pdf?ts=1704109305538

SPI Access: https://www.ti.com/lit/an/swra112b/swra112b.pdf?ts=1726759618047&ref_url=https%253A%252F%252Fe2e.ti.com%252F

design note: https://www.ti.com/lit/an/swra215e/swra215e.pdf?ts=1726734847449 


Flashing: 
 - If flashing doesn't find the device on the COM port, hold down the BOOT button while initiating flash. This is caused by a bad driver and I haven't found a driver to fix the issue. 
 - If holding BOOT got it to start flashing, which then fails with "Failed to communicate with flash chip", try removing  pin 12 connection during flash. If that fixes the issue, consider moving the connection to another pin.

COM port not detected:

For HiLetGo WROOM-32, the COM port driver seems to be CP210x, so install that and NOT FTDI, otherwise there will be no COM port.

https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/establish-serial-connection.html

Debugging:

not sure how to use openocd with ESP32-WROOM yet.
Some ESP32 chips have built-in JTAG, but HiLetGo's WROOM-32S doesn't seem to have it. So you need ESP-PROG for it. 
https://github.com/PBearson/ESP32-With-ESP-PROG-Demo


For Arduino, copy the files under components into a subdirectory called "src" under esp32-main
