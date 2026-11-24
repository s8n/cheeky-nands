#---------------------------------------------------------------------------------
# cheeky-nands
#
#   PLATFORM=switch2 (default)  Switch 2 nro, built with libounce
#   PLATFORM=switch             Switch 1 nro, built with libnx
#
# Both platforms are built from the same sources: libounce is libnx's Switch 2 counterpart
# and exposes the same API for everything this app uses. build.sh picks the docker image.
#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)
PLATFORM ?= switch2
ifeq ($(PLATFORM),switch2)
include $(DEVKITPRO)/libounce/switch2_rules
else ifeq ($(PLATFORM),switch)
include $(DEVKITPRO)/libnx/switch_rules
else
$(error PLATFORM must be switch or switch2)
endif

#---------------------------------------------------------------------------------
TARGET		:=	cheeky-nands$(if $(filter switch2,$(PLATFORM)),-switch2)
APP_TITLE	:=	cheeky-nands
APP_AUTHOR	:=	ave
APP_VERSION	:=	0.1
BUILD		:=	build$(if $(filter switch2,$(PLATFORM)),-switch2)
SOURCES		:=	source
DATA		:=	data
INCLUDES	:=	include

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
# The SDK's rules define SWITCH2_CPU (Cortex-A78C flags).
ifeq ($(PLATFORM),switch2)
ARCH	:=	$(SWITCH2_CPU) -mtp=soft -fPIE
else
ARCH	:=	-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
endif

CFLAGS	:=	-g -Wall -Wextra -O2 -ffunction-sections \
			$(ARCH) $(DEFINES)

CFLAGS	+=	$(INCLUDE) -D__SWITCH__

CXXFLAGS	:=	$(CFLAGS) -fno-rtti -fno-exceptions

ASFLAGS	:=	-g $(ARCH)
LDFLAGS	=	-specs=$(LIBNX)/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

LIBS	:= -lnx

#---------------------------------------------------------------------------------
# list of directories containing libraries, this must be the top level containing
# include and lib
#---------------------------------------------------------------------------------
LIBDIRS	:= $(PORTLIBS) $(LIBNX)


#---------------------------------------------------------------------------------
# no real need to edit anything past this point unless you need to add additional
# rules for additional file extensions
#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export TOPDIR	:=	$(CURDIR)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
			$(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

#---------------------------------------------------------------------------------
# use CXX for linking C++ projects, CC for standard C
#---------------------------------------------------------------------------------
ifeq ($(strip $(CPPFILES)),)
	export LD	:=	$(CC)
else
	export LD	:=	$(CXX)
endif

export OFILES_BIN	:=	$(addsuffix .o,$(BINFILES))
export OFILES_SRC	:=	$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES 	:=	$(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN	:=	$(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			-I$(CURDIR)/$(BUILD)

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)

ifeq ($(strip $(ICON)),)
	icons := $(wildcard *.jpg)
	ifneq (,$(findstring icon.jpg,$(icons)))
		export APP_ICON := $(TOPDIR)/icon.jpg
	endif
endif

ifeq ($(strip $(NO_ICON)),)
	export NROFLAGS += --icon=$(APP_ICON)
endif

ifeq ($(strip $(NO_NACP)),)
	export NROFLAGS += --nacp=$(CURDIR)/$(TARGET).nacp
endif

.PHONY: $(BUILD) clean all

#---------------------------------------------------------------------------------
all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

#---------------------------------------------------------------------------------
# Only this platform's build directory and outputs; the other platform's are left alone.
clean:
	@echo clean ...
	@rm -fr $(BUILD) $(OUTPUT).nro $(OUTPUT).nacp $(OUTPUT).elf

#---------------------------------------------------------------------------------
else
.PHONY:	all

DEPENDS	:=	$(OFILES:.o=.d)

#---------------------------------------------------------------------------------
# main targets
#---------------------------------------------------------------------------------
all	:	$(OUTPUT).nro

$(OUTPUT).nro	:	$(OUTPUT).elf $(OUTPUT).nacp

$(OUTPUT).elf	:	$(OFILES)

$(OFILES_SRC)	: $(HFILES_BIN)

-include $(DEPENDS)

#---------------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------------
