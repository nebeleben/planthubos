cmake_minimum_required(VERSION 3.16)
# Reuse the whole planthubos zigbee component + its dependency closure
# (h2fw lives inside planthubos/, so the components are one level up -- the
# same relative path in this repo and in the public planthubos mirror).
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../components")
# Without this, EXTRA_COMPONENT_DIRS makes IDF build EVERY planthubos component
# (swarm, webserver, wifi_manager, ...), none of which compile on the WiFi-less
# H2. Restricting to `main` trims the build to main's transitive requirements
# (zigbee + data_core + its closure), the only components the H2 needs.
set(COMPONENTS main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(h2fw)
