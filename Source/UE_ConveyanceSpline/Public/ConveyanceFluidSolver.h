// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Styling/SlateBrush.h"
#include "ConveyanceFluidSolver.generated.h"

class UTexture2D;
class UTextureRenderTarget2D;

/** 屏幕上想看的通道（控制台 fluid.view 0~4 也能切） */
UENUM()
enum class EFluidDebugView : uint8
{
	Displacement  UMETA(DisplayName = "位移场（水痕）"),
	Velocity      UMETA(DisplayName = "速度场"),
	UV            UMETA(DisplayName = "流体UV"),
	Pressure      UMETA(DisplayName = "压力"),
	Divergence    UMETA(DisplayName = "散度"),
};

/**
 * C++ 版的 Navier-Stokes 屏幕空间流体求解器（对应文章里的 8 个模拟阶段）
 *
 *   1 DrawUVColor   -> ResetField(): 每个格子记录自己的 UV，速度/压力清零
 *   2 VolumeForce   -> PushSplat() + InjectSplats(): 把"船"的屏幕速度按高斯核 Lerp 进速度场
 *   3 Advection     -> Advect(): 半拉格朗日回溯 + 双线性采样，同时搬运 UV 和速度
 *   4 Divergency    -> ComputeDivergence(): 用上下左右速度算散度
 *   5 Pressure      -> SolvePressure(): 雅克比迭代（默认 24 次）
 *   6 UpdateVelocity-> ProjectVelocity(): u = w - grad(p)
 *   7 Recover       -> Recover(): 缓慢把 UV 拉回网格自身 UV
 *   8 RenderToRT    -> WriteOutputTexture(): RG = 流体UV，BA = 速度
 *
 * 全程 CPU 计算（无蓝图、无 Niagara、无材质），结果写进一张 RGBA16F 的 UTexture2D，
 * 后处理材质直接采样这张贴图做折射即可。
 */
UCLASS(ClassGroup = (Conveyance), meta = (BlueprintSpawnableComponent))
class UE_CONVEYANCESPLINE_API UConveyanceFluidSolver : public UActorComponent
{
	GENERATED_BODY()

public:
	UConveyanceFluidSolver();

	// ---------- 参数 ----------
	/** 网格分辨率（单边）。256 约 1~2ms/帧；512 约 5~8ms/帧 */
	UPROPERTY(EditAnywhere, Category = "Fluid")
	int32 GridSize = 256;

	/** 压力泊松迭代次数（文章用 32；256 网格用 20~24 足够） */
	UPROPERTY(EditAnywhere, Category = "Fluid")
	int32 PressureIterations = 24;

	/** 速度耗散（每帧乘这个系数），越小尾迹消失越快。0.94 ≈ 0.3 秒衰减一半 */
	UPROPERTY(EditAnywhere, Category = "Fluid")
	float VelocityDissipation = 0.94f;

	/** 笔刷（高斯核）半径，单位 UV */
	UPROPERTY(EditAnywhere, Category = "Fluid")
	float SplatRadiusUV = 0.025f;

	/** 注入强度：越大越"黏"，船拖动水面越明显 */
	UPROPERTY(EditAnywhere, Category = "Fluid")
	float SplatStrength = 3.f;

	/** 速度上限（UV/秒），防止乱流数值爆炸 */
	UPROPERTY(EditAnywhere, Category = "Fluid")
	float MaxSpeedUV = 0.6f;

	/** 第 7 阶段：每帧把流体 UV 拉回自身 UV 的速率（0 = 关闭）。越大 = 划痕消失越快 */
	UPROPERTY(EditAnywhere, Category = "Fluid")
	float RecoverRate = 0.10f;

	/** 划痕（粒子笔迹）的存活时间（秒）：越早的划痕衰减越多 */
	UPROPERTY(EditAnywhere, Category = "Fluid")
	float TraceLifeSeconds = 0.35f;

	/** 远离笔迹的区域每帧速度衰减（0.75 = 每帧砍掉 25%），保证效果只留在粒子周围 */
	UPROPERTY(EditAnywhere, Category = "Fluid")
	float FieldKillFactor = 0.75f;

	/** 是否跑压力求解（关掉可看"只对流不做投影"的对比效果） */
	UPROPERTY(EditAnywhere, Category = "Fluid")
	bool bEnablePressureSolve = true;

	/** 每秒打印一次流场统计（验证解算是否在跑） */
	UPROPERTY(EditAnywhere, Category = "Fluid|Debug")
	bool bLogStats = true;

	/** 把流场直接铺满屏幕（纯 C++ 可视化，不需要任何材质） */
	UPROPERTY(EditAnywhere, Category = "Fluid|Debug")
	bool bShowOnScreen = true;

	/** 可视化放大倍数：位移场数值很小（0.001~0.02），放大后才看得见 */
	UPROPERTY(EditAnywhere, Category = "Fluid|Debug")
	float DisplayScale = 60.f;

	/** 可视化亮度曲线指数（<1 提亮弱流，>1 只显示强流） */
	UPROPERTY(EditAnywhere, Category = "Fluid|Debug")
	float DisplayPower = 0.6f;

	/** 可视化整体不透明度 */
	UPROPERTY(EditAnywhere, Category = "Fluid|Debug")
	float DisplayOpacity = 0.85f;

	/** 屏幕上显示哪个通道 */
	UPROPERTY(EditAnywhere, Category = "Fluid|Debug")
	EFluidDebugView DebugView = EFluidDebugView::Displacement;

	/** 把当前视图存成 PNG：Saved/FluidDebug/fluid_<时间>.png（控制台 fluid.dump 也可） */
	UFUNCTION(BlueprintCallable, Category = "Fluid|Debug")
	void DumpFieldToPNG();

	/** 切换显示通道（0=位移 1=速度 2=UV 3=压力 4=散度），供控制台 fluid.view 使用 */
	void SetDebugViewByIndex(int32 Index);

	/** 目标渲染目标资产（例如 /Game/ConveyanceSpline/Effect_Spline/RT_Fluid）
	 *  设了它就每帧把场写进去 —— 在内容浏览器里双击该 RT 就能实时看到画面 */
	UPROPERTY(EditAnywhere, Category = "Fluid|Output")
	TObjectPtr<UTextureRenderTarget2D> TargetRT;

	// ---------- 对外接口 ----------
	/** 每帧注入前调用：一颗粒子的屏幕位置（UV 0~1）+ 屏幕速度（UV/秒） */
	void PushSplat(const FVector2f& InUV, const FVector2f& InVelocityUVPerSec);

	/** 跑一帧完整解算（文章 8 个阶段的顺序） */
	void Simulate(float DeltaSeconds);

	/** 把所有场清零并重新初始化 UV */
	void ResetField();

	/** 结果纹理：R,G = 流体 UV；B,A = 速度（供后处理材质采样） */
	UTexture2D* GetFieldTexture() const { return FieldTexture; }

	/** 最近一帧解算耗时（毫秒），调试用 */
	float GetLastSimMs() const { return LastSimMs; }

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	// ---------- 缓冲 ----------
	int32 Num = 0;                       // 单边格子数
	TArray<FVector2f> UVField;           // 流体 UV（文章里的 UVColor）
	TArray<FVector2f> UVTemp;
	TArray<FVector2f> VelField;          // 速度场（UV/秒）
	TArray<FVector2f> VelTemp;
	TArray<float> Pressure;              // 压力
	TArray<float> PressureTemp;
	TArray<float> Divergence;            // 散度
	TArray<float> Trace;                 // 笔迹/活跃度（1 = 刚被粒子划过，随时间衰减到 0）
	TArray<FFloat16> PixelHalfs;         // 输出像素（每个像素 4 个 half：RGBA）

	TArray<FVector2f> PendingSplatUV;
	TArray<FVector2f> PendingSplatVel;

	UPROPERTY(Transient)
	TObjectPtr<UTexture2D> FieldTexture;

	float LastSimMs = 0.f;
	bool bBuffersReady = false;
	float LogAccum = 0.f;
	int32 LastSplatCount = 0;

	/** 屏幕可视化用的 LDR 纹理（B8G8R8A8） */
	UPROPERTY(Transient)
	TObjectPtr<UTexture2D> DisplayTexture;

	TArray<FColor> DisplayPixels;
	FSlateBrush ScreenBrush;
	TSharedPtr<class SWidget> ScreenWidget;
	bool bScreenWidgetAdded = false;

	void CreateScreenWidget();
	void UpdateDisplayTexture();

	/** 把某个通道画成 FColor（屏幕显示和 PNG 导出共用同一套配色） */
	void BuildViewPixels(TArray<FColor>& OutPixels, bool bOpaque) const;

	// ---------- 内部步骤 ----------
	void InitializeBuffers();
	void InjectSplats(float DeltaSeconds);
	void Advect(float DeltaSeconds);
	void ComputeDivergence();
	void SolvePressure();
	void ProjectVelocity();
	void Recover();
	void WriteOutputTexture();
	/** 把 RGBA16F 的场数据直接写进 TargetRT（这样在编辑器里能实时看到 RT 内容） */
	void PushFieldToRenderTarget();
	/** 笔迹随时间衰减 + 远离笔迹的区域把速度砍掉（保证效果只留在粒子附近） */
	void DecayTraceAndLocalize(float DeltaSeconds);

	FORCEINLINE int32 IndexOf(int32 X, int32 Y) const { return Y * Num + X; }
	FORCEINLINE int32 ClampIndex(int32 V) const { return FMath::Clamp(V, 0, Num - 1); }

	/** 在给定 UV 处对速度场做双线性采样（越界钳制） */
	FVector2f SampleVelocityBilinear(const TArray<FVector2f>& Field, const FVector2f& InUV) const;
	/** 同上，采样标量场（压力/散度） */
	float SampleScalarBilinear(const TArray<float>& Field, const FVector2f& InUV) const;
};
