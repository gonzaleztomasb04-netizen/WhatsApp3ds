
ifeq ($(strip $(DEVKITARM)),)
$(error Define DEVKITARM)
endif
include $(DEVKITARM)/3ds_rules
.RECIPEPREFIX := >

TARGET  := whatsapp3ds
CTRULIB := $(DEVKITPRO)/libctru
ARCH    := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
CFLAGS  := -g -Wall -O2 -mword-relocations -ffunction-sections $(ARCH) -D__3DS__ -I$(CTRULIB)/include
LDFLAGS := -specs=3dsx.specs -g $(ARCH) -L$(CTRULIB)/lib
LIBS    := -lctru -lm

all: $(TARGET).3dsx

$(TARGET).elf: source/main.c
> $(CC) $(CFLAGS) $< -o $@ $(LDFLAGS) $(LIBS)

$(TARGET).smdh:
> smdhtool --create "WhatsApp 3DS" "Cliente WhatsApp" "Autor" $(CTRULIB)/default_icon.png $@

$(TARGET).3dsx: $(TARGET).elf $(TARGET).smdh
> 3dsxtool $(TARGET).elf $@ --smdh=$(TARGET).smdh

clean:
> rm -f *.elf *.smdh *.3dsx
