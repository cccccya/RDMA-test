.PHONY: clean
CFLAGS  := -Wall -g
LD      := gcc
LDLIBS  := -lrdmacm -libverbs -lpthread
APPS    := ud_client ud_server
all: ${APPS}
clean:
	rm -f *.o ${APPS}