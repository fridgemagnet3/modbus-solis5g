CXX?=g++
CXXFLAGS=-g -O2 -D_FILE_OFFSET_BITS=64 -fmessage-length=0 -fPIC 

OBJS=modbus-slave.o
LIBS=-lmodbus -lboost_date_time -lboost_chrono -lboost_system
APP=modbus-slave

all: $(APP)

$(APP): $(OBJS)
	$(CXX) -o $(APP) $^ $(LIBS) 

%.o: %.cpp
	$(CXX) -c -o $@ $< $(CXXFLAGS)

.PHONY: clean install
clean:
	rm -f *.o
	rm -f $(APP)

