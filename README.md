Inspiration from several projects:
Somfy:
https://github.com/PBearson/ESP32-With-ESP-PROG-Demo  (If you need a full-featured Somfy on ESP32, use this.)
CC1101:
https://github.com/LSatan/SmartRC-CC1101-Driver-Lib
https://github.com/mfurga/cc1101/blob/main/cc1101.cc

https://github.com/nopnop2002/esp-idf-cc1101/blob/main/components/cc1101/cc1101.c

and the TI datasheet at https://www.ti.com/lit/ds/symlink/cc1101.pdf?ts=1704109305538


Flashing: 
 - If flashing doesn't find the device on the COM port, hold down the BOOT button while initiating flash.
 - If holding BOOT got it to work, but then it fails with "Failed to communicate with flash chip", try removing the pin 12 connection during flash.

COM port not detected.
For HiLetGo WROOM-32, the COM port driver seems to be CP210x, so install that and NOT FTDI, otherwise there will be no COM port.
https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/establish-serial-connection.html

Debugging:
not sure how to use openocd with ESP32-WROOM yet.
Some ESP32 chips have built-in JTAG, but HiLetGo's WROOM-32S doesn't seem to have it. So you need ESP-PROG for it. 
https://github.com/PBearson/ESP32-With-ESP-PROG-Demo