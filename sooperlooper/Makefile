# Please edit this to work for you

# This is how many seconds of loop memory you want your
# plugin to have available.
SAMPLE_MEMORY = 200

# comment this out if you don't want to see my debugging output
# It is actually useful to see when exactly it is changing states.
# All output is out stderr
DEBUG = -DDEBUG

# plugin name
DIR_NAME = $(shell basename `pwd`)
PLUGIN = $(shell echo $(DIR_NAME) | tr A-Z a-z).so

# installation path
ifndef INSTALL_PATH
	INSTALL_PATH = /usr/local/lib/lv2/
endif

# compiler and linker
CC = g++
LD = g++

# flags
CFLAGS += -I. -O2 -Wall -fPIC -DPIC -c -DSAMPLE_MEMORY=$(SAMPLE_MEMORY) $(DEBUG) -fpermissive
LDFLAGS += -shared -lc -lm -fPIC -DPIC -nostartfiles

# libs
# LIBS = -lm -lsamplerate
# LIBS = -llo -ljack
# remove command
RM = rm -f
	
# sources and objects
SRC = $(wildcard src/*.cpp)
OBJ = $(SRC:.cpp=.o)

$(PLUGIN): $(OBJ)
	$(LD) $(LDFLAGS) $(OBJ) -o $(PLUGIN) $(LIBS)

%.o: %.cpp
	$(CC) $(CFLAGS) -o $@ $<

clean:
	$(RM) src/*.o $(PLUGIN)

install: $(OBJ) 
	mkdir -p $(INSTALL_PATH)/sooperlooper.lv2
	cp $(PLUGIN) $(INSTALL_PATH)/sooperlooper.lv2/
	cp src/*.ttl $(INSTALL_PATH)/sooperlooper.lv2/
	test -d src/modgui && cp -r src/modgui/ $(INSTALL_PATH)/sooperlooper.lv2/ || true
