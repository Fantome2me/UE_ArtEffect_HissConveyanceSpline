# HissConveyanceSpline — 样条线导引系统

> Unreal Engine 5.8 工程：**靠近坐火处（土地庙）的 Hiss 风格引导线**。
> Niagara 粒子沿样条线运动，屏幕 RenderTarget 记录粒子速度在屏幕上的投影，
> 再用 Niagara 的 Grid2D 做 Navier-Stokes 流体解算，最后结合深度值合成出流体质感的引导效果。
> 样条线系统**仍在开发中**。

[![UE](https://img.shields.io/badge/Unreal%20Engine-5.8-0E1128)](https://www.unrealengine.com)
[![Platform](https://img.shields.io/badge/Platform-Win64-blue)]()
[![Status](https://img.shields.io/badge/Status-WIP-orange)]()

---

## 技术方案

整条效果是**粒子 → 屏幕空间速度场 → 流体解算 → 合成**的链路，不是简单的拖尾：

```
① Niagara 粒子沿样条线运动
        │   粒子位置 = 中点路径上的采样点，随时间向土地庙流动
        ▼
② 屏幕 RenderTarget 记录粒子的速度投影
        │   RT_Fluid 存的是屏幕空间速度场（不是颜色）
        ▼
③ Niagara Grid2D 解 Navier-Stokes 流体
        │   用上一步的速度场作为外力项驱动流体，产生扩散、卷曲、涡流
        ▼
④ 结合场景深度合成最终引导线
            流体结果 + 深度值 + Niagara 光效 → Hiss 风格的半透明流体引导线
```

| 资产 | 作用 |
|---|---|
| `Content/ConveyanceSpline/Effect_Spline/Nia_Fluid.uasset` | Grid2D 流体解算的 Niagara 系统 |
| `Content/ConveyanceSpline/Effect_Spline/RT_Fluid.uasset` | 屏幕空间速度场渲染目标 |
| `Content/FX/NS_SplineGlow.uasset` | 沿样条线流动的光效粒子 |
| `Source/.../SplineRouteActor.*` | 提供样条路径与粒子位置（C++） |

> 为什么用屏幕空间速度场而不是直接让粒子拖尾：直接拖尾只能得到"线"，做不出流体的**卷曲和涡流**。
> 把粒子的运动投影到屏幕速度场后交给 Grid2D 解算，才会出现 Hiss 那种黏稠、会自己打卷的质感。

---

## 这个工程里有什么

| 内容 | 位置 | 说明 |
|---|---|---|
| **样条线导引系统** | `Source/.../SplineRouteActor.*` + `Content/ConveyanceSpline/Effect_Spline/` | 核心功能，见下文 |
| **Niagara 特效** | `Content/FX/NS_SplineGlow.uasset`、`Content/ConveyanceSpline/Effect_Spline/Nia_Fluid.uasset` | 沿线流动粒子 + 流体解算 |
| **土地庙资产** | `Content/Target/ASS_Target/` | 树根/树屋/屋顶模型、材质实例、花草植被（58 MB） |
| **模板内容** | `Content/{Characters,ThirdPerson,Variant_Combat,Variant_Platforming,Variant_SideScrolling,LevelPrototyping,Input}` | UE 官方模板资产，未改动 |


---

## 样条线系统

### 它做什么

`ASplineRouteActor` 维护一条**三点样条**：角色 → 动态中点 → 土地庙。

中点不是固定的，它会根据角色和土地庙的相对位置实时调整，让路径既不会突然折返，
也不会在角色靠近时缩成一团。沿路径每隔一段距离挂一颗 Niagara 粒子，粒子从角色向土地庙流动。

### 中点的三种行为

阈值角 =（中点→角色）与（中点→土地庙）两个向量的夹角：

| 角度 | 状态 | 行为 | 为什么 |
|---|---|---|---|
| **> 170°**（几乎共线，中点在两者之间） | A · 瞬贴 | 每帧直接把中点贴到目标位置 | 此时路径本来就是直的，插值没有意义 |
| **< 10°**（同向，角色和土地庙在同一侧） | A · 瞬贴 | 同上 | 路径会绕远，瞬贴最稳定 |
| **10°–60°** | B · 滑行 | 中点以 `MidGlideSpeed` 匀速滑向目标 | 角度在变，硬贴会抖，滑行才有"路线在适应角色"的感觉 |
| **> 60° 且 < 170°** | 保持 | 中点不动 | 中间态，不动最平滑 |

### 静止 / 移动的切换

| 参数 | 默认 | 作用 |
|---|---|---|
| `StopDelay` | 2.0 s | 角色静止超过这个时间，中点开始慢慢移到角色位置 |
| `ResumeDelay` | 5.0 s | 连续移动超过这个时间，中点回到常规逻辑 |
| `MoveThreshold` | 1.0 | 判定"动了"的位移阈值（避免噪声触发） |

> 有一个容易忽略的细节：角色**从静止切回移动的那一帧**，中点会被瞬间拉回 `StationaryPos`。
> 不这么做的话，静止期间中点已经飘到角色脚下，一开始移动就会看到路径"弹"一下。

### 沿线粒子

| 参数 | 默认 | 作用 |
|---|---|---|
| `FxSpacing` | 75 cm | 粒子间距（空隙大小） |
| `FxSpeed` | 300 cm/s | 流动速度，从角色流向土地庙 |
| `FxSize` | 12 cm | 粒子半径 |

实现上用一个**复用的 Niagara 组件池**（`FxComponents`），不每帧创建销毁。
Niagara 资产从 `/Game/FX/NS_SplineGlow` 自动加载；找不到时退回黄色调试球，
**所以缺资产也不会完全没效果**。

### 可调参数一览（Actor 细节面板）

```
Route
├─ StopDelay            2.0     静止多久后中点跟过来
├─ ResumeDelay          5.0     移动多久后恢复正常逻辑
├─ MaxDistance       2000.0     最大路径长度
├─ MoveThreshold        1.0     位移判定阈值
├─ Route|Angle
│  ├─ AngleOpenDeg    170.0     视为"共线"的角度
│  ├─ AngleTinyDeg     10.0     视为"同向"的角度
│  └─ AngleGlideDeg    60.0     进入滑行状态的角度
├─ Route|Glide
│  └─ MidGlideSpeed   100.0     滑行速度（cm/s）
└─ Route|Particles
   ├─ FxSpacing        75.0     粒子间距（cm）
   ├─ FxSpeed         300.0     粒子流速（cm/s）
   └─ FxSize           12.0     粒子半径（cm）
```

---

## 打开工程

1. 需要 **Unreal Engine 5.8**
2. 双击 `UE_ConveyanceSpline.uproject`（首次打开会提示编译 C++ 模块）
3. 地图：`Content/ThirdPerson/Lvl_ThirdPerson`

如果提示缺模块或编译失败，用 VS 打开 `UE_ConveyanceSpline.sln` 重新生成，
或删掉 `Binaries/` `Intermediate/` 后让编辑器重新编译。

---

## 目录结构

```
UE_ConveyanceSpline/
├─ Config/                        工程配置
├─ Content/
│  ├─ ConveyanceSpline/           样条线相关资产
│  │  └─ Effect_Spline/           Nia_Fluid（Niagara 流体）、RT_Fluid（渲染目标）
│  ├─ FX/                        NS_SplineGlow（沿线光效）
│  ├─ Target/ASS_Target/          土地庙：模型 / 材质实例 / 植被
│  ├─ Characters/                 模板角色
│  ├─ ThirdPerson/                第三人称模板地图与蓝图
│  ├─ Variant_Combat/             模板变体：战斗
│  ├─ Variant_Platforming/        模板变体：平台跳跃
│  ├─ Variant_SideScrolling/      模板变体：横版
│  ├─ LevelPrototyping/           原型搭建资产
│  └─ Input/                      增强输入配置
├─ Source/
│  └─ UE_ConveyanceSpline/
│     ├─ Public/SplineRouteActor.h        样条线导引 Actor
│     ├─ Private/SplineRouteActor.cpp
│     └─ Variant_*/                       模板变体的 C++
└─ UE_ConveyanceSpline.uproject
```

---

## 未完成的部分

**样条线 / 路径**
- 中点在极端角度下的过渡还有跳跃（尤其角色绕到土地庙背后时）
- 粒子池大小固定，路径变长时粒子会变稀疏
- 没有做路径遮挡处理（穿过墙时粒子会穿模）

**流体链路**
- `Nia_Fluid` 的 Grid2D 解算只做了原型，参数（粘性 / 扩散 / 涡量）还没调到最终手感
- 屏幕空间速度场的分辨率与粒子数还没做平衡，粒子多时速度场会糊
- 深度合成只做了基本遮挡，没有做软粒子过渡

---

## 许可

MIT。模板资产版权归 Epic Games，遵循 UE 的 EULA。
