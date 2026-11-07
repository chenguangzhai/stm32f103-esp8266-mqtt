cmake_minimum_required(VERSION 3.22)

#
# This file is generated only once,
# and is not re-generated if converter is called multiple times.
#
# User is free to modify the file as much as necessary
#

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)


# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Set the project name
set(CMAKE_PROJECT_NAME UART)

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Core project settings
project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    # App 层：业务逻辑（采集、判阈、上报）
    # 暂无

    # BSP 层：板级驱动，与具体引脚/外设打交道
    Application/BSP/board.c
    Application/BSP/UART/uart_app.c
    # 0.96 寸 OLED（软件 I2C，SCL=PB8 / SDA=PB9，江协科技 V2.0 驱动）
    Application/BSP/OLED/OLED.c
    Application/BSP/OLED/OLED_Data.c

    # Middle 层：通信协议，不含硬件依赖
    Application/Middle/ESP8266/ESP8266.c
    Application/Middle/ESP8266/mqtt_client.c
    # MQTT 3.1.1 报文编解码（Paho Embedded C，纯字节处理，与平台无关）
    Application/Middle/ESP8266/mqtt/MQTTPacket.c
    Application/Middle/ESP8266/mqtt/MQTTConnectClient.c
    Application/Middle/ESP8266/mqtt/MQTTSubscribeClient.c
    Application/Middle/ESP8266/mqtt/MQTTSerializePublish.c
    Application/Middle/ESP8266/mqtt/MQTTDeserializePublish.c
    Application/Middle/ESP8266/mqtt/MQTTFormat.c
    Application/Middle/ESP8266/json/cJSON.c
)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined include paths
    Application/App
    Application/BSP
    Application/BSP/UART
    Application/BSP/OLED
    Application/Middle
    Application/Middle/ESP8266
    Application/Middle/ESP8266/mqtt
    Application/Middle/ESP8266/json
)

# printf 支持打印浮点数
# CubeMX 默认带 --specs=nano.specs，newlib-nano 为了省空间把 %f 裁掉了，
# 不加这一行 printf("%.1f") 会什么都不输出（温度湿度全空），非常难查。
target_link_options(${CMAKE_PROJECT_NAME} PRIVATE -u _printf_float)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined symbols
)

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx

    # Add user defined libraries
)
