﻿// Copyright Timothé Lapetite 2023
// Released under the MIT license https://opensource.org/license/MIT/

#include "Graph/PCGExConsolidateGraph.h"

#define LOCTEXT_NAMESPACE "PCGExConsolidateGraph"

int32 UPCGExConsolidateGraphSettings::GetPreferredChunkSize() const { return 32; }

PCGExData::EInit UPCGExConsolidateGraphSettings::GetPointOutputInitMode() const { return PCGExData::EInit::DuplicateInput; }

FPCGElementPtr UPCGExConsolidateGraphSettings::CreateElement() const
{
	return MakeShared<FPCGExConsolidateGraphElement>();
}

FPCGContext* FPCGExConsolidateGraphElement::Initialize(
	const FPCGDataCollection& InputData,
	TWeakObjectPtr<UPCGComponent> SourceComponent,
	const UPCGNode* Node)
{
	FPCGExConsolidateGraphContext* Context = new FPCGExConsolidateGraphContext();
	InitializeContext(Context, InputData, SourceComponent, Node);

	PCGEX_SETTINGS(UPCGExConsolidateGraphSettings)
	
	PCGEX_FWD(bConsolidateEdgeType)
	
	return Context;
}

bool FPCGExConsolidateGraphElement::ExecuteInternal(
	FPCGContext* InContext) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FPCGExConsolidateGraphElement::Execute);

	PCGEX_CONTEXT(FPCGExConsolidateGraphContext)

	if (Context->IsSetup())
	{
		if (!Validate(Context)) { return true; }
		Context->SetState(PCGExGraph::State_ReadyForNextGraph);
	}

	if (Context->IsState(PCGExGraph::State_ReadyForNextGraph))
	{
		if (!Context->AdvanceGraph(true)) { Context->Done(); }
		else { Context->SetState(PCGExMT::State_ReadyForNextPoints); }
	}

	if (Context->IsState(PCGExMT::State_ReadyForNextPoints))
	{
		if (!Context->AdvancePointsIO(false))
		{
			Context->SetState(PCGExGraph::State_ReadyForNextGraph); //No more points, move to next params
		}
		else
		{
			Context->SetState(PCGExGraph::State_CachingGraphIndices);
		}
	}

	// 1st Pass on points

	if (Context->IsState(PCGExGraph::State_CachingGraphIndices))
	{
		auto Initialize = [&](PCGExData::FPointIO& PointIO)
		{
			Context->IndicesRemap.Empty(PointIO.GetNum());
			PointIO.BuildMetadataEntries();
			Context->PrepareCurrentGraphForPoints(PointIO.GetOut(), true); // Prepare to read PointIO->Out
		};

		auto ProcessPoint = [&](const int32 PointIndex, const PCGExData::FPointIO& PointIO)
		{
			FWriteScopeLock WriteLock(Context->IndicesLock);
			const int64 Key = PointIO.GetOutPoint(PointIndex).MetadataEntry;
			const int32 CachedIndex = Context->CachedIndex->GetValueFromItemKey(Key);
			Context->IndicesRemap.Add(CachedIndex, PointIndex); // Store previous
			Context->CachedIndex->SetValue(Key, PointIndex);    // Update cached value with fresh one
		};


		if (Context->ProcessCurrentPoints(Initialize, ProcessPoint))
		{
			Context->SetState(PCGExGraph::State_SwappingGraphIndices);
		}
	}

	// 2nd Pass on points - Swap indices with updated ones

	if (Context->IsState(PCGExGraph::State_SwappingGraphIndices))
	{
		auto ConsolidatePoint = [&](const int32 PointIndex, const PCGExData::FPointIO& PointIO)
		{
			FReadScopeLock ReadLock(Context->IndicesLock);
			const FPCGPoint& Point = PointIO.GetOutPoint(PointIndex);
			for (const PCGExGraph::FSocketInfos& SocketInfos : Context->SocketInfos)
			{
				const int64 OldRelationIndex = SocketInfos.Socket->GetTargetIndex(Point.MetadataEntry);

				if (OldRelationIndex == -1) { continue; } // No need to fix further

				const int64 NewRelationIndex = GetFixedIndex(Context, OldRelationIndex);
				const PCGMetadataEntryKey Key = Point.MetadataEntry;
				PCGMetadataEntryKey NewEntryKey = PCGInvalidEntryKey;

				if (NewRelationIndex != -1) { NewEntryKey = PointIO.GetOutPoint(NewRelationIndex).MetadataEntry; }
				else { SocketInfos.Socket->SetEdgeType(Key, EPCGExEdgeType::Unknown); }

				SocketInfos.Socket->SetTargetIndex(Key, NewRelationIndex);
				SocketInfos.Socket->SetTargetEntryKey(Key, NewEntryKey);
			}
		};

		if (Context->ProcessCurrentPoints(ConsolidatePoint))
		{
			Context->SetState(Context->bConsolidateEdgeType ? PCGExGraph::State_FindingEdgeTypes : PCGExMT::State_ReadyForNextPoints);
		}
	}

	// Optional 3rd Pass on points - Recompute edges type

	if (Context->IsState(PCGExGraph::State_FindingEdgeTypes))
	{
		auto ConsolidateEdgesType = [&](const int32 PointIndex, const PCGExData::FPointIO& PointIO)
		{
			ComputeEdgeType(Context->SocketInfos, PointIO.GetOutPoint(PointIndex), PointIndex, PointIO);
		};

		if (Context->ProcessCurrentPoints(ConsolidateEdgesType))
		{
			Context->SetState(PCGExMT::State_ReadyForNextPoints);
		}
	}

	// Done

	if (Context->IsDone())
	{
		Context->IndicesRemap.Empty();
		Context->OutputPointsAndGraphParams();
	}

	return Context->IsDone();
}

int64 FPCGExConsolidateGraphElement::GetFixedIndex(FPCGExConsolidateGraphContext* Context, int64 InIndex)
{
	if (const int64* FixedRelationIndexPtr = Context->IndicesRemap.Find(InIndex)) { return *FixedRelationIndexPtr; }
	return -1;
}

#undef LOCTEXT_NAMESPACE
