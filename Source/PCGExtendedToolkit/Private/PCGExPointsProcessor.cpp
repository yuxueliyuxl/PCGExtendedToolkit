﻿// Copyright Timothé Lapetite 2024
// Released under the MIT license https://opensource.org/license/MIT/

#include "PCGExPointsProcessor.h"

#include "PCGPin.h"
#include "Data/PCGExData.h"
#include "Data/PCGExDataFilter.h"
#include "Helpers/PCGSettingsHelpers.h"

#define LOCTEXT_NAMESPACE "PCGExGraphSettings"


#pragma region Loops

PCGExData::FPointIO& PCGEx::FAPointLoop::GetPointIO() const { return PointIO ? *PointIO : *Context->CurrentIO; }

bool PCGEx::FPointLoop::Advance(const TFunction<void(PCGExData::FPointIO&)>&& Initialize, const TFunction<void(const int32, const PCGExData::FPointIO&)>&& LoopBody)
{
	if (CurrentIndex == -1)
	{
		PCGExData::FPointIO& PtIO = GetPointIO();
		Initialize(PtIO);
		NumIterations = PtIO.GetNum();
		CurrentIndex = 0;
	}
	return Advance(std::move(LoopBody));
}

bool PCGEx::FPointLoop::Advance(const TFunction<void(const int32, const PCGExData::FPointIO&)>&& LoopBody)
{
	const PCGExData::FPointIO& PtIO = GetPointIO();
	if (CurrentIndex == -1)
	{
		NumIterations = PtIO.GetNum();
		CurrentIndex = 0;
	}
	const int32 ChunkNumIterations = FMath::Min(NumIterations - CurrentIndex, GetCurrentChunkSize());
	if (ChunkNumIterations > 0)
	{
		for (int i = 0; i < ChunkNumIterations; i++) { LoopBody(CurrentIndex + i, PtIO); }
		CurrentIndex += ChunkNumIterations;
	}
	if (CurrentIndex >= NumIterations)
	{
		CurrentIndex = -1;
		return true;
	}
	return false;
}

bool PCGEx::FAsyncPointLoop::Advance(const TFunction<void(PCGExData::FPointIO&)>&& Initialize, const TFunction<void(const int32, const PCGExData::FPointIO&)>&& LoopBody)
{
	if (!bAsyncEnabled) { return FPointLoop::Advance(std::move(Initialize), std::move(LoopBody)); }

	PCGExData::FPointIO& PtIO = GetPointIO();
	NumIterations = PtIO.GetNum();
	return FPCGAsync::AsyncProcessingOneToOneEx(
		&(Context->AsyncState), NumIterations, [&]()
		{
			Initialize(PtIO);
		}, [&](const int32 ReadIndex, const int32 WriteIndex)
		{
			LoopBody(ReadIndex, PtIO);
			return true;
		}, true, ChunkSize);
}

bool PCGEx::FAsyncPointLoop::Advance(const TFunction<void(const int32, const PCGExData::FPointIO&)>&& LoopBody)
{
	if (!bAsyncEnabled) { return FPointLoop::Advance(std::move(LoopBody)); }

	const PCGExData::FPointIO& PtIO = GetPointIO();
	NumIterations = PtIO.GetNum();
	return FPCGAsync::AsyncProcessingOneToOneEx(
		&(Context->AsyncState), NumIterations, []()
		{
		}, [&](const int32 ReadIndex, const int32 WriteIndex)
		{
			LoopBody(ReadIndex, PtIO);
			return true;
		}, true, ChunkSize);
}

#pragma endregion

#pragma region UPCGSettings interface

UPCGExPointsProcessorSettings::UPCGExPointsProcessorSettings(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

#if WITH_EDITOR
void UPCGExPointsProcessorSettings::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
}
#endif

TArray<FPCGPinProperties> UPCGExPointsProcessorSettings::InputPinProperties() const
{
	TArray<FPCGPinProperties> PinProperties;

	if (GetMainAcceptMultipleData()) { PCGEX_PIN_POINTS(GetMainInputLabel(), "The point data to be processed.", Required, {}) }
	else { PCGEX_PIN_POINT(GetMainInputLabel(), "The point data to be processed.", Required, {}) }

	return PinProperties;
}

TArray<FPCGPinProperties> UPCGExPointsProcessorSettings::OutputPinProperties() const
{
	TArray<FPCGPinProperties> PinProperties;
	PCGEX_PIN_POINTS(GetMainOutputLabel(), "The processed points.", Required, {})
	return PinProperties;
}

bool UPCGExPointsProcessorSettings::OnlyPassThroughOneEdgeWhenDisabled() const
{
	return false;
	//return Super::OnlyPassThroughOneEdgeWhenDisabled();
}

FName UPCGExPointsProcessorSettings::GetMainOutputLabel() const { return PCGEx::OutputPointsLabel; }
FName UPCGExPointsProcessorSettings::GetMainInputLabel() const { return PCGEx::SourcePointsLabel; }

bool UPCGExPointsProcessorSettings::GetMainAcceptMultipleData() const { return true; }

PCGExData::EInit UPCGExPointsProcessorSettings::GetMainOutputInitMode() const { return PCGExData::EInit::NewOutput; }

FName UPCGExPointsProcessorSettings::GetPointFilterLabel() const { return NAME_None; }
bool UPCGExPointsProcessorSettings::SupportsPointFilters() const { return !GetPointFilterLabel().IsNone(); }
bool UPCGExPointsProcessorSettings::RequiresPointFilters() const { return false; }

int32 UPCGExPointsProcessorSettings::GetPreferredChunkSize() const { return PCGExMT::GAsyncLoop_M; }

FPCGExPointsProcessorContext::~FPCGExPointsProcessorContext()
{
	PCGEX_TERMINATE_ASYNC

	for (UPCGExOperation* Operation : ProcessorOperations)
	{
		Operation->Cleanup();
		if (OwnedProcessorOperations.Contains(Operation)) { PCGEX_DELETE_UOBJECT(Operation) }
	}

	ProcessorOperations.Empty();
	OwnedProcessorOperations.Empty();

	PCGEX_DELETE(MainPoints)

	CurrentIO = nullptr;
	World = nullptr;
}

bool FPCGExPointsProcessorContext::AdvancePointsIO(const bool bCleanupKeys)
{
	if (bCleanupKeys && CurrentIO) { CurrentIO->CleanupKeys(); }

	if (MainPoints->Pairs.IsValidIndex(++CurrentPointIOIndex))
	{
		CurrentIO = MainPoints->Pairs[CurrentPointIOIndex];
		return true;
	}

	CurrentIO = nullptr;
	return false;
}

bool FPCGExPointsProcessorContext::ExecuteAutomation() { return true; }

void FPCGExPointsProcessorContext::Done() { SetState(PCGExMT::State_Done); }

void FPCGExPointsProcessorContext::PostProcessOutputs()
{
	PCGEX_SETTINGS_LOCAL(PointsProcessor)
	if (Settings->bFlattenOutput)
	{
		TSet<uint64> InputUIDs;
		InputUIDs.Reserve(OutputData.TaggedData.Num());
		for (FPCGTaggedData& OutTaggedData : OutputData.TaggedData) { if (const UPCGSpatialData* SpatialData = Cast<UPCGSpatialData>(OutTaggedData.Data)) { InputUIDs.Add(SpatialData->UID); } }

		for (FPCGTaggedData& OutTaggedData : OutputData.TaggedData)
		{
			if (const UPCGSpatialData* SpatialData = Cast<UPCGSpatialData>(OutTaggedData.Data); SpatialData && !InputUIDs.Contains(SpatialData->UID)) { SpatialData->Metadata->Flatten(); }
		}
	}
}

void FPCGExPointsProcessorContext::SetState(const PCGExMT::AsyncState OperationId, const bool bResetAsyncWork)
{
	FReadScopeLock ReadScopeLock(StateLock);
	if (bResetAsyncWork) { ResetAsyncWork(); }
	if (CurrentState == OperationId) { return; }
	CurrentState = OperationId;
}

#pragma endregion

bool FPCGExPointsProcessorContext::ProcessCurrentPoints(TFunction<void(PCGExData::FPointIO&)>&& Initialize, TFunction<void(const int32, const PCGExData::FPointIO&)>&& LoopBody, const bool bForceSync)
{
	return bForceSync ? ChunkedPointLoop.Advance(std::move(Initialize), std::move(LoopBody)) : AsyncPointLoop.Advance(std::move(Initialize), std::move(LoopBody));
}

bool FPCGExPointsProcessorContext::ProcessCurrentPoints(TFunction<void(const int32, const PCGExData::FPointIO&)>&& LoopBody, const bool bForceSync)
{
	return bForceSync ? ChunkedPointLoop.Advance(std::move(LoopBody)) : AsyncPointLoop.Advance(std::move(LoopBody));
}

PCGExData::FPointIO* FPCGExPointsProcessorContext::TryGetSingleInput(const FName InputName, const bool bThrowError) const
{
	PCGExData::FPointIO* SingleIO = nullptr;
	const PCGExData::FPointIOCollection* Collection = new PCGExData::FPointIOCollection(this, InputName);
	if (!Collection->Pairs.IsEmpty())
	{
		SingleIO = new PCGExData::FPointIO(Collection->Pairs[0]->GetIn(), InputName);
	}
	else if (bThrowError)
	{
		PCGE_LOG_C(Error, GraphAndLog, this, FText::Format(FText::FromString(TEXT("Missing {0} inputs")), FText::FromName(InputName)));
	}

	PCGEX_DELETE(Collection)
	return SingleIO;
}

FPCGTaggedData* FPCGExPointsProcessorContext::Output(UPCGData* OutData, const FName OutputLabel)
{
	FWriteScopeLock WriteLock(ContextLock);
	FPCGTaggedData& OutputRef = OutputData.TaggedData.Emplace_GetRef();
	OutputRef.Data = OutData;
	OutputRef.Pin = OutputLabel;
	return &OutputRef;
}

bool FPCGExPointsProcessorContext::ProcessPointsBatch()
{
	if (BatchablePoints.IsEmpty()) { return true; }

	if (IsState(PCGExPointsMT::State_WaitingOnPointsProcessing))
	{
		if (!IsAsyncWorkComplete()) { return false; }

		CompleteBatch(GetAsyncManager(), MainBatch);
		SetAsyncState(PCGExPointsMT::State_WaitingOnPointsCompletedWork);
	}

	if (IsState(PCGExPointsMT::State_WaitingOnPointsCompletedWork))
	{
		if (!IsAsyncWorkComplete()) { return false; }
		SetState(State_PointsProcessingDone);
	}

	return true;
}

FPCGExAsyncManager* FPCGExPointsProcessorContext::GetAsyncManager()
{
	if (!AsyncManager)
	{
		FWriteScopeLock WriteLock(ContextLock);
		AsyncManager = new FPCGExAsyncManager();
		AsyncManager->bForceSync = !bDoAsyncProcessing;
		AsyncManager->Context = this;
	}
	return AsyncManager;
}

void FPCGExPointsProcessorContext::ResetAsyncWork() { if (AsyncManager) { AsyncManager->Reset(); } }

bool FPCGExPointsProcessorContext::IsAsyncWorkComplete()
{
	if (!bDoAsyncProcessing || !AsyncManager) { return true; }
	if (AsyncManager->IsAsyncWorkComplete())
	{
		ResetAsyncWork();
		return true;
	}
	return false;
}

FPCGContext* FPCGExPointsProcessorElementBase::Initialize(
	const FPCGDataCollection& InputData,
	TWeakObjectPtr<UPCGComponent> SourceComponent,
	const UPCGNode* Node)
{
	FPCGExPointsProcessorContext* Context = new FPCGExPointsProcessorContext();
	InitializeContext(Context, InputData, SourceComponent, Node);
	return Context;
}

void FPCGExPointsProcessorElementBase::DisabledPassThroughData(FPCGContext* Context) const
{
	//FPCGPointProcessingElementBase::DisabledPassThroughData(Context);

	const UPCGExPointsProcessorSettings* Settings = Context->GetInputSettings<UPCGExPointsProcessorSettings>();
	check(Settings);

	//Forward main points
	TArray<FPCGTaggedData> MainSources = Context->InputData.GetInputsByPin(Settings->GetMainInputLabel());
	for (const FPCGTaggedData& TaggedData : MainSources)
	{
		FPCGTaggedData& TaggedDataCopy = Context->OutputData.TaggedData.Emplace_GetRef();
		TaggedDataCopy.Data = TaggedData.Data;
		TaggedDataCopy.Tags.Append(TaggedData.Tags);
		TaggedDataCopy.Pin = Settings->GetMainOutputLabel();
	}
}

FPCGContext* FPCGExPointsProcessorElementBase::InitializeContext(
	FPCGExPointsProcessorContext* InContext,
	const FPCGDataCollection& InputData,
	TWeakObjectPtr<UPCGComponent> SourceComponent,
	const UPCGNode* Node) const
{
	InContext->InputData = InputData;
	InContext->SourceComponent = SourceComponent;
	InContext->Node = Node;

	check(SourceComponent.IsValid());
	InContext->World = SourceComponent->GetWorld();

	const UPCGExPointsProcessorSettings* Settings = InContext->GetInputSettings<UPCGExPointsProcessorSettings>();
	check(Settings);

	InContext->SetState(PCGExMT::State_Setup);
	InContext->bDoAsyncProcessing = Settings->bDoAsyncProcessing;
	InContext->ChunkSize = FMath::Max((Settings->ChunkSize <= 0 ? Settings->GetPreferredChunkSize() : Settings->ChunkSize), 1);

	InContext->AsyncLoop = InContext->MakeLoop<PCGExMT::FAsyncParallelLoop>();

	InContext->ChunkedPointLoop = InContext->MakeLoop<PCGEx::FPointLoop>();
	InContext->AsyncPointLoop = InContext->MakeLoop<PCGEx::FAsyncPointLoop>();

	InContext->MainPoints = new PCGExData::FPointIOCollection();
	InContext->MainPoints->DefaultOutputLabel = Settings->GetMainOutputLabel();

	if (!Settings->bEnabled) { return InContext; }

	if (Settings->GetMainAcceptMultipleData())
	{
		TArray<FPCGTaggedData> Sources = InContext->InputData.GetInputsByPin(Settings->GetMainInputLabel());
		InContext->MainPoints->Initialize(InContext, Sources, Settings->GetMainOutputInitMode());
	}
	else
	{
		TArray<FPCGTaggedData> Sources = InContext->InputData.GetInputsByPin(Settings->GetMainInputLabel());
		const UPCGPointData* InData = nullptr;
		const FPCGTaggedData* Source = nullptr;
		int32 SrcIndex = -1;

		while (!InData && Sources.IsValidIndex(++SrcIndex))
		{
			InData = PCGExData::GetMutablePointData(InContext, Sources[SrcIndex]);
			if (InData && !InData->GetPoints().IsEmpty()) { Source = &Sources[SrcIndex]; }
			else { InData = nullptr; }
		}

		if (InData) { InContext->MainPoints->Emplace_GetRef(*Source, InData, Settings->GetMainOutputInitMode()); }
	}

	return InContext;
}

bool FPCGExPointsProcessorElementBase::Boot(FPCGContext* InContext) const
{
	FPCGExPointsProcessorContext* Context = static_cast<FPCGExPointsProcessorContext*>(InContext);
	PCGEX_SETTINGS(PointsProcessor)

	if (Context->InputData.GetInputs().IsEmpty()) { return false; } //Get rid of errors and warning when there is no input

	if (Context->MainPoints->IsEmpty())
	{
		PCGE_LOG(Error, GraphAndLog, FText::Format(FText::FromString(TEXT("Missing {0} inputs")), FText::FromName(Settings->GetMainInputLabel())));
		return false;
	}

	if (Settings->SupportsPointFilters())
	{
		PCGExFactories::GetInputFactories(InContext, Settings->GetPointFilterLabel(), Context->FilterFactories, {PCGExFactories::EType::Filter}, false);
		if (Settings->RequiresPointFilters() && Context->FilterFactories.IsEmpty())
		{
			PCGE_LOG(Error, GraphAndLog, FText::Format(FTEXT("Missing {0}."), FText::FromName(Settings->GetPointFilterLabel())));
			return false;
		}
	}

	return true;
}

#undef LOCTEXT_NAMESPACE
