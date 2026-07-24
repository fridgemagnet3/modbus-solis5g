CC?=g++
CFLAGS=-g -O2 -D_FILE_OFFSET_BITS=64 -fmessage-length=0 -fPIC 

OBJS=logger-multislave.o
LIBS=-lmodbus
APP=logger-multislave

all: $(APP)

$(APP): $(OBJS)
	$(CC) -o $(APP) $^ $(LIBS) 

%.o: %.c
	$(CC) -c -o $@ $< $(CFLAGS)

.PHONY: clean install
clean:
	rm -f *.o
	rm -f $(APP)

