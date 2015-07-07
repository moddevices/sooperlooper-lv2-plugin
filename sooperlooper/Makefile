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
CXX ?= g++

# flags
CXXFLAGS += -I. -O2 -Wall -fPIC -DPIC -c -DSAMPLE_MEMORY=$(SAMPLE_MEMORY) $(DEBUG)
LDFLAGS += -shared -lm -Wl,--no-undefined

# remove command
RM = rm -f
	
# sources and objects
SRC = $(wildcard src/*.cpp)
OBJ = $(SRC:.cpp=.o)

$(PLUGIN): $(OBJ)
	$(CXX) $(OBJ) $(LDFLAGS) -o $(PLUGIN)

%.o: %.cpp
	$(CXX) $< $(CXXFLAGS) -o $@

clean:
	$(RM) src/*.o $(PLUGIN)

install: $(PLUGIN)
	mkdir -p $(INSTALL_PATH)/sooperlooper.lv2
	cp $(PLUGIN) $(INSTALL_PATH)/sooperlooper.lv2/
	cp -r src/*.ttl src/modgui $(INSTALL_PATH)/sooperlooper.lv2/
