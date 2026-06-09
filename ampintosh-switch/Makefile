#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

#---------------------------------------------------------------------------------
# TARGET is the name of the output
# BUILD is the directory where object files & intermediate files will be placed
# SOURCES is a list of directories containing source code
# DATA is a list of directories containing data files
# INCLUDES is a list of directories containing header files
# ROMFS is the directory containing data to be added to RomFS, relative to the Makefile (Optional)
#
# NO_ICON: if set to anything, do not use icon.
# NO_NACP: if set to anything, no .nacp file is generated.
# APP_TITLE is the name of the app stored in the .nacp file (Optional)
# APP_AUTHOR is the author of the app stored in the .nacp file (Optional)
# APP_VERSION is the version of the app stored in the .nacp file (Optional)
# APP_TITLEID is the titleID of the app stored in the .nacp file (Optional)
# ICON is the filename of the icon (.jpg), relative to the project folder.
#   If not set, it attempts to use one of the following (in this order):
#     - <Project name>.jpg
#     - icon.jpg
#     - <libnx folder>/default_icon.jpg
#
# CONFIG_JSON is the filename of the NPDM config file (.json), relative to the project folder.
#   If not set, it attempts to use one of the following (in this order):
#     - <Project name>.json
#     - config.json
#   If a JSON file is provided or autodetected, an ExeFS PFS0 (.nsp) is built instead
#   of a homebrew executable (.nro). This is intended to be used for sysmodules.
#   NACP building is skipped as well.
#---------------------------------------------------------------------------------
TARGET		:=	Ampintosh
BUILD		:=	build
SOURCES		:=	source
DATA		:=	data
INCLUDES	:=	include
ROMFS	:=	romfs

APP_TITLE	:=	Ampintosh
APP_AUTHOR	:=	Kiwi Singh
APP_VERSION	:=	1.5.6
ICON		:=	Ampintosh.jpg

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
ARCH	:=	-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

PKGCONF	:=	$(DEVKITPRO)/portlibs/switch/bin/aarch64-none-elf-pkg-config

#---------------------------------------------------------------------------------
# Optional features (opt-in). The default build is SD-card only with no extra
# dependencies. Enable on the make command line:
#   make USB=1   USB mass storage   (needs libusbhsfs; see install.sh / README)
#   make NET=1   network streams    (needs switch-curl)
#   make USB=1 NET=1   both
#---------------------------------------------------------------------------------
FEATURE_DEFINES	:=
FEATURE_CFLAGS	:=
FEATURE_LIBS	:=

ifeq ($(USB),1)
	USBHSFS_HEADER := $(DEVKITPRO)/portlibs/switch/include/usbhsfs.h
	USBHSFS_LIB    := $(DEVKITPRO)/portlibs/switch/lib/libusbhsfs.a
ifeq ($(wildcard $(USBHSFS_HEADER)),)
$(error USB=1 requires libusbhsfs, but $(USBHSFS_HEADER) was not found. Run ./install.sh --usb, or install DarkMatterCore/libusbhsfs with: make BUILD_TYPE=ISC install)
endif
ifeq ($(wildcard $(USBHSFS_LIB)),)
$(error USB=1 requires libusbhsfs, but $(USBHSFS_LIB) was not found. Run ./install.sh --usb, or install DarkMatterCore/libusbhsfs with: make BUILD_TYPE=ISC install)
endif
	FEATURE_DEFINES	+= -DAMPINTOSH_USB
	FEATURE_LIBS	+= -lusbhsfs
# libusbhsfs ISC builds need no extra filesystem libs. If you installed
# the GPL build instead, pass USB_BUILD_TYPE=GPL so NTFS/ext deps are linked.
ifeq ($(USB_BUILD_TYPE),GPL)
	FEATURE_LIBS	+= -lntfs-3g -llwext4
endif
endif

ifeq ($(NET),1)
	FEATURE_DEFINES	+= -DAMPINTOSH_NET
	FEATURE_CFLAGS	+= `$(PKGCONF) --cflags libcurl`
	FEATURE_NET_LIBS := `$(PKGCONF) --libs --static libcurl`

	# Optional remote directory browsers. If these portlibs are present,
	# Ampintosh enables real NXMP-style SMB/SFTP folder browsing. If not,
	# stream_url/direct network file playback still builds through libcurl.
ifeq ($(shell $(PKGCONF) --exists libssh2 2>/dev/null && echo yes),yes)
	FEATURE_DEFINES	+= -DAMPINTOSH_SFTP
	FEATURE_CFLAGS	+= `$(PKGCONF) --cflags libssh2`
	FEATURE_NET_LIBS += `$(PKGCONF) --libs --static libssh2`
endif
ifeq ($(shell $(PKGCONF) --exists libsmb2 2>/dev/null && echo yes),yes)
	FEATURE_DEFINES	+= -DAMPINTOSH_SMB
	FEATURE_CFLAGS	+= `$(PKGCONF) --cflags libsmb2`
	FEATURE_NET_LIBS += `$(PKGCONF) --libs --static libsmb2`
endif
endif

CFLAGS	:=	-g -Wall -O2 -ffunction-sections \
			$(ARCH) $(DEFINES) $(FEATURE_DEFINES)

CFLAGS	+=	-D__SWITCH__ $(INCLUDE) `$(PKGCONF) --cflags SDL2_image SDL2_ttf SDL2` $(FEATURE_CFLAGS)

CXXFLAGS	:= $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++17

ASFLAGS	:=	-g $(ARCH)
LDFLAGS	=	-specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

# usbhsfs is listed before the SDL/pkg-config chain so it precedes -lnx;
# curl's static deps come after. switch.specs appends -lnx last to catch all.
LIBS	:=	$(FEATURE_LIBS) `$(PKGCONF) --libs --static SDL2_image SDL2_ttf SDL2` $(FEATURE_NET_LIBS) -lm

#---------------------------------------------------------------------------------
# list of directories containing libraries, this must be the top level containing
# include and lib
#---------------------------------------------------------------------------------
LIBDIRS	:= $(PORTLIBS) $(LIBNX)


#---------------------------------------------------------------------------------
# no real need to edit anything past this point unless you need to add additional
# rules for different file extensions
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
#---------------------------------------------------------------------------------
	export LD	:=	$(CC)
#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------
	export LD	:=	$(CXX)
#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------

export OFILES_BIN	:=	$(addsuffix .o,$(BINFILES))
export OFILES_SRC	:=	$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES 	:=	$(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN	:=	$(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			-I$(CURDIR)/$(BUILD)

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)

ifeq ($(strip $(CONFIG_JSON)),)
	jsons := $(wildcard *.json)
	ifneq (,$(findstring $(TARGET).json,$(jsons)))
		export APP_JSON := $(TOPDIR)/$(TARGET).json
	else
		ifneq (,$(findstring config.json,$(jsons)))
			export APP_JSON := $(TOPDIR)/config.json
		endif
	endif
else
	export APP_JSON := $(TOPDIR)/$(CONFIG_JSON)
endif

ifeq ($(strip $(ICON)),)
	icons := $(wildcard *.jpg)
	ifneq (,$(findstring $(TARGET).jpg,$(icons)))
		export APP_ICON := $(TOPDIR)/$(TARGET).jpg
	else
		ifneq (,$(findstring icon.jpg,$(icons)))
			export APP_ICON := $(TOPDIR)/icon.jpg
		endif
	endif
else
	export APP_ICON := $(TOPDIR)/$(ICON)
endif

ifeq ($(strip $(NO_ICON)),)
	export NROFLAGS += --icon=$(APP_ICON)
endif

ifeq ($(strip $(NO_NACP)),)
	export NROFLAGS += --nacp=$(CURDIR)/$(TARGET).nacp
endif

ifneq ($(APP_TITLEID),)
	export NACPFLAGS += --titleid=$(APP_TITLEID)
endif

ifneq ($(ROMFS),)
	export NROFLAGS += --romfsdir=$(CURDIR)/$(ROMFS)
endif

.PHONY: $(BUILD) clean all

#---------------------------------------------------------------------------------
all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

#---------------------------------------------------------------------------------
clean:
	@echo clean ...
ifeq ($(strip $(APP_JSON)),)
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf
else
	@rm -fr $(BUILD) $(TARGET).nsp $(TARGET).nso $(TARGET).npdm $(TARGET).elf
endif


#---------------------------------------------------------------------------------
else
.PHONY:	all

DEPENDS	:=	$(OFILES:.o=.d)

#---------------------------------------------------------------------------------
# main targets
#---------------------------------------------------------------------------------
ifeq ($(strip $(APP_JSON)),)

all	:	$(OUTPUT).nro

ifeq ($(strip $(NO_NACP)),)
$(OUTPUT).nro	:	$(OUTPUT).elf $(OUTPUT).nacp
else
$(OUTPUT).nro	:	$(OUTPUT).elf
endif

else

all	:	$(OUTPUT).nsp

$(OUTPUT).nsp	:	$(OUTPUT).nso $(OUTPUT).npdm

$(OUTPUT).nso	:	$(OUTPUT).elf

endif

$(OUTPUT).elf	:	$(OFILES)

$(OFILES_SRC)	: $(HFILES_BIN)

#---------------------------------------------------------------------------------
# you need a rule like this for each extension you use as binary data
#---------------------------------------------------------------------------------
%.bin.o	%_bin.h :	%.bin
#---------------------------------------------------------------------------------
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)

#---------------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------------
