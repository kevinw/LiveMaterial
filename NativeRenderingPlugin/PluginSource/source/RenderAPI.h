#pragma once

#include "Unity/IUnityGraphics.h"
#include <map>
#include <string>
#include <fstream>
#include <sstream>
#include <vector>
#include <mutex>
#include <thread>
#include <iostream>

#include "ConcurrentQueue.h"
#include "ShaderProp.h"

using std::string;
using std::thread;
using std::vector;
using std::map;
using std::mutex;
using std::lock_guard;
using std::stringstream;

struct IUnityInterfaces;

#define MAX_GPU_BUFFERS 4

extern mutex debugLogMutex;
typedef void(*DebugLogFuncPtr)(const char *);
DebugLogFuncPtr GetDebugFunc();

#define Debug(m) do { \
	lock_guard<mutex> _debug_log_guard(debugLogMutex); \
	if (GetDebugFunc()) { GetDebugFunc()(m); } else { std::cout << m << std::endl; } } while(0);

#define DebugSS(ssexp) do { \
	std::stringstream _ss; _ss << ssexp; \
	std::string _sDebugString(_ss.str()); \
    if (GetDebugFunc()) { \
        GetDebugFunc()(_sDebugString.c_str()); \
    } else { \
		std::cout << _sDebugString.c_str() << std::endl; \
	} \
} while(0);

class RenderAPI;
class LiveMaterial;

enum ShaderType { Vertex, Fragment, Compute };
const char* shaderTypeName(ShaderType shaderType);

#ifndef SAFE_DELETE
#define SAFE_DELETE(a) if (a) { delete a; a = nullptr; }
#endif

void writeTextToFile(const char* filename, const char* text);


struct CompileTask {
	ShaderType shaderType;
	string src;
	string filename;
	string entryPoint;
	size_t hash() const {
		auto h1 = std::hash<string>{}(src);
		auto h2 = std::hash<string>{}(filename);
		auto h3 = std::hash<string>{}(entryPoint);
		return h1 ^ (h2 << 1) ^ (h3 << 2);
	}
	int liveMaterialId;
	int id;
	bool quitting;
};

typedef map<string, ShaderProp*> PropMap;

enum CompileState {
    NeverCompiled,
    Compiling,
    Success,
    Error
};

struct Stats {
    CompileState compileState;
    uint64_t compileTimeMs;
    unsigned int instructionCount;
};

extern mutex renderAPIMutex;
RenderAPI* GetCurrentRenderAPI();

class LiveMaterial {
public:
	LiveMaterial(RenderAPI* renderAPI, int id);
	virtual ~LiveMaterial() {}
	int id() const { return _id; }

	Stats GetStats();
	void SetDrawingEnabled(bool enabled);
	void SetStats(Stats stats);
	void GetFloat(const char* name, float* value);
	void GetVector4(const char* name, float* value);
	void GetMatrix(const char* name, float* value);
	void SetFloat(const char* name, float value);
	void SetVector4(const char* name, float* value);
	void SetMatrix(const char* name, float* value);
	void SetVectorArray(const char* name, float* values, int numVector4s);
	void SetMatrixArray(const char* name, float* values, int numMatrices);
	void SetFloatArray(const char* name, float* value, int numElems);
	bool SetTextureID(const char* name, int id);
	void SetTexturePtr(const char* name, int id, void* nativeTexturePointer);
	void SubmitUniforms(int uniformsIndex);
	bool HasProperty(const char* name);
	virtual void SetDepthWritesEnabled(bool enabled);
	void PrintUniforms();
	void setproparray(const char* name, PropType type, float* value, int numFloats);
	void getproparray(const char* name, PropType type, float* value, int numFloats);
	void getproparray_locked(const char* name, PropType type, float* value, int numFloats);
	virtual void Draw(int uniformIndex);
	virtual bool NeedsRender();
	void SetShaderSource(const char* fragSrc, const char* fragEntry, const char* vertSrc, const char* vertEntry);
	void SetComputeSource(const char* source, const char* entryPoint);
	void SetMesh(int vertexCount, float* vertices, float* normals, float* uvs);

	void DumpUniformsToFile(const char* filename, bool flatten);

	virtual void SetRenderTexture(void* nativeTexturePtr);
	virtual bool CanDraw() const;

protected:
    virtual void _QueueCompileTasks(vector<CompileTask> tasks);

	ShaderProp* propForNameSizeOffset(const char* name, uint16_t size, uint16_t offset);
	ShaderProp* propForName(const char* name, PropType type);
	virtual void _SetTexture(const char* name, void* nativeTexturePtr);

	struct MeshVertex
	{
		float pos[3];
		float normal[3];
		float uv[2];
	};

	vector<MeshVertex> mesh;

	Stats _stats = {};

	RenderAPI* _renderAPI = nullptr;
	int _id = -1;
	bool _drawingEnabled = true;

	void ensureConstantBufferSize(size_t size, PropMap* oldProps = nullptr, PropMap* newProps = nullptr);
	unsigned char* _constantBuffer = nullptr;
	size_t _constantBufferSize = 0;
	unsigned char* _gpuBuffer = nullptr;

	mutex uniformsMutex;
	PropMap shaderProps; // a mapping of name -> prop description, type, and offset into the constant buffer

	mutex gpuMutex;

	mutex texturesMutex;
	map<int, void*> texturePointers; // caches Object::InstanceID() -> GetNativeTexturePtr()

private:
	LiveMaterial();
	LiveMaterial(const LiveMaterial&);
};

// Super-simple "graphics abstraction" This is nothing like how a proper platform abstraction layer would look like;
// all this does is a base interface for whatever our plugin sample needs. Which is only "draw some triangles"
// and "modify a texture" at this point.
//
// There are implementations of this base class for D3D9, D3D11, OpenGL etc.; see individual RenderAPI_* files.
class RenderAPI
{
public:
	void Initialize();
	void runCompileFunc();
	virtual ~RenderAPI();

	void GetDebugInfo(int* numCompileTasks, int* numLiveMaterials);

	// Process general event like initialization, shutdown, device loss/reset etc.
	virtual void ProcessDeviceEvent(UnityGfxDeviceEventType type, IUnityInterfaces* interfaces) = 0;

	// Draw some triangle geometry, using some simple rendering state.
	// Upon call into our plug-in the render state can be almost completely arbitrary depending
	// on what was rendered in Unity before. Here, we turn off culling, blending, depth writes etc.
	// and draw the triangles with a given world matrix. The triangle data is
	// float3 (position) and byte4 (color) per vertex.
	virtual void DrawSimpleTriangles(const float worldMatrix[16], int triangleCount, const void* verticesFloat3Byte4) = 0;


	// Begin modifying texture data. You need to pass texture width/height too, since some graphics APIs
	// (e.g. OpenGL ES) do not have a good way to query that from the texture itself...
	//
	// Returns pointer into the data buffer to write into (or NULL on failure), and pitch in bytes of a single texture row.
	virtual void* BeginModifyTexture(void* textureHandle, int textureWidth, int textureHeight, int* outRowPitch) = 0;

	// End modifying texture data.
	virtual void EndModifyTexture(void* textureHandle, int textureWidth, int textureHeight, int rowPitch, void* dataPtr) = 0;

	LiveMaterial* CreateLiveMaterial();
	bool DestroyLiveMaterial(int id);

	enum Flags {
		ShowWarnings = 1
	};

	bool showWarnings() const { return flags & ShowWarnings; }
	void SetFlags(int flags);

	LiveMaterial* GetLiveMaterialById(int id);
	LiveMaterial* GetLiveMaterialByIdLocked(int id);

	virtual void QueueCompileTasks(vector<CompileTask> tasks);
	mutex materialsMutex;

	virtual void ClearCompileCache();

protected:
	int flags = 0;
    virtual bool supportsBackgroundCompiles();
	virtual bool compileShader(CompileTask compileTask);

	int liveMaterialCount = 0;
	map<int, LiveMaterial*> liveMaterials;

	static void compileThreadFunc(RenderAPI* renderAPI);
	friend void compileThreadFunc(RenderAPI* renderAPI);

	virtual LiveMaterial* _newLiveMaterial(int id);

};


// Create a graphics API implementation instance for the given API type.
RenderAPI* CreateRenderAPI(UnityGfxRenderer apiType);

