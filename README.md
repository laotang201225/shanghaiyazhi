# DamageSuppressor (0627)

0627 副本最终伤害压制插件（挑战/负重模式），独立目录、独立维护与构建。

## 当前进度（2026-11-14）

### 1. 已完成并实机验证通过
- **唯一扣血漏斗方案（`0x008174E0`）**：
  - 通过 IDA 9.4 对 `dnf-workbench/ida/DNF_dump.exe.i64` 中全部 **300 个**战斗对象/怪物/角色/机关虚表做全量审计，确认：
    - 上游受击/计算路径分散（`+0x2DC` 78 个实现、`+0x388` 21 个、`+0x3A0` 32 个、`+0x538` 17 个）；
    - 所有伤害最终汇合到唯一扣血终端：`+0x50C`（`0x008174E0`，`CNRDActiveObject::setHP`，300 个虚表共用且无 VMP 保护）与 `+0x2A4`（`0x00D07A40`，读取当前 HP）。
  - 当前 [DamageSuppressor.cpp](DamageSuppressor.cpp) 在游戏 UI 线程校验并 Hook `0x008174E0`，读取 `curHp - newHp` 反算真实伤害 `rawDmg`，按 [gaoDamageSuppressor.ini](gaoDamageSuppressor.ini) 的 `suppress_percent` 缩放并写回 `newHp`。
- **防误伤与归属判定**：
  - 仅在副本模块（`[0x01A5FB4C] -> +0x14 -> +0x28`，支持 `all_dungeons=1`）生效，城镇不生效。
  - 排除 `self == localPlayer`（`[0x01AB7CDC]`），玩家自身受击不压制。
  - 通过 `vtable + 0x32C` 向上追溯最多 4 层归属，覆盖本地角色本体、技能衍生物与投射物。
- **毕业号实机验证**：
  - 已在 `H:\123pan\Downloads\2019益起来单机测试25年复刻\2019益起来单机测试\gaoDamageSuppressor.dll` 实机测试通过（`suppress_percent=99.99`，`min_damage=1`）。
  - 日志 `gaoDamageSuppressor.log` 验证：**0 条 `skip#`**，普攻、大技能（`rawDmg=1909429 ~ 3812221`）、投射物/特效（`atk=001C1C00`/`001C2A00`）、以及 4–6ms 间隔的高频多段连击（`setHP#13`–`#40`）全部精确压制，怪物血量从 `90` 逐段降至 `61`。

### 2. 已确认的下一步开发计划（尚未编码）
1. **装备联动压制机制（已与用户确认规则）**：
   - 在 [gaoDamageSuppressor.ini](gaoDamageSuppressor.ini) 中按**装备 ID**配置压制减免值（划分 lv1 / lv2 等档位或直接配置每件装备的减免百分比）。
   - 计算公式：**基础压制率 − 当前穿戴装备提供的减免值累加**（如基础 99.99% 或 90%，穿戴指定装备后按件扣减对应比例）。
2. **性能与稳定性设计**：
   - 启动时一次性加载 ini 装备表到内存；
   - 通过 UI 线程轻量 `SetTimer` 低频刷新（例如 500ms）读取本地角色穿戴栏装备 ID，计算并缓存最终倍率 `g_effectiveScale`；
   - 高频 `Hook_SetHp` 保持纯 `O(1)` 乘法，零遍历、零磁盘 I/O。
3. **待完成的技术确认**：
   - 在 `DNF_dump.exe.i64` 中从 `0x004B3670` / `0x004BA410`（调用 `0x007B03C0` `Banner__getItemBySlot`）确认 0627 本地角色穿戴栏各槽位 `itemId` 的读取路径；
   - 可选增强：补上 `0x008174E0` 原生使用的 `vtable + 0x330` 顶层归属检查，以及排除受击方为本地角色召唤物（`GetOwnerOrSelf(self) != localPlayer`）。

## 构建与部署

```bat
DamageSuppressor\build_suppressor.bat
```

- 编译产物：`DamageSuppressor\_build\gaoDamageSuppressor.dll`
- 冒烟测试部署目录：`H:\123pan\Downloads\2019益起来单机测试25年复刻\2019益起来单机测试\`（含 `gaoDamageSuppressor.dll` 与 `gaoDamageSuppressor.ini`，按 [AGENTS.md](AGENTS.md) 规范部署前可直接结束旧 `DNF.exe` 进程）。
