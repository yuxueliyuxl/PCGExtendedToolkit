﻿// Copyright Timothé Lapetite 2024
// Released under the MIT license https://opensource.org/license/MIT/

#include "Topology/PCGExTopology.h"

namespace PCGExTopology
{
	bool FCellConstraints::ContainsSignedEdgeHash(const uint64 Hash) const
	{
		if (!bDedupe) { return false; }
		else
		{
			FReadScopeLock ReadScopeLock(UniquePathsStartHashLock);
			return UniquePathsStartHash.Contains(Hash);
		}
	}

	bool FCellConstraints::IsUniqueStartHash(const uint64 Hash)
	{
		if (!bDedupe) { return true; }
		else
		{
			bool bAlreadyExists;
			FWriteScopeLock WriteScopeLock(UniquePathsStartHashLock);
			UniquePathsStartHash.Add(Hash, &bAlreadyExists);
			return !bAlreadyExists;
		}
	}

	bool FCellConstraints::IsUniqueCellHash(const FCell* InCell)
	{
		if (!bDedupe) { return true; }
		else
		{
			TArray<int32> Nodes = InCell->Nodes;
			Nodes.Sort();

			int32 Hash = 0;
			for (int32 i = 0; i < Nodes.Num(); i++) { Hash = HashCombineFast(Hash, Nodes[i]); }

			bool bAlreadyExists;
			FWriteScopeLock WriteScopeLock(UniquePathsStartHashLock);
			UniquePathsBoxHash.Add(Hash, &bAlreadyExists);
			return !bAlreadyExists;
		}
	}

	ECellResult FCell::BuildFromCluster(
		const int32 SeedNodeIndex,
		const int32 SeedEdgeIndex,
		const FVector& Guide,
		TSharedRef<PCGExCluster::FCluster> InCluster,
		const TArray<FVector>& ProjectedPositions,
		TSharedPtr<TArray<PCGExCluster::FExpandedNode>> ExpandedNodes)
	{
		bCompiledSuccessfully = false;
		Bounds = FBox(ForceInit);

		int32 StartNodeIndex = SeedNodeIndex;
		int32 PrevIndex = SeedNodeIndex;
		int32 NextIndex = InCluster->GetEdgeOtherNode(SeedEdgeIndex, PrevIndex)->NodeIndex;

		const FVector A = ProjectedPositions[InCluster->GetNode(PrevIndex)->PointIndex];
		const FVector B = ProjectedPositions[InCluster->GetNode(NextIndex)->PointIndex];

		const double SanityAngle = PCGExMath::GetDegreesBetweenVectors((B - A).GetSafeNormal(), (B - Guide).GetSafeNormal());
		const bool bStartIsDeadEnd = InCluster->GetNode(StartNodeIndex)->Adjacency.Num() == 1;

		if (bStartIsDeadEnd && !Constraints->bKeepContoursWithDeadEnds) { return ECellResult::DeadEnd; }

		if (SanityAngle > 180 && !bStartIsDeadEnd)
		{
			// Swap search orientation
			PrevIndex = NextIndex;
			NextIndex = StartNodeIndex;
			StartNodeIndex = PrevIndex;
		}

		const uint64 UniqueStartEdgeHash = PCGEx::H64(PrevIndex, NextIndex);
		if (!Constraints->IsUniqueStartHash(UniqueStartEdgeHash)) { return ECellResult::Duplicate; }

		Bounds += InCluster->GetPos(PrevIndex);
		int32 NumNodes = Nodes.Add(PrevIndex) + 1;

		TSet<int32> Exclusions = {PrevIndex, NextIndex};
		TSet<uint64> SignedEdges;
		bool bHasAdjacencyToStart = false;

		while (NextIndex != -1)
		{
			bool bEdgeAlreadyExists;
			uint64 SignedEdgeHash = PCGEx::H64(PrevIndex, NextIndex);

			if (SignedEdgeHash != UniqueStartEdgeHash && Constraints->ContainsSignedEdgeHash(SignedEdgeHash)) { return ECellResult::Duplicate; }

			SignedEdges.Add(SignedEdgeHash, &bEdgeAlreadyExists);
			if (bEdgeAlreadyExists) { break; }

			//

			double BestAngle = -1;
			int32 NextBest = -1;

			const PCGExCluster::FExpandedNode& Current = *(ExpandedNodes->GetData() + NextIndex);

			NumNodes = Nodes.Add(Current) + 1;
			if (NumNodes > Constraints->MaxPointCount) { return ECellResult::ExceedPointsLimit; }

			Bounds += InCluster->GetPos(Current.Node);
			if (Bounds.GetSize().Length() > Constraints->MaxBoundsSize) { return ECellResult::ExceedBoundsLimit; }

			const FVector P = ProjectedPositions[Current.Node->PointIndex];
			const FVector GuideDir = (P - ProjectedPositions[InCluster->GetNode(PrevIndex)->PointIndex]).GetSafeNormal();

			if (Current.Neighbors.Num() == 1 && Constraints->bDuplicateDeadEndPoints) { Nodes.Add(Current); }
			if (Current.Neighbors.Num() > 1) { Exclusions.Add(PrevIndex); }

			PrevIndex = NextIndex;

			bHasAdjacencyToStart = false;
			for (const PCGExCluster::FExpandedNeighbor& N : Current.Neighbors)
			{
				const int32 NeighborIndex = N.Node->NodeIndex;

				if (NeighborIndex == StartNodeIndex) { bHasAdjacencyToStart = true; }
				if (Exclusions.Contains(NeighborIndex)) { continue; }

				const FVector OtherDir = (P - ProjectedPositions[N.Node->PointIndex]).GetSafeNormal();

				if (const double Angle = PCGExMath::GetDegreesBetweenVectors(OtherDir, GuideDir); Angle > BestAngle)
				{
					BestAngle = Angle;
					NextBest = NeighborIndex;
				}
			}

			Exclusions.Reset();

			if (NextBest == StartNodeIndex)
			{
				bHasAdjacencyToStart = true;
				NextBest = -1;
			}

			if (NextBest != -1)
			{
				if (InCluster->GetNode(NextBest)->Adjacency.Num() == 1 && !Constraints->bKeepContoursWithDeadEnds) { return ECellResult::DeadEnd; }
				if (NumNodes > Constraints->MaxPointCount) { return ECellResult::ExceedBoundsLimit; }

				if (NumNodes > 2)
				{
					PCGExMath::CheckConvex(
						InCluster->GetPos(Nodes.Last(2)),
						InCluster->GetPos(Nodes.Last(1)),
						InCluster->GetPos(Nodes.Last()),
						bIsConvex, Sign);

					if (Constraints->bConvexOnly && !bIsConvex) { return ECellResult::WrongAspect; }
				}

				NextIndex = NextBest;
			}
			else
			{
				NextIndex = -1;
			}
		}

		bIsClosedLoop = bHasAdjacencyToStart;

		if (Constraints->bClosedLoopOnly && !bIsClosedLoop) { return ECellResult::OpenCell; }
		if (Constraints->bConcaveOnly && bIsConvex) { return ECellResult::WrongAspect; }
		if (NumNodes < Constraints->MinPointCount) { return ECellResult::BelowPointsLimit; }
		if (Bounds.GetSize().Length() < Constraints->MinBoundsSize) { return ECellResult::BelowBoundsLimit; }

		if (!Constraints->IsUniqueCellHash(this)) { return ECellResult::Duplicate; }

		bCompiledSuccessfully = true;
		return ECellResult::Success;
	}

	ECellResult FCell::BuildFromPath(const TArray<FVector>& ProjectedPositions)
	{
		return ECellResult::Unknown;
	}

	int32 FCell::GetTriangleNumEstimate() const
	{
		if (!bCompiledSuccessfully) { return 0; }
		if (bIsConvex || Nodes.Num() < 3) { return Nodes.Num(); }
		return Nodes.Num() + 2; // TODO : That's 100% arbitrary, need a better way to estimate concave triangulation
	}
}
