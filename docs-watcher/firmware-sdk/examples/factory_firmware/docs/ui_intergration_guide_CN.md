**UI集成指南**
=================
## 目录
1. [UI 组件结构](#1-ui-组件结构)
2. [组管理](#2-组管理)
   - [概述和实现](#21-概述和实现)
   - [添加对象到指定组中](#22-添加对象到指定组中)
   - [页面跳转和页面对象管理](#23-页面跳转和页面对象管理)
   - [关联编码器和组](#24-关联编码器和组)
   - [打印GroupInfo结构体变量中的对象](#25-打印groupinfo结构体变量中的对象)
   - [示例用法](#26-示例用法)
3. [设备报警](#3-设备报警)
   - [概述和实现](#31-概述和实现)
   - [初始化报警事件相关的UI](#32-初始化报警事件相关的ui)
   - [开启报警UI](#33-开启报警ui)
   - [关闭报警UI](#34-关闭报警ui)
4. [AI推理实时画面渲染](#4-ai推理实时画面渲染)
   - [概述和实现](#41-概述和实现)
   - [初始化图像预览功能](#42-初始化图像预览功能)
   - [刷新预览图像](#43-刷新预览图像)
5. [UI消息事件定义](#5-ui消息事件定义)
   - [概述和实现](#51-概述和实现)
   - [UI事件处理相关函数](#52-ui事件处理相关函数)
   - [用法](#53-用法)
6. [应用](#6-应用)
   - [在Squareline中创建UI对象和回调函数](#61-在squareline中创建ui对象和回调函数)
   - [从Squareline中导出UI工程](#62-从squareline中导出ui工程)
   - [实现头文件中声明的回调函数](#63-实现头文件中声明的回调函数)
   - [将对象添加到结构体变量中](#64-将对象添加到结构体变量中)
   - [UI初始化](#65-ui初始化)
   - [查看运行效果](#66-查看运行效果)
7. [SquareLine工程](#7-squareline工程)
8. [文件说明](#8-文件说明)

## 1. **UI组件结构**

在这个教程中，你将学会如何将自己的UI设计和相关的逻辑函数集成到`view`目录中去。所有的UI设计和逻辑函数都会放在`view`目录中，这个组件中包含了`ui`和`ui_manager`这两个子目录。除此之外，`view`目录中还包含了`view.c`、`view_alarm.c`、`view_image_preview.c`、`view_pages.c`和对应的`.h`头文件。具体的框架如下图所示：

<div align="center">
  <img src="img/ui_framework.png" alt="SenseCAP-Watcher UI framework" />
</div>

- `ui`子目录包含了所有的用户自定义的ui设计，此工程中的`ui`都是由Squareline工具生成的

- `ui_manager`子目录中包含了对UI中的各种自定义动画、对象的组管理和定义以及各种类型事件的回调函数定义

- `view`开头的源文件中则是定义了一些全局页面和相关事件回调函数

- `UI`会通过发送和监听事件来和`APP layer`进行数据交互

<b><i><span style="color:red">提示：阅读下面的模块定义有助于对整个UI框架的理解和使用，如果想要通过上手掌握UI的集成可以直接跳至手册的第6章应用阅读</span></i></b>


## **2. 组管理**

### 2.1 概述
SenseCAP Watcher支持触摸屏和编码器输入设备。为了同步这些输入设备的动作并确保正确，需要进行组管理以保持对正确对象的聚焦以避免触发事件冲突。

组管理功能在以下文件中实现：
- **pm.c**：包含函数实现。
- **pm.h**：包含函数原型和类型定义。

### 2.2 添加对象到指定组中
```c
static void addObjToGroup(GroupInfo *groupInfo, lv_obj_t *objects[], int count);
```
其中`groupInfo`为指向将要添加对象的 `GroupInfo` 结构体的指针，`objects`为要添加到组中的对象数组，`count`为数组中的对象数量。

**用法：**
```c
// 定义页面中要添加的objects
lv_obj_t *example_objects[] = {example_obj1, example_obj2, ...};
// 将objects放进结构体变量中
addObjToGroup(&group_page_example, example_objects, sizeof(example_objects) / sizeof(example_objects[0]));
```

### 2.3 页面跳转和页面对象管理
```c
void lv_pm_open_page(lv_group_t *group, 
                      GroupInfo *groupInfo, 
                      pm_operation_t operation, 
                      lv_obj_t **target, 
                      lv_scr_load_anim_t fademode,
                      int spd, 
                      int delay, 
                      void (*target_init)(void));
```
**参数：**
- `group`：指向 LVGL 组的指针。
- `groupInfo`：包含页面对象的 `GroupInfo` 结构体的指针。
- `operation`：要执行的操作（将对象添加到组、无操作或清除组）。
- `target`：新页面的目标对象。
- `fademode`：屏幕加载动画模式。
- `spd`：屏幕加载动画的速度。
- `delay`：屏幕加载动画开始前的延迟。
- `target_init`：目标屏幕的初始化函数。

**用法：**
```c
// 将结构体变量中存放的objects添加进group中并跳转到对应页面
lv_pm_open_page(g_example, &group_page_example, PM_ADD_OBJS_TO_GROUP, &ui_Page_Example, LV_SCR_LOAD_ANIM_NONE, 0, 0, &ui_Page_Example_screen_init);
```

### 2.4 关联编码器和组
创建group，遍历获取输入设备，将输入设备Encoder和组关联起来，从而使得Encoder可以控制组中的对象。
```c
void lv_pm_init(void)
{
  // 创建组
  g_main = lv_group_create();
    cur_drv = NULL;
    // 循环获取输入设备
    while ((cur_drv = lv_indev_get_next(cur_drv)))
    {
        // 当输入设备为编码器时，将编码器和组进行关联，从而能够通过编码器控制组中对象
        if (cur_drv->driver->type == LV_INDEV_TYPE_ENCODER)
        {
            lv_indev_set_group(cur_drv, g_main);
            break;
        }
    }
    // 定义不同的GroupInfo结构体变量中的对象
    initGroup();
}
```

**用法：**
```c
//在`view_init`中调用，初始化创建group并将encoder和group进行关联
int view_init(void)
{
  // 注意：任何对lvgl任务中对象的操作都必须在线程锁中进行！
  lvgl_port_lock(0);
  // 初始化UI
  ui_init();
  // 初始化创建group并关联Encoder
  lv_pm_init();
  lvgl_port_unlock();
}
```


### 2.5 打印Groupinfo结构体变量中的对象
```c
static void printGroup(GroupInfo *groupInfo);
```
其中`groupInfo`为指向将要添加对象的 `GroupInfo` 结构体的指针，需要注意的是打印前需要给对象设置`user_data`，通过`lv_obj_set_user_data(example_obj, "example_obj_print")`即可设置

**用法：**
```c
printGroup(&group_page_example);
```


### 2.6 示例用法

1.定义一个`GroupInfo`变量
```c
GroupInfo group_page_example;
```
2.在`initGroup()`中初始化对象
```c
lv_obj_t * example_objects[] = {example_obj1, example_obj2, ...};
```
3.将对象添加到组中
```c
addObjToGroup(&group_page_example, example_objects, sizeof(example_objects) / sizeof(example_objects[0]));
```
4.打开页面并添加组
```c
lv_pm_open_page(g_example, &group_page_example, PM_ADD_OBJS_TO_GROUP, &ui_Page_Example, LV_SCR_LOAD_ANIM_NONE, 0, 0, &ui_Page_Example_screen_init);
```
通过执行这些步骤，可以确保触摸屏和编码器输入在应用程序中同步并正确运行。



## 3. 设备报警
### 3.1 概述
本章节介绍了如何在你的Watcher中集成和使用报警UI组件。通过理解和使用以下功能函数来管理设备的UI报警行为

报警UI在以下文件中实现：
- **view_alarm.c**：包含函数实现。
- **view_alarm.h**：包含函数原型和类型定义。

### 3.2 初始化报警事件相关的UI
```c
int view_alarm_init(lv_obj_t *ui_screen);
```
`ui_screen`为指向用于显示报警UI组件的屏幕对象的指针

**用法：**
```c
// 将报警相关UI创建在顶层
view_alarm_init(lv_layer_top());
```


### 3.3 开启报警UI
```c
int view_alarm_on(struct tf_module_local_alarm_info *alarm_st);
```
`alarm_st`为指向`tf_module_local_alarm_info`结构体的指针，该结构体中包含了报警的相关信息，比如`报警的持续时间`、`是否显示文本和图片`和`文本和图片的具体内容`

**用法：**
```c
struct tf_module_local_alarm_info info;
view_alarm_on(&info);
```

### 3.4 关闭报警UI
```c
void view_alarm_off();
```

**用法：**
```c
// 将报警相关UI进行隐藏、设置对应标志位或执行页面跳转逻辑
view_alarm_off();
```


## 4. AI推理实时画面渲染
### 4.1 概述
本章节介绍了如何在设备上解码图片并在LVGL中显示

该功能在以下文件中实现：
- **view_image_preview.c**：包含函数实现。
- **view_image_preview.h**：包含函数原型和类型定义。

### 4.2 初始化图像预览功能
```c
int view_image_preview_init(lv_obj_t *ui_screen);
```
`ui_screen`为指向用于显示实时预览的屏幕对象的指针。该函数的功能是对JPEG解码器进行初始化，同时分配内存，此外还会进行一些UI对象的初始化创建，用来渲染AI推理结果的信息，比如目标检测框和分类名称等

**用法：**
```c
// 将报警相关UI创建在ViewLive页面上
view_image_preview_init(ui_Page_ViewLive);
```


### 4.3 刷新预览图像
```c
int view_image_preview_flush(struct tf_module_ai_camera_preview_info *p_info);
```
`p_info`为指向`tf_module_ai_camera_preview_info`结构体的指针，该结构体中包含了图片和AI模型的推理信息

**用法：**
```c
struct tf_module_ai_camera_preview_info info;
view_image_preview_flush(&info);
```

## 5. UI消息事件定义
### 5.1 概述
设备的前端UI需要和后端APP任务进行数据交互。通过对特定事件进行监听消费，从而实现各种ui界面更新和页面跳转逻辑。想详细了解ESP32的事件处理使用请参考乐鑫官方文档中的`Event Loop Library`章节

UI消息事件处理在以下文件中实现：
- **view.c**: 包含函数实现。
- **view.h**: 包含函数原型和类型定义。
- **data_defs.h**: 包含了各种事件ID(前端和后端)的枚举声明

### 5.2 UI事件处理相关函数
```c
esp_err_t esp_event_handler_instance_register_with( esp_event_loop_handle_t event_loop, 
                                                    esp_event_base_t event_base, 
                                                    int32_t event_id, 
                                                    esp_event_handler_t event_handler, 
                                                    void * event_handler_arg, 
                                                    esp_event_handler_instance_t * instance ) 
```
**参数：**
- `event_loop`:将此处理程序函数注册到事件循环中，不能为NULL
- `event_base`:要注册处理程序的事件的基本ID
- `event_id`:要注册处理程序的事件的ID
- `event_handler`:在事件被调度时调用的处理函数
- `event_handler_arg`:除事件数据之外的数据，在调用处理程序时传递给处理程序
- `instance`:与已注册的事件处理程序和数据相关的事件处理程序实例对象，可为NULL



### 5.3 用法
#### 1. 声明定义事件，并将UI事件处理程序的实例注册到特定循环
```c
// VIEW事件基础声明定义
ESP_EVENT_DECLARE_BASE(VIEW_EVENT_BASE);
esp_event_loop_handle_t app_event_loop_handle;
// 将事件ID声明为枚举，在SenseCAP-Watcher工程中我们会放在data_defs.h中
enum {
    VIEW_EVENT_EXAMPLE
}
// 注册实例
ESP_ERROR_CHECK(esp_event_handler_instance_register_with(app_event_loop_handle, 
                                                            VIEW_EVENT_BASE, VIEW_EVENT_EXAMPLE, 
                                                            __view_event_handler, NULL, NULL));
```

#### 2. UI消息事件处理
```c
static void __view_event_handler(void* handler_args, esp_event_base_t base, int32_t id, void* event_data)
{
  // 获取lvgl线程锁
  lvgl_port_lock(0);
    if(base == VIEW_EVENT_BASE){
        switch (id)
        {
          // 自定义事件
          case VIEW_EVENT_EXAMPLE:{
            ESP_LOGI("ui_event", "VIEW_EVENT_EXAMPLE");
            // 根据收到的事件执行对应的逻辑
            break;
          }
        }
        default:
          break;
    }
  // 释放lvgl线程锁
  lvgl_port_unlock();
}
```

#### 3. 发送UI消息事件
```c
//发送事件触发对应逻辑
esp_event_post_to(app_event_loop_handle, VIEW_EVENT_BASE, VIEW_EVENT_EXAMPLE, NULL, 0, pdMS_TO_TICKS(10000));
```


## **6. 应用**
现在我们会结合上面介绍到的功能函数，将一个简单的UI例子集成到SenseCAP Watcher设备中，这其中会涉及到使用Squareline进行UI设计、UI回调事件定义、对象组管理等各个部分

### 6.1 在Squareline中创建UI对象和回调函数
在Squareline中创建按钮，设置好名字和样式以后，给每个按钮设置回调函数。接着在`Events`中点击`ADD EVENT`后，选择事件的触发类型后再给回调函数命名，到这里就完成了UI对象和相关回调函数的创建

<div align="center">
  <img src="img/ui_img1.png" alt="ui example image_1" style="width: 1398px; height: 876px;"/>
</div>

### 6.2 从Squareline中导出`ui`工程
在应用中选择导航栏的`File` -> `Project Settings`,在`FILE EXPORT`中将`UI Files Export Path`设置为`project_path/ui`,其中的`project_path`为squareline工程的路径。到这里我们就将ui设计的导出路径设置好了
<div align="center">
  <img src="img/ui_img2.png" alt="ui example image_2" style="width: 600px; height: 600px;"/>
</div>

接着我们需要点击导航栏的`Export` -> `Export UI Files`即可导出一个包含了所有UI设计的目录文件夹
<div align="center">
  <img src="img/ui_img3.png" alt="ui example image_3" style="width: 700px; height: 400px;"/>
</div>

### 6.3 实现头文件中声明的回调函数
将`ui`文件夹导入SenseCAP Watcher的工程中，打开并参考`ui`的`ui_events.h`中声明的函数，在`ui_manager`的`ui_events.c`中将函数进行具体实现以完成这个回调函数的逻辑

举例来说, 在`ui_events.h`:
```c
void btn1click_cb(lv_event_t * e);
void btn2click_cb(lv_event_t * e);
void btn3click_cb(lv_event_t * e);
```
对应在`ui_events.c`中的代码就会是:
```c
void btn1click_cb(lv_event_t * e)
{
    ESP_LOGI("ui_example", "btn1click_cb");
    // 定义该对象触发clicked事件后的操作逻辑
}
void btn2click_cb(lv_event_t * e)
{
    ESP_LOGI("ui_example", "btn2click_cb");
    // 定义该对象触发clicked事件后的操作逻辑
}
void btn3click_cb(lv_event_t * e)
{
    ESP_LOGI("ui_example", "btn3click_cb");
    // 定义该对象触发clicked事件后的操作逻辑
}
```

### 6.4 将对象添加到结构体变量中
这一步我们需要将输入设备encoder和创建的group进行管理，然后往group里添加和删除对象就可以实现encoder对对象的控制了。
```c
// 定义一个GroupInfo变量
GroupInfo group_page_example;
//在initGroup()中初始化对象
lv_obj_t * example_objects[] = {ui_Button1, ui_Button2, ui_Button3};
// 将对象添加到结构体变量中，方便后续直接将不同页面中要控制的对象直接添加到group中
addObjToGroup(&group_page_example, example_objects, sizeof(example_objects) / sizeof(example_objects[0]));
```


### 6.5 UI初始化
在`view.c`的`view_init`中调用`ui_init`来实现UI的初始化，这样当lvgl的任务线程运行起来后便可加载我们设计好的UI，默认加载的页面是在Squareline中设计的第一个页面
```c
int view_init(void)
{
  // 注意：任何对lvgl任务中对象的操作都必须在线程锁中进行！
  lvgl_port_lock(0);

  ui_init();
  lv_pm_init();
  // 将对象添加到group中有两种方式
  // 第一种：清空group中的对象后依次添加到group中
  lv_group_remove_all_objs(g_example);
  lv_group_add_obj(ui_Button1);
  lv_group_add_obj(ui_Button2);
  lv_group_add_obj(ui_Button3);

  // 第二种：直接通过跳转页面函数将对应的对象添加到组中：
  lv_pm_open_page(g_example, &group_page_example, PM_ADD_OBJS_TO_GROUP, &ui_Page_Example, LV_SCR_LOAD_ANIM_NONE, 0, 0, &ui_Page_Example_screen_init);

  lvgl_port_unlock();

  //其他初始化代码
}
```

### 6.6 查看运行效果
到这里我们已经简单实现了将UI集成到工程中，接下来我们就可以将代码编译并烧录进Watcher中查看运行效果了！

<div align="center">
  <img src="img/ui_img4.png" alt="ui example image_4" />
</div>

如上图所示，通过触摸屏点击或者使用滚轮点击页面中的按钮，可以在串口调试助手中看到相关对象触发了回调事件后打印的Log，说明回调函数已经成功起作用了！


## 7. SquareLine工程
SenseCAP-Watcher的绝大部分页面都是使用Squareline完成的，使用Squareline工具可以非常简单且快速地对Watcher中各种页面对象进行样式修改，因此非常推荐使用Squareline来进行UI的开发和迭代

<div align="center">
  <img src="img/ui_img5.png" alt="ui example image_5" style="width: 1200px; height: 680px;"/>
</div>

如上图所示的，工具中的页面按照跳转逻辑放置，相邻的页面可以通过按钮或者其他可触发对象进行跳转，可以通过点击对应的页面和对象查看定义的事件，你可以非常简单地对不同页面和页面中的对象进行样式修改，客制化属于你的人工智能助手！但需要注意的是，目前页面中定义好的对象和回调事件跟Watcher的APP层功能绑定，如果修改了可能会影响Watcher功能的正常使用，建议只能对对象的样式，比如颜色大小等进行修改，这样可以确保Watcher功能的正常使用

## 8. 文件说明
- `ui_intergration_demo\SenseCAP-Watcher`文件夹中存放的是SenseCAP-Watcher的完整Squareline工程，里面包含了几乎所有的UI资源设计
- `ui_intergration_demo\ui_intergration_example`中存放的是`应用`章节中入门例程的Squareline工程
- `ui_intergration_demo\view`为`应用`章节入门例程的`view`组件，将原工程的`view`直接覆盖便可使用应用例程




