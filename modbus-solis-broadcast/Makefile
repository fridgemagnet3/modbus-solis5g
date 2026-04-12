CXX?=g++
CXXFLAGS=-g -O2 -D_FILE_OFFSET_BITS=64 -fmessage-length=0 -fPIC 

ifdef RPI
CXXFLAGS+= -DRPI
endif

OBJS=modbus-solis-broadcast.o

LIBS=-lmodbus -lboost_date_time -lboost_chrono -lcjson -lboost_system
ifdef RPI
LIBS+= -lwiringPi
endif

APP=modbus-solis-broadcast

all: $(APP)

$(APP): $(OBJS)
	$(CXX) -o $(APP) $^ $(LIBS) 

%.o: %.cpp
	$(CXX) -c -o $@ $< $(CXXFLAGS)

.PHONY: clean install
clean:
	rm -f *.o
	rm -f $(APP)

