DEST_DIR=/usr/lib/lv2

all:
	$(MAKE) -C sooperlooper
	$(MAKE) -C sooperlooper-2x2

install:
	$(MAKE) install -C sooperlooper
	$(MAKE) install -C sooperlooper-2x2

clean:
	$(MAKE) clean -C sooperlooper
	$(MAKE) clean -C sooperlooper-2x2
