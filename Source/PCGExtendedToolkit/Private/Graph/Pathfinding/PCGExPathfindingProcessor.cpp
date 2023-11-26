﻿// Copyright Timothé Lapetite 2023
// Released under the MIT license https://opensource.org/license/MIT/

#include "Graph/Pathfinding/PCGExPathfindingProcessor.h"

#include "PCGPin.h"

#define LOCTEXT_NAMESPACE "PCGExPathfindingSettings"

#pragma region UPCGSettings interface

TArray<FPCGPinProperties> UPCGExPathfindingProcessorSettings::InputPinProperties() const
{
	TArray<FPCGPinProperties> PinProperties = Super::InputPinProperties();

	if (GetRequiresSeeds())
	{
		FPCGPinProperties& PinPropertySeeds = PinProperties.Emplace_GetRef(PCGExGraph::SourceSeedsLabel, EPCGDataType::Point, false, false);

#if WITH_EDITOR
		PinPropertySeeds.Tooltip = LOCTEXT("PCGExSourceSeedsPinTooltip", "Seeds points for pathfinding.");
#endif // WITH_EDITOR
	}

	if (GetRequiresGoals())
	{
		FPCGPinProperties& PinPropertyGoals = PinProperties.Emplace_GetRef(PCGExGraph::SourceGoalsLabel, EPCGDataType::Point, false, false);

#if WITH_EDITOR
		PinPropertyGoals.Tooltip = LOCTEXT("PCGExSourcGoalsPinTooltip", "Goals points for pathfinding.");
#endif // WITH_EDITOR
	}

	return PinProperties;
}

TArray<FPCGPinProperties> UPCGExPathfindingProcessorSettings::OutputPinProperties() const
{
	TArray<FPCGPinProperties> PinProperties;
	FPCGPinProperties& PinPathsOutput = PinProperties.Emplace_GetRef(PCGExGraph::OutputPathsLabel, EPCGDataType::Point);

#if WITH_EDITOR
	PinPathsOutput.Tooltip = LOCTEXT("PCGExOutputPathsTooltip", "Paths output.");
#endif // WITH_EDITOR

	return PinProperties;
}

#pragma endregion

PCGExIO::EInitMode UPCGExPathfindingProcessorSettings::GetPointOutputInitMode() const { return PCGExIO::EInitMode::NoOutput; }
bool UPCGExPathfindingProcessorSettings::GetRequiresSeeds() const { return true; }
bool UPCGExPathfindingProcessorSettings::GetRequiresGoals() const { return true; }

FPCGContext* FPCGExPathfindingProcessorElement::Initialize(
	const FPCGDataCollection& InputData,
	TWeakObjectPtr<UPCGComponent> SourceComponent,
	const UPCGNode* Node)
{
	FPCGExPathfindingProcessorContext* Context = new FPCGExPathfindingProcessorContext();
	InitializeContext(Context, InputData, SourceComponent, Node);
	return Context;
}

bool FPCGExPathfindingProcessorElement::Validate(FPCGContext* InContext) const
{
	if (!FPCGExGraphProcessorElement::Validate(InContext)) { return false; }
	const FPCGExPathfindingProcessorContext* Context = static_cast<FPCGExPathfindingProcessorContext*>(InContext);
	const UPCGExPathfindingProcessorSettings* Settings = InContext->GetInputSettings<UPCGExPathfindingProcessorSettings>();
	check(Settings);

	if (Settings->GetRequiresSeeds() && Context->SeedsPoints->IsEmpty())
	{
		PCGE_LOG(Error, GraphAndLog, LOCTEXT("MissingSeeds", "Missing Input Seeds."));
		return false;
	}

	if (Settings->GetRequiresGoals() && Context->GoalsPoints->IsEmpty())
	{
		PCGE_LOG(Error, GraphAndLog, LOCTEXT("MissingGoals", "Missing Input Goals."));
		return false;
	}

	return true;
}

void FPCGExPathfindingProcessorElement::InitializeContext(
	FPCGExPointsProcessorContext* InContext,
	const FPCGDataCollection& InputData,
	TWeakObjectPtr<UPCGComponent> SourceComponent,
	const UPCGNode* Node) const
{
	FPCGExGraphProcessorElement::InitializeContext(InContext, InputData, SourceComponent, Node);
	FPCGExPathfindingProcessorContext* Context = static_cast<FPCGExPathfindingProcessorContext*>(InContext);

	const UPCGExPathfindingProcessorSettings* Settings = InContext->GetInputSettings<UPCGExPathfindingProcessorSettings>();
	check(Settings);

	Context->PathsPoints = NewObject<UPCGExPointIOGroup>();

	if (Settings->GetRequiresSeeds())
	{
		Context->SeedsPoints = NewObject<UPCGExPointIOGroup>();
		TArray<FPCGTaggedData> Seeds = InContext->InputData.GetInputsByPin(PCGExGraph::SourceSeedsLabel);
		Context->SeedsPoints->Initialize(InContext, Seeds, PCGExIO::EInitMode::NoOutput);
	}

	if (Settings->GetRequiresGoals())
	{
		Context->GoalsPoints = NewObject<UPCGExPointIOGroup>();
		TArray<FPCGTaggedData> Goals = InContext->InputData.GetInputsByPin(PCGExGraph::SourceGoalsLabel);
		Context->GoalsPoints->Initialize(InContext, Goals, PCGExIO::EInitMode::NoOutput);
	}
}


#undef LOCTEXT_NAMESPACE
