#include "TweakChangeset.hpp"

bool App::TweakChangeset::SetFlat(RED4ext::TweakDBID aFlatId, const RED4ext::CBaseRTTIType* aType,
                                  const Core::SharedPtr<void>& aValue)
{
    if (!aFlatId.IsValid() || !aType || !aValue)
        return false;

    auto& entry = m_pendingFlats[aFlatId];
    entry.type = aType;
    entry.value = aValue;

    // Overwrite relative changes
    m_pendingAlterings.erase(aFlatId);

    return true;
}

bool App::TweakChangeset::MakeRecord(RED4ext::TweakDBID aRecordId, const RED4ext::CClass* aType,
                                     RED4ext::TweakDBID aSourceId)
{
    if (!aRecordId.IsValid() || !aType)
        return false;

    auto& entry = m_pendingRecords[aRecordId];

    if (!entry.type)
        m_orderedRecords.push_back(aRecordId);

    entry.type = aType;
    entry.sourceId = aSourceId;

    return true;
}

bool App::TweakChangeset::UpdateRecord(RED4ext::TweakDBID aRecordId)
{
    if (!aRecordId.IsValid())
        return false;

    if (!m_pendingRecords.contains(aRecordId))
    {
        m_pendingRecords.insert({aRecordId, {}});
        m_orderedRecords.push_back(aRecordId);
    }

    return true;
}

bool App::TweakChangeset::AssociateRecord(RED4ext::TweakDBID aRecordId, RED4ext::TweakDBID aFlatId)
{
    if (!aRecordId.IsValid() || !aFlatId.IsValid())
        return false;

    m_flatToRecordMap[aFlatId] = aRecordId;

    return true;
}

bool App::TweakChangeset::AppendElement(RED4ext::TweakDBID aFlatId, const RED4ext::CBaseRTTIType* aType,
                                        const Core::SharedPtr<void>& aValue, bool aUnique)
{
    if (!aFlatId.IsValid() || !aType || !aValue)
        return false;

    auto& entry = m_pendingAlterings[aFlatId];
    entry.appendings.emplace_back(aType, aValue, aUnique);

    return true;
}

bool App::TweakChangeset::PrependElement(RED4ext::TweakDBID aFlatId, const RED4ext::CBaseRTTIType* aType,
                                         const Core::SharedPtr<void>& aValue, bool aUnique)
{
    if (!aFlatId.IsValid() || !aType || !aValue)
        return false;

    auto& entry = m_pendingAlterings[aFlatId];
    entry.prependings.emplace_back(aType, aValue, aUnique);

    return true;
}

bool App::TweakChangeset::RemoveElement(RED4ext::TweakDBID aFlatId, const RED4ext::CBaseRTTIType* aType,
                                        const Core::SharedPtr<void>& aValue)
{
    if (!aFlatId.IsValid() || !aType || !aValue)
        return false;

    auto& entry = m_pendingAlterings[aFlatId];
    entry.deletions.emplace_back(aType, aValue);

    return true;
}

bool App::TweakChangeset::AppendFrom(RED4ext::TweakDBID aFlatId, RED4ext::TweakDBID aSourceId)
{
    if (!aFlatId.IsValid() || !aSourceId.IsValid())
        return false;

    auto& entry = m_pendingAlterings[aFlatId];
    entry.appendingMerges.emplace_back(aSourceId);

    return true;
}

bool App::TweakChangeset::PrependFrom(RED4ext::TweakDBID aFlatId, RED4ext::TweakDBID aSourceId)
{
    if (!aFlatId.IsValid() || !aSourceId.IsValid())
        return false;

    auto& entry = m_pendingAlterings[aFlatId];
    entry.prependingMerges.emplace_back(aSourceId);

    return true;
}

bool App::TweakChangeset::InheritChanges(RED4ext::TweakDBID aFlatId, RED4ext::TweakDBID aBaseId)
{
    if (!aFlatId.IsValid() || !aBaseId.IsValid())
        return false;

    auto alteringIt = m_pendingAlterings.find(aBaseId);
    if (alteringIt == m_pendingAlterings.end())
        return false;

    auto& entry = alteringIt.value();
    m_pendingAlterings[aFlatId] = entry;

    return false;
}

bool App::TweakChangeset::RegisterName(RED4ext::TweakDBID aId, const std::string& aName)
{
    m_pendingNames[aId] = aName;

    return true;
}

const App::TweakChangeset::FlatEntry* App::TweakChangeset::GetFlat(RED4ext::TweakDBID aFlatId)
{
    const auto it = m_pendingFlats.find(aFlatId);

    if (it == m_pendingFlats.end())
        return nullptr;

    return &it->second;
}

const App::TweakChangeset::RecordEntry* App::TweakChangeset::GetRecord(RED4ext::TweakDBID aRecordId)
{
    const auto it = m_pendingRecords.find(aRecordId);

    if (it == m_pendingRecords.end())
        return nullptr;

    return &it->second;
}

bool App::TweakChangeset::HasRecord(RED4ext::TweakDBID aRecordId)
{
    return m_pendingRecords.find(aRecordId) != m_pendingRecords.end();
}

bool App::TweakChangeset::IsEmpty()
{
    return m_pendingFlats.empty() && m_pendingRecords.empty() && m_pendingAlterings.empty() && m_pendingNames.empty();
}

void App::TweakChangeset::Commit(Core::SharedPtr<Red::TweakDB::Manager>& aManager,
                                 Core::SharedPtr<App::TweakChangelog>& aChangelog)
{
    if (!aManager)
    {
        return;
    }

    if (aChangelog)
    {
        aChangelog->RevertChanges(aManager);
        aChangelog->ForgetForeignKeys();
    }

    aManager->StartBatch();

    for (const auto& item : m_pendingNames)
    {
        aManager->RegisterName(item.first, item.second);
    }

    for (const auto& item : m_pendingFlats)
    {
        const auto& flatId = item.first;
        const auto& flatType = item.second.type;
        const auto& flatValue = item.second.value.get();

        const auto success = aManager->SetFlat(flatId, flatType, flatValue);

        if (!success)
        {
            LogError("Can't set flat [{}].", AsString(flatId));
            continue;
        }

        if (aChangelog)
        {
            if (Red::TweakDB::IsForeignKey(flatType))
            {
                const auto foreignKey = reinterpret_cast<RED4ext::TweakDBID*>(flatValue);
                aChangelog->RegisterForeignKey(*foreignKey);
            }
            else if (Red::TweakDB::IsForeignKeyArray(flatType))
            {
                const auto foreignKeyList = reinterpret_cast<RED4ext::DynArray<RED4ext::TweakDBID>*>(flatValue);
                for (const auto& foreignKey : *foreignKeyList)
                {
                    aChangelog->RegisterForeignKey(foreignKey);
                }
            }
        }
    }

    for (const auto& recordId : m_orderedRecords)
    {
        const auto& recordEntry = m_pendingRecords[recordId];

        if (aManager->IsRecordExists(recordId))
        {
            const auto success = aManager->UpdateRecord(recordId);

            if (!success)
            {
                LogError("Cannot update record [{}].", AsString(recordId));
                continue;
            }
        }
        else if (recordEntry.sourceId.IsValid())
        {
            const auto success = aManager->CloneRecord(recordId, recordEntry.sourceId);

            if (!success)
            {
                LogError("Cannot clone record [{}] from [{}].", AsString(recordId), AsString(recordEntry.sourceId));
                continue;
            }
        }
        else
        {
            const auto success = aManager->CreateRecord(recordId, recordEntry.type);

            if (!success)
            {
                LogError("Cannot create record [{}] of type [{}].", AsString(recordId), AsString(recordEntry.type));
                continue;
            }
        }
    }

    aManager->CommitBatch();

    Core::Set<RED4ext::TweakDBID> postUpdates;

    for (const auto& altering : m_pendingAlterings)
    {
        const auto& flatId = altering.first;
        const auto& flatData = aManager->GetFlat(flatId);

        if (!flatData.value)
        {
            LogError("Cannot apply changes to [{}], the flat doesn't exist.", AsString(flatId));
            continue;
        }

        if (flatData.type->GetType() != RED4ext::ERTTIType::Array)
        {
            LogError("Cannot apply changes to [{}], it's not an array.", AsString(flatId));
            continue;
        }

        auto* targetType = reinterpret_cast<RED4ext::CRTTIArrayType*>(flatData.type);
        auto* elementType = targetType->innerType;

        // The data returned by manager is a pointer to the TweakDB flat buffer,
        // we must make a copy of the original array for modifications.
        auto targetArray = Red::TweakDB::MakeDefault(targetType);
        targetType->Assign(targetArray.get(), flatData.value);

        Core::Vector<ElementChange> deletions;
        Core::Vector<ElementChange> insertions;

        for (const auto& deletion : altering.second.deletions)
        {
            const auto deletionValue = deletion.value;
            const auto deletionIndex = FindElement(targetType, targetArray.get(), deletionValue.get());

            if (deletionIndex >= 0)
            {
                deletions.emplace_back(deletionIndex, deletionValue);
            }
        }

        if (!deletions.empty())
        {
            std::sort(deletions.begin(), deletions.end(), [](ElementChange& a, ElementChange& b)
                                                          {
                                                              return a.first > b.first;
                                                          });

            for (const auto& [deletionIndex, deletionEntry] : deletions)
            {
                targetType->RemoveAt(targetArray.get(), deletionIndex);
            }
        }

        {
            InsertionHandler inserter{flatId, targetType, elementType, targetArray, insertions, aManager, *this};
            inserter.Apply(altering.second.prependings, altering.second.prependingMerges, 0);
            inserter.Apply(altering.second.appendings, altering.second.appendingMerges,
                           static_cast<int32_t>(targetType->GetLength(targetArray.get())));
        }

        const auto success = aManager->SetFlat(flatId, targetType, targetArray.get());

        if (!success)
        {
            LogError("Cannot assign flat value [{}].", AsString(flatId));
            continue;
        }

        {
            const auto association = m_flatToRecordMap.find(flatId);

            if (association != m_flatToRecordMap.end())
            {
                const auto& recordId = association.value();
                postUpdates.insert(recordId);

                if (aChangelog)
                {
                    aChangelog->AssociateRecord(recordId, flatId);
                }
            }
        }

        if (aChangelog)
        {
            const auto isForeignKey = Red::TweakDB::IsForeignKeyArray(targetType);

            for (const auto& [deletionIndex, deletionValue] : deletions)
            {
                aChangelog->RegisterDeletion(flatId, deletionIndex, deletionValue);
            }

            for (const auto& [insertionIndex, insertionValue] : insertions)
            {
                aChangelog->RegisterInsertion(flatId, insertionIndex, insertionValue);

                if (isForeignKey)
                {
                    const auto foreignKey = reinterpret_cast<RED4ext::TweakDBID*>(insertionValue.get());
                    aChangelog->RegisterForeignKey(*foreignKey);
                    aChangelog->RegisterName(*foreignKey, AsString(*foreignKey));
                }
            }

            aChangelog->RegisterName(flatId, AsString(flatId));
        }
    }

    for (const auto recordId : postUpdates)
    {
        aManager->UpdateRecord(recordId);
    }

    m_pendingFlats.clear();
    m_pendingRecords.clear();
    m_orderedRecords.clear();
    m_pendingNames.clear();
    m_pendingAlterings.clear();
}

void App::TweakChangeset::InsertionHandler::Apply(const Core::Vector<App::TweakChangeset::InsertionEntry>& aInsertions,
                                                    const Core::Vector<App::TweakChangeset::MergingEntry>& aMerges,
                                                    int32_t aStargIndex)
{
    auto appendingIndex = aStargIndex;

    for (const auto& insertion : aInsertions)
    {
        const auto insertionValue = insertion.value;

        if (insertion.unique && InArray(m_arrayType, m_array.get(), insertionValue.get()))
            continue;

        m_arrayType->InsertAt(m_array.get(), appendingIndex);
        m_elementType->Assign(m_arrayType->GetElement(m_array.get(), appendingIndex), insertionValue.get());

        m_changes.emplace_back(appendingIndex, insertionValue);

        ++appendingIndex;
    }

    for (const auto& merge : aMerges)
    {
        const auto sourceData = m_manager->GetFlat(merge.sourceId);

        if (!sourceData.value || sourceData.type != m_arrayType)
        {
            LogError("Cannot merge [{}] with [{}] because it's not an array.",
                     m_changeset.AsString(merge.sourceId), m_changeset.AsString(m_arrayId));
            continue;
        }

        auto* sourceArray = reinterpret_cast<RED4ext::DynArray<void>*>(sourceData.value);
        const auto sourceLength = m_arrayType->GetLength(sourceArray);

        for (uint32_t sourceIndex = 0; sourceIndex < sourceLength; ++sourceIndex)
        {
            auto insertionValuePtr = m_arrayType->GetElement(sourceArray, sourceIndex);

            if (InArray(m_arrayType, m_array.get(), insertionValuePtr))
                continue;

            m_arrayType->InsertAt(m_array.get(), appendingIndex);
            m_elementType->Assign(m_arrayType->GetElement(m_array.get(), appendingIndex), insertionValuePtr);

            m_changes.emplace_back(appendingIndex, Red::TweakDB::CopyValue(m_elementType, insertionValuePtr));

            ++appendingIndex;
        }
    }
}

int32_t App::TweakChangeset::FindElement(RED4ext::CRTTIArrayType* aArrayType, void* aArray, void* aValue)
{
    const auto length = aArrayType->GetLength(aArray);

    for (int32_t i = 0; i < length; ++i)
    {
        const auto element = aArrayType->GetElement(aArray, i);

        if (aArrayType->innerType->IsEqual(element, aValue))
        {
            return i;
        }
    }

    return -1;
}

bool App::TweakChangeset::InArray(RED4ext::CRTTIArrayType* aArrayType, void* aArray, void* aValue)
{
    return FindElement(aArrayType, aArray, aValue) > 0;
}

std::string App::TweakChangeset::AsString(const RED4ext::CBaseRTTIType* aType)
{
    return aType->GetName().ToString();
}

std::string App::TweakChangeset::AsString(RED4ext::TweakDBID aId)
{
    const auto name = m_pendingNames.find(aId);

    if (name != m_pendingNames.end())
        return name.value();

    return fmt::format("<TDBID:{:08X}:{:02X}>", aId.name.hash, aId.name.length);
}
