﻿// Copyright 2024 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Sampling/PCGExGetTextureData.h"

#include "Data/PCGRenderTargetData.h"
#include "Data/PCGTextureData.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Helpers/PCGBlueprintHelpers.h"
#include "Helpers/PCGHelpers.h"

#define LOCTEXT_NAMESPACE "PCGExGetTextureDataElement"
#define PCGEX_NAMESPACE GetTextureData

UPCGExGetTextureDataSettings::UPCGExGetTextureDataSettings(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

TArray<FPCGPinProperties> UPCGExGetTextureDataSettings::InputPinProperties() const
{
	TArray<FPCGPinProperties> PinProperties = Super::InputPinProperties();
	if (SourceType == EPCGExGetTexturePathType::MaterialPath) { PCGEX_PIN_PARAMS(PCGExTexture::SourceTexLabel, "Texture params to extract from reference materials.", Required, {}) }
	return PinProperties;
}

TArray<FPCGPinProperties> UPCGExGetTextureDataSettings::OutputPinProperties() const
{
	TArray<FPCGPinProperties> PinProperties = Super::OutputPinProperties();
	if (SourceType == EPCGExGetTexturePathType::TexturePath || bBuildTextureData) { PCGEX_PIN_TEXTURES(PCGExTexture::OutputTextureDataLabel, "Texture data.", Required, {}) }
	return PinProperties;
}

PCGExData::EIOInit UPCGExGetTextureDataSettings::GetMainOutputInitMode() const { return bCleanupConsumableAttributes ? PCGExData::EIOInit::Duplicate : PCGExData::EIOInit::Forward; }

PCGEX_INITIALIZE_ELEMENT(GetTextureData)

bool FPCGExGetTextureDataElement::Boot(FPCGExContext* InContext) const
{
	if (!FPCGExPointsProcessorElement::Boot(InContext)) { return false; }

	PCGEX_CONTEXT_AND_SETTINGS(GetTextureData)

	Context->Transform = Settings->Transform;

	AActor* OriginalActor = UPCGBlueprintHelpers::GetOriginalComponent(*Context)->GetOwner();

	if (!Settings->bUseAbsoluteTransform)
	{
		FTransform OriginalActorTransform = OriginalActor->GetTransform();
		Context->Transform = Context->Transform * OriginalActorTransform;

		FBox OriginalActorLocalBounds = PCGHelpers::GetActorLocalBounds(OriginalActor);
		Context->Transform.SetScale3D(Context->Transform.GetScale3D() * 0.5 * (OriginalActorLocalBounds.Max - OriginalActorLocalBounds.Min));
	}

	if (Settings->SourceType == EPCGExGetTexturePathType::MaterialPath)
	{
		if (!PCGExFactories::GetInputFactories(InContext, PCGExTexture::SourceTexLabel, Context->TexParamsFactories, {PCGExFactories::EType::TexParam}, true)) { return false; }

		if (Settings->bOutputTextureIds)
		{
			for (const TObjectPtr<const UPCGExTexParamFactoryBase>& Factory : Context->TexParamsFactories) { PCGEX_VALIDATE_NAME_C(InContext, Factory->Config.TextureIDAttributeName) }
		}
	}

	Context->AddConsumableAttributeName(Settings->SourceAttributeName);

	return true;
}

bool FPCGExGetTextureDataElement::ExecuteInternal(FPCGContext* InContext) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FPCGExGetTextureDataElement::Execute);

	PCGEX_CONTEXT_AND_SETTINGS(GetTextureData)
	PCGEX_EXECUTION_CHECK
	PCGEX_ON_INITIAL_EXECUTION
	{
		if (!Context->StartBatchProcessingPoints<PCGExPointsMT::TBatch<PCGExGetTextureData::FProcessor>>(
			[&](const TSharedPtr<PCGExData::FPointIO>& Entry) { return true; },
			[&](const TSharedPtr<PCGExPointsMT::TBatch<PCGExGetTextureData::FProcessor>>& NewBatch)
			{
			}))
		{
			return Context->CancelExecution(TEXT("Could not find any points to sample."));
		}
	}

	PCGEX_POINTS_BATCH_PROCESSING(PCGEx::State_AsyncPreparation)

	PCGEX_ON_STATE(PCGEx::State_AsyncPreparation)
	{
		if (Context->TextureReferences.IsEmpty())
		{
			// Nothing to load, skip
			Context->SetAsyncState(PCGEx::State_WaitingOnAsyncWork);
		}
		else
		{
			// Start loading textures...
			TSharedPtr<TSet<FSoftObjectPath>> Paths = MakeShared<TSet<FSoftObjectPath>>();
			for (const PCGExTexture::FReference& Ref : Context->TextureReferences) { Paths->Add(Ref.TexturePath); }
			PCGExHelpers::LoadBlocking_AnyThread(Paths);

			Context->TextureReferencesList = Context->TextureReferences.Array();
			Context->SetAsyncState(PCGEx::State_WaitingOnAsyncWork);

			Context->TextureReady.Init(false, Context->TextureReferencesList.Num());
			Context->TextureDataList.Init(nullptr, Context->TextureReferencesList.Num());

			// Push per-reference task
			const TSharedPtr<PCGExMT::FTaskManager> AsyncManager = Context->GetAsyncManager();
			PCGEX_LAUNCH(PCGExGetTextureData::FCreateTextureTask, 0)
		}
	}

	PCGEX_ON_ASYNC_STATE_READY(PCGEx::State_WaitingOnAsyncWork)
	{
		Context->Done();
		Context->MainPoints->StageOutputs();
	}

	return Context->TryComplete();
}

namespace PCGExGetTextureData
{
	FProcessor::~FProcessor()
	{
	}

	bool FProcessor::Process(const TSharedPtr<PCGExMT::FTaskManager> InAsyncManager)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(PCGExGetTextureData::Process);

		// Must be set before process for filters
		PointDataFacade->bSupportsScopedGet = Context->bScopedAttributeGet;

		if (!FPointsProcessor::Process(InAsyncManager)) { return false; }

		if (Settings->SourceType == EPCGExGetTexturePathType::MaterialPath)
		{
			MaterialReferences = MakeShared<TSet<FSoftObjectPath>>();

			// So texture params are registered last, otherwise they're first in the list and it's confusing
			TexParamLookup = MakeShared<PCGExTexture::FLookup>();
			if (!TexParamLookup->BuildFrom(Context->TexParamsFactories))
			{
				PCGE_LOG_C(Error, GraphAndLog, Context, FTEXT("There was an unknown error when processing texture parameters."));
				return false;
			}

			if (Settings->bOutputTextureIds)
			{
				TexParamLookup->PrepareForWrite(Context, PointDataFacade);
			}
		}

		PathGetter = PointDataFacade->GetScopedBroadcaster<FSoftObjectPath>(Settings->SourceAttributeName);

		if (!PathGetter)
		{
			PCGE_LOG_C(Error, GraphAndLog, Context, FText::Format(FTEXT("Asset Path attribute : \"{0}\" does not exists."), FText::FromName(Settings->SourceAttributeName)));
			return false;
		}

		StartParallelLoopForPoints();

		return true;
	}

	void FProcessor::PrepareSingleLoopScopeForPoints(const PCGExMT::FScope& Scope)
	{
		PointDataFacade->Fetch(Scope);
		FilterScope(Scope);
	}

	void FProcessor::ProcessSinglePoint(const int32 Index, FPCGPoint& Point, const PCGExMT::FScope& Scope)
	{
		if (!PointFilterCache[Index]) { return; }

		FSoftObjectPath AssetPath = PathGetter->Read(Index);

		if (Settings->SourceType == EPCGExGetTexturePathType::MaterialPath)
		{
			{
				FReadScopeLock ReadScopeLock(ReferenceLock);
				if (MaterialReferences->Contains(AssetPath)) { return; }
			}
			{
				FWriteScopeLock WriteScopeLock(ReferenceLock);
				MaterialReferences->Add(AssetPath);
			}
			return;
		}


		PCGExTexture::FReference Ref = PCGExTexture::FReference(AssetPath);

		// See if the path breaks down as a path:index textureArray2D path
		int32 LastColonIndex;
		if (const FString Str = AssetPath.ToString(); Str.FindLastChar(TEXT(':'), LastColonIndex))
		{
			const FString PotentialIndex = Str.Mid(LastColonIndex + 1);
			const FString Prefix = Str.Left(LastColonIndex);

			// Check if the part after the last ":" is numeric
			if (PotentialIndex.IsNumeric())
			{
				int32 N = FCString::Atoi(*PotentialIndex);
				if (N < 64)
				{
					// TextureArray2D don't support more entries, so if it's greater it's not that.
					// This is a very weak check :(
					Ref.TexturePath = Prefix;
					Ref.TextureIndex = FCString::Atoi(*PotentialIndex);
				}
			}
		}

		{
			FReadScopeLock ReadScopeLock(ReferenceLock);
			if (TextureReferences.Contains(Ref)) { return; }
		}
		{
			FWriteScopeLock WriteScopeLock(ReferenceLock);
			TextureReferences.Add(Ref);
		}
	}

	void FProcessor::PrepareLoopScopesForRanges(const TArray<PCGExMT::FScope>& Loops)
	{
		ScopedTextureReferences.Reserve(Loops.Num());
		for (int i = 0; i < Loops.Num(); i++)
		{
			TSharedPtr<TSet<PCGExTexture::FReference>> ScopedSet = MakeShared<TSet<PCGExTexture::FReference>>();
			ScopedTextureReferences.Add(ScopedSet);
		}
	}

	void FProcessor::ProcessSingleRangeIteration(const int32 Iteration, const PCGExMT::FScope& Scope)
	{
		TexParamLookup->ExtractParamsAndReferences(
			Iteration,
			TSoftObjectPtr<UMaterialInterface>(PathGetter->Read(Iteration)).Get(),
			*ScopedTextureReferences[Scope.LoopIndex].Get());
	}

	void FProcessor::OnRangeProcessingComplete()
	{
		FWriteScopeLock WriteScopeLock(Context->ReferenceLock);
		for (const TSharedPtr<TSet<PCGExTexture::FReference>>& Set : ScopedTextureReferences)
		{
			Context->TextureReferences.Append(*Set.Get());
		}

		PointDataFacade->Write(AsyncManager);
	}

	void FProcessor::CompleteWork()
	{
		if (Settings->SourceType == EPCGExGetTexturePathType::MaterialPath)
		{
			// Load materials on the main thread x_x
			PCGExHelpers::LoadBlocking_AnyThread(MaterialReferences);

			if (Settings->bOutputTextureIds)
			{
				StartParallelLoopForRange(PointDataFacade->GetNum());
				return;
			}

			for (const FSoftObjectPath& Path : *MaterialReferences.Get())
			{
				UMaterialInterface* Material = TSoftObjectPtr<UMaterialInterface>(Path).Get();
				if (!Material) { continue; }
				TexParamLookup->ExtractReferences(Material, TextureReferences);
			}
		}

		FWriteScopeLock WriteScopeLock(Context->ReferenceLock);
		Context->TextureReferences.Append(TextureReferences);
	}

	void FCreateTextureTask::ExecuteTask(const TSharedPtr<PCGExMT::FTaskManager>& AsyncManager)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(FCreateTextureTask::ExecuteTask);

		FPCGExGetTextureDataContext* Context = AsyncManager->GetContext<FPCGExGetTextureDataContext>();
		PCGEX_SETTINGS(GetTextureData)

		auto MoveToNextTask = [&]()
		{
			if (!Context->TextureReferencesList.IsValidIndex(TaskIndex + 1)) { return; }
			PCGEX_LAUNCH(PCGExGetTextureData::FCreateTextureTask, TaskIndex+1)
		};

		bool bIsFirstInitialization = false;

		auto ApplySettings = [&](UPCGBaseTextureData* InTex)
		{
#if PCGEX_ENGINE_VERSION >= 505
			InTex->Filter = Settings->Filter == EPCGExTextureFilter::Bilinear ? EPCGTextureFilter::Bilinear : EPCGTextureFilter::Point;
#else
			InTex->DensityFunction = EPCGTextureDensityFunction::Multiply;
#endif

			InTex->ColorChannel = Settings->ColorChannel;
			InTex->TexelSize = Settings->TexelSize;
			InTex->Rotation = Settings->Rotation;
			InTex->bUseAdvancedTiling = Settings->bUseAdvancedTiling;
			InTex->Tiling = Settings->Tiling;
			InTex->CenterOffset = Settings->CenterOffset;
			InTex->bUseTileBounds = Settings->bUseTileBounds;
			InTex->TileBounds = Settings->TileBounds;
		};

		PCGExTexture::FReference Ref = Context->TextureReferencesList[TaskIndex];
		TSoftObjectPtr<UTexture> Texture = TSoftObjectPtr<UTexture>(Ref.TexturePath);

		if (!Texture.Get()) { return; }

		TObjectPtr<UPCGTextureData> TexData = Context->TextureDataList[TaskIndex];

		if (!TexData)
		{
			bIsFirstInitialization = true;

			TRACE_CPUPROFILER_EVENT_SCOPE(FCreateTextureTask::CreateTexture);

#pragma region RenderTarget

			if (TObjectPtr<UTextureRenderTarget2D> RT = Cast<UTextureRenderTarget2D>(Texture.Get()))
			{
				TObjectPtr<UPCGRenderTargetData> RTData = Context->ManagedObjects->New<UPCGRenderTargetData>();

				ApplySettings(RTData);
				if (IsInGameThread())
				{
					RTData->Initialize(RT, Context->Transform);
				}
				else
				{
					TWeakPtr<FPCGContextHandle> CtxHandle = Context->GetOrCreateHandle();
					FEvent* BlockingEvent = FPlatformProcess::GetSynchEventFromPool();
					AsyncTask(
						ENamedThreads::GameThread, [RT, RTData, CtxHandle, BlockingEvent]()
						{
							FPCGExGetTextureDataContext* Ctx = FPCGExContext::GetContextFromHandle<FPCGExGetTextureDataContext>(CtxHandle);
							if (!Ctx)
							{
								BlockingEvent->Trigger();
								return;
							}

							RTData->Initialize(RT, Ctx->Transform);
							BlockingEvent->Trigger();
						});

					BlockingEvent->Wait(); // Wait for main thread stuff
					FPlatformProcess::ReturnSynchEventToPool(BlockingEvent);

					if (IsCanceled()) { return; }
				}

				Context->StageOutput(PCGExTexture::OutputTextureDataLabel, RTData, {Ref.GetTag()}, false, false);
				MoveToNextTask();
				return;
			}

#pragma endregion

#pragma region Regular Texture

#if PCGEX_ENGINE_VERSION <= 503
			TSoftObjectPtr<UTexture2D> Texture2D = TSoftObjectPtr<UTexture2D>(Ref.TexturePath);
			if(!Texture2D.Get() || !UPCGTextureData::IsSupported(Texture2D.Get())) { return; }
#endif

			TexData = Context->ManagedObjects->New<UPCGTextureData>();
			Context->TextureDataList[TaskIndex] = TexData;
		}

		if (!bIsFirstInitialization || IsInGameThread())
		{
#if PCGEX_ENGINE_VERSION <= 503
			if(bIsFirstInitialization)
			{
				TexData->Initialize(Texture2D.Get(), Context->Transform);
			}
#elif PCGEX_ENGINE_VERSION == 504
			if(bIsFirstInitialization)
			{
				auto PostInitializeCallback = [&]() { Context->TextureReady[TaskIndex] = true; };
				TexData->Initialize(Texture.Get(), Ref.TextureIndex, Context->Transform, PostInitializeCallback);
			}
#elif PCGEX_ENGINE_VERSION >= 505
			Context->TextureReady[TaskIndex] = TexData->Initialize(Texture.Get(), Ref.TextureIndex, Context->Transform);
#endif
		}
		else
		{
			TWeakPtr<FPCGContextHandle> CtxHandle = Context->GetOrCreateHandle();
			FEvent* WaitForMainThread = FPlatformProcess::GetSynchEventFromPool();
			AsyncTask(
				ENamedThreads::GameThread, [Texture, Ref, TexData, CtxHandle, WaitForMainThread, Idx = TaskIndex]()
				{
					FPCGExGetTextureDataContext* Ctx = FPCGExContext::GetContextFromHandle<FPCGExGetTextureDataContext>(CtxHandle);
					if (!Ctx)
					{
						WaitForMainThread->Trigger();
						return;
					}

#if PCGEX_ENGINE_VERSION <= 503
					TexData->Initialize(TSoftObjectPtr<UTexture2D>(Ref.TexturePath).Get(), Ctx->Transform);
#elif PCGEX_ENGINE_VERSION == 504
					auto PostInitializeCallback = [CtxHandle, Idx]()
					{
						FPCGExGetTextureDataContext* NestedCtx = FPCGExContext::GetContextFromHandle<FPCGExGetTextureDataContext>(CtxHandle);
						if(!NestedCtx){return;}
						NestedCtx->TextureReady[Idx] = true;
					};
							
					TexData->Initialize(Texture.Get(), Ref.TextureIndex, Ctx->Transform, PostInitializeCallback);
#elif PCGEX_ENGINE_VERSION >= 505
					Ctx->TextureReady[Idx] = TexData->Initialize(Texture.Get(), Ref.TextureIndex, Ctx->Transform);
#endif

					WaitForMainThread->Trigger();
				});

			WaitForMainThread->Wait(); // Wait for main thread execution
			FPlatformProcess::ReturnSynchEventToPool(WaitForMainThread);

			if (IsCanceled()) { return; }
		}

		if (!Context->TextureReady[TaskIndex])
		{
			// This is bad, but still less bad than sleep or wait
			// At least it leave Unreal do some balancing instead of hogging a single thread
			// TODO : Launch with lowest priority available
			PCGEX_LAUNCH_INTERNAL(FCreateTextureTask, TaskIndex)
			return;
		}

#if PCGEX_ENGINE_VERSION >= 505
		if (!TexData->IsSuccessfullyInitialized())
		{
			MoveToNextTask();
			return;
		}
#endif

		if (!TexData->IsValid())
		{
			MoveToNextTask();
			return;
		}

		Context->StageOutput(PCGExTexture::OutputTextureDataLabel, TexData, {Ref.GetTag()}, false, false);
		MoveToNextTask();

#pragma endregion
	}
}

#undef LOCTEXT_NAMESPACE
#undef PCGEX_NAMESPACE
