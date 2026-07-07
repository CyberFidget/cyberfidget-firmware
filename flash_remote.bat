@echo off
set REMOTE=S@192.168.3.2
set BUILD=%1\..
scp %BUILD%\bootloader.bin %REMOTE%:/C:/temp/bootloader.bin
scp %BUILD%\partitions.bin %REMOTE%:/C:/temp/partitions.bin
scp %BUILD%\firmware.bin %REMOTE%:/C:/temp/firmware.bin
ssh %REMOTE% "python -m esptool --port COM5 --baud 921600 write_flash 0x1000 C:/temp/bootloader.bin 0x8000 C:/temp/partitions.bin 0x10000 C:/temp/firmware.bin"