/*
 * Copyright (c) 2017-2025 The Forge Interactive Inc.
 *
 * This file is part of The-Forge
 * (see https://github.com/ConfettiFX/The-Forge).
 *
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include "../GraphicsConfig.h"

#ifdef DIRECT3D12
#include "../../Utilities/Interfaces/ILog.h"
#include "../Interfaces/IGraphics.h"

#if defined(XBOX)
#include "../../../Xbox/Common_3/Graphics/Direct3D12/Direct3D12X.h"
#else
#include "../../../Common_3/Graphics/ThirdParty/OpenSource/Direct3d12Agility/include/d3d12shader.h"
#include "../../../Common_3/Graphics/ThirdParty/OpenSource/DirectXShaderCompiler/inc/dxcapi.h"
#endif

#include "../../Utilities/Interfaces/IMemory.h"

static DescriptorType sD3D12_TO_DESCRIPTOR[] = {
    DESCRIPTOR_TYPE_UNIFORM_BUFFER,         // D3D_SIT_CBUFFER
    DESCRIPTOR_TYPE_BUFFER,                 // D3D_SIT_TBUFFER
    DESCRIPTOR_TYPE_TEXTURE,                // D3D_SIT_TEXTURE
    DESCRIPTOR_TYPE_SAMPLER,                // D3D_SIT_SAMPLER
    DESCRIPTOR_TYPE_RW_TEXTURE,             // D3D_SIT_UAV_RWTYPED
    DESCRIPTOR_TYPE_BUFFER,                 // D3D_SIT_STRUCTURED
    DESCRIPTOR_TYPE_RW_BUFFER,              // D3D_SIT_RWSTRUCTURED
    DESCRIPTOR_TYPE_BUFFER,                 // D3D_SIT_BYTEADDRESS
    DESCRIPTOR_TYPE_RW_BUFFER,              // D3D_SIT_UAV_RWBYTEADDRESS
    DESCRIPTOR_TYPE_RW_BUFFER,              // D3D_SIT_UAV_APPEND_STRUCTURED
    DESCRIPTOR_TYPE_RW_BUFFER,              // D3D_SIT_UAV_CONSUME_STRUCTURED
    DESCRIPTOR_TYPE_RW_BUFFER,              // D3D_SIT_UAV_RWSTRUCTURED_WITH_COUNTER
    DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE, // D3D_SIT_RTACCELERATIONSTRUCTURE
};

static TextureDimension sD3D12_TO_RESOURCE_DIM[D3D_SRV_DIMENSION_BUFFEREX + 1] = {
    TEXTURE_DIM_UNDEFINED,  // D3D_SRV_DIMENSION_UNKNOWN
    TEXTURE_DIM_UNDEFINED,  // D3D_SRV_DIMENSION_BUFFER
    TEXTURE_DIM_1D,         // D3D_SRV_DIMENSION_TEXTURE1D
    TEXTURE_DIM_1D_ARRAY,   // D3D_SRV_DIMENSION_TEXTURE1DARRAY
    TEXTURE_DIM_2D,         // D3D_SRV_DIMENSION_TEXTURE2D
    TEXTURE_DIM_2D_ARRAY,   // D3D_SRV_DIMENSION_TEXTURE2DARRAY
    TEXTURE_DIM_2DMS,       // D3D_SRV_DIMENSION_TEXTURE2DMS
    TEXTURE_DIM_2DMS_ARRAY, // D3D_SRV_DIMENSION_TEXTURE2DMSARRAY
    TEXTURE_DIM_3D,         // D3D_SRV_DIMENSION_TEXTURE3D
    TEXTURE_DIM_CUBE,       // D3D_SRV_DIMENSION_TEXTURECUBE
    TEXTURE_DIM_CUBE_ARRAY, // D3D_SRV_DIMENSION_TEXTURECUBEARRAY
    TEXTURE_DIM_UNDEFINED,  // D3D_SRV_DIMENSION_BUFFEREX
};

template<typename ID3D12ReflectionT, typename D3D12_SHADER_DESC_T>
void calculate_bound_resource_count(ID3D12ReflectionT* d3d12reflection, const D3D12_SHADER_DESC_T& shaderDesc, ShaderReflection& reflection)
{
    // Count string sizes of the bound resources for the name pool
    for (UINT i = 0; i < shaderDesc.BoundResources; ++i)
    {
        D3D12_SHADER_INPUT_BIND_DESC bindDesc;
        d3d12reflection->GetResourceBindingDesc(i, &bindDesc);
        reflection.mNamePoolSize += (uint32_t)strlen(bindDesc.Name) + 1;
    }

    // Count the number of variables and add to the size of the string pool
    for (UINT i = 0; i < shaderDesc.ConstantBuffers; ++i)
    {
        ID3D12ShaderReflectionConstantBuffer* buffer = d3d12reflection->GetConstantBufferByIndex(i);

        D3D12_SHADER_BUFFER_DESC bufferDesc;
        buffer->GetDesc(&bufferDesc);

        // We only care about constant buffers
        if (bufferDesc.Type != D3D_CT_CBUFFER)
            continue;

        for (UINT v = 0; v < bufferDesc.Variables; ++v)
        {
            ID3D12ShaderReflectionVariable* variable = buffer->GetVariableByIndex(v);

            D3D12_SHADER_VARIABLE_DESC varDesc;
            variable->GetDesc(&varDesc);

            // Only count used variables
            if ((varDesc.uFlags | D3D_SVF_USED) != 0)
            {
                reflection.mNamePoolSize += (uint32_t)strlen(varDesc.Name) + 1;
            }
        }
    }
}

// template<typename RefInterface = ID3D12ShaderReflection>
void d3d12_addShaderReflection(ID3D12ShaderReflection* d3d12reflection, ShaderStage shaderStage, ShaderReflection& reflection)
{
    ASSERT(d3d12reflection);

    // Get a description of this shader
    D3D12_SHADER_DESC shaderDesc;
    d3d12reflection->GetDesc(&shaderDesc);

    calculate_bound_resource_count(d3d12reflection, shaderDesc, reflection);

    // Get the number of input parameters
    reflection.mVertexInputsCount = 0;

    if (shaderStage == SHADER_STAGE_VERT)
    {
        reflection.mVertexInputsCount = shaderDesc.InputParameters;

        // Count the string sizes of the vertex inputs for the name pool
        for (UINT i = 0; i < shaderDesc.InputParameters; ++i)
        {
            D3D12_SIGNATURE_PARAMETER_DESC paramDesc;
            d3d12reflection->GetInputParameterDesc(i, &paramDesc);
            reflection.mNamePoolSize += (uint32_t)strlen(paramDesc.SemanticName) + 2;
        }
    }
    // Get the number of threads per group
    else if (shaderStage == SHADER_STAGE_COMP)
    {
        d3d12reflection->GetThreadGroupSize(&reflection.mNumThreadsPerGroup[0], &reflection.mNumThreadsPerGroup[1],
                                            &reflection.mNumThreadsPerGroup[2]);
    }
    // Get the number of cnotrol point
    else if (shaderStage == SHADER_STAGE_TESC)
    {
        reflection.mNumControlPoint = shaderDesc.cControlPoints;
    }

    // Allocate memory for the name pool
    if (reflection.mNamePoolSize)
    {
        reflection.pNamePool = (char*)tf_calloc(reflection.mNamePoolSize, 1);
    }

    char* pCurrentName = reflection.pNamePool;

    reflection.pVertexInputs = NULL;
    if (shaderStage == SHADER_STAGE_VERT && reflection.mVertexInputsCount > 0)
    {
        reflection.pVertexInputs = (VertexInput*)tf_malloc(sizeof(VertexInput) * reflection.mVertexInputsCount);

        for (UINT i = 0; i < shaderDesc.InputParameters; ++i)
        {
            D3D12_SIGNATURE_PARAMETER_DESC paramDesc;
            d3d12reflection->GetInputParameterDesc(i, &paramDesc);

            // Get the length of the semantic name
            bool     hasParamIndex = paramDesc.SemanticIndex > 0 || !strcmp(paramDesc.SemanticName, "TEXCOORD");
            uint32_t len = (uint32_t)strlen(paramDesc.SemanticName) + (hasParamIndex ? 1 : 0);

            if (hasParamIndex)
            {
                snprintf(pCurrentName, reflection.mNamePoolSize, "%s%u", paramDesc.SemanticName, paramDesc.SemanticIndex);
            }
            else
            {
                snprintf(pCurrentName, reflection.mNamePoolSize, "%s", paramDesc.SemanticName);
            }

            reflection.pVertexInputs[i].name = pCurrentName;
            reflection.pVertexInputs[i].name_size = len;
            reflection.pVertexInputs[i].size = (uint32_t)log2(paramDesc.Mask + 1) * sizeof(uint8_t[4]);

            // Copy over the name into the name pool
            pCurrentName += len + 1; // move the name pointer through the name pool
        }
    }

    reflection.mResourceHeapIndexing |=
        (d3d12reflection->GetRequiresFlags() & D3D_SHADER_REQUIRES_RESOURCE_DESCRIPTOR_HEAP_INDEXING) ? true : false;
    reflection.mSamplerHeapIndexing |=
        (d3d12reflection->GetRequiresFlags() & D3D_SHADER_REQUIRES_SAMPLER_DESCRIPTOR_HEAP_INDEXING) ? true : false;
}

#if defined(ENABLE_WORKGRAPH)
// template<typename RefInterface = ID3D12LibraryReflection>
static void d3d12_addShaderReflection(ID3D12LibraryReflection* d3d12LibReflection, ShaderStage shaderStage, ShaderReflection& reflection)
{
    UNREF_PARAM(shaderStage);

    ASSERT(d3d12LibReflection);

    D3D12_LIBRARY_DESC libDesc = {};
    CHECK_HRESULT(d3d12LibReflection->GetDesc(&libDesc));

    for (uint32_t f = 0; f < libDesc.FunctionCount; ++f)
    {
        ID3D12FunctionReflection* d3d12reflection = d3d12LibReflection->GetFunctionByIndex(f);

        // Get a description of this shader
        D3D12_FUNCTION_DESC shaderDesc;
        d3d12reflection->GetDesc(&shaderDesc);

        calculate_bound_resource_count(d3d12reflection, shaderDesc, reflection);
    }

    // Allocate memory for the name pool
    if (reflection.mNamePoolSize)
    {
        reflection.pNamePool = (char*)tf_calloc(reflection.mNamePoolSize, 1);
    }

    char* pCurrentName = reflection.pNamePool;
    char* nameIt = NULL;

    for (uint32_t f = 0; f < libDesc.FunctionCount; ++f)
    {
        ID3D12FunctionReflection* d3d12reflection = d3d12LibReflection->GetFunctionByIndex(f);

        // Get a description of this shader
        D3D12_FUNCTION_DESC shaderDesc;
        d3d12reflection->GetDesc(&shaderDesc);
        pCurrentName = nameIt;
        reflection.mResourceHeapIndexing |=
            (shaderDesc.RequiredFeatureFlags & D3D_SHADER_REQUIRES_RESOURCE_DESCRIPTOR_HEAP_INDEXING) ? true : false;
        reflection.mSamplerHeapIndexing |=
            (shaderDesc.RequiredFeatureFlags & D3D_SHADER_REQUIRES_SAMPLER_DESCRIPTOR_HEAP_INDEXING) ? true : false;
    }
}
#endif

void d3d12_addShaderReflection(const uint8_t* shaderCode, uint32_t shaderSize, ShaderStage shaderStage, ShaderReflection* pOutReflection)
{
    // Check to see if parameters are valid
    if (!VERIFY(shaderCode) || !VERIFY(shaderSize > 0) || !VERIFY(pOutReflection))
    {
        return;
    }

    // Run the D3D12 shader reflection on the compiled shader
    IDxcLibrary* pLibrary = NULL;
    DxcCreateInstance(CLSID_DxcLibrary, IID_PPV_ARGS(&pLibrary));
    IDxcBlobEncoding* pBlob = NULL;
    pLibrary->CreateBlobWithEncodingFromPinned((LPBYTE)shaderCode, (UINT32)shaderSize, 0, &pBlob);
#define DXIL_FOURCC(ch0, ch1, ch2, ch3) \
    ((uint32_t)(uint8_t)(ch0) | (uint32_t)(uint8_t)(ch1) << 8 | (uint32_t)(uint8_t)(ch2) << 16 | (uint32_t)(uint8_t)(ch3) << 24)

    IDxcContainerReflection* pReflection;
    UINT32                   shaderIdx;
    DxcCreateInstance(CLSID_DxcContainerReflection, IID_PPV_ARGS(&pReflection));
    pReflection->Load(pBlob);
    (pReflection->FindFirstPartKind(DXIL_FOURCC('D', 'X', 'I', 'L'), &shaderIdx));

#if defined(ENABLE_WORKGRAPH)
    if (SHADER_STAGE_WORKGRAPH == shaderStage)
    {
        ID3D12LibraryReflection* libReflection = NULL;
        CHECK_HRESULT(pReflection->GetPartReflection(shaderIdx, IID_PPV_ARGS(&libReflection)));
        d3d12_addShaderReflection(libReflection, shaderStage, *pOutReflection);
        libReflection->Release();
    }
    else
#endif
    {
        ID3D12ShaderReflection* d3d12reflection = NULL;
        CHECK_HRESULT(pReflection->GetPartReflection(shaderIdx, IID_PPV_ARGS(&d3d12reflection)));
        ASSERT(d3d12reflection);
        d3d12_addShaderReflection(d3d12reflection, shaderStage, *pOutReflection);
        d3d12reflection->Release();
    }

    pBlob->Release();
    pLibrary->Release();
    pReflection->Release();

    pOutReflection->mShaderStage = shaderStage;
}

#endif
