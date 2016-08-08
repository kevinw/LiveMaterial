// http://xboxforums.create.msdn.com/forums/t/32885.aspx
#define INITGUID // todo: what to link against to get IID_ID3D11ShaderReflection?
#include <d3d11shader.h>

#include <d3d11.h>
#include <d3dcompiler.h>
#pragma comment(lib, "d3dcompiler.lib")

#include <memory>
#include <tchar.h>

#include <vector>

std::vector<D3D11_INPUT_ELEMENT_DESC> inputLayoutArray;
ID3D11InputLayout* vertexLayout = nullptr;

HRESULT enumInputLayout(ID3D11Device * d3dDevice, ID3DBlob * VSBlob)
{
	HRESULT hr = S_OK;

	// Get description from precompiled shader
	ID3D11ShaderReflection* vertReflect = nullptr;
	D3DReflect(
		VSBlob->GetBufferPointer(),
		VSBlob->GetBufferSize(),
		IID_ID3D11ShaderReflection,
		(void**)&vertReflect
	);

	D3D11_SHADER_DESC descVertex;
	vertReflect->GetDesc(&descVertex);

	// save description of input parameters (attributes of vertex shader)
	inputLayoutArray.clear();
	uint32_t byteOffset = 0;
	D3D11_SIGNATURE_PARAMETER_DESC input_desc;
	for (size_t i = 0; i < descVertex.InputParameters; ++i) {
		// get description of input parameter
		vertReflect->GetInputParameterDesc(i, &input_desc);

		// fill element description to create input layout later
		D3D11_INPUT_ELEMENT_DESC ie;
		ie.SemanticName = input_desc.SemanticName;
		ie.SemanticIndex = input_desc.SemanticIndex;
		ie.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
		ie.InputSlot = i;
		ie.InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
		ie.InstanceDataStepRate = 0;
	ie.AlignedByteOffset = byteOffset;

	// determine correct format of input parameter and offset
	if (input_desc.Mask == 1)
	{
		if (input_desc.ComponentType == D3D_REGISTER_COMPONENT_UINT32)
		{
			ie.Format = DXGI_FORMAT_R32_UINT;
		}
		else if (input_desc.ComponentType == D3D_REGISTER_COMPONENT_SINT32)
		{
			ie.Format = DXGI_FORMAT_R32_SINT;
		}
		else if (input_desc.ComponentType == D3D_REGISTER_COMPONENT_FLOAT32)
		{
			ie.Format = DXGI_FORMAT_R32_FLOAT;
		}
		byteOffset += 4;
	}
	else if (input_desc.Mask <= 3)
	{
		if (input_desc.ComponentType == D3D_REGISTER_COMPONENT_UINT32)
		{
			ie.Format = DXGI_FORMAT_R32G32_UINT;
		}
		else if (input_desc.ComponentType == D3D_REGISTER_COMPONENT_SINT32)
		{
			ie.Format = DXGI_FORMAT_R32G32_SINT;
		}
		else if (input_desc.ComponentType == D3D_REGISTER_COMPONENT_FLOAT32)
		{
			ie.Format = DXGI_FORMAT_R32G32_FLOAT;
		}
		byteOffset += 8;
	}
	else if (input_desc.Mask <= 7)
	{
		if (input_desc.ComponentType == D3D_REGISTER_COMPONENT_UINT32)
		{
			ie.Format = DXGI_FORMAT_R32G32B32_UINT;
		}
		else if (input_desc.ComponentType == D3D_REGISTER_COMPONENT_SINT32)
		{
			ie.Format = DXGI_FORMAT_R32G32B32_SINT;
		}
		else if (input_desc.ComponentType == D3D_REGISTER_COMPONENT_FLOAT32)
		{
			ie.Format = DXGI_FORMAT_R32G32B32_FLOAT;
		}
		byteOffset += 12;
	}
	else if (input_desc.Mask <= 15)
	{
		if (input_desc.ComponentType == D3D_REGISTER_COMPONENT_UINT32)
		{
			ie.Format = DXGI_FORMAT_R32G32B32A32_UINT;
		}
		else if (input_desc.ComponentType == D3D_REGISTER_COMPONENT_SINT32)
		{
			ie.Format = DXGI_FORMAT_R32G32B32A32_SINT;
		}
		else if (input_desc.ComponentType == D3D_REGISTER_COMPONENT_FLOAT32)
		{
			ie.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
		}
		byteOffset += 16;
	}

	inputLayoutArray.push_back(ie);

	// you can save input_desc here (if needed)
	}

		// create input layout from previosly created description
	size_t numElements = inputLayoutArray.size();
	hr = d3dDevice->CreateInputLayout(
		inputLayoutArray.data(),
		numElements,
		VSBlob->GetBufferPointer(),
		VSBlob->GetBufferSize(),
		&vertexLayout
	);

	if (FAILED(hr))
	{
		// impossible to create input layout
		return hr;
	}

	return S_OK;
}


HRESULT CompileShader(_In_ const char* src, _In_ LPCSTR srcName, _In_ LPCSTR entryPoint, _In_ LPCSTR profile, const D3D_SHADER_MACRO defines[], _Outptr_ ID3DBlob** blob, _Outptr_ ID3DBlob** errorBlob)
{
	if (!src || !srcName || !entryPoint || !profile || !blob)
		return E_INVALIDARG;

	*blob = nullptr;
	*errorBlob = nullptr;

	UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;// D3DCOMPILE_ENABLE_STRICTNESS;
//#if defined( DEBUG ) || defined( _DEBUG )
//	flags |= D3DCOMPILE_DEBUG;
//#endif



	ID3DBlob* shaderBlob = nullptr;
	HRESULT hr = D3DCompile(src, strlen(src), srcName, defines, D3D_COMPILE_STANDARD_FILE_INCLUDE,
		entryPoint, profile,
		flags, 0, &shaderBlob, errorBlob);

	if (FAILED(hr)) {
		if (shaderBlob) {
			shaderBlob->Release();
		}

		return hr;
	}

	*blob = shaderBlob;
	return hr;
}