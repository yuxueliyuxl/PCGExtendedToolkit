﻿// Copyright Timothé Lapetite 2024
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "PCGExPathProcessor.h"

#include "PCGExPointsProcessor.h"

#include "PCGExOffsetPath.generated.h"

/**
 * 
 */
UCLASS(MinimalAPI, BlueprintType, ClassGroup = (Procedural), Category="PCGEx|Path")
class /*PCGEXTENDEDTOOLKIT_API*/ UPCGExOffsetPathSettings : public UPCGExPathProcessorSettings
{
	GENERATED_BODY()

public:
	//~Begin UPCGSettings
#if WITH_EDITOR
	PCGEX_NODE_INFOS(PathOffset, "Path : Offset", "Offset paths points.");
#endif

protected:
	virtual FPCGElementPtr CreateElement() const override;
	//~End UPCGSettings

	//~Begin UPCGExPointsProcessorSettings
public:
	virtual PCGExData::EInit GetMainOutputInitMode() const override;
	//~End UPCGExPointsProcessorSettings

	/** Offset type.*/
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta = (PCG_Overridable))
	EPCGExInputValueType OffsetInput = EPCGExInputValueType::Constant;

	/** Offset size.*/
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable, EditCondition="OffsetInput == EPCGExInputValueType::Constant"))
	double OffsetConstant = 1.0;

	/** Fetch the offset size from a local attribute. The regular Size parameter then act as a scale.*/
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta = (PCG_Overridable, EditCondition="OffsetInput == EPCGExInputValueType::Attribute"))
	FPCGAttributePropertyInputSelector OffsetAttribute;

	/** Up vector used to calculate Offset direction.*/
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable, EditCondition="UpVectorType == EPCGExInputValueType::Constant"))
	FVector UpVectorConstant = FVector::UpVector;

	/** Direction Vector type.*/
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta = (PCG_Overridable))
	EPCGExInputValueType DirectionType = EPCGExInputValueType::Constant;

	/** Type of arithmetic path point offset direction.*/
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable, EditCondition="DirectionType == EPCGExInputValueType::Constant"))
	EPCGExPathNormalDirection DirectionConstant = EPCGExPathNormalDirection::AverageNormal;

	/** Fetch the direction vector from a local point attribute. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta = (PCG_Overridable, EditCondition="DirectionType == EPCGExInputValueType::Attribute"))
	FPCGAttributePropertyInputSelector DirectionAttribute;
	
};

struct /*PCGEXTENDEDTOOLKIT_API*/ FPCGExOffsetPathContext final : FPCGExPathProcessorContext
{
	friend class FPCGExOffsetPathElement;
};

class /*PCGEXTENDEDTOOLKIT_API*/ FPCGExOffsetPathElement final : public FPCGExPathProcessorElement
{
public:
	virtual FPCGContext* Initialize(
		const FPCGDataCollection& InputData,
		TWeakObjectPtr<UPCGComponent> SourceComponent,
		const UPCGNode* Node) override;

protected:
	virtual bool Boot(FPCGExContext* InContext) const override;
	virtual bool ExecuteInternal(FPCGContext* Context) const override;
};

namespace PCGExOffsetPath
{
	class FProcessor final : public PCGExPointsMT::TPointsProcessor<FPCGExOffsetPathContext, UPCGExOffsetPathSettings>
	{

		TSharedPtr<PCGExPaths::FPath> Path;
		TSharedPtr<PCGExPaths::TPathEdgeExtra<FVector>> Direction;
		
		double OffsetConstant = 0;
		FVector Up = FVector::UpVector;

		TSharedPtr<PCGExData::TBuffer<double>> OffsetGetter;
		TSharedPtr<PCGExData::TBuffer<FVector>> DirectionGetter;

	public:
		explicit FProcessor(const TSharedRef<PCGExData::FFacade>& InPointDataFacade):
			TPointsProcessor(InPointDataFacade)
		{
		}

		virtual bool Process(const TSharedPtr<PCGExMT::FTaskManager> InAsyncManager) override;
		virtual void PrepareSingleLoopScopeForPoints(const uint32 StartIndex, const int32 Count) override;
		virtual void ProcessSinglePoint(const int32 Index, FPCGPoint& Point, const int32 LoopIdx, const int32 Count) override;
		virtual void CompleteWork() override;
	};
}
