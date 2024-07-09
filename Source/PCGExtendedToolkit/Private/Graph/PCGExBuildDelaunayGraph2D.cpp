﻿// Copyright Timothé Lapetite 2024
// Released under the MIT license https://opensource.org/license/MIT/

#include "Graph/PCGExBuildDelaunayGraph2D.h"

#include "Elements/Metadata/PCGMetadataElementCommon.h"
#include "Geometry/PCGExGeoDelaunay.h"
#include "Graph/PCGExCluster.h"
#include "Graph/Data/PCGExClusterData.h"

#define LOCTEXT_NAMESPACE "PCGExGraph"
#define PCGEX_NAMESPACE BuildDelaunayGraph2D

namespace PCGExGeoTask
{
	class FLloydRelax2;
}

PCGExData::EInit UPCGExBuildDelaunayGraph2DSettings::GetMainOutputInitMode() const { return PCGExData::EInit::NoOutput; }

FPCGExBuildDelaunayGraph2DContext::~FPCGExBuildDelaunayGraph2DContext()
{
	PCGEX_TERMINATE_ASYNC
	SitesIOMap.Empty();
	PCGEX_DELETE(MainSites)
}

TArray<FPCGPinProperties> UPCGExBuildDelaunayGraph2DSettings::OutputPinProperties() const
{
	TArray<FPCGPinProperties> PinProperties = Super::OutputPinProperties();
	PCGEX_PIN_POINTS(PCGExGraph::OutputEdgesLabel, "Point data representing edges.", Required, {})
	if (bOutputSites) { PCGEX_PIN_POINTS(PCGExGraph::OutputSitesLabel, "Complete delaunay sites.", Required, {}) }
	return PinProperties;
}

PCGEX_INITIALIZE_ELEMENT(BuildDelaunayGraph2D)

bool FPCGExBuildDelaunayGraph2DElement::Boot(FPCGContext* InContext) const
{
	if (!FPCGExPointsProcessorElement::Boot(InContext)) { return false; }

	PCGEX_CONTEXT_AND_SETTINGS(BuildDelaunayGraph2D)

	PCGEX_VALIDATE_NAME(Settings->HullAttributeName)

	if (Settings->bOutputSites)
	{
		if (Settings->bMarkSiteHull) { PCGEX_VALIDATE_NAME(Settings->SiteHullAttributeName) }
		Context->MainSites = new PCGExData::FPointIOCollection();
		Context->MainSites->DefaultOutputLabel = PCGExGraph::OutputSitesLabel;
	}

	return true;
}

bool FPCGExBuildDelaunayGraph2DElement::ExecuteInternal(
	FPCGContext* InContext) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FPCGExBuildDelaunayGraph2DElement::Execute);

	PCGEX_CONTEXT_AND_SETTINGS(BuildDelaunayGraph2D)

	if (Context->IsSetup())
	{
		if (!Boot(Context)) { return true; }

		bool bInvalidInputs = false;

		if (!Context->StartBatchProcessingPoints<PCGExPointsMT::TBatch<PCGExBuildDelaunay2D::FProcessor>>(
			[&](PCGExData::FPointIO* Entry)
			{
				if (Entry->GetNum() < 3)
				{
					bInvalidInputs = true;
					return false;
				}

				if (Context->MainSites)
				{
					PCGExData::FPointIO* SitesIO = Context->MainSites->Emplace_GetRef(Entry, PCGExData::EInit::NoOutput);
					Context->SitesIOMap.Add(Entry, SitesIO);
				}

				return true;
			},
			[&](PCGExPointsMT::TBatch<PCGExBuildDelaunay2D::FProcessor>* NewBatch)
			{
				NewBatch->bRequiresWriteStep = true;
			},
			PCGExMT::State_Done))
		{
			PCGE_LOG(Warning, GraphAndLog, FTEXT("Could not find any points to build from."));
			return true;
		}

		if (bInvalidInputs)
		{
			PCGE_LOG(Warning, GraphAndLog, FTEXT("Some inputs have less than 3 points and won't be processed."));
		}
	}

	if (!Context->ProcessPointsBatch()) { return false; }

	if (Context->IsDone())
	{
		Context->OutputMainPoints();
	}

	return Context->TryComplete();
}

namespace PCGExBuildDelaunay2D
{
	FProcessor::~FProcessor()
	{
		PCGEX_DELETE(Delaunay)

		PCGEX_DELETE(GraphBuilder)

		PCGEX_DELETE(HullMarkPointWriter)
	}

	bool FProcessor::Process(PCGExMT::FTaskManager* AsyncManager)
	{
		PCGEX_TYPED_CONTEXT_AND_SETTINGS(BuildDelaunayGraph2D)

		if (!FPointsProcessor::Process(AsyncManager)) { return false; }

		ProjectionDetails = Settings->ProjectionDetails;
		ProjectionDetails.Init(Context, PointDataFacade);

		// Build delaunay

		TArray<FVector> ActivePositions;
		PCGExGeo::PointsToPositions(PointIO->GetIn()->GetPoints(), ActivePositions);

		Delaunay = new PCGExGeo::TDelaunay2();

		if (!Delaunay->Process(ActivePositions, ProjectionDetails))
		{
			PCGE_LOG_C(Warning, GraphAndLog, Context, FTEXT("Some inputs generated invalid results."));
			PCGEX_DELETE(Delaunay)
			return false;
		}

		PointIO->InitializeOutput<UPCGExClusterNodesData>(PCGExData::EInit::DuplicateInput);

		if (Settings->bUrquhart) { Delaunay->RemoveLongestEdges(ActivePositions); }
		if (Settings->bMarkHull) { HullMarkPointWriter = new PCGEx::TFAttributeWriter<bool>(Settings->HullAttributeName, false, false); }

		ActivePositions.Empty();

		GraphBuilder = new PCGExGraph::FGraphBuilder(PointIO, &Settings->GraphBuilderDetails);
		GraphBuilder->Graph->InsertEdges(Delaunay->DelaunayEdges, -1);

		if (Settings->bOutputSites)
		{
			if (Settings->bMergeUrquhartSites) { AsyncManagerPtr->Start<FOutputDelaunayUrquhartSites2D>(BatchIndex, PointIO, this); }
			else { AsyncManagerPtr->Start<FOutputDelaunaySites2D>(BatchIndex, PointIO, this); }
		}
		GraphBuilder->CompileAsync(AsyncManagerPtr);

		if (!Settings->bMarkHull) { PCGEX_DELETE(Delaunay) }

		return true;
	}

	void FProcessor::ProcessSinglePoint(const int32 Index, FPCGPoint& Point, const int32 LoopIdx, const int32 Count)
	{
		HullMarkPointWriter->Values[Index] = Delaunay->DelaunayHull.Contains(Index);
	}

	void FProcessor::CompleteWork()
	{
		PCGEX_TYPED_CONTEXT_AND_SETTINGS(BuildDelaunayGraph2D)

		if (!GraphBuilder) { return; }

		if (!GraphBuilder->bCompiledSuccessfully)
		{
			PointIO->InitializeOutput(PCGExData::EInit::NoOutput);
			PCGEX_DELETE(GraphBuilder)
			PCGEX_DELETE(HullMarkPointWriter)
			return;
		}

		GraphBuilder->Write(Context);

		if (HullMarkPointWriter)
		{
			HullMarkPointWriter->BindAndSetNumUninitialized(PointIO);
			StartParallelLoopForPoints();
		}
	}

	void FProcessor::Write()
	{
		if (!GraphBuilder) { return; }
		if (HullMarkPointWriter) { PCGEX_ASYNC_WRITE_DELETE(AsyncManagerPtr, HullMarkPointWriter) }
	}

	bool FOutputDelaunaySites2D::ExecuteTask()
	{
		FPCGExBuildDelaunayGraph2DContext* Context = Manager->GetContext<FPCGExBuildDelaunayGraph2DContext>();
		PCGEX_SETTINGS(BuildDelaunayGraph2D)

		PCGExData::FPointIO* SitesIO = Context->SitesIOMap[PointIO];
		SitesIO->InitializeOutput(PCGExData::EInit::NewOutput);

		const TArray<FPCGPoint>& OriginalPoints = SitesIO->GetIn()->GetPoints();
		TArray<FPCGPoint>& MutablePoints = SitesIO->GetOut()->GetMutablePoints();
		PCGExGeo::TDelaunay2* Delaunay = Processor->Delaunay;
		const int32 NumSites = Delaunay->Sites.Num();

		PCGEX_SET_NUM_UNINITIALIZED(MutablePoints, NumSites)
		for (int i = 0; i < NumSites; i++)
		{
			const PCGExGeo::FDelaunaySite2& Site = Delaunay->Sites[i];

			FVector Centroid = OriginalPoints[Site.Vtx[0]].Transform.GetLocation();
			Centroid += OriginalPoints[Site.Vtx[1]].Transform.GetLocation();
			Centroid += OriginalPoints[Site.Vtx[2]].Transform.GetLocation();
			Centroid /= 3;

			MutablePoints[i] = OriginalPoints[Site.Vtx[0]];
			MutablePoints[i].Transform.SetLocation(Centroid);
		}

		if (Settings->bMarkSiteHull)
		{
			PCGEx::TFAttributeWriter<bool>* HullWriter = new PCGEx::TFAttributeWriter<bool>(Settings->SiteHullAttributeName);
			HullWriter->BindAndSetNumUninitialized(SitesIO);
			for (int i = 0; i < NumSites; i++) { HullWriter->Values[i] = Delaunay->Sites[i].bOnHull; }
			PCGEX_ASYNC_WRITE_DELETE(Manager, HullWriter);
		}

		return true;
	}

	bool FOutputDelaunayUrquhartSites2D::ExecuteTask()
	{
		FPCGExBuildDelaunayGraph2DContext* Context = Manager->GetContext<FPCGExBuildDelaunayGraph2DContext>();
		PCGEX_SETTINGS(BuildDelaunayGraph2D)

		PCGExData::FPointIO* SitesIO = Context->SitesIOMap[PointIO];
		SitesIO->InitializeOutput(PCGExData::EInit::NewOutput);

		const TArray<FPCGPoint>& OriginalPoints = SitesIO->GetIn()->GetPoints();
		TArray<FPCGPoint>& MutablePoints = SitesIO->GetOut()->GetMutablePoints();
		PCGExGeo::TDelaunay2* Delaunay = Processor->Delaunay;
		const int32 NumSites = Delaunay->Sites.Num();

		/*
		int32 NumSites = 0;
		for(PCGExGeo::FDelaunaySite2& Site : Delaunay->Sites)
		{
			const uint64 EdgeA = PCGEx::H64U(Site.Vtx[0], Site.Vtx[1]); 
			const uint64 EdgeB = PCGEx::H64U(Site.Vtx[1], Site.Vtx[2]); 
			const uint64 EdgeC = PCGEx::H64U(Site.Vtx[0], Site.Vtx[2]);
			
		}
		*/

		PCGEX_SET_NUM_UNINITIALIZED(MutablePoints, NumSites)
		for (int i = 0; i < NumSites; i++)
		{
			const PCGExGeo::FDelaunaySite2& Site = Delaunay->Sites[i];

			FVector Centroid = OriginalPoints[Site.Vtx[0]].Transform.GetLocation();
			Centroid += OriginalPoints[Site.Vtx[1]].Transform.GetLocation();
			Centroid += OriginalPoints[Site.Vtx[2]].Transform.GetLocation();
			Centroid /= 3;

			MutablePoints[i] = OriginalPoints[Site.Vtx[0]];
			MutablePoints[i].Transform.SetLocation(Centroid);
		}

		if (Settings->bMarkSiteHull)
		{
			PCGEx::TFAttributeWriter<bool>* HullWriter = new PCGEx::TFAttributeWriter<bool>(Settings->SiteHullAttributeName);
			HullWriter->BindAndSetNumUninitialized(SitesIO);
			for (int i = 0; i < NumSites; i++) { HullWriter->Values[i] = Delaunay->Sites[i].bOnHull; }
			PCGEX_ASYNC_WRITE_DELETE(Manager, HullWriter);
		}

		return true;
	}
}

#undef LOCTEXT_NAMESPACE
#undef PCGEX_NAMESPACE
