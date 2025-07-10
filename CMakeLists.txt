cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# 添加SPIFFS文件系统支持
set(EXTRA_COMPONENT_DIRS components)

project(esp32_clock_display)

# SPIFFS文件系统配置
spiffs_create_partition_image(storage wav_files FLASH_IN_PROJECT) 