CC=gcc
CFLAGS=-c -O2 -Wall -pipe
LDFLAGS=-lasound -ljack -lpthread -lltc -lm
SOURCES=ltcmidiplayer.c ltcframeutil.c
OBJECTS=$(SOURCES:.c=.o)
EXECUTABLE=ltcmidiplayer



all: $(SOURCES) $(EXECUTABLE)


$(EXECUTABLE): $(OBJECTS)
	$(CC) $(LDFLAGS) $(OBJECTS) -o $@

%.o: %.c
	$(CC) $(CFLAGS) $< -o $@


clean:
	rm -rf $(EXECUTABLE) $(OBJECTS)


.PHONY=clean all
