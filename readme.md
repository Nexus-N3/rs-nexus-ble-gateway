
# RS NEXUS custom BLE gateway to handle all bluetooth related activity

## Goal
The objective is to offload ble operations to a dedicated gateway that can handle streaming 60 / 100hz sensors (total 10).

The interface is an entry point for client software (rs-nexus-os) that provides the gateway with sensors to manage and their corressonding spec.  The gateway returns raw data from sensors and the client is responsible for parsing and further orchestration

## Zephr
if using Zephr this scaffold will need

CMakeLists.txt
prj.conf
Kconfig options
devicetree overlay if needed / yaml (examples use sample.yaml)
source files

