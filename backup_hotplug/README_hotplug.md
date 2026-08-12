# 备份: 热插拔配置 (uinput 按需创建 + X11 输入)

这是之前"可用"的配置备份:

- **input_x11**: 启用(渲染线程轮询, F1 菜单 + X 瞄准键, XQueryKeymap)
- **uinput 虚拟鼠标**: 首次按住 X 时创建, 松开 X 即销毁(热插拔)
- 瞄准 + 自动开火走 uinput

已知问题: 松开 X 销毁设备时, Wayland 合成器可能偶发一次指针跳变("偶尔自己动")。

## 构建
```sh
cd backup_hotplug
cmake -S . -B build -DBUILD_INTERNAL=ON
cmake --build build --target cs2_internal -j
cp build/libcs2_internal.so ~/Repo/injector/cmd/libcs2_internal.so
```

## 与主版本(XTest + 游戏键盘读取)的区别
| | 热插拔备份(本目录) | 主版本 |
|---|---|---|
| 输入模块 | X11(input_x11) | 无 X11, 读游戏键盘状态 |
| 瞄准移动 | uinput 按需创建/销毁 | XTest 注入 |
| F1/X 键 | X11 XQueryKeymap | 游戏 input 对象位图 |
| 鼠标稳定性 | 可能偶发自走 | 更稳 |

想切回本配置: 用本目录的 `internal/entry.cpp` + `CMakeLists.txt`(或直接在本目录构建)。
