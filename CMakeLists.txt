cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Firmware version: bump this for each release. It is baked into the
# image, printed in the boot log, and shown on the status display.
set(PROJECT_VER "0.1.0")

project(URS_V2)
