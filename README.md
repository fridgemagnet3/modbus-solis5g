# Modbus tools for Solis 5g solar inverters

This is a collection of some applications, tools and utilities I've put together for firstly investigating then retrieving information from a Solis 5G solar inverter via Modbus.

## Background

Solis 5G inverters allow for the connection of a Wifi dongle which allows information to be uploaded to the Solis cloud servers (in China). In turn, this information can be retrieved via the [SolisCloud website](https://www.soliscloud.com) and/or via the app.

Additionally, Solis provide [an API](https://solis-service.solisinverters.com/en/support/solutions/articles/44002212561-api-access-soliscloud) which allows you to retrieve this same data programatically sent over https. This is packaged up as JSON encoded data which I've made use of for some time in order to retrieve both historical and current data, the latter of which I've had presented on various displays, including [@zx85's solar-display-micropython project](https://github.com/zx85/solar-display-micropython)

<a name="udp-broadcast"></a>
One significant difference is that I've taken out the functionality from the solar displays which directly interface to the Solis API. Instead, they pick the data up from UDP packets, broadcast over the local network. I therefore only have a [single application which retrieves the data, then pushes it out periodically](https://github.com/fridgemagnet3/solar-display-micropython/tree/main/solis-broadcast) - essentially it just retransmits a subset of the JSON data, as received from Solis. It needed to be a subset because the payload is well in excess of a single datagram and the ESP32 (as used in [@z85's display](https://github.com/zx85)) IP stack can't handle UDP fragmentation. This then means I can also easily swap out where the data comes from - the Solis cloud or (as is [described later](#modbus-solis-broadcast)) locally from the inverter. 

[/fridgemagnet3/solar-display-micropython](https://github.com/fridgemagnet3/solar-display-micropython) is my fork of @zx85's solar display which receives it's data via UDP instead of directly from the Solis cloud API.

The aim of this project was to bypass the need to go via the Solis Wifi dongle, cloud and API and instead obtain the current data directly from the inverter. Aside from cutting out these additional steps, the dongle only updates the cloud data every 5 minutes and I wanted to refresh data at a much higher rate, at least every minute. This also then offers better response times for making decisions based around the data, for example using surplus energy to heat our hot water rather than exporting it, within the current arrangement, it's entirely feasible you could end up importing energy should the sun go behind a cloud seconds after the last set of data is pushed up to the cloud.

An additional requirement though was, (unlike a number of other articles which have explored this) to continue to allow the Wifi dongle to remain in play. Hence whatever I came up with would need to cooperate with it.

## Interfacing overview
The physical interface between the inverter and the Wifi dongle is a serial RS485 interface, using a [Modbus RTU](https://en.wikipedia.org/wiki/Modbus) protocol. It connects via a 4 pin, Exceedconn EC04681-2023BF (or 2014BF) connector which provides a 5V supply to the dongle and the RS485 differential pair.

Modbus is a client/server architecture, the dongle is the master (it issues the requests) and the inverter the slave (it responds to them). That immediately poses a challenge because in order to issue my own requests, I need to also be a master which generally isn't how Mobus works. [This thread](https://community.openenergymonitor.org/t/getting-data-from-inverters-via-an-rs485-connection/8377) was a useful read and if you make it to the end (it's quite long!), you'll see this problem being raised, with one of the contributers indicating issues with collisions between his kit in the dongle. The conclusion to this ends up suggesting having two RS485 interfaces, with whatever kit you end up using acting as a proxy between the dongle and inverter. To me this felt like massive overkill, overly complex and fraught with the potential for timing issues between the two bits of kit at each end. My working presmise was therefore that if the wifi dongle is acting in a predictable manner, knowing that in advance, it should be possible to minimize collisions.

RS485 is "multi-drop" meaning you can hang multiple things off the bus. My plan was therefore to wire up a cable, with plug/socket at each end, this carries the existing connection to/from the wifi dongle. I would then hang an additional RS485 interface from mid way along the cable. It's worth reading up on RS485, there are various rules about how things should be connected, terminated and so on. Shielded twisted pair cabling is recommended, I've seen it suggested that CAT-5 cable will work over short distances however to avoid any issues, I went out and bought some proper cabling.

The project is constructed in two parts - the first, is essentially the "proof of concept" and for this I used a RS485/USB adaptor connected to a Raspberry Pi. If everything pans out, part 2 replaces this with an ESP32 microcontroller.

![20241103_180441](https://github.com/user-attachments/assets/29c87abe-2c4b-43d1-8a9e-ae0e4fc55c1a)

At the current time, part 1 is complete and I'm starting to look at the ESP32 solution.

### Spurious characters
I believe that the invertor (and possibly the Wifi logger as well) is generating spurious characters on the serial line in or around the time it responds to a request. This can be seen in the [sample log file](data/2024-11-08_12-07-44.log) where it reports things like _Skipped 2 bytes in stream looking for next header_. In the case of the RS485/USB adaptor used during the first part of the project, this seems to materialise as up to 3 NULL bytes which I believe are actually _serial break_ characters (or framing errors). Where the ESP-32 module is concerned, I see just random characters. In the process of investigating this, I've tried adding in termination resistors (to the connectors at both end of the cable since I don't believe either the interter or logger has them) and bias reistors (even though I don't believe the MAX devices need them). Neither of which has made any difference, which based on the behaviour I was seeing, frankly didn't think it would. 

As an extra observation, as part of my simulated test setup, I've had the ESP-32 module connected directly to the RS485/USB module. In this configuration, the USB module was still detecting framing errors at around the point the ESP turned off it's transmitters. However the data received by the ESP was rock solid.

As a result, I've had to implement software workarounds, in effect to discard any incoming bytes up until the expected start of response sequence is detected. In the case of the Linux applications, this is in the form of a patch against the current. 3.1.11 release of the libmodbus library, this can be found in the [libmodbus folder](libmodbus/). For the ESP-32 module, I've created a fork of the [ModbusMaster](https://github.com/fridgemagnet3/ModbusMaster) library.

## ESP-32 Module
For the finished product, I'm planning on replacing the Raspberry Pi with an ESP32 WROOM-32 module. These are inexpensive, nifty little microcontrollers which have a bunch of I/O (including serial) plus built in Wifi. The hope it that I'll also be able to power it from the 5V supply that connects to the wifi logger, therefore reducing the need for further cabling.

Here is my initial test setup. 

![20241231_195839](https://github.com/user-attachments/assets/0fdb9c91-805f-46f1-a657-924b7de08527)

It's connected to the RS232 port of a Raspberry Pi running my [modbus-slave](#modbus-slave) app which simulates the behaviour of the inverter and wifi logger. As both boards use 3.3V logic levels, this provides a simple environment for me to develop & test the software for the ESP32. To date, I've now managed to successfully port across the [modbus-solis-broadcast](#modbus-solis-broadcast) application which is now fully working in the simulated environment. The next step will be to replace the RS232 link with RS485.

### RS-485
Took a little longer than anticipated to get this next bit working, in the process having to make a few tweaks to my [modbus-slave](#modbus-slave) simulator app to get it to work properly with RS485 but anyway here's the updated hardware.

![20250112_115403](https://github.com/user-attachments/assets/9af33c16-631b-48bb-9e9a-014e2b3b1a2a)

The main addition to my test setup is the presence of a [MAX3485 RS-485 transceiver](https://www.analog.com/en/products/max3485.html), this basically converts the 3.3V serial from the ESP-32 to RS-485. Connected to the other end of this is my original RS-485/USB adaptor as used for the initial prototypes, which is then attached to a Raspberry Pi (although any Linux box would do at this point) running [modbus-slave](#modbus-slave).

That cluster of reistors is the RS-485 termination, 120R spread across 3 because I didn't have one that exact value... I don't think it's strictly needed for this short length run but it's there whilst I investigate various behavioural issues.

Note that you can buy RS-485 transceiver modules similar to the RS-485/USB adaptors however in this instance it made more sense to attach the chip directly as ultimately I want to mount everything on a single PCB and that would just be another module flapping around. Ultimately they aren't much more than a PCB with a MAX485 and some discrete logic anyway. Plus there are other reasons why it could be beneficial which I'll come onto in a bit.

Two wire RS-485 is **half duplex** and this is a crucial difference from plain old RS-232 as it is necessary to explictly turn on and off the transmitters & receivers in order to avoid collisions on the bus. Using the RS-485/USB adapter this is transparant as it's done by the hardware on the board. However since we're interfacing directly to the transceiver chip, this needs to be done by the software and is controlled by one of the GPIOs from the ESP-32 (GPIO-4 in this case), which is connected to the ~RE (Receiver Output Enable) and DE (Data Output Enable) pins. As ~RE is active low and DE is active high this means we can control both with the single GPIO. The default, normal operating state of the software is to have the receiver enabled, the toggling into transmit mode (and back again) is controlled via calbbacks passed into the Modbus library which invoke them at the appropriate points during the transaction.

### Inverter

Having had the the breadboard setup connected to the inverter now for a week or so with no real problems, I've now migrated it to a more permanent, stripboard based solution.

![20250201_130430](https://github.com/user-attachments/assets/6a6d2dc6-3bbf-4f17-93a8-78bbb3b23fd2)

At present, I'm still powering it from the micro-USB connector, those two unused pins on the 4W connector are intended for connecting it directly to a 5V supply. When it's powered via that route, it won't then be possible to get to the debug serial via the USB connector so that 4 pin header is to bring it out for connection to something like a Raspberry Pi instead.

## Software
There's 3 distinct applications currently here. All are designed to be built under any recent Linux distro using the provided makefiles. Dependencies are shown in the sections below for each app. It's also possible to build these as well under Windows and Visual Studio projects are provided however these only offer limited functionality, in particular anything that does direct serial port receives & transmits won't work plus you'll need to get hold off and/or build the additional libraries. In short, these were really more for me to do some initial offline debug & test.

The RS485 link runs at 9600, 8 bits, 1 stop bit, no parity. None of the applications which interface to the serial ports directly configure any of the serial settings, you'll need to do that first by hand which is normally just a case of doing something like:

`stty -F /dev/ttyUSB0 9600 raw -echo`

### modbus-sniffer
Dependencies: boost-crc, boost-datetime (sudo apt-get install libboost-dev libboost-date-time-dev)

As the name suggests, this is an app designed to sniff traffic on the serial link, essentially to capture and profile the transactions performed by the wifi dongle. From this I was able to asertain that the wifi dongle, for the most part only ever performs relatively short transactions, every minute and retrieves the bulk of the data every 5 minutes. This also let me determine which of the, several Solis Modbus documents that are out there correspond to the register set of the inverter, that being [this document](https://www.scss.tcd.ie/Brian.Coghlan/Elios4you/RS485_MODBUS-Hybrid-BACoghlan-201811228-1854.pdf). Based on this, the tool will also decode a (very limited) subset of the registers, in turn when then allowed me to figure out how to decode the [registers holding active generation data](registers.txt)

The app also has some additional options which allow the creation of a .csv file (for import into Excel or similar) for measuring timings over a longer period and recording of the bus traffic (which itself can then be replayed back by the tool if need be). There's some sample artefacts in the [data folder](data), included an annotated spreadsheet, generated from the csv. This shows the typical wifi dongle behaviour as a result of leaving the sniffer running for a couple of days, plus some interesting oddities which seem to happen once a day (search for the word _anomaly_).

Example usage:

``./modbus-sniffer /dev/ttyUSB0``

### modbus-solis-broadcast
Dependencies: boost-chrono, boost-datetime, boost-system, cjson, libmodbus (sudo apt-get install libboost-chrono-dev libboost-date-time-dev libboost-system-dev libmodbus-dev libcjson-dev)

This is the app that actually issues Modbus requests to the inverter to retrieve the current solar metrics. It then JSON encodes them, using the same naming convention as the Solis API and [sends them out as a broadcast UDP packet](#udp-broadcast) on port 52005. 

To achieve the requirement of cooperating with the Wifi dongle, the application first waits for the next burst of serial traffic on the link (signalling the dongle performing a transaction with the inverter). It then waits for a 10s period of inactivity on the bus, ensuring that the dongle has finished. At which point it then issues requests to read the necssary registers holding the current solar generation data, which if successful are then sent as a UDP broadcast to the local network. It then performs this process twice more, with a 20s wait between each request before then looping back to sync with the wifi dongle. Under normal circumstances, this results in the solar displays being updated every 20s. I've had it running for a few weeks now and have not seen any problems or seen any evidence of the data retrieved by the dongle becoming corrupted. Again, there are some [sample logs in the data section](data/).

Example usage:

``./modbus-solis-broadcast /dev/ttyUSB0``

### modbus-slave
Dependencies: boost-chrono, boost-datetime, boost-system, libmodbus (sudo apt-get install libboost-chrono-dev libboost-date-time-dev libboost-system-dev libmodbus-dev)

modbus-slave does a passable emulation of the Solis inverter and wifi dongle. If you use a serial crossover cable between two 232 ports, you can then use the modbus-sniffer and/or modbus-solis-broadcast to test the behaviour in a simulated environment.

By default, it will generate simulated Modbus transactions, which would normally be initiated by the dongle, every minute. If you use the modbus-sniffer app, you should be able to see these arrive and be decoded. It will also respond to Modbus register queries (for example, as issued by the modbus-solis-broadcast app), responsing with fixed register values taken from my own inverter. The periodic transactions can also be disabled, if required via a command line option, in which case it will simply listen to and respond with register requests.

I've developed this primarily to support the next part of the project so I can test & debug my ESP32 solution in a test environment, prior to connecting it to the inverter.

Example usage:

``./modbus-solis-slave /dev/ttyUSB1``

then in another terminal, run:

``./modbus-solis-broadcast /dev/ttyUSB0``

The latter should wait to sync with the simulated wifi transactions sent from the slave, then proceed to issue the requests to retrieve the current solar data values.
