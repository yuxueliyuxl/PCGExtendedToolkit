// Copyright 2025 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Collections/PCGExMeshCollection.h"

void FPCGExMaterialOverrideCollection::GetAssetPaths(TSet<FSoftObjectPath>& OutPaths) const
{
	for (const FPCGExMaterialOverrideEntry& Entry : Overrides) { OutPaths.Add(Entry.Material.ToSoftObjectPath()); }
}

int32 FPCGExMaterialOverrideCollection::GetHighestIndex() const
{
	int32 HighestIndex = -1;
	for (const FPCGExMaterialOverrideEntry& Entry : Overrides) { HighestIndex = FMath::Max(HighestIndex, Entry.SlotIndex); }
	return HighestIndex;
}

#if WITH_EDITOR
void FPCGExMaterialOverrideCollection::UpdateDisplayName()
{
}

void FPCGExMaterialOverrideSingleEntry::UpdateDisplayName()
{
	DisplayName = FName(Material.GetAssetName());
}
#endif

namespace PCGExMeshCollection
{
	void FMacroCache::ProcessMaterialOverrides(const TArray<FPCGExMaterialOverrideSingleEntry>& Overrides, const int32 InSlotIndex)
	{
		const int32 NumEntries = Overrides.Num();

		HighestIndex = InSlotIndex;

		Weights.SetNumUninitialized(NumEntries);
		for (int i = 0; i < NumEntries; i++) { Weights[i] = Overrides[i].Weight + 1; }

		PCGEx::ArrayOfIndices(Order, NumEntries);

		Order.Sort([&](const int32 A, const int32 B) { return Weights[A] < Weights[B]; });
		Weights.Sort([](const int32 A, const int32 B) { return A < B; });

		WeightSum = 0;
		for (int32 i = 0; i < NumEntries; i++)
		{
			WeightSum += Weights[i];
			Weights[i] = WeightSum;
		}
	}

	void FMacroCache::ProcessMaterialOverrides(const TArray<FPCGExMaterialOverrideCollection>& Overrides)
	{
		const int32 NumEntries = Overrides.Num();

		Weights.SetNumUninitialized(NumEntries);
		HighestIndex = -1;

		for (int i = 0; i < NumEntries; i++)
		{
			Weights[i] = Overrides[i].Weight + 1;
			HighestIndex = FMath::Max(HighestIndex, Overrides[i].GetHighestIndex());
		}

		PCGEx::ArrayOfIndices(Order, NumEntries);

		Order.Sort([&](const int32 A, const int32 B) { return Weights[A] < Weights[B]; });
		Weights.Sort([](const int32 A, const int32 B) { return A < B; });

		WeightSum = 0;
		for (int32 i = 0; i < NumEntries; i++)
		{
			WeightSum += Weights[i];
			Weights[i] = WeightSum;
		}
	}

	int32 FMacroCache::GetPick(const int32 Index, const EPCGExIndexPickMode PickMode) const
	{
		switch (PickMode)
		{
		default:
		case EPCGExIndexPickMode::Ascending:
			return GetPickAscending(Index);
		case EPCGExIndexPickMode::Descending:
			return GetPickDescending(Index);
		case EPCGExIndexPickMode::WeightAscending:
			return GetPickWeightAscending(Index);
		case EPCGExIndexPickMode::WeightDescending:
			return GetPickWeightDescending(Index);
		}
	}

	int32 FMacroCache::GetPickAscending(const int32 Index) const
	{
		return Order.IsValidIndex(Index) ? Index : -1;
	}

	int32 FMacroCache::GetPickDescending(const int32 Index) const
	{
		return Order.IsValidIndex(Index) ? (Order.Num() - 1) - Index : -1;
	}

	int32 FMacroCache::GetPickWeightAscending(const int32 Index) const
	{
		return Order.IsValidIndex(Index) ? Order[Index] : -1;
	}

	int32 FMacroCache::GetPickWeightDescending(const int32 Index) const
	{
		return Order.IsValidIndex(Index) ? Order[(Order.Num() - 1) - Index] : -1;
	}

	int32 FMacroCache::GetPickRandom(const int32 Seed) const
	{
		return Order[FRandomStream(Seed).RandRange(0, Order.Num() - 1)];
	}

	int32 FMacroCache::GetPickRandomWeighted(const int32 Seed) const
	{
		if (Order.IsEmpty()) { return -1; }
		
		const int32 Threshold = FRandomStream(Seed).RandRange(0, WeightSum - 1);
		int32 Pick = 0;
		while (Pick < Weights.Num() && Weights[Pick] < Threshold) { Pick++; }
		return Order[Pick];
	}
}

void FPCGExMeshCollectionEntry::GetAssetPaths(TSet<FSoftObjectPath>& OutPaths) const
{
	FPCGExAssetCollectionEntry::GetAssetPaths(OutPaths);

	// Override materials

	switch (MaterialVariants)
	{
	default:
	case EPCGExMaterialVariantsMode::None:
		break;
	case EPCGExMaterialVariantsMode::Single:
		for (const FPCGExMaterialOverrideSingleEntry& Entry : MaterialOverrideVariants) { OutPaths.Add(Entry.Material.ToSoftObjectPath()); }
		break;
	case EPCGExMaterialVariantsMode::Multi:
		for (const FPCGExMaterialOverrideCollection& Entry : MaterialOverrideVariantsList) { Entry.GetAssetPaths(OutPaths); }
		break;
	}

	// ISM

	for (int i = 0; i < ISMDescriptor.OverrideMaterials.Num(); i++)
	{
		if (!ISMDescriptor.OverrideMaterials[i].IsNull())
		{
			OutPaths.Add(ISMDescriptor.OverrideMaterials[i].ToSoftObjectPath());
		}
	}

	for (int i = 0; i < ISMDescriptor.RuntimeVirtualTextures.Num(); i++)
	{
		if (!ISMDescriptor.RuntimeVirtualTextures[i].IsNull())
		{
			OutPaths.Add(ISMDescriptor.RuntimeVirtualTextures[i].ToSoftObjectPath());
		}
	}

	// SM

	for (int i = 0; i < SMDescriptor.OverrideMaterials.Num(); i++)
	{
		if (!SMDescriptor.OverrideMaterials[i].IsNull())
		{
			OutPaths.Add(SMDescriptor.OverrideMaterials[i].ToSoftObjectPath());
		}
	}

	for (int i = 0; i < SMDescriptor.RuntimeVirtualTextures.Num(); i++)
	{
		if (!SMDescriptor.RuntimeVirtualTextures[i].IsNull())
		{
			OutPaths.Add(SMDescriptor.RuntimeVirtualTextures[i].ToSoftObjectPath());
		}
	}
}

bool FPCGExMeshCollectionEntry::Validate(const UPCGExAssetCollection* ParentCollection)
{
	if (!bIsSubCollection)
	{
		if (!StaticMesh.ToSoftObjectPath().IsValid() && ParentCollection->bDoNotIgnoreInvalidEntries) { return false; }
	}

	return Super::Validate(ParentCollection);
}

#if WITH_EDITOR
void FPCGExMeshCollectionEntry::EDITOR_Sanitize()
{
	FPCGExAssetCollectionEntry::EDITOR_Sanitize();

	if (!bIsSubCollection)
	{
		InternalSubCollection = nullptr;
		if (StaticMesh) { ISMDescriptor.StaticMesh = StaticMesh; }
		//else if (ISMDescriptor.StaticMesh && !StaticMesh) { StaticMesh = ISMDescriptor.StaticMesh; }
	}
	else
	{
		InternalSubCollection = SubCollection;
	}
}

void FPCGExMeshCollectionEntry::BuildMacroCache()
{
	const TSharedPtr<PCGExMeshCollection::FMacroCache> NewCache = MakeShared<PCGExMeshCollection::FMacroCache>();

	switch (MaterialVariants)
	{
	default:
	case EPCGExMaterialVariantsMode::None:
		break;
	case EPCGExMaterialVariantsMode::Single:
		NewCache->ProcessMaterialOverrides(MaterialOverrideVariants, SlotIndex);
		break;
	case EPCGExMaterialVariantsMode::Multi:
		NewCache->ProcessMaterialOverrides(MaterialOverrideVariantsList);
		break;
	}

	MacroCache = NewCache;
}
#endif

void FPCGExMeshCollectionEntry::UpdateStaging(const UPCGExAssetCollection* OwningCollection, const int32 InInternalIndex, const bool bRecursive)
{
	if (bIsSubCollection)
	{
		Super::UpdateStaging(OwningCollection, InInternalIndex, bRecursive);
		return;
	}

	if (Staging.InternalIndex == -1 && GetDefault<UPCGExGlobalSettings>()->bDisableCollisionByDefault)
	{
		ISMDescriptor.BodyInstance.SetCollisionEnabled(ECollisionEnabled::Type::NoCollision);
		SMDescriptor.BodyInstance.SetCollisionEnabled(ECollisionEnabled::Type::NoCollision);
	}

	Staging.Path = StaticMesh.ToSoftObjectPath();

#if WITH_EDITOR
	if (MaterialVariants != EPCGExMaterialVariantsMode::None)
	{
		if (MaterialVariants == EPCGExMaterialVariantsMode::Single)
		{
			for (int i = 0; i < MaterialOverrideVariants.Num(); i++)
			{
				FPCGExMaterialOverrideSingleEntry& MEntry = MaterialOverrideVariants[i];
				MEntry.UpdateDisplayName();
			}
		}
		else
		{
			for (int i = 0; i < MaterialOverrideVariantsList.Num(); i++)
			{
				FPCGExMaterialOverrideCollection& MEntry = MaterialOverrideVariantsList[i];
				MEntry.UpdateDisplayName();
			}
		}
	}
#endif

	const UStaticMesh* M = PCGExHelpers::LoadBlocking_AnyThread(StaticMesh);
	PCGExAssetCollection::UpdateStagingBounds(Staging, M);

	Super::UpdateStaging(OwningCollection, InInternalIndex, bRecursive);
}

void FPCGExMeshCollectionEntry::SetAssetPath(const FSoftObjectPath& InPath)
{
	Super::SetAssetPath(InPath);
	StaticMesh = TSoftObjectPtr<UStaticMesh>(InPath);
	ISMDescriptor.StaticMesh = StaticMesh;
}

#if PCGEX_ENGINE_VERSION > 504
void FPCGExMeshCollectionEntry::InitPCGSoftISMDescriptor(FPCGSoftISMComponentDescriptor& TargetDescriptor) const
{
	PCGExHelpers::CopyStructProperties(
		&ISMDescriptor,
		&TargetDescriptor,
		FSoftISMComponentDescriptor::StaticStruct(),
		FPCGSoftISMComponentDescriptor::StaticStruct());

	TargetDescriptor.ComponentTags.Append(Tags.Array());
}
#endif

#if WITH_EDITOR
void UPCGExMeshCollection::EDITOR_RefreshDisplayNames()
{
	Super::EDITOR_RefreshDisplayNames();
	for (FPCGExMeshCollectionEntry& Entry : Entries)
	{
		FString DisplayName = Entry.bIsSubCollection ? TEXT("[") + Entry.SubCollection.GetName() + TEXT("]") : Entry.StaticMesh.GetAssetName();
		DisplayName += FString::Printf(TEXT(" @ %d "), Entry.Weight);
		Entry.DisplayName = FName(DisplayName);
	}
}

void UPCGExMeshCollection::EDITOR_DisableCollisions()
{
	Modify(true);

	for (FPCGExMeshCollectionEntry& Entry : Entries)
	{
		Entry.ISMDescriptor.BodyInstance.SetCollisionEnabled(ECollisionEnabled::Type::NoCollision);
		Entry.SMDescriptor.BodyInstance.SetCollisionEnabled(ECollisionEnabled::Type::NoCollision);
	}

	FPropertyChangedEvent EmptyEvent(nullptr);
	PostEditChangeProperty(EmptyEvent);
	MarkPackageDirty();
}

#endif
