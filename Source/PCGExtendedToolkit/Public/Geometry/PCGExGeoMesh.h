﻿// Copyright Timothé Lapetite 2024
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "PCGExGeo.h"
#include "CompGeom/Delaunay2.h"
#include "CompGeom/Delaunay3.h"

namespace PCGExGeo
{

	constexpr PCGExMT::AsyncState State_ExtractingMesh = __COUNTER__;
	
	class FExtractStaticMeshTask;

	class PCGEXTENDEDTOOLKIT_API FGeoMesh
	{
	public:
		bool bIsValid = false;
		bool bIsLoaded = false;
		TArray<FVector> Vertices;
		TArray<uint64> Edges;

		FGeoMesh()
		{
		}

		~FGeoMesh()
		{
			Vertices.Empty();
			Edges.Empty();
		}
	};

	class PCGEXTENDEDTOOLKIT_API FGeoStaticMesh : public FGeoMesh
	{
	public:
		TObjectPtr<UStaticMesh> StaticMesh;

		explicit FGeoStaticMesh(const TSoftObjectPtr<UStaticMesh>& InSoftStaticMesh)
		{
			if (!InSoftStaticMesh.ToSoftObjectPath().IsValid()) { return; }
			StaticMesh = InSoftStaticMesh.LoadSynchronous();
			StaticMesh->ConditionalPostLoad();
			bIsValid = true;
		}

		explicit FGeoStaticMesh(const FSoftObjectPath& InSoftStaticMesh):
			FGeoStaticMesh(TSoftObjectPtr<UStaticMesh>(InSoftStaticMesh))
		{
		}

		explicit FGeoStaticMesh(const FString& InSoftStaticMesh):
			FGeoStaticMesh(TSoftObjectPtr<UStaticMesh>(FSoftObjectPath(InSoftStaticMesh)))
		{
		}

		void ExtractMeshSynchronous()
		{
			if (bIsLoaded) { return; }
			if (!bIsValid) { return; }

			const FStaticMeshLODResources& LODResources = StaticMesh->GetRenderData()->LODResources[0];

			const FPositionVertexBuffer& VertexBuffer = LODResources.VertexBuffers.PositionVertexBuffer;

			TMap<FVector, int32> IndexedUniquePositions;

			//for (int i = 0; i < GSM->Vertices.Num(); i++) { GSM->Vertices[i] = FVector(VertexBuffer.VertexPosition(i)); }

			TSet<uint64> UniqueEdges;

			const FIndexArrayView& Indices = LODResources.IndexBuffer.GetArrayView();
			for (int32 i = 0; i < Indices.Num(); i += 3)
			{
				uint32 A = Indices[i];
				uint32 B = Indices[i + 1];
				uint32 C = Indices[i + 2];

				const FVector VA = FVector(VertexBuffer.VertexPosition(A));
				const FVector VB = FVector(VertexBuffer.VertexPosition(B));
				const FVector VC = FVector(VertexBuffer.VertexPosition(C));

				A = IndexedUniquePositions.FindOrAdd(VA, A);
				B = IndexedUniquePositions.FindOrAdd(VB, B);
				C = IndexedUniquePositions.FindOrAdd(VC, C);

				UniqueEdges.Add(PCGEx::H64(A, B));
				UniqueEdges.Add(PCGEx::H64(B, C));
				UniqueEdges.Add(PCGEx::H64(C, A));
			}

			Vertices.SetNum(IndexedUniquePositions.Num());

			TArray<FVector> Keys;
			IndexedUniquePositions.GetKeys(Keys);
			for (FVector Key : Keys) { Vertices[*IndexedUniquePositions.Find(Key)] = Key; }

			Edges.Reset();
			Edges.Append(UniqueEdges.Array());

			IndexedUniquePositions.Empty();
			UniqueEdges.Empty();
			bIsLoaded = true;
		}

		void ExtractMeshAsync(FPCGExAsyncManager* AsyncManager)
		{
			if (bIsLoaded) { return; }
			if (!bIsValid) { return; }
			AsyncManager->Start<FExtractStaticMeshTask>(-1, nullptr, this);
		}

		~FGeoStaticMesh()
		{
		}
	};

	class PCGEXTENDEDTOOLKIT_API FGeoStaticMeshMap : public FGeoMesh
	{
	public:
		TMap<FSoftObjectPath, int32> Map;
		TArray<FGeoStaticMesh*> GSMs;

		FGeoStaticMeshMap()
		{
		}

		int32 Find(const FSoftObjectPath& InPath)
		{
			if (const int32* GSMPtr = Map.Find(InPath)) { return *GSMPtr; }

			FGeoStaticMesh* GSM = new FGeoStaticMesh(InPath);
			const int32 Index = GSMs.Add(GSM);
			Map.Add(InPath, Index);
			return Index;
		}

		FGeoStaticMesh* GetMesh(const int32 Index) { return GSMs[Index]; }

		~FGeoStaticMeshMap()
		{
			Map.Empty();
			PCGEX_DELETE_TARRAY(GSMs)
		}
	};

	class PCGEXTENDEDTOOLKIT_API FExtractStaticMeshTask : public FPCGExNonAbandonableTask
	{
	public:
		FExtractStaticMeshTask(
			FPCGExAsyncManager* InManager, const int32 InTaskIndex, PCGExData::FPointIO* InPointIO, FGeoStaticMesh* InGSM) :
			FPCGExNonAbandonableTask(InManager, InTaskIndex, InPointIO), GSM(InGSM)
		{
		}

		FGeoStaticMesh* GSM = nullptr;

		virtual bool ExecuteTask() override
		{
			GSM->ExtractMeshSynchronous();
			return true;
		}
	};
}
