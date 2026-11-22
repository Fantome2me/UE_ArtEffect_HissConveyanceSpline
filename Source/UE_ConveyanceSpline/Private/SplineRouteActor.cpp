// Fill out your copyright notice in the Description page of Project Settings.
//UE Cpp中 一个类等于一个.h （声明我有什么）和一个.cpp（逻辑实现） ，

/*SplineRouteActor.h     → 声明：“我要一个能挂样条线的 Actor，它有 BuildTwoPointRoute 这个方法”
SplineRouteActor.cpp   → 实现：“样条线怎么建、怎么画调试球”
UE_...GameMode.h       → 声明：“GameMode 要多一个 StartPlay 函数”
UE_...GameMode.cpp     → 使用方：“什么时候建、起点是谁、终点是谁”*/

#include "SplineRouteActor.h"

// #include "DrawDebugHelpers.h"   // 调试球已关闭；想恢复 Yellow 调试球兜底时取消注释
#include "ConveyanceFluidSolver.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"

// Sets default values
ASplineRouteActor::ASplineRouteActor()
{
 	// Set this actor to call Tick() every frame.  You can turn this off to improve performance if you don't need it.
	PrimaryActorTick.bCanEverTick = true; //每帧tick一次
	bReplicates = false; //关闭网格复制

	Spline = CreateDefaultSubobject<USplineComponent>(TEXT("Spline"));
	RootComponent = Spline;

	Spline->SetMobility(EComponentMobility::Movable);

	Spline->SetClosedLoop(false, false);

	Spline->SetDrawDebug(true);

	// C++ 版 NS 流体解算（替代 Niagara 的 8 个模拟阶段）
	FluidSolver = CreateDefaultSubobject<UConveyanceFluidSolver>(TEXT("FluidSolver"));
}

// Called when the game starts or when spawned
void ASplineRouteActor::BeginPlay()
{
	Super::BeginPlay();
	
	SetLineVisible(false);//初始先隐藏，由第一帧 Tick 决定是否显示

	// 加载 Niagara 系统资产；找不到就自动退回"黄色调试球"，不会没效果
	FxSystem = LoadObject<UNiagaraSystem>(nullptr, TEXT("/Game/FX/NS_SplineGlow.NS_SplineGlow"));
}

void ASplineRouteActor::BuildThreePointRoute(const FVector& WorldStart, const FVector& WorldEnd)
{
	if (!Spline)
	{
		return;
	}

	    //计算MidPoint
	    MidPoint = (WorldStart + WorldEnd) * 0.5f;

		//MidPoint作为贝赛尔控制点，所以不在曲线上，就删去
		Spline->SetSplinePoints({ WorldStart,WorldEnd }, ESplineCoordinateSpace::World, false);

		//三点都用Curve看看能不能平滑(注意共线时的bug
		//Curve的平滑是利用切线算出来的，本身和对应的贝塞尔描述的是同条曲线
		//所以写Linear直接SetSplinePointType,写Curve一定要先写SetTangentsAtSplinePoint,和起点终点连线切线声明(用于计算)
		
        // 贝塞尔（控制点 = MidPoint）→ Hermite 切线：T = 3 * (C - P)
	    
		const FVector Tangent0 = 3.f*(MidPoint - WorldStart); 
		const FVector Tangent1 = 3.f * (WorldEnd - MidPoint);

		Spline->SetTangentsAtSplinePoint(0, Tangent0, Tangent0, ESplineCoordinateSpace::World, false);
		Spline->SetTangentsAtSplinePoint(1, Tangent1, Tangent1, ESplineCoordinateSpace::World, false);

	    //单条双点的写法
		/*Spline->SetSplinePointType(0, ESplinePointType::Curve, false);
		Spline->SetSplinePointType(1, ESplinePointType::Curve, false);
		Spline->SetSplinePointType(2, ESplinePointType::Curve, false);*/

		//多点曲线
		Spline->SetSplinePointType(0, ESplinePointType::CurveCustomTangent, false);
		Spline->SetSplinePointType(1, ESplinePointType::CurveCustomTangent, false);

		Spline->UpdateSpline();

        //初始化状态机
		LastPlayerLoc = WorldStart;
		StationaryPos = WorldStart;
		bStationarySettled = false;
		StationaryTime = 0.f;
		MovingTime = 0.f;
}

void ASplineRouteActor:: SetLineVisible(bool bVisible)
{
	if (bLineVisible == bVisible)// 状态没变就直接返回，避免每帧重建渲染状态
	{
		return;
	}
	bLineVisible = bVisible;

	if (Spline)
	{
		Spline->SetDrawDebug(bVisible);           //UE自带的调试线可见性
		Spline->SetVisibility(bVisible, true);     //后面的Niagara等可见性
	}
}

// Called every frame
// 每帧：判定显示范围 / 移动静止 / 阈值角状态机 -> 更新控制点 MidPoint -> 写回样条
void ASplineRouteActor::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (!Spline || Spline->GetNumberOfSplinePoints() < 2)
	{
		return;
	}

	const APawn* PlayerPawn = UGameplayStatics::GetPlayerPawn(this, 0);

	//判空
	if (!PlayerPawn)
	{
		SetLineVisible(false);
		return;
	}

	const FVector PlayerLoc = PlayerPawn->GetActorLocation();//起点 玩家
	const FVector EndLoc = Spline->GetLocationAtSplinePoint(1, ESplineCoordinateSpace::World);//终点 土地庙
	const FVector Center = (PlayerLoc + EndLoc) * 0.5f;//角色与土地庙的中点

	//可见范围
	SetLineVisible(FVector::Dist(PlayerLoc, EndLoc) <= MaxDistance);

	//位置变化判断 并 更新上次的停留点
	const bool bMoving = FVector::DistSquared(PlayerLoc, LastPlayerLoc) > FMath::Square(MoveThreshold);
	LastPlayerLoc = PlayerLoc;

	if (bMoving)
	{
		MovingTime += DeltaTime;
		StationaryTime = 0.f;
	}
	else
	{
		StationaryTime += DeltaTime;
		MovingTime = 0.f;
	}

	// ---------- 记录"角色静止时所在的位置"（只在静止帧更新） ----------
	if (!bMoving)
	{
		StationaryPos = PlayerLoc;
	}

	// 静止满 StopDelay(3 秒) -> 记下"已经进入静止停留"，用于后面检测 静止->移动 的切换
	if (StationaryTime >= StopDelay)
	{
		bStationarySettled = true;
	}

	// ================= 阈值角 =================
	// V0 = MidPoint -> 角色；V1 = MidPoint -> 土地庙
	// 两向量夹角的含义：
	//   接近 180°：MidPoint 夹在角色和土地庙中间（三点共线）
	//   接近 0°  ：角色和土地庙在 MidPoint 的同一侧（MidPoint 跑到了外面）
	const FVector V0 = (PlayerLoc - MidPoint).GetSafeNormal();
	const FVector V1 = (EndLoc - MidPoint).GetSafeNormal();
	const float Dot = FMath::Clamp(FVector::DotProduct(V0, V1), -1.f, 1.f);
	const float AngleDeg = FMath::RadiansToDegrees(FMath::Acos(Dot));

	// ================= 状态机 =================
	// 优先级 0：从"静止停留"切换到"移动"的第一帧 -> MidPoint 瞬间回到角色静止时所在的位置
	if (bMoving && bStationarySettled)
	{
		MidPoint = StationaryPos;
		bStationarySettled = false;
	}
	else if (AngleDeg > AngleOpenDeg || AngleDeg < AngleTinyDeg)
	{
		// 状态A：几乎共线(>170) 或 同向(<10) -> 瞬间贴到中点（这里按设计就是瞬间恢复）
		MidPoint = Center;
	}
	else if (AngleDeg < AngleGlideDeg)
	{
		// 状态B：10 < 角度 < 60
		if (bMoving)
		{
			// B-1：角色移动中 -> 每帧朝中点匀速挪一小段
			MidPoint = FMath::VInterpConstantTo(MidPoint, Center, DeltaTime, MidGlideSpeed);
		}
		else if (StationaryTime >= ResumeDelay)
		{
			// B-2：角色静止且满 5 秒 -> 用同样速度匀速挪向中点（不是瞬间）
			MidPoint = FMath::VInterpConstantTo(MidPoint, Center, DeltaTime, MidGlideSpeed);
		}
		// B-0：静止但未满 5 秒 -> 保持不变
	}
	else
	{
		// 状态C：其余角度(60~170) -> 沿用原逻辑
		if (StationaryTime >= StopDelay)
		{
			// 静止满 StopDelay(3 秒) -> 朝角色【慢慢】靠拢（不是瞬间变位）
			MidPoint = FMath::VInterpConstantTo(MidPoint, PlayerLoc, DeltaTime, MidGlideSpeed);
		}
		else if (MovingTime >= ResumeDelay)
		{
			// 连续移动满 5 秒 -> 回中点（同样改成匀速滑行，避免每 5 秒被瞬间拉平一次）
			MidPoint = FMath::VInterpConstantTo(MidPoint, Center, DeltaTime, MidGlideSpeed);
		}
		// 其余情况保持不变
	}

	// ================= 写回样条 =================
	if (bLineVisible)
	{
		// 流体注入需要的相机信息（每帧取一次）
		APlayerController* PlayerController = Cast<APlayerController>(PlayerPawn->GetController());
		int32 ViewportW = 0;
		int32 ViewportH = 0;
		if (PlayerController)
		{
			PlayerController->GetViewportSize(ViewportW, ViewportH);
		}

		//Spline上的动点
		Spline->SetLocationAtSplinePoint(0, PlayerLoc, ESplineCoordinateSpace::World, false);
		
		//因为MidPoint每帧更新，所以切线跟着重算（控制点在 MidPoint：T = 3 * (控制点 - 端点)）
		const FVector Tangent0 = 3.f * (MidPoint - PlayerLoc);
		const FVector Tangent1 = 3.f * (EndLoc - MidPoint);

		Spline->SetTangentsAtSplinePoint(0, Tangent0, Tangent0, ESplineCoordinateSpace::World, false);
		Spline->SetTangentsAtSplinePoint(1, Tangent1, Tangent1, ESplineCoordinateSpace::World, false);

		Spline->UpdateSpline();

		// ================= 沿线粒子：一粒一粒从角色流向土地庙 =================
		const float TotalLen = Spline->GetSplineLength();
		if (TotalLen > KINDA_SMALL_NUMBER)
		{
			// 头部匀速前进；超过末端就绕回起点，形成"管道流动"的效果
			FxHeadDist = FMath::Fmod(FxHeadDist + FxSpeed * DeltaTime, TotalLen);

			const float Step = FMath::Max(FxSpacing, 1.f);            // 粒子间距（空隙）
			const int32 Count = FMath::CeilToInt(TotalLen / Step);     // 需要几颗粒子铺满整条线

			// ---- Niagara 组件池：不够就补，够了就复用（不要每帧创建） ----
			if (FxSystem && FxComponents.Num() < Count)
			{
				for (int32 i = FxComponents.Num(); i < Count; ++i)
				{
					UNiagaraComponent* Comp = NewObject<UNiagaraComponent>(this);
					Comp->SetAsset(FxSystem);
					Comp->SetupAttachment(RootComponent);
					Comp->RegisterComponent();
					Comp->Activate();
					Comp->SetVariableLinearColor(FName("Color"), FLinearColor::Yellow);   // 系统里若有 Color 参数就染成黄色
					FxComponents.Add(Comp);
				}
			}

			for (int32 Index = 0; Index < Count; ++Index)
			{
				// 第 Index 颗粒子在头部后面 Index*Step 的位置；负数就绕到线尾
				float Dist = FMath::Fmod(FxHeadDist - Index * Step, TotalLen);
				if (Dist < 0.f)
				{
					Dist += TotalLen;
				}

				const FVector ParticlePos = Spline->GetLocationAtDistanceAlongSpline(Dist, ESplineCoordinateSpace::World);

				if (FxSystem && FxComponents.IsValidIndex(Index))
				{
					// Niagara 路径：把这一颗粒子的组件挪到样条上
					UNiagaraComponent* Comp = FxComponents[Index];
					if (!Comp->IsVisible())
					{
						Comp->SetVisibility(true, true);
					}
					Comp->SetWorldLocation(ParticlePos);
				}
				// 调试球已按需求关闭：没有 Niagara 资产时这里什么都不画
				// 想恢复"黄色调试球兜底"，把下面这行取消注释即可：
				// DrawDebugSphere(GetWorld(), ParticlePos, FxSize, 8, FColor::Yellow, false, -1.f, 0, 1.f);

				// ---------- 流体注入：把粒子的"屏幕位置 + 屏幕速度"喂给 NS 解算 ----------
				if (FluidSolver && PlayerController && ViewportW > 0 && ViewportH > 0)
				{
					FVector2D ScreenPos = FVector2D::ZeroVector;
					if (UGameplayStatics::ProjectWorldToScreen(PlayerController, ParticlePos, ScreenPos, false))
					{
						const FVector2f ScreenUV(
							static_cast<float>(ScreenPos.X / static_cast<double>(ViewportW)),
							static_cast<float>(ScreenPos.Y / static_cast<double>(ViewportH)));

						if (LastParticleScreenUV.IsValidIndex(Index))
						{
							const FVector2f DeltaUV = ScreenUV - LastParticleScreenUV[Index];
							const FVector2f ScreenVelUV = DeltaUV / FMath::Max(DeltaTime, KINDA_SMALL_NUMBER);
							FluidSolver->PushSplat(ScreenUV, ScreenVelUV);
						}

						if (LastParticleScreenUV.Num() <= Index)
						{
							LastParticleScreenUV.SetNum(Index + 1);
						}
						LastParticleScreenUV[Index] = ScreenUV;
					}
				}
			}

			// 多余的组件隐藏起来
			for (int32 i = Count; i < FxComponents.Num(); ++i)
			{
				FxComponents[i]->SetVisibility(false, true);
			}

			// ---------- 跑一帧 NS 解算（文章 8 个阶段的 C++ 版）----------
			if (FluidSolver)
			{
				FluidSolver->Simulate(DeltaTime);
			}
		}
	}
}
