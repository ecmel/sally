CC = clang
MIN_MACOS = 14.0
# ARCHS="arm64 x86_64" builds a universal app.
ARCHS ?=
ARCHFLAGS = $(foreach a,$(ARCHS),-arch $(a))
CFLAGS = -O2 -std=gnu11 -Wall -Wextra -Wno-unused-parameter -mmacosx-version-min=$(MIN_MACOS) $(ARCHFLAGS) -MMD -MP
OBJCFLAGS = $(CFLAGS) -fobjc-arc
FRAMEWORKS = -framework Cocoa -framework Metal -framework QuartzCore \
             -framework AudioToolbox -framework CoreAudio -framework GameController \
             -framework UniformTypeIdentifiers -framework Carbon

BUILD = build
APP = $(BUILD)/Sally.app
EXE = $(APP)/Contents/MacOS/Sally
ICON = $(APP)/Contents/Resources/Sally.icns
INSTALL_DIR ?= /Applications
LSREGISTER = /System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister

# Release settings, read from the environment by default.
VERSION ?=
SIGN_IDENTITY ?= $(APPLE_SIGNING_IDENTITY)
DMG = $(BUILD)/Sally$(if $(VERSION),-$(VERSION)).dmg

CORE = src/cpu.c src/machine.c src/antic.c src/gtia.c src/pokey.c src/os.c src/asm6502.c
FRONT_C = src/control.c
FRONT = src/main.m src/SallyView.m src/Emulator.m src/Audio.m src/Keyboard.m

CORE_OBJS = $(CORE:src/%.c=$(BUILD)/%.o)
FRONT_OBJS = $(FRONT:src/%.m=$(BUILD)/%.o) $(FRONT_C:src/%.c=$(BUILD)/%.o)

all: $(EXE) $(ICON)

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: src/%.m | $(BUILD)
	$(CC) $(OBJCFLAGS) -c $< -o $@

$(BUILD):
	mkdir -p $(BUILD)

$(EXE): $(CORE_OBJS) $(FRONT_OBJS) Info.plist
	mkdir -p $(APP)/Contents/MacOS
	cp Info.plist $(APP)/Contents/Info.plist
	$(if $(VERSION),plutil -replace CFBundleShortVersionString -string $(VERSION) $(APP)/Contents/Info.plist)
	$(if $(VERSION),plutil -replace CFBundleVersion -string $(VERSION) $(APP)/Contents/Info.plist)
	$(CC) -mmacosx-version-min=$(MIN_MACOS) $(ARCHFLAGS) $(CORE_OBJS) $(FRONT_OBJS) $(FRAMEWORKS) -o $@

# The icon is drawn by tools/makeicon.m at every size iconutil wants.
$(BUILD)/makeicon: tools/makeicon.m | $(BUILD)
	$(CC) $(OBJCFLAGS) $< -framework Foundation -framework CoreGraphics -framework ImageIO \
		-framework UniformTypeIdentifiers -o $@

$(ICON): $(BUILD)/makeicon
	rm -rf $(BUILD)/Sally.iconset
	$(BUILD)/makeicon $(BUILD)/Sally.iconset
	mkdir -p $(dir $@)
	iconutil -c icns $(BUILD)/Sally.iconset -o $@

# Headless runner and tests.
$(BUILD)/sallyrun: tools/sallyrun.c $(CORE_OBJS)
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD)/cputest: tools/cputest.c $(CORE_OBJS)
	$(CC) $(CFLAGS) $^ -o $@

tools: $(BUILD)/sallyrun $(BUILD)/cputest

# Klaus Dormann's 6502 functional test, downloaded on first use.
KLAUS = $(BUILD)/6502_functional_test.bin
$(KLAUS): | $(BUILD)
	curl -sSfL -o $@ https://raw.githubusercontent.com/Klaus2m5/6502_65C02_functional_tests/master/bin_files/6502_functional_test.bin

test: $(BUILD)/cputest $(KLAUS)
	$(BUILD)/cputest $(KLAUS)

# Runs the build directly: `open` would register build/Sally.app with
# Launch Services next to the installed copy.
run: $(EXE) $(ICON)
	$(EXE) $(if $(CONTROL),-control "$(CONTROL)") $(if $(ROM),"$(ROM)")

# Installs the app and registers it with Launch Services, so Finder opens
# cartridges with the installed copy rather than the ones in build/. The
# ad hoc signature covers the whole bundle, not only the linker-signed
# executable.
install: $(EXE) $(ICON)
	rm -rf "$(INSTALL_DIR)/Sally.app"
	ditto $(APP) "$(INSTALL_DIR)/Sally.app"
	codesign --force --sign - "$(INSTALL_DIR)/Sally.app"
	$(LSREGISTER) -f "$(INSTALL_DIR)/Sally.app"
	@$(LSREGISTER) -u $(APP) 2>/dev/null || true
	@$(LSREGISTER) -u $(BUILD)/dmg/Sally.app 2>/dev/null || true

uninstall:
	-$(LSREGISTER) -u "$(INSTALL_DIR)/Sally.app" 2>/dev/null
	rm -rf "$(INSTALL_DIR)/Sally.app"

# Signs the app with the hardened runtime, as notarization requires.
sign: $(EXE) $(ICON)
	codesign --force --options runtime --timestamp --sign "$(SIGN_IDENTITY)" $(APP)
	codesign --verify --strict --verbose=2 $(APP)

# A signed disk image with the app and a link to /Applications.
dmg: sign
	rm -rf $(BUILD)/dmg $(DMG)
	mkdir -p $(BUILD)/dmg
	ditto $(APP) $(BUILD)/dmg/Sally.app
	ln -s /Applications $(BUILD)/dmg/Applications
	hdiutil create -volname Sally -srcfolder $(BUILD)/dmg -format UDZO -ov $(DMG)
	codesign --force --timestamp --sign "$(SIGN_IDENTITY)" $(DMG)

# Sends the image to Apple's notary service and staples the ticket to it.
# The credentials stay in the environment so make does not echo them.
notarize: dmg
	xcrun notarytool submit $(DMG) --apple-id "$$APPLE_ID" --password "$$APPLE_PASSWORD" \
		--team-id "$$APPLE_TEAM_ID" --wait
	xcrun stapler staple $(DMG)
	spctl --assess --type open --context context:primary-signature --verbose=2 $(DMG)

clean:
	rm -rf $(BUILD)

.PHONY: all tools test run install uninstall sign dmg notarize clean

-include $(CORE_OBJS:.o=.d) $(FRONT_OBJS:.o=.d)
