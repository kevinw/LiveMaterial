#include <d3d11.h>
#include <cassert>
#include <d3dcompiler.h>
#pragma comment(lib, "d3dcompiler.lib")
#include <memory>
#include <tchar.h>
#include <fstream>
#include <vector>
#include "StopWatch.h"

HRESULT CompileShader(
	_In_ const char* src,
	_In_ LPCSTR srcName,
	_In_ LPCSTR entryPoint,
	_In_ LPCSTR profile,
	const D3D_SHADER_MACRO defines[],
	_Outptr_ ID3DBlob** blob,
	_Outptr_ ID3DBlob** errorBlob)
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
		if (shaderBlob)
			shaderBlob->Release();
		return hr;
	}

	*blob = shaderBlob;
	return hr;
}