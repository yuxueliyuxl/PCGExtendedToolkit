﻿// Copyright Timothé Lapetite 2024
// Released under the MIT license https://opensource.org/license/MIT/

#include "Graph/PCGExCutClusters.h"


#include "Graph/PCGExGraph.h"
#include "Graph/Edges/Refining/PCGExEdgeRefinePrimMST.h"
#include "Graph/Filters/PCGExClusterFilter.h"

#define LOCTEXT_NAMESPACE "PCGExCutEdges"
#define PCGEX_NAMESPACE CutEdges

TArray<FPCGPinProperties> UPCGExCutEdgesSettings::InputPinProperties() const
{
	TArray<FPCGPinProperties> PinProperties = Super::InputPinProperties();

	PCGEX_PIN_POINTS(PCGExGraph::SourcePathsLabel, "Cutting paths.", Required, {})
	if (Mode != EPCGExCutEdgesMode::Edges) { PCGEX_PIN_PARAMS(PCGExCutEdges::SourceNodeFilters, "Node preservation filters.", Normal, {}) }
	if (Mode != EPCGExCutEdgesMode::Nodes) { PCGEX_PIN_PARAMS(PCGExCutEdges::SourceEdgeFilters, "Edge preservation filters.", Normal, {}) }

	return PinProperties;
}

PCGExData::EInit UPCGExCutEdgesSettings::GetMainOutputInitMode() const { return PCGExData::EInit::NewOutput; }
PCGExData::EInit UPCGExCutEdgesSettings::GetEdgeOutputInitMode() const { return PCGExData::EInit::NoOutput; }

PCGEX_INITIALIZE_ELEMENT(CutEdges)

bool FPCGExCutEdgesElement::Boot(FPCGExContext* InContext) const
{
	if (!FPCGExEdgesProcessorElement::Boot(InContext)) { return false; }

	PCGEX_CONTEXT_AND_SETTINGS(CutEdges)

	PCGEX_FWD(IntersectionDetails)
	Context->IntersectionDetails.Init();

	PCGEX_FWD(GraphBuilderDetails)

	if (Settings->Mode != EPCGExCutEdgesMode::Nodes)
	{
		GetInputFactories(Context, PCGExCutEdges::SourceEdgeFilters, Context->EdgeFilterFactories, PCGExFactories::ClusterEdgeFilters, false);
	}

	if (Settings->Mode != EPCGExCutEdgesMode::Edges)
	{
		GetInputFactories(Context, PCGExCutEdges::SourceNodeFilters, Context->NodeFilterFactories, PCGExFactories::ClusterNodeFilters, false);
	}


	TSharedPtr<PCGExData::FPointIOCollection> PathCollection = MakeShared<PCGExData::FPointIOCollection>(Context, PCGExGraph::SourcePathsLabel);
	if (PathCollection->IsEmpty())
	{
		PCGE_LOG(Error, GraphAndLog, FTEXT("Empty paths."));
		return false;
	}

	Context->PathFacades.Reserve(PathCollection->Num());
	Context->Paths.Reserve(PathCollection->Num());

	int32 ExcludedNum = 0;

	for (TSharedPtr<PCGExData::FPointIO> PathIO : PathCollection->Pairs)
	{
		if (PathIO->GetNum() < 2)
		{
			ExcludedNum++;
			continue;
		}

		TSharedPtr<PCGExData::FFacade> Facade = MakeShared<PCGExData::FFacade>(PathIO.ToSharedRef());
		Facade->bSupportsScopedGet = Context->bScopedAttributeGet;

		Context->PathFacades.Add(Facade.ToSharedRef());
	}

	if (ExcludedNum != 0)
	{
		PCGE_LOG(Warning, GraphAndLog, FTEXT("Some input paths had less than 2 points and will be ignored."));
	}

	if (Context->PathFacades.IsEmpty())
	{
		PCGE_LOG(Error, GraphAndLog, FTEXT("No valid paths found."));
		return false;
	}

	PCGEX_FWD(ClosedLoop)
	Context->ClosedLoop.Init();


	return true;
}

bool FPCGExCutEdgesElement::ExecuteInternal(
	FPCGContext* InContext) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FPCGExCutEdgesElement::Execute);

	PCGEX_CONTEXT_AND_SETTINGS(CutEdges)
	PCGEX_EXECUTION_CHECK
	PCGEX_ON_INITIAL_EXECUTION
	{
		Context->SetAsyncState(PCGExPaths::State_BuildingPaths);
		PCGEX_ASYNC_GROUP_CHKD(Context->GetAsyncManager(), BuildPathsTask)

		BuildPathsTask->OnIterationRangeStartCallback = [Settings, Context](const int32 StartIndex, const int32 Count, const int32 LoopIdx)
		{
			TSharedRef<PCGExData::FFacade> PathFacade = Context->PathFacades[StartIndex];
			TSharedPtr<PCGExPaths::FPath> Path = PCGExPaths::MakePath(
				PathFacade->Source->GetIn()->GetPoints(), 0,
				Context->ClosedLoop.IsClosedLoop(PathFacade->Source), false);

			Path->BuildEdgeOctree();

			Context->Paths.Add(Path.ToSharedRef());
		};

		BuildPathsTask->StartRangePrepareOnly(Context->PathFacades.Num(), 1);
	}

	PCGEX_ON_ASYNC_STATE_READY(PCGExPaths::State_BuildingPaths)
	{
		if (!Context->StartProcessingClusters<PCGExCutEdges::FProcessorBatch>(
			[](const TSharedPtr<PCGExData::FPointIOTaggedEntries>& Entries) { return true; },
			[&](const TSharedPtr<PCGExCutEdges::FProcessorBatch>& NewBatch)
			{
				NewBatch->GraphBuilderDetails = Context->GraphBuilderDetails;
			}))
		{
			PCGE_LOG(Warning, GraphAndLog, FTEXT("Could not build any clusters."));
			return true;
		}
	}

	PCGEX_CLUSTER_BATCH_PROCESSING(PCGExGraph::State_ReadyToCompile)
	if (!Context->CompileGraphBuilders(true, PCGEx::State_Done)) { return false; }

	Context->MainPoints->StageOutputs();

	return Context->TryComplete();
}

namespace PCGExCutEdges
{
	TSharedPtr<PCGExCluster::FCluster> FProcessor::HandleCachedCluster(const TSharedRef<PCGExCluster::FCluster>& InClusterRef)
	{
		// Create a light working copy with edges only, will be deleted.
		return MakeShared<PCGExCluster::FCluster>(
			InClusterRef, VtxDataFacade->Source, EdgeDataFacade->Source,
			Settings->Mode != EPCGExCutEdgesMode::Edges,
			Settings->Mode != EPCGExCutEdgesMode::Nodes,
			false);
	}

	FProcessor::~FProcessor()
	{
	}

	bool FProcessor::Process(TSharedPtr<PCGExMT::FTaskManager> InAsyncManager)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(PCGExCutEdges::Process);

		if (!FClusterProcessor::Process(InAsyncManager)) { return false; }

		EdgeFilterCache.Init(false, EdgeDataFacade->Source->GetNum());
		NodeFilterCache.Init(false, Cluster->Nodes->Num());

		if (Settings->bInvert)
		{
			if (Settings->Mode != EPCGExCutEdgesMode::Nodes) { for (PCGExGraph::FIndexedEdge& E : *Cluster->Edges) { E.bValid = false; } }
			if (Settings->Mode != EPCGExCutEdgesMode::Edges) { for (PCGExCluster::FNode& N : *Cluster->Nodes) { N.bValid = false; } }
		}

		if (Settings->Mode != EPCGExCutEdgesMode::Nodes)
		{
			if (!Context->EdgeFilterFactories.IsEmpty())
			{
				EdgeFilterManager = MakeShared<PCGExClusterFilter::FManager>(Cluster.ToSharedRef(), VtxDataFacade, EdgeDataFacade);
				EdgeFilterManager->bUseEdgeAsPrimary = true;
				if (!EdgeFilterManager->Init(ExecutionContext, Context->EdgeFilterFactories)) { return false; }
			}

			StartParallelLoopForEdges();
		}

		if (Settings->Mode != EPCGExCutEdgesMode::Edges)
		{
			if (!Context->NodeFilterFactories.IsEmpty())
			{
				NodeFilterManager = MakeShared<PCGExClusterFilter::FManager>(Cluster.ToSharedRef(), VtxDataFacade, EdgeDataFacade);
				if (!NodeFilterManager->Init(ExecutionContext, Context->NodeFilterFactories)) { return false; }
			}

			StartParallelLoopForNodes();
		}

		return true;
	}

	void FProcessor::PrepareSingleLoopScopeForEdges(const uint32 StartIndex, const int32 Count)
	{
		const int32 MaxIndex = StartIndex + Count;

		EdgeDataFacade->Fetch(StartIndex, Count);

		TArray<PCGExGraph::FIndexedEdge>& Edges = *Cluster->Edges;
		if (EdgeFilterManager) { for (int i = StartIndex; i < MaxIndex; i++) { EdgeFilterCache[i] = EdgeFilterManager->Test(Edges[i]); } }
	}

	void FProcessor::ProcessSingleEdge(const int32 EdgeIndex, PCGExGraph::FIndexedEdge& Edge, const int32 LoopIdx, const int32 Count)
	{
		if (EdgeFilterCache[EdgeIndex])
		{
			if (Settings->bInvert) { Edge.bValid = true; }
			return;
		}

		const FVector A1 = VtxDataFacade->Source->GetInPoint(Edge.Start).Transform.GetLocation();
		const FVector B1 = VtxDataFacade->Source->GetInPoint(Edge.End).Transform.GetLocation();
		const FVector Dir = (B1 - A1).GetSafeNormal();

		FBox EdgeBox = FBox(ForceInit);
		EdgeBox += A1;
		EdgeBox += B1;

		for (TSharedPtr<PCGExPaths::FPath> Path : Context->Paths)
		{
			if (!Path->Bounds.Intersect(EdgeBox)) { continue; }

			// Check paths
			Path->GetEdgeOctree()->FindFirstElementWithBoundsTest(
				EdgeBox, [&](const PCGExPaths::FPathEdge* PathEdge)
				{
					if (Settings->bInvert) { if (Edge.bValid) { return false; } }
					else { if (!Edge.bValid) { return false; } }

					if (Context->IntersectionDetails.bUseMinAngle || Context->IntersectionDetails.bUseMaxAngle)
					{
						if (!Context->IntersectionDetails.CheckDot(FMath::Abs(FVector::DotProduct(Path->GetEdgeDir(*PathEdge), Dir))))
						{
							return true;
						}
					}

					const FVector A2 = Path->GetPosUnsafe(PathEdge->Start);
					const FVector B2 = Path->GetPosUnsafe(PathEdge->End);
					FVector A = FVector::ZeroVector;
					FVector B = FVector::ZeroVector;

					FMath::SegmentDistToSegment(A1, B1, A2, B2, A, B);
					if (A == A1 || A == B1 || B == A2 || B == B2) { return true; }

					if (FVector::DistSquared(A, B) >= Context->IntersectionDetails.ToleranceSquared) { return true; }

					PCGExCluster::FNode& StartNode = *(Cluster->Nodes->GetData() + (*Cluster->NodeIndexLookup)[Edge.Start]);
					PCGExCluster::FNode& EndNode = *(Cluster->Nodes->GetData() + (*Cluster->NodeIndexLookup)[Edge.End]);

					if (Settings->bInvert)
					{
						FPlatformAtomics::InterlockedExchange(&Edge.bValid, 1);
						FPlatformAtomics::InterlockedExchange(&StartNode.bValid, 1);
						FPlatformAtomics::InterlockedExchange(&EndNode.bValid, 1);
					}
					else
					{
						FPlatformAtomics::InterlockedExchange(&Edge.bValid, 0);
						if (Settings->bAffectedEdgesAffectEndpoints)
						{
							FPlatformAtomics::InterlockedExchange(&StartNode.bValid, 0);
							FPlatformAtomics::InterlockedExchange(&EndNode.bValid, 0);
						}
					}

					return false;
				});

			if (!Edge.bValid) { return; }
		}
	}

	void FProcessor::PrepareSingleLoopScopeForNodes(const uint32 StartIndex, const int32 Count)
	{
		const int32 MaxIndex = StartIndex + Count;

		TArray<PCGExCluster::FNode>& Nodes = *Cluster->Nodes;
		if (NodeFilterManager) { for (int i = StartIndex; i < MaxIndex; i++) { NodeFilterCache[i] = NodeFilterManager->Test(Nodes[i]); } }
	}

	void FProcessor::ProcessSingleNode(const int32 Index, PCGExCluster::FNode& Node, const int32 LoopIdx, const int32 Count)
	{
		if (NodeFilterCache[Index])
		{
			if (Settings->bInvert) { Node.bValid = true; }
			return;
		}

		const FVector A1 = Cluster->GetPos(Node);

		const FPCGPoint& NodePoint = VtxDataFacade->Source->GetInPoint(Index);
		FBox PointBox = PCGExMath::GetLocalBounds<EPCGExPointBoundsSource::ScaledBounds>(NodePoint).ExpandBy(Settings->NodeExpansion).TransformBy(NodePoint.Transform);

		for (TSharedPtr<PCGExPaths::FPath> Path : Context->Paths)
		{
			if (!Path->Bounds.Intersect(PointBox)) { continue; }

			// Check paths
			Path->GetEdgeOctree()->FindFirstElementWithBoundsTest(
				PointBox, [&](const PCGExPaths::FPathEdge* PathEdge)
				{
					if (Settings->bInvert) { if (Node.bValid) { return false; } }
					else { if (!Node.bValid) { return false; } }

					const FVector A2 = Path->GetPosUnsafe(PathEdge->Start);
					const FVector B2 = Path->GetPosUnsafe(PathEdge->End);

					const FVector B1 = FMath::ClosestPointOnSegment(A1, A2, B2);
					const FVector C1 = PCGExMath::GetSpatializedCenter(Settings->NodeDistanceSettings, NodePoint, A1, B1);

					if (FVector::DistSquared(B1, C1) >= Context->IntersectionDetails.ToleranceSquared) { return true; }

					if (Settings->bInvert)
					{
						FPlatformAtomics::InterlockedExchange(&Node.bValid, 1);
						if (Settings->bAffectedNodesAffectConnectedEdges)
						{
							for (const uint64 Hash : Node.Adjacency)
							{
								FPlatformAtomics::InterlockedExchange(&((Cluster->Edges->GetData() + PCGEx::H64B(Hash)))->bValid, 1);
								FPlatformAtomics::InterlockedExchange(&((Cluster->Nodes->GetData() + PCGEx::H64A(Hash)))->bValid, 1);
							}
						}
					}
					else
					{
						FPlatformAtomics::InterlockedExchange(&Node.bValid, 0);
						if (Settings->bAffectedNodesAffectConnectedEdges)
						{
							for (const uint64 Hash : Node.Adjacency)
							{
								FPlatformAtomics::InterlockedExchange(&((Cluster->Edges->GetData() + PCGEx::H64B(Hash)))->bValid, 0);
							}
						}
					}
					return false;
				});

			if (!Node.bValid) { return; }
		}
	}

	void FProcessor::OnEdgesProcessingComplete()
	{
		FPlatformAtomics::InterlockedExchange(&EdgesProcessed, 1);
		TryConsolidate();
	}

	void FProcessor::OnNodesProcessingComplete()
	{
		FPlatformAtomics::InterlockedExchange(&NodesProcessed, 1);
		TryConsolidate();
	}

	void FProcessor::TryConsolidate()
	{
		if (Settings->bInvert && EdgesProcessed && Settings->bKeepEdgeThatConnectValidNodes)
		{
			StartParallelLoopForRange(Cluster->Edges->Num());
		}
	}

	void FProcessor::ProcessSingleRangeIteration(const int32 Iteration, const int32 LoopIdx, const int32 Count)
	{
		PCGExGraph::FIndexedEdge& Edge = *(Cluster->Edges->GetData() + Iteration);

		if (Edge.bValid) { return; }

		const PCGExCluster::FNode* StartNode = (Cluster->Nodes->GetData() + (*Cluster->NodeIndexLookup)[Edge.Start]);
		const PCGExCluster::FNode* EndNode = (Cluster->Nodes->GetData() + (*Cluster->NodeIndexLookup)[Edge.End]);

		if (StartNode->bValid && EndNode->bValid) { Edge.bValid = true; }
	}

	void FProcessor::CompleteWork()
	{
		TArray<PCGExGraph::FIndexedEdge> ValidEdges;
		Cluster->GetValidEdges(ValidEdges);

		if (ValidEdges.IsEmpty()) { return; }

		GraphBuilder->Graph->InsertEdges(ValidEdges);
	}

	void FProcessorBatch::RegisterBuffersDependencies(PCGExData::FFacadePreloader& FacadePreloader)
	{
		TBatch<FProcessor>::RegisterBuffersDependencies(FacadePreloader);
		PCGEX_TYPED_CONTEXT_AND_SETTINGS(CutEdges)

		PCGExPointFilter::RegisterBuffersDependencies(ExecutionContext, Context->EdgeFilterFactories, FacadePreloader);
		PCGExPointFilter::RegisterBuffersDependencies(ExecutionContext, Context->NodeFilterFactories, FacadePreloader);
	}

	void FProcessorBatch::OnProcessingPreparationComplete()
	{
		PCGEX_TYPED_CONTEXT_AND_SETTINGS(CutEdges)

		TBatch<FProcessor>::OnProcessingPreparationComplete();
	}
}

#undef LOCTEXT_NAMESPACE
#undef PCGEX_NAMESPACE
