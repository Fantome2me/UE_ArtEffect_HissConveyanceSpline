// Fill out your copyright notice in the Description page of Project Settings.

#include "ConveyanceFluidSolver.h"

#include "Async/ParallelFor.h"
#include "DynamicRHI.h"                 // FUpdateTextureRegion2D
#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/PostProcessComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/Scene.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "HAL/IConsoleManager.h"
#include "ImageUtils.h"
#include "Math/Float16.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "PixelFormat.h"
#include "RenderTargetPool.h"
#include "RHIResources.h"
#include "Styling/SlateBrush.h"
#include "TextureResource.h"
#include "Widgets/Images/SImage.h"

DEFINE_LOG_CATEGORY_STATIC(LogFluidSolver, Log, All);

// 控制台命令用的当前实例（同一个世界一般只有一个）
static TWeakObjectPtr<UConveyanceFluidSolver> GFluidSolverForConsole;

static void FluidDumpCommand(const TArray<FString>& Args, UWorld* World)
{
	if (UConveyanceFluidSolver* Solver = GFluidSolverForConsole.Get())
	{
		Solver->DumpFieldToPNG();
	}
}

static void FluidViewCommand(const TArray<FString>& Args, UWorld* World)
{
	if (UConveyanceFluidSolver* Solver = GFluidSolverForConsole.Get())
	{
		const int32 Index = Args.Num() > 0 ? FCString::Atoi(*Args[0]) : 0;
		Solver->SetDebugViewByIndex(Index);
	}
}

static void FluidPPTestCommand(const TArray<FString>& Args, UWorld* World)
{
	if (UConveyanceFluidSolver* Solver = GFluidSolverForConsole.Get())
	{
		Solver->SetPPTest(Args.Num() > 0 ? FCString::Atof(*Args[0]) : 1.f);
	}
}

static FAutoConsoleCommandWithWorldAndArgs GFluidPPTestCmd(
	TEXT("fluid.pptest"),
	TEXT("fluid.pptest <0|1> : 1 = 挂纯洋红后处理材质，验证后处理链路"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&FluidPPTestCommand));

static FAutoConsoleCommandWithWorldAndArgs GFluidDumpCmd(
	TEXT("fluid.dump"),
	TEXT("把当前流体视图存成 PNG（Saved/FluidDebug/）"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&FluidDumpCommand));

static FAutoConsoleCommandWithWorldAndArgs GFluidViewCmd(
	TEXT("fluid.view"),
	TEXT("fluid.view <0..4> : 0=位移 1=速度 2=UV 3=压力 4=散度"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&FluidViewCommand));

UConveyanceFluidSolver::UConveyanceFluidSolver()
{
	// 由外部（ASplineRouteActor::Tick）显式驱动，组件本身不 Tick
	PrimaryComponentTick.bCanEverTick = false;
}

void UConveyanceFluidSolver::BeginPlay()
{
	Super::BeginPlay();
	GFluidSolverForConsole = this;
	InitializeBuffers();

	// 没手动指定就用工程里那张 RT_Fluid（双击它就能实时看到流场）
	if (!TargetRT)
	{
		TargetRT = LoadObject<UTextureRenderTarget2D>(nullptr,
			TEXT("/Game/ConveyanceSpline/Effect_Spline/RT_Fluid.RT_Fluid"));
	}

	SetupRefractionMaterial();
	SetupBloodTint();          // 屏幕血红覆盖层：RT 非黑底区域 = 半透明血红

	if (TargetRT && (TargetRT->SizeX != Num || TargetRT->SizeY != Num))
	{
		// 让 RT 尺寸自动跟网格一致，避免"写了但看不到"
		TargetRT->ResizeTarget(Num, Num);
		UE_LOG(LogFluidSolver, Log, TEXT("[Fluid] TargetRT 尺寸已同步为 %dx%d"), Num, Num);
	}
}

void UConveyanceFluidSolver::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (GFluidSolverForConsole.Get() == this)
	{
		GFluidSolverForConsole.Reset();
	}

	if (bScreenWidgetAdded && ScreenWidget.IsValid() && GEngine && GEngine->GameViewport)
	{
		GEngine->GameViewport->RemoveViewportWidgetContent(ScreenWidget.ToSharedRef());
	}
	ScreenWidget.Reset();
	bScreenWidgetAdded = false;

	Super::EndPlay(EndPlayReason);
}

void UConveyanceFluidSolver::SetDebugViewByIndex(int32 Index)
{
	DebugView = static_cast<EFluidDebugView>(FMath::Clamp(Index, 0, 4));
	UE_LOG(LogFluidSolver, Log, TEXT("[Fluid] view = %d"), static_cast<int32>(DebugView));
}

void UConveyanceFluidSolver::ResetField()
{
	InitializeBuffers();
}

// ============================================================================
// 初始化：对应文章「1. DrawUVColor」——每个格子记录自己的 UV，速度/压力清零
// ============================================================================
void UConveyanceFluidSolver::InitializeBuffers()
{
	Num = FMath::Clamp(GridSize, 16, 512);
	const int32 Total = Num * Num;

	UVField.SetNumUninitialized(Total);
	UVTemp.SetNumUninitialized(Total);
	VelField.Init(FVector2f::ZeroVector, Total);
	VelTemp.Init(FVector2f::ZeroVector, Total);
	Pressure.Init(0.f, Total);
	PressureTemp.Init(0.f, Total);
	Divergence.Init(0.f, Total);
	Trace.Init(0.f, Total);
	PixelHalfs.SetNumZeroed(Total * 4);

	const float InvNum = 1.f / static_cast<float>(Num);
	for (int32 Y = 0; Y < Num; ++Y)
	{
		for (int32 X = 0; X < Num; ++X)
		{
			const int32 I = IndexOf(X, Y);
			UVField[I] = FVector2f((X + 0.5f) * InvNum, (Y + 0.5f) * InvNum);
		}
	}

	// 输出纹理（RGBA16F，供后处理材质使用）
	if (!FieldTexture || FieldTexture->GetSizeX() != Num || FieldTexture->GetSizeY() != Num)
	{
		FieldTexture = UTexture2D::CreateTransient(Num, Num, PF_FloatRGBA);
		if (FieldTexture)
		{
			FieldTexture->SRGB = false;
			FieldTexture->Filter = TF_Bilinear;
			FieldTexture->AddressX = TA_Clamp;
			FieldTexture->AddressY = TA_Clamp;
			FieldTexture->UpdateResource();
		}
	}

	// 屏幕可视化纹理（LDR）
	if (!DisplayTexture || DisplayTexture->GetSizeX() != Num || DisplayTexture->GetSizeY() != Num)
	{
		DisplayTexture = UTexture2D::CreateTransient(Num, Num, PF_B8G8R8A8);
		if (DisplayTexture)
		{
			DisplayTexture->SRGB = false;
			DisplayTexture->Filter = TF_Bilinear;
			DisplayTexture->AddressX = TA_Clamp;
			DisplayTexture->AddressY = TA_Clamp;
			DisplayTexture->UpdateResource();
		}
		DisplayPixels.SetNumUninitialized(Num * Num);
		bScreenWidgetAdded = false;   // 纹理换了，重建屏幕控件
	}

	bBuffersReady = true;
}

// ============================================================================
// 把某个通道画成 FColor（屏幕显示 / PNG 导出共用）
// ============================================================================
void UConveyanceFluidSolver::BuildViewPixels(TArray<FColor>& OutPixels, bool bOpaque) const
{
	OutPixels.SetNumUninitialized(Num * Num);

	const float InvNum = 1.f / static_cast<float>(Num);

	// 压力/散度取绝对值最大值做归一化
	float PressureMax = KINDA_SMALL_NUMBER;
	float DivergenceMax = KINDA_SMALL_NUMBER;
	if (DebugView == EFluidDebugView::Pressure || DebugView == EFluidDebugView::Divergence)
	{
		for (int32 I = 0; I < Num * Num; ++I)
		{
			PressureMax = FMath::Max(PressureMax, FMath::Abs(Pressure[I]));
			DivergenceMax = FMath::Max(DivergenceMax, FMath::Abs(Divergence[I]));
		}
	}

	for (int32 Y = 0; Y < Num; ++Y)
	{
		for (int32 X = 0; X < Num; ++X)
		{
			const int32 I = IndexOf(X, Y);
			const FVector2f CellUV((X + 0.5f) * InvNum, (Y + 0.5f) * InvNum);
			const FVector2f Disp = UVField[I] - CellUV;
			const FVector2f Vel = VelField[I];

			FLinearColor Color = FLinearColor::Black;
			float Alpha01 = 0.f;

			switch (DebugView)
			{
			case EFluidDebugView::Displacement:
			{
				// 用"笔迹"做遮罩：只有粒子刚划过的区域才亮，且越早越淡
				// 注意：遮罩必须在"归一化之后"再乘，否则位移再小的地方也会被放大到饱和 → 全屏发亮
				const float Mask = FMath::Clamp(Trace[I] * 2.f, 0.f, 1.f);
				const float Raw = FMath::Clamp((Disp.Size() + Vel.Size() * 0.02f) * DisplayScale, 0.f, 1.f);
				const float A = FMath::Pow(Raw, DisplayPower) * Mask;
				Color = FLinearColor(
					FMath::Lerp(0.02f, 0.85f, A * A),
					FMath::Lerp(0.18f, 0.98f, A),
					FMath::Lerp(0.55f, 1.00f, A),
					1.f);
				Alpha01 = A * DisplayOpacity;
				break;
			}
			case EFluidDebugView::Velocity:
			{
				// 方向 -> 色相；速度大小 -> 亮度（0.5 表示静止）
				const FVector2f N = Vel.GetSafeNormal();
				const float Hue = FMath::Fmod((FMath::Atan2(N.Y, N.X) / (2.f * PI)) + 1.f, 1.f);
				const float Lum = FMath::Clamp(Vel.Size() * 4.f, 0.f, 1.f);
				Color = FLinearColor::MakeFromHSV8(static_cast<uint8>(Hue * 255.f), 255, static_cast<uint8>(Lum * 255.f));
				Alpha01 = FMath::Clamp(Lum * 1.5f, 0.f, 1.f) * DisplayOpacity;
				break;
			}
			case EFluidDebugView::UV:
			{
				// 流体 UV 直接当颜色：能看到纹理被"揉动"
				Color = FLinearColor(UVField[I].X, UVField[I].Y, 0.5f, 1.f);
				Alpha01 = 1.f;
				break;
			}
			case EFluidDebugView::Pressure:
			{
				const float N = Pressure[I] / PressureMax;
				Color = (N >= 0.f)
					? FLinearColor(0.1f, 0.3f + 0.7f * N, 1.f, 1.f)
					: FLinearColor(0.3f + 0.7f * (-N), 0.1f, 0.2f, 1.f);
				Alpha01 = FMath::Abs(N) * DisplayOpacity;
				break;
			}
			case EFluidDebugView::Divergence:
			default:
			{
				const float N = Divergence[I] / DivergenceMax;
				Color = (N >= 0.f)
					? FLinearColor(0.5f + 0.5f * N, 0.5f, 0.1f, 1.f)
					: FLinearColor(0.1f, 0.5f, 0.5f + 0.5f * (-N), 1.f);
				Alpha01 = FMath::Abs(N) * DisplayOpacity;
				break;
			}
			}

			FColor SRGB = Color.ToFColor(true);
			if (bOpaque)
			{
				SRGB.A = 255;
			}
			else
			{
				SRGB.A = static_cast<uint8>(255.f * FMath::Clamp(Alpha01, 0.f, 1.f));
			}
			OutPixels[I] = SRGB;
		}
	}
}

// ============================================================================
// 纯 C++ 的屏幕可视化（不需要材质）：把流场铺满视口
// ============================================================================
void UConveyanceFluidSolver::CreateScreenWidget()
{
	if (bScreenWidgetAdded || !bShowOnScreen || !DisplayTexture)
	{
		return;
	}
	if (!GEngine || !GEngine->GameViewport)
	{
		return;
	}

	ScreenBrush.SetResourceObject(DisplayTexture);
	ScreenBrush.ImageSize = FVector2D(static_cast<float>(Num), static_cast<float>(Num));
	ScreenBrush.DrawAs = ESlateBrushDrawType::Image;

	ScreenWidget = SNew(SImage).Image(&ScreenBrush);
	GEngine->GameViewport->AddViewportWidgetContent(ScreenWidget.ToSharedRef(), 5000);
	bScreenWidgetAdded = true;
}

void UConveyanceFluidSolver::UpdateDisplayTexture()
{
	if (!DisplayTexture || DisplayPixels.Num() != Num * Num)
	{
		return;
	}

	BuildViewPixels(DisplayPixels, false);

	// 同样：Region 用 new，回调里释放
	FUpdateTextureRegion2D* Region = new FUpdateTextureRegion2D(0, 0, 0, 0, static_cast<uint32>(Num), static_cast<uint32>(Num));
	const uint32 SrcPitch = static_cast<uint32>(Num) * sizeof(FColor);

	DisplayTexture->UpdateTextureRegions(
		0, 1, Region, SrcPitch, sizeof(FColor),
		reinterpret_cast<uint8*>(DisplayPixels.GetData()),
		[](uint8* SrcData, const FUpdateTextureRegion2D* InRegion)
		{
			delete InRegion;
		});

	CreateScreenWidget();
}

// ============================================================================
// 把当前视图导出成 PNG（Saved/FluidDebug/fluid_xxx.png）
// ============================================================================
void UConveyanceFluidSolver::DumpFieldToPNG()
{
	if (!bBuffersReady || Num <= 0)
	{
		return;
	}

	TArray<FColor> Opaque;
	BuildViewPixels(Opaque, true);

	TArray64<uint8> PNGData;
	FImageUtils::PNGCompressImageArray(Num, Num, TArrayView64<const FColor>(Opaque.GetData(), Opaque.Num()), PNGData);

	const FString Dir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("FluidDebug"));
	IFileManager::Get().MakeDirectory(*Dir, true);
	const FString FileName = FString::Printf(TEXT("fluid_%s.png"), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));
	const FString FullPath = FPaths::Combine(Dir, FileName);

	if (FFileHelper::SaveArrayToFile(PNGData, *FullPath))
	{
		UE_LOG(LogFluidSolver, Log, TEXT("[Fluid] PNG saved: %s"), *FullPath);
	}
	else
	{
		UE_LOG(LogFluidSolver, Warning, TEXT("[Fluid] PNG save FAILED: %s"), *FullPath);
	}
}

// ============================================================================
// 文章「2. VolumeForce」：把船（粒子）的屏幕速度按高斯核 Lerp 进速度场
// ============================================================================
void UConveyanceFluidSolver::PushSplat(const FVector2f& InUV, const FVector2f& InVelocityUVPerSec, float StrengthScale)
{
	if (!InUV.ContainsNaN() && !InVelocityUVPerSec.ContainsNaN())
	{
		PendingSplatUV.Add(InUV);
		PendingSplatVel.Add(InVelocityUVPerSec);
		PendingSplatScale.Add(FMath::Clamp(StrengthScale, 0.05f, 4.f));
	}
}

void UConveyanceFluidSolver::InjectSplats(float DeltaSeconds)
{
	LastSplatCount = PendingSplatUV.Num();

	if (PendingSplatUV.Num() == 0)
	{
		return;
	}

	const float InvNum = 1.f / static_cast<float>(Num);
	const float RadiusCells = FMath::Max(SplatRadiusUV, 1.f * InvNum) * Num;
	const float InvSigma2 = 1.f / (2.f * FMath::Square(RadiusCells * 0.5f));
	const int32 Reach = FMath::CeilToInt(RadiusCells * 1.5f);

	// 先把这一帧的 splat 整理成只读数组（速度限幅 / 格坐标预先算好），下面好做行并行
	const int32 SplatNum = PendingSplatUV.Num();
	TArray<FVector2f> SplatVel;
	TArray<int32> SplatCX;
	TArray<int32> SplatCY;
	TArray<float> SplatScale;
	SplatVel.SetNumUninitialized(SplatNum);
	SplatCX.SetNumUninitialized(SplatNum);
	SplatCY.SetNumUninitialized(SplatNum);
	SplatScale.SetNumUninitialized(SplatNum);

	for (int32 S = 0; S < SplatNum; ++S)
	{
		FVector2f SrcVel = PendingSplatVel[S];

		// 速度上限，避免数值爆炸
		const float Speed = SrcVel.Size();
		if (Speed > MaxSpeedUV)
		{
			SrcVel = SrcVel / Speed * MaxSpeedUV;
		}

		SplatVel[S] = SrcVel;
		SplatCX[S] = FMath::FloorToInt32(PendingSplatUV[S].X * Num);
		SplatCY[S] = FMath::FloorToInt32(PendingSplatUV[S].Y * Num);
		SplatScale[S] = PendingSplatScale.IsValidIndex(S) ? PendingSplatScale[S] : 1.f;
	}

	// 逐行并行：每行只归一个线程所有，所以同一格上的多颗粒子仍然按索引顺序累加，结果和串行完全一致
	const float Strength = SplatStrength;
	const float RadPush = RadialPush;
	const float MaxSpeed = MaxSpeedUV;
	const float Dt = DeltaSeconds;
	const int32 RowBlock = 8;
	const int32 NumTasks = FMath::DivideAndRoundUp(Num, RowBlock);

	ParallelFor(NumTasks, [&](int32 T)
	{
		const int32 RowY0 = T * RowBlock;
		const int32 RowY1 = FMath::Min(RowY0 + RowBlock, Num);

		for (int32 S = 0; S < SplatNum; ++S)
		{
			const int32 CX = SplatCX[S];
			const int32 CY = SplatCY[S];
			const int32 YA = FMath::Max(RowY0, CY - Reach);
			const int32 YB = FMath::Min(RowY1 - 1, CY + Reach);
			if (YA > YB)
			{
				continue;
			}

			const FVector2f SrcVel = SplatVel[S];
			const float Scale = SplatScale[S];
			const int32 XA = FMath::Max(0, CX - Reach);
			const int32 XB = FMath::Min(Num - 1, CX + Reach);

			for (int32 Y = YA; Y <= YB; ++Y)
			{
				const int32 DY = Y - CY;
				for (int32 X = XA; X <= XB; ++X)
				{
					const int32 DX = X - CX;
					const float Dist2 = static_cast<float>(DX * DX + DY * DY);
					const float W = FMath::Exp(-Dist2 * InvSigma2);
					const int32 I = IndexOf(X, Y);

					// ① 方向拖曳：粒子把水往自己运动方向带
					const float Alpha = FMath::Clamp(W * Strength * Dt * Scale, 0.f, 1.f);
					VelField[I] = FMath::Lerp(VelField[I], SrcVel, Alpha);

					// ② 径向挤压：把水从粒子位置往四周推开（产生"水面被挤开"的环状扰动）
					if (RadPush > 0.f && Dist2 > KINDA_SMALL_NUMBER)
					{
						const FVector2f Dir =
							FVector2f(static_cast<float>(DX), static_cast<float>(DY)).GetSafeNormal();
						VelField[I] += Dir * (W * RadPush * Dt * Scale);

						const float NewSpeed = VelField[I].Size();
						if (NewSpeed > MaxSpeed)
						{
							VelField[I] = VelField[I] / NewSpeed * MaxSpeed;
						}
					}

					// ③ 打上"笔迹"：刚划过的地方 = 1，之后随时间衰减（越早的划痕越淡）
					Trace[I] = FMath::Max(Trace[I], W * Scale);
				}
			}
		}
	});

	PendingSplatUV.Reset();
	PendingSplatVel.Reset();
	PendingSplatScale.Reset();
}

// ============================================================================
// 文章「3. Advection」：半拉格朗日回溯 + 双线性采样（速度、UV 一起搬）
// ============================================================================
FVector2f UConveyanceFluidSolver::SampleVelocityBilinear(const TArray<FVector2f>& Field, const FVector2f& InUV) const
{
	const FVector2f P(InUV.X * Num - 0.5f, InUV.Y * Num - 0.5f);
	const int32 X0 = FMath::FloorToInt32(P.X);
	const int32 Y0 = FMath::FloorToInt32(P.Y);
	const float FX = P.X - static_cast<float>(X0);
	const float FY = P.Y - static_cast<float>(Y0);

	const int32 X0C = ClampIndex(X0);
	const int32 X1C = ClampIndex(X0 + 1);
	const int32 Y0C = ClampIndex(Y0);
	const int32 Y1C = ClampIndex(Y0 + 1);

	const FVector2f V00 = Field[IndexOf(X0C, Y0C)];
	const FVector2f V10 = Field[IndexOf(X1C, Y0C)];
	const FVector2f V01 = Field[IndexOf(X0C, Y1C)];
	const FVector2f V11 = Field[IndexOf(X1C, Y1C)];

	const FVector2f A = FMath::Lerp(V00, V10, FX);
	const FVector2f B = FMath::Lerp(V01, V11, FX);
	return FMath::Lerp(A, B, FY);
}

float UConveyanceFluidSolver::SampleScalarBilinear(const TArray<float>& Field, const FVector2f& InUV) const
{
	const FVector2f P(InUV.X * Num - 0.5f, InUV.Y * Num - 0.5f);
	const int32 X0 = FMath::FloorToInt32(P.X);
	const int32 Y0 = FMath::FloorToInt32(P.Y);
	const float FX = P.X - static_cast<float>(X0);
	const float FY = P.Y - static_cast<float>(Y0);

	const int32 X0C = ClampIndex(X0);
	const int32 X1C = ClampIndex(X0 + 1);
	const int32 Y0C = ClampIndex(Y0);
	const int32 Y1C = ClampIndex(Y0 + 1);

	const float V00 = Field[IndexOf(X0C, Y0C)];
	const float V10 = Field[IndexOf(X1C, Y0C)];
	const float V01 = Field[IndexOf(X0C, Y1C)];
	const float V11 = Field[IndexOf(X1C, Y1C)];

	const float A = FMath::Lerp(V00, V10, FX);
	const float B = FMath::Lerp(V01, V11, FX);
	return FMath::Lerp(A, B, FY);
}

void UConveyanceFluidSolver::Advect(float DeltaSeconds)
{
	const float InvNum = 1.f / static_cast<float>(Num);
	const float UVStrength = FMath::Clamp(AdvectUVStrength, 0.f, 1.f);
	const float MaxSpeed = MaxSpeedUV;
	const float Dissipation = VelocityDissipation;
	const float Dt = DeltaSeconds;

	// 逐行并行（行之间完全独立，读写的是不同的两块缓冲）
	ParallelFor(Num, [&](int32 Y)
	{
		for (int32 X = 0; X < Num; ++X)
		{
			const int32 I = IndexOf(X, Y);
			const FVector2f CellUV = UVField[I];
			const FVector2f Vel = VelField[I];
			const FVector2f GridUV((X + 0.5f) * InvNum, (Y + 0.5f) * InvNum);

			// 回溯：x(t-dt) = x(t) - u * dt
			const FVector2f BackUV = CellUV - Vel * Dt;

			FVector2f NewVel = SampleVelocityBilinear(VelField, BackUV) * Dissipation;
			FVector2f NewUV = SampleVelocityBilinear(UVField, BackUV);

			// 限制 UV 被搬运的比例：不限制的话位移场会越滚越大，最后整张 RT 失控
			NewUV = FMath::Lerp(GridUV, NewUV, UVStrength);

			// 速度限幅
			const float Speed = NewVel.Size();
			if (Speed > MaxSpeed)
			{
				NewVel = NewVel / Speed * MaxSpeed;
			}

			// 越界钳制
			NewUV.X = FMath::Clamp(NewUV.X, 0.f, 1.f - InvNum);
			NewUV.Y = FMath::Clamp(NewUV.Y, 0.f, 1.f - InvNum);

			VelTemp[I] = NewVel;
			UVTemp[I] = NewUV;
		}
	});

	// 交换而不是整块拷贝（512 网格下每帧省掉两次 1MB memcpy）
	Swap(VelField, VelTemp);
	Swap(UVField, UVTemp);
}

// ============================================================================
// 文章「4. Divergency」：∇·w = (w[i+1]-w[i-1] + w[j+1]-w[j-1]) / 2
// ============================================================================
void UConveyanceFluidSolver::ComputeDivergence()
{
	ParallelFor(Num, [&](int32 Y)
	{
		const int32 Ym = ClampIndex(Y - 1);
		const int32 Yp = ClampIndex(Y + 1);

		for (int32 X = 0; X < Num; ++X)
		{
			const int32 I = IndexOf(X, Y);
			const FVector2f VL = VelField[IndexOf(ClampIndex(X - 1), Y)];
			const FVector2f VR = VelField[IndexOf(ClampIndex(X + 1), Y)];
			const FVector2f VD = VelField[IndexOf(X, Ym)];
			const FVector2f VU = VelField[IndexOf(X, Yp)];

			Divergence[I] = 0.5f * ((VR.X - VL.X) + (VU.Y - VD.Y));
		}
	});
}

// ============================================================================
// 文章「5. Pressure」：p = (pL+pR+pD+pU - div) / 4，雅克比迭代
// ============================================================================
void UConveyanceFluidSolver::SolvePressure()
{
	// 雅可比迭代：从 Src 读、往 Dst 写，每轮只交换指针（不再每轮做一次全网格拷贝）
	PressureTemp = Pressure;

	TArray<float>* Src = &PressureTemp;
	TArray<float>* Dst = &Pressure;

	for (int32 Iter = 0; Iter < PressureIterations; ++Iter)
	{
		const float* S = Src->GetData();
		float* D = Dst->GetData();

		ParallelFor(Num, [&](int32 Y)
		{
			const int32 Ym = ClampIndex(Y - 1);
			const int32 Yp = ClampIndex(Y + 1);

			for (int32 X = 0; X < Num; ++X)
			{
				const int32 Xm = ClampIndex(X - 1);
				const int32 Xp = ClampIndex(X + 1);
				const int32 I = IndexOf(X, Y);

				D[I] = 0.25f * (S[IndexOf(Xm, Y)] + S[IndexOf(Xp, Y)]
					+ S[IndexOf(X, Ym)] + S[IndexOf(X, Yp)] - Divergence[I]);
			}
		});

		Swap(Src, Dst);
	}

	// 迭代次数为偶数时结果落在 PressureTemp 上，搬回 Pressure（一帧只搬一次）
	if (Src != &Pressure)
	{
		Pressure = PressureTemp;
	}
}

// ============================================================================
// 文章「6. UpdateVelocity」：u = w - ∇p
// ============================================================================
void UConveyanceFluidSolver::ProjectVelocity()
{
	ParallelFor(Num, [&](int32 Y)
	{
		const int32 Ym = ClampIndex(Y - 1);
		const int32 Yp = ClampIndex(Y + 1);

		for (int32 X = 0; X < Num; ++X)
		{
			const int32 I = IndexOf(X, Y);
			const float PL = Pressure[IndexOf(ClampIndex(X - 1), Y)];
			const float PR = Pressure[IndexOf(ClampIndex(X + 1), Y)];
			const float PD = Pressure[IndexOf(X, Ym)];
			const float PU = Pressure[IndexOf(X, Yp)];

			VelField[I].X -= 0.5f * (PR - PL);
			VelField[I].Y -= 0.5f * (PU - PD);
		}
	});
}

// ============================================================================
// 笔迹衰减 + 局部化：保证"扰动只在粒子投影附近"，且越早的划痕越淡
// ============================================================================
void UConveyanceFluidSolver::DecayTraceAndLocalize(float DeltaSeconds)
{
	if (Trace.Num() != Num * Num)
	{
		return;
	}

	const float Life = FMath::Max(TraceLifeSeconds, 0.01f);
	const float TraceMul = FMath::Exp(-DeltaSeconds / Life);   // 指数衰减，帧率无关
	const float Kill = FieldKillFactor;
	const int32 Total = Num * Num;

	ParallelFor(Total, [&](int32 I)
	{
		Trace[I] *= TraceMul;

		// 没有新鲜笔迹的地方，把速度狠狠压掉 —— 压力求解是全局的，
		// 不压的话速度会靠"不可压缩性"扩散到整个屏幕
		const float Local = FMath::Clamp(Trace[I] * 4.f, 0.f, 1.f);
		if (Local < 1.f)
		{
			VelField[I] = VelField[I] * FMath::Lerp(Kill, 1.f, Local);
		}
	});
}

// ============================================================================
// 文章「7. Recover」：缓慢把流体 UV 拉回网格自身 UV
// ============================================================================
void UConveyanceFluidSolver::Recover()
{
	if (RecoverRate <= 0.f)
	{
		return;
	}

	const float InvNum = 1.f / static_cast<float>(Num);
	const float Alpha = FMath::Clamp(RecoverRate, 0.f, 1.f);

	ParallelFor(Num, [&](int32 Y)
	{
		for (int32 X = 0; X < Num; ++X)
		{
			const int32 I = IndexOf(X, Y);
			const FVector2f CellUV((X + 0.5f) * InvNum, (Y + 0.5f) * InvNum);
			UVField[I] = FMath::Lerp(UVField[I], CellUV, Alpha);
		}
	});
}

// ============================================================================
// 文章「8. RenderToRT」：RG = 流体 UV，BA = 速度，写进 RGBA16F 贴图
// ============================================================================
void UConveyanceFluidSolver::WriteOutputTexture()
{
	if (!FieldTexture)
	{
		return;
	}

	float MaxVel = 0.f;

	{
		const float InvNum = 1.f / static_cast<float>(Num);
		const int32 BlockRows = 16;
		const int32 NumTasks = FMath::DivideAndRoundUp(Num, BlockRows);
		TArray<float> BlockMax;
		BlockMax.SetNumZeroed(NumTasks);

		ParallelFor(NumTasks, [&](int32 T)
		{
			float LocalMax = 0.f;
			const int32 Y0 = T * BlockRows;
			const int32 Y1 = FMath::Min(Y0 + BlockRows, Num);

			for (int32 Y = Y0; Y < Y1; ++Y)
			{
				for (int32 X = 0; X < Num; ++X)
				{
					const int32 I = IndexOf(X, Y);
					const FVector2f CellUV((X + 0.5f) * InvNum, (Y + 0.5f) * InvNum);

					// RG = 位移场（流体 UV 偏离自身 UV 的量）—— 后处理材质直接拿它当折射偏移
					// B  = 速度大小（血红覆盖层用它做 alpha）
					const FVector2f Disp = UVField[I] - CellUV;

					// 位移限幅到 ±0.02 UV（约 ±20px@1080p），防止折射把画面撕开
					const float DispX = FMath::Clamp(Disp.X, -0.02f, 0.02f);
					const float DispY = FMath::Clamp(Disp.Y, -0.02f, 0.02f);
					const float Speed = VelField[I].Size();
					LocalMax = FMath::Max(LocalMax, Speed);

					PixelHalfs[I * 4 + 0] = FFloat16(DispX);
					PixelHalfs[I * 4 + 1] = FFloat16(DispY);
					PixelHalfs[I * 4 + 2] = FFloat16(Speed);
					PixelHalfs[I * 4 + 3] = FFloat16(1.f);
				}
			}

			BlockMax[T] = LocalMax;
		});

		for (int32 T = 0; T < NumTasks; ++T)
		{
			MaxVel = FMath::Max(MaxVel, BlockMax[T]);
		}
	}

	LastMaxVel = MaxVel;

	// 关键：Region 必须活到渲染线程执行完（引擎内部只保存指针），所以用 new + 回调里 delete
	FUpdateTextureRegion2D* Region = new FUpdateTextureRegion2D(0, 0, 0, 0, static_cast<uint32>(Num), static_cast<uint32>(Num));
	const uint32 SrcPitch = static_cast<uint32>(Num) * sizeof(FFloat16) * 4;

	FieldTexture->UpdateTextureRegions(
		0, 1, Region, SrcPitch, sizeof(FFloat16) * 4,
		reinterpret_cast<uint8*>(PixelHalfs.GetData()),
		[](uint8* SrcData, const FUpdateTextureRegion2D* InRegion)
		{
			delete InRegion;
		});
}

// ============================================================================
// 把场数据直接写进 RT 资产 —— 内容浏览器双击该 RT 就能实时看到
// ============================================================================
void UConveyanceFluidSolver::PushFieldToRenderTarget()
{
	if (!TargetRT || PixelHalfs.Num() != Num * Num * 4)
	{
		return;
	}
	if (TargetRT->SizeX != Num || TargetRT->SizeY != Num)
	{
		return;
	}

	FTextureRenderTargetResource* Res = TargetRT->GameThread_GetRenderTargetResource();
	if (!Res)
	{
		return;
	}

	FTextureRHIRef RHITexture = Res->GetRenderTargetTexture();
	if (!RHITexture.IsValid())
	{
		return;
	}

	// 数据拷贝一份再送到渲染线程（PixelHalfs 下一帧会被改写）
	TArray<uint8> Payload;
	Payload.SetNumUninitialized(PixelHalfs.Num() * static_cast<int32>(sizeof(FFloat16)));
	FMemory::Memcpy(Payload.GetData(), PixelHalfs.GetData(), Payload.Num());

	const uint32 Pitch = static_cast<uint32>(Num) * sizeof(FFloat16) * 4;
	const FUpdateTextureRegion2D Region(0, 0, 0, 0, static_cast<uint32>(Num), static_cast<uint32>(Num));

	ENQUEUE_RENDER_COMMAND(FluidPushToRT)(
		[RHITexture, Payload = MoveTemp(Payload), Region, Pitch](FRHICommandListImmediate& RHICmdList)
		{
			RHIUpdateTexture2D(RHITexture.GetReference(), 0, Region, Pitch, Payload.GetData());
		});
}

// ============================================================================
// 折射材质：加载 / 建 MID / 挂到玩家相机
// ============================================================================
void UConveyanceFluidSolver::SetupRefractionMaterial()
{
	// 洋红测试材质：无条件准备好，方便随时验证后处理链路是否通（fluid.pptest 1）
	if (!PPTestMID)
	{
		if (UMaterialInterface* TestMat = LoadObject<UMaterialInterface>(nullptr,
			TEXT("/Game/ConveyanceSpline/Effect_Spline/M_FluidPPTest.M_FluidPPTest")))
		{
			PPTestMID = UMaterialInstanceDynamic::Create(TestMat, this);
			UE_LOG(LogFluidSolver, Log, TEXT("[Fluid] 洋红测试材质已就绪（fluid.pptest 1 开启）"));
		}
	}

	if (!bApplyRefraction)
	{
		return;
	}

	if (!RefractionMaterial)
	{
		RefractionMaterial = LoadObject<UMaterialInterface>(nullptr,
			TEXT("/Game/ConveyanceSpline/Effect_Spline/M_FluidRefraction.M_FluidRefraction"));
	}
	if (!RefractionMaterial)
	{
		UE_LOG(LogFluidSolver, Warning,
			TEXT("[Fluid] 没找到 /Game/ConveyanceSpline/Effect_Spline/M_FluidRefraction，屏幕折射跳过（只显示可视化覆盖层）"));
		return;
	}

	RefractionMID = UMaterialInstanceDynamic::Create(RefractionMaterial, this);
	if (!RefractionMID)
	{
		return;
	}

	// 用 Unbound 的后处理组件挂到本 Actor 上 —— 比挂相机后处理更可靠（一定全局生效）
	if (AActor* Owner = GetOwner())
	{
		UPostProcessComponent* PPComp = NewObject<UPostProcessComponent>(Owner);
		if (PPComp)
		{
			PPComp->bEnabled = true;
			PPComp->bUnbound = true;
			PPComp->Priority = 1000.f;
			PPComp->BlendWeight = 1.f;
			PPComp->Settings.AddBlendable(RefractionMID, 1.0f);
			PPComp->SetupAttachment(Owner->GetRootComponent());
			PPComp->RegisterComponent();
			PPCompRef = PPComp;

			bRefractionBound = true;
			UE_LOG(LogFluidSolver, Log, TEXT("[Fluid] 折射材质已通过 PostProcessComponent(bUnbound) 全局生效"));
		}
	}

	// 相机后处理也挂一份（有些工程的后处理体积会覆盖全局设置）
	if (UWorld* World = GetWorld())
	{
		if (APlayerController* PC = World->GetFirstPlayerController())
		{
			if (APawn* Pawn = PC->GetPawn())
			{
				if (UCameraComponent* Cam = Pawn->FindComponentByClass<UCameraComponent>())
				{
					Cam->PostProcessSettings.AddBlendable(RefractionMID, 1.0f);
					UE_LOG(LogFluidSolver, Log, TEXT("[Fluid] 折射材质同时挂到相机 %s"), *Cam->GetName());
				}
			}
		}
	}
	// 洋红测试材质：加载一次，命令开关时挂到 PP 组件上
	if (!PPTestMID)
	{
		if (UMaterialInterface* TestMat = LoadObject<UMaterialInterface>(nullptr,
			TEXT("/Game/ConveyanceSpline/Effect_Spline/M_FluidPPTest.M_FluidPPTest")))
		{
			PPTestMID = UMaterialInstanceDynamic::Create(TestMat, this);
			UE_LOG(LogFluidSolver, Log, TEXT("[Fluid] 洋红测试材质已就绪（fluid.pptest 1 开启）"));
		}
	}
}

void UConveyanceFluidSolver::UpdateRefraction()
{
	if (!RefractionMID)
	{
		return;
	}

	if (FieldTexture)
	{
		RefractionMID->SetTextureParameterValue(TEXT("FluidRT"), FieldTexture);
	}
	// 折射强度 = 速度的线性函数：strength = Min + saturate(|v|/SpeedRef) * (Max - Min)
	RefractionMID->SetScalarParameterValue(TEXT("StrengthMin"), RefractionStrengthMin);
	RefractionMID->SetScalarParameterValue(TEXT("StrengthSpan"), RefractionStrengthMax - RefractionStrengthMin);
	RefractionMID->SetScalarParameterValue(TEXT("InvSpeedRef"), 1.f / FMath::Max(RefractionSpeedRef, 0.01f));
	RefractionMID->SetScalarParameterValue(TEXT("MaskScale"), RefractionMaskScale);
	RefractionMID->SetScalarParameterValue(TEXT("GridSize"), static_cast<float>(Num));
	// SolidTest 通道已随材质简化移除

	// 兜底层：每帧塞进 PlayerCameraManager 的缓存 PP 混合（每帧开头会被清空，所以要每帧加）
	if (bUseCameraManagerBlend)
	{
		if (UWorld* World = GetWorld())
		{
			if (APlayerController* PC = World->GetFirstPlayerController())
			{
				if (PC->PlayerCameraManager)
				{
					FPostProcessSettings PP;
					PP.WeightedBlendables.Array.Add(FWeightedBlendable(1.f, RefractionMID));
					PC->PlayerCameraManager->AddCachedPPBlend(PP, 1.0f);
				}
			}
		}
	}
	// 洋红测试开关（每帧调用 AddBlendable 会重复叠加，所以只在 SetPPTest 里切）
}

void UConveyanceFluidSolver::SetPPTest(float Value)
{
	bPPTestOn = (Value > 0.5f);
	if (PPCompRef && PPTestMID)
	{
		if (bPPTestOn)
		{
			PPCompRef->Settings.AddBlendable(PPTestMID, 1.0f);
		}
		else
		{
			PPCompRef->Settings.RemoveBlendable(PPTestMID);
		}
	}
	UE_LOG(LogFluidSolver, Log, TEXT("[Fluid] PPTest = %d（PPComp=%d TESTMID=%d）"),
		bPPTestOn ? 1 : 0, PPCompRef ? 1 : 0, PPTestMID ? 1 : 0);
}

// ============================================================================
// 一帧完整解算（对应文章 8 个阶段的执行顺序）
// ============================================================================
void UConveyanceFluidSolver::Simulate(float DeltaSeconds)
{
	if (!bBuffersReady)
	{
		InitializeBuffers();
	}

	const double StartTime = FPlatformTime::Seconds();

	const float Dt = FMath::Clamp(DeltaSeconds, 0.f, 1.f / 30.f);   // 夹住 dt，避免大跳帧炸掉

	InjectSplats(Dt);          // 2 VolumeForce
	Advect(Dt);                // 3 Advection

	if (bEnablePressureSolve)
	{
		ComputeDivergence();   // 4 Divergency
		SolvePressure();       // 5 Pressure（Jacobi）
		ProjectVelocity();     // 6 UpdateVelocity
	}

	DecayTraceAndLocalize(Dt); // 笔迹衰减 + 只在粒子附近保留扰动
	Recover();                 // 7 Recover
	WriteOutputTexture();      // 8 RenderToRT
	PushFieldToRenderTarget(); // 同步写进 RT 资产（可在编辑器里直接看）
	UpdateRefraction();        // 把场纹理喂给屏幕折射材质
	UpdateBloodTint();         // 把场纹理喂给屏幕血红覆盖层材质

	if (bShowOnScreen)
	{
		UpdateDisplayTexture(); // 把流场直接铺到屏幕上（不需要材质）
	}

	LastSimMs = static_cast<float>((FPlatformTime::Seconds() - StartTime) * 1000.0);

	// 每秒打印一次流场统计，用来确认"解算真的在跑"
	if (bLogStats)
	{
		LogAccum += DeltaSeconds;
		if (LogAccum >= 1.f)
		{
			LogAccum = 0.f;

			float MaxSpeed = 0.f;
			float SumSpeed = 0.f;
			float MaxPressure = 0.f;
			const int32 Total = Num * Num;
			for (int32 I = 0; I < Total; ++I)
			{
				const float S = VelField[I].Size();
				MaxSpeed = FMath::Max(MaxSpeed, S);
				SumSpeed += S;
				MaxPressure = FMath::Max(MaxPressure, FMath::Abs(Pressure[I]));
			}

			UE_LOG(LogFluidSolver, Log,
				TEXT("[Fluid] grid=%dx%d splats=%d  maxV=%.4f UV/s  meanV=%.4f  maxP=%.4f  sim=%.2f ms"),
				Num, Num, LastSplatCount, MaxSpeed, SumSpeed / FMath::Max(1, Total), MaxPressure, LastSimMs);
		}
	}
}

// ============================================================================
// 屏幕血红覆盖层：RT 里"非黑"的像素 = 被粒子扰动过的区域，
// 把这些区域在屏幕上叠一层半透明血红（材质 M_FluidBloodTint，PostProcess 域）
// ============================================================================
void UConveyanceFluidSolver::SetupBloodTint()
{
	if (!bApplyBloodTint)
	{
		return;
	}

	if (!BloodTintMaterial)
	{
		BloodTintMaterial = LoadObject<UMaterialInterface>(nullptr,
			TEXT("/Game/ConveyanceSpline/Effect_Spline/M_FluidBloodTint.M_FluidBloodTint"));
	}
	if (!BloodTintMaterial)
	{
		UE_LOG(LogFluidSolver, Warning,
			TEXT("[Fluid] 没找到 /Game/ConveyanceSpline/Effect_Spline/M_FluidBloodTint，屏幕血红覆盖层跳过"));
		return;
	}

	BloodTintMID = UMaterialInstanceDynamic::Create(BloodTintMaterial, this);
	if (!BloodTintMID)
	{
		return;
	}
	UE_LOG(LogFluidSolver, Log, TEXT("[Fluid] 屏幕血红覆盖层材质就绪：%s"),
		*BloodTintMaterial->GetName());

	// 折射关掉时 PP 组件是空的，这里自己建一个；
	// 有就复用 —— 避免两个 Unbound 组件把同一个效果叠两次
	if (!PPCompRef)
	{
		if (AActor* Owner = GetOwner())
		{
			UPostProcessComponent* PPComp = NewObject<UPostProcessComponent>(Owner);
			if (PPComp)
			{
				PPComp->bEnabled = true;
				PPComp->bUnbound = true;
				PPComp->Priority = 1000.f;
				PPComp->BlendWeight = 1.f;
				PPComp->SetupAttachment(Owner->GetRootComponent());
				PPComp->RegisterComponent();
				PPCompRef = PPComp;
			}
		}
	}

	if (PPCompRef)
	{
		PPCompRef->Settings.AddBlendable(BloodTintMID, 1.f);
		UE_LOG(LogFluidSolver, Log, TEXT("[Fluid] 血红覆盖层已挂到 PostProcessComponent(bUnbound)"));
	}
	else
	{
		// 建不出 PP 组件就退回 PlayerCameraManager（在 UpdateBloodTint 里每帧加）
		UE_LOG(LogFluidSolver, Warning, TEXT("[Fluid] PP 组件没建出来，血红覆盖层改走相机兜底通道"));
	}
}

void UConveyanceFluidSolver::UpdateBloodTint()
{
	if (!BloodTintMID)
	{
		return;
	}

	// 不透明度与 V 线性：alpha = saturate(V / Ref) * TintAlpha * 整体权重
	//   最快的地方（V >= Ref）→ TintAlpha（默认 1.0，完全不透明）
	//   最慢的地方（V = 0）    → 0（完全透明）
	//   整体权重 BloodTintWeight 由 ASplineRouteActor 按"到土地庙的距离"每帧写入（和血丝同一套）
	float Ref = FMath::Max(BloodTintSpeedRef, 0.01f);
	if (bBloodTintAutoSpeed)
	{
		// 自动归一化：本帧最快的格子 alpha = 1（完全不透明），最慢 = 0（完全透明）
		// 只留一个很小的下限，防止流场快死掉时把噪点放大成满屏血红
		Ref = FMath::Max(LastMaxVel, 0.02f);
	}

	BloodTintMID->SetTextureParameterValue(TEXT("FluidRT"), FieldTexture);
	BloodTintMID->SetVectorParameterValue(TEXT("BloodTintColor"), BloodTintColor);
	BloodTintMID->SetScalarParameterValue(TEXT("InvSpeedRef"), 1.f / Ref);
	BloodTintMID->SetScalarParameterValue(TEXT("TintAlpha"),
		FMath::Clamp(BloodTintAlpha, 0.f, 1.f) * BloodTintWeight);
	BloodTintMID->SetScalarParameterValue(TEXT("Brightness"), BloodTintBrightness);

	// ---- 端点软洞：角色脚下 / 土地庙底部各挖一个圆形渐隐 ----
	const float HoleScale = FMath::Max(BloodTintHoleScale, 0.0001f);
	const FVector2D RA(FMath::Max(BloodTintEndARadius.X * HoleScale, 1e-4),
		FMath::Max(BloodTintEndARadius.Y * HoleScale, 1e-4));
	const FVector2D RB(FMath::Max(BloodTintEndBRadius.X * HoleScale, 1e-4),
		FMath::Max(BloodTintEndBRadius.Y * HoleScale, 1e-4));

	BloodTintMID->SetVectorParameterValue(TEXT("EndAUV"),
		FLinearColor(static_cast<float>(BloodTintEndAUV.X), static_cast<float>(BloodTintEndAUV.Y), 0.f, 0.f));
	BloodTintMID->SetVectorParameterValue(TEXT("InvRadiusA"),
		FLinearColor(static_cast<float>(1.0 / RA.X), static_cast<float>(1.0 / RA.Y), 0.f, 0.f));
	BloodTintMID->SetVectorParameterValue(TEXT("EndBUV"),
		FLinearColor(static_cast<float>(BloodTintEndBUV.X), static_cast<float>(BloodTintEndBUV.Y), 0.f, 0.f));
	BloodTintMID->SetVectorParameterValue(TEXT("InvRadiusB"),
		FLinearColor(static_cast<float>(1.0 / RB.X), static_cast<float>(1.0 / RB.Y), 0.f, 0.f));

	// 只有 PP 组件没建出来时才走这条 —— 保证效果只叠一次
	if (!PPCompRef && bUseCameraManagerBlend)
	{
		if (UWorld* World = GetWorld())
		{
			if (APlayerController* PC = World->GetFirstPlayerController())
			{
				if (PC->PlayerCameraManager)
				{
					FPostProcessSettings PP;
					PP.WeightedBlendables.Array.Add(FWeightedBlendable(1.f, BloodTintMID));
					PC->PlayerCameraManager->AddCachedPPBlend(PP, 1.0f);
				}
			}
		}
	}
}

// ============================================================================
// 端点软洞：记录两端在屏幕上的 UV 和 UV 半径（半径由 0.5m 世界距离投影得到）
// ============================================================================
void UConveyanceFluidSolver::SetBloodTintEndpoints(const FVector2D& InUVA, const FVector2D& InRadiusA,
	const FVector2D& InUVB, const FVector2D& InRadiusB)
{
	BloodTintEndAUV = InUVA;
	BloodTintEndBUV = InUVB;
	BloodTintEndARadius = FVector2D(FMath::Max(InRadiusA.X, 1e-4), FMath::Max(InRadiusA.Y, 1e-4));
	BloodTintEndBRadius = FVector2D(FMath::Max(InRadiusB.X, 1e-4), FMath::Max(InRadiusB.Y, 1e-4));
}
