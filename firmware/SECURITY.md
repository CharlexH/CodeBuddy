# Firmware secret handling

The ESP32 Arduino SDK used by this build is compiled with coredump support, but
CodeBuddy's checked-in partition table deliberately provides no `coredump`
partition. Flash coredump persistence is therefore disabled for this firmware.
Do not replace `partitions_codebuddy.csv` with an SDK default table while Wi-Fi
credentials are supported.

Portal request bodies and password-bearing structures use fixed buffers and are
overwritten through volatile writes on success, rejection, timeout, cancel, and
connection failure. Saved Wi-Fi passwords are loaded from NVS only immediately
before `WiFi.begin()` and the temporary full-size buffer is then overwritten.
