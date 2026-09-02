# SPDX-License-Identifier: GPL-2.0-or-later

SHELL := /bin/sh

GIT ?= git
BLUEZ_GIT ?= https://github.com/bluez/bluez.git
BLUEZ_REF ?= 5.87

BUILD_ROOT ?= .build
BLUEZ_DIR ?= $(BUILD_ROOT)/bluez

JOBS ?= $(shell getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)

# Installation layout. Defaults match the common system-wide Linux layout and
# can be overridden by distributions/users:
#
#   make PREFIX=/usr/local LIBEXECDIR=/usr/local/lib
#
PREFIX ?= /usr
SYSCONFDIR ?= /etc
LOCALSTATEDIR ?= /var
LIBDIR ?= $(PREFIX)/lib
LIBEXECDIR ?= $(LIBDIR)
MANDIR ?= $(PREFIX)/share/man

CONFIGURE_ARGS ?= \
	--prefix=$(PREFIX) \
	--libdir=$(LIBDIR) \
	--libexecdir=$(LIBEXECDIR) \
	--sysconfdir=$(SYSCONFDIR) \
	--localstatedir=$(LOCALSTATEDIR) \
	--mandir=$(MANDIR)

FETCH_STAMP := $(BLUEZ_DIR)/.switch2-fetched
PREPARE_STAMP := $(BLUEZ_DIR)/.switch2-integrated
CONFIGURE_STAMP := $(BLUEZ_DIR)/.switch2-configured

.PHONY: all fetch prepare configure build check install uninstall clean distclean info

all: build

info:
	@echo "BlueZ repository : $(BLUEZ_GIT)"
	@echo "BlueZ ref        : $(BLUEZ_REF)"
	@echo "BlueZ source     : $(BLUEZ_DIR)"
	@echo "Prefix           : $(PREFIX)"
	@echo "Libdir           : $(LIBDIR)"
	@echo "Libexecdir       : $(LIBEXECDIR)"
	@echo "Sysconfdir       : $(SYSCONFDIR)"
	@echo "Configure args   : $(CONFIGURE_ARGS)"

fetch: $(FETCH_STAMP)

$(FETCH_STAMP):
	@command -v $(GIT) >/dev/null 2>&1 || { echo "error: git is required" >&2; exit 1; }
	@mkdir -p "$(BUILD_ROOT)"
	@if test ! -d "$(BLUEZ_DIR)/.git"; then \
		echo "Cloning BlueZ..."; \
		$(GIT) clone --no-checkout "$(BLUEZ_GIT)" "$(BLUEZ_DIR)"; \
	fi
	@echo "Checking out BlueZ $(BLUEZ_REF)..."
	@$(GIT) -C "$(BLUEZ_DIR)" fetch --force --tags origin "$(BLUEZ_REF)"
	@$(GIT) -C "$(BLUEZ_DIR)" checkout --detach --force FETCH_HEAD
	@touch "$@"

prepare: $(PREPARE_STAMP)

$(PREPARE_STAMP): switch2.c $(FETCH_STAMP)
	@test -f "$(BLUEZ_DIR)/Makefile.plugins" || { \
		echo "error: BlueZ Makefile.plugins not found" >&2; exit 1; }
	@grep -q '^builtin_modules[[:space:]]*=' "$(BLUEZ_DIR)/Makefile.am" || { \
		echo "error: unsupported BlueZ build layout (builtin_modules missing)" >&2; exit 1; }
	@cp switch2.c "$(BLUEZ_DIR)/plugins/switch2.c"
	@if ! grep -q '^builtin_modules += switch2$$' "$(BLUEZ_DIR)/Makefile.plugins"; then \
		printf '\n# Nintendo Joy-Con 2 BLE transport\nbuiltin_modules += switch2\nbuiltin_sources += plugins/switch2.c\n' \
			>> "$(BLUEZ_DIR)/Makefile.plugins"; \
	fi
	@touch "$@"

configure: $(CONFIGURE_STAMP)

$(CONFIGURE_STAMP): $(PREPARE_STAMP)
	@echo "Bootstrapping BlueZ..."
	@cd "$(BLUEZ_DIR)" && ./bootstrap
	@echo "Configuring BlueZ..."
	@cd "$(BLUEZ_DIR)" && ./configure $(CONFIGURE_ARGS)
	@touch "$@"

build: $(CONFIGURE_STAMP)
	@$(MAKE) -C "$(BLUEZ_DIR)" -j"$(JOBS)"

check: build
	@test -f "$(BLUEZ_DIR)/src/builtin.h" || { \
		echo "error: BlueZ builtin plugin table was not generated" >&2; exit 1; }
	@grep -q 'switch2' "$(BLUEZ_DIR)/src/builtin.h" || { \
		echo "error: switch2 is not present in BlueZ builtin plugin table" >&2; exit 1; }
	@strings "$(BLUEZ_DIR)/src/bluetoothd" | grep -q 'switch2:' || { \
		echo "error: switch2 strings are absent from built bluetoothd" >&2; exit 1; }
	@echo "switch2 is built into $(BLUEZ_DIR)/src/bluetoothd"

install: check
	@echo "Installing the complete BlueZ build under DESTDIR='$(DESTDIR)'..."
	@$(MAKE) -C "$(BLUEZ_DIR)" DESTDIR="$(DESTDIR)" install
	@echo
	@echo "BlueZ with the built-in switch2 plugin is installed."
	@echo "Restart bluetoothd using your distribution's service manager."

uninstall:
	@test -f "$(BLUEZ_DIR)/Makefile" || { \
		echo "error: no configured BlueZ tree; nothing to uninstall" >&2; exit 1; }
	@echo "WARNING: this invokes upstream BlueZ 'make uninstall'."
	@echo "If files overlap a distro package, reinstall that package afterwards."
	@$(MAKE) -C "$(BLUEZ_DIR)" DESTDIR="$(DESTDIR)" uninstall

clean:
	@if test -f "$(BLUEZ_DIR)/Makefile"; then \
		$(MAKE) -C "$(BLUEZ_DIR)" clean; \
	fi
	@rm -f "$(CONFIGURE_STAMP)"

distclean:
	rm -rf "$(BUILD_ROOT)"
