#include "Buffer.hpp"

Red::TweakDBBuffer::TweakDBBuffer()
    : TweakDBBuffer(Red::TweakDB::Get())
{
}

Red::TweakDBBuffer::TweakDBBuffer(Red::TweakDB* aTweakDb)
    : m_tweakDb(aTweakDb)
    , m_bufferEnd(0)
    , m_offsetEnd(0)
{
}

int32_t Red::TweakDBBuffer::AllocateValue(const Red::CBaseRTTIType* aType, Red::ScriptInstance aValue)
{
    if (m_bufferEnd != m_tweakDb->flatDataBufferEnd)
        Initialize();

    // TODO: Own mutex

    const auto poolKey = aType->GetName();
    auto poolIt = m_pools.find(poolKey);

    if (poolIt == m_pools.end())
        poolIt = m_pools.emplace(poolKey, 0).first;

    FlatValueMap& pool = poolIt.value();

    const auto hash = Hash(aType, aValue);
    const auto offsetIt = pool.find(hash);

    int32_t offset;

    if (offsetIt == pool.end())
    {
        offset = m_tweakDb->CreateFlatValue({ const_cast<Red::CBaseRTTIType*>(aType), aValue });

        if (offset != InvalidOffset)
            pool.emplace(hash, offset);

        SyncBuffer();
    }
    else
    {
        offset = offsetIt->second;
    }

    return offset;
}

int32_t Red::TweakDBBuffer::AllocateData(const Red::CStackType& aData)
{
    return AllocateValue(aData.type, aData.value);
}

int32_t Red::TweakDBBuffer::AllocateDefault(const Red::CBaseRTTIType* aType)
{
    if (m_bufferEnd != m_tweakDb->flatDataBufferEnd)
        Initialize();

    int32_t offset = InvalidOffset;

    const auto typeKey = aType->GetName();
    auto offsetIt = m_defaults.find(typeKey);

    if (offsetIt == m_defaults.end())
    {
        const auto defaultFlat = m_tweakDb->GetDefaultFlatValue(typeKey);

        if (defaultFlat)
        {
            offset = defaultFlat->ToTDBOffset();
        }
        else
        {
            auto value = aType->GetAllocator()->AllocAligned(aType->GetSize(), aType->GetAlignment());
            std::memset(value.memory, 0, value.size);
            aType->Construct(value.memory);

            offset = AllocateValue(aType, value.memory);

            aType->Destruct(value.memory);
            aType->GetAllocator()->Free(value);

            // TODO: Add to the TweakDB::defaultValues?
        }

        if (offset != InvalidOffset)
        {
            m_defaults.emplace(typeKey, offset);

            SyncBuffer();
        }
    }
    else
    {
        offset = offsetIt->second;
    }

    return offset;
}

Red::CStackType Red::TweakDBBuffer::GetData(int32_t aOffset)
{
    if (m_bufferEnd != m_tweakDb->flatDataBufferEnd)
        Initialize();

    return GetFlatData(aOffset);
}

Red::ScriptInstance Red::TweakDBBuffer::GetValuePtr(int32_t aOffset)
{
    if (m_bufferEnd != m_tweakDb->flatDataBufferEnd)
        Initialize();

    return GetFlatData(aOffset).value;
}

void Red::TweakDBBuffer::Initialize()
{
    uintptr_t offsetEnd = m_tweakDb->flatDataBufferEnd - m_tweakDb->flatDataBuffer;

    if (m_offsetEnd == offsetEnd)
    {
        SyncBuffer();
        return;
    }

    const auto startTimePoint = std::chrono::steady_clock::now();

    {
        std::shared_lock<Red::SharedMutex> flatLockR(m_tweakDb->mutex00);

        constexpr auto FlatVFTSize = 8u;
        constexpr auto FlatAlignment = 8u;

        auto offset = Red::AlignUp(static_cast<uint32_t>(m_offsetEnd), FlatAlignment);
        while (offset < offsetEnd)
        {
            // The current offset should always point to the VFT of the next flat.
            // If there's zero instead, that means the next value is 16-byte aligned,
            // and we need to skip the 8-byte padding to get to the flat.
            if (*reinterpret_cast<uint64_t*>(m_tweakDb->flatDataBuffer + offset) == 0ull)
                 offset += 8u;

            const auto data = GetFlatData(static_cast<int32_t>(offset));
            const auto poolKey = data.type->GetName();

            auto poolIt = m_pools.find(poolKey);

            if (poolIt == m_pools.end())
                poolIt = m_pools.emplace(poolKey, 0).first;

            FlatValueMap& pool = poolIt.value();

            const auto hash = Hash(data.type, data.value);

            // Check for duplicates...
            // (Original game's blob has ~24K duplicates)
            if (!pool.contains(hash))
                pool.emplace(hash, offset);

            // Step {vft + data_size} aligned by {max(data_align, 8)}
            offset += Red::AlignUp(FlatVFTSize + data.type->GetSize(),
                                   std::max(FlatAlignment, data.type->GetAlignment()));
        }

        SyncBuffer();
    }

    const auto endTimePoint = std::chrono::steady_clock::now();
    const auto updateTime = std::chrono::duration_cast<std::chrono::duration<float>>(endTimePoint - startTimePoint).count();

    UpdateStats(updateTime);
}

Red::CStackType Red::TweakDBBuffer::GetFlatData(int32_t aOffset)
{
    // This method uses VFTs to determine the flat type.
    // It's 11% to 33% faster than calling GetValue() every time.

    const auto addr = m_tweakDb->flatDataBuffer + aOffset;
    const auto vft = *reinterpret_cast<uintptr_t*>(addr);
    const auto it = m_vfts.find(vft);

    // For a known VFT we can immediately get RTTI type and data pointer.
    if (it != m_vfts.end())
        return { it->second.type, reinterpret_cast<void*>(addr + it->second.offset) };

    // For an unknown VFT, we call the virtual GetValue() once to get the type.
    const auto data = reinterpret_cast<TweakDBFlatValue*>(addr)->GetValue();

    // Add type info to the map.
    // In addition to the RTTI type, we also store the data offset considering alignment.
    // Quaternion is 16-byte aligned, so there is 8-byte padding between the VFT and the data:
    // [ 8B VFT ][ 8B PAD ][ 16B QUATERNION ]
    m_vfts.insert({ vft, { data.type, std::max(data.type->GetAlignment(), 8u) } });

    return data;
}

uint64_t Red::TweakDBBuffer::Hash(const Red::CBaseRTTIType* aType, Red::ScriptInstance aValue)
{
    // Case 1: Everything is processed as a sequence of bytes and passed to the hash function,
    //         except for an array of strings.
    // Case 2: Arrays of strings are different because of empty strings that don't produce any
    //         hashing value. Therefore hash will be equal for different arrays in cases like:
    //         [] == [""] == ["", ""]
    //         ["", "a", "b"] == ["a", "", "b"] == ["a", "b", ""]
    //         As a workaround, we hash the string length as part of the data.

    uint64_t hash;

    if (aType->GetType() == Red::ERTTIType::Array)
    {
        auto* arrayType = reinterpret_cast<const Red::CRTTIArrayType*>(aType);
        auto* innerType = arrayType->GetInnerType();

        if (innerType->GetName() == "String")
        {
            const auto* array = reinterpret_cast<Red::DynArray<Red::CString>*>(aValue);
            hash = Red::FNV1a64(reinterpret_cast<uint8_t*>(0), 0); // Initial seed
            for (uint32_t i = 0; i != array->size; ++i)
            {
                const auto* str = array->entries + i;
                const auto length = str->Length();
                hash = Red::FNV1a64(reinterpret_cast<const uint8_t*>(&length), sizeof(length), hash);
                hash = Red::FNV1a64(reinterpret_cast<const uint8_t*>(str->c_str()), length, hash);
            }
        }
        else
        {
            const auto* array = reinterpret_cast<Red::DynArray<uint8_t>*>(aValue);
            hash = Red::FNV1a64(array->entries, array->size * innerType->GetSize());
        }
    }
    else if (aType->GetName() == "String")
    {
        const auto* str = reinterpret_cast<Red::CString*>(aValue);
        const auto* data = reinterpret_cast<const uint8_t*>(str->c_str());
        hash = Red::FNV1a64(data, str->Length());
    }
    else
    {
        const auto* data = reinterpret_cast<const uint8_t*>(aValue);
        hash = Red::FNV1a64(data, aType->GetSize());
    }

    return hash;
}

void Red::TweakDBBuffer::SyncBuffer()
{
    m_bufferEnd = m_tweakDb->flatDataBufferEnd;
    m_offsetEnd = m_tweakDb->flatDataBufferEnd - m_tweakDb->flatDataBuffer;
}

void Red::TweakDBBuffer::UpdateStats(float updateTime)
{
    if (updateTime != 0)
    {
        if (m_stats.initTime == 0)
            m_stats.initTime = updateTime;
        else
            m_stats.updateTime = updateTime;
    }

    size_t totalValues = 0;
    for (const auto& pool : m_pools)
        totalValues += pool.second.size();

    m_stats.poolSize = m_offsetEnd;
    m_stats.poolValues = totalValues;
    m_stats.knownTypes = m_vfts.size();
    m_stats.flatEntries = m_tweakDb->flats.size;

#ifdef VERBOSE
    Red::Log::Debug(
        "[Red::TweakDBFlatPool] init {:.3f}s | update {:.6f}s | {} KiB | {} values | {} flats | {} types",
        m_stats.initTime, m_stats.updateTime,
        m_stats.poolSize / 1024, m_stats.poolValues,
        m_stats.flatEntries, m_stats.knownTypes);
#endif
}

Red::TweakDBBuffer::BufferStats Red::TweakDBBuffer::GetStats() const
{
    return m_stats;
}

void Red::TweakDBBuffer::Invalidate()
{
    m_bufferEnd = 0;
    m_offsetEnd = 0;
    m_pools.clear();
    m_defaults.clear();
    m_stats = {};
}
