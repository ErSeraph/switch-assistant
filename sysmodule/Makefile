TARGET := switch-ha-sysmodule
BUILD := build
SOURCES := source ../source
INCLUDES := ../include
CONFIG_JSON := switch-ha-sysmodule.json
NO_ICON := 1
NO_NACP := 1

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>devkitPro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

ARCH := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

CFLAGS := -g -Wall -O2 -ffunction-sections $(ARCH)
CFLAGS += -D__SWITCH__ -DCURL_STATICLIB
CXXFLAGS := $(CFLAGS) -fno-rtti -fno-exceptions
ASFLAGS := -g $(ARCH)
LDFLAGS := -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

PORTLIBS := $(DEVKITPRO)/portlibs/switch
LIBNX := $(DEVKITPRO)/libnx
LIBDIRS := $(PORTLIBS) $(LIBNX)
LIBS := -lnx

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT := $(CURDIR)/$(TARGET)
export TOPDIR
export APP_JSON := $(CURDIR)/$(CONFIG_JSON)
export DEPSDIR := $(CURDIR)/$(BUILD)

export VPATH := $(CURDIR)/source $(CURDIR)/../source
export OFILES := main.o app_state.o config.o mqtt_client.o title_cache.o

export INCLUDE := -I$(CURDIR)/../include \
	$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
	-I$(CURDIR)/$(BUILD)

export CPPFLAGS := $(INCLUDE)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)
export LD := $(CC)
export ARCH CPPFLAGS CFLAGS CXXFLAGS ASFLAGS LDFLAGS LIBS LD

.PHONY: all clean install-layout

all:
	@mkdir -p $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

install-layout: all
	@mkdir -p atmosphere/contents/00FF000053484101/flags
	@cp $(TARGET).nsp atmosphere/contents/00FF000053484101/exefs.nsp
	@touch atmosphere/contents/00FF000053484101/flags/boot2.flag

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).nsp $(TARGET).nso $(TARGET).npdm $(TARGET).elf $(TARGET).lst atmosphere

else

include $(DEVKITPRO)/libnx/switch_rules

DEPENDS := $(OFILES:.o=.d)

all: $(OUTPUT).nsp

$(OUTPUT).nsp: $(OUTPUT).nso $(OUTPUT).npdm

$(OUTPUT).elf: $(OFILES)

-include $(DEPENDS)

endif
