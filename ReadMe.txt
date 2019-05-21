/*
Author: Roger Philp
Date: 25/03/19
This program is one aof a pair of programs to transfer files between an Arduino Mega SD card
and a host linux system or bash shell under windows. 
Transfer is bidirectional: to the host and from the host, 
with a very simplistic command line interface.

The complimentary programs that needs to be loaded on to the Arduino is:

HostSeriaPport_v4_crc32
ArduinoSerialPort_v4_crc32



The program connects to:
 	char *portname = "/dev/ttyS5";
	which is comm port 5
	baudrate 115200, 8 bits, no parity, 1 stop bit
	
Transfers occur with 32bit crc checking

*/