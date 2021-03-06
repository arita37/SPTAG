// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "inc/Core/VectorIndex.h"
#include "inc/Core/Common/DataUtils.h"
#include "inc/Helper/CommonHelper.h"
#include "inc/Helper/StringConvert.h"
#include "inc/Helper/SimpleIniReader.h"
#include "inc/Helper/BufferStream.h"

#include "inc/Core/BKT/Index.h"
#include "inc/Core/KDT/Index.h"
#include <fstream>


using namespace SPTAG;


VectorIndex::VectorIndex()
{
}


VectorIndex::~VectorIndex()
{
}


std::string 
VectorIndex::GetParameter(const std::string& p_param) const
{
    return GetParameter(p_param.c_str());
}


ErrorCode
VectorIndex::SetParameter(const std::string& p_param, const std::string& p_value)
{
    return SetParameter(p_param.c_str(), p_value.c_str());
}


void 
VectorIndex::SetMetadata(const std::string& p_metadataFilePath, const std::string& p_metadataIndexPath) {
    m_pMetadata.reset(new MemMetadataSet(p_metadataFilePath, p_metadataIndexPath));
}


ByteArray 
VectorIndex::GetMetadata(SizeType p_vectorID) const {
    if (nullptr != m_pMetadata)
    {
        return m_pMetadata->GetMetadata(p_vectorID);
    }
    return ByteArray::c_empty;
}


std::shared_ptr<std::vector<std::uint64_t>> VectorIndex::CalculateBufferSize() const
{
    std::shared_ptr<std::vector<std::uint64_t>> ret = BufferSize();
    if (m_pMetadata != nullptr)
    {
        auto metasize = m_pMetadata->BufferSize();
        ret->push_back(metasize.first);
        ret->push_back(metasize.second);
    }
    return std::move(ret);
}


ErrorCode
VectorIndex::LoadIndexConfig(Helper::IniReader& p_reader)
{
    std::string metadataSection("MetaData");
    if (p_reader.DoesSectionExist(metadataSection))
    {
        m_sMetadataFile = p_reader.GetParameter(metadataSection, "MetaDataFilePath", std::string());
        m_sMetadataIndexFile = p_reader.GetParameter(metadataSection, "MetaDataIndexPath", std::string());
    }

    if (DistCalcMethod::Undefined == p_reader.GetParameter("Index", "DistCalcMethod", DistCalcMethod::Undefined))
    {
        std::cerr << "Error: Failed to load parameter DistCalcMethod." << std::endl;
        return ErrorCode::Fail;
    }
    return LoadConfig(p_reader);
}


ErrorCode
VectorIndex::SaveIndexConfig(std::ostream& p_configOut)
{
    if (nullptr != m_pMetadata)
    {
        p_configOut << "[MetaData]" << std::endl;
        p_configOut << "MetaDataFilePath=" << m_sMetadataFile << std::endl;
        p_configOut << "MetaDataIndexPath=" << m_sMetadataIndexFile << std::endl;
        if (nullptr != m_pMetaToVec) p_configOut << "MetaDataToVectorIndex=true" << std::endl;
        p_configOut << std::endl;
    }

    p_configOut << "[Index]" << std::endl;
    p_configOut << "IndexAlgoType=" << Helper::Convert::ConvertToString(GetIndexAlgoType()) << std::endl;
    p_configOut << "ValueType=" << Helper::Convert::ConvertToString(GetVectorValueType()) << std::endl;
    p_configOut << std::endl;

    return SaveConfig(p_configOut);
}


void
VectorIndex::BuildMetaMapping()
{
    m_pMetaToVec.reset(new std::unordered_map<std::string, SizeType>);
    for (SizeType i = 0; i < m_pMetadata->Count(); i++) {
        if (ContainSample(i)) {
            ByteArray meta = m_pMetadata->GetMetadata(i);
            m_pMetaToVec->emplace(std::string((char*)meta.Data(), meta.Length()), i);
        }
    }
}


ErrorCode
VectorIndex::SaveIndex(std::string& p_config, const std::vector<ByteArray>& p_indexBlobs)
{
    if (GetNumSamples() - GetNumDeleted() == 0) return ErrorCode::EmptyIndex;

    std::ostringstream p_configStream;
    SaveIndexConfig(p_configStream);
    p_config = p_configStream.str();
    
    std::vector<std::ostream*> p_indexStreams;
    for (size_t i = 0; i < p_indexBlobs.size(); i++)
    {
        p_indexStreams.push_back(new Helper::obufferstream(new Helper::streambuf((char*)p_indexBlobs[i].Data(), p_indexBlobs[i].Length()), true));
    }

    ErrorCode ret = ErrorCode::Success;
    if (NeedRefine()) 
    {
        ret = RefineIndex(p_indexStreams);
    }
    else 
    {
        if (m_pMetadata != nullptr && p_indexStreams.size() > 5)
        {
            ret = m_pMetadata->SaveMetadata(*p_indexStreams[p_indexStreams.size() - 2], *p_indexStreams[p_indexStreams.size() - 1]);
        }
        if (ErrorCode::Success == ret) ret = SaveIndexData(p_indexStreams);
    }
    for (size_t i = 0; i < p_indexStreams.size(); i++)
    {
        delete p_indexStreams[i];
    }
    return ret;
}


ErrorCode
VectorIndex::SaveIndex(const std::string& p_folderPath)
{
    if (GetNumSamples() - GetNumDeleted() == 0) return ErrorCode::EmptyIndex;

    std::string folderPath(p_folderPath);
    if (!folderPath.empty() && *(folderPath.rbegin()) != FolderSep)
    {
        folderPath += FolderSep;
    }

    if (!direxists(folderPath.c_str()))
    {
        mkdir(folderPath.c_str());
    }

    std::ofstream configFile(folderPath + "indexloader.ini");
    if (!configFile.is_open()) return ErrorCode::FailedCreateFile;
    SaveIndexConfig(configFile);
    configFile.close();
    
    if (NeedRefine()) return RefineIndex(p_folderPath);

    if (m_pMetadata != nullptr)
    {
        ErrorCode ret = m_pMetadata->SaveMetadata(folderPath + m_sMetadataFile, folderPath + m_sMetadataIndexFile);
        if (ErrorCode::Success != ret) return ret;
    }
    return SaveIndexData(folderPath);
}

ErrorCode
VectorIndex::BuildIndex(std::shared_ptr<VectorSet> p_vectorSet,
    std::shared_ptr<MetadataSet> p_metadataSet, bool p_withMetaIndex)
{
    if (nullptr == p_vectorSet || p_vectorSet->GetValueType() != GetVectorValueType())
    {
        return ErrorCode::Fail;
    }

    BuildIndex(p_vectorSet->GetData(), p_vectorSet->Count(), p_vectorSet->Dimension());
    m_pMetadata = std::move(p_metadataSet);
    if (p_withMetaIndex && m_pMetadata != nullptr) 
    {
        BuildMetaMapping();
    }
    return ErrorCode::Success;
}


ErrorCode
VectorIndex::SearchIndex(const void* p_vector, int p_vectorCount, int p_neighborCount, bool p_withMeta, BasicResult* p_results) const {
    size_t vectorSize = GetValueTypeSize(GetVectorValueType()) * GetFeatureDim();
#pragma omp parallel for schedule(dynamic,10)
    for (int i = 0; i < p_vectorCount; i++) {
        QueryResult res((char*)p_vector + i * vectorSize, p_neighborCount, p_withMeta, p_results + i * p_neighborCount);
        SearchIndex(res);
    }
    return ErrorCode::Success;
}


ErrorCode 
VectorIndex::AddIndex(std::shared_ptr<VectorSet> p_vectorSet, std::shared_ptr<MetadataSet> p_metadataSet, bool p_withMetaIndex) {
    if (nullptr == p_vectorSet || p_vectorSet->GetValueType() != GetVectorValueType())
    {
        return ErrorCode::Fail;
    }

    return AddIndex(p_vectorSet->GetData(), p_vectorSet->Count(), p_vectorSet->Dimension(), p_metadataSet, p_withMetaIndex);
}


ErrorCode
VectorIndex::DeleteIndex(ByteArray p_meta) {
    if (m_pMetaToVec == nullptr) return ErrorCode::VectorNotFound;

    std::string meta((char*)p_meta.Data(), p_meta.Length());
    auto iter = m_pMetaToVec->find(meta);
    if (iter != m_pMetaToVec->end()) return DeleteIndex(iter->second);
    return ErrorCode::VectorNotFound;
}


ErrorCode
VectorIndex::MergeIndex(VectorIndex* p_addindex, int p_threadnum)
{
    if (p_addindex->m_pMetadata != nullptr) {
#pragma omp parallel for num_threads(p_threadnum) schedule(dynamic,128)
        for (SizeType i = 0; i < p_addindex->GetNumSamples(); i++)
            if (p_addindex->ContainSample(i))
            {
                ByteArray meta = p_addindex->GetMetadata(i);
                std::uint64_t offsets[2] = { 0, meta.Length() };
                std::shared_ptr<MetadataSet> p_metaSet(new MemMetadataSet(meta, ByteArray((std::uint8_t*)offsets, sizeof(offsets), false), 1));
                AddIndex(p_addindex->GetSample(i), 1, p_addindex->GetFeatureDim(), p_metaSet);
            }
    }
    else {
#pragma omp parallel for num_threads(p_threadnum) schedule(dynamic,128)
        for (SizeType i = 0; i < p_addindex->GetNumSamples(); i++)
            if (p_addindex->ContainSample(i))
            {
                AddIndex(p_addindex->GetSample(i), 1, p_addindex->GetFeatureDim(), nullptr);
            }
    }
    return ErrorCode::Success;
}


const void* VectorIndex::GetSample(ByteArray p_meta, bool& deleteFlag)
{
    if (m_pMetaToVec == nullptr) return nullptr;

    std::string meta((char*)p_meta.Data(), p_meta.Length());
    auto iter = m_pMetaToVec->find(meta);
    if (iter != m_pMetaToVec->end()) {
        deleteFlag = !ContainSample(iter->second);
        return GetSample(iter->second);
    }
    return nullptr;
}


std::shared_ptr<VectorIndex>
VectorIndex::CreateInstance(IndexAlgoType p_algo, VectorValueType p_valuetype)
{
    if (IndexAlgoType::Undefined == p_algo || VectorValueType::Undefined == p_valuetype)
    {
        return nullptr;
    }

    if (p_algo == IndexAlgoType::BKT) {
        switch (p_valuetype)
        {
#define DefineVectorValueType(Name, Type) \
    case VectorValueType::Name: \
        return std::shared_ptr<VectorIndex>(new BKT::Index<Type>); \

#include "inc/Core/DefinitionList.h"
#undef DefineVectorValueType

        default: break;
        }
    }
    else if (p_algo == IndexAlgoType::KDT) {
        switch (p_valuetype)
        {
#define DefineVectorValueType(Name, Type) \
    case VectorValueType::Name: \
        return std::shared_ptr<VectorIndex>(new KDT::Index<Type>); \

#include "inc/Core/DefinitionList.h"
#undef DefineVectorValueType

        default: break;
        }
    }
    return nullptr;
}


ErrorCode
VectorIndex::LoadIndex(const std::string& p_loaderFilePath, std::shared_ptr<VectorIndex>& p_vectorIndex)
{
    std::string folderPath(p_loaderFilePath);
    if (!folderPath.empty() && *(folderPath.rbegin()) != FolderSep) folderPath += FolderSep;

    Helper::IniReader iniReader;
    if (ErrorCode::Success != iniReader.LoadIniFile(folderPath + "indexloader.ini")) return ErrorCode::FailedOpenFile;

    IndexAlgoType algoType = iniReader.GetParameter("Index", "IndexAlgoType", IndexAlgoType::Undefined);
    VectorValueType valueType = iniReader.GetParameter("Index", "ValueType", VectorValueType::Undefined);
    p_vectorIndex = CreateInstance(algoType, valueType);
    if (p_vectorIndex == nullptr) return ErrorCode::FailedParseValue;

    ErrorCode ret = p_vectorIndex->LoadIndexConfig(iniReader);
    if (ErrorCode::Success != ret) return ret;

    ret = p_vectorIndex->LoadIndexData(folderPath);
    if (ErrorCode::Success != ret) return ret;

    if (iniReader.DoesSectionExist("MetaData"))
    {
        p_vectorIndex->m_pMetadata.reset(new MemMetadataSet(folderPath + p_vectorIndex->m_sMetadataFile, 
            folderPath + p_vectorIndex->m_sMetadataIndexFile));

        if (!(p_vectorIndex->m_pMetadata)->Available())
        {
            std::cerr << "Error: Failed to load metadata." << std::endl;
            return ErrorCode::Fail;
        }

        if (iniReader.GetParameter("MetaData", "MetaDataToVectorIndex", std::string()) == "true")
        {
            p_vectorIndex->BuildMetaMapping();
        }
    }
    return ErrorCode::Success;
}


ErrorCode
VectorIndex::LoadIndex(const std::string& p_config, const std::vector<ByteArray>& p_indexBlobs, std::shared_ptr<VectorIndex>& p_vectorIndex)
{
    SPTAG::Helper::IniReader iniReader;
    std::istringstream p_configin(p_config);
    if (SPTAG::ErrorCode::Success != iniReader.LoadIni(p_configin)) return ErrorCode::FailedParseValue;

    IndexAlgoType algoType = iniReader.GetParameter("Index", "IndexAlgoType", IndexAlgoType::Undefined);
    VectorValueType valueType = iniReader.GetParameter("Index", "ValueType", VectorValueType::Undefined);
    p_vectorIndex = CreateInstance(algoType, valueType);
    if (p_vectorIndex == nullptr) return ErrorCode::FailedParseValue;

    ErrorCode ret = p_vectorIndex->LoadIndexConfig(iniReader);
    if (ErrorCode::Success != ret) return ret;

    ret = p_vectorIndex->LoadIndexDataFromMemory(p_indexBlobs);
    if (ErrorCode::Success != ret) return ret;

    if (iniReader.DoesSectionExist("MetaData") && p_indexBlobs.size() > 4)
    {
        ByteArray pMetaIndex = p_indexBlobs[p_indexBlobs.size() - 1];
        p_vectorIndex->m_pMetadata.reset(new MemMetadataSet(p_indexBlobs[p_indexBlobs.size() - 2],
            ByteArray(pMetaIndex.Data() + sizeof(SizeType), pMetaIndex.Length() - sizeof(SizeType), false),
            *((SizeType*)pMetaIndex.Data())));

        if (!(p_vectorIndex->m_pMetadata)->Available())
        {
            std::cerr << "Error: Failed to load metadata." << std::endl;
            return ErrorCode::Fail;
        }

        if (iniReader.GetParameter("MetaData", "MetaDataToVectorIndex", std::string()) == "true")
        {
            p_vectorIndex->BuildMetaMapping();
        }
    }
    return ErrorCode::Success;
}


std::uint64_t VectorIndex::EstimatedVectorCount(std::uint64_t p_memory, DimensionType p_dimension, IndexAlgoType p_algo, VectorValueType p_valuetype, int p_treeNumber, int p_neighborhoodSize)
{
    size_t treeNodeSize;
    if (p_algo == IndexAlgoType::BKT) {
        treeNodeSize = sizeof(SizeType) * 3;
    }
    else if (p_algo == IndexAlgoType::KDT) {
        treeNodeSize = sizeof(SizeType) * 2 + sizeof(DimensionType) + sizeof(float);
    }
    else {
        return 0;
    }
    std::uint64_t unit = GetValueTypeSize(p_valuetype) * p_dimension + sizeof(std::uint64_t) + sizeof(SizeType) * p_neighborhoodSize + 1 + treeNodeSize * p_treeNumber;
    return p_memory / unit;
}


std::uint64_t VectorIndex::EstimatedMemoryUsage(std::uint64_t p_vectorCount, DimensionType p_dimension, IndexAlgoType p_algo, VectorValueType p_valuetype, int p_treeNumber, int p_neighborhoodSize)
{
    size_t treeNodeSize;
    if (p_algo == IndexAlgoType::BKT) {
        treeNodeSize = sizeof(SizeType) * 3;
    }
    else if (p_algo == IndexAlgoType::KDT) {
        treeNodeSize = sizeof(SizeType) * 2 + sizeof(DimensionType) + sizeof(float);
    }
    else {
        return 0;
    }
    std::uint64_t ret = GetValueTypeSize(p_valuetype) * p_dimension * p_vectorCount; //Vector Size
    ret += sizeof(std::uint64_t) * p_vectorCount; // MetaIndex Size
    ret += sizeof(SizeType) * p_neighborhoodSize * p_vectorCount; // Graph Size
    ret += p_vectorCount; // DeletedFlag Size
    ret += treeNodeSize * p_treeNumber * p_vectorCount; // Tree Size
    return ret;
}