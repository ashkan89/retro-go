cmake_minimum_required(VERSION 3.5)
set(COMPONENTS "main retro-go media wifi esp_http_server")
# Keep UI/display and I2S on core 1; source/codec work runs on core 0.
set(RG_MEDIA_DUAL_CORE ON)
include(../base.cmake)
project(launcher)
