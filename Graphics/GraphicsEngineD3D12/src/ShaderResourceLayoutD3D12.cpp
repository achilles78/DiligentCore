/*     Copyright 2015-2017 Egor Yusov
 *  
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 * 
 *     http://www.apache.org/licenses/LICENSE-2.0
 * 
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF ANY PROPRIETARY RIGHTS.
 *
 *  In no event and under no legal theory, whether in tort (including negligence), 
 *  contract, or otherwise, unless required by applicable law (such as deliberate 
 *  and grossly negligent acts) or agreed to in writing, shall any Contributor be
 *  liable for any damages, including any direct, indirect, special, incidental, 
 *  or consequential damages of any character arising as a result of this License or 
 *  out of the use or inability to use the software (including but not limited to damages 
 *  for loss of goodwill, work stoppage, computer failure or malfunction, or any and 
 *  all other commercial damages or losses), even if such Contributor has been advised 
 *  of the possibility of such damages.
 */

#include "pch.h"

#include "ShaderResourceLayoutD3D12.h"
#include "ShaderResourceCacheD3D12.h"
#include "D3DShaderResourceLoader.h"
#include "BufferD3D12Impl.h"
#include "BufferViewD3D12.h"
#include "TextureD3D12Impl.h"
#include "TextureViewD3D12Impl.h"
#include "SamplerD3D12Impl.h"
#include "ShaderD3D12Impl.h"
#include "RootSignature.h"
#include "PipelineStateD3D12Impl.h"

namespace Diligent
{
 
ShaderResourceLayoutD3D12::ShaderResourceLayoutD3D12(IObject &Owner,
                                                     IMemoryAllocator &ResourceLayoutDataAllocator) : 
    m_Owner(Owner),
#if USE_VARIABLE_HASH_MAP
    m_VariableHash(STD_ALLOCATOR_RAW_MEM(VariableHashElemType, GetRawAllocator(), "Allocator for unordered_map<HashMapStringKey, IShaderVariable*>")),
#endif
    m_ResourceBuffer(nullptr, STDDeleterRawMem<void>(ResourceLayoutDataAllocator))
{
}

ShaderResourceLayoutD3D12::~ShaderResourceLayoutD3D12()
{
    auto* CbvSrvUav = reinterpret_cast<SRV_CBV_UAV*>(m_ResourceBuffer.get());
    for(Uint32 r=0; r < GetTotalSrvCbvUavCount(); ++r)
        CbvSrvUav[r].~SRV_CBV_UAV();

    for(Uint32 s=0; s < GetTotalSamplerCount(); ++s)
        m_Samplers[s].~Sampler();
}

D3D12_DESCRIPTOR_RANGE_TYPE GetDescriptorRangeType(CachedResourceType ResType)
{
    static D3D12_DESCRIPTOR_RANGE_TYPE RangeTypes[(size_t)CachedResourceType::NumTypes] = {};
    static bool Initialized = false;
    if (!Initialized)
    {
        RangeTypes[(size_t)CachedResourceType::CBV]    = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
        RangeTypes[(size_t)CachedResourceType::TexSRV] = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        RangeTypes[(size_t)CachedResourceType::BufSRV] = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        RangeTypes[(size_t)CachedResourceType::TexUAV] = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        RangeTypes[(size_t)CachedResourceType::BufUAV] = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        RangeTypes[(size_t)CachedResourceType::Sampler] = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
        Initialized = true;
    }
    auto Ind = static_cast<size_t>(ResType);
    VERIFY(Ind >= 0 && Ind < (size_t)CachedResourceType::NumTypes, "Unexpected resource type")
    return RangeTypes[Ind];
}

void ShaderResourceLayoutD3D12::AllocateMemory(IMemoryAllocator &Allocator)
{
    VERIFY( &m_ResourceBuffer.get_deleter().m_Allocator == &Allocator, "Inconsistent memory allocators" )
    Uint32 TotalSrvCbvUav = GetTotalSrvCbvUavCount();
    Uint32 TotalSamplers = GetTotalSamplerCount();
    size_t MemSize = TotalSrvCbvUav * sizeof(SRV_CBV_UAV) + TotalSamplers * sizeof(Sampler);
    if(MemSize == 0)
        return;

    auto *pRawMem = ALLOCATE(Allocator, "Raw memory buffer for shader resource layout resources", MemSize);
    m_ResourceBuffer.reset(pRawMem);
    if(TotalSamplers)
        m_Samplers = reinterpret_cast<Sampler*>(reinterpret_cast<SRV_CBV_UAV*>(pRawMem) + TotalSrvCbvUav);
}

// Clones layout from the reference layout
// Root indices and descriptor table offsets must be correct
// Resource cache is not initialized.
ShaderResourceLayoutD3D12::ShaderResourceLayoutD3D12(IObject &Owner,
                                                     const ShaderResourceLayoutD3D12& SrcLayout, 
                                                     IMemoryAllocator &ResourceLayoutDataAllocator,
                                                     const SHADER_VARIABLE_TYPE *AllowedVarTypes, 
                                                     Uint32 NumAllowedTypes, 
                                                     ShaderResourceCacheD3D12 &ResourceCache) :
    ShaderResourceLayoutD3D12(Owner, ResourceLayoutDataAllocator)
{
    m_pd3d12Device = SrcLayout.m_pd3d12Device;
    m_pResources = SrcLayout.m_pResources;
    m_pResourceCache = &ResourceCache;
    
    Uint32 AllowedTypeBits = GetAllowedTypeBits(AllowedVarTypes, NumAllowedTypes);
    
    for(SHADER_VARIABLE_TYPE VarType = SHADER_VARIABLE_TYPE_STATIC; VarType < SHADER_VARIABLE_TYPE_NUM_TYPES; VarType = static_cast<SHADER_VARIABLE_TYPE>(VarType+1))
    {
        if( !IsAllowedType(VarType, AllowedTypeBits))
            continue;

        m_NumCbvSrvUav[VarType] = SrcLayout.m_NumCbvSrvUav[VarType];
        m_NumSamplers[VarType] = SrcLayout.m_NumSamplers[VarType];
    }
    
    AllocateMemory(ResourceLayoutDataAllocator);

    Uint32 CurrCbvSrvUav[SHADER_VARIABLE_TYPE_NUM_TYPES] = {0,0,0};
    Uint32 CurrSampler[SHADER_VARIABLE_TYPE_NUM_TYPES] = {0,0,0};

    for(SHADER_VARIABLE_TYPE VarType = SHADER_VARIABLE_TYPE_STATIC; VarType < SHADER_VARIABLE_TYPE_NUM_TYPES; VarType = static_cast<SHADER_VARIABLE_TYPE>(VarType+1))
    {
        if( !IsAllowedType(VarType, AllowedTypeBits))
            continue;

        Uint32 NumSrcCbvSrvUav = SrcLayout.m_NumCbvSrvUav[VarType];
        VERIFY_EXPR(NumSrcCbvSrvUav == m_NumCbvSrvUav[VarType]);
        for( Uint32 r=0; r < NumSrcCbvSrvUav; ++r )
        {
            const auto &SrcRes = SrcLayout.GetSrvCbvUav(VarType, r);
            Uint32 SamplerId = SRV_CBV_UAV::InvalidSamplerId;
            if (SrcRes.IsValidSampler())
            {
                const auto &SrcSamplerAttribs = SrcLayout.GetSampler(VarType, SrcRes.GetSamplerId());
                VERIFY(!SrcSamplerAttribs.Attribs.IsStaticSampler(), "Only non-static samplers can be assigned space in shader cache")
                VERIFY(SrcSamplerAttribs.Attribs.GetVariableType() == SrcRes.Attribs.GetVariableType(), "Inconsistent texture and sampler variable types" );
                VERIFY(SrcSamplerAttribs.IsValidRootIndex(), "Root index must be valid")
                VERIFY(SrcSamplerAttribs.IsValidOffset(), "Offset must be valid")
                VERIFY_EXPR(SrcSamplerAttribs.Attribs.BindCount == SrcRes.Attribs.BindCount || SrcSamplerAttribs.Attribs.BindCount == 1);

                SamplerId = CurrSampler[VarType];
                VERIFY(SamplerId <= SRV_CBV_UAV::MaxSamplerId, "SamplerId exceeds maximum allowed value (", SRV_CBV_UAV::MaxSamplerId, ")");
                VERIFY_EXPR(SamplerId == SrcRes.GetSamplerId());
                ::new (&GetSampler(VarType, CurrSampler[VarType]++)) Sampler( *this, SrcSamplerAttribs );
            }

            VERIFY(SrcRes.IsValidRootIndex(), "Root index must be valid")
            VERIFY(SrcRes.IsValidOffset(), "Offset must be valid")
            ::new (&GetSrvCbvUav(VarType, CurrCbvSrvUav[VarType]++)) SRV_CBV_UAV( *this, SrcRes, SamplerId );
        }
    }

#ifdef _DEBUG
    for(SHADER_VARIABLE_TYPE VarType = SHADER_VARIABLE_TYPE_STATIC; VarType < SHADER_VARIABLE_TYPE_NUM_TYPES; VarType = static_cast<SHADER_VARIABLE_TYPE>(VarType+1))
    {
        VERIFY_EXPR( CurrCbvSrvUav[VarType] == m_NumCbvSrvUav[VarType] );
        VERIFY_EXPR( CurrSampler[VarType] == m_NumSamplers[VarType] );
    }
#endif
}

void ShaderResourceLayoutD3D12::Initialize(ID3D12Device *pd3d12Device,
                                           const std::shared_ptr<const ShaderResourcesD3D12>& pSrcResources, 
                                           IMemoryAllocator &LayoutDataAllocator,
                                           const SHADER_VARIABLE_TYPE *AllowedVarTypes, 
                                           Uint32 NumAllowedTypes, 
                                           ShaderResourceCacheD3D12* pResourceCache,
                                           RootSignature *pRootSig)
{
    m_pResources = pSrcResources;
    m_pResourceCache = pResourceCache;
    m_pd3d12Device = pd3d12Device;

    VERIFY_EXPR( (pResourceCache != nullptr) ^ (pRootSig != nullptr) );

    Uint32 AllowedTypeBits = GetAllowedTypeBits(AllowedVarTypes, NumAllowedTypes);

    m_pResources->ProcessResources(
        AllowedVarTypes, NumAllowedTypes,

        [&](const D3DShaderResourceAttribs &CB)
        {
            VERIFY_EXPR(IsAllowedType(CB.GetVariableType(), AllowedTypeBits));
            ++m_NumCbvSrvUav[CB.GetVariableType()];
        },
        [&](const D3DShaderResourceAttribs& TexSRV)
        {
            auto VarType = TexSRV.GetVariableType();
            VERIFY_EXPR(IsAllowedType(VarType, AllowedTypeBits));
            ++m_NumCbvSrvUav[VarType];
            if(TexSRV.IsValidSampler())
            {
                auto SamplerId = TexSRV.GetSamplerId();
                const auto &SamplerAttribs = m_pResources->GetSampler(SamplerId);
                VERIFY(SamplerAttribs.GetVariableType() == VarType, "Texture and sampler variable types are not conistent");
                if(!SamplerAttribs.IsStaticSampler())
                {
                    ++m_NumSamplers[VarType];
                }
            }
        },
        [&](const D3DShaderResourceAttribs &TexUAV)
        {
            VERIFY_EXPR(IsAllowedType(TexUAV.GetVariableType(), AllowedTypeBits));
            ++m_NumCbvSrvUav[TexUAV.GetVariableType()];
        },
        [&](const D3DShaderResourceAttribs &BufSRV)
        {
            VERIFY_EXPR(IsAllowedType(BufSRV.GetVariableType(), AllowedTypeBits));
            ++m_NumCbvSrvUav[BufSRV.GetVariableType()];
        },
        [&](const D3DShaderResourceAttribs &BufUAV)
        {
            VERIFY_EXPR(IsAllowedType(BufUAV.GetVariableType(), AllowedTypeBits));
            ++m_NumCbvSrvUav[BufUAV.GetVariableType()];
        }
    );


    AllocateMemory(LayoutDataAllocator);

    Uint32 CurrCbvSrvUav[SHADER_VARIABLE_TYPE_NUM_TYPES] = {0,0,0};
    Uint32 CurrSampler[SHADER_VARIABLE_TYPE_NUM_TYPES] = {0,0,0};
    int MaxBindPoint[4] = {-1, -1, -1, -1};

    auto AddResource = [&](const D3DShaderResourceAttribs &Attribs, CachedResourceType ResType, Uint32 SamplerId = SRV_CBV_UAV::InvalidSamplerId)
    {
        Uint32 RootIndex = SRV_CBV_UAV::InvalidRootIndex;
        Uint32 Offset = SRV_CBV_UAV::InvalidOffset;
        D3D12_DESCRIPTOR_RANGE_TYPE DescriptorRangeType = GetDescriptorRangeType(ResType);
        if (pRootSig)
        {
            pRootSig->AllocateResourceSlot(m_pResources->GetShaderType(), Attribs, DescriptorRangeType, RootIndex, Offset );
            VERIFY(RootIndex <= SRV_CBV_UAV::MaxRootIndex, "Root index excceeds allowed limit");
        }
        else
        {
            // If root signature is not provided - use artifial root signature to store
            // static shader resources
            VERIFY_EXPR(m_pResourceCache != nullptr);

            RootIndex = DescriptorRangeType;
            Offset = Attribs.BindPoint;
            // Resources in the static resource cache are indexed by the bind point
            MaxBindPoint[RootIndex] = std::max(MaxBindPoint[RootIndex], static_cast<int>(Offset + Attribs.BindCount));
        }
        VERIFY(RootIndex != SRV_CBV_UAV::InvalidRootIndex, "Root index must be valid")
        VERIFY(Offset != SRV_CBV_UAV::InvalidOffset, "Offset must be valid")

        // Static samplers are never copied, and SamplerId == InvalidSamplerId
        ::new (&GetSrvCbvUav(Attribs.GetVariableType(), CurrCbvSrvUav[Attribs.GetVariableType()]++)) SRV_CBV_UAV( *this, Attribs, ResType, RootIndex, Offset, SamplerId);
    };



    m_pResources->ProcessResources(
        AllowedVarTypes, NumAllowedTypes,

        [&](const D3DShaderResourceAttribs &CB)
        {
            VERIFY_EXPR( IsAllowedType(CB.GetVariableType(), AllowedTypeBits) );
            AddResource(CB, CachedResourceType::CBV);
        },
        [&](const D3DShaderResourceAttribs& TexSRV)
        {
            auto VarType = TexSRV.GetVariableType();
            VERIFY_EXPR(IsAllowedType(VarType, AllowedTypeBits) )
            
            Uint32 SamplerId = SRV_CBV_UAV::InvalidSamplerId;
            if(TexSRV.IsValidSampler())
            {
                const auto &SrcSamplerAttribs = m_pResources->GetSampler(TexSRV.GetSamplerId());
                VERIFY(SrcSamplerAttribs.GetVariableType() == VarType, "Inconsistent texture and sampler variable types" );

                if (SrcSamplerAttribs.IsStaticSampler())
                {
                    if(pRootSig != nullptr)
                        pRootSig->InitStaticSampler(m_pResources->GetShaderType(), TexSRV.Name, SrcSamplerAttribs);

                    // Static samplers are never copied, and SamplerId == InvalidSamplerId
                }
                else
                {
                    Uint32 SamplerRootIndex = Sampler::InvalidRootIndex;
                    Uint32 SamplerOffset = Sampler::InvalidOffset;
                    if (pRootSig)
                    {
                        pRootSig->AllocateResourceSlot(m_pResources->GetShaderType(), SrcSamplerAttribs, D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, SamplerRootIndex, SamplerOffset );
                    }
                    else
                    {
                        // If root signature is not provided, we are initializing resource cache to store 
                        // static shader resources. 
                        VERIFY_EXPR(m_pResourceCache != nullptr);

                        // We use the following artifial root signature:
                        // CBVs at root index D3D12_DESCRIPTOR_RANGE_TYPE_CBV,
                        // SRVs at root index D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
                        // UAVs at root index D3D12_DESCRIPTOR_RANGE_TYPE_UAV, and
                        // Samplers at root index D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER
                        // Every resource is stored at offset that equals its bind point
                        SamplerRootIndex = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER; 
                        SamplerOffset = SrcSamplerAttribs.BindPoint;
                        // Resources in the static resource cache are indexed by the bind point
                        MaxBindPoint[SamplerRootIndex] = std::max(MaxBindPoint[SamplerRootIndex], static_cast<int>(SamplerOffset + SrcSamplerAttribs.BindCount));
                    }
                    VERIFY(SamplerRootIndex != Sampler::InvalidRootIndex, "Sampler root index must be valid")
                    VERIFY(SamplerOffset != Sampler::InvalidOffset, "Sampler offset must be valid")

                    SamplerId = CurrSampler[VarType];
                    VERIFY(SamplerId <= SRV_CBV_UAV::MaxSamplerId, "Sampler index excceeds allowed limit");
                    ::new (&GetSampler(VarType, CurrSampler[VarType]++)) Sampler( *this, SrcSamplerAttribs, SamplerRootIndex, SamplerOffset );
                }
            }
            AddResource(TexSRV, CachedResourceType::TexSRV, SamplerId);
        },
        [&](const D3DShaderResourceAttribs &TexUAV)
        {
            VERIFY_EXPR( IsAllowedType(TexUAV.GetVariableType(), AllowedTypeBits) );
            AddResource(TexUAV, CachedResourceType::TexUAV);
        },
        [&](const D3DShaderResourceAttribs &BufSRV)
        {
            VERIFY_EXPR( IsAllowedType(BufSRV.GetVariableType(), AllowedTypeBits) );
            AddResource(BufSRV, CachedResourceType::BufSRV);
        },
        [&](const D3DShaderResourceAttribs &BufUAV)
        {
            VERIFY_EXPR( IsAllowedType(BufUAV.GetVariableType(), AllowedTypeBits) );
            AddResource(BufUAV, CachedResourceType::BufUAV);
        }
    );

#ifdef _DEBUG
    for(SHADER_VARIABLE_TYPE VarType = SHADER_VARIABLE_TYPE_STATIC; VarType < SHADER_VARIABLE_TYPE_NUM_TYPES; VarType = static_cast<SHADER_VARIABLE_TYPE>(VarType+1))
    {
        VERIFY( CurrCbvSrvUav[VarType] == m_NumCbvSrvUav[VarType], "Not all Srv/Cbv/Uavs are initialized, which result in a crash when dtor is called" );
        VERIFY( CurrSampler[VarType] == m_NumSamplers[VarType], "Not all Samplers are initialized, which result in a crash when dtor is called" );
    }
#endif

    if(m_pResourceCache)
    {
        VERIFY_EXPR(pRootSig == nullptr);
        Uint32 CachedTblSizes[4] = 
        {
            static_cast<Uint32>(MaxBindPoint[D3D12_DESCRIPTOR_RANGE_TYPE_SRV] + 1),
            static_cast<Uint32>(MaxBindPoint[D3D12_DESCRIPTOR_RANGE_TYPE_UAV] + 1),
            static_cast<Uint32>(MaxBindPoint[D3D12_DESCRIPTOR_RANGE_TYPE_CBV] + 1),
            static_cast<Uint32>(MaxBindPoint[D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER] + 1)
        };
        m_pResourceCache->Initialize(GetRawAllocator(), _countof(CachedTblSizes), CachedTblSizes);
#ifdef _DEBUG
        m_pResourceCache->GetRootTable(D3D12_DESCRIPTOR_RANGE_TYPE_SRV).SetDebugAttribs(MaxBindPoint[D3D12_DESCRIPTOR_RANGE_TYPE_SRV] + 1, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, m_pResources->GetShaderType());
        m_pResourceCache->GetRootTable(D3D12_DESCRIPTOR_RANGE_TYPE_UAV).SetDebugAttribs(MaxBindPoint[D3D12_DESCRIPTOR_RANGE_TYPE_UAV] + 1, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, m_pResources->GetShaderType());
        m_pResourceCache->GetRootTable(D3D12_DESCRIPTOR_RANGE_TYPE_CBV).SetDebugAttribs(MaxBindPoint[D3D12_DESCRIPTOR_RANGE_TYPE_CBV] + 1, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, m_pResources->GetShaderType());
        m_pResourceCache->GetRootTable(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER).SetDebugAttribs(MaxBindPoint[D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER] + 1, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, m_pResources->GetShaderType());
#endif
    }

    InitVariablesHashMap();
}


void ShaderResourceLayoutD3D12::InitVariablesHashMap()
{
#if USE_VARIABLE_HASH_MAP
    Uint32 TotalResources = GetTotalSrvCbvUavCount();
    for(Uint32 r=0; r < TotalResources; ++r)
    {
        auto &Res = GetSrvCbvUav(r);
        /* HashMapStringKey will make a copy of the string*/
        m_VariableHash.insert( std::make_pair( Diligent::HashMapStringKey(Res.Name), &Res ) );
    }
#endif
}


#define LOG_RESOURCE_BINDING_ERROR(ResType, pResource, VarName, ShaderName, ...)\
{                                                                                                   \
    const auto &ResName = pResource->GetDesc().Name;                                                \
    LOG_ERROR_MESSAGE( "Failed to bind ", ResType, " \"", ResName, "\" to variable \"", VarName,    \
                        "\" in shader \"", ShaderName, "\". ", __VA_ARGS__ );                \
}



void ShaderResourceLayoutD3D12::SRV_CBV_UAV::CacheCB(IDeviceObject *pBuffer, ShaderResourceCacheD3D12::Resource& DstRes, Uint32 ArrayInd, D3D12_CPU_DESCRIPTOR_HANDLE ShdrVisibleHeapCPUDescriptorHandle)
{
    // We cannot use ValidatedCast<> here as the resource retrieved from the
    // resource mapping can be of wrong type
    RefCntAutoPtr<BufferD3D12Impl> pBuffD3D12(pBuffer, IID_BufferD3D12);
    if( pBuffD3D12 )
    {
        if( pBuffD3D12->GetDesc().BindFlags & BIND_UNIFORM_BUFFER )
        {
            if( Attribs.GetVariableType() != SHADER_VARIABLE_TYPE_DYNAMIC && DstRes.pObject != nullptr && DstRes.pObject != pBuffD3D12)
            {
                auto VarTypeStr = GetShaderVariableTypeLiteralName(Attribs.GetVariableType());
                LOG_ERROR_MESSAGE( "Non-null constant buffer is already bound to ", VarTypeStr, " shader variable \"", Attribs.GetPrintName(ArrayInd), "\" in shader \"", m_ParentResLayout.GetShaderName(), "\". Attempring to bind another constant buffer is an error and may cause unpredicted behavior. Use another shader resource binding instance or mark shader variable as dynamic." )
            }

            DstRes.Type = GetResType();
            DstRes.CPUDescriptorHandle = pBuffD3D12->GetCBVHandle();
            VERIFY(DstRes.CPUDescriptorHandle.ptr != 0 || pBuffD3D12->GetDesc().Usage == USAGE_DYNAMIC, "No relevant CBV CPU descriptor handle")
            
            if(ShdrVisibleHeapCPUDescriptorHandle.ptr != 0 )
            {
                ID3D12Device *pd3d12Device = m_ParentResLayout.m_pd3d12Device;
                pd3d12Device->CopyDescriptorsSimple(1, ShdrVisibleHeapCPUDescriptorHandle, DstRes.CPUDescriptorHandle, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            }

            DstRes.pObject = pBuffD3D12;
        }
        else
        {
            LOG_RESOURCE_BINDING_ERROR("buffer", pBuffer, Attribs.GetPrintName(ArrayInd), m_ParentResLayout.GetShaderName(), "Buffer was not created with BIND_UNIFORM_BUFFER flag.")
        }
    }
    else
    {
        LOG_RESOURCE_BINDING_ERROR("buffer", pBuffer, Attribs.GetPrintName(ArrayInd), m_ParentResLayout.GetShaderName(), "Incorrect resource type: buffer is expected.")
    }
}


template<typename TResourceViewType>
struct ResourceViewTraits{};

template<>
struct ResourceViewTraits<ITextureViewD3D12>
{
    static const Char *Name;
    static const INTERFACE_ID &IID;
};
const Char *ResourceViewTraits<ITextureViewD3D12>::Name = "texture view";
const INTERFACE_ID& ResourceViewTraits<ITextureViewD3D12>::IID = IID_TextureViewD3D12;

template<>
struct ResourceViewTraits<IBufferViewD3D12>
{
    static const Char *Name;
    static const INTERFACE_ID &IID;
};
const Char *ResourceViewTraits<IBufferViewD3D12>::Name = "buffer view";
const INTERFACE_ID& ResourceViewTraits<IBufferViewD3D12>::IID = IID_BufferViewD3D12;

template<typename TResourceViewType,        ///< ResType of the view (ITextureViewD3D12 or IBufferViewD3D12)
         typename TViewTypeEnum,            ///< ResType of the expected view type enum (TEXTURE_VIEW_TYPE or BUFFER_VIEW_TYPE)
         typename TBindSamplerProcType>     ///< ResType of the procedure to set sampler
void ShaderResourceLayoutD3D12::SRV_CBV_UAV::CacheResourceView(IDeviceObject *pView, 
                                                               ShaderResourceCacheD3D12::Resource& DstRes, 
                                                               Uint32 ArrayIndex,
                                                               D3D12_CPU_DESCRIPTOR_HANDLE ShdrVisibleHeapCPUDescriptorHandle, 
                                                               TViewTypeEnum dbgExpectedViewType,
                                                               TBindSamplerProcType BindSamplerProc)
{
    // We cannot use ValidatedCast<> here as the resource retrieved from the
    // resource mapping can be of wrong type
    RefCntAutoPtr<TResourceViewType> pViewD3D12(pView, ResourceViewTraits<TResourceViewType>::IID);
    if( pViewD3D12 )
    {
#ifdef VERIFY_SHADER_BINDINGS
        const auto& ViewDesc = pViewD3D12->GetDesc();
        auto ViewType = ViewDesc.ViewType;
        if( ViewType != dbgExpectedViewType )
        {
            const auto *ExpectedViewTypeName = GetViewTypeLiteralName( dbgExpectedViewType );
            const auto *ActualViewTypeName = GetViewTypeLiteralName( ViewType );
            LOG_RESOURCE_BINDING_ERROR(ResourceViewTraits<TResourceViewType>::Name, pViewD3D12, Attribs.GetPrintName(ArrayIndex), m_ParentResLayout.GetShaderName(), 
                                        "Incorrect view type: ", ExpectedViewTypeName, " is expected, ", ActualViewTypeName, " provided." );
            return;
        }
#endif
        if( Attribs.GetVariableType() != SHADER_VARIABLE_TYPE_DYNAMIC && DstRes.pObject != nullptr && DstRes.pObject != pViewD3D12)
        {
            auto VarTypeStr = GetShaderVariableTypeLiteralName(Attribs.GetVariableType());
            LOG_ERROR_MESSAGE( "Non-null resource is already bound to ", VarTypeStr, " shader variable \"", Attribs.GetPrintName(ArrayIndex), "\" in shader \"", m_ParentResLayout.GetShaderName(), "\". Attempting to bind another resource or null is an error and may cause unpredicted behavior. Use another shader resource binding instance or mark shader variable as dynamic." );
        }

        DstRes.Type = GetResType();
        DstRes.pObject = pViewD3D12;
        DstRes.CPUDescriptorHandle = pViewD3D12->GetCPUDescriptorHandle();
        VERIFY(DstRes.CPUDescriptorHandle.ptr != 0, "No relevant D3D12 view")

        if(ShdrVisibleHeapCPUDescriptorHandle.ptr != 0)
        {
            ID3D12Device *pd3d12Device = m_ParentResLayout.m_pd3d12Device;
            pd3d12Device->CopyDescriptorsSimple(1, ShdrVisibleHeapCPUDescriptorHandle, DstRes.CPUDescriptorHandle, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }

        BindSamplerProc(pViewD3D12);
    }
    else
    {
        LOG_RESOURCE_BINDING_ERROR("resource", pView, Attribs.GetPrintName(ArrayIndex), m_ParentResLayout.GetShaderName(), "Incorect resource type: ", ResourceViewTraits<TResourceViewType>::Name, " is expected.")
    }   
}

void ShaderResourceLayoutD3D12::Sampler::CacheSampler(ITextureViewD3D12 *pTexViewD3D12, Uint32 ArrayIndex, D3D12_CPU_DESCRIPTOR_HANDLE ShdrVisibleHeapCPUDescriptorHandle)
{
    auto &pResourceCache = m_ParentResLayout.m_pResourceCache;
    VERIFY(pResourceCache, "Resource cache is null");
    VERIFY(Attribs.IsValidBindPoint(), "Invalid bind point");
    VERIFY_EXPR(ArrayIndex < Attribs.BindCount);

    auto &DstSam = pResourceCache->GetRootTable(RootIndex).GetResource(OffsetFromTableStart + ArrayIndex, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, m_ParentResLayout.m_pResources->GetShaderType());

#ifdef _DEBUG
    {
        if (pResourceCache->DbgGetContentType() == ShaderResourceCacheD3D12::DbgCacheContentType::StaticShaderResources)
        {
            VERIFY(ShdrVisibleHeapCPUDescriptorHandle.ptr == 0, "Static shader resources of a shader should not be assigned shader visible descriptor space")
        }
        else if (pResourceCache->DbgGetContentType() == ShaderResourceCacheD3D12::DbgCacheContentType::SRBResources)
        {
            if(Attribs.GetVariableType() == SHADER_VARIABLE_TYPE_DYNAMIC)
                VERIFY(ShdrVisibleHeapCPUDescriptorHandle.ptr == 0, "Dynamic resources of a shader resource binding should be assigned shader visible descriptor space at every draw call")
            else
                VERIFY(ShdrVisibleHeapCPUDescriptorHandle.ptr != 0 || pTexViewD3D12 == nullptr, "Non-dynamics resources of a shader resource binding must be assigned shader visible descriptor space")
        }
        else
        {
            UNEXPECTED("Unknown content type")
        }
    }
#endif

    if( pTexViewD3D12 )
    {
        auto pSampler = pTexViewD3D12->GetSampler();
        if( pSampler )
        {
            if( Attribs.GetVariableType() != SHADER_VARIABLE_TYPE_DYNAMIC && DstSam.pObject != nullptr && DstSam.pObject != pSampler)
            {
                auto VarTypeStr = GetShaderVariableTypeLiteralName(Attribs.GetVariableType());
                LOG_ERROR_MESSAGE( "Non-null sampler is already bound to ", VarTypeStr, " shader variable \"", Attribs.GetPrintName(ArrayIndex), "\" in shader \"", m_ParentResLayout.GetShaderName(), "\". Attempting to bind another sampler is an error and may cause unpredicted behavior. Use another shader resource binding instance or mark shader variable as dynamic." )
            }

            DstSam.Type = CachedResourceType::Sampler;

            auto *pSamplerD3D12 = ValidatedCast<SamplerD3D12Impl>(pSampler);
            DstSam.CPUDescriptorHandle = pSamplerD3D12->GetCPUDescriptorHandle();
            VERIFY(DstSam.CPUDescriptorHandle.ptr != 0, "No relevant D3D12 sampler descriptor handle")

            if(ShdrVisibleHeapCPUDescriptorHandle.ptr != 0)
            {
                ID3D12Device *pd3d12Device = m_ParentResLayout.m_pd3d12Device;
                pd3d12Device->CopyDescriptorsSimple(1, ShdrVisibleHeapCPUDescriptorHandle, DstSam.CPUDescriptorHandle, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
            }

            DstSam.pObject = pSampler;
        }
        else
        {
            LOG_ERROR_MESSAGE( "Failed to bind sampler to variable \"", Attribs.Name, ". Sampler is not set in the texture view \"", pTexViewD3D12->GetDesc().Name, "\"" );
        }
    }
    else
    {
        DstSam = ShaderResourceCacheD3D12::Resource();
    }
} 


ShaderResourceLayoutD3D12::Sampler &ShaderResourceLayoutD3D12::GetAssignedSampler(const SRV_CBV_UAV &TexSrv)
{
    VERIFY(TexSrv.GetResType() == CachedResourceType::TexSRV, "Unexpected resource type: texture SRV is expected")
    VERIFY(TexSrv.IsValidSampler(), "Texture SRV has no associated sampler")
    auto &SamInfo = GetSampler(TexSrv.Attribs.GetVariableType(), TexSrv.GetSamplerId());
    VERIFY(SamInfo.Attribs.GetVariableType() == TexSrv.Attribs.GetVariableType(), "Inconsistent texture and sampler variable types")
    VERIFY(SamInfo.Attribs.Name == TexSrv.Attribs.Name + D3DSamplerSuffix, "Sampler name \"", SamInfo.Attribs.Name, "\" does not match texture name \"", TexSrv.Attribs.Name, '\"');
    return SamInfo;
}


void ShaderResourceLayoutD3D12::SRV_CBV_UAV::BindResource(IDeviceObject *pObj, Uint32 ArrayIndex, const ShaderResourceLayoutD3D12 *dbgResLayout)
{
    auto &pResourceCache = m_ParentResLayout.m_pResourceCache;
    VERIFY(pResourceCache, "Resource cache is null");
    VERIFY(dbgResLayout == nullptr || pResourceCache == dbgResLayout->m_pResourceCache, "Invalid resource cache");
    VERIFY_EXPR(ArrayIndex < Attribs.BindCount);

    auto &DstRes = pResourceCache->GetRootTable(GetRootIndex()).GetResource(OffsetFromTableStart + ArrayIndex, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, m_ParentResLayout.m_pResources->GetShaderType());
    auto ShdrVisibleHeapCPUDescriptorHandle = pResourceCache->GetShaderVisibleTableCPUDescriptorHandle<D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV>(GetRootIndex(), OffsetFromTableStart+ArrayIndex);

#ifdef _DEBUG
    {
        if (pResourceCache->DbgGetContentType() == ShaderResourceCacheD3D12::DbgCacheContentType::StaticShaderResources)
        {
            VERIFY(ShdrVisibleHeapCPUDescriptorHandle.ptr == 0, "Static shader resources of a shader should not be assigned shader visible descriptor space")
        }
        else if (pResourceCache->DbgGetContentType() == ShaderResourceCacheD3D12::DbgCacheContentType::SRBResources)
        {
            if (GetResType() == CachedResourceType::CBV)
            {
                VERIFY(ShdrVisibleHeapCPUDescriptorHandle.ptr == 0, "Constant buffers are bound as root views and should not be assigned shader visible descriptor space")
            }
            else
            {
                if(Attribs.GetVariableType() == SHADER_VARIABLE_TYPE_DYNAMIC)
                    VERIFY(ShdrVisibleHeapCPUDescriptorHandle.ptr == 0, "Dynamic resources of a shader resource binding should be assigned shader visible descriptor space at every draw call")
                else
                    VERIFY(ShdrVisibleHeapCPUDescriptorHandle.ptr != 0, "Non-dynamics resources of a shader resource binding must be assigned shader visible descriptor space")
            }
        }
        else
        {
            UNEXPECTED("Unknown content type")
        }
    }
#endif

    if( pObj )
    {
        switch (GetResType())
        {
            case CachedResourceType::CBV:
                CacheCB(pObj, DstRes, ArrayIndex, ShdrVisibleHeapCPUDescriptorHandle); 
            break;
            
            case CachedResourceType::TexSRV: 
                CacheResourceView<ITextureViewD3D12>(pObj, DstRes, ArrayIndex, ShdrVisibleHeapCPUDescriptorHandle, TEXTURE_VIEW_SHADER_RESOURCE, [&](ITextureViewD3D12* pTexView)
                {
                    if(IsValidSampler())
                    {
                        auto &Sam = m_ParentResLayout.GetAssignedSampler(*this);
                        VERIFY( !Sam.Attribs.IsStaticSampler(), "Static samplers should never be assigned space in the cache" )
                        VERIFY_EXPR(Attribs.BindCount == Sam.Attribs.BindCount || Sam.Attribs.BindCount == 1);
                        auto SamplerArrInd = Sam.Attribs.BindCount > 1 ? ArrayIndex : 0;
                        auto ShdrVisibleSamplerHeapCPUDescriptorHandle = pResourceCache->GetShaderVisibleTableCPUDescriptorHandle<D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER>(Sam.RootIndex, Sam.OffsetFromTableStart + SamplerArrInd);
                        Sam.CacheSampler(pTexView, SamplerArrInd, ShdrVisibleSamplerHeapCPUDescriptorHandle);
                    }
                });
            break;

            case CachedResourceType::TexUAV: 
                CacheResourceView<ITextureViewD3D12>(pObj, DstRes, ArrayIndex, ShdrVisibleHeapCPUDescriptorHandle, TEXTURE_VIEW_UNORDERED_ACCESS, [](ITextureViewD3D12*){});
            break;

            case CachedResourceType::BufSRV: 
                CacheResourceView<IBufferViewD3D12>(pObj, DstRes, ArrayIndex, ShdrVisibleHeapCPUDescriptorHandle, BUFFER_VIEW_SHADER_RESOURCE, [](IBufferViewD3D12*){});
            break;

            case CachedResourceType::BufUAV: 
                CacheResourceView<IBufferViewD3D12>( pObj, DstRes, ArrayIndex, ShdrVisibleHeapCPUDescriptorHandle, BUFFER_VIEW_UNORDERED_ACCESS, [](IBufferViewD3D12*){});
            break;

            default: UNEXPECTED("Unknown resource type ", static_cast<Int32>(GetResType()))
        }
    }
    else
    {
        if (DstRes.pObject && Attribs.GetVariableType() != SHADER_VARIABLE_TYPE_DYNAMIC)
        {
            LOG_ERROR_MESSAGE( "Shader variable \"", Attribs.Name, "\" in shader \"", m_ParentResLayout.GetShaderName(), "\" is not dynamic but being unbound. This is an error and may cause unpredicted behavior. Use another shader resource binding instance or mark shader variable as dynamic if you need to bind another resource." );
        }

        DstRes = ShaderResourceCacheD3D12::Resource();
        if(IsValidSampler())
        {
            auto &Sam = m_ParentResLayout.GetAssignedSampler(*this);
            D3D12_CPU_DESCRIPTOR_HANDLE NullHandle = {0};
            auto SamplerArrInd = Sam.Attribs.BindCount > 1 ? ArrayIndex : 0;
            Sam.CacheSampler(nullptr, SamplerArrInd, NullHandle);
        }
    }
}

bool ShaderResourceLayoutD3D12::SRV_CBV_UAV::IsBound(Uint32 ArrayIndex)
{
    auto &pResourceCache = m_ParentResLayout.m_pResourceCache;
    VERIFY(pResourceCache, "Resource cache is null");
    VERIFY_EXPR(ArrayIndex < Attribs.BindCount);

    auto RootIndex = GetRootIndex();
    if( RootIndex < pResourceCache->GetNumRootTables() )
    {
        auto &RootTable = pResourceCache->GetRootTable(RootIndex);
        if(OffsetFromTableStart + ArrayIndex < RootTable.GetSize())
        {
            auto &CachedRes = RootTable.GetResource(OffsetFromTableStart + ArrayIndex, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, m_ParentResLayout.m_pResources->GetShaderType());
            if( CachedRes.pObject != nullptr )
            {
                VERIFY(CachedRes.CPUDescriptorHandle.ptr != 0 || ValidatedCast<BufferD3D12Impl>(CachedRes.pObject.RawPtr())->GetDesc().Usage == USAGE_DYNAMIC, "No relevant descriptor handle")
                return true;
            }
        }
    }

    return false;
}



void ShaderResourceLayoutD3D12::BindResources( IResourceMapping* pResourceMapping, Uint32 Flags, const ShaderResourceCacheD3D12 *dbgResourceCache )
{
    VERIFY(dbgResourceCache == m_pResourceCache, "Resource cache does not match the cache provided at initialization");

    if( !pResourceMapping )
    {
        LOG_ERROR_MESSAGE( "Failed to bind resources in shader \"", GetShaderName(), "\": resource mapping is null" );
        return;
    }

    // Bind constant buffers
    Uint32 TotalResources = GetTotalSrvCbvUavCount();
    for(Uint32 r=0; r < TotalResources; ++r)
    {
        auto &Res = GetSrvCbvUav(r);
        for(Uint32 ArrInd = 0; ArrInd < Res.Attribs.BindCount; ++ArrInd)
        {
            if( Flags & BIND_SHADER_RESOURCES_RESET_BINDINGS )
                Res.BindResource(nullptr, ArrInd, this);

            if( (Flags & BIND_SHADER_RESOURCES_UPDATE_UNRESOLVED) && Res.IsBound(ArrInd) )
                return;

            const auto& VarName = Res.Attribs.Name;
            RefCntAutoPtr<IDeviceObject> pObj;
            VERIFY_EXPR(pResourceMapping != nullptr);
            pResourceMapping->GetResource( VarName.c_str(), &pObj, ArrInd );
            if( pObj )
            {
                //  Call non-virtual function
                Res.BindResource(pObj, ArrInd, this);
            }
            else
            {
                if( (Flags & BIND_SHADER_RESOURCES_ALL_RESOLVED) && !Res.IsBound(ArrInd) )
                    LOG_ERROR_MESSAGE( "Cannot bind resource to shader variable \"", Res.Attribs.GetPrintName(ArrInd), "\": resource view not found in the resource mapping" )
            }
        }
    }
}


IShaderVariable* ShaderResourceLayoutD3D12::GetShaderVariable(const Char* Name)
{
    IShaderVariable* pVar = nullptr;
#if USE_VARIABLE_HASH_MAP
    // Name will be implicitly converted to HashMapStringKey without making a copy
    auto it = m_VariableHash.find( Name );
    if( it != m_VariableHash.end() )
        pVar = it->second;
#else
    Uint32 TotalResources = GetTotalSrvCbvUavCount();
    for(Uint32 r=0; r < TotalResources; ++r)
    {
        auto &Res = GetSrvCbvUav(r);
        if(Res.Attribs.Name.compare(Name) == 0)
        {
            pVar = &Res;
            break;
        }
    }
#endif

    if(pVar == nullptr)
    {
        LOG_ERROR_MESSAGE( "Shader variable \"", Name, "\" is not found in shader \"", GetShaderName(), "\" (", GetShaderTypeLiteralName(m_pResources->GetShaderType()), "). Attempts to set the variable will be silently ignored." );
    }
    return pVar;
}



void ShaderResourceLayoutD3D12::CopyStaticResourceDesriptorHandles(const ShaderResourceLayoutD3D12 &SrcLayout)
{
    if (!m_pResourceCache)
    {
        LOG_ERROR("Resource layout has no resource cache");
        return;
    }

    if (!SrcLayout.m_pResourceCache)
    {
        LOG_ERROR("Dst layout has no resource cache");
        return;
    }

    // Static shader resources are stored as follows:
    // CBVs at root index D3D12_DESCRIPTOR_RANGE_TYPE_CBV,
    // SRVs at root index D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
    // UAVs at root index D3D12_DESCRIPTOR_RANGE_TYPE_UAV, and
    // Samplers at root index D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER
    // Every resource is stored at offset that equals resource bind point

    for(Uint32 r=0; r < m_NumCbvSrvUav[SHADER_VARIABLE_TYPE_STATIC]; ++r)
    {
        const auto &res = GetSrvCbvUav(SHADER_VARIABLE_TYPE_STATIC, r);
        VERIFY(SrcLayout.m_pResources->GetShaderType() == m_pResources->GetShaderType(), "Incosistent shader types")
        auto RangeType = GetDescriptorRangeType(res.GetResType());
        for(Uint32 ArrInd = 0; ArrInd < res.Attribs.BindCount; ++ArrInd)
        {
            auto BindPoint = res.Attribs.BindPoint + ArrInd;
            // Static resources are indexed in the resource cache by its bind point
            const auto &SrcRes = SrcLayout.m_pResourceCache->GetRootTable(RangeType).GetResource(BindPoint, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, SrcLayout.m_pResources->GetShaderType());
            if( !SrcRes.pObject )
                LOG_ERROR_MESSAGE( "No resource assigned to static shader variable \"", res.Attribs.GetPrintName(ArrInd), "\" in shader \"", GetShaderName(), "\"." );
            auto &DstRes = m_pResourceCache->GetRootTable(res.GetRootIndex()).GetResource(res.OffsetFromTableStart + ArrInd, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, m_pResources->GetShaderType());
        
            if(DstRes.pObject != SrcRes.pObject)
            {
                VERIFY(DstRes.pObject == nullptr, "Static resource has already been initialized, and the resource to be assigned from the shader does not match previously assigned resource");

                DstRes.pObject = SrcRes.pObject;
                DstRes.Type = SrcRes.Type;
                DstRes.CPUDescriptorHandle = SrcRes.CPUDescriptorHandle;

                auto ShdrVisibleHeapCPUDescriptorHandle = m_pResourceCache->GetShaderVisibleTableCPUDescriptorHandle<D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV>(res.GetRootIndex(), res.OffsetFromTableStart + ArrInd);
                VERIFY_EXPR(ShdrVisibleHeapCPUDescriptorHandle.ptr != 0 || DstRes.Type == CachedResourceType::CBV);
                if (ShdrVisibleHeapCPUDescriptorHandle.ptr != 0)
                {
                    m_pd3d12Device->CopyDescriptorsSimple(1, ShdrVisibleHeapCPUDescriptorHandle, SrcRes.CPUDescriptorHandle, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                }
            }
            else
            {
                VERIFY_EXPR(DstRes.pObject == SrcRes.pObject);
                VERIFY_EXPR(DstRes.Type == SrcRes.Type);
                VERIFY_EXPR(DstRes.CPUDescriptorHandle.ptr == SrcRes.CPUDescriptorHandle.ptr);
            }
        }

        if(res.IsValidSampler())
        {
            auto &SamInfo = GetAssignedSampler(res);

            VERIFY(!SamInfo.Attribs.IsStaticSampler(), "Static samplers should never be assigned space in the cache")
            
            VERIFY(SamInfo.Attribs.IsValidBindPoint(), "Sampler bind point must be valid")
            VERIFY_EXPR(SamInfo.Attribs.BindCount == res.Attribs.BindCount || SamInfo.Attribs.BindCount == 1);

            for(Uint32 ArrInd = 0; ArrInd < SamInfo.Attribs.BindCount; ++ArrInd)
            {
                auto BindPoint = SamInfo.Attribs.BindPoint + ArrInd;
                // Static resources are indexed by the bind point in the resource cache
                auto& SrcSampler = SrcLayout.m_pResourceCache->GetRootTable(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER).GetResource(BindPoint, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, SrcLayout.m_pResources->GetShaderType());
                if( !SrcSampler.pObject )
                    LOG_ERROR_MESSAGE( "No sampler assigned to static shader variable \"", res.Attribs.GetPrintName(ArrInd), "\" in shader \"", GetShaderName(), "\"." );
                auto &DstSampler = m_pResourceCache->GetRootTable(SamInfo.RootIndex).GetResource(SamInfo.OffsetFromTableStart + ArrInd, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, m_pResources->GetShaderType());
            
                if(DstSampler.pObject != SrcSampler.pObject)
                {
                    VERIFY(DstSampler.pObject == nullptr, "Static sampler resource has already been initialized, and the resource to be assigned from the shader does not match previously assigned resource");

                    DstSampler.pObject = SrcSampler.pObject;
                    DstSampler.Type = SrcSampler.Type;
                    DstSampler.CPUDescriptorHandle = SrcSampler.CPUDescriptorHandle;

                    auto ShdrVisibleSamplerHeapCPUDescriptorHandle = m_pResourceCache->GetShaderVisibleTableCPUDescriptorHandle<D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER>(SamInfo.RootIndex, SamInfo.OffsetFromTableStart + ArrInd);
                    VERIFY_EXPR(ShdrVisibleSamplerHeapCPUDescriptorHandle.ptr != 0);
                    if (ShdrVisibleSamplerHeapCPUDescriptorHandle.ptr != 0)
                    {
                        m_pd3d12Device->CopyDescriptorsSimple(1, ShdrVisibleSamplerHeapCPUDescriptorHandle, SrcSampler.CPUDescriptorHandle, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
                    }
                }
                else
                {
                    VERIFY_EXPR(DstSampler.pObject == SrcSampler.pObject);
                    VERIFY_EXPR(DstSampler.Type == SrcSampler.Type);
                    VERIFY_EXPR(DstSampler.CPUDescriptorHandle.ptr == SrcSampler.CPUDescriptorHandle.ptr);
                }
            }
        }
    }
}


#ifdef VERIFY_SHADER_BINDINGS
void ShaderResourceLayoutD3D12::dbgVerifyBindings()const
{
    VERIFY(m_pResourceCache, "Resource cache is null")

    for(SHADER_VARIABLE_TYPE VarType = SHADER_VARIABLE_TYPE_STATIC; VarType < SHADER_VARIABLE_TYPE_NUM_TYPES; VarType = static_cast<SHADER_VARIABLE_TYPE>(VarType+1))
    {
        for(Uint32 r=0; r < m_NumCbvSrvUav[VarType]; ++r)
        {
            const auto &res = GetSrvCbvUav(VarType, r);
            VERIFY(res.Attribs.GetVariableType() == VarType, "Unexpected variable type")

            for(Uint32 ArrInd = 0; ArrInd < res.Attribs.BindCount; ++ArrInd)
            {
                const auto &CachedRes = m_pResourceCache->GetRootTable(res.GetRootIndex()).GetResource(res.OffsetFromTableStart + ArrInd, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, m_pResources->GetShaderType());
                if(CachedRes.pObject)
                    VERIFY(CachedRes.Type == res.GetResType(), "Inconsistent cached resource types")
                else
                    VERIFY(CachedRes.Type == CachedResourceType::Unknown, "Unexpected cached resource types")

                if( !CachedRes.pObject || 
                     (CachedRes.CPUDescriptorHandle.ptr == 0 && ValidatedCast<const BufferD3D12Impl>(CachedRes.pObject.RawPtr())->GetDesc().Usage != USAGE_DYNAMIC) )
                    LOG_ERROR_MESSAGE( "No resource is bound to ", GetShaderVariableTypeLiteralName(res.Attribs.GetVariableType()), " variable \"", res.Attribs.GetPrintName(ArrInd), "\" in shader \"", GetShaderName(), "\"" )
                
                if (res.Attribs.BindCount > 1 && res.IsValidSampler())
                {
                    // Verify that if single sampler is used for all texture array elements, all samplers set in the resource views are consistent
                    const auto &SamInfo = const_cast<ShaderResourceLayoutD3D12*>(this)->GetAssignedSampler(res);
                    if(SamInfo.Attribs.BindCount == 1)
                    {
                        const auto &CachedSampler = m_pResourceCache->GetRootTable(SamInfo.RootIndex).GetResource(SamInfo.OffsetFromTableStart, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, m_pResources->GetShaderType());
                        if( auto *pTexView = ValidatedCast<const ITextureView>(CachedRes.pObject.RawPtr()) )
                        {
                            auto *pSampler = const_cast<ITextureView*>(pTexView)->GetSampler();
                            if (pSampler != nullptr && CachedSampler.pObject != pSampler)
                                LOG_ERROR_MESSAGE( "All elements of texture array \"", res.Attribs.Name, "\" in shader \"", GetShaderName(), "\" share the same sampler. However, the sampler set in view for element ", ArrInd, " does not match bound sampler. This may cause incorrect behavior on GL platform."  )
                        }
                    }
                }

#ifdef _DEBUG
                {
                    auto ShdrVisibleHeapCPUDescriptorHandle = m_pResourceCache->GetShaderVisibleTableCPUDescriptorHandle<D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV>(res.GetRootIndex(), res.OffsetFromTableStart + ArrInd);
                    if (m_pResourceCache->DbgGetContentType() == ShaderResourceCacheD3D12::DbgCacheContentType::StaticShaderResources)
                    {
                        VERIFY(ShdrVisibleHeapCPUDescriptorHandle.ptr == 0, "Static shader resources of a shader should not be assigned shader visible descriptor space")
                    }
                    else if (m_pResourceCache->DbgGetContentType() == ShaderResourceCacheD3D12::DbgCacheContentType::SRBResources)
                    {
                        if (res.GetResType() == CachedResourceType::CBV)
                        {
                            VERIFY(ShdrVisibleHeapCPUDescriptorHandle.ptr == 0, "Constant buffers are bound as root views and should not be assigned shader visible descriptor space")
                        }
                        else
                        {
                            if(res.Attribs.GetVariableType() == SHADER_VARIABLE_TYPE_DYNAMIC)
                                VERIFY(ShdrVisibleHeapCPUDescriptorHandle.ptr == 0, "Dynamic resources of a shader resource binding should be assigned shader visible descriptor space at every draw call")
                            else
                                VERIFY(ShdrVisibleHeapCPUDescriptorHandle.ptr != 0, "Non-dynamics resources of a shader resource binding must be assigned shader visible descriptor space")
                        }
                    }
                    else
                    {
                        UNEXPECTED("Unknown content type")
                    }
                }
#endif
            }

            if (res.IsValidSampler())
            {
                VERIFY(res.GetResType() == CachedResourceType::TexSRV, "Sampler can only be assigned to texture SRV" );
                const auto &SamInfo = const_cast<ShaderResourceLayoutD3D12*>(this)->GetAssignedSampler(res);
                VERIFY( !SamInfo.Attribs.IsStaticSampler(), "Static samplers should never be assigned space in the cache" )
                VERIFY(SamInfo.Attribs.IsValidBindPoint(), "Sampler bind point must be valid")
                
                for(Uint32 ArrInd = 0; ArrInd < SamInfo.Attribs.BindCount; ++ArrInd)
                {
                    const auto &CachedSampler = m_pResourceCache->GetRootTable(SamInfo.RootIndex).GetResource(SamInfo.OffsetFromTableStart + ArrInd, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, m_pResources->GetShaderType());
                    if( CachedSampler.pObject )
                        VERIFY(CachedSampler.Type == CachedResourceType::Sampler, "Incorrect cached sampler type")
                    else
                        VERIFY(CachedSampler.Type == CachedResourceType::Unknown, "Unexpected cached sampler type")
                    if( !CachedSampler.pObject || CachedSampler.CPUDescriptorHandle.ptr == 0 )
                        LOG_ERROR_MESSAGE("No sampler is assigned to texture variable \"", res.Attribs.GetPrintName(ArrInd), "\" in shader \"", GetShaderName(), "\"")

    #ifdef _DEBUG
                    {
                        auto ShdrVisibleHeapCPUDescriptorHandle = m_pResourceCache->GetShaderVisibleTableCPUDescriptorHandle<D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER>(SamInfo.RootIndex, SamInfo.OffsetFromTableStart + ArrInd);
                        if (m_pResourceCache->DbgGetContentType() == ShaderResourceCacheD3D12::DbgCacheContentType::StaticShaderResources)
                        {
                            VERIFY(ShdrVisibleHeapCPUDescriptorHandle.ptr == 0, "Static shader resources of a shader should not be assigned shader visible descriptor space")
                        }
                        else if (m_pResourceCache->DbgGetContentType() == ShaderResourceCacheD3D12::DbgCacheContentType::SRBResources)
                        {
                            if(SamInfo.Attribs.GetVariableType() == SHADER_VARIABLE_TYPE_DYNAMIC)
                                VERIFY(ShdrVisibleHeapCPUDescriptorHandle.ptr == 0, "Dynamic resources of a shader resource binding should be assigned shader visible descriptor space at every draw call")
                            else
                                VERIFY(ShdrVisibleHeapCPUDescriptorHandle.ptr != 0, "Non-dynamics resources of a shader resource binding must be assigned shader visible descriptor space")
                        }
                        else
                        {
                            UNEXPECTED("Unknown content type")
                        }
                    }
    #endif
                }
            }
        }

        for(Uint32 s=0; s < m_NumSamplers[VarType]; ++s)
        {
            const auto &sam = GetSampler(VarType, s);
            VERIFY(sam.Attribs.GetVariableType() == VarType, "Unexpected sampler variable type")
            
            for(Uint32 ArrInd = 0; ArrInd < sam.Attribs.BindCount; ++ArrInd)
            {
                const auto &CachedSampler = m_pResourceCache->GetRootTable(sam.RootIndex).GetResource(sam.OffsetFromTableStart + ArrInd, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, m_pResources->GetShaderType());
                if( CachedSampler.pObject )
                    VERIFY(CachedSampler.Type == CachedResourceType::Sampler, "Incorrect cached sampler type")
                else
                    VERIFY(CachedSampler.Type == CachedResourceType::Unknown, "Unexpected cached sampler type")
                if( !CachedSampler.pObject || CachedSampler.CPUDescriptorHandle.ptr == 0 )
                    LOG_ERROR_MESSAGE( "No sampler is bound to sampler variable \"", sam.Attribs.GetPrintName(ArrInd), "\" in shader \"", GetShaderName(), "\"" )
            }
        }
    }
}
#endif

const Char* ShaderResourceLayoutD3D12::GetShaderName()const
{
    RefCntAutoPtr<IShader> pShader(&m_Owner, IID_Shader);
    if (pShader)
    {
        return pShader->GetDesc().Name;
    }
    else
    {
        RefCntAutoPtr<IShaderResourceBinding> pSRB(&m_Owner, IID_ShaderResourceBinding);
        if(pSRB)
        {
            auto *pPSO = pSRB->GetPipelineState();
            auto *pPSOD3D12 = ValidatedCast<PipelineStateD3D12Impl>(pPSO);
            auto *ppShaders = pPSOD3D12->GetShaders();
            auto NumShaders = pPSOD3D12->GetNumShaders();
            for (Uint32 s = 0; s < NumShaders; ++s)
            {
                const auto &ShaderDesc = ppShaders[s]->GetDesc();
                if(ShaderDesc.ShaderType == m_pResources->GetShaderType())
                    return ShaderDesc.Name;
            }
            UNEXPECTED("Shader not found")
        }
        else
        {
            UNEXPECTED("Owner is expected to be a shader or a shader resource binding")
        }
    }
    return "";
}

}