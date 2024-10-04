﻿// Copyright Timothé Lapetite 2024
// Released under the MIT license https://opensource.org/license/MIT/

#include "Graph/Edges/PCGExWriteEdgeProperties.h"

#include "Data/Blending/PCGExMetadataBlender.h"


#include "Kismet/KismetMathLibrary.h"

#define LOCTEXT_NAMESPACE "PCGExEdgesToPaths"
#define PCGEX_NAMESPACE WriteEdgeProperties

PCGExData::EInit UPCGExWriteEdgePropertiesSettings::GetMainOutputInitMode() const { return PCGExData::EInit::Forward; }
PCGExData::EInit UPCGExWriteEdgePropertiesSettings::GetEdgeOutputInitMode() const { return PCGExData::EInit::DuplicateInput; }

TArray<FPCGPinProperties> UPCGExWriteEdgePropertiesSettings::InputPinProperties() const
{
	TArray<FPCGPinProperties> PinProperties = Super::InputPinProperties();
	if (bWriteHeuristics) { PCGEX_PIN_PARAMS(PCGExGraph::SourceHeuristicsLabel, "Heuristics that will be computed and written.", Required, {}) }
	return PinProperties;
}

PCGEX_INITIALIZE_ELEMENT(WriteEdgeProperties)

bool FPCGExWriteEdgePropertiesElement::Boot(FPCGExContext* InContext) const
{
	if (!FPCGExEdgesProcessorElement::Boot(InContext)) { return false; }

	PCGEX_CONTEXT_AND_SETTINGS(WriteEdgeProperties)

	PCGEX_FOREACH_FIELD_EDGEEXTRAS(PCGEX_OUTPUT_VALIDATE_NAME)

	return true;
}

bool FPCGExWriteEdgePropertiesElement::ExecuteInternal(
	FPCGContext* InContext) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FPCGExWriteEdgePropertiesElement::Execute);

	PCGEX_CONTEXT_AND_SETTINGS(WriteEdgeProperties)
	PCGEX_EXECUTION_CHECK
	PCGEX_ON_INITIAL_EXECUTION
	{

		if (!Context->StartProcessingClusters<PCGExWriteEdgeProperties::FProcessorBatch>(
			[](const TSharedPtr<PCGExData::FPointIOTaggedEntries>& Entries) { return true; },
			[&](const TSharedPtr<PCGExWriteEdgeProperties::FProcessorBatch>& NewBatch)
			{
				if (Settings->bWriteHeuristics) { NewBatch->SetRequiresHeuristics(true); }
				if (Settings->DirectionSettings.RequiresEndpointsMetadata()) { NewBatch->bRequiresWriteStep = true; }
			}))
		{
			return Context->CancelExecution(TEXT("Could not build any clusters."));
		}
	}

	PCGEX_CLUSTER_BATCH_PROCESSING(PCGEx::State_Done)

	Context->OutputPointsAndEdges();

	return Context->TryComplete();
}


namespace PCGExWriteEdgeProperties
{
	FProcessor::~FProcessor()
	{
	}

	bool FProcessor::Process(TSharedPtr<PCGExMT::FTaskManager> InAsyncManager)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(PCGExWriteEdgeProperties::Process);

		EdgeDataFacade->bSupportsScopedGet = Context->bScopedAttributeGet;

		if (!FClusterProcessor::Process(InAsyncManager)) { return false; }

		if (!DirectionSettings.InitFromParent(ExecutionContext, StaticCastWeakPtr<FProcessorBatch>(ParentBatch).Pin()->DirectionSettings, EdgeDataFacade))
		{
			return false;
		}

		{
			const TSharedRef<PCGExData::FFacade>& OutputFacade = EdgeDataFacade;
			PCGEX_FOREACH_FIELD_EDGEEXTRAS(PCGEX_OUTPUT_INIT)
		}

		bSolidify = Settings->SolidificationAxis != EPCGExMinimalAxis::None;

		if (bSolidify)
		{
#define PCGEX_CREATE_LOCAL_AXIS_SET_CONST(_AXIS) if (Settings->bWriteRadius##_AXIS){Rad##_AXIS##Constant = Settings->Radius##_AXIS##Constant;}
			PCGEX_FOREACH_XYZ(PCGEX_CREATE_LOCAL_AXIS_SET_CONST)
#undef PCGEX_CREATE_LOCAL_AXIS_SET_CONST

			// Create edge-scope getters
#define PCGEX_CREATE_LOCAL_AXIS_GETTER(_AXIS)\
			if (Settings->bWriteRadius##_AXIS && Settings->Radius##_AXIS##Type == EPCGExFetchType::Attribute){\
				SolidificationRad##_AXIS = Settings->Radius##_AXIS##Source == EPCGExGraphValueSource::Edge ? EdgeDataFacade->GetBroadcaster<double>(Settings->Radius##_AXIS##SourceAttribute) : VtxDataFacade->GetBroadcaster<double>(Settings->Radius##_AXIS##SourceAttribute);\
				if (!SolidificationRad##_AXIS){ PCGE_LOG_C(Warning, GraphAndLog, Context, FText::Format(FTEXT("Some edges don't have the specified Radius Attribute \"{0}\"."), FText::FromName(Settings->Radius##_AXIS##SourceAttribute.GetName()))); return false; }}
			PCGEX_FOREACH_XYZ(PCGEX_CREATE_LOCAL_AXIS_GETTER)
#undef PCGEX_CREATE_LOCAL_AXIS_GETTER

			if (Settings->SolidificationLerpOperand == EPCGExFetchType::Attribute)
			{
				SolidificationLerpGetter = EdgeDataFacade->GetBroadcaster<double>(Settings->SolidificationLerpAttribute);
				if (!SolidificationLerpGetter)
				{
					PCGE_LOG_C(Warning, GraphAndLog, ExecutionContext, FText::Format(FTEXT("Some edges don't have the specified SolidificationEdgeLerp Attribute \"{0}\"."), FText::FromName(Settings->SolidificationLerpAttribute.GetName())));
					return false;
				}
			}
		}

		if (Settings->bEndpointsBlending)
		{
			MetadataBlender = MakeUnique<PCGExDataBlending::FMetadataBlender>(const_cast<FPCGExBlendingDetails*>(&Settings->BlendingSettings));
			MetadataBlender->PrepareForData(EdgeDataFacade, VtxDataFacade, PCGExData::ESource::In);
		}

		StartWeight = FMath::Clamp(Settings->EndpointsWeights, 0, 1);
		EndWeight = 1 - StartWeight;


		if (!DirectionSettings.RequiresEndpointsMetadata()) { StartParallelLoopForEdges(); } // Need to wait for data
		return true;
	}

	void FProcessor::PrepareSingleLoopScopeForEdges(const uint32 StartIndex, const int32 Count)
	{
		FClusterProcessor::PrepareSingleLoopScopeForEdges(StartIndex, Count);
		EdgeDataFacade->Fetch(StartIndex, Count);
	}

	void FProcessor::ProcessSingleEdge(const int32 EdgeIndex, PCGExGraph::FIndexedEdge& Edge, const int32 LoopIdx, const int32 Count)
	{
		DirectionSettings.SortEndpoints(Cluster.Get(), Edge);

		const PCGExCluster::FNode& StartNode = *(Cluster->Nodes->GetData() + (*Cluster->NodeIndexLookup)[Edge.Start]);
		const PCGExCluster::FNode& EndNode = *(Cluster->Nodes->GetData() + (*Cluster->NodeIndexLookup)[Edge.End]);

		double BlendWeightStart = StartWeight;
		double BlendWeightEnd = EndWeight;

		const FVector A = Cluster->GetPos(StartNode);
		const FVector B = Cluster->GetPos(EndNode);

		const FVector EdgeDirection = (A - B).GetSafeNormal();
		const double EdgeLength = FVector::Distance(A, B);

		PCGEX_OUTPUT_VALUE(EdgeDirection, Edge.PointIndex, EdgeDirection);
		PCGEX_OUTPUT_VALUE(EdgeLength, Edge.PointIndex, EdgeLength);

		if (Settings->bWriteHeuristics)
		{
			switch (Settings->HeuristicsMode)
			{
			case EPCGExHeuristicsWriteMode::EndpointsOrder:
				PCGEX_OUTPUT_VALUE(Heuristics, Edge.PointIndex, HeuristicsHandler->GetEdgeScore(StartNode, EndNode, Edge, StartNode, EndNode));
				break;
			case EPCGExHeuristicsWriteMode::Smallest:
				PCGEX_OUTPUT_VALUE(
					Heuristics, Edge.PointIndex, FMath::Min(
						HeuristicsHandler->GetEdgeScore(StartNode, EndNode, Edge, StartNode, EndNode),
						HeuristicsHandler->GetEdgeScore(EndNode, StartNode, Edge, EndNode, StartNode)));
				break;
			case EPCGExHeuristicsWriteMode::Highest:
				PCGEX_OUTPUT_VALUE(
					Heuristics, Edge.PointIndex, FMath::Max(
						HeuristicsHandler->GetEdgeScore(StartNode, EndNode, Edge, StartNode, EndNode),
						HeuristicsHandler->GetEdgeScore(EndNode, StartNode, Edge, EndNode, StartNode)));
				break;
			default: ;
			}
		}

		FPCGPoint& MutableTarget = EdgeDataFacade->Source->GetMutablePoint(Edge.PointIndex);

		auto MetadataBlend = [&]()
		{
			const PCGExData::FPointRef Target = EdgeDataFacade->Source->GetOutPointRef(Edge.PointIndex);
			MetadataBlender->PrepareForBlending(Target);
			MetadataBlender->Blend(Target, VtxDataFacade->Source->GetInPointRef(Edge.Start), Target, BlendWeightStart);
			MetadataBlender->Blend(Target, VtxDataFacade->Source->GetInPointRef(Edge.End), Target, BlendWeightEnd);
			MetadataBlender->CompleteBlending(Target, 2, BlendWeightStart + BlendWeightEnd);
		};

		if (bSolidify)
		{
			FRotator EdgeRot;
			FVector Extents = MutableTarget.GetExtents();
			FVector TargetBoundsMin = MutableTarget.BoundsMin;
			FVector TargetBoundsMax = MutableTarget.BoundsMax;

			const double EdgeLerp = FMath::Clamp(SolidificationLerpGetter ? SolidificationLerpGetter->Read(Edge.PointIndex) : Settings->SolidificationLerpConstant, 0, 1);
			const double EdgeLerpInv = 1 - EdgeLerp;
			bool bProcessAxis;

#define PCGEX_SOLIDIFY_DIMENSION(_AXIS)\
				bProcessAxis = Settings->bWriteRadius##_AXIS || Settings->SolidificationAxis == EPCGExMinimalAxis::_AXIS;\
				if (bProcessAxis){\
					if (Settings->SolidificationAxis == EPCGExMinimalAxis::_AXIS){\
						TargetBoundsMin._AXIS = -EdgeLength * EdgeLerpInv;\
						TargetBoundsMax._AXIS = EdgeLength * EdgeLerp;\
					}else{\
						double Rad = Rad##_AXIS##Constant;\
						if(SolidificationRad##_AXIS){\
						if (Settings->Radius##_AXIS##Source == EPCGExGraphValueSource::Vtx) { Rad = FMath::Lerp(SolidificationRad##_AXIS->Read(Edge.Start), SolidificationRad##_AXIS->Read(Edge.End), EdgeLerp); }\
						else { Rad = SolidificationRad##_AXIS->Read(Edge.PointIndex); }}\
						TargetBoundsMin._AXIS = -Rad;\
						TargetBoundsMax._AXIS = Rad;\
					}}

			PCGEX_FOREACH_XYZ(PCGEX_SOLIDIFY_DIMENSION)
#undef PCGEX_SOLIDIFY_DIMENSION

			switch (Settings->SolidificationAxis)
			{
			default:
			case EPCGExMinimalAxis::X:
				EdgeRot = FRotationMatrix::MakeFromX(EdgeDirection).Rotator();
				break;
			case EPCGExMinimalAxis::Y:
				EdgeRot = FRotationMatrix::MakeFromY(EdgeDirection).Rotator();
				break;
			case EPCGExMinimalAxis::Z:
				EdgeRot = FRotationMatrix::MakeFromZ(EdgeDirection).Rotator();
				break;
			}


			BlendWeightStart = EdgeLerp;
			BlendWeightEnd = 1 - EdgeLerp;

			if (MetadataBlender) { MetadataBlend(); } // Blend first THEN apply bounds otherwise it gets overwritten

			MutableTarget.Transform = FTransform(EdgeRot, FMath::Lerp(B, A, EdgeLerp), MutableTarget.Transform.GetScale3D());

			MutableTarget.BoundsMin = TargetBoundsMin;
			MutableTarget.BoundsMax = TargetBoundsMax;
		}
		else if (Settings->bWriteEdgePosition)
		{
			MutableTarget.Transform.SetLocation(FMath::Lerp(B, A, Settings->EdgePositionLerp));
			BlendWeightStart = Settings->EdgePositionLerp;
			BlendWeightEnd = 1 - Settings->EdgePositionLerp;

			if (MetadataBlender) { MetadataBlend(); }
		}
	}

	void FProcessor::CompleteWork()
	{
		if (DirectionSettings.RequiresEndpointsMetadata())
		{
			StartParallelLoopForEdges();
			return;
		}

		EdgeDataFacade->Write(AsyncManager);
	}

	void FProcessor::Write()
	{
		EdgeDataFacade->Write(AsyncManager);
	}

	void FProcessorBatch::OnProcessingPreparationComplete()
	{
		TBatch<FProcessor>::OnProcessingPreparationComplete();

		PCGEX_TYPED_CONTEXT_AND_SETTINGS(WriteEdgeProperties)

		VtxDataFacade->bSupportsScopedGet = Context->bScopedAttributeGet;
		DirectionSettings = Settings->DirectionSettings;

		if (!DirectionSettings.Init(Context, VtxDataFacade))
		{
			PCGE_LOG_C(Warning, GraphAndLog, Context, FTEXT("Some vtx are missing the specified Direction attribute."));
			return;
		}

		if (DirectionSettings.RequiresEndpointsMetadata())
		{
			// Fetch attributes while processors are searching for chains

			const int32 PLI = GetDefault<UPCGExGlobalSettings>()->GetClusterBatchChunkSize();

			PCGEX_ASYNC_GROUP_CHKD_VOID(AsyncManager, FetchVtxTask)
			FetchVtxTask->OnIterationRangeStartCallback =
				[&](const int32 StartIndex, const int32 Count, const int32 LoopIdx)
				{
					VtxDataFacade->Fetch(StartIndex, Count);
				};

			FetchVtxTask->PrepareRangesOnly(VtxDataFacade->GetNum(), PLI);
		}
	}
}

#undef LOCTEXT_NAMESPACE
#undef PCGEX_NAMESPACE
