

all:
	$(MAKE) -C modbus-sniffer
	$(MAKE) -C modbus-solis-broadcast
	$(MAKE) -C modbus-slave
	
clean:
	$(MAKE) -C modbus-sniffer clean
	$(MAKE) -C modbus-solis-broadcast clean
	$(MAKE) -C modbus-slave clean
