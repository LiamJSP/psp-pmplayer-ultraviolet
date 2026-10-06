SHELL := /bin/sh
ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
DIST ?= $(ROOT)/dist/PPU-1.0
CONFIG ?= config-en.xml
FONT_SUBSETS := $(ROOT)/generated-fonts
MINICONV ?= miniconv.eur.prx
I18N ?= i18n.eng.prx

# Detect Windows (OS environment variable is Windows_NT on Windows systems)
ifeq ($(OS),Windows_NT)
	SETUP_BOOST_CMD := powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$(ROOT)/tools/setup_boost_headers.ps1"
	BUILD_FONTS_CMD := powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$(ROOT)/ppu/tools/build_psp_subtitle_fonts.ps1"
	STAGE_PKG_CMD := powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$(ROOT)/tools/stage_psp_package.ps1"
	RM_FONTS_CMD := powershell.exe -NoProfile -Command "if (Test-Path '$(FONT_SUBSETS)') { Remove-Item -Recurse -Force '$(FONT_SUBSETS)' }"
else
	SETUP_BOOST_CMD := sh "$(ROOT)/tools/setup_boost_headers.sh"
	BUILD_FONTS_CMD := sh "$(ROOT)/ppu/tools/build_psp_subtitle_fonts.sh"
	STAGE_PKG_CMD := sh "$(ROOT)/tools/stage_psp_package.sh"
	RM_FONTS_CMD := rm -rf "$(FONT_SUBSETS)"
endif

.DEFAULT_GOAL := all
.PHONY: all psp setup_boost deps eboot3xx ppu3xx stage package ppu3xxen ppu3xxzh ppa3xx ppa3xxen ppa3xxzh fonts clean

all psp: stage

setup_boost:
	$(SETUP_BOOST_CMD)

# Making 'deps' rely on 'setup_boost' forces Make to run and complete
# the setup script synchronously before beginning any of the $(MAKE) calls.
deps: setup_boost
	$(MAKE) -C "$(ROOT)/libmpeg"
	$(MAKE) -C "$(ROOT)/cooleyesBridge"
	$(MAKE) -C "$(ROOT)/libbufferedio.psp"
	$(MAKE) -C "$(ROOT)/libmp4info.psp"
	$(MAKE) -C "$(ROOT)/libmkvinfo.psp"
	$(MAKE) -C "$(ROOT)/ppu/miniconv"
	$(MAKE) -C "$(ROOT)/ppu/i18n"

eboot3xx: deps
	$(MAKE) -C "$(ROOT)/ppu" ppu3xx

ppu3xx ppa3xx: eboot3xx

fonts:
	$(BUILD_FONTS_CMD) "$(ROOT)/ppu/extra/fonts" "$(FONT_SUBSETS)"

stage package: eboot3xx fonts
	$(STAGE_PKG_CMD) "$(ROOT)/ppu" "$(DIST)" "$(CONFIG)" "$(MINICONV)" "$(I18N)" "$(FONT_SUBSETS)"

ppu3xxen ppa3xxen:
	$(MAKE) stage CONFIG=config-en.xml MINICONV=miniconv.eur.prx I18N=i18n.eng.prx

ppu3xxzh ppa3xxzh:
	$(MAKE) stage CONFIG=config.xml MINICONV=miniconv.cjk.prx I18N=i18n.chs.prx

clean:
	$(RM_FONTS_CMD)
	$(MAKE) -C "$(ROOT)/libmpeg" clean
	$(MAKE) -C "$(ROOT)/cooleyesBridge" clean
	$(MAKE) -C "$(ROOT)/libbufferedio.psp" clean
	$(MAKE) -C "$(ROOT)/libmp4info.psp" clean
	$(MAKE) -C "$(ROOT)/libmkvinfo.psp" clean
	$(MAKE) -C "$(ROOT)/ppu/miniconv" clean
	$(MAKE) -C "$(ROOT)/ppu/i18n" clean
	$(MAKE) -C "$(ROOT)/ppu" clean
