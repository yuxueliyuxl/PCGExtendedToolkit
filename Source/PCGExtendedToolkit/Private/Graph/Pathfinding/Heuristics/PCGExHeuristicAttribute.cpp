﻿// Copyright Timothé Lapetite 2024
// Released under the MIT license https://opensource.org/license/MIT/

#include "Graph/Pathfinding/Heuristics/PCGExHeuristicAttribute.h"

#include "Graph/Pathfinding/PCGExPathfinding.h"

#define LOCTEXT_NAMESPACE "PCGExCreateHeuristicAttribute"
#define PCGEX_NAMESPACE CreateHeuristicAttribute

void UPCGExHeuristicAttribute::PrepareForCluster(const PCGExCluster::FCluster* InCluster)
{
	Super::PrepareForCluster(InCluster);

	PCGExData::FPointIO* InPoints = Source == EPCGExGraphValueSource::Vtx ? InCluster->VtxIO : InCluster->EdgesIO;
	PCGExData::FFacade* DataCache = Source == EPCGExGraphValueSource::Vtx ? PrimaryDataCache : SecondaryDataCache;

	if (LastPoints == InPoints) { return; }

	const int32 NumPoints = Source == EPCGExGraphValueSource::Vtx ? InCluster->Nodes->Num() : InPoints->GetNum();

	LastPoints = InPoints;
	InPoints->CreateInKeys();
	CachedScores.SetNumZeroed(NumPoints);

	PCGExData::FCache<double>* ModifiersCache = DataCache->GetOrCreateGetter<double>(Attribute, true);

	if (!ModifiersCache)
	{
		PCGE_LOG_C(Error, GraphAndLog, Context, FText::Format(FTEXT("Invalid Heuristic attribute: {0}."), FText::FromName(Attribute.GetName())));
		return;
	}

	const double MinValue = ModifiersCache->Min;
	const double MaxValue = ModifiersCache->Max;

	const double OutMin = bInvert ? 1 : 0;
	const double OutMax = bInvert ? 0 : 1;

	const double Factor = ReferenceWeight * WeightFactor;

	if (Source == EPCGExGraphValueSource::Vtx)
	{
		for (const PCGExCluster::FNode& Node : (*InCluster->Nodes))
		{
			const double NormalizedValue = PCGExMath::Remap(ModifiersCache->Values[Node.PointIndex], MinValue, MaxValue, OutMin, OutMax);
			CachedScores[Node.NodeIndex] += FMath::Max(0, ScoreCurveObj->GetFloatValue(NormalizedValue)) * Factor;
		}
	}
	else
	{
		for (int i = 0; i < NumPoints; i++)
		{
			const double NormalizedValue = PCGExMath::Remap(ModifiersCache->Values[i], MinValue, MaxValue, OutMin, OutMax);
			CachedScores[i] += FMath::Max(0, ScoreCurveObj->GetFloatValue(NormalizedValue)) * Factor;
		}
	}
}

double UPCGExHeuristicAttribute::GetGlobalScore(
	const PCGExCluster::FNode& From,
	const PCGExCluster::FNode& Seed,
	const PCGExCluster::FNode& Goal) const
{
	return 0;
}

double UPCGExHeuristicAttribute::GetEdgeScore(
	const PCGExCluster::FNode& From,
	const PCGExCluster::FNode& To,
	const PCGExGraph::FIndexedEdge& Edge,
	const PCGExCluster::FNode& Seed,
	const PCGExCluster::FNode& Goal) const
{
	return CachedScores[Source == EPCGExGraphValueSource::Edge ? Edge.PointIndex : To.NodeIndex];
}

void UPCGExHeuristicAttribute::Cleanup()
{
	CachedScores.Empty();
	Super::Cleanup();
}

UPCGExHeuristicOperation* UPCGHeuristicsFactoryAttribute::CreateOperation() const
{
	UPCGExHeuristicAttribute* NewOperation = NewObject<UPCGExHeuristicAttribute>();
	PCGEX_FORWARD_HEURISTIC_DESCRIPTOR
	NewOperation->Attribute = Descriptor.Attribute;
	return NewOperation;
}

UPCGExParamFactoryBase* UPCGExCreateHeuristicAttributeSettings::CreateFactory(FPCGContext* InContext, UPCGExParamFactoryBase* InFactory) const
{
	UPCGHeuristicsFactoryAttribute* NewFactory = NewObject<UPCGHeuristicsFactoryAttribute>();
	PCGEX_FORWARD_HEURISTIC_FACTORY
	return Super::CreateFactory(InContext, NewFactory);
}

#if WITH_EDITOR
FString UPCGExCreateHeuristicAttributeSettings::GetDisplayName() const
{
	return Descriptor.Attribute.GetName().ToString()
		+ TEXT(" @ ")
		+ FString::Printf(TEXT("%.3f"), (static_cast<int32>(1000 * Descriptor.WeightFactor) / 1000.0));
}
#endif

#undef LOCTEXT_NAMESPACE
#undef PCGEX_NAMESPACE
