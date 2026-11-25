// Fill out your copyright notice in the Description page of Project Settings.
//UE Cpp中 一个类等于一个.h （声明我有什么）和一个.cpp（逻辑实现） ，

/*SplineRouteActor.h     → 声明：“我要一个能挂样条线的 Actor，它有 BuildTwoPointRoute 这个方法”
SplineRouteActor.cpp   → 实现：“样条线怎么建、怎么画调试球”
UE_...GameMode.h       → 声明：“GameMode 要多一个 StartPlay 函数”
UE_...GameMode.cpp     → 使用方：“什么时候建、起点是谁、终点是谁”*/

#include "SplineRouteActor.h"

// #include "DrawDebugHelpers.h"   // 调试球已关闭；想恢复 Yellow 调试球兜底时取消注释
#include "ConveyanceFluidSolver.h"
#include "Components/SplineMeshComponent.h"
#include "ProceduralMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
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
		// 起点在角色上方 0.3m（StartHeightOffset），视觉上从头顶/胸口引出
		const FVector SplineStartPos = PlayerLoc + FVector(0.f, 0.f, StartHeightOffset);
		Spline->SetLocationAtSplinePoint(0, SplineStartPos, ESplineCoordinateSpace::World, false);
		
		// 因为MidPoint每帧更新，所以切线跟着重算（控制点在 MidPoint：T = 3 * (控制点 - 端点)）
		const FVector Tangent0 = 3.f * (MidPoint - SplineStartPos);
		const FVector Tangent1 = 3.f * (EndLoc - MidPoint);

		Spline->SetTangentsAtSplinePoint(0, Tangent0, Tangent0, ESplineCoordinateSpace::World, false);
		Spline->SetTangentsAtSplinePoint(1, Tangent1, Tangent1, ESplineCoordinateSpace::World, false);

		Spline->UpdateSpline();

		// ================= 沿样条的深血红丝线 =================
		RebuildStrands(Spline->GetSplineLength());

		// ================= 屏幕血红覆盖层的整体透明权重（和血丝同一套距离逻辑）=================
		if (FluidSolver)
		{
			// 对应血丝里的 StrandOpacity + 端点渐隐，用的是同一批参数：
			//   clamp((MaxDistance - DToEnd) / (MaxDistance - FadeStartDistance))  -> 离开土地庙 20m 外 0，10m 内 1
			//   clamp(DToEnd / EdgeFadeDistance)                                   -> 贴到土地庙的最后 0.5m 内再淡回 0
			const FVector GuideStart = Spline->GetLocationAtSplinePoint(0, ESplineCoordinateSpace::World);
			const FVector GuideEnd = Spline->GetLocationAtSplinePoint(1, ESplineCoordinateSpace::World);
			const float GuideLen = FVector::Dist(GuideStart, GuideEnd);
			const float FadeSpan = FMath::Max(MaxDistance - FadeStartDistance, 1.f);

			float GuideOpacity = FMath::Clamp((MaxDistance - GuideLen) / FadeSpan, 0.f, 1.f);
			GuideOpacity *= FMath::Clamp(GuideLen / FMath::Max(EdgeFadeDistance, 1.f), 0.f, 1.f);
			FluidSolver->SetBloodTintWeight(GuideOpacity);

			// ---- 两端各挖一个软洞：把角色端点 / 土地庙端点的屏幕 UV + UV 半径喂给材质 ----
			// 半径用血丝端点渐隐的同一个 0.5m（EdgeFadeDistance）投影到屏幕
			if (PlayerController && ViewportW > 0 && ViewportH > 0)
			{
				FVector CamLoc;
				FRotator CamRot;
				PlayerController->GetPlayerViewPoint(CamLoc, CamRot);
				const FVector CamRight = FRotationMatrix(CamRot).GetScaledAxis(EAxis::Y);
				const FVector CamUp = FRotationMatrix(CamRot).GetScaledAxis(EAxis::Z);
				const float HoleWorldR = FMath::Max(EdgeFadeDistance, 1.f);

				const double VW = static_cast<double>(ViewportW);
				const double VH = static_cast<double>(ViewportH);

				auto ProjectUV = [&](const FVector& World, FVector2D& OutUV) -> bool
				{
					FVector2D S;
					if (!UGameplayStatics::ProjectWorldToScreen(PlayerController, World, S, false))
					{
						return false;
					}
					OutUV = FVector2D(S.X / VW, S.Y / VH);
					return true;
				};

				auto RadiusUV = [&](const FVector& World, const FVector2D& Center) -> FVector2D
				{
					FVector2D UV;
					FVector2D R(1e-4, 1e-4);
					if (ProjectUV(World + CamRight * HoleWorldR, UV))
					{
						R.X = FMath::Max(FMath::Abs(UV.X - Center.X), 1e-4);
					}
					if (ProjectUV(World + CamUp * HoleWorldR, UV))
					{
						R.Y = FMath::Max(FMath::Abs(UV.Y - Center.Y), 1e-4);
					}
					return R;
				};

				FVector2D UVA(0.5, 0.5);
				FVector2D UVB(0.5, 0.5);
				FVector2D RA(1e-4, 1e-4);
				FVector2D RB(1e-4, 1e-4);

				if (ProjectUV(GuideStart, UVA))
				{
					RA = RadiusUV(GuideStart, UVA);
				}
				if (ProjectUV(GuideEnd, UVB))
				{
					RB = RadiusUV(GuideEnd, UVB);
				}
				FluidSolver->SetBloodTintEndpoints(UVA, RA, UVB, RB);
			}
		}

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
					Comp->SetVisibility(bRenderParticles, true);                          // 默认隐藏粒子本身，只做流体注入
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

				FVector ParticlePos = Spline->GetLocationAtDistanceAlongSpline(Dist, ESplineCoordinateSpace::World);

				// ---------- 垂直于样条的圆盘内做不规则运动（每颗粒子不同）----------
				if (FxJitterRadius > 0.f)
				{
					const FVector Tangent = Spline->GetDirectionAtDistanceAlongSpline(Dist, ESplineCoordinateSpace::World).GetSafeNormal();
					FVector Right = FVector::CrossProduct(Tangent, FVector::UpVector).GetSafeNormal();
					if (Right.IsNearlyZero())
					{
						Right = FVector::RightVector;
					}
					const FVector Up = FVector::CrossProduct(Right, Tangent).GetSafeNormal();

					const float Time = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;
					const float Seed = static_cast<float>(Index) * 12.9898f;

					// 每颗粒子固定的随机角速度 / 半径呼吸频率（稳定，不会每帧乱跳）
					const float RandA = FMath::Frac(FMath::Sin(Seed) * 43758.5453f);
					const float RandB = FMath::Frac(FMath::Sin(Seed * 1.7f + 2.3f) * 43758.5453f);

					const float AngleRate = (0.8f + 2.2f * RandA) * FxJitterSpeedScale;
					const float RadiusRate = (1.2f + 2.6f * RandB) * FxJitterSpeedScale;

					const float Angle = Seed + Time * AngleRate;
					// 半径在 0.15~1.0 之间呼吸，整体不超过 FxJitterRadius
					const float Radius01 = FMath::Clamp(0.575f + 0.425f * FMath::Sin(Time * RadiusRate + Seed * 3.1f), 0.f, 1.f);
					const float Radius = Radius01 * FxJitterRadius;

					ParticlePos += (Right * FMath::Cos(Angle) + Up * FMath::Sin(Angle)) * Radius;
				}

				if (FxSystem && FxComponents.IsValidIndex(Index))
				{
					// Niagara 路径：把这一颗粒子的组件挪到样条上
					UNiagaraComponent* Comp = FxComponents[Index];
					if (Comp->IsVisible() != bRenderParticles)
					{
						Comp->SetVisibility(bRenderParticles, true);
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

							// 每颗粒子一个"稳定的随机力道"：同一个 Index 每帧都一样，不会闪烁
							const float Rand01 = FMath::Frac(
								FMath::Sin(static_cast<float>(Index) * 12.9898f + 4.1414f) * 43758.5453f);
							const float StrengthScale = FMath::Lerp(1.f - FxStrengthJitter, 1.f + FxStrengthJitter, Rand01);

							FluidSolver->PushSplat(ScreenUV, ScreenVelUV, StrengthScale);
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
	else
	{
		// 线不可见时把丝线也藏起来
		for (TObjectPtr<UProceduralMeshComponent>& Comp : StrandComps)
		{
			if (Comp)
			{
				Comp->SetVisibility(false, true);
			}
		}
	}
}

// ============================================================================
// 沿样条生成"一缕一缕"的深血红丝线（SplineMesh + 无光照材质）
// ============================================================================
void ASplineRouteActor::RebuildStrands(float TotalLen)
{
	const bool bWantVisible = bShowStrands && StrandCount > 0 && TotalLen > KINDA_SMALL_NUMBER;

	if (!bWantVisible)
	{
		for (TObjectPtr<UProceduralMeshComponent>& Comp : StrandComps)
		{
			if (Comp)
			{
				Comp->SetVisibility(false, true);
			}
		}
		return;
	}

	if (!StrandMaterial)
	{
		StrandMaterial = LoadObject<UMaterialInterface>(nullptr,
			TEXT("/Game/ConveyanceSpline/Effect_Spline/M_FluidStrand.M_FluidStrand"));
	}

	const int32 NeedMeshes = StrandCount;
	while (StrandComps.Num() < NeedMeshes)
	{
		const int32 S = StrandComps.Num();

		UMaterialInstanceDynamic* MID = StrandMaterial ? UMaterialInstanceDynamic::Create(StrandMaterial, this) : nullptr;
		if (MID)
		{
			const float Rnd = FMath::Frac(FMath::Sin((S + 1) * 7.31f) * 43758.5453f);
			MID->SetVectorParameterValue(TEXT("BloodColor"),
				FLinearColor(0.070f + 0.045f * Rnd, 0.003f + 0.003f * Rnd, 0.006f, 1.f));
			MID->SetScalarParameterValue(TEXT("Roughness"), 0.42f + 0.18f * Rnd);
			MID->SetScalarParameterValue(TEXT("Specular"), 0.18f + 0.08f * Rnd);
		}

		UProceduralMeshComponent* Comp = NewObject<UProceduralMeshComponent>(this);
		Comp->SetMobility(EComponentMobility::Movable);
		Comp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Comp->SetCastShadow(false);
		Comp->bUseComplexAsSimpleCollision = false;
		Comp->SetTranslucentSortPriority(10);
		if (MID)
		{
			Comp->SetMaterial(0, MID);
		}
		else if (StrandMaterial)
		{
			Comp->SetMaterial(0, StrandMaterial);
		}
		Comp->SetupAttachment(RootComponent);
		Comp->RegisterComponent();
		if (MID)
		{
			Comp->SetMaterial(0, MID);
		}

		StrandComps.Add(Comp);
		StrandMIDs.Add(MID);

		UE_LOG(LogTemp, Log, TEXT("[Strand] 新建飘带 %d: mat=%s mid=%s"),
			S, *GetNameSafe(Comp->GetMaterial(0)), MID ? TEXT("valid") : TEXT("null"));
	}
	for (int32 i = NeedMeshes; i < StrandComps.Num(); ++i)
	{
		if (StrandComps[i])
		{
			StrandComps[i]->SetVisibility(false, true);
		}
	}

	// ---- 端点与距离 ----
	const FVector StrandStart = Spline->GetLocationAtSplinePoint(0, ESplineCoordinateSpace::World);
	const FVector StrandEnd = Spline->GetLocationAtSplinePoint(1, ESplineCoordinateSpace::World);
	const float DToEnd = FVector::Dist(StrandStart, StrandEnd);

	const float FadeSpan = FMath::Max(MaxDistance - FadeStartDistance, 1.f);
	const float StrandOpacity = FMath::Clamp((MaxDistance - DToEnd) / FadeSpan, 0.f, 1.f);

	if (StrandOpacity <= KINDA_SMALL_NUMBER)
	{
		for (TObjectPtr<UProceduralMeshComponent>& Comp : StrandComps)
		{
			if (Comp)
			{
				Comp->SetVisibility(false, true);
			}
		}
		return;
	}

	const float Time = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;
	const int32 Samples = 128;
	const int32 Sides = FMath::Clamp(StrandSides, 5, 16);

	TArray<FVector> Verts;
	TArray<int32> Tris;
	TArray<FVector> Normals;
	TArray<FVector2D> UVs;
	TArray<FLinearColor> Colors;
	TArray<FProcMeshTangent> Tangents;

	for (int32 S = 0; S < StrandCount; ++S)
	{
		UProceduralMeshComponent* Comp = StrandComps[S];
		if (!Comp)
		{
			continue;
		}

		const float Phase = static_cast<float>(S) * 2.399f;

		// 每条丝线粗细不同，且随时间起伏（0.19 ~ 2.3 倍）
		const float TSeed = FMath::Frac(FMath::Sin((S + 1) * 3.17f) * 43758.5453f);
		const float ThickBase = 0.55f + 0.90f * TSeed;
		const float ThickOsc = 0.35f + 1.25f * (0.5f + 0.5f * FMath::Sin(Time * (0.40f + 1.30f * TSeed) + TSeed * 6.2831f));
		const float ThickFactor = ThickBase * ThickOsc;
		const float Radius = FMath::Max(StrandThickness * ThickFactor, 0.15f) * 0.5f;

		Verts.Reset(); Tris.Reset(); Normals.Reset(); UVs.Reset(); Colors.Reset(); Tangents.Reset();
		Verts.Reserve(Samples * Sides);
		Normals.Reserve(Samples * Sides);
		UVs.Reserve(Samples * Sides);
		Colors.Reserve(Samples * Sides);
		Tangents.Reserve(Samples * Sides);

		for (int32 K = 0; K < Samples; ++K)
		{
			const float Alpha = static_cast<float>(K) / static_cast<float>(Samples - 1);
			const float Dist = Alpha * TotalLen;

			FVector Pos = Spline->GetLocationAtDistanceAlongSpline(Dist, ESplineCoordinateSpace::World);
			const FVector Dir = Spline->GetDirectionAtDistanceAlongSpline(Dist, ESplineCoordinateSpace::World).GetSafeNormal();

			FVector Side = FVector::CrossProduct(Dir, FVector::UpVector).GetSafeNormal();
			if (Side.IsNearlyZero())
			{
				Side = FVector::RightVector;
			}
			const FVector Up = FVector::CrossProduct(Side, Dir).GetSafeNormal();

			// 紧密度：远端 0.25（偏移 4x）→ 近端 2.0（偏移 0.5x）
			const float ToEnd = FVector::Dist(Pos, StrandEnd);
			const float Tight01 = 1.f - FMath::Clamp(ToEnd / FMath::Max(MaxDistance, 1.f), 0.f, 1.f);
			const float Tight = FMath::Lerp(StrandTightFar, StrandTightNear, Tight01);
			const float OffsetScale = 1.f / FMath::Max(Tight, 0.05f);

			const float WobbleA = FMath::Sin(Alpha * 10.f + Phase + Time * 0.8f) * StrandSway;
			const float WobbleB = FMath::Cos(Alpha * 7.f + Phase * 1.7f + Time * 0.55f) * (StrandSway * 0.7f);
			Pos += (Side * WobbleA + Up * WobbleB) * OffsetScale;

			// 端点 0.3m 渐隐（逐顶点 alpha → 材质 Masked 抖动）
			const float EdgeFade = FMath::Clamp(Dist / FMath::Max(EdgeFadeDistance, 1.f), 0.f, 1.f)
				* FMath::Clamp((TotalLen - Dist) / FMath::Max(EdgeFadeDistance, 1.f), 0.f, 1.f);
			const float VertexOpacity = StrandOpacity * EdgeFade;

			const float V = Dist / FMath::Max(StrandUVTile, 1.f);

			for (int32 J = 0; J < Sides; ++J)
			{
				const float Ang = 2.f * PI * static_cast<float>(J) / static_cast<float>(Sides);
				const FVector N = (Side * FMath::Cos(Ang) + Up * FMath::Sin(Ang)).GetSafeNormal();

				Verts.Add(Pos + N * Radius);
				Normals.Add(N);
				UVs.Add(FVector2D(static_cast<float>(J) / static_cast<float>(Sides), V));
				Colors.Add(FLinearColor(1.f, 1.f, 1.f, VertexOpacity));
				Tangents.Add(FProcMeshTangent(Dir, false));
			}
		}

		for (int32 K = 0; K < Samples - 1; ++K)
		{
			for (int32 J = 0; J < Sides; ++J)
			{
				const int32 J2 = (J + 1) % Sides;
				const int32 A0 = K * Sides + J;
				const int32 A1 = K * Sides + J2;
				const int32 B0 = (K + 1) * Sides + J;
				const int32 B1 = (K + 1) * Sides + J2;

				Tris.Add(A0); Tris.Add(B0); Tris.Add(A1);
				Tris.Add(A1); Tris.Add(B0); Tris.Add(B1);
			}
		}

		Comp->SetVisibility(true, true);

		while (StrandVertCounts.Num() <= S)
		{
			StrandVertCounts.Add(0);
		}

		const bool bNeedRebuild = (Comp->GetNumSections() == 0) || (StrandVertCounts[S] != Verts.Num());
		if (bNeedRebuild)
		{
			Comp->CreateMeshSection_LinearColor(0, Verts, Tris, Normals, UVs, Colors, Tangents, false);
			StrandVertCounts[S] = Verts.Num();
		}
		else
		{
			Comp->UpdateMeshSection_LinearColor(0, Verts, Normals, UVs, Colors, Tangents);
		}
	}
}