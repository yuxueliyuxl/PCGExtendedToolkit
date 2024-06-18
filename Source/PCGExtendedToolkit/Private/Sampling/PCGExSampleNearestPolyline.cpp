﻿// Copyright Timothé Lapetite 2024
// Released under the MIT license https://opensource.org/license/MIT/

#include "Sampling/PCGExSampleNearestPolyline.h"

#include "Data/PCGExDataFilter.h"

#define LOCTEXT_NAMESPACE "PCGExSampleNearestPolylineElement"
#define PCGEX_NAMESPACE SampleNearestPolyLine

UPCGExSampleNearestPolylineSettings::UPCGExSampleNearestPolylineSettings(
	const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	if (LookAtUpSource.GetName() == FName("@Last")) { LookAtUpSource.Update(TEXT("$Transform.Up")); }
	if (!WeightOverDistance) { WeightOverDistance = PCGEx::WeightDistributionLinearInv; }
}

TArray<FPCGPinProperties> UPCGExSampleNearestPolylineSettings::InputPinProperties() const
{
	TArray<FPCGPinProperties> PinProperties = Super::InputPinProperties();
	PCGEX_PIN_POLYLINES(PCGEx::SourceTargetsLabel, "The spline data set to check against.", Required, {})
	PCGEX_PIN_PARAMS(PCGEx::SourcePointFilters, "Filter which points will be processed.", Advanced, {})
	return PinProperties;
}

PCGExData::EInit UPCGExSampleNearestPolylineSettings::GetMainOutputInitMode() const { return PCGExData::EInit::DuplicateInput; }

int32 UPCGExSampleNearestPolylineSettings::GetPreferredChunkSize() const { return PCGExMT::GAsyncLoop_L; }

FName UPCGExSampleNearestPolylineSettings::GetPointFilterLabel() const { return PCGExDataFilter::SourceFiltersLabel; }

PCGEX_INITIALIZE_ELEMENT(SampleNearestPolyline)

FPCGExSampleNearestPolylineContext::~FPCGExSampleNearestPolylineContext()
{
	PCGEX_TERMINATE_ASYNC

	PCGEX_DELETE(Targets)
}

bool FPCGExSampleNearestPolylineElement::Boot(FPCGContext* InContext) const
{
	if (!FPCGExPointsProcessorElementBase::Boot(InContext)) { return false; }

	PCGEX_CONTEXT_AND_SETTINGS(SampleNearestPolyline)

	TArray<FPCGTaggedData> Targets = Context->InputData.GetInputsByPin(PCGEx::SourceTargetsLabel);

	if (!Targets.IsEmpty())
	{
		Context->Targets = new PCGExData::FPolyLineIOGroup(Targets);
		Context->NumTargets = Context->Targets->Lines.Num();
	}

	if (Context->NumTargets <= 0)
	{
		PCGE_LOG(Error, GraphAndLog, FTEXT("No targets (either no input or empty dataset)"));
		return false;
	}

	Context->WeightCurve = Settings->WeightOverDistance.LoadSynchronous();
	if (!Context->WeightCurve)
	{
		PCGE_LOG(Error, GraphAndLog, FTEXT("Weight Curve asset could not be loaded."));
		return false;
	}

	PCGEX_FOREACH_FIELD_NEARESTPOLYLINE(PCGEX_OUTPUT_VALIDATE_NAME)

	return true;
}

bool FPCGExSampleNearestPolylineElement::ExecuteInternal(FPCGContext* InContext) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FPCGExSampleNearestPolylineElement::Execute);

	PCGEX_CONTEXT_AND_SETTINGS(SampleNearestPolyline)

	if (Context->IsSetup())
	{
		if (!Boot(Context)) { return true; }

		if (!Context->StartBatchProcessingPoints<PCGExPointsMT::TBatch<PCGExSampleNearestPolyline::FProcessor>>(
			[&](PCGExData::FPointIO* Entry) { return true; },
			[&](PCGExPointsMT::TBatch<PCGExSampleNearestPolyline::FProcessor>* NewBatch)
			{
				NewBatch->SetPointsFilterData(&Context->FilterFactories);
			},
			PCGExMT::State_Done))
		{
			PCGE_LOG(Warning, GraphAndLog, FTEXT("Could not find any paths to split."));
			return true;
		}
	}

	if (!Context->ProcessPointsBatch()) { return false; }

	if (Context->IsDone())
	{
		Context->OutputMainPoints();
		Context->ExecuteEnd();
	}

	return Context->IsDone();
}


namespace PCGExSampleNearestPolyline
{
	FProcessor::FProcessor(PCGExData::FPointIO* InPoints):
		FPointsProcessor(InPoints)
	{
	}

	FProcessor::~FProcessor()
	{
		PCGEX_DELETE(RangeMinGetter)
		PCGEX_DELETE(RangeMaxGetter)
		PCGEX_DELETE(LookAtUpGetter)

		PCGEX_FOREACH_FIELD_NEARESTPOLYLINE(PCGEX_OUTPUT_DELETE)
	}

	bool FProcessor::Process(FPCGExAsyncManager* AsyncManager)
	{
		PCGEX_TYPED_CONTEXT_AND_SETTINGS(SampleNearestPolyline)

		if (!FPointsProcessor::Process(AsyncManager)) { return false; }

		{
			PCGExData::FPointIO& OutputIO = *PointIO;
			PCGEX_FOREACH_FIELD_NEARESTPOLYLINE(PCGEX_OUTPUT_FWD_INIT)
		}

		RangeMinGetter = new PCGEx::FLocalSingleFieldGetter();
		RangeMinGetter->Capture(Settings->LocalRangeMin);

		RangeMaxGetter = new PCGEx::FLocalSingleFieldGetter();
		RangeMaxGetter->Capture(Settings->LocalRangeMax);

		LookAtUpGetter = new PCGEx::FLocalVectorGetter();
		if (Settings->bWriteLookAtTransform && Settings->LookAtUpSelection != EPCGExSampleSource::Constant)
		{
			LookAtUpGetter->Capture(Settings->LookAtUpSource);
		}

		if (Settings->bUseLocalRangeMin)
		{
			if (!RangeMinGetter->Grab(*PointIO)) { PCGE_LOG_C(Warning, GraphAndLog, Context, FTEXT("RangeMin metadata missing")); }
		}

		if (Settings->bUseLocalRangeMax)
		{
			if (!RangeMaxGetter->Grab(*PointIO)) { PCGE_LOG_C(Warning, GraphAndLog, Context, FTEXT("RangeMax metadata missing")); }
		}

		if (Settings->bWriteLookAtTransform)
		{
			if (Settings->LookAtUpSelection == EPCGExSampleSource::Source &&
				!LookAtUpGetter->Grab(*PointIO))
			{
				PCGE_LOG_C(Warning, GraphAndLog, Context, FTEXT("LookUp is invalid on source."));
			}
		}

		PointIO->CreateOutKeys();

		StartParallelLoopForPoints();

		return true;
	}

	void FProcessor::ProcessSinglePoint(int32 Index, FPCGPoint& Point)
	{
		PCGEX_TYPED_CONTEXT_AND_SETTINGS(SampleNearestPolyline)

		if (!PointFilterCache[Index]) { return; }

		double RangeMin = FMath::Pow(RangeMinGetter->SafeGet(Index, Settings->RangeMin), 2);
		double RangeMax = FMath::Pow(RangeMaxGetter->SafeGet(Index, Settings->RangeMax), 2);

		if (RangeMin > RangeMax) { std::swap(RangeMin, RangeMax); }

		TArray<PCGExPolyLine::FSampleInfos> TargetsInfos;
		TargetsInfos.Reserve(TypedContext->NumTargets);

		PCGExPolyLine::FTargetsCompoundInfos TargetsCompoundInfos;

		FVector Origin = Point.Transform.GetLocation();
		auto ProcessTarget = [&](const FTransform& Transform, const double& Time)
		{
			const FVector ModifiedOrigin = PCGExMath::GetSpatializedCenter(Settings->DistanceSettings, Point, Origin, Transform.GetLocation());
			const double Dist = FVector::DistSquared(ModifiedOrigin, Transform.GetLocation());

			if (Settings->SampleMethod == EPCGExSampleMethod::ClosestTarget ||
				Settings->SampleMethod == EPCGExSampleMethod::FarthestTarget)
			{
				TargetsCompoundInfos.UpdateCompound(PCGExPolyLine::FSampleInfos(Transform, Dist, Time));
				return;
			}

			if (RangeMax > 0 && (Dist < RangeMin || Dist > RangeMax)) { return; }

			const PCGExPolyLine::FSampleInfos& Infos = TargetsInfos.Emplace_GetRef(Transform, Dist, Time);
			TargetsCompoundInfos.UpdateCompound(Infos);
		};

		// First: Sample all possible targets
		if (RangeMax > 0)
		{
			for (PCGExData::FPolyLineIO* Line : TypedContext->Targets->Lines)
			{
				FTransform SampledTransform;
				double Time;
				if (!Line->SampleNearestTransform(Origin, FMath::Sqrt(RangeMax), SampledTransform, Time)) { continue; }
				ProcessTarget(SampledTransform, Time);
			}
		}
		else
		{
			for (PCGExData::FPolyLineIO* Line : TypedContext->Targets->Lines)
			{
				double Time;
				ProcessTarget(Line->SampleNearestTransform(Origin, Time), Time);
			}
		}

		// Compound never got updated, meaning we couldn't find target in range
		if (TargetsCompoundInfos.UpdateCount <= 0)
		{
			double FailSafeDist = FMath::Sqrt(RangeMax);
			PCGEX_OUTPUT_VALUE(Success, Index, false)
			PCGEX_OUTPUT_VALUE(Transform, Index, Point.Transform)
			PCGEX_OUTPUT_VALUE(LookAtTransform, Index, Point.Transform)
			PCGEX_OUTPUT_VALUE(Distance, Index, FailSafeDist)
			PCGEX_OUTPUT_VALUE(SignedDistance, Index, FailSafeDist)
			return;
		}

		// Compute individual target weight
		if (Settings->WeightMethod == EPCGExRangeType::FullRange && RangeMax > 0)
		{
			// Reset compounded infos to full range
			TargetsCompoundInfos.SampledRangeMin = RangeMin;
			TargetsCompoundInfos.SampledRangeMax = RangeMax;
			TargetsCompoundInfos.SampledRangeWidth = RangeMax - RangeMin;
		}

		FTransform WeightedTransform = FTransform::Identity;
		WeightedTransform.SetScale3D(FVector::ZeroVector);

		FVector WeightedUp = Settings->LookAtUpSelection == EPCGExSampleSource::Source ?
			                     LookAtUpGetter->SafeGet(Index, SafeUpVector) :
			                     SafeUpVector;
		FVector WeightedSignAxis = FVector::Zero();
		FVector WeightedAngleAxis = FVector::Zero();
		double WeightedTime = 0;
		double TotalWeight = 0;


		auto ProcessTargetInfos = [&](const PCGExPolyLine::FSampleInfos& TargetInfos, const double Weight)
		{
			const FQuat Quat = TargetInfos.Transform.GetRotation();

			WeightedTransform.SetRotation(WeightedTransform.GetRotation() + (TargetInfos.Transform.GetRotation() * Weight));
			WeightedTransform.SetScale3D(WeightedTransform.GetScale3D() + (TargetInfos.Transform.GetScale3D() * Weight));
			WeightedTransform.SetLocation(WeightedTransform.GetLocation() + (TargetInfos.Transform.GetLocation() * Weight));

			if (Settings->LookAtUpSelection == EPCGExSampleSource::Target) { WeightedUp += PCGExMath::GetDirection(Quat, Settings->LookAtUpAxis) * Weight; }

			WeightedSignAxis += PCGExMath::GetDirection(Quat, Settings->SignAxis) * Weight;
			WeightedAngleAxis += PCGExMath::GetDirection(Quat, Settings->AngleAxis) * Weight;
			WeightedTime += TargetInfos.Time * Weight;
			TotalWeight += Weight;
		};


		if (Settings->SampleMethod == EPCGExSampleMethod::ClosestTarget ||
			Settings->SampleMethod == EPCGExSampleMethod::FarthestTarget)
		{
			const PCGExPolyLine::FSampleInfos& TargetInfos = Settings->SampleMethod == EPCGExSampleMethod::ClosestTarget ? TargetsCompoundInfos.Closest : TargetsCompoundInfos.Farthest;
			const double Weight = TypedContext->WeightCurve->GetFloatValue(TargetsCompoundInfos.GetRangeRatio(TargetInfos.Distance));
			ProcessTargetInfos(TargetInfos, Weight);
		}
		else
		{
			for (PCGExPolyLine::FSampleInfos& TargetInfos : TargetsInfos)
			{
				const double Weight = TypedContext->WeightCurve->GetFloatValue(TargetsCompoundInfos.GetRangeRatio(TargetInfos.Distance));
				if (Weight == 0) { continue; }
				ProcessTargetInfos(TargetInfos, Weight);
			}
		}

		if (TotalWeight != 0) // Dodge NaN
		{
			WeightedUp /= TotalWeight;

			WeightedTransform.SetRotation(WeightedTransform.GetRotation() / TotalWeight);
			WeightedTransform.SetScale3D(WeightedTransform.GetScale3D() / TotalWeight);
			WeightedTransform.SetLocation(WeightedTransform.GetLocation() / TotalWeight);
		}

		WeightedUp.Normalize();

		FVector LookAt = (Point.Transform.GetLocation() - WeightedTransform.GetLocation()).GetSafeNormal();
		const double WeightedDistance = FVector::Dist(Origin, WeightedTransform.GetLocation());

		PCGEX_OUTPUT_VALUE(Success, Index, TargetsCompoundInfos.IsValid())
		PCGEX_OUTPUT_VALUE(Transform, Index, WeightedTransform)
		PCGEX_OUTPUT_VALUE(LookAtTransform, Index, PCGExMath::MakeLookAtTransform(LookAt, WeightedUp, Settings->LookAtAxisAlign))
		PCGEX_OUTPUT_VALUE(Distance, Index, WeightedDistance)
		PCGEX_OUTPUT_VALUE(SignedDistance, Index, FMath::Sign(WeightedSignAxis.Dot(LookAt)) * WeightedDistance)
		PCGEX_OUTPUT_VALUE(Angle, Index, PCGExSampling::GetAngle(Settings->AngleRange, WeightedAngleAxis, LookAt))
		PCGEX_OUTPUT_VALUE(Time, Index, WeightedTime)
	}

	void FProcessor::CompleteWork()
	{
		FPointsProcessor::CompleteWork();
		PCGEX_FOREACH_FIELD_NEARESTPOLYLINE(PCGEX_OUTPUT_WRITE)
	}
}

#undef LOCTEXT_NAMESPACE
#undef PCGEX_NAMESPACE
