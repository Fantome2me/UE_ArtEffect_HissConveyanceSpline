// Copyright Epic Games, Inc. All Rights Reserved.

#include "UE_ConveyanceSplineGameMode.h"

#include "SplineRouteActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/StaticMesh.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SplineComponent.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/Character.h"
#include "GameFramework/Pawn.h"
#include "Kismet/GameplayStatics.h"


//获取玩家起点，在场景中按名字找到目标 Mesh Actor 作为终点，然后生成 Spline 路线并建线

namespace
{
	//按名字在场景里找静态网格 Actor
	AStaticMeshActor* FindMeshActorByName(const UWorld* World, FName MeshName)
	{
		if (!World)
		{
			return nullptr;
		}
			for (TActorIterator<AStaticMeshActor> It(const_cast<UWorld*>(World)); It; ++It)
			{
				AStaticMeshActor* Candidate = *It;
				if (!Candidate || !Candidate->GetStaticMeshComponent()) 
				{
					continue;
				}
				const UStaticMesh* MeshComponent = Candidate->GetStaticMeshComponent()->GetStaticMesh();
				if (MeshComponent && MeshComponent->GetFName() == MeshName)
				{
					return Candidate;
				}
			}
			return nullptr;
    }
}

AUE_ConveyanceSplineGameMode::AUE_ConveyanceSplineGameMode()
{

}

void AUE_ConveyanceSplineGameMode::StartPlay()
{
	Super::StartPlay();

	UWorld* World = GetWorld();
	if (!World)
	{
		return ;
	}

	//取得玩家 Pawn，用它的位置作为起点
	APawn* PlayerPawn = UGameplayStatics:: GetPlayerPawn(this,0);
	if (!PlayerPawn)
	{
		return ;
	}
	const FVector StartPoint = PlayerPawn->GetActorLocation();

	//用 FindMeshActorByName 找到场景里的目标物
	const FName TargetMeshName(TEXT("SM_Item_TuDiMiao_02"));//这里写目标网格体的名字
	AStaticMeshActor* TargetActor = FindMeshActorByName(World, TargetMeshName);
	if (!TargetActor)
	{
		return;
	}

	//以目标物位置作为终点
	const FVector EndPoint = TargetActor->GetActorLocation();

	//生成 Spline 路线 Actor
	FActorSpawnParameters Params;
	Params.Owner = this;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	ASplineRouteActor* Route = World->SpawnActor<ASplineRouteActor>(ASplineRouteActor::StaticClass(),FTransform::Identity,Params);
	if (!Route)
	{
		return;
	}

	//建线
	Route->BuildThreePointRoute(StartPoint, EndPoint);
}
