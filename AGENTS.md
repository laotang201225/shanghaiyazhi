# DamageSuppressor 规范

- 独立目录维护与构建，不跨目录引用 `dnf-workbench` 或 `DungeonRogueMvp` 的源码、配置或产物。
- 源码 UTF-8，构建 `/utf-8`。
- 冒烟测试换版前若检测到客户端 `DNF.exe` 仍在运行，直接结束 `DNF.exe` 进程后再覆盖/清理测试 DLL 与日志，无需额外征求用户意见。
- 永远不修改客户端原有的 `gaocsm.dll` 与 `gao默认创缔造为了玩女鬼方便插件.dll`。
