/*     Copyright 2015-2018 Egor Yusov
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

#pragma once

/// \file
/// Implementation of the Diligent::PipelineStateBase template class

#include <array>
#include "PipelineState.h"
#include "DeviceObjectBase.h"
#include "STDAllocator.h"
#include "EngineMemory.h"
#include "GraphicsAccessories.h"

namespace Diligent
{

/// Template class implementing base functionality for a pipeline state object.

/// \tparam BaseInterface - base interface that this class will inheret
///                         (Diligent::IPipelineStateD3D11, Diligent::IPipelineStateD3D12,
///                          Diligent::IPipelineStateGL or Diligent::IPipelineStateVk).
/// \tparam RenderDeviceImplType - type of the render device implementation
///                                (Diligent::RenderDeviceD3D11Impl, Diligent::RenderDeviceD3D12Impl,
///                                 Diligent::RenderDeviceGLImpl, or Diligent::RenderDeviceVkImpl)
template<class BaseInterface, class RenderDeviceImplType>
class PipelineStateBase : public DeviceObjectBase<BaseInterface, RenderDeviceImplType, PipelineStateDesc>
{
public:
    using TDeviceObjectBase = DeviceObjectBase<BaseInterface, RenderDeviceImplType, PipelineStateDesc>;

    /// \param pRefCounters - reference counters object that controls the lifetime of this PSO
	/// \param pDevice - pointer to the device.
	/// \param PSODesc - pipeline state description.
	/// \param bIsDeviceInternal - flag indicating if the blend state is an internal device object and 
	///							   must not keep a strong reference to the device.
    PipelineStateBase( IReferenceCounters*      pRefCounters, 
                       RenderDeviceImplType*    pDevice,
                       const PipelineStateDesc& PSODesc,
                       bool                     bIsDeviceInternal = false ) :
        TDeviceObjectBase( pRefCounters, pDevice, PSODesc, bIsDeviceInternal ),
        m_LayoutElements( PSODesc.GraphicsPipeline.InputLayout.NumElements, LayoutElement(), STD_ALLOCATOR_RAW_MEM(LayoutElement, GetRawAllocator(), "Allocator for vector<LayoutElement>" ) ),
        m_NumShaders(0)
    {
        if (this->m_Desc.IsComputePipeline)
        {
            const auto &ComputePipeline = PSODesc.ComputePipeline;
            if (ComputePipeline.pCS == nullptr)
            {
                LOG_ERROR_AND_THROW( "Compute shader is not provided" );
            }

#define VALIDATE_SHADER_TYPE(Shader, ExpectedType, ShaderName)\
            if (Shader && Shader->GetDesc().ShaderType != ExpectedType)   \
            {                                                   \
                LOG_ERROR_AND_THROW( GetShaderTypeLiteralName(Shader->GetDesc().ShaderType), " is not a valid type for ", ShaderName, " shader" );\
            }
            VALIDATE_SHADER_TYPE(ComputePipeline.pCS, SHADER_TYPE_COMPUTE, "compute")

            m_pCS = ComputePipeline.pCS;
            m_ppShaders[0] = ComputePipeline.pCS;
            m_NumShaders = 1;
        }
        else
        {
            const auto &GraphicsPipeline = PSODesc.GraphicsPipeline;

            VALIDATE_SHADER_TYPE(GraphicsPipeline.pVS, SHADER_TYPE_VERTEX,   "vertex")
            VALIDATE_SHADER_TYPE(GraphicsPipeline.pPS, SHADER_TYPE_PIXEL,    "pixel")
            VALIDATE_SHADER_TYPE(GraphicsPipeline.pGS, SHADER_TYPE_GEOMETRY, "geometry")
            VALIDATE_SHADER_TYPE(GraphicsPipeline.pHS, SHADER_TYPE_HULL,     "hull")
            VALIDATE_SHADER_TYPE(GraphicsPipeline.pDS, SHADER_TYPE_DOMAIN,   "domain")
#undef VALIDATE_SHADER_TYPE

            m_pVS = GraphicsPipeline.pVS;
            m_pPS = GraphicsPipeline.pPS;
            m_pGS = GraphicsPipeline.pGS;
            m_pDS = GraphicsPipeline.pDS;
            m_pHS = GraphicsPipeline.pHS;

            if( GraphicsPipeline.pVS )m_ppShaders[m_NumShaders++] = GraphicsPipeline.pVS;
            if( GraphicsPipeline.pPS )m_ppShaders[m_NumShaders++] = GraphicsPipeline.pPS;
            if( GraphicsPipeline.pGS )m_ppShaders[m_NumShaders++] = GraphicsPipeline.pGS;
            if( GraphicsPipeline.pHS )m_ppShaders[m_NumShaders++] = GraphicsPipeline.pHS;
            if( GraphicsPipeline.pDS )m_ppShaders[m_NumShaders++] = GraphicsPipeline.pDS;
        }

        const auto &InputLayout = PSODesc.GraphicsPipeline.InputLayout;
        for( size_t Elem = 0; Elem < InputLayout.NumElements; ++Elem )
            m_LayoutElements[Elem] = InputLayout.LayoutElements[Elem];
        this->m_Desc.GraphicsPipeline.InputLayout.LayoutElements = m_LayoutElements.data();
        
        // Correct description and compute offsets and tight strides
        decltype(m_Strides) TightStrides = {};
        for( auto It = m_LayoutElements.begin(); It != m_LayoutElements.end(); ++It )
        {
            if( It->ValueType == VT_FLOAT32 || It->ValueType == VT_FLOAT16 )
                It->IsNormalized = false; // Floating point values cannot be normalized

            auto BuffSlot = It->BufferSlot;
            if( BuffSlot >= m_Strides.size() )
            {
                UNEXPECTED("Buffer slot (", BuffSlot, ") exceeds the limit (", m_Strides.size(), ")");
                continue;
            }
            m_BufferSlotsUsed = std::max(m_BufferSlotsUsed, BuffSlot + 1);

            auto &CurrStride = TightStrides[BuffSlot];
            if( It->RelativeOffset < CurrStride )
            {
                if( It->RelativeOffset == 0 )
                    It->RelativeOffset = CurrStride;
                else
                    UNEXPECTED( "Overlapping layout elements" );
            }

            if(It->Stride != 0)
            {
                if(m_Strides[BuffSlot] != 0)
                {
                    if (m_Strides[BuffSlot] != It->Stride)
                        LOG_ERROR_AND_THROW("Inconsistent strides specified for buffer slot ", BuffSlot,
                                            ". Current value: ", m_Strides[BuffSlot], ". New value: ", It->Stride);
                }
                m_Strides[BuffSlot] = It->Stride;
            }

            CurrStride += It->NumComponents * GetValueSize( It->ValueType );
        }

        for( auto It = m_LayoutElements.begin(); It != m_LayoutElements.end(); ++It )
        {
            auto BuffSlot = It->BufferSlot;
            if(m_Strides[BuffSlot] == 0)
            {
                m_Strides[BuffSlot] = TightStrides[BuffSlot];
            }
            else
            {
                VERIFY(m_Strides[BuffSlot] >= TightStrides[BuffSlot], "Stride (", m_Strides[BuffSlot], ") explicitly specified for slot ", BuffSlot, " is smaller than the total element size (", TightStrides[BuffSlot], ")");
            }
            if(It->Stride == 0)
                It->Stride = m_Strides[BuffSlot];
            else
                VERIFY(m_Strides[BuffSlot] == It->Stride, "Incosistent stride");
        }
    }

    ~PipelineStateBase()
    {
        /*
        /// \note Destructor cannot directly remove the object from the registry as this may cause a  
        ///       deadlock at the point where StateObjectsRegistry::Find() locks the weak pointer: if we
        ///       are in dtor, the object is locked by Diligent::RefCountedObject::Release() and 
        ///       StateObjectsRegistry::Find() will wait for that lock to be released.
        ///       A the same time this thread will be waiting for the other thread to unlock the registry.\n
        ///       Thus destructor only notifies the registry that there is a deleted object.
        ///       The reference to the object will be removed later.
        auto &PipelineStateRegistry = static_cast<TRenderDeviceBase*>(this->GetDevice())->GetBSRegistry();
        auto &RasterizerStateRegistry = static_cast<TRenderDeviceBase*>(this->GetDevice())->GetRSRegistry();
        auto &DSSRegistry = static_cast<TRenderDeviceBase*>(this->GetDevice())->GetDSSRegistry();
        // StateObjectsRegistry::ReportDeletedObject() does not lock the registry, but only 
        // atomically increments the outstanding deleted objects counter.
        PipelineStateRegistry.ReportDeletedObject();
        RasterizerStateRegistry.ReportDeletedObject();
        DSSRegistry.ReportDeletedObject();
        */
    }

    IMPLEMENT_QUERY_INTERFACE_IN_PLACE( IID_PipelineState, TDeviceObjectBase )

    virtual const Uint32* GetBufferStrides()const
    { 
        return m_Strides.data();
    }

    Uint32 GetNumBufferSlotsUsed()const
    {
        return m_BufferSlotsUsed;
    }

    IShader* GetVS(){return m_pVS;}
    IShader* GetPS(){return m_pPS;}
    IShader* GetGS(){return m_pGS;}
    IShader* GetDS(){return m_pDS;}
    IShader* GetHS(){return m_pHS;}
    IShader* GetCS(){return m_pCS;}

    IShader* const* GetShaders()const{return m_ppShaders;}
    Uint32 GetNumShaders()const{return m_NumShaders;}

    template<typename ShaderType>
    ShaderType* GetShader(Uint32 ShaderInd)
    {
        VERIFY_EXPR(ShaderInd < m_NumShaders);
        return ValidatedCast<ShaderType>(m_ppShaders[ShaderInd]);
    }
    template<typename ShaderType>
    ShaderType* GetShader(Uint32 ShaderInd)const
    {
        VERIFY_EXPR(ShaderInd < m_NumShaders);
        return ValidatedCast<ShaderType>(m_ppShaders[ShaderInd]);
    }

    // This function only compares shader resource layout hashes, so
    // it can potentially give false negatives
    bool IsIncompatibleWith(IPipelineState* pPSO)const
    {
        return m_ShaderResourceLayoutHash != ValidatedCast<PipelineStateBase>(pPSO)->m_ShaderResourceLayoutHash;
    }

    virtual void BindShaderResources( IResourceMapping* pResourceMapping, Uint32 Flags )override
    {
        for(Uint32 s=0; s < m_NumShaders; ++s)
            m_ppShaders[s]->BindResources(pResourceMapping, Flags);
    }

protected:
    std::vector<LayoutElement, STDAllocatorRawMem<LayoutElement> > m_LayoutElements;

    Uint32 m_BufferSlotsUsed = 0;
    // The size of this array must be equal to the
    // maximum number of buffer slots, because a layout 
    // element can refer to any input slot
    std::array<Uint32, MaxBufferSlots> m_Strides = {};

    RefCntAutoPtr<IShader> m_pVS; ///< Strong reference to the vertex shader
    RefCntAutoPtr<IShader> m_pPS; ///< Strong reference to the pixel shader
    RefCntAutoPtr<IShader> m_pGS; ///< Strong reference to the geometry shader
    RefCntAutoPtr<IShader> m_pDS; ///< Strong reference to the domain shader
    RefCntAutoPtr<IShader> m_pHS; ///< Strong reference to the hull shader
    RefCntAutoPtr<IShader> m_pCS; ///< Strong reference to the compute shader
    IShader* m_ppShaders[5] = {}; ///< Array of pointers to the shaders used by this PSO
    Uint32 m_NumShaders = 0;      ///< Number of shaders that this PSO uses
    size_t m_ShaderResourceLayoutHash = 0;///< Hash computed from the shader resource layout
};

}
