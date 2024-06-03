﻿// Copyright Timothé Lapetite 2024
// Released under the MIT license https://opensource.org/license/MIT/

#include "Sampling/Neighbors/PCGExNeighborSampleAttribute.h"

#define LOCTEXT_NAMESPACE "PCGExCreateNeighborSample"
#define PCGEX_NAMESPACE PCGExCreateNeighborSample

void UPCGExNeighborSampleAttribute::PrepareForCluster(const FPCGContext* InContext, PCGExCluster::FCluster* InCluster)
{
	Super::PrepareForCluster(InContext, InCluster);

	PCGEX_DELETE(Blender)
	bIsValidOperation = false;

	if (SourceAttributes.IsEmpty())
	{
		PCGE_LOG_C(Warning, GraphAndLog, InContext, FTEXT("No source attribute set."));
		return;
	}

	// Prepare blender settings
	PCGEx::FAttributesInfos* AttributesInfos = PCGEx::FAttributesInfos::Get(GetSourceIO().GetIn()->Metadata);
	MetadataBlendingSettings = FPCGExBlendingSettings(EPCGExDataBlendingType::None);
	MetadataBlendingSettings.BlendingFilter = EPCGExBlendingFilter::Include;

	TSet<FName> MissingAttributes;
	AttributesInfos->FindMissing(SourceAttributes, MissingAttributes);

	if (MissingAttributes.Num() == SourceAttributes.Num())
	{
		PCGEX_DELETE(AttributesInfos)
		PCGE_LOG_C(Error, GraphAndLog, InContext, FText::Format(FTEXT("Missing all source attribute(s) on Sampler {0}."), GetClass()->GetName()));
		return;
	}
	
	for (const FName& Id : SourceAttributes)
	{
		if (MissingAttributes.Contains(Id))
		{
			PCGE_LOG_C(Warning, GraphAndLog, InContext, FText::Format(FTEXT("Missing source attribute: {0}."), FText::FromName(Id)));
			continue;
		}

		MetadataBlendingSettings.AttributesOverrides.Add(Id, Blending);
		MetadataBlendingSettings.FilteredAttributes.Add(Id);
	}

	PCGEX_DELETE(AttributesInfos)

	if (MetadataBlendingSettings.FilteredAttributes.IsEmpty())
	{
		PCGE_LOG_C(Error, GraphAndLog, InContext, FText::Format(FTEXT("Missing all source attribute(s) on Sampler {0}."), GetClass()->GetName()));
		return;
	}

	Blender = new PCGExDataBlending::FMetadataBlender(&MetadataBlendingSettings);
	bIsValidOperation = true;
}

void UPCGExNeighborSampleAttribute::PrepareNode(PCGExCluster::FNode& TargetNode) const
{
	Blender->PrepareForBlending(TargetNode.PointIndex);
}

void UPCGExNeighborSampleAttribute::BlendNodePoint(PCGExCluster::FNode& TargetNode, const PCGExCluster::FNode& OtherNode, const double Weight) const
{
	const int32 PrimaryIndex = TargetNode.PointIndex;
	Blender->Blend(PrimaryIndex, OtherNode.PointIndex, PrimaryIndex, Weight);
}

void UPCGExNeighborSampleAttribute::BlendNodeEdge(PCGExCluster::FNode& TargetNode, const int32 InEdgeIndex, const double Weight) const
{
	const int32 PrimaryIndex = TargetNode.PointIndex;
	Blender->Blend(PrimaryIndex, InEdgeIndex, PrimaryIndex, Weight);
}

void UPCGExNeighborSampleAttribute::FinalizeNode(PCGExCluster::FNode& TargetNode, const int32 Count, const double TotalWeight) const
{
	const int32 PrimaryIndex = TargetNode.PointIndex;
	Blender->CompleteBlending(PrimaryIndex, Count, TotalWeight);
}

void UPCGExNeighborSampleAttribute::FinalizeOperation()
{
	Super::FinalizeOperation();
	Blender->Write();
	PCGEX_DELETE(Blender)
}

void UPCGExNeighborSampleAttribute::Cleanup()
{
	PCGEX_DELETE(Blender)
	Super::Cleanup();
}

#if WITH_EDITOR
FString UPCGExNeighborSampleAttributeSettings::GetDisplayName() const
{
	if (Descriptor.SourceAttributes.IsEmpty()) { return TEXT(""); }
	TArray<FName> Names = Descriptor.SourceAttributes.Array();

	if (Names.Num() == 1) { return Names[0].ToString(); }
	if (Names.Num() == 2) { return Names[0].ToString() + TEXT(" (+1 other)"); }

	return Names[0].ToString() + FString::Printf(TEXT(" (+%d others)"), (Names.Num() - 1));
}
#endif

UPCGExNeighborSampleOperation* UPCGNeighborSamplerFactoryAttribute::CreateOperation() const
{
	UPCGExNeighborSampleAttribute* NewOperation = NewObject<UPCGExNeighborSampleAttribute>();

	PCGEX_SAMPLER_CREATE

	NewOperation->SourceAttributes = Descriptor.SourceAttributes;
	NewOperation->Blending = Descriptor.Blending;

	return NewOperation;
}

UPCGExParamFactoryBase* UPCGExNeighborSampleAttributeSettings::CreateFactory(FPCGContext* InContext, UPCGExParamFactoryBase* InFactory) const
{
	UPCGNeighborSamplerFactoryAttribute* SamplerFactory = NewObject<UPCGNeighborSamplerFactoryAttribute>();
	SamplerFactory->Descriptor = Descriptor;

	return Super::CreateFactory(InContext, SamplerFactory);
}


#undef LOCTEXT_NAMESPACE
#undef PCGEX_NAMESPACE
