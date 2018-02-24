# co2sensorsw

This directory contains software for reading a certain type of (relatively) cheap USB connected CO2 sensors.
These sensors are available in Germany from TFA Dostmann as "Aircontrol Mini CO2-Monitor" (Kat.-Nr. 31.5006) and in various other countries under numerous different labels.
In some countries these are sold with the premise that they can be read via USB, and they come with software to do that. The german ones from TFA are not, they only claim to use USB for power, but they still show up as USB HID devices, and because someone reverse-engineered the weird protocol they use, they can be read.
The software in this directory will do that. It contains:

## co2sensord.c

This is a little daemon written in C that makes the data received from the CO2-sensor available on a network-socket.
There is a simple Makefile included, so in most cases it should be enough to call `make` to get a usable binary.

## 38_co2mini.pm

This is a module for FHEM. It is not really my work:
It's mostly the original FHEM-module from Henryk Ploetz available from
https://github.com/henryk/fhem-co2mini with some patches from one of its forks,
https://github.com/verybadsoldier/fhem-co2mini and minor patches from me.
