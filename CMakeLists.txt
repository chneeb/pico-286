if (NOT (DEFINED PICO_PLATFORM AND PICO_PLATFORM STREQUAL "host"))
# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
endif()
cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Pull in Raspberry Pi Pico SDK (must be before project, but after PICO_BOARD)
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(286 C CXX ASM)
SET(BUILD_NAME "${PROJECT_NAME}")

#set(MURM20 ON)
#set(m1p2launcher ON)
# CPU Configuration
set(CPU_FREQ_MHZ "500")
set(FLASH_FREQ_MHZ "100")
set(PSRAM_FREQ_MHZ "166")

# =========================
# OUTPUT DIRECTORIES
# =========================
set(OUTPUT_DIR "${CMAKE_SOURCE_DIR}/bin/${PICO_PLATFORM}/${CMAKE_BUILD_TYPE}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${OUTPUT_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${OUTPUT_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${OUTPUT_DIR}")

# =========================
# SOURCE FILES
# =========================
file(GLOB_RECURSE SRC "src/emulator/*.cpp" "src/emulator/*.c" "src/emu8950/*.c" "src/emu8950/*.cpp")

message(STATUS "Add source files:")
foreach (SRC_FILE IN LISTS SRC)
    message(STATUS "${SRC_FILE}")
endforeach ()
message(STATUS "")

# =========================
# HOST BUILD (Windows/Linux)
# =========================
if (PICO_PLATFORM STREQUAL "host")
    add_compile_options(-fms-extensions -O2)

    if (WIN32)
        add_executable(${PROJECT_NAME} ${SRC} src/win32-main.cpp src/WinMiniFB.c src/printf/printf.c)
        target_link_libraries(${PROJECT_NAME} PRIVATE winmm)
    else ()
        # Linux build
        add_executable(${PROJECT_NAME} ${SRC} src/linux-main.cpp src/LinuxMiniFB.c src/linux-audio.c src/printf/printf.c findfirst/findfirst.c findfirst/spec.c)
        target_link_libraries(${PROJECT_NAME} PRIVATE X11 pthread)
        target_include_directories(${PROJECT_NAME} PRIVATE src src/emu8950 src/printf findfirst/)
    endif ()

    target_include_directories(${PROJECT_NAME} PRIVATE src src/emu8950 src/printf)
else ()
    #set(CMAKE_BUILD_TYPE "MinSizeRel") /// outside!
    # =========================
    # PICO BUILD CONFIGURATION
    # =========================
    #set(PICO_BOARD pico2 CACHE STRING "Board type") // should be early
    pico_sdk_init()

    # =========================
    # BUILD OPTIONS
    # =========================

    # Display Options (mutually exclusive)
    option(ENABLE_TFT "Enable TFT display output" OFF)
    option(ENABLE_VGA "Enable VGA display output" OFF)
    option(ENABLE_HDMI "Enable HDMI display output" OFF)
    option(ENABLE_NTSC-TV "Enable NTSC TV output" OFF)

    # ClockworkPi PicoCalc: ILI9488 320x320 panel + I2C keyboard + on-board
    # PSRAM. Counts as a display option; it also replaces the PS/2 keyboard and
    # drops the NES pad (its pins collide with the panel and the SD card).
    option(ENABLE_PICOCALC "Enable ClockworkPi PicoCalc board support" OFF)

    # Bring-up aids: a colour-bar self-test at boot and a legible 8x8 debug
    # overlay painted across the bottom of the panel. Turn this OFF for a normal
    # build - the overlay then only appears in the letterbox, in the 4x6 font.
    option(PICOCALC_BRINGUP "PicoCalc: boot self-test pattern + large debug overlay" ON)

    # SD bus clock. 12.5 MHz is what tiny_agi uses on this board and is the safe
    # default; raise it if your card and wiring tolerate it.
    # 30 MHz: freesci-archive runs this rate on the same class of card on this
    # board (and notes frank-quest does so without DMA, as we do). Safe by
    # construction - the card is negotiated at 100 kHz and only then switched,
    # so one that cannot sustain it fails visibly at mount rather than
    # corrupting data. Drop to 12500000 if a card misbehaves.
    set(PICOCALC_SD_CLK_HZ "30000000" CACHE STRING "PicoCalc SD SPI clock in Hz")

    # Panel clock. 75 MHz is device-verified here (30 fps vs 24 at 50 MHz, colours
    # and geometry correct) and is the rate shapones drives this same panel at.
    # Note it is above the ILI9488 datasheet's nominal 66 MHz serial write cycle:
    # in spec terms this is an overclock, accepted because the failure mode is
    # visible - shearing, noise or dropped pixels - rather than silent. Drop back
    # to 50000000 if a panel shows artefacts. The achieved rate is reported on the
    # bring-up overlay as fps@NNMHz.
    set(PICOCALC_LCD_CLK_VAL "75000000" CACHE STRING "PicoCalc panel SPI clock in Hz")

    # One-shot measuring build: sweeps PSRAM (divisor x fudge), prints a table
    # and stops. Does not boot the emulator.
    option(PICOCALC_PSRAM_SWEEP "PicoCalc: PSRAM timing sweep build" OFF)

    # Soak build: hammers the configured PSRAM operating point and reports a
    # running error total. Use before committing to a divisor from the sweep.
    option(PICOCALC_PSRAM_SOAK "PicoCalc: PSRAM soak-test build" OFF)
    # Device-verified operating point: 198 MHz SM = 99 MHz SPI with the fudge
    # PIO program, soak-tested clean (0 errors, ~5.0 MB/s) on PicoCalc hardware.
    # These two are coupled: psram_spi.pio documents the fudge variant as
    # required above 83 MHz SPI and it is DEAD below that, so lowering the clock
    # past 166 MHz SM without also setting PSRAM_FUDGE_VAL=0 gives a dead bus.
    # Use -DPICOCALC_PSRAM_SWEEP=ON to re-derive both for a different board.
    set(PSRAM_FUDGE_VAL "1" CACHE STRING "PSRAM PIO program: 0 = plain, 1 = fudge (needed above 83MHz SPI)")
    set(PSRAM_SM_CLOCK_VAL "198000000" CACHE STRING "PSRAM PIO state-machine clock in Hz (SPI rate is half this)")

    # Audio Options (mutually exclusive)
    option(ENABLE_I2S_SOUND "Enable I2S audio output" OFF)
    option(ENABLE_PWM_SOUND "Enable PWM audio output" OFF)
    option(ENABLE_HARDWARE_SOUND "Enable hardware audio output" OFF)

    # =========================
    # VALIDATION
    # =========================

    # Validate display options
    set(DISPLAY_COUNT 0)
    if (ENABLE_TFT)
        message(STATUS "TFT Enabled")
        math(EXPR DISPLAY_COUNT "${DISPLAY_COUNT} + 1")
    endif ()
    if (ENABLE_VGA)
        message(STATUS "VGA Enabled")
        math(EXPR DISPLAY_COUNT "${DISPLAY_COUNT} + 1")
    endif ()
    if (ENABLE_HDMI)
        IF(PICO_BOARD EQUAL "pico2")
            set(CPU_FREQ_MHZ "504")
            message(STATUS "HDMI enabled - CPU frequency locked to 504 MHz")
        else ()
            set(CPU_FREQ_MHZ "504")
            message(STATUS "HDMI enabled - CPU frequency locked to 378 MHz")
        endif ()

        math(EXPR DISPLAY_COUNT "${DISPLAY_COUNT} + 1")
    endif ()
    if (ENABLE_NTSC-TV)
        set(CPU_FREQ_MHZ "315")
        message(STATUS "NTSC TV out enabled - CPU frequency locked to 315 MHz")
        math(EXPR DISPLAY_COUNT "${DISPLAY_COUNT} + 1")
    endif ()
    if (ENABLE_PICOCALC)
        if (PICO_PLATFORM STREQUAL "rp2040")
            message(FATAL_ERROR "PicoCalc support requires an RP2350 (Pico 2)")
        endif ()
        # The stock 500 MHz is above what the PicoCalc's PSRAM wiring has been
        # shown to take; 396 MHz is a conservative starting point.
        set(CPU_FREQ_MHZ "396")
        message(STATUS "PicoCalc enabled - CPU frequency set to ${CPU_FREQ_MHZ} MHz")
        math(EXPR DISPLAY_COUNT "${DISPLAY_COUNT} + 1")
    endif ()

    if (DISPLAY_COUNT EQUAL 0)
        message(FATAL_ERROR "At least one display option must be enabled")
    elseif (DISPLAY_COUNT GREATER 1)
        message(FATAL_ERROR "Only one display option can be enabled at a time")
    endif ()

    # Validate audio options
    set(AUDIO_COUNT 0)
    if (ENABLE_I2S_SOUND)
        math(EXPR AUDIO_COUNT "${AUDIO_COUNT} + 1")
    endif ()
    if (ENABLE_PWM_SOUND)
        math(EXPR AUDIO_COUNT "${AUDIO_COUNT} + 1")
    endif ()
    if (ENABLE_HARDWARE_SOUND)
        math(EXPR AUDIO_COUNT "${AUDIO_COUNT} + 1")
    endif ()

    if (AUDIO_COUNT EQUAL 0)
        message(FATAL_ERROR "At least one audio option must be enabled")
    elseif (AUDIO_COUNT GREATER 1)
        message(FATAL_ERROR "Only one audio option can be enabled at a time")
    endif ()

    # =========================
    # BUILD NAME GENERATION
    # =========================
    set(BUILD_NAME "${PROJECT_NAME}")

    # Add platform
    if (PICO_PLATFORM STREQUAL "rp2040")
        if (MURM20)
            set(BUILD_NAME "m2p1-${BUILD_NAME}")
        else ()
            set(BUILD_NAME "m1p1-${BUILD_NAME}")
        endif ()
    else ()
        if (MURM20)
            set(BUILD_NAME "m2p2-${BUILD_NAME}")
        else ()
            set(BUILD_NAME "m1p2-${BUILD_NAME}")
        endif ()
    endif ()

    # Add frequency
    set(BUILD_NAME "${BUILD_NAME}-F${FLASH_FREQ_MHZ}-P${PSRAM_FREQ_MHZ}-${CPU_FREQ_MHZ}MHz")

    # Add display type
    if (ENABLE_PICOCALC)
        set(BUILD_NAME "${BUILD_NAME}-PICOCALC")
        # Distinct output name: the bring-up and normal builds are otherwise
        # byte-for-byte indistinguishable by filename, and flashing the wrong
        # one looks like the option did nothing.
        if (PICOCALC_PSRAM_SWEEP)
            set(BUILD_NAME "${BUILD_NAME}-PSRAMSWEEP")
        elseif (PICOCALC_PSRAM_SOAK)
            set(BUILD_NAME "${BUILD_NAME}-PSRAMSOAK")
        elseif (PICOCALC_BRINGUP)
            set(BUILD_NAME "${BUILD_NAME}-BRINGUP")
        endif ()
    elseif (ENABLE_TFT)
        set(BUILD_NAME "${BUILD_NAME}-TFT")
    elseif (ENABLE_NTSC-TV)
        set(BUILD_NAME "${BUILD_NAME}-TV")
    elseif (ENABLE_VGA)
        set(BUILD_NAME "${BUILD_NAME}-VGA")
    elseif (ENABLE_HDMI)
        set(BUILD_NAME "${BUILD_NAME}-HDMI")
    endif ()

    # Add audio type
    if (ENABLE_I2S_SOUND)
        set(BUILD_NAME "${BUILD_NAME}-I2S")
    elseif (ENABLE_PWM_SOUND)
        set(BUILD_NAME "${BUILD_NAME}-PWM")
    elseif (ENABLE_HARDWARE_SOUND)
        set(BUILD_NAME "${BUILD_NAME}-HW")
    endif ()

    # =========================
    # EXECUTABLE SETUP
    # =========================
    add_link_options(-flto -fwhole-program)
    # -frename-registers -fno-tree-vectorize
    add_compile_options(
            -flto=auto
            -frename-registers
            -fomit-frame-pointer
            -fwhole-program
            #        -ffreestanding // build issues with std::malloc/std::free in SDK 2.1+
            -ffast-math
            -ffunction-sections
            -fdata-sections
            -fms-extensions
            -O2
    )

    add_executable(${PROJECT_NAME} ${SRC} src/pico-main.c src/printf/printf.c src/74hc595/74hc595.c)
    target_include_directories(${PROJECT_NAME} PRIVATE src/74hc595 src src/emu8950 src/printf)
    target_sources(${PROJECT_NAME} PRIVATE src/emu8950/slot_render_pico.S)

    # =========================
    # BOOT STAGE 2 CONFIGURATION
    # =========================
    pico_define_boot_stage2(slower_boot2 ${PICO_DEFAULT_BOOT_STAGE2_FILE})

    if (PICO_PLATFORM STREQUAL "rp2040")
        add_subdirectory(memops_opt)
        target_link_libraries(${PROJECT_NAME} PRIVATE memops_opt)
        target_compile_definitions(slower_boot2 PRIVATE PICO_FLASH_SPI_CLKDIV=4)
    else ()
        if (m1p2launcher)
            if (MURM20)
                set(BUILD_NAME "${BUILD_NAME}.m2p2")
            else ()
                set(BUILD_NAME "${BUILD_NAME}.m1p2")
            endif ()
            pico_set_linker_script(${PROJECT_NAME} "${CMAKE_SOURCE_DIR}/memmap12.ld")
        else ()
            pico_set_linker_script(${PROJECT_NAME} "${CMAKE_SOURCE_DIR}/memmap.ld")
        endif ()
    endif ()

    set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "${BUILD_NAME}")
    target_link_options(${PROJECT_NAME} PRIVATE -Xlinker --print-memory-usage --data-sections --function-sections)

    pico_set_program_name(286 "PC 8086/8088/186/286 emulator")
    pico_set_program_version(${PROJECT_NAME} "1.1.0")
    pico_set_boot_stage2(${PROJECT_NAME} slower_boot2)

    # =========================
    # SUBDIRECTORIES
    # =========================
    add_subdirectory(drivers/fatfs)
    add_subdirectory(drivers/sdcard)
    add_subdirectory(drivers/audio)
    add_subdirectory(drivers/psram)
    if (ENABLE_PICOCALC)
        # ps2 and nespad are mutually exclusive with the PicoCalc: ps2.c defines
        # keyboard_init(), and the NES pad pins collide with the panel.
        add_subdirectory(drivers/picocalc-kbd)
        target_link_libraries(${PROJECT_NAME} PRIVATE picocalc-kbd)
    else ()
        add_subdirectory(drivers/ps2)
        add_subdirectory(drivers/nespad)
        target_link_libraries(${PROJECT_NAME} PRIVATE ps2 nespad)
    endif ()

    # Display drivers
    if (ENABLE_PICOCALC)
        add_subdirectory(drivers/graphics)
        add_subdirectory(drivers/picocalc-lcd)
        target_link_libraries(${PROJECT_NAME} PRIVATE picocalc-lcd)
        message(STATUS "PicoCalc ILI9488 display enabled")
    elseif (ENABLE_TFT)
        add_subdirectory(drivers/graphics)
        add_subdirectory(drivers/st7789)
        target_link_libraries(${PROJECT_NAME} PRIVATE st7789)
        message(STATUS "TFT display enabled")
    elseif (ENABLE_NTSC-TV)
        add_subdirectory(drivers/ntsc-tv)
        message(STATUS "NTSC TV out enabled")
    elseif (ENABLE_VGA)
        add_subdirectory(drivers/graphics)
        add_subdirectory(drivers/vga-nextgen)
        target_link_libraries(${PROJECT_NAME} PRIVATE vga-nextgen)
        message(STATUS "VGA display enabled")
    elseif (ENABLE_HDMI)
        add_subdirectory(drivers/graphics)
        add_subdirectory(drivers/hdmi)
        target_link_libraries(${PROJECT_NAME} PRIVATE hdmi)
        message(STATUS "HDMI display enabled")
    endif ()

    # =========================
    # LIBRARIES
    # =========================
    target_link_libraries(${PROJECT_NAME} PRIVATE
            sdcard audio graphics psram
            pico_runtime pico_stdlib pico_multicore
            hardware_dma hardware_pio hardware_i2c hardware_exception
            hardware_interp hardware_timer hardware_clocks hardware_pwm hardware_flash
            -Wl,--wrap=atexit,--wrap=abort,--strip-all,--gc-sections # size optimizations
    )
    pico_set_printf_implementation(${PROJECT_NAME} none)

    # =========================
    # COMPILE DEFINITIONS
    # =========================
    target_compile_definitions(${PROJECT_NAME} PRIVATE
            # Core definitions
            PICO_PANIC_FUNCTION=
            EMU8950_ASM=1
            CPU_FREQ_MHZ=${CPU_FREQ_MHZ}
            FLASH_FREQ_MHZ=${FLASH_FREQ_MHZ}
            PSRAM_FREQ_MHZ=${PSRAM_FREQ_MHZ}
            # Murmulator boards carry a 16MB chip; the PicoCalc's Pico 2 has 4MB.
            # Declaring more than is fitted makes anything that reasons about
            # flash bounds (and the XIP aliasing) wrong.
            PICO_FLASH_SIZE_BYTES=$<IF:$<BOOL:${ENABLE_PICOCALC}>,4194304,16777216>

            # Display type
            $<$<BOOL:${ENABLE_TFT}>:TFT>
            $<$<BOOL:${ENABLE_VGA}>:VGA>
            $<$<BOOL:${ENABLE_HDMI}>:HDMI>
            $<$<BOOL:${ENABLE_NTSC-TV}>:NTSC>
            $<$<BOOL:${ENABLE_PICOCALC}>:PICOCALC>

            # Audio type
            $<$<BOOL:${ENABLE_I2S_SOUND}>:I2S_SOUND>
            $<$<BOOL:${ENABLE_PWM_SOUND}>:PWM_SOUND>
            $<$<BOOL:${ENABLE_HARDWARE_SOUND}>:HARDWARE_SOUND>

            PSRAM
    )
    if (ENABLE_PICOCALC)
        # ClockworkPi PicoCalc mainboard v2.0 pinout.
        #   GP2/3,20/21 PSRAM (PIO)   GP6/7   keyboard I2C
        #   GP10-15     ILI9488 panel GP16-19 SD card (SPI0)
        #   GP26/27     audio PWM
        # The panel backlight is driven by the keyboard MCU over I2C, not a GPIO,
        # so there is no TFT_LED_PIN. No PS/2 mouse or NES pad: GP14/15/16 are
        # taken by the panel and the SD card.
        target_compile_definitions(${PROJECT_NAME} PRIVATE
                PICOCALC_LCD_CLK_PIN=10 PICOCALC_LCD_DATA_PIN=11
                PICOCALC_LCD_CS_PIN=13 PICOCALC_LCD_DC_PIN=14 PICOCALC_LCD_RST_PIN=15
                PICOCALC_KBD_SDA_PIN=6 PICOCALC_KBD_SCL_PIN=7
                PICOCALC_LCD_MAX_HZ=${PICOCALC_LCD_CLK_VAL}
                SDCARD_PIN_SPI0_CS=17 SDCARD_PIN_SPI0_SCK=18 SDCARD_PIN_SPI0_MOSI=19 SDCARD_PIN_SPI0_MISO=16
                PSRAM_SPINLOCK=1 PSRAM_ASYNC=1 PSRAM_PIN_CS=20 PSRAM_PIN_SCK=21 PSRAM_PIN_MOSI=2 PSRAM_PIN_MISO=3
                # ~100 MHz SM clock is the verified operating point for this
                # board's PSRAM wiring; the divisor follows the system clock.
                PSRAM_SM_CLOCK_HZ=${PSRAM_SM_CLOCK_VAL}
                PSRAM_FUDGE=${PSRAM_FUDGE_VAL}
                $<$<BOOL:${PICOCALC_PSRAM_SWEEP}>:PICOCALC_PSRAM_SWEEP>
                $<$<BOOL:${PICOCALC_PSRAM_SOAK}>:PICOCALC_PSRAM_SOAK>
                PWM_BEEPER=28 PWM_LEFT_CHANNEL=26 PWM_RIGHT_CHANNEL=27
                CLOCK_PIN=22 PCM_PIN=22
                # 30 MHz is a Murmulator value. tiny_agi runs the PicoCalc's
                # card at 12.5 MHz; shapones never exceeds 250 kHz.
                SDCARD_CLK_FAST_HZ=${PICOCALC_SD_CLK_HZ}
                PICOCALC_LCD_SELFTEST=$<IF:$<BOOL:${PICOCALC_BRINGUP}>,1,0>
        )
    elseif (MURM20)
        target_compile_definitions(${PROJECT_NAME} PRIVATE
                MURM2=1
                # Pin configurations
                MOUSE_CLOCK_PIN=0 MOUSE_DATA_PIN=1
                KBD_CLOCK_PIN=2 KBD_DATA_PIN=3
                VGA_BASE_PIN=12 HDMI_BASE_PIN=12
                TFT_CS_PIN=12 TFT_RST_PIN=14 TFT_LED_PIN=15 TFT_DC_PIN=16 TFT_DATA_PIN=18 TFT_CLK_PIN=19
                SDCARD_PIN_SPI0_CS=5 SDCARD_PIN_SPI0_SCK=6 SDCARD_PIN_SPI0_MOSI=7 SDCARD_PIN_SPI0_MISO=4
                PSRAM_SPINLOCK=1 PSRAM_ASYNC=1 PSRAM_PIN_CS=18 PSRAM_PIN_SCK=19 PSRAM_PIN_MOSI=20 PSRAM_PIN_MISO=21
                USE_NESPAD NES_GPIO_CLK=20 NES_GPIO_DATA=21 NES_GPIO_LAT=26
                CLOCK_PIN=27 PCM_PIN=22
                PWM_BEEPER=9 PWM_LEFT_CHANNEL=10 PWM_RIGHT_CHANNEL=11
        )
    else ()
        target_compile_definitions(${PROJECT_NAME} PRIVATE
                # Pin configurations
                MOUSE_CLOCK_PIN=14 MOUSE_DATA_PIN=15
                KBD_CLOCK_PIN=0 KBD_DATA_PIN=1
                VGA_BASE_PIN=6 HDMI_BASE_PIN=6
                TFT_CS_PIN=6 TFT_RST_PIN=8 TFT_LED_PIN=9 TFT_DC_PIN=10 TFT_DATA_PIN=12 TFT_CLK_PIN=13
                SDCARD_PIN_SPI0_CS=5 SDCARD_PIN_SPI0_SCK=2 SDCARD_PIN_SPI0_MOSI=3 SDCARD_PIN_SPI0_MISO=4
                PSRAM_SPINLOCK=1 PSRAM_ASYNC=1 PSRAM_PIN_CS=18 PSRAM_PIN_SCK=19 PSRAM_PIN_MOSI=20 PSRAM_PIN_MISO=21
                USE_NESPAD NES_GPIO_CLK=14 NES_GPIO_DATA=16 NES_GPIO_LAT=15
                CLOCK_PIN=17 PCM_PIN=22
                PWM_BEEPER=28 PWM_LEFT_CHANNEL=26 PWM_RIGHT_CHANNEL=27
        )
    endif ()
    pico_add_extra_outputs(${PROJECT_NAME})

    if (m1p2launcher)
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E rename
            "${OUTPUT_DIR}/${BUILD_NAME}.uf2"
            "${OUTPUT_DIR}/${BUILD_NAME}"
        )
    endif ()
endif ()

# =========================
# COMMON DEFINITIONS (HOST + PICO)
# =========================
target_compile_definitions(${PROJECT_NAME} PRIVATE
        USE_EMU8950_OPL
        EMU8950_SLOT_RENDER=1
        EMU8950_NO_RATECONV=1
        EMU8950_NO_WAVE_TABLE_MAP=1
        EMU8950_NO_TLL=1
        EMU8950_NO_FLOAT=1
        EMU8950_NO_TIMER=1
        EMU8950_NO_TEST_FLAG=1
        EMU8950_SIMPLER_NOISE=1
        EMU8950_SHORT_NOISE_UPDATE_CHECK=1
        EMU8950_LINEAR_SKIP=1
        EMU8950_LINEAR_END_OF_NOTE_OPTIMIZATION
        EMU8950_NO_PERCUSSION_MODE=1
        EMU8950_LINEAR=1
)
