#include "Unreal/CoreUObject/UObject/UnrealType.hpp"
#include "Unreal/Engine/UDataTable.hpp"
#include "SDK/Classes/Custom/UObjectGlobals.h"
#include "SDK/Classes/PalItemIDManager.h"
#include "SDK/Classes/PalStaticItemDataTable.h"
#include "SDK/Classes/PalStaticArmorItemData.h"
#include "SDK/Classes/PalStaticConsumeItemData.h"
#include "SDK/Classes/PalStaticWeaponItemData.h"
#include "SDK/Classes/PalDynamicWeaponItemDataBase.h"
#include "SDK/Classes/PalDynamicArmorItemDataBase.h"
#include "SDK/Classes/PalUtility.h"
#include "SDK/Structs/FPalCharacterIconDataRow.h"
#include "SDK/Structs/Custom/FManagedStruct.h"
#include "SDK/Helper/PropertyHelper.h"
#include "Helpers/String.hpp"
#include "Utility/Logging.h"
#include "Utility/JsonHelpers.h"
#include "Loader/PalItemModLoader.h"
#include <SDK/Structs/FPalLocalizedTextData.h>
#include <SDK/PalSignatures.h>

using namespace RC;
using namespace RC::Unreal;

namespace Palworld {
	PalItemModLoader::PalItemModLoader() : PalModLoaderBase("items") {
        SetDisplayName(TEXT("Item Loader"));
    }

	PalItemModLoader::~PalItemModLoader() {}

	void PalItemModLoader::OnLoad(const std::filesystem::path& loaderPath, const RC::StringType& modName, const EEngineLifecyclePhase& engineLifecyclePhase)
	{
        if (engineLifecyclePhase != EEngineLifecyclePhase::GameInstanceInit)
        {
            return;
        }

        PS::JsonHelpers::ParseJsonFilesInPath(loaderPath, [&](const nlohmann::json& Data) {
            LoadItems(Data);
        });
	}

    void PalItemModLoader::OnAutoReload(const RC::StringType& modName, const std::filesystem::path& modFilePath)
    {
        PS::JsonHelpers::ParseJsonFileInPath(modFilePath, [&](const nlohmann::json& Data) {
            LoadItems(Data);
        });
    }

    bool PalItemModLoader::CanInitialize(const EEngineLifecyclePhase& engineLifecyclePhase)
    {
        if (engineLifecyclePhase == EEngineLifecyclePhase::GameInstanceInit)
        {
            return true;
        }

        return false;
    }

    bool PalItemModLoader::OnInitialize()
    {
        try
        {
            GItemDataAsset = UECustom::UObjectGlobals::StaticFindObject<UPalStaticItemDataAsset*>(nullptr, nullptr,
                TEXT("/Game/Pal/DataAsset/Item/DA_StaticItemDataAsset.DA_StaticItemDataAsset"));

            m_itemDataTable = GetDatatableByName("DT_ItemDataTable");
            m_itemRecipeTable = GetDatatableByName("DT_ItemRecipeDataTable");
            m_nameTranslationTable = GetDatatableByName("DT_ItemNameText");
            m_descriptionTranslationTable = GetDatatableByName("DT_ItemDescriptionText");

            SetupHooks();
        }
        catch (const std::exception& e)
        {
            PS::Log<LogLevel::Error>(TEXT("Unable to initialize {}, {}\n"), GetDisplayName(), RC::to_generic_string(e.what()));
            return false;
        }

        return true;
    }

    void PalItemModLoader::LoadItems(const nlohmann::json& Data)
    {
        for (auto& [Key, Value] : Data.items())
        {
            auto ItemId = FName(RC::to_generic_string(Key), FNAME_Add);
            auto Row = GItemDataAsset->StaticItemDataMap.Find(ItemId);
            if (Value.is_null())
            {
                if (!Row) return;
                GItemDataAsset->StaticItemDataMap.Remove(ItemId);
                PS::Log<RC::LogLevel::Normal>(TEXT("Deleted Item '{}'\n"), ItemId.ToString());
            }
            else
            {
                if (Row)
                {
                    Edit(ItemId, *Row, Value);
                    PS::Log<RC::LogLevel::Normal>(TEXT("Modified Item '{}'\n"), ItemId.ToString());
                }
                else
                {
                    Add(ItemId, Value);
                    PS::Log<RC::LogLevel::Normal>(TEXT("Added Item '{}'\n"), ItemId.ToString());
                }
            }
        }
    }

    void PalItemModLoader::Add(const RC::Unreal::FName& ItemId, const nlohmann::json& Data)
    {
        if (ItemId == NAME_None)
        {
            throw std::runtime_error("ID was set to None");
        }

        FString Type;
        if (!PS::JsonHelpers::GetString(Data, "Type", Type))
        {
            throw std::runtime_error(std::format("You must supply a Type field in '{}' and it must be a string when adding new items",
                RC::to_string(ItemId.ToString())));
        }

        UClass* DatabaseClass = nullptr;
        UClass* DynamicDatabaseClass = nullptr;
        if (Type == TEXT("Armor") || Type == TEXT("PalStaticArmorItemData"))
        {
            DatabaseClass = UPalStaticArmorItemData::StaticClass();
            DynamicDatabaseClass = UPalDynamicArmorItemDataBase::StaticClass();
        }
        else if (Type == TEXT("Weapon") || Type == TEXT("PalStaticWeaponItemData"))
        {
            DatabaseClass = UPalStaticWeaponItemData::StaticClass();
            DynamicDatabaseClass = UPalDynamicWeaponItemDataBase::StaticClass();
        }
        else if (Type == TEXT("Consumable") || Type == TEXT("PalStaticConsumeItemData"))
        {
            DatabaseClass = UPalStaticConsumeItemData::StaticClass();
        }
        else if (Type == TEXT("Generic") || Type == TEXT("PalStaticItemDataBase"))
        {
            DatabaseClass = UPalStaticItemDataBase::StaticClass();
        }
        else
        {
            throw std::runtime_error(RC::fmt("Type %S in %S isn't supported, must be Armor, Weapon, Consumable or Generic", *Type, ItemId.ToString().c_str()));
        }

        if (!DatabaseClass->GetPropertyByNameInChain(TEXT("ID")))
        {
            throw std::runtime_error("Property 'ID' has changed in DA_StaticItemDataAsset. Update to Pal Schema is needed.");
        }

        if (!DatabaseClass->GetPropertyByNameInChain(TEXT("DynamicItemDataClass")))
        {
            throw std::runtime_error("Property 'DynamicItemDataClass' has changed in DA_StaticItemDataAsset. Update to Pal Schema is needed.");
        }

        FString TypeB;
        if (PS::JsonHelpers::GetString(Data, "TypeB", TypeB) && IsThrowableWeapon(TypeB))
        {
            DynamicDatabaseClass = nullptr;
        }

        FStaticConstructObjectParameters ConstructParams(DatabaseClass, GItemDataAsset);
        ConstructParams.Name = NAME_None;

        auto Item = UObjectGlobals::StaticConstructObject<UPalStaticItemDataBase*>(ConstructParams);

        auto IdProperty = Item->GetValuePtrByPropertyNameInChain<FName>(TEXT("ID"));
        if (IdProperty)
        {
            *IdProperty = ItemId;
        }

        auto DynamicItemDataClassProperty = Item->GetValuePtrByPropertyNameInChain<UClass*>(TEXT("DynamicItemDataClass"));
        if (DynamicItemDataClassProperty)
        {
            *DynamicItemDataClassProperty = DynamicDatabaseClass;
        }

        for (const auto& [Key, Value] : Data.items())
        {
            if (Key == "DynamicItemDataClass" || IsCustomProperty(Key))
            {
                // Ignore DynamicItemDataClass and Custom Props.
                continue;
            }

            RC::StringType KeyWide = RC::to_generic_string(Key);
            FProperty* Property = DatabaseClass->GetPropertyByNameInChain(KeyWide.c_str());
            if (Property)
            {
                PropertyHelper::CopyJsonValueToContainer(reinterpret_cast<uint8_t*>(Item), Property, Value);
            }
            else
            {
                PS::Log<LogLevel::Warning>(STR("Property '{}' not found in Item '{}'.\n"), KeyWide, ItemId.ToString());
            }
        }

        if (Data.contains("Recipe"))
        {
            AddRecipe(ItemId, Data.at("Recipe"));
        }

        AddItemData(ItemId, Type, Data);

        AddTranslations(ItemId, Data);

        GItemDataAsset->StaticItemDataMap.Add(ItemId, Item);
	}

	void PalItemModLoader::Edit(const RC::Unreal::FName& ItemId, UPalStaticItemDataBase* Item, const nlohmann::json& Data)
	{
        UClass* ItemClass = Item->GetClassPrivate();
        for (const auto& [Key, Value] : Data.items())
        {
            if (Key == "DynamicItemDataClass" || Key == "ID" || IsCustomProperty(Key))
            {
                // Ignore DynamicItemDataClass, ID and Custom Props.
                // Editing the ID is a bad idea, hence we skip it.
                continue;
            }

            RC::StringType KeyWide = RC::to_generic_string(Key);
            FProperty* Property = ItemClass->GetPropertyByNameInChain(KeyWide.c_str());
            if (Property)
            {
                PropertyHelper::CopyJsonValueToContainer(reinterpret_cast<uint8_t*>(Item), Property, Value);
            }
            else
            {
                PS::Log<LogLevel::Warning>(STR("Property '{}' not found in Item '{}'.\n"), KeyWide, ItemId.ToString());
            }
        }

		if (Data.contains("Recipe"))
		{
			EditRecipe(ItemId, Data.at("Recipe"));
		}

		EditTranslations(ItemId, Data);
	}

	void PalItemModLoader::AddRecipe(const RC::Unreal::FName& ItemId, const nlohmann::json& Recipe)
	{
		auto RowStruct = m_itemRecipeTable->GetRowStruct().Get();

		auto ItemRecipeData = FMemory::Malloc(RowStruct->GetStructureSize());
		RowStruct->InitializeStruct(ItemRecipeData);

		for (auto& [Key, Value] : Recipe.items())
		{
			auto KeyWide = RC::to_generic_string(Key);

			if (KeyWide == TEXT("Product_Id"))
            {
                // We will set this later based on the Key used for the json object, so we skip it for now.
                continue;
            }

            if (KeyWide == TEXT("Editor_RowNameHash"))
			{
				// We don't need to change this due to it being editor related, skip.
				continue;
			}

			auto Property = RowStruct->GetPropertyByName(KeyWide.c_str());
			if (Property)
			{
				try
				{
					PropertyHelper::CopyJsonValueToContainer(ItemRecipeData, Property, Value);
				}
				catch (const std::exception& e)
				{
					FMemory::Free(ItemRecipeData);
					throw std::runtime_error(e.what());
				}
			}
            else
            {
                PS::Log<LogLevel::Warning>(STR("Property '{}' not found in Item Recipe -> '{}'.\n"), KeyWide, ItemId.ToString());
            }
		}

		auto ProductIdProperty = RowStruct->GetPropertyByName(TEXT("Product_Id"));
		if (ProductIdProperty)
		{
			FMemory::Memcpy(ProductIdProperty->ContainerPtrToValuePtr<void>(ItemRecipeData), &ItemId, sizeof(FName));
		}

		m_itemRecipeTable->AddRow(ItemId, *reinterpret_cast<RC::Unreal::FTableRowBase*>(ItemRecipeData));

        PS::Log<LogLevel::Normal>(TEXT("Added new Recipe for Item '{}'.\n"), ItemId.ToString());
	}

	void PalItemModLoader::EditRecipe(const RC::Unreal::FName& ItemId, const nlohmann::json& Recipe)
	{
		auto RowStruct = m_itemRecipeTable->GetRowStruct().Get();

		auto RecipeRow = m_itemRecipeTable->FindRowUnchecked(ItemId);
		if (!RecipeRow)
		{
			throw std::runtime_error(std::format("Row for Recipe '{}' doesn't exist", RC::to_string(ItemId.ToString())));
		}

		for (auto& [Key, Value] : Recipe.items())
		{
            if (Key == "Editor_RowNameHash")
			{
				// We don't need to change this due to it being editor related, skip.
				continue;
			}

            auto KeyWide = RC::to_generic_string(Key);
			auto Property = RowStruct->GetPropertyByName(KeyWide.c_str());
			if (Property)
			{
				PropertyHelper::CopyJsonValueToContainer(RecipeRow, Property, Value);
			}
            else
            {
                PS::Log<LogLevel::Warning>(STR("Property '{}' not found in Item Recipe -> '{}'.\n"), KeyWide, ItemId.ToString());
            }
		}

        PS::Log<LogLevel::Normal>(TEXT("Modified Recipe for Item '{}'.\n"), ItemId.ToString());
	}

	void PalItemModLoader::AddTranslations(const RC::Unreal::FName& ItemId, const nlohmann::json& Data)
	{
		if (Data.contains("Name"))
		{
			auto RowId = std::format(TEXT("ITEM_NAME_{}"), ItemId.ToString());
			auto RowStruct = m_nameTranslationTable->GetRowStruct().Get();
			auto TextDataProperty = RowStruct->GetPropertyByName(TEXT("TextData"));
            if (TextDataProperty)
            {
                auto RowData = FMemory::Malloc(RowStruct->GetStructureSize());
                RowStruct->InitializeStruct(RowData);

                try
                {
                    PropertyHelper::CopyJsonValueToContainer(RowData, TextDataProperty, Data.at("Name"));
                }
                catch (const std::exception& e)
                {
                    FMemory::Free(RowData);
					throw std::runtime_error(e.what());
				}

				m_nameTranslationTable->AddRow(FName(RowId, FNAME_Add), *reinterpret_cast<RC::Unreal::FTableRowBase*>(RowData));
			}
		}

		if (Data.contains("Description"))
		{
			auto RowId = std::format(TEXT("ITEM_DESC_{}"), ItemId.ToString());
            auto RowStruct = m_descriptionTranslationTable->GetRowStruct().Get();
            auto TextDataProperty = RowStruct->GetPropertyByName(TEXT("TextData"));
            if (TextDataProperty)
            {
                auto RowData = FMemory::Malloc(RowStruct->GetStructureSize());
                RowStruct->InitializeStruct(RowData);

                try
                {
                    PropertyHelper::CopyJsonValueToContainer(RowData, TextDataProperty, Data.at("Description"));
                }
                catch (const std::exception& e)
                {
                    FMemory::Free(RowData);
                    throw std::runtime_error(e.what());
                }

                m_descriptionTranslationTable->AddRow(FName(RowId, FNAME_Add), *reinterpret_cast<RC::Unreal::FTableRowBase*>(RowData));
			}
		}
	}

	void PalItemModLoader::EditTranslations(const RC::Unreal::FName& ItemId, const nlohmann::json& Data)
	{
		if (Data.contains("Name"))
		{
			auto RowId = std::format(TEXT("ITEM_NAME_{}"), ItemId.ToString());
			auto RowStruct = m_nameTranslationTable->GetRowStruct().Get();
			auto TextDataProperty = RowStruct->GetPropertyByName(TEXT("TextData"));
			if (TextDataProperty)
			{
				auto Row = m_nameTranslationTable->FindRowUnchecked(FName(RowId, FNAME_Add));
				if (Row)
				{
					PropertyHelper::CopyJsonValueToContainer(Row, TextDataProperty, Data.at("Name"));
				}
			}
		}

		if (Data.contains("Description"))
		{
			auto RowId = std::format(TEXT("ITEM_DESC_{}"), ItemId.ToString());
			auto RowStruct = m_nameTranslationTable->GetRowStruct().Get();
			auto TextDataProperty = RowStruct->GetPropertyByName(TEXT("TextData"));
			if (TextDataProperty)
			{
				auto Row = m_descriptionTranslationTable->FindRowUnchecked(FName(RowId, FNAME_Add));
				if (Row)
				{
					PropertyHelper::CopyJsonValueToContainer(Row, TextDataProperty, Data.at("Description"));
				}
			}
		}
	}

    void PalItemModLoader::AddItemData(const RC::Unreal::FName& ItemId, const FString& Type, const nlohmann::json& Data)
    {
        auto RowStruct = m_itemDataTable->GetRowStruct().Get();
        FManagedStruct RowData{ RowStruct };

        auto SortIdProp = RowStruct->GetPropertyByName(TEXT("SortID"));
        if (SortIdProp && Data.contains("SortID"))
        {
            PropertyHelper::CopyJsonValueToContainer(RowData.GetData(), SortIdProp, Data.at("SortID"));
        }
        else if (SortIdProp && Data.contains("SortId"))
        {
            PropertyHelper::CopyJsonValueToContainer(RowData.GetData(), SortIdProp, Data.at("SortId"));
        }

        auto ItemActorClassProp = RowStruct->GetPropertyByName(TEXT("ItemActorClass"));
        if (ItemActorClassProp && Type == TEXT("Armor"))
        {
            FName ItemActorClassValue = ItemId;
            FMemory::Memcpy(ItemActorClassProp->ContainerPtrToValuePtr<void>(RowData.GetData()), &ItemActorClassValue, sizeof(FName));
        }

        auto LegalProp = RowStruct->GetPropertyByName(TEXT("bLegalInGame"));
        if (LegalProp)
        {
            if (Data.contains("bLegalInGame"))
            {
                PropertyHelper::CopyJsonValueToContainer(RowData.GetData(), LegalProp, Data.at("bLegalInGame"));
            }
            else
            {
                bool LegalValue = true;
                FMemory::Memcpy(LegalProp->ContainerPtrToValuePtr<void>(RowData.GetData()), &LegalValue, sizeof(bool));
            }
        }

        m_itemDataTable->AddRow(ItemId, *reinterpret_cast<RC::Unreal::FTableRowBase*>(RowData.GetData()));
    }

    void PalItemModLoader::SetupHooks()
    {
        try
        {
            auto address = Palworld::SignatureManager::GetSignature("UPalItemSlot::UpdateItem_ServerInternal");
            if (!address)
            {
                throw std::runtime_error("Signature for UPalItemSlot::UpdateItem_ServerInternal could not be found");
            }

            ApplyItemSaveDataAddress = Palworld::SignatureManager::GetSignature("UPalItemContainer::ApplySaveData");
            if (!ApplyItemSaveDataAddress)
            {
                throw std::runtime_error("Signature for UPalItemContainer::ApplySaveData could not be found");
            }

            auto address2 = Palworld::SignatureManager::GetSignature("UPalDynamicItemWorldSubsystem::Create_ServerInternal");
            if (!address2)
            {
                throw std::runtime_error("Signature for UPalDynamicItemWorldSubsystem::Create_ServerInternal could not be found");
            }

            ApplyDynamicItemSaveDataAddress = Palworld::SignatureManager::GetSignature("UPalDynamicItemWorldSubsystem::ApplyWorldSaveData");
            if (!ApplyDynamicItemSaveDataAddress)
            {
                throw std::runtime_error("Signature for UPalDynamicItemWorldSubsystem::ApplyWorldSaveData could not be found");
            }

            auto address3 = Palworld::SignatureManager::GetSignature("ValidateWorldSaveDynamicItemStaticIds");
            if (!address3)
            {
                throw std::runtime_error("Signature for ValidateWorldSaveDynamicItemStaticIds could not be found");
            }

            auto address4 = Palworld::SignatureManager::GetSignature("ValidateDynamicItemSaveData");
            if (!address4)
            {
                throw std::runtime_error("Signature for ValidateDynamicItemSaveData could not be found");
            }

            auto address5 = Palworld::SignatureManager::GetSignature("FPalPlayerRecordDataRepInfoArrayThreadSafe_IntVal::ApplyDataMap");
            if (!address5)
            {
                throw std::runtime_error("Signature for FPalPlayerRecordDataRepInfoArrayThreadSafe_IntVal::ApplyDataMap could not be found");
            }

            CraftItemCount_ApplyDataMapReturnAddress = Palworld::SignatureManager::GetSignature("CraftItemCount_ApplyDataMapReturn");
            if (!CraftItemCount_ApplyDataMapReturnAddress)
            {
                throw std::runtime_error("Signature for CraftItemCount_ApplyDataMapReturn could not be found");
            }

            UpdateItem_ServerInternalHook = safetyhook::create_inline(reinterpret_cast<void*>(address),
                UpdateItem_Detour);

            DynamicItemHook = safetyhook::create_inline(reinterpret_cast<void*>(address2),
                CreateDynamicItemDatabase_Detour);

            // If I have to add anymore detours and signatures for this functionality I'm going to lose it
            // Probably a good reason to finally look into how the FPalMemoryReader works

            ValidateWorldSaveDynamicItemStaticIdsHook = safetyhook::create_inline(reinterpret_cast<void*>(address3),
                ValidateWorldSaveDynamicItemStaticIds);

            ValidateDynamicItemSaveDataHook = safetyhook::create_inline(reinterpret_cast<void*>(address4),
                ValidateDynamicItemSaveData);

            ApplyDataMapHook = safetyhook::create_inline(reinterpret_cast<void*>(address5),
                FPalPlayerRecordDataRepInfoArrayThreadSafe_IntVal_ApplyDataMap);
        }
        catch (const std::exception& e)
        {
            PS::Log<LogLevel::Error>(TEXT("{}. PalSchema will be unable to clean up invalid items and worlds with invalid items will crash on load.\n"), 
                                     RC::to_generic_string(e.what()));
        }
    }

    bool PalItemModLoader::IsCustomProperty(const std::string& Key)
    {
        const std::unordered_set<std::string> CustomProps = { "Type", "Name", "Description", "Recipe", "bLegalInGame" };
        return CustomProps.contains(Key);
    }

    bool PalItemModLoader::IsThrowableWeapon(const RC::Unreal::FString& TypeB)
    {
        if (TypeB == TEXT("EPalItemTypeB::WeaponThrowObject") || TypeB == TEXT("WeaponThrowObject") ||
            TypeB == TEXT("EPalItemTypeB::SPWeaponCaptureRope") || TypeB == TEXT("SPWeaponCaptureRope") ||
            TypeB == TEXT("EPalItemTypeB::SPWeaponCaptureBall") || TypeB == TEXT("SPWeaponCaptureBall"))
        {
            return true;
        }

        return false;
    }

    bool PalItemModLoader::IsValidItem(RC::Unreal::UObject* worldContextObject, const RC::Unreal::FName& staticId)
    {
        auto ItemIdManager = UPalUtility::GetItemIDManager(worldContextObject);
        auto StaticItemData = ItemIdManager->GetStaticItemData(staticId);
        if (StaticItemData)
        {
            return true;
        }
        
        return false;
    }

    void PalItemModLoader::UpdateItem_Detour(RC::Unreal::UObject* self, FPalItemId* ItemId, int amount, bool param4, bool param5)
    {
        if (_ReturnAddress() == ApplyItemSaveDataAddress && !IsValidItem(self, ItemId->StaticId))
        {
            PS::Log<LogLevel::Warning>(TEXT("Item '{}' is invalid. Deleting.\n"), ItemId->StaticId.ToString());
            ItemId->StaticId = NAME_None;
            amount = 0;
        }

        UpdateItem_ServerInternalHook.call(self, ItemId, amount, param4, param5);
    }

    UPalDynamicItemDataBase* PalItemModLoader::CreateDynamicItemDatabase_Detour(RC::Unreal::UObject* self, FPalDynamicItemId* dynamicItemId, RC::Unreal::FName staticId, void* itemCreateParam)
    {
        if (_ReturnAddress() == ApplyDynamicItemSaveDataAddress && !IsValidItem(self, staticId))
        {
            PS::Log<LogLevel::Warning>(TEXT("Item '{}' is invalid. Dynamic Data for this item will be deleted on next save.\n"), staticId.ToString());

            // Shouldn't matter what we use as the staticId as long as it meets the two conditions:
            // 1. Must exist in the vanilla game
            // 2. Has a DynamicItemDataBase (Weapon, Armor, Egg)
            staticId = FName(TEXT("ClothArmor"));
            auto dynamicItemDatabase = DynamicItemHook.call<UPalDynamicItemDataBase*>(self, dynamicItemId, staticId, itemCreateParam);

            // This makes it so that when the game saves, this dynamic Data will be excluded which permanently removes it from the save.
            dynamicItemDatabase->GetIgnoreOnSave() = true;
            return dynamicItemDatabase;
        }

        return DynamicItemHook.call<UPalDynamicItemDataBase*>(self, dynamicItemId, staticId, itemCreateParam);
    }

    bool PalItemModLoader::ValidateWorldSaveDynamicItemStaticIds(UObject* idk, UObject* SaveGame,FString& idk3, FString& idk4)
    {
        return true;
    }

    bool PalItemModLoader::ValidateDynamicItemSaveData(void* idk, UObject* DynamicItemDataBase, UObject* ItemIDManager, const RC::StringType& idk2)
    {
        return true;
    }

    void PalItemModLoader::FPalPlayerRecordDataRepInfoArrayThreadSafe_IntVal_ApplyDataMap(void* self, const RC::Unreal::TMap<RC::Unreal::FName, RC::Unreal::int32>& MapToApply)
    {
        /*
        * This fixes crashing related to craft item counts in UPalUserAchievementChecker::CheckCraftCount for custom items that were uninstalled-
        * but still exist in the save.
        */
        if (_ReturnAddress() == CraftItemCount_ApplyDataMapReturnAddress)
        {
            RC::Unreal::TMap<RC::Unreal::FName, RC::Unreal::int32> NewMap;
            for (auto& [StaticItemId, Count] : MapToApply)
            {
                if (GItemDataAsset->StaticItemDataMap.Contains(StaticItemId))
                {
                    NewMap.Add(StaticItemId, Count);
                }
                else
                {
                    PS::Log<LogLevel::Warning>(TEXT("Item '{}' is invalid. Craft Item Count for this item will be deleted on next save.\n"), StaticItemId.ToString());
                }
            }

            ApplyDataMapHook.call(self, NewMap);
        }
        else
        {
            ApplyDataMapHook.call(self, MapToApply);
        }
    }
}#include "Unreal/CoreUObject/UObject/UnrealType.hpp"
#include "Unreal/Engine/UDataTable.hpp"
#include "SDK/Classes/Custom/UObjectGlobals.h"
#include "SDK/Classes/PalItemIDManager.h"
#include "SDK/Classes/PalStaticItemDataTable.h"
#include "SDK/Classes/PalStaticArmorItemData.h"
#include "SDK/Classes/PalStaticConsumeItemData.h"
#include "SDK/Classes/PalStaticWeaponItemData.h"
#include "SDK/Classes/PalDynamicWeaponItemDataBase.h"
#include "SDK/Classes/PalDynamicArmorItemDataBase.h"
#include "SDK/Classes/PalUtility.h"
#include "SDK/Structs/FPalCharacterIconDataRow.h"
#include "SDK/Structs/Custom/FManagedStruct.h"
#include "SDK/Helper/PropertyHelper.h"
#include "Helpers/String.hpp"
#include "Utility/Logging.h"
#include "Utility/JsonHelpers.h"
#include "Loader/PalItemModLoader.h"
#include <SDK/Structs/FPalLocalizedTextData.h>
#include <SDK/PalSignatures.h>

using namespace RC;
using namespace RC::Unreal;

namespace Palworld {
	PalItemModLoader::PalItemModLoader() : PalModLoaderBase("items") {
        SetDisplayName(TEXT("Item Loader"));
    }

	PalItemModLoader::~PalItemModLoader() {}

	void PalItemModLoader::OnLoad(const std::filesystem::path& loaderPath, const RC::StringType& modName, const EEngineLifecyclePhase& engineLifecyclePhase)
	{
        if (engineLifecyclePhase != EEngineLifecyclePhase::GameInstanceInit)
        {
            return;
        }

        PS::JsonHelpers::ParseJsonFilesInPath(loaderPath, [&](const nlohmann::json& data) {
            LoadItems(data);
        });
	}

    void PalItemModLoader::OnAutoReload(const RC::StringType& modName, const std::filesystem::path& modFilePath)
    {
        PS::JsonHelpers::ParseJsonFileInPath(modFilePath, [&](const nlohmann::json& data) {
            LoadItems(data);
        });
    }

    bool PalItemModLoader::CanInitialize(const EEngineLifecyclePhase& engineLifecyclePhase)
    {
        if (engineLifecyclePhase == EEngineLifecyclePhase::GameInstanceInit)
        {
            return true;
        }

        return false;
    }

    bool PalItemModLoader::OnInitialize()
    {
        try
        {
            m_itemDataAsset = UECustom::UObjectGlobals::StaticFindObject<UPalStaticItemDataAsset*>(nullptr, nullptr,
                TEXT("/Game/Pal/DataAsset/Item/DA_StaticItemDataAsset.DA_StaticItemDataAsset"));

            m_itemDataTable = GetDatatableByName("DT_ItemDataTable");
            m_itemRecipeTable = GetDatatableByName("DT_ItemRecipeDataTable");
            m_nameTranslationTable = GetDatatableByName("DT_ItemNameText");
            m_descriptionTranslationTable = GetDatatableByName("DT_ItemDescriptionText");

            SetupHooks();
        }
        catch (const std::exception& e)
        {
            PS::Log<LogLevel::Error>(TEXT("Unable to initialize {}, {}\n"), GetDisplayName(), RC::to_generic_string(e.what()));
            return false;
        }

        return true;
    }

    void PalItemModLoader::LoadItems(const nlohmann::json& data)
    {
        for (auto& [key, value] : data.items())
        {
            auto itemId = FName(RC::to_generic_string(key), FNAME_Add);
            auto row = m_itemDataAsset->StaticItemDataMap.Find(itemId);
            if (value.is_null())
            {
                if (!row) return;
                m_itemDataAsset->StaticItemDataMap.Remove(itemId);
                PS::Log<RC::LogLevel::Normal>(TEXT("Deleted Item '{}'\n"), itemId.ToString());
            }
            else
            {
                if (row)
                {
                    Edit(itemId, *row, value);
                    PS::Log<RC::LogLevel::Normal>(TEXT("Modified Item '{}'\n"), itemId.ToString());
                }
                else
                {
                    Add(itemId, value);
                    PS::Log<RC::LogLevel::Normal>(TEXT("Added Item '{}'\n"), itemId.ToString());
                }
            }
        }
    }

	void PalItemModLoader::Add(const RC::Unreal::FName& itemId, const nlohmann::json& data)
	{
		if (itemId == NAME_None)
		{
			throw std::runtime_error("ID was set to None");
		}

		if (!data.contains("Type"))
		{
			throw std::runtime_error(std::format("You must supply a Type field in {} when adding new items", RC::to_string(itemId.ToString())));
		}

		if (!data.at("Type").is_string())
		{
			throw std::runtime_error(std::format("Type must be a string in {}", RC::to_string(itemId.ToString())));
		}

		auto type = data.at("Type").get<std::string>();

		UClass* databaseClass = nullptr;
		UClass* dynamicDatabaseClass = nullptr;
		if (type == "Armor" || type == "PalStaticArmorItemData")
		{
			databaseClass = UPalStaticArmorItemData::StaticClass();
			dynamicDatabaseClass = UPalDynamicArmorItemDataBase::StaticClass();
		}
		else if (type == "Weapon" || type == "PalStaticWeaponItemData")
		{
			databaseClass = UPalStaticWeaponItemData::StaticClass();
			dynamicDatabaseClass = UPalDynamicWeaponItemDataBase::StaticClass();
		}
		else if (type == "Consumable" || type == "PalStaticConsumeItemData")
		{
			databaseClass = UPalStaticConsumeItemData::StaticClass();
		}
		else if (type == "Generic" || type == "PalStaticItemDataBase")
		{
			databaseClass = UPalStaticItemDataBase::StaticClass();
		}
		else
		{
			throw std::runtime_error(std::format("Type {} in {} isn't supported, must be Armor, Weapon, Consumable or Generic", type, RC::to_string(itemId.ToString())));
		}

		if (!databaseClass->GetPropertyByNameInChain(TEXT("ID")))
		{
			throw std::runtime_error("Property 'ID' has changed in DA_StaticItemDataAsset. Update to Pal Schema is needed.");
		}

		if (!databaseClass->GetPropertyByNameInChain(TEXT("DynamicItemDataClass")))
		{
			throw std::runtime_error("Property 'DynamicItemDataClass' has changed in DA_StaticItemDataAsset. Update to Pal Schema is needed.");
		}

		FStaticConstructObjectParameters constructParams(databaseClass, m_itemDataAsset);
		constructParams.Name = NAME_None;

		auto item = UObjectGlobals::StaticConstructObject<UPalStaticItemDataBase*>(constructParams);

		auto idProperty = item->GetValuePtrByPropertyNameInChain<FName>(TEXT("ID"));
		if (idProperty)
		{
			*idProperty = itemId;
		}

		auto dynamicItemDataClassProperty = item->GetValuePtrByPropertyNameInChain<UClass*>(TEXT("DynamicItemDataClass"));
		if (dynamicItemDataClassProperty)
		{
			*dynamicItemDataClassProperty = dynamicDatabaseClass;
		}

        for (FProperty* property : TFieldRange<FProperty>(databaseClass, EFieldIterationFlags::IncludeSuper))
        {
            auto propertyName = RC::to_string(property->GetName());
            if (propertyName == "DynamicItemDataClass")
            {
                // We've already set this earlier so we skip it.
                continue;
            }
            if (data.contains(propertyName))
            {
                PropertyHelper::CopyJsonValueToContainer(reinterpret_cast<uint8_t*>(item), property, data.at(propertyName));
            }
        }
		
		if (data.contains("Recipe"))
		{
			AddRecipe(itemId, data.at("Recipe"));
		}

        AddItemData(itemId, data);

		AddTranslations(itemId, data);

		m_itemDataAsset->StaticItemDataMap.Add(itemId, item);
	}

	void PalItemModLoader::Edit(const RC::Unreal::FName& itemId, UPalStaticItemDataBase* item, const nlohmann::json& data)
	{
        for (FProperty* property : TFieldRange<FProperty>(item->GetClassPrivate(), EFieldIterationFlags::IncludeSuper))
        {
            auto propertyName = RC::to_string(property->GetName());
            if (propertyName == "DynamicItemDataClass")
            {
                continue;
            }
            if (propertyName == "ID")
            {
                // Editing the ID is a bad idea, hence we skip it.
                continue;
            }
            if (data.contains(propertyName))
            {
                PropertyHelper::CopyJsonValueToContainer(reinterpret_cast<uint8_t*>(item), property, data.at(propertyName));
            }
        }

		if (data.contains("Recipe"))
		{
			EditRecipe(itemId, data.at("Recipe"));
		}

		EditTranslations(itemId, data);
	}

	void PalItemModLoader::AddRecipe(const RC::Unreal::FName& itemId, const nlohmann::json& recipe)
	{
		auto rowStruct = m_itemRecipeTable->GetRowStruct().Get();

		auto itemRecipeData = FMemory::Malloc(rowStruct->GetStructureSize());
		rowStruct->InitializeStruct(itemRecipeData);

		for (auto& [propertyName, propertyValue] : recipe.items())
		{
			auto propertyNameWide = RC::to_generic_string(propertyName);

			if (propertyNameWide == TEXT("Product_Id"))
            {
                // We will set this later based on the key used for the json object, so we skip it for now.
                continue;
            }

            if (propertyNameWide == TEXT("Editor_RowNameHash"))
			{
				// We don't need to change this due to it being editor related, skip.
				continue;
			}

			auto property = rowStruct->GetPropertyByName(propertyNameWide.c_str());
			if (property)
			{
				try
				{
					PropertyHelper::CopyJsonValueToContainer(itemRecipeData, property, propertyValue);
				}
				catch (const std::exception& e)
				{
					FMemory::Free(itemRecipeData);
					throw std::runtime_error(e.what());
				}
			}
		}

		auto productIdProperty = rowStruct->GetPropertyByName(TEXT("Product_Id"));
		if (productIdProperty)
		{
			FMemory::Memcpy(productIdProperty->ContainerPtrToValuePtr<void>(itemRecipeData), &itemId, sizeof(FName));
		}

		m_itemRecipeTable->AddRow(itemId, *reinterpret_cast<RC::Unreal::FTableRowBase*>(itemRecipeData));

        PS::Log<LogLevel::Normal>(TEXT("Added new Recipe for Item '{}'.\n"), itemId.ToString());
	}

	void PalItemModLoader::EditRecipe(const RC::Unreal::FName& itemId, const nlohmann::json& recipe)
	{
		auto rowStruct = m_itemRecipeTable->GetRowStruct().Get();

		auto recipeRow = m_itemRecipeTable->FindRowUnchecked(itemId);
		if (!recipeRow)
		{
			throw std::runtime_error(std::format("Row for Recipe '{}' doesn't exist", RC::to_string(itemId.ToString())));
		}

		for (auto& [propertyName, propertyValue] : recipe.items())
		{
			auto propertyNameWide = RC::to_generic_string(propertyName);
            if (propertyNameWide == TEXT("Editor_RowNameHash"))
			{
				// We don't need to change this due to it being editor related, skip.
				continue;
			}

			auto property = rowStruct->GetPropertyByName(propertyNameWide.c_str());
			if (property)
			{
				PropertyHelper::CopyJsonValueToContainer(recipeRow, property, propertyValue);
			}
		}

        PS::Log<LogLevel::Normal>(TEXT("Modified Recipe for Item '{}'.\n"), itemId.ToString());
	}

	void PalItemModLoader::AddTranslations(const RC::Unreal::FName& itemId, const nlohmann::json& data)
	{
		if (data.contains("Name"))
		{
			auto rowId = std::format(TEXT("ITEM_NAME_{}"), itemId.ToString());
			auto rowStruct = m_nameTranslationTable->GetRowStruct().Get();
			auto textDataProperty = rowStruct->GetPropertyByName(TEXT("TextData"));
            if (textDataProperty)
            {
                auto rowData = FMemory::Malloc(rowStruct->GetStructureSize());
                rowStruct->InitializeStruct(rowData);

                try
                {
                    PropertyHelper::CopyJsonValueToContainer(rowData, textDataProperty, data.at("Name"));
                }
                catch (const std::exception& e)
                {
                    FMemory::Free(rowData);
					throw std::runtime_error(e.what());
				}

				m_nameTranslationTable->AddRow(FName(rowId, FNAME_Add), *reinterpret_cast<RC::Unreal::FTableRowBase*>(rowData));
			}
		}

		if (data.contains("Description"))
		{
			auto rowId = std::format(TEXT("ITEM_DESC_{}"), itemId.ToString());
            auto rowStruct = m_descriptionTranslationTable->GetRowStruct().Get();
            auto textDataProperty = rowStruct->GetPropertyByName(TEXT("TextData"));
            if (textDataProperty)
            {
                auto rowData = FMemory::Malloc(rowStruct->GetStructureSize());
                rowStruct->InitializeStruct(rowData);

                try
                {
                    PropertyHelper::CopyJsonValueToContainer(rowData, textDataProperty, data.at("Description"));
                }
                catch (const std::exception& e)
                {
                    FMemory::Free(rowData);
                    throw std::runtime_error(e.what());
                }

                m_descriptionTranslationTable->AddRow(FName(rowId, FNAME_Add), *reinterpret_cast<RC::Unreal::FTableRowBase*>(rowData));
			}
		}
	}

	void PalItemModLoader::EditTranslations(const RC::Unreal::FName& itemId, const nlohmann::json& data)
	{
		if (data.contains("Name"))
		{
			auto rowId = std::format(TEXT("ITEM_NAME_{}"), itemId.ToString());
			auto rowStruct = m_nameTranslationTable->GetRowStruct().Get();
			auto textDataProperty = rowStruct->GetPropertyByName(TEXT("TextData"));
			if (textDataProperty)
			{
				auto row = m_nameTranslationTable->FindRowUnchecked(FName(rowId, FNAME_Add));
				if (row)
				{
					PropertyHelper::CopyJsonValueToContainer(row, textDataProperty, data.at("Name"));
				}
			}
		}

		if (data.contains("Description"))
		{
			auto rowId = std::format(TEXT("ITEM_DESC_{}"), itemId.ToString());
			auto rowStruct = m_nameTranslationTable->GetRowStruct().Get();
			auto textDataProperty = rowStruct->GetPropertyByName(TEXT("TextData"));
			if (textDataProperty)
			{
				auto row = m_descriptionTranslationTable->FindRowUnchecked(FName(rowId, FNAME_Add));
				if (row)
				{
					PropertyHelper::CopyJsonValueToContainer(row, textDataProperty, data.at("Description"));
				}
			}
		}
	}

    void PalItemModLoader::AddItemData(const RC::Unreal::FName& itemId, const nlohmann::json& data)
    {
        auto rowStruct = m_itemDataTable->GetRowStruct().Get();

        auto legalProp = rowStruct->GetPropertyByName(TEXT("bLegalInGame"));
        if (!legalProp)
        {
            PS::Log<LogLevel::Error>(TEXT("Property 'bLegalInGame' does not exist in DT_ItemDataTable, skipping addition of data for {}.\n"), itemId.ToString());
            return;
        }

        FManagedStruct rowData{ rowStruct };

        if (data.contains("bLegalInGame"))
        {
            PropertyHelper::CopyJsonValueToContainer(rowData.GetData(), legalProp, data.at("bLegalInGame"));
        }
        else
        {
            bool legalValue = true;
            FMemory::Memcpy(legalProp->ContainerPtrToValuePtr<void>(rowData.GetData()), &legalValue, sizeof(bool));
        }

        m_itemDataTable->AddRow(itemId, *reinterpret_cast<RC::Unreal::FTableRowBase*>(rowData.GetData()));
    }

    void PalItemModLoader::SetupHooks()
    {
        try
        {
            auto address = Palworld::SignatureManager::GetSignature("UPalItemSlot::UpdateItem_ServerInternal");
            if (!address)
            {
                throw std::runtime_error("Signature for UPalItemSlot::UpdateItem_ServerInternal could not be found");
            }

            ApplyItemSaveDataAddress = Palworld::SignatureManager::GetSignature("UPalItemContainer::ApplySaveData");
            if (!ApplyItemSaveDataAddress)
            {
                throw std::runtime_error("Signature for UPalItemContainer::ApplySaveData could not be found");
            }

            auto address2 = Palworld::SignatureManager::GetSignature("UPalDynamicItemWorldSubsystem::Create_ServerInternal");
            if (!address2)
            {
                throw std::runtime_error("Signature for UPalDynamicItemWorldSubsystem::Create_ServerInternal could not be found");
            }

            ApplyDynamicItemSaveDataAddress = Palworld::SignatureManager::GetSignature("UPalDynamicItemWorldSubsystem::ApplyWorldSaveData");
            if (!ApplyDynamicItemSaveDataAddress)
            {
                throw std::runtime_error("Signature for UPalDynamicItemWorldSubsystem::ApplyWorldSaveData could not be found");
            }

            auto address3 = Palworld::SignatureManager::GetSignature("ValidateWorldSaveDynamicItemStaticIds");
            if (!address3)
            {
                throw std::runtime_error("Signature for ValidateWorldSaveDynamicItemStaticIds could not be found");
            }

            auto address4 = Palworld::SignatureManager::GetSignature("ValidateDynamicItemSaveData");
            if (!address4)
            {
                throw std::runtime_error("Signature for ValidateDynamicItemSaveData could not be found");
            }

            UpdateItem_ServerInternalHook = safetyhook::create_inline(reinterpret_cast<void*>(address),
                UpdateItem_Detour);

            DynamicItemHook = safetyhook::create_inline(reinterpret_cast<void*>(address2),
                CreateDynamicItemDatabase_Detour);

            // If I have to add anymore detours and signatures for this functionality I'm going to lose it
            // Probably a good reason to finally look into how the FPalMemoryReader works

            ValidateWorldSaveDynamicItemStaticIdsHook = safetyhook::create_inline(reinterpret_cast<void*>(address3),
                ValidateWorldSaveDynamicItemStaticIds);

            ValidateDynamicItemSaveDataHook = safetyhook::create_inline(reinterpret_cast<void*>(address4),
                ValidateDynamicItemSaveData);
        }
        catch (const std::exception& e)
        {
            PS::Log<LogLevel::Error>(TEXT("{}. PalSchema will be unable to clean up invalid items and worlds with invalid items will crash on load.\n"), 
                                     RC::to_generic_string(e.what()));
        }
    }

    bool PalItemModLoader::IsValidItem(RC::Unreal::UObject* worldContextObject, const RC::Unreal::FName& staticId)
    {
        auto itemIdManager = UPalUtility::GetItemIDManager(worldContextObject);
        auto staticItemData = itemIdManager->GetStaticItemData(staticId);
        if (staticItemData)
        {
            return true;
        }
        
        return false;
    }

    void PalItemModLoader::UpdateItem_Detour(RC::Unreal::UObject* self, FPalItemId* itemId, int amount, bool param4, bool param5)
    {
        if (_ReturnAddress() == ApplyItemSaveDataAddress && !IsValidItem(self, itemId->StaticId))
        {
            PS::Log<LogLevel::Warning>(TEXT("Item '{}' is invalid. Deleting.\n"), itemId->StaticId.ToString());
            itemId->StaticId = NAME_None;
            amount = 0;
        }

        UpdateItem_ServerInternalHook.call(self, itemId, amount, param4, param5);
    }

    UPalDynamicItemDataBase* PalItemModLoader::CreateDynamicItemDatabase_Detour(RC::Unreal::UObject* self, FPalDynamicItemId* dynamicItemId, RC::Unreal::FName staticId, void* itemCreateParam)
    {
        if (_ReturnAddress() == ApplyDynamicItemSaveDataAddress && !IsValidItem(self, staticId))
        {
            PS::Log<LogLevel::Warning>(TEXT("Item '{}' is invalid. Dynamic data for this item will be deleted on next save.\n"), staticId.ToString());

            // Shouldn't matter what we use as the staticId as long as it meets the two conditions:
            // 1. Must exist in the vanilla game
            // 2. Has a DynamicItemDataBase (Weapon, Armor, Egg)
            staticId = FName(TEXT("ClothArmor"));
            auto dynamicItemDatabase = DynamicItemHook.call<UPalDynamicItemDataBase*>(self, dynamicItemId, staticId, itemCreateParam);

            // This makes it so that when the game saves, this dynamic data will be excluded which permanently removes it from the save.
            dynamicItemDatabase->GetIgnoreOnSave() = true;
            return dynamicItemDatabase;
        }

        return DynamicItemHook.call<UPalDynamicItemDataBase*>(self, dynamicItemId, staticId, itemCreateParam);
    }

    bool PalItemModLoader::ValidateWorldSaveDynamicItemStaticIds(UObject* idk, UObject* SaveGame,FString& idk3, FString& idk4)
    {
        return true;
    }

    bool PalItemModLoader::ValidateDynamicItemSaveData(void* idk, UObject* DynamicItemDataBase, UObject* ItemIDManager, const RC::StringType& idk2)
    {
        return true;
    }
}