
# This is how many seconds of loop memory you want your
# plugin to have available.
SAMPLE_MEMORY = 200

# plugin name
PLUGIN = $(shell basename $(shell pwd) | tr A-Z a-z)
PLUGIN_SO = $(PLUGIN).so

# installation path
ifndef INSTALL_PATH
INSTALL_PATH = /usr/local/lib/lv2/
endif
INSTALLATION_PATH = $(DESTDIR)$(INSTALL_PATH)/$(PLUGIN).lv2/

# compiler and linker
CXX ?= g++

# flags
CXXFLAGS += -I. -O2 -Wall -fPIC -DPIC -c -DSAMPLE_MEMORY=$(SAMPLE_MEMORY)
LDFLAGS += -shared -lm -Wl,--no-undefined

# remove command
RM = rm -f

# sources and objects
SRC = $(wildcard src/*.cpp)
OBJ = $(SRC:.cpp=.o)

$(PLUGIN_SO): $(OBJ)
	$(CXX) $(OBJ) $(LDFLAGS) -o $(PLUGIN_SO)

%.o: %.cpp
	$(CXX) $< $(CXXFLAGS) -o $@

clean:
	$(RM) src/*.o $(PLUGIN_SO)

install: $(PLUGIN_SO)
	mkdir -p $(INSTALLATION_PATH)
	cp $(PLUGIN_SO) $(INSTALLATION_PATH)
	cp -r src/*.ttl src/modgui $(INSTALLATION_PATH)
