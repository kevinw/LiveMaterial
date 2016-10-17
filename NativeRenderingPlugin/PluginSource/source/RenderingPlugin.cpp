// Example low level rendering Unity plugin

#include "PlatformBase.h"
#include "RenderAPI.h"

#include <assert.h>
#include <math.h>
#include <map>


mutex debugLogMutex;
static DebugLogFuncPtr s_debugLogFunc = nullptr;
DebugLogFuncPtr GetDebugFunc() { return s_debugLogFunc; }

static string s_shaderIncludePath;
string GetShaderIncludePath() { return s_shaderIncludePath; }


// --------------------------------------------------------------------------
// SetTextureFromUnity, an example function we export which is called by one of the scripts.

static void* g_TextureHandle = NULL;
static int   g_TextureWidth  = 0;
static int   g_TextureHeight = 0;

extern "C" void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API SetTextureFromUnity(void* textureHandle, int w, int h)
{
	// A script calls this at initialization time; just remember the texture pointer here.
	// Will update texture pixels each frame from the plugin rendering event (texture update
	// needs to happen on the rendering thread).
	g_TextureHandle = textureHandle;
	g_TextureWidth = w;
	g_TextureHeight = h;
}




// --------------------------------------------------------------------------
// UnitySetInterfaces

static void UNITY_INTERFACE_API OnGraphicsDeviceEvent(UnityGfxDeviceEventType eventType);

static IUnityInterfaces* s_UnityInterfaces = NULL;
static IUnityGraphics* s_Graphics = NULL;

extern "C" void	UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API UnityPluginLoad(IUnityInterfaces* unityInterfaces)
{
	s_UnityInterfaces = unityInterfaces;
	s_Graphics = s_UnityInterfaces->Get<IUnityGraphics>();
	s_Graphics->RegisterDeviceEventCallback(OnGraphicsDeviceEvent);
	
	// Run OnGraphicsDeviceEvent(initialize) manually on plugin load
	OnGraphicsDeviceEvent(kUnityGfxDeviceEventInitialize);
}


extern "C" void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API SetCallbackFunctions(DebugLogFuncPtr debugLogFunc) {
	{
		lock_guard<mutex> guard(debugLogMutex);
		s_debugLogFunc = debugLogFunc;
	}
}

extern "C" void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API UnityPluginUnload()
{
	s_Graphics->UnregisterDeviceEventCallback(OnGraphicsDeviceEvent);

	// clear the debug log function
	{
		lock_guard<mutex> guard(debugLogMutex);
		s_debugLogFunc = nullptr;
	}
}



// --------------------------------------------------------------------------
// GraphicsDeviceEvent


static RenderAPI* s_CurrentAPI = NULL;
static UnityGfxRenderer s_DeviceType = kUnityGfxRendererNull;

static void UNITY_INTERFACE_API OnGraphicsDeviceEvent(UnityGfxDeviceEventType eventType)
{
	// Create graphics API implementation upon initialization
	if (eventType == kUnityGfxDeviceEventInitialize) {
		assert(s_CurrentAPI == NULL);
		{
			lock_guard<mutex> guard(renderAPIMutex);
			s_DeviceType = s_Graphics->GetRenderer();
			s_CurrentAPI = CreateRenderAPI(s_DeviceType);
			s_CurrentAPI->Initialize();
		}
	}

	// Let the implementation process the device related events
	if (s_CurrentAPI) {
		s_CurrentAPI->ProcessDeviceEvent(eventType, s_UnityInterfaces);
	}

	// Cleanup graphics API implementation upon shutdown
	if (eventType == kUnityGfxDeviceEventShutdown) {
		{
			lock_guard<mutex> guard(renderAPIMutex);
			auto api = s_CurrentAPI;
			s_CurrentAPI = nullptr;
			delete api;
			s_DeviceType = kUnityGfxRendererNull;
		}
	}
}


#define UNITY_FUNC UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API

typedef LiveMaterial* NativePtr;

extern "C" {
	void UNITY_FUNC SetShaderIncludePath(const char* includePath) { s_shaderIncludePath = includePath; }

	NativePtr UNITY_FUNC CreateLiveMaterial() {
		assert(s_CurrentAPI);
		return s_CurrentAPI->CreateLiveMaterial();
	}


	int UNITY_FUNC CreateLiveMaterialId() {
		auto liveMaterial = CreateLiveMaterial();
		return liveMaterial ? liveMaterial->id() : -1;
	}

	NativePtr UNITY_FUNC GetLiveMaterialPtr(int id) { return s_CurrentAPI ? s_CurrentAPI->GetLiveMaterialById(id) : nullptr; }
	void UNITY_FUNC DestroyLiveMaterial(int id) { if (s_CurrentAPI) s_CurrentAPI->DestroyLiveMaterial(id); }
    Stats UNITY_FUNC GetStats(LiveMaterial* liveMaterial) { return liveMaterial->GetStats(); }
	void UNITY_FUNC SetStats(LiveMaterial* liveMaterial, Stats stats) { return liveMaterial->SetStats(stats); }
	bool UNITY_FUNC HasProperty(LiveMaterial* liveMaterial, const char* name) { return liveMaterial->HasProperty(name); }
	bool UNITY_FUNC NeedsRender(LiveMaterial* liveMaterial) { return liveMaterial->NeedsRender(); }
	void UNITY_FUNC SetDepthWritesEnabled(LiveMaterial* liveMaterial, bool enabled) { liveMaterial->SetDepthWritesEnabled(enabled); }
	void UNITY_FUNC SetShaderSource(LiveMaterial* liveMaterial, const char* fragSrc, const char* fragEntry, const char* vertSrc, const char* vertEntry) { liveMaterial->SetShaderSource(fragSrc, fragEntry, vertSrc, vertEntry); }
	void UNITY_FUNC SubmitUniforms(LiveMaterial* liveMaterial, int uniformsIndex) { liveMaterial->SubmitUniforms(uniformsIndex); }
	bool UNITY_FUNC SetTextureID(LiveMaterial* liveMaterial, const char* name, int id) { return liveMaterial->SetTextureID(name, id); }
	void UNITY_FUNC SetTexturePtr(LiveMaterial* liveMaterial, const char* name, int id, void* nativeTexturePointer) { return liveMaterial->SetTexturePtr(name, id, nativeTexturePointer); }
	void UNITY_FUNC SetFloat(LiveMaterial* liveMaterial, const char* name, float value) { liveMaterial->SetFloat(name, value); }
	void UNITY_FUNC SetVector4(LiveMaterial* liveMaterial, const char* name, float* value) { liveMaterial->SetVector4(name, value); }
	void UNITY_FUNC SetMatrix(LiveMaterial* liveMaterial, const char* name, float* value) { liveMaterial->SetMatrix(name, value); }
	void UNITY_FUNC SetFloatArray(LiveMaterial* liveMaterial, const char* name, float* value, int numFloats) { liveMaterial->SetFloatArray(name, value, numFloats); }
	void UNITY_FUNC SetVectorArray(LiveMaterial* liveMaterial, const char* name, float* values, int numVector4s) { liveMaterial->SetVectorArray(name, values, numVector4s); }
	void UNITY_FUNC SetMatrixArray(LiveMaterial* liveMaterial, const char* name, float* values, int numMatrices) { liveMaterial->SetMatrixArray(name, values, numMatrices); }
	void UNITY_FUNC GetVector4(LiveMaterial* liveMaterial, const char* name, float* value) { liveMaterial->GetVector4(name, value); }
	void UNITY_FUNC GetMatrix(LiveMaterial* liveMaterial, const char* name, float* value) { liveMaterial->GetMatrix(name, value); }
	float UNITY_FUNC GetFloat(LiveMaterial* liveMaterial, const char* name) {
		float value = 0;
		liveMaterial->GetFloat(name, &value);
		return value;
	}
	void UNITY_FUNC PrintUniforms(LiveMaterial* liveMaterial) { liveMaterial->PrintUniforms();  }
	void UNITY_FUNC GetDebugInfo(int* numCompileTasks, int* numLiveMaterials) {
		if (s_CurrentAPI)
			s_CurrentAPI->GetDebugInfo(numCompileTasks, numLiveMaterials);
	}
	void UNITY_FUNC SetFlags(int flags) {
		if (s_CurrentAPI)
			s_CurrentAPI->SetFlags(flags);
	}
	void UNITY_FUNC DumpUniformsToFile(LiveMaterial* liveMaterial, const char* filename) {
		assert(GetLiveMaterialPtr(liveMaterial->id()));
		liveMaterial->DumpUniformsToFile(filename, true);
	}
	void UNITY_FUNC ClearCompileCache() {
		if (s_CurrentAPI)
			s_CurrentAPI->ClearCompileCache();
	}

	bool UNITY_FUNC CanDraw(LiveMaterial* liveMaterial) { return liveMaterial->CanDraw(); }
}

mutex renderAPIMutex;
RenderAPI* GetCurrentRenderAPI() { return s_CurrentAPI; }

static void UNITY_INTERFACE_API OnRenderEvent(int packedValue) {
	if (s_CurrentAPI == nullptr)
		return;

	int16_t uniformIndex = packedValue & 0xffff;
	int16_t id = (packedValue >> 16) & 0xffff;

	//DebugSS("OnRenderEvent(id=" << id << ", uniformIndex=" << uniformIndex << ")");

	//DrawColoredTriangle(uniformIndex);
	lock_guard<mutex> guard(s_CurrentAPI->materialsMutex);
	auto liveMaterial = s_CurrentAPI->GetLiveMaterialByIdLocked(id);
	if (liveMaterial) {
		liveMaterial->Draw(uniformIndex);
	}
	else {
		DebugSS("not drawing: id: " << id << ", uniformIndex: " << uniformIndex);
	}
	//ModifyTexturePixels();
}


extern "C" UnityRenderingEvent UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API GetRenderEventFunc()
{
	return OnRenderEvent;
}

