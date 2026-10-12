# 安装和首次构建

## IDF安装

请参阅Espressif的[官方安装指南](https://docs.espressif.com/projects/esp-idf/en/v5.2.1/esp32s3/get-started/index.html)。
https://docs.espressif.com/projects/esp-idf/en/v5.2.1/esp32s3/get-started/index.html

请注意，`factory_fw`示例基于IDF的确切版本v5.2.1。在克隆git存储库时，请不要忽略 `-b v5.2.1` 部分。
```
mkdir -p ~/esp
cd ~/esp
git clone -b v5.2.1 --recursive https://github.com/espressif/esp-idf.git
```
如果您使用的是macOS或Linux，强烈建议您为IDF环境初始化创建别名 `get_idf`。

## 首次构建

#### 1. 克隆代码仓库

```
git clone https://github.com/Seeed-Studio/SenseCAP-Watcher
cd SenseCAP-Watcher
git submodule update --init
cd examples/factory_firmware
```

#### 2. 获取IDF环境

```
get_idf
idf.py
```

如果您已正确安装IDF，在执行 `idf.py` 命令后，将获得 `idf.py` 工具的帮助打印。

```
$ idf.py
Usage: idf.py [OPTIONS] COMMAND1 [ARGS]... [COMMAND2 [ARGS]...]...

  ESP-IDF CLI构建管理工具。对于 `idf.py` 未知的命令，将尝试将其执行为构建系统目标。选择目标：esp32s3

...

```

将芯片目标设置为 `esp32s3`。

```
idf.py set-target esp32s3
```

#### 3. 构建项目

```
idf.py build
```

最新的开发分支是 `factory_fw` 分支，如果您想尝试最新功能或参与项目的开发，请随时切换到 `factory_fw` 分支并构建该分支。

```
git checkout factory_fw
idf.py build
```

#### 4. 烧录

使用**USB数据线**将SenseCAP Watcher连接到您的PC或笔记本电脑。

**请注意!!!**

**仅底部（侧面）的USB端口具有数据传输功能**

**背面的USB端口仅为设备提供电源。**

当您使用正确的数据线将Watcher连接到PC并从正确的USB C端口时，您的PC将会显示1个USB设备条目，以及2个UART设备。它们分别连接到ESP32S3和Himax SoC的UART。没有观察到哪个SoC将使用哪个UART的模式。因此，请尝试以下命令，并通过每个UART设备查看日志打印，直到您看到日志输出。

```
idf.py --port /dev/ttyACM0 monitor
```

请将 `/dev/ttyACM0` 替换为您操作系统上正确的UART设备名称。例如，在MacOS上它看起来像 `/dev/tty.wchusbserial56F3067xxxx`，在Windows上它看起来像 `COMx`。如果您看不到日志打印的进展，请尝试下一个UART。

**请注意!!!**

名为 `nvsfactory` 的分区包含设备正常运行所需的关键工厂数据，请务必不要擦除该分区。因此，强烈建议在执行任何烧录操作之前备份此分区。

我们将使用 `esptool.py` 来备份。该工具是IDF安装的一部分，所以如果您通过了IDF安装，它应该已经存在。

```
# Linux / MacOS
which esptool.py

# Windows
where esptool.py
```

现在让我们备份 `nvsfactory` 分区。

```
esptool.py --port /dev/tty.wchusbserial56F3067xxxx --baud 2000000 --chip esp32s3 --before default_reset --after hard_reset --no-stub read_flash 0x9000 204800 nvsfactory.bin
```

备份完成后，现在是时候烧录我们的固件了。

```
idf.py --port /dev/ttyACM0 -b 2000000 app-flash
```

使用子命令 `app-flash` 仅烧录应用程序分区，这将保护您的 `nvsfactory` 分区，最重要的是它会节省您的时间。

#### 5. 监控日志输出

```
idf.py monitor
```

使用 `ctrl + ]` 退出监控。
