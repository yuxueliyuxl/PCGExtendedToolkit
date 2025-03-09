﻿// Copyright 2025 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Layout/PCGExBinPacking.h"


#include "Layout/PCGExLayout.h"

#define LOCTEXT_NAMESPACE "PCGExBinPackingElement"
#define PCGEX_NAMESPACE BinPacking

bool UPCGExBinPackingSettings::GetSortingRules(FPCGExContext* InContext, TArray<FPCGExSortRuleConfig>& OutRules) const
{
	OutRules.Append(PCGExSorting::GetSortingRules(InContext, PCGExSorting::SourceSortingRules));
	return !OutRules.IsEmpty();
}

TArray<FPCGPinProperties> UPCGExBinPackingSettings::InputPinProperties() const
{
	TArray<FPCGPinProperties> PinProperties = Super::InputPinProperties();
	PCGEX_PIN_POINTS(PCGExLayout::SourceBinsLabel, "List of bins to fit input points into. Each input collection is expected to have a matching collection of bins.", Required, {})
	PCGEX_PIN_FACTORIES(PCGExSorting::SourceSortingRules, "Plug sorting rules here. Order is defined by each rule' priority value, in ascending order.", Normal, {})
	return PinProperties;
}

TArray<FPCGPinProperties> UPCGExBinPackingSettings::OutputPinProperties() const
{
	TArray<FPCGPinProperties> PinProperties = Super::OutputPinProperties();
	PCGEX_PIN_POINTS(PCGExLayout::OutputBinsLabel, "Input bins, with added statistics.", Required, {})
	PCGEX_PIN_POINTS(PCGExLayout::OutputDiscardedLabel, "Discarded points, one that could not fit into any bin.", Required, {})
	return PinProperties;
}

PCGEX_INITIALIZE_ELEMENT(BinPacking)

bool FPCGExBinPackingElement::Boot(FPCGExContext* InContext) const
{
	if (!FPCGExPointsProcessorElement::Boot(InContext)) { return false; }

	PCGEX_CONTEXT_AND_SETTINGS(BinPacking)

	Context->Bins = MakeShared<PCGExData::FPointIOCollection>(InContext, PCGExLayout::SourceBinsLabel, PCGExData::EIOInit::None);
	Context->Bins->OutputPin = PCGExLayout::OutputBinsLabel;

	int32 NumBins = Context->Bins->Num();
	int32 NumInputs = Context->MainPoints->Num();

	if (NumBins != NumInputs)
	{
		if (NumBins > NumInputs)
		{
			NumBins = NumInputs;
			if (!Settings->bQuietTooManyBinsWarning)
			{
				PCGE_LOG_C(Error, GraphAndLog, InContext, FTEXT("There are more bins than there are inputs. Extra bins will be ignored."));
			}

			for (int i = 0; i < NumInputs; i++)
			{
				Context->MainPoints->Pairs[i]->InitializeOutput(PCGExData::EIOInit::Duplicate);
				Context->Bins->Pairs[i]->InitializeOutput(PCGExData::EIOInit::Duplicate);
			}
		}
		else if (NumInputs > NumBins)
		{
			NumInputs = NumBins;
			if (!Settings->bQuietTooFewBinsWarning)
			{
				PCGE_LOG_C(Error, GraphAndLog, InContext, FTEXT("There are more inputs than there are bins. Extra inputs will be ignored."));
			}

			for (int i = 0; i < NumBins; i++)
			{
				Context->MainPoints->Pairs[i]->InitializeOutput(PCGExData::EIOInit::Duplicate);
				Context->Bins->Pairs[i]->InitializeOutput(PCGExData::EIOInit::Duplicate);
			}
		}
	}
	else
	{
		for (int i = 0; i < NumInputs; i++)
		{
			Context->MainPoints->Pairs[i]->InitializeOutput(PCGExData::EIOInit::Duplicate);
			Context->Bins->Pairs[i]->InitializeOutput(PCGExData::EIOInit::Duplicate);
		}
	}

	for (int i = 0; i < NumBins; i++) { Context->Bins->Pairs[i]->OutputPin = Context->Bins->OutputPin; }

	Context->Discarded = MakeShared<PCGExData::FPointIOCollection>(InContext);
	Context->Discarded->OutputPin = PCGExLayout::OutputDiscardedLabel;

	return true;
}

bool FPCGExBinPackingElement::ExecuteInternal(FPCGContext* InContext) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FPCGExBinPackingElement::Execute);

	PCGEX_CONTEXT_AND_SETTINGS(BinPacking)
	PCGEX_EXECUTION_CHECK
	PCGEX_ON_INITIAL_EXECUTION
	{
		if (!Context->StartBatchProcessingPoints<PCGExPointsMT::TBatch<PCGExBinPacking::FProcessor>>(
			[&](const TSharedPtr<PCGExData::FPointIO>& Entry)
			{
				return Entry->GetOut() != nullptr;
			},
			[&](const TSharedPtr<PCGExPointsMT::TBatch<PCGExBinPacking::FProcessor>>& NewBatch)
			{
				TArray<FPCGExSortRuleConfig> OutRules;
				Settings->GetSortingRules(Context, OutRules);
				NewBatch->bPrefetchData = !OutRules.IsEmpty();
			}))
		{
			return Context->CancelExecution(TEXT("Could not find any points to process."));
		}
	}

	PCGEX_POINTS_BATCH_PROCESSING(PCGEx::State_Done)

	Context->MainPoints->StageOutputs();
	Context->Bins->StageOutputs();
	Context->Discarded->StageOutputs();

	return Context->TryComplete();
}

namespace PCGExBinPacking
{
	void FBin::AddSpace(const FBox& InBox)
	{
		FSpace& NewSpace = Spaces.Emplace_GetRef(InBox, Seed);
		NewSpace.DistanceScore /= MaxDist;
	}

	FBin::FBin(const FPCGPoint& InBinPoint, const FVector& InSeed)
	{
		Seed = InSeed;
		Bounds = PCGExMath::GetLocalBounds<EPCGExPointBoundsSource::ScaledBounds>(InBinPoint);

		Transform = InBinPoint.Transform;
		Transform.SetScale3D(FVector::OneVector); // Reset scale for later transform

		MaxVolume = Bounds.GetVolume();
		FVector FurthestLocation = InSeed;

		for (int C = 0; C < 3; C++)
		{
			const double DistToMin = FMath::Abs(Seed[C] - Bounds.Min[C]);
			const double DistToMax = FMath::Abs(Seed[C] - Bounds.Max[C]);
			FurthestLocation[C] = (DistToMin > DistToMax) ? Bounds.Min[C] : Bounds.Max[C];
		}

		MaxDist = FVector::DistSquared(FurthestLocation, Seed);

		AddSpace(Bounds);
	}

	int32 FBin::GetBestSpaceScore(const FItem& InItem, double& OutScore, FRotator& OutRotator) const
	{
		int32 BestIndex = -1;
		const double BoxVolume = InItem.Box.GetVolume();
		const FVector ItemSize = InItem.Box.GetSize();

		for (int i = 0; i < Spaces.Num(); i++)
		{
			const FSpace& Space = Spaces[i];

			// TODO : Rotate & try fit

			if (Space.CanFit(ItemSize))
			{
				const double SpaceScore = 1 - ((Space.Volume - BoxVolume) / MaxVolume);
				const double DistScore = Space.DistanceScore;
				const double Score = SpaceScore + DistScore;

				if (Score < OutScore)
				{
					BestIndex = i;
					OutScore = Score;
				}
			}
		}

		return BestIndex;
	}

	void FBin::AddItem(int32 SpaceIndex, FItem& InItem)
	{
		Items.Add(InItem);

		const FSpace& Space = Spaces[SpaceIndex];

		const FVector ItemSize = InItem.Box.GetSize();
		FVector ItemMin = Space.Box.Min;

		for (int C = 0; C < 3; C++) { ItemMin[C] = FMath::Clamp(Seed[C] - ItemSize[C] * 0.5, Space.Box.Min[C], Space.Box.Max[C] - ItemSize[C]); }

		FBox ItemBox = FBox(ItemMin, ItemMin + ItemSize);
		InItem.Box = ItemBox;

		Space.Expand(ItemBox, InItem.Padding);

		if (Settings->bAvoidWastedSpace)
		{
			const FVector Amplitude = Space.Inflate(ItemBox, WastedSpaceThresholds);
		}

		TArray<FBox> NewPartitions;
		NewPartitions.Reserve(6);

		NewPartitions.Emplace(Space.Box.Min, FVector(ItemBox.Min.X, Space.Box.Max.Y, Space.Box.Max.Z));                                          // Left
		NewPartitions.Emplace(FVector(ItemBox.Max.X, Space.Box.Min.Y, Space.Box.Min.Z), Space.Box.Max);                                          // Right
		NewPartitions.Emplace(FVector(ItemBox.Min.X, Space.Box.Min.Y, Space.Box.Min.Z), FVector(ItemBox.Max.X, Space.Box.Max.Y, ItemBox.Min.Z)); // Bottom
		NewPartitions.Emplace(FVector(ItemBox.Min.X, ItemBox.Min.Y, ItemBox.Max.Z), FVector(ItemBox.Max.X, ItemBox.Max.Y, Space.Box.Max.Z));     // Top
		NewPartitions.Emplace(FVector(ItemBox.Min.X, ItemBox.Max.Y, ItemBox.Min.Z), FVector(ItemBox.Max.X, Space.Box.Max.Y, Space.Box.Max.Z));   // Front
		NewPartitions.Emplace(FVector(ItemBox.Min.X, Space.Box.Min.Y, ItemBox.Min.Z), FVector(ItemBox.Max.X, ItemBox.Min.Y, Space.Box.Max.Z));   // Back

		Spaces.RemoveAt(SpaceIndex);
		Spaces.Reserve(Spaces.Num() + 8);

		for (const FBox& Partition : NewPartitions) { if (!FMath::IsNearlyZero(Partition.GetVolume())) { AddSpace(Partition); } }
	}

	bool FBin::Insert(FItem& InItem)
	{
		FRotator OutRotation = FRotator::ZeroRotator;
		double OutScore = MAX_dbl;

		const int32 BestIndex = GetBestSpaceScore(InItem, OutScore, OutRotation);

		if (BestIndex == -1) { return false; }

		// TODO : Check other bins as well, while it fits, this one may not be the ideal candidate

		AddItem(BestIndex, InItem);

		return true;
	}

	void FBin::UpdatePoint(FPCGPoint& InPoint, const FItem& InItem) const
	{
		const FTransform T = FTransform(FQuat::Identity, InItem.Box.GetCenter() - InPoint.GetLocalCenter(), InPoint.Transform.GetScale3D());
		InPoint.Transform = T * Transform;
	}

	void FProcessor::RegisterBuffersDependencies(PCGExData::FFacadePreloader& FacadePreloader)
	{
		TPointsProcessor::RegisterBuffersDependencies(FacadePreloader);

		TArray<FPCGExSortRuleConfig> RuleConfigs;
		if (Settings->GetSortingRules(ExecutionContext, RuleConfigs) && !RuleConfigs.IsEmpty())
		{
			Sorter = MakeShared<PCGExSorting::PointSorter<true>>(Context, PointDataFacade, RuleConfigs);
			Sorter->SortDirection = Settings->SortDirection;
			Sorter->RegisterBuffersDependencies(FacadePreloader);
		}
	}

	bool FProcessor::Process(const TSharedPtr<PCGExMT::FTaskManager>& InAsyncManager)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(PCGExBinPacking::Process);

		PointDataFacade->bSupportsScopedGet = Context->bScopedAttributeGet;

		if (!FPointsProcessor::Process(InAsyncManager)) { return false; }

		if (Settings->OccupationPaddingInput == EPCGExInputValueType::Attribute)
		{
			PaddingBuffer = PointDataFacade->GetScopedBroadcaster<FVector>(Settings->OccupationPaddingAttribute);
			if (!PaddingBuffer)
			{
				PCGE_LOG_C(Error, GraphAndLog, Context, FText::Format(FTEXT("Could not find occupation attribute : {0}."), FText::FromName(Settings->OccupationPaddingAttribute.GetName())));
				return false;
			}
		}

		Fitted.Init(false, PointDataFacade->GetNum());

		TSharedPtr<PCGExData::FPointIO> TargetBins = Context->Bins->Pairs[BatchIndex];
		Bins.Reserve(TargetBins->GetNum());

		bool bRelativeSeed = Settings->SeedMode == EPCGExBinSeedMode::UVWConstant;
		TSharedPtr<PCGEx::TAttributeBroadcaster<FVector>> SeedGetter = MakeShared<PCGEx::TAttributeBroadcaster<FVector>>();
		if (Settings->SeedMode == EPCGExBinSeedMode::PositionAttribute)
		{
			if (!SeedGetter->Prepare(Settings->SeedPositionAttribute, TargetBins.ToSharedRef()))
			{
				PCGE_LOG_C(Error, GraphAndLog, Context, FText::Format(FTEXT("A bin pool is missing the seed position attribute : {0}."), FText::FromName(Settings->SeedPositionAttribute.GetName())));
				return false;
			}
		}
		else if (Settings->SeedMode == EPCGExBinSeedMode::UVWAttribute)
		{
			bRelativeSeed = true;
			if (!SeedGetter->Prepare(Settings->SeedUVWAttribute, TargetBins.ToSharedRef()))
			{
				PCGE_LOG_C(Error, GraphAndLog, Context, FText::Format(FTEXT("A bin pool is missing the seed UVW attribute : {0}."), FText::FromName(Settings->SeedUVWAttribute.GetName())));
				return false;
			}
		}
		else
		{
			SeedGetter.Reset();
		}


		if (Sorter && Sorter->Init())
		{
			PointDataFacade->GetOut()->GetMutablePoints().Sort([&](const FPCGPoint& A, const FPCGPoint& B) { return Sorter->Sort(A, B); });
		}

		if (Settings->bAvoidWastedSpace)
		{
			MinOccupation = MAX_dbl;
			const TArray<FPCGPoint>& InPoints = PointDataFacade->GetOut()->GetPoints();
			for (const FPCGPoint& P : InPoints)
			{
				const FVector Size = PCGExMath::GetLocalBounds<EPCGExPointBoundsSource::ScaledBounds>(P).GetSize();
				MinOccupation = FMath::Min(MinOccupation, FMath::Min3(Size.X, Size.Y, Size.Z));
			}
		}

		for (int i = 0; i < TargetBins->GetNum(); i++)
		{
			const FPCGPoint& BinPoint = TargetBins->GetInPoint(i);

			FVector Seed = FVector::ZeroVector;
			if (bRelativeSeed)
			{
				FBox Box = PCGExMath::GetLocalBounds<EPCGExPointBoundsSource::ScaledBounds>(BinPoint);
				Seed = Box.GetCenter() + (SeedGetter ? SeedGetter->SoftGet(i, BinPoint, FVector::ZeroVector) : Settings->SeedUVW) * Box.GetExtent();
			}
			else
			{
				Seed = BinPoint.Transform.InverseTransformPositionNoScale(SeedGetter ? SeedGetter->SoftGet(i, BinPoint, FVector::ZeroVector) : Settings->SeedPosition);
			}

			PCGEX_MAKE_SHARED(NewBin, FBin, BinPoint, Seed)

			NewBin->Settings = Settings;
			NewBin->WastedSpaceThresholds = FVector(MinOccupation);

			Bins.Add(NewBin);
		}

		// OPTIM : Find the smallest bound dimension possible to use as min threshold for free spaces later on

		StartParallelLoopForPoints();

		return true;
	}

	void FProcessor::PrepareSingleLoopScopeForPoints(const PCGExMT::FScope& Scope)
	{
		PointDataFacade->Fetch(Scope);
	}

	void FProcessor::ProcessSinglePoint(const int32 Index, FPCGPoint& Point, const PCGExMT::FScope& Scope)
	{
		FItem Item = FItem();

		Item.Index = Index;
		Item.Box = FBox(FVector::ZeroVector, PCGExMath::GetLocalBounds<EPCGExPointBoundsSource::ScaledBounds>(Point).GetSize());
		Item.Padding = PaddingBuffer ? PaddingBuffer->Read(Index) : Settings->OccupationPadding;

		bool bPlaced = false;
		for (const TSharedPtr<FBin>& Bin : Bins)
		{
			if (Bin->Insert(Item))
			{
				bPlaced = true;
				Bin->UpdatePoint(Point, Item);
				break;
			}
		}

		Fitted[Index] = bPlaced;
		if (!bPlaced) { bHasUnfitted = true; }

		// TODO : post process pass to move things around based on initial placement
	}

	void FProcessor::CompleteWork()
	{
		if (bHasUnfitted)
		{
			const TArray<FPCGPoint>& SourcePoints = PointDataFacade->GetIn()->GetPoints();
			TArray<FPCGPoint>& FittedPoints = PointDataFacade->GetMutablePoints();
			TArray<FPCGPoint>& DiscardedPoints = Context->Discarded->Emplace_GetRef(PointDataFacade->GetIn(), PCGExData::EIOInit::New)->GetMutablePoints();
			const int32 NumPoints = PointDataFacade->GetNum();
			int32 WriteIndex = 0;

			DiscardedPoints.Reserve(NumPoints);

			for (int32 i = 0; i < NumPoints; i++)
			{
				if (Fitted[i]) { FittedPoints[WriteIndex++] = FittedPoints[i]; }
				else { DiscardedPoints.Add(SourcePoints[i]); }
			}

			FittedPoints.SetNum(WriteIndex);
			DiscardedPoints.Shrink();
		}
	}
}

#undef LOCTEXT_NAMESPACE
#undef PCGEX_NAMESPACE
