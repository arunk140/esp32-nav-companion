# 功能模块开发指南

建议您首先阅读[软件架构](architecture_CN.md)以了解功能模块的工作原理。

在本文档中，我们将逐步展示如何开发一个新的功能模块。我们将以 `UART Alarm` 模块为例。

## 1. 安装和首次构建

请按照[安装和首次构建](installation_CN.md)中的步骤进行操作，如果您已经跳过了这一部分。

```shell
# 您在 PROJ_ROOT_DIR/examples/factory_firmware/ 目录下
cd main/task_flow_module
```

## 2. 选择合适的模板

在[软件架构](architecture_CN.md)部分，我们介绍了现有的功能模块（在接下来的文档中简写为 **FM**，Function Module）及其用途。当我们开发一个新的 FM 时，最好从一个现有的、最接近的 FM 开始作为参考。在本教程中，我们将开发一个报警 FM，因此我们选择最简单的一个报警 FM——`local alarmer` 作为参考。

```shell
cp tf_module_local_alarm.h tf_module_uart_alarm.h
cp tf_module_local_alarm.c tf_module_uart_alarm.c
```

文件名无关紧要，任何 `.h` 和 `.c` 文件都会被构建系统扫描并纳入编译代码树。但仍然建议使用有意义的文件名。

## 3. 实现注册

任务流引擎（**TFE**，Task Flow Engine）提供了一个 API 函数来注册一个新的 FM。

```c
esp_err_t tf_module_register(const char *p_name,
                                const char *p_desc,
                                const char *p_version,
                                tf_module_mgmt_t *mgmt_handle);
```

前三个参数是您的 FM 的名称、描述和版本，它们目前在内部使用，例如从注册表中匹配 FM、日志打印等，但将在将来用于 FM 与本地服务通信时。

```c
// 在 tf_module_uart_alarm.h 中
#define TF_MODULE_UART_ALARM_NAME "uart alarm"
#define TF_MODULE_UART_ALARM_VERSION "1.0.0"
#define TF_MODULE_UART_ALARM_DESC "uart alarm function module"

// 在 tf_module_uart_alarm.c 中
esp_err_t tf_module_uart_alarm_register(void)
{
    return tf_module_register(TF_MODULE_UART_ALARM_NAME,
                              TF_MODULE_UART_ALARM_DESC,
                              TF_MODULE_UART_ALARM_VERSION,
                              &__g_module_management);
}
```

第四个参数是一个包含必要 API 函数的结构体，用于管理此 FM 的生命周期。

```c
// 在 tf_module.h 中
typedef struct tf_module_mgmt {
    tf_module_t *(*tf_module_instance)(void);
    void (*tf_module_destroy)(tf_module_t *p_module);
}tf_module_mgmt_t;
```

`tf_module_instance` 是一个函数，当引擎初始化任务流中指定的所有 FM 时，TFE 将调用该函数，这基本上意味着引擎刚刚收到一个任务流创建请求并开始流程。`tf_module_destroy` 是一个函数，当 TFE 停止流程时将调用该函数。

### 3.1 实例化

```c
tf_module_t *tf_module_uart_alarm_instance(void)
{
    tf_module_uart_alarm_t *p_module_ins = (tf_module_uart_alarm_t *) tf_malloc(sizeof(tf_module_uart_alarm_t));
    if (p_module_ins == NULL)
    {
        return NULL;
    }
    p_module_ins->module_base.p_module = p_module_ins;
    p_module_ins->module_base.ops = &__g_module_ops;

    if (atomic_fetch_add(&g_ins_cnt, 1) == 0) {
        // 第一次实例化，我们应该初始化硬件
        esp_err_t ret;
        uart_config_t uart_config = {
            .baud_rate = 115200,
            .data_bits = UART_DATA_8_BITS,
            .parity = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        };
        const int buffer_size = 2 * 1024;
        ESP_GOTO_ON_ERROR(uart_param_config(UART_NUM_2, &uart_config), err, TAG, "uart_param_config failed");
        ESP_GOTO_ON_ERROR(uart_set_pin(UART_NUM_2, GPIO_NUM_19/*TX*/, GPIO_NUM_20/*RX*/, -1, -1), err, TAG, "uart_set_pin failed");
        ESP_GOTO_ON_ERROR(uart_driver_install(UART_NUM_2, buffer_size, buffer_size, 0, NULL, ESP_INTR_FLAG_SHARED), err, TAG, "uart_driver_install failed");
    }

    return &p_module_ins->module_base;

err:
    free(p_module_ins);
    return NULL;
}
```

上述代码是我们的 `instance` 函数的实现。它为我们为此 FM 定义的结构体 `tf_module_uart_alarm_t` 分配内存，该结构体用于保存该 FM 的参数，类似于 C++ 类的成员。在结构体 `tf_module_uart_alarm_t` 中，第一个字段很重要——`tf_module_t module_base`，在 C++ 编程的角度来看，`tf_module_t` 是所有 FM 的父类。`instance` 函数只是给 TFE 一个指向 `tf_module_t` 结构体的指针。

```c
// 在 tf_module_uart_alarm.h 中
typedef struct {
    tf_module_t module_base;
    int input_evt_id;           //这也可以是模块实例 ID
    int output_format;          //默认值为 0，参见上面的注释
    bool include_big_image;     //默认值：false
    bool include_small_image;   //默认值：false
    bool include_boxes;         //默认值：false，敬请期待
} tf_module_uart_alarm_t;

// 在 tf_module_uart_alarm.c 中
tf_module_t *tf_module_uart_alarm_instance(void)
{
    ...
    return &p_module_ins->module_base;
    ...
}
```

必须分配 `tf_module_t` 的两个成员。

```c
// 在 tf_module_uart_alarm.c 中
tf_module_t *tf_module_uart_alarm_instance(void)
{
    ...
    p_module_ins->module_base.p_module = p_module_ins;
    p_module_ins->module_base.ops = &__g_module_ops;
```
`p_module` - 一个指针，指向 FM 实例本身，用于 `destroy` 函数获取实例的句柄并释放其内存。
`ops` - 一个包含由 TFE 操作 FM 的 API 函数的结构体，我们将在后面讨论。

实例函数的其余部分是初始化硬件和与您的 FM 逻辑相关的内容。

需要提到的一点是，FM 可能会被实例化多次。您需要处理 `instance` 函数的重新进入，如果您的 FM 不支持多实例，您需要在 `instance` 函数的第二次调用时返回一个 NULL 指针。

在这个 `uart alarmer` 示例中，我们将使用引用计数器来处理重新进入逻辑。

```c
if (atomic_fetch_add(&g_ins_cnt, 1) == 0) {
        // 第一次实例化，我们应该初始化硬件
        esp_err_t ret;
        uart_config_t uart_config = {
            .baud_rate = 115200,
            .data_bits = UART_DATA_8_BITS,
            .parity = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        };
        const int buffer_size = 2 * 1024;
        ESP_GOTO_ON_ERROR(uart_param_config(UART_NUM_2, &uart_config), err, TAG, "uart_param_config failed");
        ESP_GOTO_ON_ERROR(uart_set_pin(UART_NUM_2, GPIO_NUM_19/*TX*/, GPIO_NUM_20/*RX*/, -1, -1), err, TAG, "uart_set_pin failed");
        ESP_GOTO_ON_ERROR(uart_driver_install(UART_NUM_2, buffer_size, buffer_size, 0, NULL, ESP_INTR_FLAG_SHARED), err, TAG, "uart_driver_install failed");
    }
```

### 3.2 销毁

```c
void tf_module_uart_alarm_destroy(tf_module_t *p_module_base)
{
    if (p_module_base) {
        if (atomic_fetch_sub(&g_ins_cnt, 1) <= 1) {
            // 这是最后一次销毁调用，反初始化 uart
            uart_driver_delete(UART_NUM_2);
            ESP_LOGI(TAG, "uart driver is deleted.");
        }
        if (p_module_base->p_module) {
            free(p_module_base->p_module);
        }
    }
}
```

`destroy` 总是很简单 😂 我们只需要释放内存，并在必要时反初始化硬件。

## 4. 实现操作

我们父类的`ops`成员定义如下，

```c
struct tf_module_ops
{
    int (*start)(void *p_module);
    int (*stop)(void *p_module);
    int (*cfg)(void *p_module, cJSON *p_json);
    int (*msgs_sub_set)(void *p_module, int evt_id);
    int (*msgs_pub_set)(void *p_module, int output_index, int *p_evt_id, int num);
};
```

当TFE初始化FM时，它将按照以下顺序调用这些函数，`cfg` -> `msgs_sub_set` -> `msgs_pub_set` -> `start` ----> `stop`。

`cfg` - 从任务流JSON中获取参数，使用这些参数来配置您的FM。

`msgs_sub_set` - 通过将事件处理程序注册到上游FM的事件ID来创建连接。输入参数`evt_id`由TFE从任务流JSON中提取准备好。第一个参数`p_module`是指向FM实例本身的指针。

`msgs_pub_set` - 存储到下游FM的连接，如果此FM没有输出，可以将此函数留空。第一个参数`p_module`是指向FM实例本身的指针。第二个参数`output_index`是端口号，例如，此FM有2个输出，将连续调用`msgs_pub_set`两次，其中`output_index`分别为0和1。第三个参数`p_evt_id`是指向数组的指针，该数组保存此端口下游FM的所有事件ID，数组的大小为`num`，即最后一个参数。

`start`和`stop` - 就是它们字面上的意思。它们都接受`p_module`作为参数，即指向FM实例本身的指针。

### 4.1 cfg

```c
static int __cfg(void *p_module, cJSON *p_json)
{
    tf_module_uart_alarm_t *p_module_ins = (tf_module_uart_alarm_t *)p_module;

    cJSON *output_format = cJSON_GetObjectItem(p_json, "output_format");
    if (output_format == NULL || !cJSON_IsNumber(output_format))
    {
        ESP_LOGE(TAG, "params output_format missing, default 0 (binary output)");
        p_module_ins->output_format = 0;
    } else {
        ESP_LOGI(TAG, "params output_format=%d", output_format->valueint);
        p_module_ins->output_format = output_format->valueint;
    }

    cJSON *include_big_image = cJSON_GetObjectItem(p_json, "include_big_image");
    if (include_big_image == NULL || !cJSON_IsBool(include_big_image))
    {
        ESP_LOGE(TAG, "params include_big_image missing, default false");
        p_module_ins->include_big_image = false;
    } else {
        ESP_LOGI(TAG, "params include_big_image=%s", cJSON_IsTrue(include_big_image)?"true":"false");
        p_module_ins->include_big_image = cJSON_IsTrue(include_big_image);
    }

    cJSON *include_small_image = cJSON_GetObjectItem(p_json, "include_small_image");
    if (include_small_image == NULL || !cJSON_IsBool(include_small_image))
    {
        ESP_LOGE(TAG, "params include_small_image missing, default false");
        p_module_ins->include_small_image = false;
    } else {
        ESP_LOGI(TAG, "params include_small_image=%s", cJSON_IsTrue(include_small_image)?"true":"false");
        p_module_ins->include_small_image = cJSON_IsTrue(include_small_image);
    }

    cJSON *include_boxes = cJSON_GetObjectItem(p_json, "include_boxes");
    if (include_boxes == NULL || !cJSON_IsBool(include_boxes))
    {
        ESP_LOGE(TAG, "params include_boxes missing, default false");
        p_module_ins->include_boxes = false;
    } else {
        ESP_LOGI(TAG, "params include_boxes=%s", cJSON_IsTrue(include_boxes)?"true":"false");
        p_module_ins->include_boxes = cJSON_IsTrue(include_boxes);
    }
    return 0;
}
```

正如您所见，`cfg`函数只是从任务流中的cJSON对象中提取字段值。例如，以下是包含`uart alarmer` FM的简单任务流示例。

```json
{
  "tlid": 3,
  "ctd": 3,
  "tn": "Local Human Detection",
  "type": 0,
  "task_flow": [
    {
      "id": 1,
      "type": "ai camera",
      "index": 0,
      "version": "1.0.0",
      "params": {
        "model_type": 1,
        "modes": 0,
        "model": {
          "arguments": {
            "iou": 45,
            "conf": 50
          }
        },
        "conditions": [
          {
            "class": "person",
            "mode": 1,
            "type": 2,
            "num": 0
          }
        ],
        "conditions_combo": 0,
        "silent_period": {
          "silence_duration": 5
        },
        "output_type": 0,
        "shutter": 0
      },
      "wires": [
        [2]
      ]
    },
    {
      "id": 2,
      "type": "alarm trigger",
      "index": 1,
      "version": "1.0.0",
      "params": {
        "text": "human detected",
        "audio": ""
      },
      "wires": [
        [3]
      ]
    },
    {
      "id": 3,
      "type": "uart alarm",
      "index": 2,
      "version": "1.0.0",
      "params": {
        "output_format": 1,
        "include_big_image": false,
        "include_small_image": false
      },
      "wires": []
    }
  ]
}
```

在上述任务流中，`uart alarmer`的`params`为 

```json
{
  "output_format": 1,
  "include_big_image": false,
  "include_small_image": false
}
```

我们分析cJSON，提取所需的值并通常将它们存储到模块实例中。

### 4.2 msgs_sub_set

```c
static int __msgs_sub_set(void *p_module, int evt_id)
{
    tf_module_uart_alarm_t *p_module_ins = (tf_module_uart_alarm_t *)p_module;
    p_module_ins->input_evt_id = evt_id;
    return tf_event_handler_register(evt_id, __event_handler, p_module_ins);
}
```

标记上游FM的事件ID以供将来使用，并为事件注册事件处理程序。

### 4.3 事件处理程序

在[软件架构](architecture_CN.md)中，我们了解到数据流由事件循环驱动。基本上，FM将从其事件处理程序接收数据，然后消耗数据，进行计算并得到一些结果。最后，它需要将结果发布到事件循环中，目标是对此FM数据感兴趣的下游FM。

在这个`uart alarmer`的示例中，我们从一个警报触发器FM中获取数据，该FM的输出数据类型为`TF_DATA_TYPE_DUALIMAGE_WITH_INFERENCE_AUDIO_TEXT`。由于UART数据准备很简单，我们在事件循环处理程序中完成所有数据生成工作。不过，如果您的数据处理耗时较长或者对IO有较高要求，建议创建一个工作任务（线程）来进行后台处理。

我们根据输入参数`output_format`准备一个二进制输出缓冲区或JSON字符串。最后，我们将这些数据写入UART。我们的FM只有一个输出，即硬件，而不是另一个FM，因此我们的`msgs_pub_set`是虚拟的。最后，我们需要释放来自事件循环的数据，下一节将解释原因。

### 4.4 msgs_pub_set

在这个示例中，`msgs_pub_set`是虚拟的，因为我们的FM没有下游消费者。让我们以`ai camera` FM为例。

```c
// in tf_module_ai_camera.c
static int __msgs_pub_set(void *p_module, int output_index, int *p_evt_id, int num)
{
    tf_module_ai_camera_t *p_module_ins = (tf_module_ai_camera_t *)p_module;
    __data_lock(p_module_ins);
    if (output_index == 0 && num > 0)
    {
        p_module_ins->p_output_evt_id = (int *)tf_malloc(sizeof(int) * num);
        if (p_module_ins->p_output_evt_id )
        {
            memcpy(p_module_ins->p_output_evt_id, p_evt_id, sizeof(int) * num);
            p_module_ins->output_evt_num = num;
        } else {
            ESP_LOGE(TAG, "Failed to malloc p_output_evt_id");
            p_module_ins->output_evt_num = 0;
        }
    }
    else
    {
        ESP_LOGW(TAG, "Only support output port 0, ignore %d", output_index);
    }
    __data_unlock(p_module_ins);
    return 0;
}
```

这并不复杂，只是将事件ID存储到FM实例的结构中。您需要在FM类型结构体中添加一个成员字段，例如`tf_module_ai_camera_t`。

当我们使用这些事件ID时？在数据生成并通过时间门控时刻。例如在`ai camera`中的示例中，数据源自Himax SoC的SPI输出，该SoC运行本地AI推理，并经过几个条件门控，如果所有条件都满足，则数据达到需要发布到事件循环的时刻。

```c
// 在 tf_module_ai_camera.c 中
...
                    for (int i = 0; i < p_module_ins->output_evt_num; i++)
                    {
                        tf_data_image_copy(&p_module_ins->output_data.img_small, &info.img);
                        tf_data_inference_copy(&p_module_ins->output_data.inference, &info.inference);

                        ret = tf_event_post(p_module_ins->p_output_evt_id[i], &p_module_ins->output_data, sizeof(p_module_ins->output_data), pdMS_TO_TICKS(100));
                        if( ret != ESP_OK) {
                            ESP_LOGE(TAG, "Failed to post event %d", p_module_ins->p_output_evt_id[i]);
                            tf_data_free(&p_module_ins->output_data);
                        } else {
                            ESP_LOGI(TAG, "Output --> %d", p_module_ins->p_output_evt_id[i]);
                        }
                    }
...
```

我们需要向我们的每个输出订阅者发布消息。如您所见，我们为每个订阅者复制了数据。

**内存分配和释放规则**
- 数据生成FM为每个订阅者进行内存分配
- 数据消费FM在数据使用完后进行内存释放。

### 4.5 启动和停止

这些是FM的运行时控制，以支持未来的流程暂停/恢复。目前您可以在实例化后使FM运行，但我们仍建议将逻辑分成FM的生命周期管理和FM的运行时控制。

## 5. 测试

现在我们有了`uart alarmer` FM，在我们提交请求之前，如何在本地测试它。

我们实现了一个控制台命令来本地发起一个任务流。

```shell
SenseCAP> help taskflow
taskflow  [-iej] [-f <string>]
  通过json字符串或SD文件导入任务流，例如：taskflow -i -f "test.json"。

export taskflow to stdout or SD file, eg: taskflow -e -f "test.json"
  -i, --import  导入任务流
  -e, --export  导出任务流
  -f, --file=<string>  文件路径，通过SD导入或导出任务流json字符串，例如：test.json
    -j, --json  通过标准输入导入任务流json字符串
```

请参阅[安装和首次构建](installation_CN.md) - `5. 监控日志输出`以获取控制台。准备一个去除空格和空白字符的任务流，并使用以下命令发出任务流：

```shell
taskflow -i -j<enter>
请键入任务流json：
#<在此粘贴您的任务流json示例>
{"tlid":3,"ctd":3,"tn":"Local Human Detection","type":0,"task_flow":[{"id":1,"type":"ai camera","index":0,"version":"1.0.0","params":{"model_type":1,"modes":0,"model":{"arguments":{"iou":45,"conf":50}},"conditions":[{"class":"person","mode":1,"type":2,"num":0}],"conditions_combo":0,"silent_period":{"silence_duration":5},"output_type":0,"shutter":0},"wires":[[2]]},{"id":2,"type":"alarm trigger","index":1,"version":"1.0.0","params":{"text":"human detected","audio":""},"wires":[[3,4]]},{"id":3,"type":"uart alarm","index":2,"version":"1.0.0","params":{"output_format":1},"wires":[]}]}
```

如何组合任务流？在[软件架构](architecture_CN.md)中介绍了每个FM及其参数。组合任务流基本上就是在FM块之间绘制连线，就像Node-RED一样。

在我们有GUI用于组合任务流之前，我们可以使用导出命令收集示例。只需使用移动应用程序启动启用本地报警功能（RGB灯）的流程，当流程运行时，使用以下命令导出任务流：

```shell
taskflow -e
```

此命令将运行中的任务流导出到控制台。如果任务流非常长，其输出可能会被其他日志中断，在这种情况下，我们需要一个TF卡。将TF卡格式化为FAT/exFAT文件系统，插入Watcher。现在我们可以将运行中的任务流导出到TF卡中，

```shell
taskflow -e -f tf1.json
# 仅支持根目录中的文件名
# 请不要在路径中指定前导目录，命令无法创建目录
```

现在您有了示例，请修改其中一个alarmer FM（通常是最后一个FM），用您的`uart alarmer` FM替换它，并向FM的JSON对象添加一些参数，使用JSON编辑器去除空白字符，并使用上述`taskflow -i -j`命令导入。

就是这样，享受探索吧。

## 附录 - 更多任务流示例

这里我们提供了几个可以开始的任务流示例。

```json
{"tlid":3,"ctd":3,"tn":"Local Human Detection","type":0,"task_flow":[{"id":1,"type":"ai camera","index":0,"version":"1.0.0","params":{"model_type":1,"modes":0,"model":{"arguments":{"iou":45,"conf":50}},"conditions":[{"class":"person","mode":1,"type":2,"num":0}],"conditions_combo":0,"silent_period":{"silence_duration":5},"output_type":0,"shutter":0},"wires":[[2]]},{"id":2,"type":"alarm trigger","index":1,"version":"1.0.0","params":{"text":"human detected","audio":""},"wires":[[3,4]]},{"id":3,"type":"local alarm","index":2,"version":"1.0.0","params":{"sound":1,"rgb":1,"img":0,"text":0,"duration":1},"wires":[]},{"id":4,"type":"sensecraft alarm","index":3,"version":"1.0.0","params":{"silence_duration":30},"wires":[]}]}
```

```json
{"tlid":1,"ctd":1,"tn":"Local Gesture Detection","type":0,"task_flow":[{"id":1,"type":"ai camera","index":0,"version":"1.0.0","params":{"model_type":3,"modes":0,"model":{"arguments":{"iou":45,"conf":65}},"conditions":[{"class":"paper","mode":1,"type":2,"num":0}],"conditions_combo":0,"silent_period":{"silence_duration":5},"output_type":0,"shutter":0},"wires":[[2]]},{"id":2,"type":"alarm trigger","index":1,"version":"1.0.0","params":{"text":"scissors detected","audio":""},"wires":[[3,4]]},{"id":3,"type":"local alarm","index":2,"version":"1.0.0","params":{"sound":1,"rgb":1,"img":0,"text":0,"duration":1},"wires":[]},{"id":4,"type":"sensecraft alarm","index":3,"version":"1.0.0","params":{"silence_duration":30},"wires":[]}]}
```

```json
{"tlid":1719396404172,"ctd":1719396419707,"tn":"Man with glasses spotted, notify immediately","task_flow":[{"id":753589649,"type":"ai camera","type_id":0,"index":0,"vision":"0.0.1","params":{"model_type":0,"model":{"model_id":"60086","version":"1.0.0","arguments":{"size":1644.08,"url":"https://sensecraft-statics.oss-accelerate.aliyuncs.com/refer/model/1705306215159_jVQf4u_swift_yolo_nano_person_192_int8_vela(2).tflite","icon":"https://sensecraft-statics.oss-accelerate.aliyuncs.com/refer/pic/1705306138275_iykYXV_detection_person.png","task":"detect","createdAt":1705306231,"updatedAt":null},"model_name":"Person Detection--Swift YOLO","model_format":"tfLite","ai_framework":"6","author":"SenseCraft AI","description":"The model is a Swift-YOLO model trained on the person detection dataset. It can detect human body  existence.","task":1,"algorithm":"Object Dectect(TensorRT,SMALL,COCO)","classes":["person"]},"modes":0,"conditions":[{"class":"person","mode":1,"type":2,"num":0}],"conditions_combo":0,"silent_period":{"time_period":{"repeat":[1,1,1,1,1,1,1],"time_start":"00:00:00","time_end":"23:59:59"},"silence_duration":60},"output_type":1,"shutter":0},"wires":[[193818631]]},{"id":193818631,"type":"image analyzer","type_id":3,"index":1,"version":"0.0.1","params":{"url":"","header":"","body":{"prompt":"Is there a man with glasses?","type":1,"audio_txt":"Man with glasses"}},"wires":[[420037647,452707375]]},{"id":452707375,"type_id":99,"type":"sensecraft alarm","index":2,"version":"0.0.1","params":{"silence_duration":10,"text":"Man with glasses"},"wires":[]},{"id":420037647,"type_id":5,"type":"local alarm","index":3,"version":"0.0.1","params":{"sound":1,"rgb":1,"img":1,"text":1,"duration":10},"wires":[]}],"type":0}
```







