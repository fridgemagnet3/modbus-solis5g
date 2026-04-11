# Modbus solis-broadcast with TCP support

This branch currently exists as a **proof of concept** in extending my [modbus-solis-broadcast](./modbus-solis-broadcast) app to operate as a Modbus TCP server, forwarding requests to/from the inverter. This then allows a Modbus TCP client (eg. HA) to make arbitrary register accesses (primarily writes). 

As background, perusing the README on the main branch is worthwhile to give an overview of how the original app works however as a brief summary, the Wifi datalogger issues a Modbus request to the inverter every minute, with the main register retrieval occurring every 5 minutes. This leaves plenty of spare capacity on the serial link which _modbus-solis-broadcast_ takes advantage of to read the most useful status registers, encode them as a JSON payload & transmit them out as a UDP broadcast packet. It does this roughly every 20 seconds. 

The new functionality allows one of those 20s slots to be used to instead be used to perform an abirtary read/write of one or more registers from the inverter, as depicted in the following diagram.

<img width="668" height="255" alt="tcp-transact-timeline" src="https://github.com/user-attachments/assets/60fcd1a4-b990-42cf-944e-7f659e6e4a66" />

Note: At the present time, I have only tested this in simulation with my [modbus-slave](./modbus-slave) app, it's NOT been connected to a Solis inverter however since it uses the same Modbus library to perform the RTU transactions, I forsee no reason why it shouldn't work as expected.

## How it works

The initial response to any TCP Modbus request is to return an exception code 6, _Server Device Busy_, the request is then added to an internal queue to be serviced on the next 20s cycle. The next time the client makes the same request (and in the case of a write, with the same data), the response from the register access is returned. The following packet trace shows this behaviour:

```
No.     Time        Source                Destination           Protocol Length Info
  22211 0.022096    192.168.0.159         192.168.0.201         Modbus/TCP 78        Query: Trans:     4; Unit:   1, Func:   6: Write Single Register

Modbus/TCP
Modbus
    .000 0110 = Function Code: Write Single Register (6)
    Reference Number: 43003
    Data: 000d

No.     Time        Source                Destination           Protocol Length Info
  22212 0.000523    192.168.0.201         192.168.0.159         Modbus/TCP 75     Response: Trans:     4; Unit:   1, Func:   6: Write Single Register. Exception returned 

Modbus/TCP
Function 6:  Write Single Register.  Exception: Slave device busy
    .000 0110 = Function Code: Write Single Register (6)
    Exception Code: Slave device busy (6)

No.     Time        Source                Destination           Protocol Length Info
  22214 0.017578    192.168.0.159         192.168.0.201         Modbus/TCP 78        Query: Trans:     5; Unit:   1, Func:   6: Write Single Register

...

Modbus/TCP
Modbus
    .000 0110 = Function Code: Write Single Register (6)
    Reference Number: 43003
    Data: 000d

No.     Time        Source                Destination           Protocol Length Info
  50690 0.000619    192.168.0.201         192.168.0.159         Modbus/TCP 78     Response: Trans:    10; Unit:   1, Func:   6: Write Single Register

Modbus/TCP
Modbus
    .000 0110 = Function Code: Write Single Register (6)
    [Request Frame: 50689]
    [Time from request: 0.000619391 seconds]
    Reference Number: 43003
    Data: 000d
```
Whilst read requests are supported, the primary purpose of this extension is for performing thd odd write request ie. to control the inverter in some way. If regular reads are required, it would be better to incorporate them into the regular UDP broadcast packets. Input register reads are cached for 5 minutes, holding registers for 20 minutes (on the basis the latter are more likely only to change when updated by an external write). 

This works quite nicely with [fboundy's ha_solis_modbus](https://github.com/fboundy/ha_solis_modbus), which was used to generate the above network capture by using the example script to set the inverter's time. There is obviously a delay between any write/read and getting the response back however after running the script, 40-50s later, the expected results are reflected in the Solis Hour/Minute/Second RW registers. 

![PXL_20260328_184423624](https://github.com/user-attachments/assets/40655d48-b9ca-4094-84f3-a8fa8cb3b5e1)

It's not though really going to work that well with [Pho3niX90's Modbus integration](https://github.com/Pho3niX90/solis_modbus) which performs a LOT of register accesses, although I have run it up with this, just to prove the point. What you find is that it will take 10-15 minutes to fully enable all the sensors and controls during which period there will be no UDP broadcast packets. Ultimately what you end up with is a lag that is arguably worse than talking to the Solis cloud.

![PXL_20260327_142546306](https://github.com/user-attachments/assets/92b22f75-bd58-4632-aad1-fa3f12113fba)

The server should support multiple TCP clients although this is untested.
