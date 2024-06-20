﻿// Copyright Timothé Lapetite 2024
// Released under the MIT license https://opensource.org/license/MIT/

#include "Graph/Edges/Extras/PCGExVtxExtraEdgeMatch.h"

#define LOCTEXT_NAMESPACE "PCGExVtxExtraEdgeMatch"
#define PCGEX_NAMESPACE PCGExVtxExtraEdgeMatch

void UPCGExVtxExtraEdgeMatch::CopySettingsFrom(const UPCGExOperation* Other)
{
	Super::CopySettingsFrom(Other);
	const UPCGExVtxExtraEdgeMatch* TypedOther = Cast<UPCGExVtxExtraEdgeMatch>(Other);
	if (TypedOther)
	{
		Descriptor = TypedOther->Descriptor;
	}
}

bool UPCGExVtxExtraEdgeMatch::PrepareForCluster(const FPCGContext* InContext, PCGExCluster::FCluster* InCluster)
{
	if (!Super::PrepareForCluster(InContext, InCluster)) { return false; }

	if (!Descriptor.MatchingEdge.Validate(InContext))
	{
		bIsValidOperation = false;
		return false;
	}

	if (Descriptor.MatchingEdge.bWriteDirection)
	{
		MatchingDirWriter = new PCGEx::TFAttributeWriter<FVector>(Descriptor.MatchingEdge.DirectionAttribute);
		MatchingDirWriter->BindAndSetNumUninitialized(*InCluster->PointsIO);
	}

	if (Descriptor.MatchingEdge.bWriteLength)
	{
		MatchingLenWriter = new PCGEx::TFAttributeWriter<double>(Descriptor.MatchingEdge.LengthAttribute);
		MatchingLenWriter->BindAndSetNumUninitialized(*InCluster->PointsIO);
	}

	return bIsValidOperation;
}

void UPCGExVtxExtraEdgeMatch::ProcessNode(PCGExCluster::FNode& Node, const TArray<PCGExCluster::FAdjacencyData>& Adjacency)
{
	// TODO : Implement
}

void UPCGExVtxExtraEdgeMatch::Write()
{
	Super::Write();
	Descriptor.MatchingEdge.Write();
}

void UPCGExVtxExtraEdgeMatch::Write(const TArrayView<int32> Indices)
{
	Super::Write(Indices);
	Descriptor.MatchingEdge.Write(Indices);
}

void UPCGExVtxExtraEdgeMatch::Cleanup()
{
	Descriptor.MatchingEdge.Cleanup();
	Super::Cleanup();
}

#if WITH_EDITOR
FString UPCGExVtxExtraEdgeMatchSettings::GetDisplayName() const
{
	/*
	if (Descriptor.SourceAttributes.IsEmpty()) { return TEXT(""); }
	TArray<FName> Names = Descriptor.SourceAttributes.Array();

	if (Names.Num() == 1) { return Names[0].ToString(); }
	if (Names.Num() == 2) { return Names[0].ToString() + TEXT(" (+1 other)"); }

	return Names[0].ToString() + FString::Printf(TEXT(" (+%d others)"), (Names.Num() - 1));
	*/
	return TEXT("");
}
#endif

UPCGExVtxExtraOperation* UPCGExVtxExtraEdgeMatchFactory::CreateOperation() const
{
	UPCGExVtxExtraEdgeMatch* NewOperation = NewObject<UPCGExVtxExtraEdgeMatch>();
	PCGEX_VTX_EXTRA_CREATE
	return NewOperation;
}

UPCGExParamFactoryBase* UPCGExVtxExtraEdgeMatchSettings::CreateFactory(FPCGContext* InContext, UPCGExParamFactoryBase* InFactory) const
{
	UPCGExVtxExtraEdgeMatchFactory* NewFactory = NewObject<UPCGExVtxExtraEdgeMatchFactory>();
	NewFactory->Descriptor = Descriptor;
	return Super::CreateFactory(InContext, NewFactory);
}


#undef LOCTEXT_NAMESPACE
#undef PCGEX_NAMESPACE
