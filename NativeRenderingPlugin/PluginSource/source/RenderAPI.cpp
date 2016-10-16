#include "RenderAPI.h"
#include "PlatformBase.h"
#include "Unity/IUnityGraphics.h"

#include <assert.h>
#include <math.h>

using std::endl;

void writeTextToFile(const char* filename, const char* text) {
    std::ofstream debugOut;
    debugOut.open(filename);
    debugOut << text;
    debugOut.close();
}



const char* shaderTypeName(ShaderType shaderType) {
	switch (shaderType) {
	case Vertex: return "Vertex";
	case Fragment: return "Fragment";
	case Compute: return "Compute";
	default: assert(false);
	}
	return nullptr;
}

LiveMaterial::LiveMaterial(RenderAPI* renderAPI, int id)
	: _renderAPI(renderAPI)
	, _id(id)
{}

void LiveMaterial::SubmitUniforms(int uniformIndex) {
	lock_guard<mutex> uniformsGuard(uniformsMutex);
	lock_guard<mutex> gpuGuard(gpuMutex);
	assert(uniformIndex < MAX_GPU_BUFFERS);
	if (_gpuBuffer && _constantBuffer) {
		unsigned char* dest = _gpuBuffer + _constantBufferSize * uniformIndex;
#if false
		for (auto iter = shaderProps.begin(); iter != shaderProps.end(); ++iter) {
			auto name = iter->first;
			auto prop = iter->second;
			if (0 != memcmp(dest + prop->offset, _constantBuffer + prop->offset, prop->arraySize * prop->size))
				DebugSS("Changed prop " << name);

		}
#endif
		memcpy(dest, _constantBuffer, _constantBufferSize);
	}
}

void LiveMaterial::setproparray(const char* name, PropType type, float* value, int numElems) {
    lock_guard<mutex> guard(uniformsMutex);

    if (!_constantBuffer || numElems < 1) return;

    auto prop = propForName(name, type);
    if (!prop) return;
    
	size_t bytesToCopy = prop->size * (int)fmin(numElems, prop->arraySize);
	if (numElems > 1) {
		//DebugSS("setproparray(" << name << ", " << propTypeStrings[type] << ", " << value << ", " << numElems << ") is copying " << bytesToCopy << " bytes.");
	}
	memcpy(_constantBuffer + prop->offset, value, bytesToCopy);
}

void LiveMaterial::getproparray(const char* name, PropType type, float* value, int numElems) {
	lock_guard<mutex> guard(uniformsMutex);
	getproparray_locked(name, type, value, numElems);
}

void LiveMaterial::getproparray_locked(const char* name, PropType type, float* value, int numElems) {
    if (!_constantBuffer || numElems < 1) return;

    auto prop = propForName(name, type);
    if (!prop) return;
    
	size_t bytesToCopy = prop->size * (int)fmin(numElems, prop->arraySize);
	memcpy(value, _constantBuffer + prop->offset, bytesToCopy);
}

void LiveMaterial::Draw(int uniformIndex) { }

bool LiveMaterial::NeedsRender()
{
	return false;
}

void LiveMaterial::SetFloat(const char * name, float value) { setproparray(name, PropType::Float, &value, 1); }
void LiveMaterial::SetVector4(const char * name, float* value) { setproparray(name, PropType::Vector4, value, 1); }
void LiveMaterial::SetMatrix(const char * name, float * value) { setproparray(name, PropType::Matrix, value, 1); }

void LiveMaterial::SetFloatArray(const char * name, float * value, int numFloats) {
	setproparray(name, PropType::FloatBlock, value, numFloats);
}

void LiveMaterial::SetVectorArray(const char* name, float* values, int numVector4s) {
	setproparray(name, PropType::Vector4, values, numVector4s);
}

void LiveMaterial::SetMatrixArray(const char* name, float* values, int numMatrices) {
	setproparray(name, PropType::Matrix, values, numMatrices);
}

bool LiveMaterial::SetTextureID(const char * name, int id)
{
	if (!id) {
		_SetTexture(name, nullptr);
		return false;
	}
	
	void* nativeTexturePointer = nullptr;
	{
		lock_guard<mutex> guard(texturesMutex);

		auto iter = texturePointers.find(id);
		if (iter == texturePointers.end())
			return true; // needs pointer. caller will do expensive GetNativeTexturePtr

		nativeTexturePointer = iter->second;
		assert(nativeTexturePointer);
	}

	_SetTexture(name, nativeTexturePointer);
	return false;
}

void LiveMaterial::SetTexturePtr(const char * name, int id, void * nativeTexturePointer)
{
	assert(id);
	assert(texturePointers.find(id) == texturePointers.end());
	texturePointers[id] = nativeTexturePointer;
	bool needsSet = SetTextureID(name, id);
	assert(!needsSet);
}

void LiveMaterial::_SetTexture(const char* name, void* nativeTexturePointer) {
	assert(false);
}

void LiveMaterial::GetFloat(const char * name, float* value) { getproparray(name, PropType::Float, value, 1); }
void LiveMaterial::GetVector4(const char* name, float* value) { getproparray(name, PropType::Vector4, value, 1); }
void LiveMaterial::GetMatrix(const char* name, float* value) { getproparray(name, PropType::Vector4, value, 1); }

static ShaderProp* _lookupPropByName(const PropMap& props, const char* name) {
	auto i = props.find(name);
	return i != props.end() ? i->second : nullptr;
}

void LiveMaterial::SetDepthWritesEnabled(bool enabled) {
}

bool LiveMaterial::HasProperty(const char* name) {
	lock_guard<mutex> guard(uniformsMutex);
	return _lookupPropByName(shaderProps, name) != nullptr;
}

void LiveMaterial::DumpUniformsToFile(const char* filename, bool flatten) {
	lock_guard<mutex> guard(uniformsMutex);
    std::ofstream js(filename);
    js << "{" << endl;
    for (auto i = shaderProps.begin(); i != shaderProps.end(); ++i) {
        auto prop = i->second;
		js << "    \"" << prop->name << "\": ";

		float numFloats = (float)prop->size / (float)sizeof(float);
		assert(abs(numFloats - (int)numFloats) < 0.001); // for now assume all things are floats

		if (flatten) {
			if (prop->arraySize > 1 || numFloats > 1)
				js << "[";

			bool first = true;
			for (int a = 0; a < prop->arraySize; ++a) {
				for (int f = 0; f < (int)numFloats; ++f) {
					if (first) first = false;
					else js << ", ";
					js << *(float*)(_constantBuffer + prop->offset + f * sizeof(float) + a * prop->size);
				}
			}
			if (prop->arraySize > 1 || numFloats > 1)
				js << "]";
		} else {
			for (int a = 0; a < prop->arraySize; ++a) {
				if (a == 0 && prop->arraySize >= 2) js << "[";

				for (int f = 0; f < (int)numFloats; ++f) {
					if (f == 0 && numFloats > 1) js << "[";
					js << *(float*)(_constantBuffer + prop->offset + f * sizeof(float) + a * prop->size);
					if (f != numFloats - 1) js << ", ";
					if (f == ((int)numFloats) - 1 && numFloats > 1) js << "]";
				}

				if (prop->arraySize >= 2) js << (a < prop->arraySize - 1) ? ", " : "]";
			}
		}

        auto nexti = i;
        nexti++;
        if (nexti != shaderProps.end())
          js << ", ";

        js << "\n";
    }

    js << "}" << endl;
}

ShaderProp * LiveMaterial::propForNameSizeOffset(const char * name, uint16_t size, uint16_t offset) {
	auto prop = _lookupPropByName(shaderProps, name);
	if (!prop || prop->size != size || prop->offset != offset) {
		if (prop)
			DebugSS("WARNING: deleting prop named " << prop->name);
		SAFE_DELETE(prop);
		auto type = ShaderProp::typeForSize(size);
		int arraySize = 1;
		if (type == FloatBlock) {
			arraySize = size / sizeof(float);
			size = sizeof(float);
			type = PropType::Float;
		}
		prop = shaderProps[name] = new ShaderProp(type, name);
		prop->size = size;
		prop->arraySize = arraySize;
		prop->offset = offset;
	}
	else {
		assert(prop->size == size);
		assert(prop->offset == offset);
	}

	return prop;
}

ShaderProp * LiveMaterial::propForName(const char * name, PropType type)
{
	auto prop = _lookupPropByName(shaderProps, name);
	if (!prop || (prop->type != type && type != PropType::FloatBlock)) {
		if (prop)
			DebugSS("WARNING: deleting prop named " << prop->name);
		SAFE_DELETE(prop);
		prop = shaderProps[name] = new ShaderProp(type, name);
	}
	//assert(prop->type == type);
	return prop;
}

static void copyProps(PropMap* oldProps, PropMap* newProps, unsigned char* oldBuffer, unsigned char* newBuffer) {
	for (auto i = oldProps->begin(); i != oldProps->end(); ++i) {
		auto oldProp = i->second;
		auto newPropI = newProps->find(oldProp->name);
		if (newPropI == newProps->end())
			continue;
		auto newProp = newPropI->second;
		if (newProp->type != oldProp->type ||
			newProp->arraySize != oldProp->arraySize ||
			newProp->size != oldProp->size)
			continue;

		size_t bytesToCopy = newProp->size * newProp->arraySize;
		memcpy(newBuffer + newProp->offset, oldBuffer + oldProp->offset, bytesToCopy);
	}	
}


void LiveMaterial::ensureConstantBufferSize(size_t size, PropMap* oldProps, PropMap* newProps) {
	// must have GUARD_UNIFORMS and GUARD_GPU

	auto oldConstantBuffer = _constantBuffer;
	auto oldGpuBuffer = _gpuBuffer;
	auto oldConstantBufferSize = _constantBufferSize;

	_constantBuffer = new unsigned char[size];
	_gpuBuffer = new unsigned char[size * MAX_GPU_BUFFERS];
	_constantBufferSize = size;

    memset(_constantBuffer, 0, size);
    memset(_gpuBuffer, 0, size * MAX_GPU_BUFFERS);		

	// If we have references to the old props, we can copy the values over to keep rendering
	// relatively smooth.
	if (oldProps && newProps) {
		copyProps(oldProps, newProps, oldConstantBuffer, _constantBuffer);
		for (int i = 0; i < MAX_GPU_BUFFERS; ++i)
			copyProps(oldProps, newProps, oldGpuBuffer + oldConstantBufferSize * i, _gpuBuffer + _constantBufferSize * i);
	}

	if (oldConstantBuffer) delete[] oldConstantBuffer;
	if (oldGpuBuffer) delete[] oldGpuBuffer;
}

LiveMaterial* RenderAPI::CreateLiveMaterial() {
	lock_guard<mutex> guard(materialsMutex);
	++liveMaterialCount;

	// Wrap around at 16 bits; the C# side packs the LiveMaterial's id
	// into half an int when rendering (the other half is the uniform index).
	if (liveMaterialCount > std::numeric_limits<int16_t>::max())
		liveMaterialCount = 0;

	auto id = liveMaterialCount;
	assert(id > 0);
	auto liveMaterial = _newLiveMaterial(id);
	assert(liveMaterials.find(id) == liveMaterials.end());
	liveMaterials[id] = liveMaterial;
	return liveMaterial;
}

LiveMaterial* RenderAPI::_newLiveMaterial(int id) {
	assert(false);
	return nullptr;
}

static int inputId = 0;

void LiveMaterial::SetShaderSource(
	const char* fragSrc, const char* fragEntry,
	const char* vertSrc, const char* vertEntry) {

	vector<CompileTask> tasks;

	extern string GetShaderIncludePath();

	if (fragSrc && strlen(fragSrc) > 0) {
		CompileTask task;
		task.quitting = false;
		task.shaderType = Fragment;
		task.src = fragSrc;
		task.entryPoint = fragEntry;
		task.filename = GetShaderIncludePath() + "\\frag.hlsl";
		task.liveMaterialId = id();
		task.id = ++inputId;
		tasks.push_back(task);
	}

	if (vertSrc && strlen(vertSrc) > 0) {
		CompileTask task;
		task.quitting = false;
		task.shaderType = Vertex;
		task.src = vertSrc;
		task.entryPoint = vertEntry;
		task.filename = GetShaderIncludePath() + "\\vert.hlsl";
		task.liveMaterialId = id();
		task.id = ++inputId;
		tasks.push_back(task);
	}

	if (tasks.size() > 0) {
		_stats.compileState = CompileState::Compiling;
	}

	_QueueCompileTasks(tasks);
}

void LiveMaterial::_QueueCompileTasks(vector<CompileTask> compileTasks) {
	_renderAPI->QueueCompileTasks(compileTasks);
}

Stats LiveMaterial::GetStats() { return _stats; }
void LiveMaterial::SetStats(Stats stats) { _stats = stats; }

void LiveMaterial::PrintUniforms() {
	lock_guard<mutex> guard(uniformsMutex);

    std::stringstream ss;
    for (auto i = shaderProps.begin(); i != shaderProps.end(); ++i) {
        auto prop = i->second;
        ss << prop->name << " ";
#if SUPPORT_D3D11
		ss << "(offset: " << prop->offset << ", size: " << prop->size << ") ";
#endif

		float values[16];
		int numFloats;

		switch (prop->type) {
		case Float: numFloats = 1; break;
		case Vector2: numFloats = 2; break;
		case Vector3: numFloats = 3; break;
		case Vector4: numFloats = 4; break;
		case Matrix: numFloats = 16; break;
		default: numFloats = 0; break;
		}
		getproparray_locked(prop->name.c_str(), prop->type, &values[0], numFloats);
		for (int i = 0; i < numFloats; ++i) {
			ss << values[i] << " ";
		}
        ss << "\n";
    }
    std::string s(ss.str());
    Debug(s.c_str());
}

void LiveMaterial::SetComputeSource(
	const char* source, const char* entryPoint) {
	assert(false);
}

static Queue<CompileTask> compileQueue;

bool RenderAPI::supportsBackgroundCompiles() {
	return true;
}

void RenderAPI::Initialize() {
	if (supportsBackgroundCompiles()) {
		thread compileThread(compileThreadFunc, this);
		compileThread.detach();
	}
}

RenderAPI::~RenderAPI() {
	{
		lock_guard<mutex> guard(materialsMutex);
		for (auto iter = liveMaterials.begin(); iter != liveMaterials.end(); iter++) {
			auto liveMaterial = iter->second;
			delete liveMaterial;
		}
		liveMaterials.clear();
	}

	{
		CompileTask task;
		task.quitting = true;
		compileQueue.push(task);
	}
}


void RenderAPI::runCompileFunc() {
	bool quitting = true;
	while (quitting) {
		auto compileTask = compileQueue.pop();
		if (compileTask.quitting) // TODO: signal some other way
			quitting = true;
		else
			compileShader(compileTask);
	}
}

void RenderAPI::GetDebugInfo(int * numCompileTasks, int * numLiveMaterials)
{
	*numCompileTasks = (int)compileQueue.approximate_size();
	{
		lock_guard<mutex> guard(materialsMutex);
		*numLiveMaterials = static_cast<int>(liveMaterials.size());
	}
}

void RenderAPI::DrawMaterials(int uniformIndex)
{
}

bool RenderAPI::compileShader(CompileTask task) {
	assert(false);
	return false;
}

void RenderAPI::compileThreadFunc(RenderAPI * renderAPI) { renderAPI->runCompileFunc(); }

bool RenderAPI::DestroyLiveMaterial(int id) {
	lock_guard<mutex> guard(materialsMutex);

	auto iter = liveMaterials.find(id);
	if (iter == liveMaterials.end())
		return false;

	auto liveMaterial = iter->second;
	assert(liveMaterial->id() == id);

	liveMaterials.erase(id);
	delete liveMaterial;
	return true;
}

LiveMaterial * RenderAPI::GetLiveMaterialById(int id)
{
	lock_guard<mutex> guard(materialsMutex);
	return GetLiveMaterialByIdLocked(id);
}

LiveMaterial * RenderAPI::GetLiveMaterialByIdLocked(int id)
{
	auto iter = liveMaterials.find(id);
	if (iter == liveMaterials.end())
		return nullptr;

	auto liveMaterial = iter->second;
	return liveMaterial;
}

void RenderAPI::QueueCompileTasks(vector<CompileTask> tasks)
{
	for (size_t i = 0; i < tasks.size(); ++i)
		compileQueue.push(tasks[i]);
}

void RenderAPI::SetFlags(int flags) { this->flags = flags; }

RenderAPI* CreateRenderAPI(UnityGfxRenderer apiType)
{
#	if SUPPORT_D3D11
	if (apiType == kUnityGfxRendererD3D11)
	{
		extern RenderAPI* CreateRenderAPI_D3D11();
		return CreateRenderAPI_D3D11();
	}
#	endif // if SUPPORT_D3D11

#	if SUPPORT_D3D9
	if (apiType == kUnityGfxRendererD3D9)
	{
		extern RenderAPI* CreateRenderAPI_D3D9();
		return CreateRenderAPI_D3D9();
	}
#	endif // if SUPPORT_D3D9

#	if SUPPORT_D3D12
	if (apiType == kUnityGfxRendererD3D12)
	{
		extern RenderAPI* CreateRenderAPI_D3D12();
		return CreateRenderAPI_D3D12();
	}
#	endif // if SUPPORT_D3D9


#	if SUPPORT_OPENGL_UNIFIED
	if (apiType == kUnityGfxRendererOpenGLCore || apiType == kUnityGfxRendererOpenGLES20 || apiType == kUnityGfxRendererOpenGLES30)
	{
		extern RenderAPI* CreateRenderAPI_OpenGLCoreES(UnityGfxRenderer apiType);
		return CreateRenderAPI_OpenGLCoreES(apiType);
	}
#	endif // if SUPPORT_OPENGL_UNIFIED

#	if SUPPORT_OPENGL_LEGACY
	if (apiType == kUnityGfxRendererOpenGL)
	{
		extern RenderAPI* CreateRenderAPI_OpenGL2();
		return CreateRenderAPI_OpenGL2();
	}
#	endif // if SUPPORT_OPENGL_LEGACY

#	if SUPPORT_METAL
	if (apiType == kUnityGfxRendererMetal)
	{
		extern RenderAPI* CreateRenderAPI_Metal();
		return CreateRenderAPI_Metal();
	}
#	endif // if SUPPORT_METAL


	// Unknown or unsupported graphics API
	return NULL;
}
