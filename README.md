# Modbus tools for Solis 5g solar inverters

This is a collection of some applications, tools and utilities I've put together for firstly investigating then retrieving information from a Solis 5G solar inverter via Modbus.

## !! Check your DataLogger version !!

It's transpired that different versions of firmware in the datalogger behave radically differently, as a result you may NOT see the same behaviour I describe below with your setup. As such I can only confirm the [modbus-solis-broadcast](#modbus-solis-broadcast) and [modbus-esp32](#modbus-esp32) apps work against firmware version **13230**. Likewise the [modbus-slave](#modbus-slave) app will be mimicing the behaviour of that same version of datalogger. As such I strongly recommend using the [modbus-sniffer](#modbus-sniffer) app first to ascertain the exact behaviour of your datalogger.

It _may_ also work with older and newer (if they exist) versions however no guarantees. Also check my branches as there may be a variant that is a closer match to your firmware than current.

## Background

Solis 5G inverters allow for the connection of a Wifi dongle which allows information to be uploaded to the Solis cloud servers (in China). In turn, this information can be retrieved via the [SolisCloud website](https://www.soliscloud.com) and/or via the app.

Additionally, Solis provide [an API](https://solis-service.solisinverters.com/en/support/solutions/articles/44002212561-api-access-soliscloud) which allows you to retrieve this same data programatically sent over https. This is packaged up as JSON encoded data which I've made use of for some time in order to retrieve both historical and current data, the latter of which I've had presented on various displays, including [@zx85's solar-display-micropython project](https://github.com/zx85/solar-display-micropython)

<a name="udp-broadcast"></a>
One significant difference is that I've taken out the functionality from the solar displays which directly interface to the Solis API. Instead, they pick the data up from UDP packets, broadcast over the local network. I therefore only have a [single application which retrieves the data, then pushes it out periodically](https://github.com/fridgemagnet3/solar-display-micropython/tree/main/solis-broadcast) - essentially it just retransmits a subset of the JSON data, as received from Solis. It needed to be a subset because the payload is well in excess of a single datagram and the ESP32 (as used in [@zx85's display](https://github.com/zx85)) IP stack can't handle UDP fragmentation. This then means I can also easily swap out where the data comes from - the Solis cloud or (as is [described later](#modbus-solis-broadcast)) locally from the inverter. 

[/fridgemagnet3/solar-display-micropython](https://github.com/fridgemagnet3/solar-display-micropython) is my fork of @zx85's solar display which receives it's data via UDP instead of directly from the Solis cloud API.

The aim of this project was to bypass the need to go via the Solis Wifi dongle, cloud and API and instead obtain the current data directly from the inverter. Aside from cutting out these additional steps, the dongle only updates the cloud data every 5 minutes and I wanted to refresh data at a much higher rate, at least every minute. This also then offers better response times for making decisions based around the data, for example using surplus energy to heat our hot water rather than exporting it, within the current arrangement, it's entirely feasible you could end up importing energy should the sun go behind a cloud seconds after the last set of data is pushed up to the cloud.

An additional requirement though was, (unlike a number of other articles which have explored this) to continue to allow the Wifi dongle to remain in play. Hence whatever I came up with would need to cooperate with it.

## Solis Cloud Control API
If you use the Solis Cloud Control API for controlling your inverter (eg. via the Solis app or HA component), there is evidence to suggest those requests are actioned immediately by the datalogger ie. outside of the normally polling cycle. As such, there is scope for these to then fail if they happen to occur when _modbus-solis-broadcast_ (or the ESP equivalant) is performing a transction. Retrying the request immediately afterwards should then succeed because those apps are designed to 'back off' under those conditions and re-sync with the next datalogger polling cycle however only limited testing has been performed in this area.

## Interfacing overview
The physical interface between the inverter and the Wifi dongle is a serial RS485 interface, using a [Modbus RTU](https://en.wikipedia.org/wiki/Modbus) protocol. It connects via a 4 pin, Exceedconn EC04681-2023BF (or 2014BF) connector which provides a 5V supply to the dongle and the RS485 differential pair.

Modbus is a client/server architecture, the dongle is the master (it issues the requests) and the inverter the slave (it responds to them). That immediately poses a challenge because in order to issue my own requests, I need to also be a master which generally isn't how Mobus works. [This thread](https://community.openenergymonitor.org/t/getting-data-from-inverters-via-an-rs485-connection/8377) was a useful read and if you make it to the end (it's quite long!), you'll see this problem being raised, with one of the contributers indicating issues with collisions between his kit in the dongle. The conclusion to this ends up suggesting having two RS485 interfaces, with whatever kit you end up using acting as a proxy between the dongle and inverter. To me this felt like massive overkill, overly complex and fraught with the potential for timing issues between the two bits of kit at each end. My working presmise was therefore that if the wifi dongle is acting in a predictable manner, knowing that in advance, it should be possible to minimize collisions.

RS485 is "multi-drop" meaning you can hang multiple things off the bus. My plan was therefore to wire up a cable, with plug/socket at each end, this carries the existing connection to/from the wifi dongle. I would then hang an additional RS485 interface from mid way along the cable. It's worth reading up on RS485, there are various rules about how things should be connected, terminated and so on. Shielded twisted pair cabling is recommended, I've seen it suggested that CAT-5 cable will work over short distances however to avoid any issues, I went out and bought some proper cabling.

The project was constructed in two parts - the first, the "proof of concept" used a RS485/USB adaptor connected to a Raspberry Pi (pictured below). I used this to investigate the protocol then prototype the software.

![20241103_180441](https://github.com/user-attachments/assets/29c87abe-2c4b-43d1-8a9e-ae0e4fc55c1a)

The second part replaced this with an ESP32 microcontroller, powered of the 5V supply from the inverter.

![20250301_170933](https://github.com/user-attachments/assets/4aea6824-fb2a-4ca0-a5db-8a98bb198143)

Thanks to [@zx85](https://github.com/zx85) for the 3D printed case.

![PXL_20250505_122722543 MACRO_FOCUS](https://github.com/user-attachments/assets/8b201774-31a7-448d-b5b4-102050755da1)

### Spurious characters
In my setup, the inverter/datalogger can induce spurious characters on the serial line probably when the transmitters are being turned on or off. My RS485/USB adapter that I used during the first part of the project seemed particularly prone to this. This seems to materialise as up to 3 NULL bytes which I believe are actually _serial break_ characters (or framing errors). For directly attached devices, you can often filter these out via _stty_ settings however with a USB bridge in the way, they are treated as actual NULL bytes. 

Directly attached RS485 adapters (eg. the MAX3485 I use with the ESP-32) seemed less prone to this however I still get the odd random character appear. In the process of investigating this, I've tried adding in termination resistors (to the connectors at both end of the cable since I don't believe either the interter or logger has them) and bias reistors (even though I don't believe the MAX devices need them). Neither of which has made any difference, which based on the behaviour I was seeing, frankly didn't think it would. 

As a result, I've had to implement software workarounds, in effect to discard any incoming bytes up until the expected start of response sequence is detected. In the case of the Linux applications, this is in the form of a patch against the current. 3.1.11 release of the libmodbus library, this can be found in the [libmodbus folder](libmodbus/). For the ESP-32 module, I've created a fork of the [ModbusMaster](https://github.com/fridgemagnet3/ModbusMaster) library.

## Datalogger reset
Every 12 hours, the datalogger appears to perform some form of reset/restart sequence. Additionally, over time the reset point may move (possibly to even out load on Solis's servers) so you may see a cycle time less than 12 hours from time to time. After the reset event (and immediately after power on), the logger performs a sustained series of repeated register reads, [slave polls](#modbus-solis-broadcast) which may occupy the bus constantly anywhere from 15-30 minutes. [data/logger-reset.ods][data/logger-reset.ods] shows an example of this behaviour. 

The [modbus-solis-broadcast](#modbus-solis-broadcast) app (and [ESP-32 equivalent](#modbus-esp32)) are both designed to try and detect and then suspend initiating transactions whilst the logger does it's thing (whatever that may be). This of course means that for the duration, no (or limited) updates will be performed by the software. However given the unpredictable nature of when this reset may occur, there is scope for odd things to occur in this time period, historically the datalogger has shown itself to be senstive to other unexpected bus traffic occurring during this period.

## ESP-32 Module
For the finished product, I replaced the Raspberry Pi with an ESP32 WROOM-32 module. These are inexpensive, nifty little microcontrollers which have a bunch of I/O (including serial) plus built in Wifi. 

This was my initial test setup. 

![20241231_195839](https://github.com/user-attachments/assets/0fdb9c91-805f-46f1-a657-924b7de08527)

It's connected to the RS232 port of a Raspberry Pi running my [modbus-slave](#modbus-slave) app which simulates the behaviour of the inverter and wifi logger. As both boards use 3.3V logic levels, this provides a simple environment for me to develop & test the software for the ESP32. To date, I've now managed to successfully port across the [modbus-solis-broadcast](#modbus-solis-broadcast) application which is now fully working in the simulated environment. The next step will be to replace the RS232 link with RS485.

### RS-485

The main addition to my test setup is the presence of a [MAX3485 RS-485 transceiver](https://www.analog.com/en/products/max3485.html), this basically converts the 3.3V serial from the ESP-32 to RS-485. Connected to the other end of this is my original RS-485/USB adaptor as used for the initial prototypes, which is then attached to a Raspberry Pi (although any Linux box would do at this point) running [modbus-slave](#modbus-slave).

![20250112_115403](https://github.com/user-attachments/assets/9af33c16-631b-48bb-9e9a-014e2b3b1a2a)

That cluster of reistors is the RS-485 termination, 120R spread across 3 because I didn't have one that exact value... I don't think it's strictly needed for this short length run but it was there whilst I investigated various behavioural issues.

Note that you can buy RS-485 transceiver modules similar to the RS-485/USB adaptors however in this instance it made more sense to attach the chip directly as ultimately I want to mount everything on a single PCB and that would just be another module flapping around. Ultimately they aren't much more than a PCB with a MAX485 and some discrete logic anyway. Plus there are other reasons why it could be beneficial which I'll come onto in a bit.

Two wire RS-485 is **half duplex** and this is a crucial difference from plain old RS-232 as it is necessary to explictly turn on and off the transmitters & receivers in order to avoid collisions on the bus. Using the RS-485/USB adapter this is transparant as it's done by the hardware on the board. However since we're interfacing directly to the transceiver chip, this needs to be done by the software and is controlled by one of the GPIOs from the ESP-32 (GPIO-4 in this case), which is connected to the ~RE (Receiver Output Enable) and DE (Data Output Enable) pins. As ~RE is active low and DE is active high this means we can control both with the single GPIO. The default, normal operating state of the software is to have the receiver enabled, the toggling into transmit mode (and back again) is controlled via calbbacks passed into the Modbus library which invoke them at the appropriate points during the transaction.

![schematic](https://github.com/user-attachments/assets/a093913b-f679-46b6-b560-40dc0281505c)

### Inverter

After having had the the breadboard setup connected to the inverter now for a week or so with no real problems, I migrated it to a more permanent, stripboard based solution.

![20250201_130430](https://github.com/user-attachments/assets/6a6d2dc6-3bbf-4f17-93a8-78bbb3b23fd2)

In this picture, I'm still powering it from the micro-USB connector, those two unused pins on the 4W connector are intended for connecting it directly to the 5V supply. When it's powered via that route, it won't then be possible to get to the debug serial via the USB connector so that 4 pin header is to bring it out for connection to something like a Raspberry Pi instead.

![stripboard](https://github.com/user-attachments/assets/92398d00-1148-46f1-81ac-eb5e879a39ba)

## Software
There are 4 distinct applications currently here. The first 3 are designed to be built under any recent Linux distro using the provided makefiles. Dependencies are shown in the sections below for each app. It's also possible to build these as well under Windows and Visual Studio projects are provided however you'll need to get hold off and/or build the additional libraries. The fourth application is the [Arduino sketch for the ESP32.](#modbus-esp32)

The RS485 link runs at 9600, 8 bits, 1 stop bit, no parity. None of the Linux applications which interface to the serial ports directly configure any of the serial settings, you'll need to do that first by hand which is normally just a case of doing something like:

`stty -F /dev/ttyUSB0 9600 raw -echo`

### modbus-sniffer
Dependencies: boost-crc, boost-datetime (sudo apt-get install libboost-dev libboost-date-time-dev)

As the name suggests, this is an app designed to sniff traffic on the serial link, essentially to capture and profile the transactions performed by the wifi dongle. This also let me determine which of the, several Solis Modbus documents that are out there correspond to the register set of the inverter, that being [this document](https://www.scss.tcd.ie/Brian.Coghlan/Elios4you/RS485_MODBUS-Hybrid-BACoghlan-201811228-1854.pdf). Based on this, the tool will also decode a (very limited) subset of the registers, in turn when then allowed me to figure out how to decode the [registers holding active generation data](registers.txt)

The app also has some additional options which allow the creation of a .csv file (for import into Excel or similar) for measuring timings over a longer period and recording of the bus traffic (which itself can then be replayed back by the tool if need be). There's some sample artefacts in the [data folder](data).

Example usage:

``./modbus-sniffer /dev/ttyUSB0``

### modbus-solis-broadcast
Dependencies: boost-chrono, boost-datetime, boost-system, cjson, libmodbus (sudo apt-get install libboost-chrono-dev libboost-date-time-dev libboost-system-dev libmodbus-dev libcjson-dev)

This is the app that actually issues Modbus requests to the inverter to retrieve the current solar metrics. It then JSON encodes them, using the same naming convention as the Solis API and [sends them out as a broadcast UDP packet](#udp-broadcast) on port 52005. 

Under normal conditions, every 5 minutes the datalogger retrieves many of the input registers from the inverter (these then form the source of the information stored in the cloud). The datalogger itself can talk to up to 10 inverters (or Modbus slaves) & after the register retrieval has been completed, it then proceeds to issue 4 register read requests (with a 3 second timeout between each read) to slaves 2 through 10. In a system with only one inverter (slave 1), these will all time out. This process takes just over 2 minutes to complete. [data/13230_traffic.log](data/13230_traffic.log) and [data/13230_traffic.ods](data/13230_traffic.ods) show this behaviour.

This effectively locks out the bus for that period, therefore the _modbus-solis-broadcast_ app monitors for those redundant slave requests and answers them with a Modbus exception code. This then reduces the busy time to ~45s. The trace in [data/13230_slaves_answered.ods](data/13230_slaves_answered.ods) depicts this behaviour. It then waits for a 10s period of inactivity on the bus, ensuring that the dongle has finished. At which point it then issues requests to read the necssary registers holding the current solar generation data, which if successful are then sent as a UDP broadcast to the local network. It then performs this process for the remainder of the 5 minute window, with a 16s wait between each request before then looping back to sync with the wifi dongle. 

Example usage:

``./modbus-solis-broadcast /dev/ttyUSB0``

#### Using a directly attached RS485 adapter with a Raspberry Pi
The app was primarily designed to work with something like a USB/RS485 adapter where the turning on/off of the transceivers is managed automatically by the device. However it can also be used on a Raspberry Pi with something like a MAX4385 chip connected to the Pi's UART - in effect, a similar setup to that used with the [ESP-32 setup](#RS-485). With this configuration, the transceivers need to be managed under software control, using one (or two) of the Pi's GPIO lines. 

To configure the software to work in this mode, you first need to build and install the [WiringPi](https://github.com/WiringPi/WiringPi) library on the Pi. Then edit the [modbus-solis-broadcast/modbus-solis-broadcast.cpp](modbus-solis-broadcast/modbus-solis-broadcast.cpp] file and set the RS485_RE (and/or RS485_DE) definitions to match those connected to the MAX device. Then build as follows:

``RPI=1 make``

### modbus-slave
Dependencies: boost-chrono, boost-datetime, boost-system, libmodbus (sudo apt-get install libboost-chrono-dev libboost-date-time-dev libboost-system-dev libmodbus-dev)

modbus-slave does a passable emulation of the Solis inverter and wifi dongle. If you use a serial crossover cable between two 232 ports, you can then use the modbus-sniffer and/or modbus-solis-broadcast to test the behaviour in a simulated environment.

By default, it will generate simulated Modbus transactions, which would normally be initiated by the dongle, five minutes. If you use the modbus-sniffer app, you should be able to see these arrive and be decoded. It will also respond to Modbus register queries (for example, as issued by the modbus-solis-broadcast app), responsing with fixed register values taken from my own inverter. The periodic transactions can also be disabled, if required via a command line option, in which case it will simply listen to and respond with register requests.

I developed this primarily to support test & debug of the ESP32 solution, prior to connecting it to the inverter.

Example usage:

``./modbus-solis-slave /dev/ttyUSB1``

then in another terminal, run:

``./modbus-solis-broadcast /dev/ttyUSB0``

The latter should wait to sync with the simulated wifi transactions sent from the slave, then proceed to issue the requests to retrieve the current solar data values.

### modbus-esp32 
Dependencies: [ModbusMaster](https://github.com/fridgemagnet3/ModbusMaster)

This is the Arduino sketch for the ESP-32 port of [modbus-solis-broadcast](#modbus-solis-broadcast). As a minimum, you will need to edit the [config.h](modbus-esp32/config.h)  file to define your Wifi SSID and password. Additionally, if you are using different GPIO pins to those shown on the schematic, you'll need to edit those settings as well.

## MQTT and Home Assistant Integration

The script [mqtt/solar_mqtt_publisher.py](mqtt/solar_mqtt_publisher.py) listens to the solar UDP broadcast packets and publishes a subset of them to an MQTT broker. It also publishes [home assistant MQTT auto-discovery](https://www.home-assistant.io/integrations/mqtt/#mqtt-discovery) topics for each sensor, meaning they should automatically show up in HA.

