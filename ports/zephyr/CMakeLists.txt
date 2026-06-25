cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(rs_nexus_ble_gateway)

target_sources(app PRIVATE
  ../../src/main.c
  ../../src/app/gateway.c
  ../../src/app/rf_survey.c
  ../../src/interface/gateway_interface.c
  ../../src/ble/ble_interface.c
  ../../src/ble/ble_scheduler.c
  ../../src/hardware/led.c
  ../../src/hardware/button.c
)

target_include_directories(app PRIVATE
  ../../src
  ../../src/app
  ../../src/interface
  ../../src/ble
  ../../src/config
  ../../src/sensors
  ../../src/hardware
)