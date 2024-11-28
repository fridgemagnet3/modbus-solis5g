CXX?=g++
CXXFLAGS=-g -O2 -D_FILE_OFFSET_BITS=64 -fmessage-length=0 -fPIC 

OBJS=modbus.o

LIBS=-lboost_date_time

APP=modbus-sniffer

all: $(APP)

$(APP): $(OBJS)
	$(CXX) -o $(APP) $^ $(LIBS) 

%.o: %.cpp
	$(CXX) -c -o $@ $< $(CXXFLAGS)

.PHONY: clean
clean:
	rm -f *.o
	rm -f $(APP)

