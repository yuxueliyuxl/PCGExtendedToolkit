﻿// Copyright Timothé Lapetite 2024
// Released under the MIT license https://opensource.org/license/MIT/

#include "Sampling/PCGExSampleNeighbors.h"

#include "Graph/Pathfinding/PCGExPathfinding.h"
#include "Sampling/Neighbors/PCGExNeighborSampleFactoryProvider.h"

#define LOCTEXT_NAMESPACE "PCGExSampleNeighbors"
#define PCGEX_NAMESPACE SampleNeighbors

UPCGExSampleNeighborsSettings::UPCGExSampleNeighborsSettings(
	const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

TArray<FPCGPinProperties> UPCGExSampleNeighborsSettings::InputPinProperties() const
{
	TArray<FPCGPinProperties> PinProperties = Super::InputPinProperties();
	PCGEX_PIN_PARAMS(PCGExNeighborSample::SourceSamplersLabel, "Neighbor samplers.", Required, {})
	return PinProperties;
}

PCGExData::EInit UPCGExSampleNeighborsSettings::GetEdgeOutputInitMode() const { return PCGExData::EInit::Forward; }
PCGExData::EInit UPCGExSampleNeighborsSettings::GetMainOutputInitMode() const { return PCGExData::EInit::DuplicateInput; }

bool FPCGExSampleNeighborsContext::PrepareSettings(
	FPCGExBlendingSettings& OutSettings,
	TArray<UPCGExNeighborSampleOperation*>& OutOperations,
	const PCGExData::FPointIO& FromPointIO,
	EPCGExGraphValueSource Source) const
{
	PCGEx::FAttributesInfos* AttributesInfos = PCGEx::FAttributesInfos::Get(FromPointIO.GetIn()->Metadata);

	OutSettings = FPCGExBlendingSettings(EPCGExDataBlendingType::None);
	OutSettings.BlendingFilter = EPCGExBlendingFilter::Include;

	OutOperations.Empty();

	TArray<PCGEx::FAttributeIdentity> OutIdentities;
	PCGEx::FAttributeIdentity::Get(FromPointIO.GetIn()->Metadata, OutIdentities);

	for (UPCGExNeighborSampleOperation* Operation : SamplingOperations)
	{
		if (Operation->BaseSettings.NeighborSource != Source) { continue; }

		if (Operation->SourceAttributes.IsEmpty())
		{
			PCGE_LOG_C(Warning, GraphAndLog, this, FTEXT("No source attribute set."));
			continue;
		}

		TSet<FName> MissingAttributes;
		const bool HasMissingAttributes = AttributesInfos->FindMissing(Operation->SourceAttributes, MissingAttributes);

		if (MissingAttributes.Num() == Operation->SourceAttributes.Num())
		{
			PCGE_LOG_C(Warning, GraphAndLog, this, FTEXT("Missing all source attribute."));
			continue;
		}

		for (const FName& Id : Operation->SourceAttributes)
		{
			if (MissingAttributes.Contains(Id))
			{
				PCGE_LOG_C(Warning, GraphAndLog, this, FText::Format(FTEXT("Missing source attribute: {0}."), FText::FromName(Id)));
				continue;
			}

			OutSettings.AttributesOverrides.Add(Id, Operation->Blending);
			OutSettings.FilteredAttributes.Add(Id);
		}

		OutOperations.Add(Operation);
	}

	return !OutSettings.FilteredAttributes.IsEmpty();
	PCGEX_DELETE(AttributesInfos)
}

PCGEX_INITIALIZE_ELEMENT(SampleNeighbors)

FPCGExSampleNeighborsContext::~FPCGExSampleNeighborsContext()
{
	PCGEX_TERMINATE_ASYNC

	for (UPCGExNeighborSampleOperation* Operation : SamplingOperations)
	{
		Operation->Cleanup();
		PCGEX_DELETE_UOBJECT(Operation)
	}

	SamplingOperations.Empty();

	PCGEX_DELETE(BlenderFromPoints)
	PCGEX_DELETE(BlenderFromEdges)
}

bool FPCGExSampleNeighborsElement::Boot(FPCGContext* InContext) const
{
	if (!FPCGExEdgesProcessorElement::Boot(InContext)) { return false; }

	PCGEX_CONTEXT_AND_SETTINGS(SampleNeighbors)

	TArray<UPCGNeighborSamplerFactoryBase*> SamplerFactories;

	if (!PCGExFactories::GetInputFactories(InContext, PCGExNeighborSample::SourceSamplersLabel, SamplerFactories, {PCGExFactories::EType::Sampler}, false))
	{
		PCGE_LOG(Warning, GraphAndLog, FTEXT("No valid sampler found."));
		return false;
	}

	// Sort samplers so higher priorities come last, as they have to potential to override values.
	SamplerFactories.Sort([&](const UPCGNeighborSamplerFactoryBase& A, const UPCGNeighborSamplerFactoryBase& B) { return A.Priority < B.Priority; });

	for (const UPCGNeighborSamplerFactoryBase* OperationFactory : SamplerFactories)
	{
		for (const FName& Id : OperationFactory->Descriptor.SourceAttributes)
		{
			if (!PCGEx::IsValidName(Id)) { PCGE_LOG(Warning, GraphAndLog, FTEXT("A source sampler contains invalid source attributes.")); }
		}

		UPCGExNeighborSampleOperation* Operation = OperationFactory->CreateOperation();
		Context->SamplingOperations.Add(Operation);
		Context->RegisterOperation<UPCGExNeighborSampleOperation>(Operation);
	}

	if(Context->SamplingOperations.IsEmpty())
	{
		PCGE_LOG(Warning, GraphAndLog, FTEXT("Could not find any valid samplers."));
		return false;
	}

	return true;
}

bool FPCGExSampleNeighborsElement::ExecuteInternal(
	FPCGContext* InContext) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FPCGExSampleNeighborsElement::Execute);

	PCGEX_CONTEXT_AND_SETTINGS(SampleNeighbors)

	if (Context->IsSetup())
	{
		if (!Boot(Context)) { return true; }
		Context->SetState(PCGExMT::State_ReadyForNextPoints);
	}

	if (Context->IsState(PCGExMT::State_ReadyForNextPoints))
	{
		Context->PointPointBlendingSettings = FPCGExBlendingSettings(EPCGExDataBlendingType::None);

		PCGEX_DELETE(Context->BlenderFromPoints)

		if (!Context->AdvancePointsIO()) { Context->Done(); }
		else
		{
			if (!Context->TaggedEdges)
			{
				PCGE_LOG(Warning, GraphAndLog, FTEXT("Some input points have no associated edges."));
				Context->SetState(PCGExMT::State_ReadyForNextPoints);
				return false;
			}

			if (Context->PrepareSettings(
				Context->PointPointBlendingSettings,
				Context->PointPointOperations,
				*Context->CurrentIO,
				EPCGExGraphValueSource::Point))
			{
				Context->BlenderFromPoints = new PCGExDataBlending::FMetadataBlender(&Context->PointPointBlendingSettings);
				Context->BlenderFromPoints->PrepareForData(*Context->CurrentIO);
			}

			Context->SetState(PCGExGraph::State_ReadyForNextEdges);
		}
	}

	if (Context->IsState(PCGExGraph::State_ReadyForNextEdges))
	{
		Context->PointEdgeBlendingSettings = FPCGExBlendingSettings(EPCGExDataBlendingType::None);

		PCGEX_DELETE(Context->BlenderFromEdges)

		if (!Context->AdvanceEdges(true))
		{
			if (Context->BlenderFromPoints) { Context->BlenderFromPoints->Write(); }

			Context->SetState(PCGExMT::State_ReadyForNextPoints);
			return false;
		}

		if (!Context->CurrentCluster) { return false; } // Corrupted or invalid cluster

		if (Context->PrepareSettings(
			Context->PointEdgeBlendingSettings,
			Context->PointEdgeOperations,
			*Context->CurrentEdges,
			EPCGExGraphValueSource::Edge))
		{
			Context->BlenderFromEdges = new PCGExDataBlending::FMetadataBlender(&Context->PointEdgeBlendingSettings);
			Context->BlenderFromEdges->PrepareForData(*Context->CurrentIO, *Context->CurrentEdges);
		}

		if (!Context->BlenderFromPoints && !Context->BlenderFromEdges) { return false; } // Nothing to blend

		Context->SetState(PCGExSampleNeighbors::State_ReadyForNextOperation);
	}

	if (Context->IsState(PCGExSampleNeighbors::State_ReadyForNextOperation))
	{
		const int32 NextIndex = Context->CurrentOperation ? Context->SamplingOperations.Find(Context->CurrentOperation) + 1 : 0;
		
		if (Context->SamplingOperations.IsValidIndex(NextIndex))
		{
			Context->CurrentOperation = Context->SamplingOperations[NextIndex];
			Context->SetState(PCGExSampleNeighbors::State_Sampling);
		}
		else
		{
			if (Context->BlenderFromEdges) { Context->BlenderFromEdges->Write(); }
			Context->SetState(PCGExGraph::State_ReadyForNextEdges);
		}
	}

	if (Context->IsState(PCGExSampleNeighbors::State_Sampling))
	{
		auto Initialize = [&]()
		{
			if (Context->CurrentOperation->BaseSettings.NeighborSource == EPCGExGraphValueSource::Point) { Context->CurrentOperation->Blender = Context->BlenderFromPoints; }
			else { Context->CurrentOperation->Blender = Context->BlenderFromEdges; }

			Context->CurrentOperation->PrepareForCluster(Context->CurrentCluster);
		};

		auto ProcessNodePoints = [&](const int32 NodeIndex) { Context->CurrentOperation->ProcessNodeForPoints(NodeIndex); };
		auto ProcessNodeEdges = [&](const int32 NodeIndex) { Context->CurrentOperation->ProcessNodeForEdges(NodeIndex); };

		if (Context->CurrentOperation->BaseSettings.NeighborSource == EPCGExGraphValueSource::Point)
		{
			if (!Context->Process(Initialize, ProcessNodePoints, Context->CurrentCluster->Nodes.Num(), true)) { return false; }
		}
		else
		{
			if (!Context->Process(Initialize, ProcessNodeEdges, Context->CurrentCluster->Nodes.Num(), true)) { return false; }
		}

		Context->SetState(PCGExSampleNeighbors::State_ReadyForNextOperation);
	}

	if (Context->IsDone())
	{
		Context->OutputPointsAndEdges();
	}

	return Context->IsDone();
}

bool FPCGExSampleNeighborTask::ExecuteTask()
{
	const FPCGExSampleNeighborsContext* Context = Manager->GetContext<FPCGExSampleNeighborsContext>();

	for (const UPCGExNeighborSampleOperation* Operation : Context->PointPointOperations) { Operation->ProcessNodeForPoints(TaskIndex); }
	for (const UPCGExNeighborSampleOperation* Operation : Context->PointEdgeOperations) { Operation->ProcessNodeForEdges(TaskIndex); }

	return true;
}

#undef LOCTEXT_NAMESPACE
#undef PCGEX_NAMESPACE
