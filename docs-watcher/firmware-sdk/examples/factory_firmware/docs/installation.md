# Installation and First Build

## IDF Installation

Please refer to espressif's [official installation guide](https://docs.espressif.com/projects/esp-idf/en/v5.2.1/esp32s3/get-started/index.html).
https://docs.espressif.com/projects/esp-idf/en/v5.2.1/esp32s3/get-started/index.html

Please be noticed that the `factory_fw` example is based on exactly the version v5.2.1 of IDF. Please don't miss the part `-b v5.2.1` when you clone the git repository.
```
mkdir -p ~/esp
cd ~/esp
git clone -b v5.2.1 --recursive https://github.com/espressif/esp-idf.git
```
We strongly recommend you make the alias `get_idf` for the IDF environment initialization if you're using the macOS or Linux.

## First Build

#### 1. Clone the repository

```
git clone https://github.com/Seeed-Studio/SenseCAP-Watcher
cd SenseCAP-Watcher
git submodule update --init
cd examples/factory_firmware
```

#### 2. Get the IDF env

```
get_idf
idf.py
```

If you've installed the IDF correctly, after the `idf.py` command you will get a help printing of idf.py tool.

```
$ idf.py
Usage: idf.py [OPTIONS] COMMAND1 [ARGS]... [COMMAND2 [ARGS]...]...

  ESP-IDF CLI build management tool. For commands that are not known to idf.py an attempt to execute it as a build system target will be
  made. Selected target: esp32s3

...

```

Set the chip target to `esp32s3`.

```
idf.py set-target esp32s3
```

#### 3. Build the project

```
idf.py build
```

The latest development branch is the `factory_fw` branch, if you want to try the newest feature or participate into the development of this project, feel free to checkout the `factory_fw` branch and build that branch.

```
git checkout factory_fw
idf.py build
```

#### 4. Flash

Plug the SenseCAP Watcher to your PC or laptop with a **USB data cable**.

**PLEASE NOTE!!! **

**ONLY the USB port on the bottom (on the side) has data transmission**

**The USB port on the back only provides power to the device.**

When you connect the Watcher with a proper data cable and from the proper USB C port, you will get 1 USB device entry on your PC, and also 2 UART devices. They're the UART connected to the ESP32S3, and the UART connected to the Himax SoC. There's no observed pattern that which SoC will use which UART. So please try the following command with every UART device until you see log printings. 

```
idf.py --port /dev/ttyACM0 monitor
```

Please replace `/dev/ttyACM0` with the correct UART device name on you operating system. e.g. on MacOS it looks like `/dev/tty.wchusbserial56F3067xxxx, and on Windows it looks like `COMx`. And again if you don't see the progress of the log printing, try next UART.

**PLEASE NOTE!!!**

The partition named `nvsfactory` contains critical factory data for the device to work, please be careful that you don't erase that partition. So it's strongly recommended that you backup this partition before you issue any flash operations.

We're gonna use the esptool.py to do the backup. This tool is part of the IDF installation, so it should be already there if you passed through the IDF installation.

```
# Linux / MacOS
which esptool.py

# Windows
where esptool.py
```

Now let's backup our `nvsfactory` partition.

```
esptool.py --port /dev/tty.wchusbserial56F3067xxxx --baud 2000000 --chip esp32s3 --before default_reset --after hard_reset --no-stub read_flash 0x9000 204800 nvsfactory.bin
```

Once you finished the backup, it's time to flash our firmware.

```
idf.py --port /dev/ttyACM0 -b 2000000 app-flash
```

Use the subcommand `app-flash` to only flash the application partition, that will protect your `nvsfactory` partition, and most importantly it will save your time.

#### 5. Monitor the log output

```
idf.py monitor
```

Use `ctrl + ]` to exit the monitoring.









