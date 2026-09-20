#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

TARGET      := NearChuckle_nx
APP_TITLE   := Far Cry
APP_AUTHOR  := artslay
APP_VERSION := 1.0.0
BUILD       := build
SOURCES     := source vnx/source/compat
DATA        :=
INCLUDES    := source vnx/include

MESA_SDK    := $(TOPDIR)/mesa-sdk/opt/devkitpro/portlibs/switch
LIBDIRS     := $(MESA_SDK) $(PORTLIBS) $(LIBNX)

ARCH := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

CFLAGS := -g -Wall -O2 -ffunction-sections \
          $(ARCH) $(DEFINES)
CFLAGS += $(INCLUDE) -I$(DEVKITPRO)/portlibs/switch/include \
          -D__SWITCH__ -D_GNU_SOURCE -fno-stack-protector
CXXFLAGS := $(CFLAGS) -std=gnu++17 -fno-rtti -fno-exceptions
ASFLAGS := -g $(ARCH)

LDFLAGS = -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) \
          -Wl,--allow-multiple-definition -Wl,-Map,$(notdir $*.map)

LIBS := -pthread \
        -Wl,-u,vk_icdGetInstanceProcAddr \
        -Wl,-u,vk_icdNegotiateLoaderICDInterfaceVersion \
        -Wl,--start-group \
        -l:libGL.a -l:libGLESv1_CM.a -l:libGLESv2.a -l:libEGL.a -l:libvulkan.a \
        -l:libglapi.a -l:libcompiler.a -l:libmesa_util_c11.a -l:libblake3.a \
        -l:libmesa_util.a -l:libmesa_util_simd.a -l:libxmlconfig.a \
        -lexpat -lz \
        -lnx -lstdc++ -lm \
        -Wl,--end-group

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT := $(CURDIR)/$(TARGET)
export TOPDIR := $(CURDIR)
export VPATH := $(foreach dir,$(SOURCES),$(TOPDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)

CFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))

export LD := $(CXX)
export OFILES := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)

export INCLUDE := $(foreach dir,$(INCLUDES),-I$(TOPDIR)/$(dir)) \
                 $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                 -I$(TOPDIR)/$(BUILD)

export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

ifeq ($(strip $(ICON)),)
icons := $(wildcard *.jpg)
ifneq (,$(findstring $(TARGET).jpg,$(icons)))
export APP_ICON := $(TOPDIR)/$(TARGET).jpg
else ifneq (,$(findstring icon.jpg,$(icons)))
export APP_ICON := $(TOPDIR)/icon.jpg
endif
else
export APP_ICON := $(TOPDIR)/$(ICON)
endif

ifneq ($(strip $(APP_ICON)),)
export NROFLAGS += --icon=$(APP_ICON)
endif

export NROFLAGS += --nacp=$(CURDIR)/$(TARGET).nacp

.PHONY: all clean $(BUILD)
all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf

else

DEPENDS := $(OFILES:.o=.d)
all: $(OUTPUT).nro
$(OUTPUT).nro: $(OUTPUT).elf $(OUTPUT).nacp
$(OUTPUT).elf: $(OFILES)

-include $(DEPENDS)

endif
