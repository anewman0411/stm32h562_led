cmake_minimum_required(VERSION 3.16)
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_C_COMPILER arm-none-eabi-gcc)
set(CMAKE_ASM_COMPILER arm-none-eabi-gcc)
set(CMAKE_OBJCOPY arm-none-eabi-objcopy)

# Turn off compiler checking (compiler is working)
set(CMAKE_C_COMPILER_WORKS TRUE)

# Set C standard
set(CMAKE_C_STANDARD 17)

# Set directory for binary files
set(BUILD_DIR ${CMAKE_SOURCE_DIR}/bin)

# Set path for build files
set(EXECUTABLE_OUTPUT_PATH ${BUILD_DIR})

# Create target variables
set(TARGET "main")
set(TARGET_ELF "${TARGET}.elf")
set(TARGET_HEX "${TARGET}.hex")
set(TARGET_BIN "${TARGET}.bin")

project(STM32H562_BLINK C ASM)

# Source directory
set(MAIN_SRC_DIR "${CMAKE_SOURCE_DIR}/src")

# Linker directory
set(LINKER_DIR "${CMAKE_SOURCE_DIR}/link")

# Linker options
set(LD_include "-lc -lm -lnosys -L${LINKER_DIR}")
set(linker_script "${LINKER_DIR}/STM32H562RGTX_FLASH.ld")

# MCU flags  (Cortex-M33 with single-precision FPU)
set(MCU_flags "-mcpu=cortex-m33 -mfpu=fpv5-sp-d16 -mfloat-abi=hard -mthumb")

# C defines
set(C_DEFS "-DSTM32H562xx")

# Compiler flags
set(C_flags "${MCU_flags} ${C_DEFS} -Wall -fdata-sections -ffunction-sections")
set(AS_flags "${MCU_flags} -Wall -fdata-sections -ffunction-sections")
set(LD_flags "${MCU_flags} -specs=nano.specs -specs=nosys.specs -T${linker_script} ${LD_include} -Wl,--print-memory-usage -Wl,--gc-sections")

# Add executable with minimal sources
# ---------------------------------------------------------------
# Adjust the startup and system file names/paths to match the
# CMSIS pack you downloaded.  Typical names:
#   startup_stm32h562xx.s       (or startup_stm32h562rgtx.s)
#   system_stm32h5xx.c
# ---------------------------------------------------------------
add_executable(${TARGET_ELF}
    src/main.c
    src/setup.c
    drivers/startup_stm32h562xx.s
    drivers/system_stm32h5xx.c
)

# Include directories
# ---------------------------------------------------------------
# Your drivers/ folder should contain (at minimum):
#   drivers/
#     stm32h5xx.h
#     stm32h562xx.h
#     system_stm32h5xx.h
#     system_stm32h5xx.c
#     startup_stm32h562xx.s
#     Include/            <-- CMSIS core (core_cm33.h, etc.)
#     STM32H5xx/Include/  <-- Device headers
# Adjust paths as needed to match your layout.
# ---------------------------------------------------------------
target_include_directories(${TARGET_ELF} PUBLIC
    src
    drivers
    drivers/Include
    drivers/STM32H5xx/Include
)

# Set compiler and linker flags
set(CMAKE_C_FLAGS "${C_flags}")
set(CMAKE_ASM_FLAGS "${AS_flags}")
set(CMAKE_EXE_LINKER_FLAGS "${LD_flags}")

# Link standard libraries
target_link_libraries(${TARGET_ELF} PUBLIC c m)

# Create .hex and .bin targets
set(OBJCOPY arm-none-eabi-objcopy)
add_custom_target(${TARGET_BIN} ALL COMMAND ${OBJCOPY} -O binary -S ${BUILD_DIR}/${TARGET_ELF} ${BUILD_DIR}/${TARGET_BIN})
add_custom_target(${TARGET_HEX} ALL COMMAND ${OBJCOPY} -O ihex -S ${BUILD_DIR}/${TARGET_ELF} ${BUILD_DIR}/${TARGET_HEX})

# Define dependencies
add_dependencies(${TARGET_HEX} ${TARGET_ELF})
add_dependencies(${TARGET_BIN} ${TARGET_ELF} ${TARGET_HEX})
