#include "MetadataImporter.hpp"

App::MetadataImporter::MetadataImporter(Core::SharedPtr<Red::TweakDBManager> aManager)
    : m_manager(std::move(aManager))
    , m_reflection(m_manager->GetReflection())
{
}

bool App::MetadataImporter::ImportInheritanceMap(const std::filesystem::path& aPath)
{
    std::error_code error;
    if (!std::filesystem::exists(aPath, error))
        return false;

    auto data = YAML::LoadFile(aPath.string());
    if (!data.IsDefined() || !data.IsMap())
        return false;

    Core::Set<Red::TweakDBID> descendantIDs;

    for (const auto& topNodeIt : data)
    {
        const auto recordID = Red::TweakDBID(topNodeIt.first.Scalar());

        if (!recordID)
            return false;

        const auto& descendantNames = topNodeIt.second;

        if (!descendantNames.IsSequence())
            return false;

        descendantIDs.clear();

        for (const auto& descendantName : descendantNames)
        {
            const auto descendantID = Red::TweakDBID(descendantName.Scalar());

            if (!descendantID)
                return false;

            descendantIDs.insert(descendantID);
        }

        if (descendantIDs.empty())
            return false;

        m_reflection->RegisterDescendants(recordID, descendantIDs);
    }

    return true;
}

bool App::MetadataImporter::ImportExtraFlats(const std::filesystem::path& aPath)
{
    std::error_code error;
    if (!std::filesystem::exists(aPath, error))
        return false;

    auto data = YAML::LoadFile(aPath.string());
    if (!data.IsDefined() || !data.IsMap())
        return false;

    for (const auto& topNodeIt : data)
    {
        const auto recordType = m_reflection->GetRecordFullName(topNodeIt.first.Scalar().data());

        if (!m_reflection->IsRecordType(recordType))
            return false;

        const auto& extraFlats = topNodeIt.second;

        if (!extraFlats.IsMap())
            return false;

        for (const auto& extraFlatIt : extraFlats)
        {
            const auto& propName = extraFlatIt.first.Scalar();
            const auto& propDataNode = extraFlatIt.second;

            if (!propDataNode.IsMap())
                return false;

            const auto& propTypeNode = propDataNode["flatType"];

            if (!propTypeNode.IsScalar())
                return false;

            const auto propType = Red::CName(propTypeNode.Scalar().data());

            if (!m_reflection->IsFlatType(propType))
                return false;

            const auto& foreignTypeNode = propDataNode["foreignType"];

            auto foreignType = Red::CName();

            if (foreignTypeNode.IsDefined())
            {
                if (!foreignTypeNode.IsScalar())
                    return false;

                foreignType = m_reflection->GetRecordFullName(foreignTypeNode.Scalar().data());

                if (!m_reflection->IsRecordType(foreignType))
                    return false;
            }

            m_reflection->RegisterExtraFlat(recordType, propName, propType, foreignType);
        }
    }

    return true;
}
