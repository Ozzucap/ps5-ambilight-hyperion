# Bridge (runs on your NAS/PC):          make bridge
# LED test payload (PS5):                make led-test PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk
# App logger payload (PS5):              make whatapp PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk
#
# The ps5-payload-sdk toolchain is only needed for the two PS5 payloads.
SDK_CC ?=
ifdef PS5_PAYLOAD_SDK
  include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
  SDK_CC := $(CC)
endif

.PHONY: bridge led-test whatapp clean

bridge: build/ps5-bridge

build/ps5-bridge: src/ps5_bridge.c src/hyperion_fb.c src/hyperion_fb.h
	mkdir -p build
	gcc -O2 -Wall -Wextra -static -o $@ src/ps5_bridge.c src/hyperion_fb.c -lm

led-test: build/ps5-ambilight-test.elf

build/ps5-ambilight-test.elf: src/test_sender.c src/hyperion_fb.c src/hyperion_fb.h
	@test -n "$(SDK_CC)" || (echo "set PS5_PAYLOAD_SDK"; exit 1)
	mkdir -p build
	$(SDK_CC) -O2 -Wall -DPS5_PAYLOAD -o $@ src/test_sender.c src/hyperion_fb.c

whatapp: build/whatapp.elf

build/whatapp.elf: tools/whatapp/main.c
	@test -n "$(SDK_CC)" || (echo "set PS5_PAYLOAD_SDK"; exit 1)
	mkdir -p build
	$(SDK_CC) -O2 -Wall -g -o $@ $<

clean:
	rm -rf build
