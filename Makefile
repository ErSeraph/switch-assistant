TARGET := switch-ha
BUILD := build
SOURCES := source
DATA := romfs
INCLUDES := include
ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>devkitPro")
endif

TOPDIR ?= $(CURDIR)

APP_TITLE ?= Switch Assistant
APP_AUTHOR ?= ErSeraph
APP_VERSION ?= 1.0
APP_ICON ?= $(TOPDIR)/assets/icon.jpg

ARCH := -march=armv8-a -mtune=cortex-a57 -mtp=soft -fPIE

CFLAGS := -g -Wall -O2 -ffunction-sections $(ARCH)
CFLAGS += -D__SWITCH__ -DCURL_STATICLIB
CXXFLAGS := $(CFLAGS) -fno-rtti -fno-exceptions
ASFLAGS := -g $(ARCH)
LDFLAGS := -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)
LD = $(CC)

PORTLIBS := $(DEVKITPRO)/portlibs/switch
LIBNX := $(DEVKITPRO)/libnx
LIBDIRS := $(PORTLIBS) $(LIBNX)
LIBS := -lcurl -lz -lnx

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT := $(CURDIR)/$(TARGET)
export TOPDIR
ifneq ($(wildcard $(APP_ICON)),)
export APP_ICON
export NROFLAGS := --nacp=$(OUTPUT).nacp --icon=$(APP_ICON)
else
export NROFLAGS := --nacp=$(OUTPUT).nacp
endif

export APP_TITLE APP_AUTHOR APP_VERSION

export VPATH := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
	$(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR := $(CURDIR)/$(BUILD)

CFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES := $(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

export OFILES_BIN := $(addsuffix .o,$(BINFILES))
export OFILES_SRC := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES := $(OFILES_BIN) $(OFILES_SRC)

export HFILES := $(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
	$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
	-I$(CURDIR)/$(BUILD)

export CPPFLAGS := $(INCLUDE)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

export ARCH CPPFLAGS CFLAGS CXXFLAGS ASFLAGS LDFLAGS LIBS LD

.PHONY: all clean nsp

all:
	@mkdir -p $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile all

nsp:
	@mkdir -p $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile nsp

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).nsp $(TARGET).nacp $(TARGET).pfs0 $(TARGET).nso $(TARGET).elf $(TARGET).lst $(TARGET).map

else

include $(DEVKITPRO)/libnx/switch_rules

DEPENDS := $(OFILES:.o=.d)

all: $(OUTPUT).nro

nsp: $(OUTPUT).nsp

$(OUTPUT).nro: $(OUTPUT).nacp
ifneq ($(wildcard $(APP_ICON)),)
$(OUTPUT).nro: $(APP_ICON)
endif

$(OUTPUT).elf: $(OFILES)

-include $(DEPENDS)

endif
