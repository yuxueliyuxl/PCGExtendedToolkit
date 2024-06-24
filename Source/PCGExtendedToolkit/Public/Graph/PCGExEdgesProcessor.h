﻿// Copyright Timothé Lapetite 2024
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "PCGExCluster.h"
#include "PCGExClusterMT.h"
#include "PCGExPointsProcessor.h"

#include "PCGExEdgesProcessor.generated.h"

class UPCGExNodeStateFactory;

UCLASS(Abstract, BlueprintType, ClassGroup = (Procedural))
class PCGEXTENDEDTOOLKIT_API UPCGExEdgesProcessorSettings : public UPCGExPointsProcessorSettings
{
	GENERATED_BODY()

public:
	//~Begin UPCGSettings interface
#if WITH_EDITOR
	PCGEX_NODE_INFOS(EdgesProcessorSettings, "Edges Processor Settings", "TOOLTIP_TEXT");
	virtual FLinearColor GetNodeTitleColor() const override { return GetDefault<UPCGExGlobalSettings>()->NodeColorEdge; }
#endif

	virtual TArray<FPCGPinProperties> InputPinProperties() const override;
	virtual TArray<FPCGPinProperties> OutputPinProperties() const override;
	//~End UPCGSettings interface

	//~Begin UPCGExPointsProcessorSettings interface
public:
	virtual PCGExData::EInit GetMainOutputInitMode() const override;
	virtual PCGExData::EInit GetEdgeOutputInitMode() const;

	virtual FName GetMainInputLabel() const override;
	virtual FName GetMainOutputLabel() const override;

	virtual FName GetVtxFilterLabel() const;
	virtual FName GetEdgesFilterLabel() const;

	bool SupportsVtxFilters() const;
	bool SupportsEdgesFilters() const;

	virtual bool GetMainAcceptMultipleData() const override;
	//~End UPCGExPointsProcessorSettings interface
};

struct PCGEXTENDEDTOOLKIT_API FPCGExEdgesProcessorContext : public FPCGExPointsProcessorContext
{
	friend class UPCGExEdgesProcessorSettings;
	friend class FPCGExEdgesProcessorElement;

	virtual ~FPCGExEdgesProcessorContext() override;

	bool bDeterministicClusters = false;
	bool bBuildEndpointsLookup = true;

	PCGExData::FPointIOCollection* MainEdges = nullptr;
	PCGExData::FPointIO* CurrentEdges = nullptr;

	PCGExData::FPointIOTaggedDictionary* InputDictionary = nullptr;
	PCGExData::FPointIOTaggedEntries* TaggedEdges = nullptr;
	TMap<uint32, int32> EndpointsLookup;
	TArray<int32> EndpointsAdjacency;

	virtual bool AdvancePointsIO(const bool bCleanupKeys = true) override;
	virtual bool AdvanceEdges(const bool bBuildCluster, const bool bCleanupKeys = true); // Advance edges within current points

	PCGExCluster::FCluster* CurrentCluster = nullptr;
	PCGExCluster::FClusterProjection* ClusterProjection = nullptr;

	void OutputPointsAndEdges();

	template <class InitializeFunc, class LoopBodyFunc>
	bool ProcessCurrentEdges(InitializeFunc&& Initialize, LoopBodyFunc&& LoopBody, bool bForceSync = false) { return Process(Initialize, LoopBody, CurrentEdges->GetNum(), bForceSync); }

	template <class LoopBodyFunc>
	bool ProcessCurrentEdges(LoopBodyFunc&& LoopBody, bool bForceSync = false) { return Process(LoopBody, CurrentEdges->GetNum(), bForceSync); }

	template <class InitializeFunc, class LoopBodyFunc>
	bool ProcessCurrentCluster(InitializeFunc&& Initialize, LoopBodyFunc&& LoopBody, bool bForceSync = false) { return Process(Initialize, LoopBody, CurrentCluster->Nodes->Num(), bForceSync); }

	template <class LoopBodyFunc>
	bool ProcessCurrentCluster(LoopBodyFunc&& LoopBody, bool bForceSync = false) { return Process(LoopBody, CurrentCluster->Nodes->Num(), bForceSync); }

	FPCGExGraphBuilderSettings GraphBuilderSettings;

	bool bWaitingOnClusterProjection = false;

protected:
	virtual bool ProcessClusters();

	TArray<PCGExClusterMT::FClusterProcessorBatchBase*> Batches;

	bool bHasValidHeuristics = false;

	PCGExMT::AsyncState TargetState_ClusterProcessingDone;
	bool bDoClusterBatchGraphBuilding = false;
	bool bDoClusterBatchWritingStep = false;

	bool bClusterRequiresHeuristics = false;
	bool bClusterBatchInlined = false;
	int32 CurrentBatchIndex = -1;
	PCGExClusterMT::FClusterProcessorBatchBase* CurrentBatch = nullptr;


	template <typename T, class ValidateEntriesFunc, class InitBatchFunc>
	bool StartProcessingClusters(ValidateEntriesFunc&& ValidateEntries, InitBatchFunc&& InitBatch, const PCGExMT::AsyncState InState, bool bInlined = false)
	{
		ResetAsyncWork();

		PCGEX_DELETE_TARRAY(Batches)

		bClusterBatchInlined = bInlined;
		CurrentBatchIndex = -1;
		TargetState_ClusterProcessingDone = InState;

		bClusterRequiresHeuristics = true;
		bDoClusterBatchGraphBuilding = false;
		bDoClusterBatchWritingStep = false;
		bBuildEndpointsLookup = false;

		while (AdvancePointsIO(false))
		{
			if (!TaggedEdges)
			{
				PCGE_LOG_C(Warning, GraphAndLog, this, FTEXT("Some input points have no bound edges."));
				continue;
			}

			if (!ValidateEntries(TaggedEdges)) { continue; }

			T* NewBatch = new T(this, CurrentIO, TaggedEdges->Entries);
			InitBatch(NewBatch);

			if (NewBatch->RequiresHeuristics())
			{
				bClusterRequiresHeuristics = true;

				if (!bHasValidHeuristics)
				{
					PCGEX_DELETE(NewBatch)
					continue;
				}
			}

			if (NewBatch->bRequiresWriteStep)
			{
				bDoClusterBatchWritingStep = true;
			}

			NewBatch->EdgeCollection = MainEdges;
			if (VtxFiltersData) { NewBatch->SetVtxFilterData(VtxFiltersData); }

			if (NewBatch->RequiresGraphBuilder())
			{
				bDoClusterBatchGraphBuilding = true;
				NewBatch->GraphBuilderSettings = GraphBuilderSettings;
			}

			Batches.Add(NewBatch);

			PCGExClusterMT::ScheduleBatch(GetAsyncManager(), NewBatch);
		}

		if (Batches.IsEmpty()) { return false; }

		if (bClusterBatchInlined) { AdvanceBatch(); }
		else { SetAsyncState(PCGExClusterMT::MTState_ClusterProcessing); }
		return true;
	}

	bool HasValidHeuristics() const;

	void AdvanceBatch();

	int32 CurrentEdgesIndex = -1;

	UPCGExNodeStateFactory* VtxFiltersData = nullptr;
	UPCGExNodeStateFactory* EdgesFiltersData = nullptr;
};

class PCGEXTENDEDTOOLKIT_API FPCGExEdgesProcessorElement : public FPCGExPointsProcessorElement
{
public:
	virtual FPCGContext* Initialize(
		const FPCGDataCollection& InputData,
		TWeakObjectPtr<UPCGComponent> SourceComponent,
		const UPCGNode* Node) override;

	virtual void DisabledPassThroughData(FPCGContext* Context) const override;

protected:
	virtual bool Boot(FPCGContext* InContext) const override;
	virtual FPCGContext* InitializeContext(
		FPCGExPointsProcessorContext* InContext,
		const FPCGDataCollection& InputData,
		TWeakObjectPtr<UPCGComponent> SourceComponent,
		const UPCGNode* Node) const override;
};
