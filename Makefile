# System Shock (Shockolate, the cross-platform port of the source Night
# Dive Studios released) for the m20 SoC: builds sshock.bin, which the core
# firmware's loader copies into SDRAM and runs (see README.md). Requires
# riscv64-unknown-elf-gcc/g++ and picolibc-riscv64-unknown-elf.
#
# `make host` builds tools/host's test harness instead: the same game and
# port code for 32-bit x86 Linux (gcc-multilib, g++-multilib), the span
# engine emulated (HOST_ENGINE=0: without it).

CROSS   ?= riscv64-unknown-elf-
CC      := $(CROSS)gcc
CXX     := $(CROSS)g++
OBJCOPY := $(CROSS)objcopy
SIZE    := $(CROSS)size

# The m20 core (../core): its firmware (SD card, the flash disk, FatFs) and
# the runtime the programs share (system calls, trap handler, linker script).
CORE ?= ../core
CORE_FW := $(CORE)/firmware
CORE_RT := $(CORE)/runtime
S := shockolate/src
L := $(S)/Libraries

# Memory map (SDRAM, $(CORE)/runtime/m9b.ld): the image (code, rodata,
# .data) from 0x800000, then bss and the heap up to a 128 KB stack at the
# top.

B ?= build

INCLUDES := -Iport $(addprefix -I$(L)/,2D/Source 3D/Source AFILE/Source DSTRUCT/Source EDMS/Source \
              EDMS/Source/MODELS FIXPP/Source INPUT/Source PALETTE/Source RES/Source UI/Source RND/Source \
              VOX/Source FIX/Source SND/Source H LG/Source LG/Source/LOG/src 2D/Source/GR 2D/Source/Clip \
              2D/Source/Flat8 2D/Source/Gen 2D/Source/RSD) \
            -I$(S)/GameSrc/Headers -I$(S)/MacSrc -I$(CORE_FW) -I$(CORE_FW)/fatfs
WARN     := -Wall -Wno-unused-but-set-variable -Wno-unused-variable -Wno-parentheses -Wno-char-subscripts \
            -Wno-format -Wno-unused-function -Wno-missing-braces -Wno-unused-label -Wno-address \
            -Wno-misleading-indentation -Wno-unused-value -Wno-switch -Wno-maybe-uninitialized \
            -Wno-array-bounds -Wno-dangling-else -Wno-comment -Wno-format-overflow -Wno-stringop-overflow \
            -Wno-sizeof-pointer-memaccess -Wno-unknown-pragmas -Wno-stringop-truncation -Wno-restrict
CWARN    := -Wno-pointer-sign -Wno-implicit-function-declaration -Wno-int-conversion \
            -Wno-incompatible-pointer-types -Wno-array-parameter
# (The game is built as upstream builds it: -fsigned-char, SVGA_SUPPORT.)
GAMEFLAGS := -fsigned-char -fno-strict-aliasing -fcommon -DSVGA_SUPPORT -DNDEBUG

# The m20's CPU: the F extension, and Zicboz (the runtime's memset)
ARCH     := -march=rv32imf_zba_zicbop_zicsr_zifencei_zicbom_zicboz -mabi=ilp32f
COMMON   := $(ARCH) -O2 -g --picolibc-buildtype=release -ffunction-sections -fdata-sections \
            $(GAMEFLAGS) -DM9B -DM20 -DM20_ENGINE -DFF_FS_READONLY=0 $(INCLUDES) $(WARN)
CFLAGS   := $(COMMON) -specs=picolibc.specs -std=gnu99 $(CWARN)
CXXFLAGS := $(COMMON) -specs=picolibcpp.specs -std=gnu++11 -fno-exceptions -fno-rtti
LDFLAGS  := $(ARCH) -specs=picolibc.specs --picolibc-buildtype=release \
            -Wl,--defsym=__stack_size=0x20000 \
            -Wl,--gc-sections -Wl,-Map=$(B)/sshock.map -Wl,--wrap=open -Wl,--wrap=unlink -Wl,--wrap=rename \
            -T$(CORE_RT)/m9b.ld

# The game (upstream's CMakeLists.txt, less the SDL platform files, which
# port/ replaces).
GAME := ai airupt amap amaploop ammomfd anim archiveformat audiolog automap bark biohelp cardmfd \
        citres combat cone criterr cyber cybermfd cybmem cybrnd cutsloop damage digifx drugs effect \
        email faceobj fixtrmfd frcamera frclip frcompil frmain frobj froslew frpipe frpts frsetup \
        frtables frterr frutil FrUtils fullamap fullscrn gameloop gameobj gamesort gamestrn gamesys \
        gametime gamewrap gearmfd gr2ss grenades hand hflip hkeyfunc hud hudobj init input invent \
        leanmetr mainloop map mfdfunc mfdgadg mfdgames mfdgump mfdpanel minimax mlimbs movekeys \
        musicai newai newmfd objapp objects objload objprop objsim objuse olh olhscan palfx \
        pathfind physics player plotware popups render rendtool saveload schedule screen setup \
        shodan sideicon sndcall star statics status target textmaps tickcount tfdirect tfutil tools \
        trigger view360 viewhelp vitals vmail wares weapons wrapper gamerend mouselook
MAC  := InitMac Prefs MacTune Modding

LIB_2D := $(addprefix 2D/Source/, bit bitmap blend canvas close cnvtab context detect devtab fcntab \
            init lintab mode pal permap persetup pertol rgb screen sscrn StateStk svgainit tempbm \
            temptm tlucdat tluctab valloc vtab MacDev) \
          $(addprefix 2D/Source/GR/, grilin grmalloc grnull gruilin grd) \
          $(addprefix 2D/Source/Gen/, gendisk gentm genwclin genuclin genrsd8 genuvlin genhlin genrsdbm \
            genvlin genvcply genclin genfl8c genrsdtm genel genwlin genpix genuhlin genhfl8 genov gente \
            genlin gencwlin genchfl8 gengfl8 genbox genmono genulin gencnv genuslin genvpoly general \
            gencirc genfl8 genf24 gentl8 genrect genvrect genslin) \
          $(addprefix 2D/Source/Flat8/, fl8vlin FL8OPL fl8cply fl8clin fl8lp fl8lf fl8ll fl8s fl8hfl8 \
            fl8pnt fl8fl8m fl8ltp fl8fl8c fl8w fl8cop fl8wlin fl8lw fl8pix fl8row fl8ft fl8rsd8 Fl8F \
            fl8bldbl fl8clear fl8p fl8wclin fl8hlin fl8bl fl8gpix fl8ns fl8lnop fl8dbl fl8cnv fl8slin \
            fl8lin fl8tsmap fl8ply fl8sply fl8rect fl8gfl8 fl8tl8 fl8p24 fl8g24 fl8ctp fl8mscl fl8lop \
            fl8ntrp2 fl8sub fl8tpl fl8nl fl8fltr2 fl8mono fl8fl8 fl8chfl8) \
          $(addprefix 2D/Source/Clip/, clpply clplin clpclin clpltab clpf24 clplin2 clppoly clpslin \
            clprect clpmono) \
          $(addprefix 2D/Source/RSD/, rsdcvt RSDUnpack) \
          $(addprefix 2D/Source/string/, chrsiz genchr genstr genuchr genustr strscl strsiz struscl strwrap)
LIB_C := $(LIB_2D) \
         $(addprefix 3D/Source/, alloc Bitmap clip detail fov GlobalV instance interp light matrix \
           points polygon slew tmap vector) \
         $(addprefix AFILE/Source/, afile amov compose movie) \
         $(addprefix DSTRUCT/Source/, array hash pqueue rect) \
         $(addprefix FIX/Source/, f_exp fix fix_sqrt MakeTables) \
         $(addprefix INPUT/Source/, kbcook mouse) \
         LG/Source/LOG/src/log $(addprefix LG/Source/, memall stack tmpalloc) \
         PALETTE/Source/palette \
         $(addprefix RES/Source/, caseless lzw refacc resacc resbuild res resfile resformat resload \
           resmake restypes) \
         RND/Source/rnd \
         $(addprefix UI/Source/, curdrw cursors event hotkey region slab vmouse) \
         $(addprefix VOX/Source/, vox2d vox3d voxinit)
LIB_CXX := AFILE/Source/draw4x4.cpp AFILE/Source/huffde.cpp FIXPP/Source/fixpp.cpp \
           $(addprefix EDMS/Source/, interfac.cc collide.cc intrsect.cc globals.cc phy_tool.cc soliton.cc) \
           $(addprefix EDMS/Source/MODELS/, robot.cc pelface.cc ftl.cc pelvis.cc d_frame.cc d_f_face.cc \
             ftlface.cc d_f_2.cc)

GAME_SRC := $(GAME:%=$(S)/GameSrc/%.c) $(MAC:%=$(S)/MacSrc/%.c)
PORT_SRC := $(filter-out port/m9b_posix.c,$(wildcard port/*.c))
C_SRC    := $(GAME_SRC) $(LIB_C:%=$(L)/%.c) $(PORT_SRC) port/m9b_posix.c \
            $(CORE_RT)/syscalls.c $(CORE_RT)/string.c $(CORE_RT)/trap.c $(CORE_RT)/shot.c \
            $(CORE_FW)/sd.c $(CORE_FW)/diskio.c $(CORE_FW)/flashdisk.c $(CORE_FW)/fatfs/ff.c $(CORE_FW)/fatfs/ffunicode.c
CXX_SRC  := $(LIB_CXX:%=$(L)/%)
ASM_SRC  := $(CORE_RT)/trap_entry.S

# Objects mirror the sources' paths (several names repeat: init.c, ...).
obj = $(B)/$(subst ../,_/,$(basename $(1))).o
OBJ := $(foreach f,$(C_SRC) $(CXX_SRC) $(ASM_SRC),$(call obj,$(f)))

all: $(B)/sshock.bin

define c_rule
$(call obj,$(1)): $(1)
	@mkdir -p $$(@D)
	$$(CC) $$(CFLAGS) $$(EXTRA) -MMD -MP -c $$< -o $$@
endef
define cxx_rule
$(call obj,$(1)): $(1)
	@mkdir -p $$(@D)
	$$(CXX) $$(CXXFLAGS) $$(EXTRA) -MMD -MP -c $$< -o $$@
endef
$(foreach f,$(C_SRC) $(ASM_SRC),$(eval $(call c_rule,$(f))))
$(foreach f,$(CXX_SRC),$(eval $(call cxx_rule,$(f))))

# The game, MacSrc and the port get upstream's forced include (precompiled.h).
$(foreach f,$(GAME_SRC) $(PORT_SRC),$(call obj,$(f))): CFLAGS += -include precompiled.h
$(foreach f,$(PORT_SRC) port/m9b_posix.c,$(call obj,$(f))): CFLAGS += -std=gnu11
$(call obj,$(CORE_RT)/string.c): CFLAGS += -fno-tree-loop-distribute-patterns -fno-builtin
# The resource files (up to 32) stay open, and a movie opens its own two.
$(call obj,$(CORE_RT)/syscalls.c): CFLAGS += -DMAX_FILES=40
# Only the game needs -fcommon (common symbols aren't garbage-collected:
# trap.c's profile histogram).
$(foreach f,$(filter $(CORE_RT)/% $(CORE_FW)/%,$(C_SRC)),$(call obj,$(f))): CFLAGS += -fno-common

$(B)/sshock.elf: $(OBJ) $(CORE_RT)/m9b.ld Makefile
	$(CC) $(LDFLAGS) $(LDEXTRA) $(OBJ) -lm -o $@
	$(SIZE) $@

$(B)/sshock.bin: $(B)/sshock.elf
	$(OBJCOPY) -O binary $< $@
	@ls -l $@

# -------------------------------------------------------------------------
# The host test harness (tools/host/host.c, README.md): 32-bit x86, so the
# game's structures and pointers are as on the board. The C++ (EDMS) is
# built without M9B, whose additions are for int32_t being long (riscv32).
# As the board (M20, M20_ENGINE), the span engine's register writes going
# to its C model (../core/firmware/span_emu.c), into build-host/;
# HOST_ENGINE=0 leaves the engine out (the CPU draws the whole view), into
# build-host-cpu/. Linked at 0x800000, the game's heap an arena the board's
# heap's size (M20_HEAP: 0xFE0000 - build/sshock.elf's __heap_start when
# it's built), so what the engine reads and writes is in the board's SDRAM
# window.
# -------------------------------------------------------------------------
HOST_ENGINE ?= 1
ifeq ($(HOST_ENGINE),1)
HB ?= build-host
HOST_DEFS := -DM20 -DM20_ENGINE
HOST_EMU := $(CORE_FW)/span_emu.c
else
HB ?= build-host-cpu
HOST_DEFS := -DM20
HOST_EMU :=
endif
BOARD_ELF := build/sshock.elf
M20_HEAP ?= $(if $(wildcard $(BOARD_ELF)),$(shell printf '0x%x' $$(( 0xFE0000 - 0x$$($(CROSS)nm $(BOARD_ELF) | awk '$$3 == "__heap_start" { print $$1 }') ))),0x4EE280)
HOST_C_SRC := $(GAME_SRC) $(LIB_C:%=$(L)/%.c) $(PORT_SRC) $(HOST_EMU) tools/host/host.c
hobj = $(HB)/$(subst ../,_/,$(basename $(1))).o
HOST_OBJ := $(foreach f,$(HOST_C_SRC) $(CXX_SRC),$(call hobj,$(f)))
HOST_CFLAGS := -m32 -msse2 -mfpmath=sse -O1 -g -fno-pie -fno-asynchronous-unwind-tables $(GAMEFLAGS) -DM9B $(HOST_DEFS) -DM20_HEAP=$(M20_HEAP)u $(INCLUDES) $(WARN) $(CWARN) -std=gnu11
HOST_CXXFLAGS := -m32 -msse2 -mfpmath=sse -O1 -g -fno-pie -fno-asynchronous-unwind-tables -fno-exceptions $(GAMEFLAGS) $(INCLUDES) $(WARN) -std=gnu++11

# (the objects depend on the Makefile: the flags)
define host_c_rule
$(call hobj,$(1)): $(1) Makefile
	@mkdir -p $$(@D)
	gcc $$(HOST_CFLAGS) $$(EXTRA) -MMD -MP -c $$< -o $$@
endef
define host_cxx_rule
$(call hobj,$(1)): $(1) Makefile
	@mkdir -p $$(@D)
	g++ $$(HOST_CXXFLAGS) $$(EXTRA) -MMD -MP -c $$< -o $$@
endef
$(foreach f,$(HOST_C_SRC),$(eval $(call host_c_rule,$(f))))
$(foreach f,$(CXX_SRC),$(eval $(call host_cxx_rule,$(f))))
$(foreach f,$(GAME_SRC) $(PORT_SRC),$(call hobj,$(f))): HOST_CFLAGS += -include precompiled.h

comma := ,
HOST_WRAP := main pump_events malloc calloc realloc aligned_alloc free strdup open fopen unlink remove rename access

host: $(HB)/sshock-host

$(HB)/sshock-host: $(HOST_OBJ)
	g++ -m32 -no-pie -Wl,-Ttext-segment=0x800000 $(HOST_OBJ) $(addprefix -Wl$(comma)--wrap=,$(HOST_WRAP)) -lpthread -lm -o $@
	@echo "$@: heap $(M20_HEAP) (the board's), engine $(HOST_ENGINE)"; size $@

-include $(OBJ:.o=.d) $(HOST_OBJ:.o=.d)

clean:
	rm -rf build build-*

.PHONY: all host clean
