cmake_minimum_required(VERSION 3.5)
set(COMPONENTS "main retro-go media wifi esp_http_server")
# Keep launcher rendering on the UI core while music decoding and I2S use the audio core.
set(RG_SEPARATE_DISPLAY_AUDIO ON)
include(../base.cmake)
project(launcher)
