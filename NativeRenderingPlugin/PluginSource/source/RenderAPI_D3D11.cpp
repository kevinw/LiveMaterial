#define INITGUID

#include "RenderAPI.h"
#include "PlatformBase.h"
#include "lrucache.hpp"

// Direct3D 11 implementation of RenderAPI.

#if SUPPORT_D3D11

#define DebugHR(hr)

#include <algorithm>
#include <assert.h>
#include <d3d11.h>
#include "Unity/IUnityGraphicsD3D11.h"

#include <d3d11shader.h>
#include <d3dcompiler.h>
#include "dxerr.h"

#ifndef SAFE_RELEASE
#define SAFE_RELEASE(a) if (a) { a->Release(); a = nullptr; }
#endif

static bool DX_CHECK(HRESULT hr) {
	if (FAILED(hr)) {
		DebugHR(hr);
		return false;
	}
	else
		return true;
}


struct CompileOutput {
	ShaderType shaderType;
	string shaderBlob;
	int inputId;
	bool success;
};

class LiveMaterial_D3D11 : public LiveMaterial
{
public:
	LiveMaterial_D3D11(RenderAPI* renderAPI, int id)
		: LiveMaterial(renderAPI, id)
	{
		D3D11_DEPTH_STENCIL_DESC dsdesc;
		memset(&dsdesc, 0, sizeof(dsdesc));
		dsdesc.DepthEnable = TRUE;
		dsdesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
		dsdesc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
		device()->CreateDepthStencilState(&dsdesc, &_depthState);
	}

	virtual ~LiveMaterial_D3D11() {
		SAFE_RELEASE(_pixelShader);
		SAFE_RELEASE(_vertexShader);
		SAFE_RELEASE(_computeShader);
		SAFE_RELEASE(_deviceConstantBuffer);
		SAFE_RELEASE(_samplerState);
		SAFE_RELEASE(_depthState);
		SAFE_RELEASE(_renderTargetView);

		{ // Cleanup textures
			lock_guard<mutex> guard(texturesMutex);
			for (size_t i = 0; i < resourceViews.size(); ++i)
				SAFE_RELEASE(resourceViews[i]);
			resourceViews.clear();

			for (size_t i = 0; i < pendingResources.size(); ++i)
				SAFE_RELEASE(pendingResources[i].resource);
			pendingResources.clear();
		}

		{ // Cleanup compile outputs
			lock_guard<mutex> guard(compileOutputMutex);
			compileOutput.clear();
		}
	}

	void QueueCompileOutput(CompileOutput& output);
	virtual void SetDepthWritesEnabled(bool enabled);

	virtual bool CanDraw() const;

	virtual void Draw(int uniformIndex);
	void DrawD3D11(ID3D11DeviceContext* ctx, int uniformIndex);

	virtual bool NeedsRender();

	void updateD3D11Shader(CompileOutput output);
	void constantBufferReflect(const string& shaderBlob);

	virtual void SetRenderTexture(void* nativeTexturePtr);

protected:
	virtual void _SetTexture(const char* name, void* nativeTexturePtr);

	ID3D11Device* device() const;

	ID3D11SamplerState* samplerState() {
		if (!_samplerState) {
			D3D11_SAMPLER_DESC samplerDesc;
			memset(&samplerDesc, 0, sizeof(samplerDesc));
			samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
			samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
			samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
			samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
			DX_CHECK(device()->CreateSamplerState(&samplerDesc, &_samplerState));
		}

		return _samplerState;
	}

	void setupPendingResources(ID3D11DeviceContext* ctx);
	void updateUniforms(ID3D11DeviceContext* ctx, int uniformIndex);

	// Uniforms
	ID3D11Buffer* _deviceConstantBuffer = nullptr;
	UINT _deviceConstantBufferSize = 0;

	ID3D11PixelShader* _pixelShader = nullptr;
	ID3D11VertexShader* _vertexShader = nullptr;
	ID3D11ComputeShader* _computeShader = nullptr;
	ID3D11SamplerState* _samplerState = nullptr;
	ID3D11DepthStencilState* _depthState = nullptr;
	ID3D11RenderTargetView* _renderTargetView = nullptr;

	// Resource Views (textures)
	vector<ID3D11ShaderResourceView*> resourceViews;

	struct PendingResource {
		PendingResource(ID3D11Resource* resource_, size_t index_, string name_)
			: resource(resource_)
			, index(index_)
			, name(name_)
		{}

		ID3D11Resource* resource;
		size_t index;
		string name;
	};

	vector<PendingResource> pendingResources;
	map<string, size_t> resourceViewIndexes;

	// Compile outputs
	mutex compileOutputMutex;
	vector<CompileOutput> compileOutput;
};

static ID3D11ShaderReflection* shaderReflector(const string& shader) {
	ID3D11ShaderReflection* reflector = nullptr;

	HRESULT hr = D3DReflect(
		shader.data(),
		shader.size(),
		IID_ID3D11ShaderReflection,
		(void**)&reflector);

	if (FAILED(hr)) {
		DebugHR(hr);
		return nullptr;
	}
	
	return reflector;
}

static int roundUp(int numToRound, int multiple) {
	assert(multiple);
	int isPositive = (int)(numToRound >= 0);
	return ((numToRound + isPositive * (multiple - 1)) / multiple) * multiple;
}

bool LiveMaterial_D3D11::CanDraw() const {
	return _pixelShader && _vertexShader;
}

bool LiveMaterial_D3D11::NeedsRender() {
	lock_guard<mutex> guard(compileOutputMutex);
	for (size_t i = 0; i < compileOutput.size(); ++i)
		if (compileOutput[i].success)
			return true;
	return false;
}

void LiveMaterial_D3D11::SetDepthWritesEnabled(bool enabled) {
}

void LiveMaterial_D3D11::_SetTexture(const char* name, void* nativeTexturePtr) {
	lock_guard<mutex> guard(texturesMutex);

	auto iter = resourceViewIndexes.find(name);

	// Ignore if the texture doesn't have a slot in the shader.
	if (iter == resourceViewIndexes.end())
		return;

	// Increment the texture's reference count; we'll use it later on the render thread.
	auto index = iter->second;
	assert(index < resourceViews.size());
	auto resource = (ID3D11Resource*)nativeTexturePtr;
	resource->AddRef();

	pendingResources.push_back(PendingResource(resource, index, name));
}

void LiveMaterial_D3D11::SetRenderTexture(void* nativeTexturePtr) {
	lock_guard<mutex> guard(texturesMutex);
	auto resource = (ID3D11Resource*)nativeTexturePtr;
	if (resource) resource->AddRef();
	pendingResources.push_back(PendingResource(resource, -1, ""));
}

void LiveMaterial_D3D11::QueueCompileOutput(CompileOutput& output) {
	lock_guard<mutex> guard(compileOutputMutex);
	compileOutput.push_back(output);
}

void LiveMaterial_D3D11::constantBufferReflect(const string& shaderBlob) {
	auto pReflector = shaderReflector(shaderBlob);
	if (!pReflector)
		return;

	D3D11_SHADER_DESC desc;
	pReflector->GetDesc(&desc);

	UINT maxBind = 0;
	for (UINT i = 0; i < desc.BoundResources; ++i) {
		D3D11_SHADER_INPUT_BIND_DESC inputBindDesc;
		if (DX_CHECK(pReflector->GetResourceBindingDesc(i, &inputBindDesc))) {
			maxBind = max(maxBind, inputBindDesc.BindPoint);
		}
	}

	_stats.instructionCount = desc.InstructionCount;

	{
		lock_guard<mutex> guard(texturesMutex);
		for (size_t i = 0; i < resourceViews.size(); ++i)
			SAFE_RELEASE(resourceViews[i]);
		resourceViews.clear();
		for (UINT i = 0; i < maxBind + 1; ++i)
			resourceViews.push_back(nullptr);
		for (UINT i = 0; i < desc.BoundResources; ++i) {
			D3D11_SHADER_INPUT_BIND_DESC inputBindDesc;
			if (DX_CHECK(pReflector->GetResourceBindingDesc(i, &inputBindDesc))) {
				resourceViewIndexes[inputBindDesc.Name] = inputBindDesc.BindPoint;
			}
		}
	}

	// TODO: if we add enough uniforms, do we need to split them into multiple buffers?
	if (desc.ConstantBuffers >= 2) {
		Debug("WARNING: more than one D3D11 constant buffer, not implemented!");
		assert(false);
	}

	{
		lock_guard<mutex> uniformsGuard(uniformsMutex);
		lock_guard<mutex> gpuGuard(gpuMutex);

		SAFE_RELEASE(_deviceConstantBuffer);
		auto oldProps = shaderProps;
		shaderProps.clear();
		_deviceConstantBufferSize = 0;

		if (desc.ConstantBuffers > 0) {
			D3D11_SHADER_BUFFER_DESC Description;
			ID3D11ShaderReflectionConstantBuffer* pConstBuffer = pReflector->GetConstantBufferByIndex(0);
			pConstBuffer->GetDesc(&Description);
			for (UINT j = 0; j < Description.Variables; j++) {
				ID3D11ShaderReflectionVariable* pVariable = pConstBuffer->GetVariableByIndex(j);
				D3D11_SHADER_VARIABLE_DESC var_desc;
				pVariable->GetDesc(&var_desc);

				auto var_type = pVariable->GetType();

				D3D11_SHADER_TYPE_DESC type_desc;
				auto typeHR = var_type->GetDesc(&type_desc);
				assert(!FAILED(typeHR));
				string typeName = type_desc.Name;

				PropType propType;
				if      (typeName == "float4")   propType = PropType::Vector4;
				else if (typeName == "float3")   propType = PropType::Vector3;
				else if (typeName == "float2")   propType = PropType::Vector2;
				else if (typeName == "float")    propType = PropType::Float;
				else if (typeName == "float4x4") propType = PropType::Matrix;
				else assert(false);

				int arraySize = type_desc.Elements > 0 ? type_desc.Elements : 1;

				assert(shaderProps.find(var_desc.Name) == shaderProps.end());

				auto prop = shaderProps[var_desc.Name] = new ShaderProp(propType, var_desc.Name);
				prop->offset = var_desc.StartOffset;
				prop->size = ShaderProp::sizeForType(propType);
				prop->arraySize = arraySize;
				assert(prop->size * prop->arraySize == var_desc.Size);

				int totalSize = prop->arraySize * prop->size;
				if (arraySize > 1) {
					//DebugSS("prop " << prop->name << " has size " << prop->size << " and array size of " << prop->arraySize << " for a total of " << totalSize);
				}
				_deviceConstantBufferSize = max(_deviceConstantBufferSize, var_desc.StartOffset + totalSize);
			}

			if (_deviceConstantBufferSize > 0) {
				// secret microsoft shitiness: buffersize must be aligned to 16 bytes
				_deviceConstantBufferSize = roundUp(_deviceConstantBufferSize, 16);

				// constant buffer
				D3D11_BUFFER_DESC bufdesc;
				memset(&bufdesc, 0, sizeof(bufdesc));
				bufdesc.Usage = D3D11_USAGE_DEFAULT;
				bufdesc.ByteWidth = _deviceConstantBufferSize;
				bufdesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
				bufdesc.CPUAccessFlags = 0;//D3D11_CPU_ACCESS_WRITE;

				HRESULT hr = device()->CreateBuffer(&bufdesc, NULL, &_deviceConstantBuffer);
				if (FAILED(hr)) {
					Debug("ERROR: could not create constant buffer:");
					DebugHR(hr);
				}
			}
		}

		ensureConstantBufferSize(_deviceConstantBufferSize, &oldProps, &shaderProps);
	}

	pReflector->Release();
}

void LiveMaterial_D3D11::updateD3D11Shader(CompileOutput output)
{
	if (!output.success) {
		_stats.compileState = CompileState::Error;
		assert(output.shaderBlob.empty());
		return;
	}

	_stats.compileState = CompileState::Success;

	assert(!output.shaderBlob.empty());
	if (output.shaderType == Fragment || output.shaderType == Compute)
		constantBufferReflect(output.shaderBlob);

	auto buf = output.shaderBlob.data();
	auto bufSize = output.shaderBlob.size();
	assert(buf && bufSize > 0);

	switch (output.shaderType) {
	case Fragment: {
		ID3D11PixelShader* newPixelShader = nullptr;
		HRESULT hr = device()->CreatePixelShader(buf, bufSize, nullptr, &newPixelShader);
		if (FAILED(hr)) {
			Debug("CreatePixelShader failed\n"); DebugHR(hr);
		} else {
			SAFE_RELEASE(_pixelShader);
			_pixelShader = newPixelShader;
		}
		break;
	}
	case Vertex: {
		ID3D11VertexShader* newVertexShader = nullptr;
		HRESULT hr = device()->CreateVertexShader(buf, bufSize, nullptr, &newVertexShader);
		if (FAILED(hr)) {
			DebugHR(hr);
			DebugSS("CreateVertexShader failed:" << 
				"\n\n inputId: " << output.inputId <<
				"\n\n shaderType: " << shaderTypeName(output.shaderType));
		} else {
			SAFE_RELEASE(_vertexShader);
			_vertexShader = newVertexShader;
		}
		break;
	}
	case Compute: {
		ID3D11ComputeShader* newShader = nullptr;
		HRESULT hr = device()->CreateComputeShader(buf, bufSize, nullptr, &newShader);
		if (FAILED(hr)) {
			Debug("CreateComputeShader failed"); DebugHR(hr);
		} else {
			SAFE_RELEASE(_computeShader);
			_computeShader = newShader;
		}
		break;
	}
	default:
		assert(false);
	}
}


class RenderAPI_D3D11 : public RenderAPI
{
public:
	RenderAPI_D3D11();
	virtual ~RenderAPI_D3D11() {}

	virtual void ProcessDeviceEvent(UnityGfxDeviceEventType type, IUnityInterfaces* interfaces);

	virtual void DrawSimpleTriangles(const float worldMatrix[16], int triangleCount, const void* verticesFloat3Byte4);

	virtual void* BeginModifyTexture(void* textureHandle, int textureWidth, int textureHeight, int* outRowPitch);
	virtual void EndModifyTexture(void* textureHandle, int textureWidth, int textureHeight, int rowPitch, void* dataPtr);

	virtual LiveMaterial* _newLiveMaterial(int id);
	virtual bool compileShader(CompileTask task);

	virtual void ClearCompileCache();

	ID3D11Device* D3D11Device() const { return m_Device; }

private:
	void CreateResources();
	void ReleaseResources();

	ID3D11Device* m_Device;
	ID3D11Buffer* m_VB; // vertex buffer
	ID3D11Buffer* m_CB; // constant buffer
	ID3D11VertexShader* m_VertexShader;
	ID3D11PixelShader* m_PixelShader;
	ID3D11InputLayout* m_InputLayout;
	ID3D11RasterizerState* m_RasterState;
	ID3D11BlendState* m_BlendState;
	ID3D11DepthStencilState* m_DepthState;
};


LiveMaterial* RenderAPI_D3D11::_newLiveMaterial(int id) { return new LiveMaterial_D3D11(this, id); }

static const char* profileNameForShaderType(ShaderType shaderType) {
	switch (shaderType) {
	case Fragment: return "ps_5_0";
	case Vertex: return "vs_5_0";
	case Compute: return "cs_5_0";
	default:
		assert(false);
		return nullptr;
	}
}

static cache::lru_cache<size_t, string> cachedShaderBlobs(20);

static bool getCachedOutput(const CompileTask& task, CompileOutput& output) {
	auto hashValue = task.hash();
	auto iter = cachedShaderBlobs.find(hashValue);
	if (iter != cachedShaderBlobs.end()) {
		output.success = true;
		output.shaderBlob = iter->second->second;
		return true;
	}
	return false;
}

static void cacheOutput(const CompileTask& task, const CompileOutput& output) {
	assert(!output.shaderBlob.empty());
	auto hashValue = task.hash();
	cachedShaderBlobs.put(hashValue, output.shaderBlob);
}

void RenderAPI_D3D11::ClearCompileCache() {
	// TODO: not threadsafe; should only be used in debugging
	cachedShaderBlobs.clear();
}

static int compileCount = 0;

bool RenderAPI_D3D11::compileShader(CompileTask task)
{
	CompileOutput output;
	output.shaderType = task.shaderType;
	output.inputId = task.id;
	output.shaderBlob = "";
	output.success = false;

	if (!getCachedOutput(task, output)) {
		const D3D_SHADER_MACRO defines[] = { NULL, NULL };
		UINT flags = D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY;
		/*if (optimizationLevel == 0) flags |= D3DCOMPILE_OPTIMIZATION_LEVEL0;
		if (optimizationLevel == 1) flags |= D3DCOMPILE_OPTIMIZATION_LEVEL1;
		if (optimizationLevel == 2) flags |= D3DCOMPILE_OPTIMIZATION_LEVEL2;
		if (optimizationLevel == 3) flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
		else DebugSS("Unknown optimization level " << optimizationLevel);
		*/
		flags |= D3DCOMPILE_OPTIMIZATION_LEVEL0;
		//if (shaderDebugging) {
			flags |= D3DCOMPILE_DEBUG;
		//}
		auto profile = profileNameForShaderType(task.shaderType);
		if (!profile) {
			Debug("no profile found for shader type");
		}
		else if (task.src.empty() || task.filename.empty() || task.entryPoint.empty()) {
			Debug("empty src or srcName or entryPoint");
		}
		else {
			ID3DBlob *shaderBlob = nullptr;
			ID3DBlob* errorBlob = nullptr;

			int currentCount = ++compileCount;

			DebugSS("Starting compile " << currentCount);

			HRESULT hr = D3DCompile(
				task.src.data(), task.src.size(), task.filename.c_str(), defines, D3D_COMPILE_STANDARD_FILE_INCLUDE,
				task.entryPoint.c_str(), profile, flags, 0, &shaderBlob, &errorBlob);

			DebugSS("..finished compile " << currentCount);

			string errstr;
			if (errorBlob)
				errstr = string((const char*)errorBlob->GetBufferPointer(), errorBlob->GetBufferSize());

			//const char* inputFilename = "c:\\Users\\kevin\\Desktop\\input.hlsl";
			//writeTextToFile(inputFilename, task.src.c_str());

			if (FAILED(hr)) {
				DebugSS("Could not compile shader: " << errstr);
				//DebugSS("file is at " << inputFilename);
			}
			else {
				if (!errstr.empty() && showWarnings())
					DebugSS(errstr);
				output.shaderBlob = string((const char*)shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize());
				output.success = true;
				cacheOutput(task, output);
			}
			SAFE_RELEASE(shaderBlob);
			SAFE_RELEASE(errorBlob);
		}
	}

	{
		lock_guard<mutex> renderAPIGuard(renderAPIMutex);
		if (GetCurrentRenderAPI()) {
			lock_guard<mutex> guard(materialsMutex);
			auto liveMaterial = (LiveMaterial_D3D11*)GetLiveMaterialByIdLocked(task.liveMaterialId);
			if (liveMaterial)
				liveMaterial->QueueCompileOutput(output);
		}
	}

	return output.success;
}

RenderAPI* CreateRenderAPI_D3D11() { return new RenderAPI_D3D11(); }


// Simple compiled shader bytecode.
//
// Shader source that was used:
#if 0
cbuffer MyCB : register(b0)
{
	float4x4 worldMatrix;
}
void VS(float3 pos : POSITION, float4 color : COLOR, out float4 ocolor : COLOR, out float4 opos : SV_Position)
{
	opos = mul(worldMatrix, float4(pos, 1));
	ocolor = color;
}
float4 PS(float4 color : COLOR) : SV_TARGET
{
	return color;
}
#endif // #if 0
//
// Which then was compiled with:
// fxc /Tvs_4_0_level_9_3 /EVS source.hlsl /Fh outVS.h /Qstrip_reflect /Qstrip_debug /Qstrip_priv
// fxc /Tps_4_0_level_9_3 /EPS source.hlsl /Fh outPS.h /Qstrip_reflect /Qstrip_debug /Qstrip_priv
// and results pasted & formatted to take less lines here
const BYTE kVertexShaderCode[] =
{
	68,88,66,67,86,189,21,50,166,106,171,1,10,62,115,48,224,137,163,129,1,0,0,0,168,2,0,0,4,0,0,0,48,0,0,0,0,1,0,0,4,2,0,0,84,2,0,0,
	65,111,110,57,200,0,0,0,200,0,0,0,0,2,254,255,148,0,0,0,52,0,0,0,1,0,36,0,0,0,48,0,0,0,48,0,0,0,36,0,1,0,48,0,0,0,0,0,
	4,0,1,0,0,0,0,0,0,0,0,0,1,2,254,255,31,0,0,2,5,0,0,128,0,0,15,144,31,0,0,2,5,0,1,128,1,0,15,144,5,0,0,3,0,0,15,128,
	0,0,85,144,2,0,228,160,4,0,0,4,0,0,15,128,1,0,228,160,0,0,0,144,0,0,228,128,4,0,0,4,0,0,15,128,3,0,228,160,0,0,170,144,0,0,228,128,
	2,0,0,3,0,0,15,128,0,0,228,128,4,0,228,160,4,0,0,4,0,0,3,192,0,0,255,128,0,0,228,160,0,0,228,128,1,0,0,2,0,0,12,192,0,0,228,128,
	1,0,0,2,0,0,15,224,1,0,228,144,255,255,0,0,83,72,68,82,252,0,0,0,64,0,1,0,63,0,0,0,89,0,0,4,70,142,32,0,0,0,0,0,4,0,0,0,
	95,0,0,3,114,16,16,0,0,0,0,0,95,0,0,3,242,16,16,0,1,0,0,0,101,0,0,3,242,32,16,0,0,0,0,0,103,0,0,4,242,32,16,0,1,0,0,0,
	1,0,0,0,104,0,0,2,1,0,0,0,54,0,0,5,242,32,16,0,0,0,0,0,70,30,16,0,1,0,0,0,56,0,0,8,242,0,16,0,0,0,0,0,86,21,16,0,
	0,0,0,0,70,142,32,0,0,0,0,0,1,0,0,0,50,0,0,10,242,0,16,0,0,0,0,0,70,142,32,0,0,0,0,0,0,0,0,0,6,16,16,0,0,0,0,0,
	70,14,16,0,0,0,0,0,50,0,0,10,242,0,16,0,0,0,0,0,70,142,32,0,0,0,0,0,2,0,0,0,166,26,16,0,0,0,0,0,70,14,16,0,0,0,0,0,
	0,0,0,8,242,32,16,0,1,0,0,0,70,14,16,0,0,0,0,0,70,142,32,0,0,0,0,0,3,0,0,0,62,0,0,1,73,83,71,78,72,0,0,0,2,0,0,0,
	8,0,0,0,56,0,0,0,0,0,0,0,0,0,0,0,3,0,0,0,0,0,0,0,7,7,0,0,65,0,0,0,0,0,0,0,0,0,0,0,3,0,0,0,1,0,0,0,
	15,15,0,0,80,79,83,73,84,73,79,78,0,67,79,76,79,82,0,171,79,83,71,78,76,0,0,0,2,0,0,0,8,0,0,0,56,0,0,0,0,0,0,0,0,0,0,0,
	3,0,0,0,0,0,0,0,15,0,0,0,62,0,0,0,0,0,0,0,1,0,0,0,3,0,0,0,1,0,0,0,15,0,0,0,67,79,76,79,82,0,83,86,95,80,111,115,
	105,116,105,111,110,0,171,171
};
const BYTE kPixelShaderCode[]=
{
	68,88,66,67,196,65,213,199,14,78,29,150,87,236,231,156,203,125,244,112,1,0,0,0,32,1,0,0,4,0,0,0,48,0,0,0,124,0,0,0,188,0,0,0,236,0,0,0,
	65,111,110,57,68,0,0,0,68,0,0,0,0,2,255,255,32,0,0,0,36,0,0,0,0,0,36,0,0,0,36,0,0,0,36,0,0,0,36,0,0,0,36,0,1,2,255,255,
	31,0,0,2,0,0,0,128,0,0,15,176,1,0,0,2,0,8,15,128,0,0,228,176,255,255,0,0,83,72,68,82,56,0,0,0,64,0,0,0,14,0,0,0,98,16,0,3,
	242,16,16,0,0,0,0,0,101,0,0,3,242,32,16,0,0,0,0,0,54,0,0,5,242,32,16,0,0,0,0,0,70,30,16,0,0,0,0,0,62,0,0,1,73,83,71,78,
	40,0,0,0,1,0,0,0,8,0,0,0,32,0,0,0,0,0,0,0,0,0,0,0,3,0,0,0,0,0,0,0,15,15,0,0,67,79,76,79,82,0,171,171,79,83,71,78,
	44,0,0,0,1,0,0,0,8,0,0,0,32,0,0,0,0,0,0,0,0,0,0,0,3,0,0,0,0,0,0,0,15,0,0,0,83,86,95,84,65,82,71,69,84,0,171,171
};


RenderAPI_D3D11::RenderAPI_D3D11()
	: m_Device(NULL)
	, m_VB(NULL)
	, m_CB(NULL)
	, m_VertexShader(NULL)
	, m_PixelShader(NULL)
	, m_InputLayout(NULL)
	, m_RasterState(NULL)
	, m_BlendState(NULL)
	, m_DepthState(NULL)
{
}


void RenderAPI_D3D11::ProcessDeviceEvent(UnityGfxDeviceEventType type, IUnityInterfaces* interfaces)
{
	switch (type)
	{
	case kUnityGfxDeviceEventInitialize:
	{
		IUnityGraphicsD3D11* d3d = interfaces->Get<IUnityGraphicsD3D11>();
		m_Device = d3d->GetDevice();
		CreateResources();
		break;
	}
	case kUnityGfxDeviceEventShutdown:
		ReleaseResources();
		break;
	}
}


void RenderAPI_D3D11::CreateResources()
{
	D3D11_BUFFER_DESC desc;
	memset(&desc, 0, sizeof(desc));

	// vertex buffer
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.ByteWidth = 1024;
	desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
	m_Device->CreateBuffer(&desc, NULL, &m_VB);

	// constant buffer
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.ByteWidth = 64; // hold 1 matrix
	desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	desc.CPUAccessFlags = 0;
	m_Device->CreateBuffer(&desc, NULL, &m_CB);

	// shaders
	HRESULT hr;
	hr = m_Device->CreateVertexShader(kVertexShaderCode, sizeof(kVertexShaderCode), nullptr, &m_VertexShader);
	if (FAILED(hr))
		OutputDebugStringA("Failed to create vertex shader.\n");
	hr = m_Device->CreatePixelShader(kPixelShaderCode, sizeof(kPixelShaderCode), nullptr, &m_PixelShader);
	if (FAILED(hr))
		OutputDebugStringA("Failed to create pixel shader.\n");

	// input layout
	if (m_VertexShader)
	{
		D3D11_INPUT_ELEMENT_DESC s_DX11InputElementDesc[] =
		{
			{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		};
		m_Device->CreateInputLayout(s_DX11InputElementDesc, 2, kVertexShaderCode, sizeof(kVertexShaderCode), &m_InputLayout);
	}

	// render states
	D3D11_RASTERIZER_DESC rsdesc;
	memset(&rsdesc, 0, sizeof(rsdesc));
	rsdesc.FillMode = D3D11_FILL_SOLID;
	rsdesc.CullMode = D3D11_CULL_NONE;
	rsdesc.DepthClipEnable = TRUE;
	m_Device->CreateRasterizerState(&rsdesc, &m_RasterState);

	D3D11_DEPTH_STENCIL_DESC dsdesc;
	memset(&dsdesc, 0, sizeof(dsdesc));
	dsdesc.DepthEnable = TRUE;
	dsdesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
	dsdesc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
	m_Device->CreateDepthStencilState(&dsdesc, &m_DepthState);

	D3D11_BLEND_DESC bdesc;
	memset(&bdesc, 0, sizeof(bdesc));
	bdesc.RenderTarget[0].BlendEnable = FALSE;
	bdesc.RenderTarget[0].RenderTargetWriteMask = 0xF;
	m_Device->CreateBlendState(&bdesc, &m_BlendState);

}


void RenderAPI_D3D11::ReleaseResources() {
	SAFE_RELEASE(m_VB);
	SAFE_RELEASE(m_CB);
	SAFE_RELEASE(m_VertexShader);
	SAFE_RELEASE(m_PixelShader);
	SAFE_RELEASE(m_InputLayout);
	SAFE_RELEASE(m_RasterState);
	SAFE_RELEASE(m_BlendState);
	SAFE_RELEASE(m_DepthState);
}


void RenderAPI_D3D11::DrawSimpleTriangles(const float worldMatrix[16], int triangleCount, const void* verticesFloat3Byte4) {
	ID3D11DeviceContext* ctx = NULL;
	m_Device->GetImmediateContext(&ctx);

	// Set basic render state
	ctx->OMSetDepthStencilState(m_DepthState, 0);
	ctx->RSSetState(m_RasterState);
	ctx->OMSetBlendState(m_BlendState, NULL, 0xFFFFFFFF);

	// Update constant buffer - just the world matrix in our case
	ctx->UpdateSubresource(m_CB, 0, NULL, worldMatrix, 64, 0);

	// Set shaders
	ctx->VSSetConstantBuffers(0, 1, &m_CB);
	ctx->VSSetShader(m_VertexShader, NULL, 0);
	ctx->PSSetShader(m_PixelShader, NULL, 0);

	// Update vertex buffer
	const int kVertexSize = 12 + 4;
	ctx->UpdateSubresource(m_VB, 0, NULL, verticesFloat3Byte4, triangleCount * 3 * kVertexSize, 0);

	// set input assembler data and draw
	ctx->IASetInputLayout(m_InputLayout);
	ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	UINT stride = kVertexSize;
	UINT offset = 0;
	ctx->IASetVertexBuffers(0, 1, &m_VB, &stride, &offset);
	ctx->Draw(triangleCount * 3, 0);

	ctx->Release();
}


void* RenderAPI_D3D11::BeginModifyTexture(void* textureHandle, int textureWidth, int textureHeight, int* outRowPitch)
{
	const int rowPitch = textureWidth * 4;
	// Just allocate a system memory buffer here for simplicity
	unsigned char* data = new unsigned char[rowPitch * textureHeight];
	*outRowPitch = rowPitch;
	return data;
}


void RenderAPI_D3D11::EndModifyTexture(void* textureHandle, int textureWidth, int textureHeight, int rowPitch, void* dataPtr)
{
	ID3D11Texture2D* d3dtex = (ID3D11Texture2D*)textureHandle;
	assert(d3dtex);

	ID3D11DeviceContext* ctx = NULL;
	m_Device->GetImmediateContext(&ctx);
	if (ctx) {
		// Update texture data, and free the memory buffer
		ctx->UpdateSubresource(d3dtex, 0, NULL, dataPtr, rowPitch, 0);
		delete[](unsigned char*)dataPtr;
		ctx->Release();
	}
}

void LiveMaterial_D3D11::Draw(int uniformIndex) {
	ID3D11DeviceContext* ctx = nullptr;
	device()->GetImmediateContext(&ctx);
	if (ctx) {
		DrawD3D11(ctx, uniformIndex);
		ctx->Release();
	}
}


ID3D11Device* LiveMaterial_D3D11::device() const {
	return ((RenderAPI_D3D11*)_renderAPI)->D3D11Device();
}

void LiveMaterial_D3D11::setupPendingResources(ID3D11DeviceContext* ctx) {

	for (size_t i = 0; i < pendingResources.size(); ++i) {
		auto resource = pendingResources[i].resource;
		auto index = pendingResources[i].index;
		auto name = pendingResources[i].name;

		if (index == -1) {
			assert(name == "");
			if (resource == nullptr) {
				SAFE_RELEASE(_renderTargetView);
			} else {
				ID3D11RenderTargetView* renderTargetView = nullptr;

				D3D11_RESOURCE_DIMENSION resourceDimension;
				resource->GetType(&resourceDimension);
				switch (resourceDimension) {
				case D3D10_RESOURCE_DIMENSION_UNKNOWN: { Debug("unknown"); break; }
				case D3D11_RESOURCE_DIMENSION_BUFFER: { Debug("Resource is a buffer."); break; }
				case D3D11_RESOURCE_DIMENSION_TEXTURE1D: { Debug("Resource is a 1D texture."); break; }
				case D3D11_RESOURCE_DIMENSION_TEXTURE2D: { Debug("Resource is a 2D texture."); break; }
				case D3D11_RESOURCE_DIMENSION_TEXTURE3D: { Debug("Resource is a 3D texture."); break;  }
				default: assert(false);
				}

				ID3D11Texture2D* tex2d = nullptr;
				if (!FAILED(resource->QueryInterface(IID_ID3D11Texture2D, (void**)&tex2d))) {
					assert(tex2d);
					D3D11_TEXTURE2D_DESC textureDesc;
					tex2d->GetDesc(&textureDesc);

					DebugSS("render texture size: " << textureDesc.Width << "x" << textureDesc.Height);

					D3D11_RENDER_TARGET_VIEW_DESC renderTargetViewDesc;
					renderTargetViewDesc.Format = textureDesc.Format;
					renderTargetViewDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
					renderTargetViewDesc.Texture2D.MipSlice = 0;

					if (FAILED(device()->CreateRenderTargetView(tex2d, &renderTargetViewDesc, &_renderTargetView))) {
						Debug("failed creating render target view");
					}
				}
			}
		}
		else {
			auto resourceView = resourceViews[index];

			if (resourceView != nullptr) {
				SAFE_RELEASE(resourceView);
				resourceView = nullptr;
			}

			if (resource) {
				{
					HRESULT hr = device()->CreateShaderResourceView(resource, nullptr, &resourceView);
					if (FAILED(hr)) {
						Debug("Could not CreateShaderResourceView");
						DebugHR(hr);
						resourceView = nullptr;
					}
				}
			}

			resourceViews[index] = resourceView;
		}

		SAFE_RELEASE(resource);
	}

	pendingResources.clear();

	ctx->PSSetShaderResources(0, (UINT)resourceViews.size(), &resourceViews[0]);

	// TODO: is the number of samplers equal to the number of resource views? no idea
	vector<ID3D11SamplerState*> samplers;
	auto sampler = samplerState();
	assert(sampler);
	const UINT numSamplers = (UINT)resourceViews.size();
	for (UINT i = 0; i < numSamplers; ++i)
		samplers.push_back(sampler);
	ctx->PSSetSamplers(0, numSamplers, &samplers[0]);
}

void LiveMaterial_D3D11::updateUniforms(ID3D11DeviceContext* ctx, int uniformIndex) {

	assert(uniformIndex < MAX_GPU_BUFFERS);
	if (_deviceConstantBuffer && _deviceConstantBufferSize > 0) {
		ctx->UpdateSubresource(_deviceConstantBuffer, 0, 0, _gpuBuffer + _constantBufferSize * uniformIndex, 0, 0);
	}
}

void LiveMaterial_D3D11::DrawD3D11(ID3D11DeviceContext* ctx, int uniformIndex) {
	vector<CompileOutput> outputs;

	if (_depthState)
		ctx->OMSetDepthStencilState(_depthState, 0);

	{
		lock_guard<mutex> guard(compileOutputMutex);
		outputs = compileOutput;
		compileOutput.clear();
	}

	for (size_t i = 0; i < outputs.size(); ++i)
		updateD3D11Shader(outputs[i]);
	
	if (_drawingEnabled && _pixelShader && _vertexShader) {
		{
			lock_guard<mutex> guard(texturesMutex);
			setupPendingResources(ctx);
		}

		{
			lock_guard<mutex> uniformsGuard(uniformsMutex);
			lock_guard<mutex> gpuGuard(gpuMutex);
			updateUniforms(ctx, uniformIndex);

		}

		ctx->VSSetShader (_vertexShader, NULL, 0);		
		ctx->PSSetShader (_pixelShader, NULL, 0);
		ctx->PSSetConstantBuffers(0, 1, &_deviceConstantBuffer);
		ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

		if (_renderTargetView) {
			ID3D11RenderTargetView* oldRenderTargetView = nullptr;
			ID3D11DepthStencilView* oldDepthStencilView = nullptr;
			ctx->OMGetRenderTargets(1, &oldRenderTargetView, &oldDepthStencilView);

			DebugSS("old render target " << oldRenderTargetView);

			ctx->OMSetRenderTargets(1, &_renderTargetView, oldDepthStencilView);
			ctx->Draw(4, 0);

			ctx->OMSetRenderTargets(1, &oldRenderTargetView, oldDepthStencilView);
		}

		ctx->Draw(4, 0);
	}
	else {
		//DebugSS("Can't draw, no pixel or vertex shader in (id=" << id() << ", ptr=" << this << ")");
	}
}



#endif // #if SUPPORT_D3D11