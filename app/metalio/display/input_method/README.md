# HiOS 手机式轻量输入法

原生 LVGL 9 控件实现，默认九宫格拼音，也可切换 QWERTY 全键盘。
深色圆角按键、30px 大字、蓝色完成键，已接入「文字生图」。无需联网，不增加字体。

## 使用

- 九宫格每个字母组按一次：`MNO → GHI → GHI → ABC → MNO` 即 `nihao`，选择「你好」。
- 同码拼音存在歧义时，点候选下方的 `mi / ni` 等读音，缩小汉字范围。
- 「分词」插入拼音分隔符。删除优先删拼音，拼音为空时删除正文；长按可连续删除。
- 「全键 / 九键」切换布局；本次运行会记住所选布局。QWERTY 中 `v` 表示 ü。
- 「中/英」「123」「符号」切换输入模式；数字页、符号页的「返回」回到文字键盘。
- 默认显示三项候选，首项蓝色突出；「候选」展开每页 12 项候选，可前后翻页。「更多」打开关于弹窗。
- 空格选择当前页第一项，拼音为空时输入空格。完成键先提交拼音再发送 READY。
- 「收起」只隐藏键盘，保留正文和拼音；重新点击当前输入框继续编辑。
- QWERTY 下点击顶部拼音可原样上屏；九宫格不将内部数字编码误当正文提交。
- 地址、密码及受字符过滤限制的字段自动使用 QWERTY 直接输入，不能切中文。

九键、全键都支持连续拼音中的常用词组优先上屏，例如 `nihaoshijie` →「你好世界」、
`nihaoma` 先选择「你好」再选剩余字。不是完整的全句输入引擎。

## 接入

在 LVGL UI 线程使用 `hi_input_method.h`：

1. `HiInputMethod_Create(parent)` 返回普通面板，720px 屏推荐高度 360px。
2. `HiInputMethod_SetTextArea(panel, textarea, chinese_allowed)` 关联输入框并显示键盘。
3. 在输入框的 FOCUSED 和 CLICKED 事件中调用该方法。同一字段再次点击不会丢失拼音，
   切换到其他字段则丢弃未确认拼音。删除输入框或面板自动解除关联。
4. 如需设定默认布局，调用 `HiInputMethod_SetLayout(panel, HiInputLayout::Qwerty)`。
5. 面板监听 `LV_EVENT_READY`；独立提交按钮读取 textarea 前调用 `HiInputMethod_Flush`。
   失败时保留编辑状态。页面的取消按钮自行关闭表单；收起键不会发送取消事件。

## 资源与范围

两种拼音引擎的输入缓冲均固定 32 字节，不分配堆内存；候选从静态词库读取。
九键用最多约 1.6KB 栈空间整理读音次序，避免尾页反复扫描整个词库。
词库含 405 音节、3612 个字、5025 个候选读音，约 23KB 源文件，位于固件只读区。
生成器读取现有字体 cmaps，过滤缺字；复用 `font_puhui_20_4`、`font_puhui_30_4` 及其中文回退。
UI 每面板分配一个固定 Context 和 LVGL 控件。

词库无声调，没有用户词频学习、模糊音、云词库或全句语言模型。
常用词组表覆盖问候、设备操作和画面描述。不是移植小米的闭源输入法。

固件不依赖 Python。更新词库时使用：

```sh
python -m pip install pypinyin==0.55.0
python scripts/generate_ime_dictionary.py
```

读音数据 MIT 许可见 `DICTIONARY_LICENSE.txt`。

## 验证

```sh
cmake -S scripts/tests/input_method -B build/input-method -DCMAKE_BUILD_TYPE=Debug
cmake --build build/input-method --parallel 4
ctest --test-dir build/input-method --output-on-failure
```

测试使用仓库真实 LVGL 和中文字体，覆盖两个引擎的全部音节、字形、实际九键与候选触摸、
拼音歧义、连续词组、分页、QWERTY、数字、符号、英语、密码、收起/恢复、长度限制及销毁顺序。
键位在开始/结束拼音编辑时保持稳定。测试将真实显示缓冲写出 `nine_key.ppm` 和 `qwerty.ppm`。
板上仍需实际试按确认触摸体验及文字生图页面的整体效果。
