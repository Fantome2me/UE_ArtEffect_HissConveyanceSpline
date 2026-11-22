// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Components/SplineComponent.h"
#include "SplineRouteActor.generated.h"

class UNiagaraComponent;
class UNiagaraSystem;
class UConveyanceFluidSolver;

UCLASS()
class UE_CONVEYANCESPLINE_API ASplineRouteActor : public AActor
{
	GENERATED_BODY()
	
public:	
	// Sets default values for this actor's properties
	ASplineRouteActor();//用于创建组件的地方
	void BuildThreePointRoute(const FVector& WorldStart, const FVector& WorldEnd);

	FORCEINLINE USplineComponent* GetSpline() const
	{
		return Spline;
	}

protected:
	// Called when the game starts or when spawned
	virtual void BeginPlay() override;

public:	
	// Called every frame
	virtual void Tick(float DeltaTime) override;

private:
	UPROPERTY(VisibleAnywhere , Category = "Route")
	TObjectPtr<USplineComponent> Spline;

	//中点,判断点初始化
	FVector MidPoint = FVector::ZeroVector;
	FVector LastPlayerLoc = FVector::ZeroVector;

	/** 角色静止时所在的位置（只在静止帧更新）
	 *  用途：角色从"静止停留"切换回"移动"的那一帧，把 MidPoint 瞬间拉回这个位置 */
	FVector StationaryPos = FVector::ZeroVector;

	/** 是否已经进入"静止停留"状态（静止满 StopDelay）
	 *  用途：检测 静止 -> 移动 的切换，只触发一次 */
	bool bStationarySettled = false;

	//静止时间计时值初始化
	float StationaryTime = 0.f;
	float MovingTime = 0.f;

	//静止超过2.f（两秒），MidPoint慢慢移动到角色位置
	UPROPERTY(EditDefaultsOnly, Category = "Route")
	float StopDelay = 2.f;

	//连续移动超过5.f,返回到中点
	UPROPERTY(EditDefaultsOnly, Category = "Route")
	float ResumeDelay = 5.f;

	UPROPERTY(EditDefaultsOnly, Category = "Route")
	float MaxDistance = 2000.f;

	UPROPERTY(EditDefaultsOnly, Category = "Route")
	float MoveThreshold = 1.f;

	// ================= 阈值角（单位：度）=================
	// 阈值角 = (MidPoint->角色) 与 (MidPoint->土地庙) 两向量的夹角
	/** 大于该角度视为"几乎共线"（MidPoint 夹在角色和土地庙中间）-> 状态 A：每帧直接贴到中点 */
	UPROPERTY(EditDefaultsOnly, Category = "Route|Angle")
	float AngleOpenDeg = 170.f;

	/** 小于该角度视为"同向"（角色和土地庙在 MidPoint 同一侧）-> 状态 A：每帧直接贴到中点 */
	UPROPERTY(EditDefaultsOnly, Category = "Route|Angle")
	float AngleTinyDeg = 10.f;

	/** 小于该角度（且大于 AngleTinyDeg）进入状态 B：MidPoint 匀速滑向中点 */
	UPROPERTY(EditDefaultsOnly, Category = "Route|Angle")
	float AngleGlideDeg = 60.f;

	/** 状态 B 的滑行速度（厘米/秒）：越大越快靠拢目标点
	 *  100 = 每秒滑 1 米（10 米要 10 秒）；想更慢写 50 */
	UPROPERTY(EditDefaultsOnly, Category = "Route|Glide")
	float MidGlideSpeed = 100.f;

	// ================= 沿线粒子（黄色） =================
	/** 粒子间距（厘米）：沿样条每隔这么远放一颗，"空隙"大小由它决定
	 *  75 = 每 75cm 一颗（密度是原来 150 的两倍） */
	UPROPERTY(EditDefaultsOnly, Category = "Route|Particles")
	float FxSpacing = 75.f;

	/** 粒子流动速度（厘米/秒）：从角色流向土地庙 */
	UPROPERTY(EditDefaultsOnly, Category = "Route|Particles")
	float FxSpeed = 300.f;

	/** 粒子半径（厘米） */
	UPROPERTY(EditDefaultsOnly, Category = "Route|Particles")
	float FxSize = 12.f;

	/** 领头粒子已经走过的距离（内部状态，每帧自动累积，不用手填） */
	float FxHeadDist = 0.f;

	/** Niagara 系统资产：把资产放在 /Game/FX/NS_SplineGlow 就会被自动加载；
	 *  找不到时自动退回"黄色调试球"，所以不会因为缺资产而没效果 */
	UPROPERTY(Transient)
	TObjectPtr<UNiagaraSystem> FxSystem;

	/** 每颗粒子对应一个 Niagara 组件（复用的池子，不用每帧创建） */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UNiagaraComponent>> FxComponents;

	// ================= C++ 版 NS 流体解算 =================
	/** 替代 Niagara 的 8 个模拟阶段：CPU 解算 Navier-Stokes，输出一张 RGBA16F 场纹理 */
	UPROPERTY(VisibleAnywhere, Category = "Fluid")
	TObjectPtr<UConveyanceFluidSolver> FluidSolver;

	/** 上一帧每颗粒子的屏幕 UV（0~1），用来算屏幕速度 */
	TArray<FVector2f> LastParticleScreenUV;

	bool bLineVisible = false;//当前显示状态缓存，避免每帧重复设置可见性
	
	void SetLineVisible(bool bVisible);//线和子组件的可见性
};

