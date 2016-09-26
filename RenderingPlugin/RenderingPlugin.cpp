#define	D3D_DEBUG_INFO
#include "RenderingPlugin.h"
#include "Unity/IUnityGraphics.h"

#if SUPPORT_D3D9 || SUPPORT_D3D11 || SUPPORT_D3D12
#define SUPPORT_D3D
#define INITGUID // todo: what to link against to get IID_ID3D11ShaderReflection?
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include "dxerr.h"
#endif

#include <sstream>
#include <cassert>
#include <iostream>
#include <cmath>
#include <thread>
#include <mutex>
#include <cstdio>
#include <vector>
#include <string>
#include <fstream>
#include <map>

using std::string;
using std::endl;
using std::mutex;
using std::lock_guard;
using std::vector;

#include "ProducerConsumerQueue.h"
#include "StopWatch.h"

static mutex compileMutex;
static std::condition_variable compileCV;
static vector<CompileTask> pendingCompileTasks;

static bool writeDebugFiles = false;
static bool shaderDebugging = false;
static int optimizationLevel = 3;

static std::thread* compileThread;

static void compileThreadFunc();

static void startCompileThread() {
	if (compileThread == nullptr) {
		compileThread = new std::thread(compileThreadFunc);
		compileThread->detach();
	}
}

static void submitCompileTasks(vector<CompileTask> compileTasks, bool append) {
	if (compileTasks.size() == 0)
		return;

	{
		std::lock_guard<mutex> lock(compileMutex);

		if (!append) {
			// if we're not appending, replace any vertex and fragment shaders
			vector<CompileTask> newTasks;
			for (size_t i = 0; i < pendingCompileTasks.size(); ++i) {
				auto shaderType = pendingCompileTasks[i].shaderType;
				if (!(shaderType == Fragment || shaderType == Vertex))
					newTasks.push_back(pendingCompileTasks[i]);
			}
			pendingCompileTasks = newTasks;
		}

		pendingCompileTasks.insert(pendingCompileTasks.end(), compileTasks.begin(), compileTasks.end());
		startCompileThread();
	}
	compileCV.notify_one();
}


static bool quitCompileThread = false;

static void terminateCompileThread() {
	std::unique_lock<mutex> lk(compileMutex);
	quitCompileThread = true;
	compileCV.notify_one();
}

static void compileThreadFunc() {
	while (true) {
		{
			std::unique_lock<mutex> lk(compileMutex);
			compileCV.wait(lk);
			if (quitCompileThread) {
				quitCompileThread = false;
				compileThread = nullptr;
				break;
			}
		}

		vector<CompileTask> compileTasks;
		bool keepGoing = true;
		while (keepGoing) {
			{
				std::lock_guard<std::mutex> lock(compileMutex);
				compileTasks = pendingCompileTasks;
				pendingCompileTasks.clear();
			}

			for (size_t i = 0; i < compileTasks.size(); ++i)
				compileTasks[i]();

			{
				std::lock_guard<std::mutex> lock(compileMutex);
				keepGoing = pendingCompileTasks.size() > 0;
			}
		}

	}
}


static mutex uniformMutex;
#define GUARD_UNIFORMS lock_guard<mutex> _lock_guard_uniforms(uniformMutex)

static mutex callbackMutex;
#define GUARD_CALLBACK lock_guard<mutex> _lock_guard_callbacks(callbackMutex)

static mutex debugMutex;
#define GUARD_DEBUG lock_guard<mutex> _lock_guard_debug(debugMutex)

static mutex gpuMutex;
#define GUARD_GPU lock_guard<mutex> _lock_guard_gpu(gpuMutex)

static mutex texturesMutex;
#define GUARD_TEXTURES lock_guard<mutex> _lock_guard_textures(texturesMutex)

#define NUM_VERTS 6

unsigned char* constantBuffer = nullptr;
size_t constantBufferSize = 0;

unsigned char* gpuBuffer = nullptr;

#define MAX_GPU_BUFFERS 4

enum PropType {
	Float,
	Vector2,
	Vector3,
	Vector4,
	Matrix,
};

static int numElemsForPropType(PropType type) {
	switch (type) {
	case PropType::Float: return 1;
	case PropType::Vector4: return 4;
	case PropType::Vector3: return 3;
	case PropType::Vector2: return 2;
	case PropType::Matrix: return 16;
	default: {
		assert(false);
		Debug("Unknown proptype");
		return 0;
	}
	}

}

const std::string propTypeStrings[] = {
	"Float",
	"Vector2",
	"Vector3",
	"Vector4",
	"Matrix",
};


struct ShaderProp {
	ShaderProp(PropType type_, std::string name_)
		: type(type_)
		, name(name_)
	{
#if SUPPORT_OPENGL_UNIFIED || SUPPORT_OPENGL_CORE
		uniformIndex = UNIFORM_UNSET;
#endif
		offset = 0;
		size = 0;
		arraySize = 0;

#ifdef SUPPORT_D3D
		arraySize = 1;
#endif
	}


	PropType type;
	const std::string typeString() { return propTypeStrings[(size_t)type]; }
	std::string name;

	float value(int n) {
		if (!constantBuffer || size == 0) return 0.0f;
		return *((float*)(constantBuffer + offset + n * sizeof(float)));
	}

#if SUPPORT_OPENGL_UNIFIED || SUPPORT_OPENGL_CORE
	static const int UNIFORM_UNSET = -2;
	static const int UNIFORM_INVALID = -1;
	int uniformIndex;
#endif

	uint16_t offset;
	uint16_t size;
	uint16_t arraySize;

	static PropType typeForSize(uint16_t size) {
		switch (size) {
		case sizeof(float) : return Float;
		case 2 * sizeof(float) : return Vector2;
		case 3 * sizeof(float) : return Vector3;
		case 4 * sizeof(float) : return Vector4;
		case 16 * sizeof(float) : return Matrix;
		default: {
			DebugSS("unknown size " << size);
			assert(false);
			return Float;
		}
		}
	}
};

static ShaderProp* propForNameSizeOffset(const char* name, uint16_t size, uint16_t offset);
typedef std::map<std::string, ShaderProp*> PropMap;
PropMap shaderProps; // a mapping of name -> prop description, type, and offset into the constant buffer


// --------------------------------------------------------------------------
// Include headers for the graphics APIs we support

#if SUPPORT_D3D9
#	include <d3d9.h>
#	include "Unity/IUnityGraphicsD3D9.h"
#endif
#if SUPPORT_D3D11
#	include <d3d11.h>
#	include "Unity/IUnityGraphicsD3D11.h"
#endif
#if SUPPORT_D3D12
#	include <d3d12.h>
#	include "Unity/IUnityGraphicsD3D12.h"
#endif

#if SUPPORT_OPENGL_UNIFIED
#	if UNITY_IPHONE
#		include <OpenGLES/ES2/gl.h>
#	elif UNITY_ANDROID
#		include <GLES2/gl2.h>
#	else
#       if defined(_WIN32) || defined(_WIN64)
#			include "GL/glew.h"
#       else
#			include "OpenGL/gl3.h"
#			include "OpenGL/glu.h"
#		endif
#	endif
#endif

static PluginCallback PluginCallbackFunc = nullptr;
static FuncPtr _DebugFunc = nullptr;

FuncPtr GetDebugFunc() { return _DebugFunc; }

static bool verbose = false;
static bool updateUniforms = true;
static bool didInit = false;
#if SUPPORT_D3D11
static void updateUniformsD3D11(ID3D11DeviceContext* ctx, int uniformIndex);
#endif
static void updateUniformsGL();
#if SUPPORT_OPENGL_UNIFIED
static void DiscoverGLUniforms(GLuint program);
#endif
static void printUniforms();

folly::ProducerConsumerQueue<ShaderSource> shaderSourceQueue(10);

#if SUPPORT_OPENGL_UNIFIED

struct GLTypeTable {
  GLenum type;
  const char* typeName;
  const char* glslTypeName;
};

static GLTypeTable glTypeTable[] = {
  {GL_FLOAT, "GL_FLOAT",	"float"},
  {GL_FLOAT_VEC2, "GL_FLOAT_VEC2",	"vec2"},
  {GL_FLOAT_VEC3, "GL_FLOAT_VEC3",	"vec3"},
  {GL_FLOAT_VEC4, "GL_FLOAT_VEC4",	"vec4"},
  {GL_DOUBLE, "GL_DOUBLE",	"double"},
  {GL_DOUBLE_VEC2, "GL_DOUBLE_VEC2",	"dvec2"},
  {GL_DOUBLE_VEC3, "GL_DOUBLE_VEC3",	"dvec3"},
  {GL_DOUBLE_VEC4, "GL_DOUBLE_VEC4",	"dvec4"},
  {GL_INT, "GL_INT",	"int"},
  {GL_INT_VEC2, "GL_INT_VEC2",	"ivec2"},
  {GL_INT_VEC3, "GL_INT_VEC3",	"ivec3"},
  {GL_INT_VEC4, "GL_INT_VEC4",	"ivec4"},
  {GL_UNSIGNED_INT, "GL_UNSIGNED_INT",	"unsigned int"},
  {GL_UNSIGNED_INT_VEC2, "GL_UNSIGNED_INT_VEC2",	"uvec2"},
  {GL_UNSIGNED_INT_VEC3, "GL_UNSIGNED_INT_VEC3",	"uvec3"},
  {GL_UNSIGNED_INT_VEC4, "GL_UNSIGNED_INT_VEC4",	"uvec4"},
  {GL_BOOL, "GL_BOOL",	"bool"},
  {GL_BOOL_VEC2, "GL_BOOL_VEC2",	"bvec2"},
  {GL_BOOL_VEC3, "GL_BOOL_VEC3",	"bvec3"},
  {GL_BOOL_VEC4, "GL_BOOL_VEC4",	"bvec4"},
  {GL_FLOAT_MAT2, "GL_FLOAT_MAT2",	"mat2"},
  {GL_FLOAT_MAT3, "GL_FLOAT_MAT3",	"mat3"},
  {GL_FLOAT_MAT4, "GL_FLOAT_MAT4",	"mat4"},
  {GL_FLOAT_MAT2x3, "GL_FLOAT_MAT2x3",	"mat2x3"},
  {GL_FLOAT_MAT2x4, "GL_FLOAT_MAT2x4",	"mat2x4"},
  {GL_FLOAT_MAT3x2, "GL_FLOAT_MAT3x2",	"mat3x2"},
  {GL_FLOAT_MAT3x4, "GL_FLOAT_MAT3x4",	"mat3x4"},
  {GL_FLOAT_MAT4x2, "GL_FLOAT_MAT4x2",	"mat4x2"},
  {GL_FLOAT_MAT4x3, "GL_FLOAT_MAT4x3",	"mat4x3"},
  {GL_DOUBLE_MAT2, "GL_DOUBLE_MAT2",	"dmat2"},
  {GL_DOUBLE_MAT3, "GL_DOUBLE_MAT3",	"dmat3"},
  {GL_DOUBLE_MAT4, "GL_DOUBLE_MAT4",	"dmat4"},
  {GL_DOUBLE_MAT2x3, "GL_DOUBLE_MAT2x3",	"dmat2x3"},
  {GL_DOUBLE_MAT2x4, "GL_DOUBLE_MAT2x4",	"dmat2x4"},
  {GL_DOUBLE_MAT3x2, "GL_DOUBLE_MAT3x2",	"dmat3x2"},
  {GL_DOUBLE_MAT3x4, "GL_DOUBLE_MAT3x4",	"dmat3x4"},
  {GL_DOUBLE_MAT4x2, "GL_DOUBLE_MAT4x2",	"dmat4x2"},
  {GL_DOUBLE_MAT4x3, "GL_DOUBLE_MAT4x3",	"dmat4x3"},
  {GL_SAMPLER_1D, "GL_SAMPLER_1D",	"sampler1D"},
  {GL_SAMPLER_2D, "GL_SAMPLER_2D",	"sampler2D"},
  {GL_SAMPLER_3D, "GL_SAMPLER_3D",	"sampler3D"},
  {GL_SAMPLER_CUBE, "GL_SAMPLER_CUBE",	"samplerCube"},
  {GL_SAMPLER_1D_SHADOW, "GL_SAMPLER_1D_SHADOW",	"sampler1DShadow"},
  {GL_SAMPLER_2D_SHADOW, "GL_SAMPLER_2D_SHADOW",	"sampler2DShadow"},
  {GL_SAMPLER_1D_ARRAY, "GL_SAMPLER_1D_ARRAY",	"sampler1DArray"},
  {GL_SAMPLER_2D_ARRAY, "GL_SAMPLER_2D_ARRAY",	"sampler2DArray"},
  {GL_SAMPLER_1D_ARRAY_SHADOW, "GL_SAMPLER_1D_ARRAY_SHADOW",	"sampler1DArrayShadow"},
  {GL_SAMPLER_2D_ARRAY_SHADOW, "GL_SAMPLER_2D_ARRAY_SHADOW",	"sampler2DArrayShadow"},
  {GL_SAMPLER_2D_MULTISAMPLE, "GL_SAMPLER_2D_MULTISAMPLE",	"sampler2DMS"},
  {GL_SAMPLER_2D_MULTISAMPLE_ARRAY, "GL_SAMPLER_2D_MULTISAMPLE_ARRAY",	"sampler2DMSArray"},
  {GL_SAMPLER_CUBE_SHADOW, "GL_SAMPLER_CUBE_SHADOW",	"samplerCubeShadow"},
  {GL_SAMPLER_BUFFER, "GL_SAMPLER_BUFFER",	"samplerBuffer"},
  {GL_SAMPLER_2D_RECT, "GL_SAMPLER_2D_RECT",	"sampler2DRect"},
  {GL_SAMPLER_2D_RECT_SHADOW, "GL_SAMPLER_2D_RECT_SHADOW",	"sampler2DRectShadow"},
  {GL_INT_SAMPLER_1D, "GL_INT_SAMPLER_1D",	"isampler1D"},
  {GL_INT_SAMPLER_2D, "GL_INT_SAMPLER_2D",	"isampler2D"},
  {GL_INT_SAMPLER_3D, "GL_INT_SAMPLER_3D",	"isampler3D"},
  {GL_INT_SAMPLER_CUBE, "GL_INT_SAMPLER_CUBE",	"isamplerCube"},
  {GL_INT_SAMPLER_1D_ARRAY, "GL_INT_SAMPLER_1D_ARRAY",	"isampler1DArray"},
  {GL_INT_SAMPLER_2D_ARRAY, "GL_INT_SAMPLER_2D_ARRAY",	"isampler2DArray"},
  {GL_INT_SAMPLER_2D_MULTISAMPLE, "GL_INT_SAMPLER_2D_MULTISAMPLE",	"isampler2DMS"},
  {GL_INT_SAMPLER_2D_MULTISAMPLE_ARRAY, "GL_INT_SAMPLER_2D_MULTISAMPLE_ARRAY",	"isampler2DMSArray"},
  {GL_INT_SAMPLER_BUFFER, "GL_INT_SAMPLER_BUFFER",	"isamplerBuffer"},
  {GL_INT_SAMPLER_2D_RECT, "GL_INT_SAMPLER_2D_RECT",	"isampler2DRect"},
  {GL_UNSIGNED_INT_SAMPLER_1D, "GL_UNSIGNED_INT_SAMPLER_1D",	"usampler1D"},
  {GL_UNSIGNED_INT_SAMPLER_2D, "GL_UNSIGNED_INT_SAMPLER_2D",	"usampler2D"},
  {GL_UNSIGNED_INT_SAMPLER_3D, "GL_UNSIGNED_INT_SAMPLER_3D",	"usampler3D"},
  {GL_UNSIGNED_INT_SAMPLER_CUBE, "GL_UNSIGNED_INT_SAMPLER_CUBE",	"usamplerCube"},
  {GL_UNSIGNED_INT_SAMPLER_1D_ARRAY, "GL_UNSIGNED_INT_SAMPLER_1D_ARRAY",	"usampler2DArray"},
  {GL_UNSIGNED_INT_SAMPLER_2D_ARRAY, "GL_UNSIGNED_INT_SAMPLER_2D_ARRAY",	"usampler2DArray"},
  {GL_UNSIGNED_INT_SAMPLER_2D_MULTISAMPLE, "GL_UNSIGNED_INT_SAMPLER_2D_MULTISAMPLE",	"usampler2DMS"},
  {GL_UNSIGNED_INT_SAMPLER_2D_MULTISAMPLE_ARRAY, "GL_UNSIGNED_INT_SAMPLER_2D_MULTISAMPLE_ARRAY",	"usampler2DMSArray"},
  {GL_UNSIGNED_INT_SAMPLER_BUFFER, "GL_UNSIGNED_INT_SAMPLER_BUFFER",	"usamplerBuffer"},
  {GL_UNSIGNED_INT_SAMPLER_2D_RECT, "GL_UNSIGNED_INT_SAMPLER_2D_RECT",	"usampler2DRect"},
  {0, nullptr, nullptr}
};

const char* getGLTypeName(GLenum typeEnum) {
    int i = 0;
    
    GLTypeTable entry = glTypeTable[i];
    while (entry.typeName) {
        if (entry.type == typeEnum)
            return entry.typeName;
        
        entry = glTypeTable[++i];
    }
    
    return nullptr;
}

#endif

#ifdef SUPPORT_D3D
static vector<ID3D11ShaderResourceView*> resourceViews;
static vector<std::pair<ID3D11Resource*, size_t> > pendingResources;
static std::map<string, size_t> resourceViewIndexes;
#endif


struct Shader {
	Shader(int id);
	~Shader();

	int id;
	string source;
	string entryPoint;
#ifdef SUPPORT_D3D
	ID3D11ComputeShader* d3d11ComputeShader;

	bool IsReady() const { return this->d3d11ComputeShader != nullptr; }
#else
    bool IsReady() const { return false; }
#endif
    
	void SetSource(const char* source, const char* entryPoint);
	void Dispatch(int x, int y, int z);
};

struct ComputeBuffer {
	ComputeBuffer(int id);
	~ComputeBuffer();

	int id;
};

static Shader* findComputeShader(int id);
static ComputeBuffer* findComputeBuffer(int id);


#if SUPPORT_OPENGL_UNIFIED
static vector<GLint> textureIDs;
static vector<GLint> uniformLocs;
static std::map<string, size_t> textureUnits;
#endif



// --------------------------------------------------------------------------
// Helper utilities


static void writeTextToFile(const char* filename, const char* text) {
	std::ofstream debugOut;
	debugOut.open(filename);
	debugOut << text;
	debugOut.close();
}

// Prints a string
static void DebugLog (const char* str)
{
	#if UNITY_WIN
	OutputDebugStringA (str);
	#else
	printf ("%s", str);
	#endif
}

// COM-like Release macro
#ifndef SAFE_RELEASE
#define SAFE_RELEASE(a) if (a) { a->Release(); a = NULL; }
#endif

//-----------------------------------------------------------------
// Print for OpenGL errors
//
// Returns 1 if an OpenGL error occurred, 0 otherwise.
//

#define printOpenGLError() printOglError(__FILE__, __LINE__)

int printOglError(const char *file, int line) {
    GLenum glErr = glGetError();
    if (glErr == GL_NO_ERROR)
        return 0;
    
    const int SIZE = 1024 * 5;
    char buffer [SIZE];

    snprintf(buffer, SIZE, "glError in %s:%d: %s\n",
           file, line, gluErrorString(glErr));
    Debug(buffer);
    return 1;
}

GLuint loadShader(GLenum type, const char *shaderSrc, const char* debugOutPath);

enum
{
	ATTRIB_POSITION = 0,
	ATTRIB_COLOR = 1,
    ATTRIB_UV = 2
};

// --------------------------------------------------------------------------
// SetUnityStreamingAssetsPath, an example function we export which is called by one of the scripts.

static string shaderIncludePath;

static void INIT_MESSAGE(const char* msg) {
    if (!didInit) {
        didInit = true;
        Debug(msg);
    }
}

// --------------------------------------------------------------------------
// UnitySetInterfaces

static void UNITY_INTERFACE_API OnGraphicsDeviceEvent(UnityGfxDeviceEventType eventType);

static IUnityInterfaces* s_UnityInterfaces = NULL;
static IUnityGraphics* s_Graphics = NULL;
static UnityGfxRenderer s_DeviceType = kUnityGfxRendererNull;

extern "C" void	UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API UnityPluginLoad(IUnityInterfaces* unityInterfaces)
{
	s_UnityInterfaces = unityInterfaces;
	s_Graphics = s_UnityInterfaces->Get<IUnityGraphics>();
	s_Graphics->RegisterDeviceEventCallback(OnGraphicsDeviceEvent);
	
	// Run OnGraphicsDeviceEvent(initialize) manually on plugin load
	OnGraphicsDeviceEvent(kUnityGfxDeviceEventInitialize);
}

extern "C" void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API UnityPluginUnload()
{
	s_Graphics->UnregisterDeviceEventCallback(OnGraphicsDeviceEvent);
}

// --------------------------------------------------------------------------
// GraphicsDeviceEvent

// Actual setup/teardown functions defined below
#if SUPPORT_D3D9
static void DoEventGraphicsDeviceD3D9(UnityGfxDeviceEventType eventType);
#endif
#if SUPPORT_D3D11
static void DoEventGraphicsDeviceD3D11(UnityGfxDeviceEventType eventType);
#endif
#if SUPPORT_D3D12
static void DoEventGraphicsDeviceD3D12(UnityGfxDeviceEventType eventType);
#endif
#if SUPPORT_OPENGL_UNIFIED
static void DoEventGraphicsDeviceGLUnified(UnityGfxDeviceEventType eventType);
#endif

static void UNITY_INTERFACE_API OnGraphicsDeviceEvent(UnityGfxDeviceEventType eventType)
{
	UnityGfxRenderer currentDeviceType = s_DeviceType;

	switch (eventType)
	{
	case kUnityGfxDeviceEventInitialize:
		{
			Debug("OnGraphicsDeviceEvent(Initialize).");
			s_DeviceType = s_Graphics->GetRenderer();
			currentDeviceType = s_DeviceType;

#ifdef SUPPORT_D3D
			startCompileThread();
#endif

			break;
		}

	case kUnityGfxDeviceEventShutdown:
		{
			Debug("OnGraphicsDeviceEvent(Shutdown).");
			s_DeviceType = kUnityGfxRendererNull;
            
#ifdef SUPPORT_D3D
			terminateCompileThread();
#endif
            
			break;
		}

	case kUnityGfxDeviceEventBeforeReset:
		{
			Debug("OnGraphicsDeviceEvent(BeforeReset).");
			break;
		}

	case kUnityGfxDeviceEventAfterReset:
		{
			Debug("OnGraphicsDeviceEvent(AfterReset).");
			break;
		}
	};

	#if SUPPORT_D3D9
	if (currentDeviceType == kUnityGfxRendererD3D9)
		DoEventGraphicsDeviceD3D9(eventType);
	#endif

	#if SUPPORT_D3D11
	if (currentDeviceType == kUnityGfxRendererD3D11)
		DoEventGraphicsDeviceD3D11(eventType);
	#endif

	#if SUPPORT_D3D12
	if (currentDeviceType == kUnityGfxRendererD3D12)
		DoEventGraphicsDeviceD3D12(eventType);
	#endif
	
	#if SUPPORT_OPENGL_UNIFIED
	if (currentDeviceType == kUnityGfxRendererOpenGLES20 ||
		currentDeviceType == kUnityGfxRendererOpenGLES30 ||
		currentDeviceType == kUnityGfxRendererOpenGLCore)
		DoEventGraphicsDeviceGLUnified(eventType);
	#endif
}


#pragma pack(push, r1, 1)   // n = 16, pushed to stack
struct MyVertex {
	float x, y, z;
    float u, v;
};
#pragma pack(pop, r1)   // n = 2 , stack popped
static void SetDefaultGraphicsState ();
static void DoRendering (const float* worldMatrix, const float* identityMatrix, float* projectionMatrix, const MyVertex* verts, int uniformIndex);

static GLuint	g_VProg   = 0;
static GLuint	g_FShader = 0;
static GLuint	g_Program = 0;

static void LinkProgram();

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

static Stats stats = {};

#if SUPPORT_D3D11
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

static void constantBufferReflect(ID3DBlob*);
HRESULT CompileShader(_In_ const char* src, _In_ LPCSTR srcName, _In_ LPCSTR entryPoint, _In_ LPCSTR profile, const D3D_SHADER_MACRO defines[], _Outptr_ ID3DBlob** blob, _Outptr_ ID3DBlob** errorBlob, UINT flags);
#endif


struct CompileTaskOutput {
	ShaderType shaderType;
#if SUPPORT_D3D11
	ID3DBlob* shaderBlob;
#endif
	int shaderId;
};

folly::ProducerConsumerQueue<CompileTaskOutput> shaderCompilerOutputs(10);


void CompileTask::operator()() {
#if SUPPORT_D3D11
	const D3D_SHADER_MACRO defines[] = {
		"LIVE_MATERIAL", "1",
		NULL, NULL
	};

	ID3DBlob *shaderBlob = nullptr;
	ID3DBlob *error = nullptr;

	if (shaderType == Fragment)
		stats.compileState = CompileState::Compiling;

	UINT flags = D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY;

  /*if (optimizationLevel == 0) flags |= D3DCOMPILE_OPTIMIZATION_LEVEL0;
  if (optimizationLevel == 1) flags |= D3DCOMPILE_OPTIMIZATION_LEVEL1;
  if (optimizationLevel == 2) flags |= D3DCOMPILE_OPTIMIZATION_LEVEL2;
  if (optimizationLevel == 3) flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
  else DebugSS("Unknown optimization level " << optimizationLevel);
  */
	flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;

	if (shaderDebugging) {
		Debug("Compiling shader with D3DCOMPILE_DEBUG");
		flags |= D3DCOMPILE_DEBUG;
	}


	auto profile = profileNameForShaderType(shaderType);
	if (!profile) return;

	if (src.empty() || srcName.empty() || entryPoint.empty()) {
		Debug("empty src or srcName or entryPoint");
		return;
	}

	StopWatch d3dCompileWatch;
	auto hr = CompileShader(src.c_str(), srcName.c_str(), entryPoint.c_str(), profile, defines, &shaderBlob, &error, flags);

	if (FAILED(hr)) {
		std::string errstr;
		if (error) errstr = std::string((char*)error->GetBufferPointer(), error->GetBufferSize());
		DebugSS("Could not compile shader:\n " << errstr);
		stats.compileState = CompileState::Error;		
	} else {
		if (shaderType == Fragment) {
			stats.compileTimeMs = d3dCompileWatch.ElapsedMs();
			stats.compileState = CompileState::Success;
		}

		CompileTaskOutput output = { shaderType, shaderBlob, this->id };
		if (!shaderCompilerOutputs.write(output)) {
			Debug("Shader compiler output queue is full");
		} else {
			// TODO: not necessary in release builds
			GUARD_CALLBACK;
			if (PluginCallbackFunc)
				PluginCallbackFunc(NeedsSceneViewRepaint);
		}
	}
	if (error)
		error->Release();
#endif

}


static void MaybeCompileNewShaders();
static void MaybeLoadNewShaders();

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

		int numElems = numElemsForPropType(newProp->type);
		size_t bytesToCopy = sizeof(float) * numElems * newProp->arraySize;
		memcpy(newBuffer + newProp->offset, oldBuffer + oldProp->offset, bytesToCopy);
	}	
}

void ensureConstantBufferSize(size_t size, PropMap* oldProps = nullptr, PropMap* newProps = nullptr) {
	// must have GUARD_UNIFORMS and GUARD_GPU

	auto oldConstantBuffer = constantBuffer;
	auto oldGpuBuffer = gpuBuffer;
	auto oldConstantBufferSize = constantBufferSize;

	constantBuffer = new unsigned char[size];
	gpuBuffer = new unsigned char[size * MAX_GPU_BUFFERS];
	constantBufferSize = size;

    memset(constantBuffer, 0, size);
    memset(gpuBuffer, 0, size * MAX_GPU_BUFFERS);		

	if (oldProps && newProps) {
		copyProps(oldProps, newProps, oldConstantBuffer, constantBuffer);
		for (int i = 0; i < MAX_GPU_BUFFERS; ++i)
			copyProps(oldProps, newProps, oldGpuBuffer + oldConstantBufferSize * i, gpuBuffer + constantBufferSize * i);
	}

	if (oldConstantBuffer) delete[] oldConstantBuffer;
	if (oldGpuBuffer) delete[] oldGpuBuffer;
}



// --------------------------------------------------------------------------
// OnRenderEvent
// This will be called for GL.IssuePluginEvent script calls; eventID will
// be the integer passed to IssuePluginEvent. In this example, we just ignore
// that value.

static void UNITY_INTERFACE_API OnRenderEvent(int uniformIndex)
{
	assert(uniformIndex < MAX_GPU_BUFFERS);

	// Unknown graphics device type? Do nothing.
	if (s_DeviceType == kUnityGfxRendererNull)
		return;

	MaybeCompileNewShaders();
	MaybeLoadNewShaders();

	MyVertex verts[NUM_VERTS] = {
		{  -1.0f, -1.0f,   0.0f, 0.0f, 0.0f },
		{   1.0f,  1.0f,   0.0f, 1.0f, 1.0f },
		{  -1.0,   1.0f,   0.0f, 0.0f, 1.0f },

		{ -1.0f, -1.0f,    0.0f, 0.0f, 0.0f },
		{ 1.0f,  -1.0f,    0.0f, 1.0f, 0.0f },
		{ 1.0,    1.0f ,   0.0f, 1.0f, 1.0f },
	};

	// Some transformation matrices: rotate around Z axis for world
	// matrix, identity view matrix, and identity projection matrix.
	float worldMatrix[16] = {
		1,0,0,0,
		0,1,0,0,
		0,0,1,0,
		0,0,0,1,
	};
	float identityMatrix[16] = {
		1,0,0,0,
		0,1,0,0,
		0,0,1,0,
		0,0,0,1,
	};
	float projectionMatrix[16] = {
		1,0,0,0,
		0,1,0,0,
		0,0,1,0,
		0,0,0,1,
	};

	// Actual functions defined below
	SetDefaultGraphicsState ();
	DoRendering (worldMatrix, identityMatrix, projectionMatrix, verts, uniformIndex);
}

// --------------------------------------------------------------------------
// GetRenderEventFunc, an example function we export which is used to get a rendering event callback function.
extern "C" UnityRenderingEvent UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API GetRenderEventFunc()
{
	return OnRenderEvent;
}



// -------------------------------------------------------------------
// Shared code

#if SUPPORT_D3D11
typedef vector<unsigned char> Buffer;
bool LoadFileIntoBuffer(string fileName, Buffer& data)
{
	FILE* fp;
	fopen_s(&fp, fileName.c_str(), "rb");
	if (fp)
	{
		fseek (fp, 0, SEEK_END);
		int size = ftell (fp);
		fseek (fp, 0, SEEK_SET);
		data.resize(size);

		fread(&data[0], size, 1, fp);

		fclose(fp);

		return true;
	}
	else
	{
		string errorMessage = "Failed to find ";
		errorMessage += fileName;
		DebugLog(errorMessage.c_str());
		return false;
	}
}
#endif


// -------------------------------------------------------------------
//  Direct3D 9 setup/teardown code


#if SUPPORT_D3D9

static IDirect3DDevice9* g_D3D9Device = nullptr;

// A dynamic vertex buffer just to demonstrate how to handle D3D9 device resets.
static IDirect3DVertexBuffer9* g_D3D9DynamicVB;

static void DoEventGraphicsDeviceD3D9(UnityGfxDeviceEventType eventType)
{
	// Create or release a small dynamic vertex buffer depending on the event type.
	switch (eventType) {
	case kUnityGfxDeviceEventInitialize:
		{
			IUnityGraphicsD3D9* d3d9 = s_UnityInterfaces->Get<IUnityGraphicsD3D9>();
			g_D3D9Device = d3d9->GetDevice();
		}
	case kUnityGfxDeviceEventAfterReset:
		// After device is initialized or was just reset, create the VB.
		if (!g_D3D9DynamicVB)
			g_D3D9Device->CreateVertexBuffer (1024, D3DUSAGE_WRITEONLY | D3DUSAGE_DYNAMIC, 0, D3DPOOL_DEFAULT, &g_D3D9DynamicVB, NULL);
		break;
	case kUnityGfxDeviceEventBeforeReset:
	case kUnityGfxDeviceEventShutdown:
		// Before device is reset or being shut down, release the VB.
		SAFE_RELEASE(g_D3D9DynamicVB);
		break;
	}
}

#endif // #if SUPPORT_D3D9



// -------------------------------------------------------------------
//  Direct3D 11 setup/teardown code


#if SUPPORT_D3D11

static ID3D11Device* g_D3D11Device = NULL;

static ID3D11Buffer* d3d11ConstantBuffer = NULL; // constant buffer
static UINT d3d11ConstantBufferSize = 0; // ByteWidth of constant buffer

static ID3D11VertexShader* g_D3D11VertexShader = NULL;
static ID3D11PixelShader* g_D3D11PixelShader = NULL;
static ID3D11InputLayout* g_D3D11InputLayout = NULL;
static ID3D11RasterizerState* g_D3D11RasterState = NULL;
static ID3D11BlendState* g_D3D11BlendState = NULL;
static ID3D11DepthStencilState* g_D3D11DepthState = NULL;
static ID3D11SamplerState* g_D3D11SamplerState = NULL;

static string toString(const WCHAR* wbuf) {
	std::wstring wstr(wbuf);
	string str(wstr.begin(), wstr.end());
	return str;
}

static void DebugHR(HRESULT hr) {
	assert(FAILED(hr));

	std::stringstream ss;
	ss << toString(DXGetErrorString(hr));

	const int maxSize = 1024;
	WCHAR buf[maxSize];
	DXGetErrorDescription(hr, (WCHAR*)&buf, maxSize);

	ss << ": " << toString(buf);

	Debug(ss.str().c_str());
}


static bool DX_CHECK(HRESULT hr) {
	if (FAILED(hr)) {
		DebugHR(hr);
		return false;
	}
	else
		return true;
}



static bool EnsureD3D11ResourcesAreCreated()
{
	if (g_D3D11BlendState)
		return true;
		
	// render states
	D3D11_RASTERIZER_DESC rsdesc;
	memset (&rsdesc, 0, sizeof(rsdesc));
	rsdesc.FillMode = D3D11_FILL_SOLID;
	rsdesc.CullMode = D3D11_CULL_NONE;
	rsdesc.DepthClipEnable = TRUE;
	g_D3D11Device->CreateRasterizerState (&rsdesc, &g_D3D11RasterState);

	D3D11_DEPTH_STENCIL_DESC dsdesc;
	memset (&dsdesc, 0, sizeof(dsdesc));
	dsdesc.DepthEnable = TRUE;
	dsdesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
	dsdesc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
	DX_CHECK(g_D3D11Device->CreateDepthStencilState (&dsdesc, &g_D3D11DepthState));

	D3D11_SAMPLER_DESC samplerDesc;
	memset(&samplerDesc, 0, sizeof(samplerDesc));
	samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
	samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
	samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
	DX_CHECK(g_D3D11Device->CreateSamplerState(&samplerDesc, &g_D3D11SamplerState));

	D3D11_BLEND_DESC bdesc;
	memset (&bdesc, 0, sizeof(bdesc));
	bdesc.RenderTarget[0].BlendEnable = FALSE;
	bdesc.RenderTarget[0].RenderTargetWriteMask = 0xF;
	DX_CHECK(g_D3D11Device->CreateBlendState (&bdesc, &g_D3D11BlendState));

	return true;
}

static void clearResourceViews() {
	for (size_t i = 0; i < resourceViews.size(); ++i)
		SAFE_RELEASE(resourceViews[i]);
	resourceViews.clear();
}

static void ReleaseD3D11Resources() {
	ID3D11DeviceContext* ctx = NULL;
	g_D3D11Device->GetImmediateContext(&ctx);
	ctx->VSSetShader(NULL, NULL, 0);
	ctx->PSSetShader(NULL, NULL, 0);
	ctx->PSSetConstantBuffers(0, 0, NULL);
	ctx->PSSetShaderResources(0, 0, nullptr);
	ctx->PSSetSamplers(0, 0, nullptr);
	ctx->Release();

	Debug("Releasing D3D11Resources...");
	SAFE_RELEASE(d3d11ConstantBuffer);
	SAFE_RELEASE(g_D3D11VertexShader);
	SAFE_RELEASE(g_D3D11PixelShader);
	SAFE_RELEASE(g_D3D11InputLayout);
	SAFE_RELEASE(g_D3D11RasterState);
	SAFE_RELEASE(g_D3D11BlendState);
	SAFE_RELEASE(g_D3D11DepthState);
	SAFE_RELEASE(g_D3D11SamplerState);
	Debug("... done releasing D3D11Resources.");
}


static void DoEventGraphicsDeviceD3D11(UnityGfxDeviceEventType eventType)
{
	if (eventType == kUnityGfxDeviceEventInitialize) {
		IUnityGraphicsD3D11* d3d11 = s_UnityInterfaces->Get<IUnityGraphicsD3D11>();
		g_D3D11Device = d3d11->GetDevice();		
		EnsureD3D11ResourcesAreCreated();
	} else if (eventType == kUnityGfxDeviceEventShutdown) {
		ReleaseD3D11Resources();
	}
}

static int roundUp(int numToRound, int multiple) {
	assert(multiple);
	int isPositive = (int)(numToRound >= 0);
	return ((numToRound + isPositive * (multiple - 1)) / multiple) * multiple;
}

static ID3D11ShaderReflection* shaderReflector(ID3DBlob* shader) {
	ID3D11ShaderReflection* reflector = nullptr;

	HRESULT hr = D3DReflect(
		shader->GetBufferPointer(),
		shader->GetBufferSize(),
		IID_ID3D11ShaderReflection,
		(void**)&reflector);

	if (FAILED(hr)) {
		DebugHR(hr);
		return nullptr;
	}
	
	return reflector;
}

static void constantBufferReflect(ID3DBlob* shader) {	
	assert(g_D3D11Device);
	auto pReflector = shaderReflector(shader);
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

	stats.instructionCount = desc.InstructionCount;

	{
		GUARD_TEXTURES;
		clearResourceViews();
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
	if (desc.ConstantBuffers >= 2)
		Debug("WARNING: more than one D3D11 constant buffer, not implemented!");
	
	{
		GUARD_UNIFORMS;
		SAFE_RELEASE(d3d11ConstantBuffer);
		PropMap oldProps = shaderProps;
		shaderProps.clear();		
		d3d11ConstantBufferSize = 0;

		if (desc.ConstantBuffers > 0) {
			D3D11_SHADER_BUFFER_DESC Description;
			ID3D11ShaderReflectionConstantBuffer* pConstBuffer = pReflector->GetConstantBufferByIndex(0);
			pConstBuffer->GetDesc(&Description);
			for (UINT j = 0; j < Description.Variables; j++) {
				ID3D11ShaderReflectionVariable* pVariable = pConstBuffer->GetVariableByIndex(j);
				D3D11_SHADER_VARIABLE_DESC var_desc;
				pVariable->GetDesc(&var_desc);


				// mark the prop's name, size and offset
				propForNameSizeOffset(var_desc.Name, var_desc.Size, var_desc.StartOffset);
				d3d11ConstantBufferSize = max(d3d11ConstantBufferSize, var_desc.StartOffset + var_desc.Size);
			}

			if (d3d11ConstantBufferSize > 0) {
				d3d11ConstantBufferSize = roundUp(d3d11ConstantBufferSize, 16);

				// constant buffer
				D3D11_BUFFER_DESC bufdesc;
				memset(&bufdesc, 0, sizeof(bufdesc));
				bufdesc.Usage = D3D11_USAGE_DEFAULT;
				bufdesc.ByteWidth = d3d11ConstantBufferSize;
				bufdesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
				bufdesc.CPUAccessFlags = 0;//D3D11_CPU_ACCESS_WRITE;

				HRESULT hr = g_D3D11Device->CreateBuffer(&bufdesc, NULL, &d3d11ConstantBuffer);
				if (FAILED(hr)) {
					Debug("ERROR: could not create constant buffer:");
					DebugHR(hr);
				}
			}
		}
		GUARD_GPU;
		ensureConstantBufferSize(d3d11ConstantBufferSize, &oldProps, &shaderProps);
	}	

	pReflector->Release();
}


#endif // #if SUPPORT_D3D11

#define isOpenGLDevice(d) (d == kUnityGfxRendererOpenGLES20 || \
    d == kUnityGfxRendererOpenGLES30 || \
    d == kUnityGfxRendererOpenGLCore || \
    d == kUnityGfxRendererOpenGL)

static bool getLatestShader(ShaderSource& shaderSource) {
	bool didRead = false;
	while (shaderSourceQueue.read(shaderSource))
		didRead = true;
	return didRead;
}

static void MaybeLoadNewShaders() {
#ifdef SUPPORT_D3D
	CompileTaskOutput compileTaskOutput{ Fragment, nullptr, -1 };
	while (shaderCompilerOutputs.read(compileTaskOutput)) {
		ID3DBlob* shaderBlob = compileTaskOutput.shaderBlob;
		assert(shaderBlob);

		auto buf = shaderBlob->GetBufferPointer();
		auto bufSize = shaderBlob->GetBufferSize();
		assert(buf && bufSize > 0);

		switch (compileTaskOutput.shaderType) {
		case Fragment: {
			constantBufferReflect(shaderBlob);
			SAFE_RELEASE(g_D3D11PixelShader);
			HRESULT hr = g_D3D11Device->CreatePixelShader(buf, bufSize, nullptr, &g_D3D11PixelShader);
			if (FAILED(hr)) { Debug("CreatePixelShader failed\n"); DebugHR(hr); }
			break;
		}
		case Vertex: {
			SAFE_RELEASE(g_D3D11VertexShader);
			HRESULT hr = g_D3D11Device->CreateVertexShader(buf, bufSize, nullptr, &g_D3D11VertexShader);
			if (FAILED(hr)) { Debug("CreateVertexShader failed"); DebugHR(hr); }
			break;
		}
		case Compute: {
			auto computeShader = findComputeShader(compileTaskOutput.shaderId);
			if (computeShader) {
				HRESULT hr = g_D3D11Device->CreateComputeShader(buf, bufSize, nullptr, &computeShader->d3d11ComputeShader);
				if (FAILED(hr)) { Debug("CreateComputeShader failed"); DebugHR(hr); }
			}
			break;
		}
		default:
			assert(false);
		}

		SAFE_RELEASE(compileTaskOutput.shaderBlob);
	}
#endif
}

static void MaybeCompileNewShaders() {
    ShaderSource shaderSource;
	if (!getLatestShader(shaderSource))
		return;

	//StopWatch loadingShaders;
	
#if SUPPORT_OPENGL_UNIFIED
    if (isOpenGLDevice(s_DeviceType)) {
        bool needsUpdate = false;
        if (shaderSource.fragShader.size() > 0) {
          GLuint newFrag = loadShader(GL_FRAGMENT_SHADER, shaderSource.fragShader.c_str(), writeDebugFiles ? "/Users/kevin/Desktop/frag.glsl" : nullptr);
          if (newFrag) {
              if (g_FShader)
                  glDeleteShader(g_FShader);
              g_FShader = newFrag;
              needsUpdate = true;
          }
        }
        
        if (shaderSource.vertShader.size() > 0) {
          GLuint newVert = loadShader(GL_VERTEX_SHADER, shaderSource.vertShader.c_str(), writeDebugFiles ? "/Users/kevin/Desktop/vert.glsl" : nullptr);
          if (newVert) {
              if (g_VProg)
                  glDeleteShader(g_VProg);
              g_VProg = newVert;
              needsUpdate = true;
          }
        }

        if (needsUpdate) {
          LinkProgram();
          {
            GUARD_UNIFORMS;
			shaderProps.clear();
            if (g_Program)
              DiscoverGLUniforms(g_Program);
          }
          printOpenGLError();
        }
    }
#endif

	//DebugSS("loading new shaders took " << loadingShaders.ElapsedMs() << " ms");

}


// -------------------------------------------------------------------
// Direct3D 12 setup/teardown code


#if SUPPORT_D3D12
const UINT kNodeMask = 0;

static IUnityGraphicsD3D12* s_D3D12 = NULL;
static ID3D12Resource* s_D3D12Upload = NULL;

static ID3D12CommandAllocator* s_D3D12CmdAlloc = NULL;
static ID3D12GraphicsCommandList* s_D3D12CmdList = NULL;

static ID3D12Fence* s_D3D12Fence = NULL;
static UINT64 s_D3D12FenceValue = 1;
static HANDLE s_D3D12Event = NULL;

ID3D12Resource* GetD3D12UploadResource(UINT64 size)
{
	if (s_D3D12Upload)
	{
		D3D12_RESOURCE_DESC desc = s_D3D12Upload->GetDesc();
		if (desc.Width == size)
			return s_D3D12Upload;
		else
			s_D3D12Upload->Release();
	}

	// Texture upload buffer
	D3D12_HEAP_PROPERTIES heapProps = {};
	heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
	heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
	heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
	heapProps.CreationNodeMask = kNodeMask;
	heapProps.VisibleNodeMask = kNodeMask;

	D3D12_RESOURCE_DESC heapDesc = {};
	heapDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	heapDesc.Alignment = 0;
	heapDesc.Width = size;
	heapDesc.Height = 1;
	heapDesc.DepthOrArraySize = 1;
	heapDesc.MipLevels = 1;
	heapDesc.Format = DXGI_FORMAT_UNKNOWN;
	heapDesc.SampleDesc.Count = 1;
	heapDesc.SampleDesc.Quality = 0;
	heapDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	heapDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

	ID3D12Device* device = s_D3D12->GetDevice();
	HRESULT hr = device->CreateCommittedResource(
			&heapProps,
			D3D12_HEAP_FLAG_NONE,
			&heapDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr,
			IID_PPV_ARGS(&s_D3D12Upload));
	if (FAILED(hr)) DebugLog("Failed to CreateCommittedResource.\n");

	return s_D3D12Upload;
}

static void CreateD3D12Resources()
{
	ID3D12Device* device = s_D3D12->GetDevice();

	HRESULT hr = E_FAIL;

	// Command list
	hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s_D3D12CmdAlloc));
	if (FAILED(hr)) DebugLog("Failed to CreateCommandAllocator.\n");
	hr = device->CreateCommandList(kNodeMask, D3D12_COMMAND_LIST_TYPE_DIRECT, s_D3D12CmdAlloc, nullptr, IID_PPV_ARGS(&s_D3D12CmdList));
	if (FAILED(hr)) DebugLog("Failed to CreateCommandList.\n");
	s_D3D12CmdList->Close();

	// Fence
	s_D3D12FenceValue = 1;
	device->CreateFence(s_D3D12FenceValue, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s_D3D12Fence));
	if (FAILED(hr)) DebugLog("Failed to CreateFence.\n");
	s_D3D12Event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
}

static void ReleaseD3D12Resources()
{
	SAFE_RELEASE(s_D3D12Upload);
	
	if (s_D3D12Event)
		CloseHandle(s_D3D12Event);

	SAFE_RELEASE(s_D3D12Fence);
	SAFE_RELEASE(s_D3D12CmdList);
	SAFE_RELEASE(s_D3D12CmdAlloc);
}

static void DoEventGraphicsDeviceD3D12(UnityGfxDeviceEventType eventType)
{
	if (eventType == kUnityGfxDeviceEventInitialize)
	{
		s_D3D12 = s_UnityInterfaces->Get<IUnityGraphicsD3D12>();
		CreateD3D12Resources();
	}
	else if (eventType == kUnityGfxDeviceEventShutdown)
	{
		ReleaseD3D12Resources();
	}
}
#endif // #if SUPPORT_D3D12



// -------------------------------------------------------------------
// OpenGL ES / Core setup/teardown code


#if SUPPORT_OPENGL_UNIFIED
GLuint loadShader(GLenum type, const char *shaderSrc, const char* debugOutPath)
{
   GLuint shader = glCreateShader(type);
   if (shader == 0) {
       Debug("could not create shader object");
   	   return 0;
   }
    
   if (debugOutPath)
	     writeTextToFile(debugOutPath, shaderSrc);
    
   //if (verbose)
      //DebugSS("GLSL Version " << (const char*)glGetString(GL_SHADING_LANGUAGE_VERSION));
    

   if (type == GL_FRAGMENT_SHADER)
     stats.compileState = CompileState::Compiling;
   glShaderSource(shader, 1, &shaderSrc, NULL);
   glCompileShader(shader);

   GLint compiled;
   glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
   if (!compiled) {
       GLint infoLen = 0;
       glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &infoLen);
       Debug("error compiling glsl shader:");
       if (type == GL_FRAGMENT_SHADER)
         stats.compileState = CompileState::Error;
       if (infoLen > 1) {
           char* infoLog = (char*)malloc (sizeof(char) * infoLen);
           if (infoLog) {
               glGetShaderInfoLog(shader, infoLen, NULL, infoLog);
               Debug(infoLog);
               free(infoLog);
           }
       }

       glDeleteShader(shader);
       return 0;
   }

   return shader;
}

static void LinkProgram() {
    
    GLuint program = glCreateProgram();
    assert(program > 0);
    //glBindAttribLocation(program, ATTRIB_POSITION, "xlat_attrib_POSITION");
    //glBindAttribLocation(program, ATTRIB_COLOR, "xlat_attrib_COLOR");
    //glBindAttribLocation(program, ATTRIB_UV, "xlat_attrib_TEXCOORD0");
    glAttachShader(program, g_VProg);
    glAttachShader(program, g_FShader);
#if SUPPORT_OPENGL_CORE
    if(s_DeviceType == kUnityGfxRendererOpenGLCore)
        glBindFragDataLocationEXT(program, 0, "fragColor");
#endif
    glLinkProgram(program);
    
    
    
    GLint status = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &status);
    
    if (status == GL_TRUE) {
        if (g_Program)
            glDeleteProgram(g_Program);
        
        g_Program = program;
        stats.compileState = CompileState::Success;
    } else {
        Debug("failure linking program:");
        stats.compileState = CompileState::Error;
        
        GLint infoLen = 0;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &infoLen);
        if (infoLen > 1) {
            char* infoLog = (char*)malloc (sizeof(char) * infoLen);
            glGetProgramInfoLog(program, infoLen, NULL, infoLog);
            Debug(infoLog);
            free(infoLog);
        }
        
    }
    
}

static void DoEventGraphicsDeviceGLUnified(UnityGfxDeviceEventType eventType)
{
	if (eventType == kUnityGfxDeviceEventInitialize)
	{
	}
	else if (eventType == kUnityGfxDeviceEventShutdown)
	{
	}
}
#endif



// --------------------------------------------------------------------------
// SetDefaultGraphicsState
//
// Helper function to setup some "sane" graphics state. Rendering state
// upon call into our plugin can be almost completely arbitrary depending
// on what was rendered in Unity before.
// Before calling into the plugin, Unity will set shaders to null,
// and will unbind most of "current" objects (e.g. VBOs in OpenGL case).
//
// Here, we set culling off, lighting off, alpha blend & test off, Z
// comparison to less equal, and Z writes off.

static void SetDefaultGraphicsState ()
{
	#if SUPPORT_D3D9
	// D3D9 case
	if (s_DeviceType == kUnityGfxRendererD3D9)
	{
		g_D3D9Device->SetRenderState (D3DRS_CULLMODE, D3DCULL_NONE);
		g_D3D9Device->SetRenderState (D3DRS_LIGHTING, FALSE);
		g_D3D9Device->SetRenderState (D3DRS_ALPHABLENDENABLE, FALSE);
		g_D3D9Device->SetRenderState (D3DRS_ALPHATESTENABLE, FALSE);
		g_D3D9Device->SetRenderState (D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
		g_D3D9Device->SetRenderState (D3DRS_ZWRITEENABLE, FALSE);
	}
	#endif


	#if SUPPORT_D3D11
	// D3D11 case
	if (s_DeviceType == kUnityGfxRendererD3D11)
	{
		ID3D11DeviceContext* ctx = NULL;
		g_D3D11Device->GetImmediateContext (&ctx);
		ctx->OMSetDepthStencilState (g_D3D11DepthState, 0);
		ctx->RSSetState (g_D3D11RasterState);
		ctx->OMSetBlendState (g_D3D11BlendState, NULL, 0xFFFFFFFF);
		ctx->Release();
	}
	#endif


	#if SUPPORT_D3D12
	// D3D12 case
	if (s_DeviceType == kUnityGfxRendererD3D12)
	{
		// Stateless. Nothing to do.
	}
	#endif

	#if SUPPORT_OPENGL_UNIFIED
	// OpenGL ES / core case
	if (s_DeviceType == kUnityGfxRendererOpenGLES20 ||
		s_DeviceType == kUnityGfxRendererOpenGLES30 ||
		s_DeviceType == kUnityGfxRendererOpenGLCore)
	{
		glDisable(GL_CULL_FACE);
		glDisable(GL_BLEND);
		glDepthFunc(GL_LEQUAL);
		glEnable(GL_DEPTH_TEST);
		glDepthMask(GL_TRUE);

		assert(glGetError() == GL_NO_ERROR);
	}
	#endif
}

#if SUPPORT_D3D11
static void setupPendingResourcesD3D11(ID3D11DeviceContext* ctx);
#endif

static void DoRendering (const float* worldMatrix, const float* identityMatrix, float* projectionMatrix, const MyVertex* verts, int uniformIndex)
{
	// Does actual rendering of a simple triangle
	#if SUPPORT_D3D9
	// D3D9 case
	if (s_DeviceType == kUnityGfxRendererD3D9)
	{
		// Transformation matrices
		g_D3D9Device->SetTransform (D3DTS_WORLD, (const D3DMATRIX*)worldMatrix);
		g_D3D9Device->SetTransform (D3DTS_VIEW, (const D3DMATRIX*)identityMatrix);
		g_D3D9Device->SetTransform (D3DTS_PROJECTION, (const D3DMATRIX*)projectionMatrix);

		// Vertex layout
		g_D3D9Device->SetFVF (D3DFVF_XYZ|D3DFVF_DIFFUSE);

		// Texture stage states to output vertex color
		g_D3D9Device->SetTextureStageState (0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
		g_D3D9Device->SetTextureStageState (0, D3DTSS_COLORARG1, D3DTA_CURRENT);
		g_D3D9Device->SetTextureStageState (0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
		g_D3D9Device->SetTextureStageState (0, D3DTSS_ALPHAARG1, D3DTA_CURRENT);
		g_D3D9Device->SetTextureStageState (1, D3DTSS_COLOROP, D3DTOP_DISABLE);
		g_D3D9Device->SetTextureStageState (1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

		// Copy vertex data into our small dynamic vertex buffer. We could have used
		// DrawPrimitiveUP just fine as well.
		void* vbPtr;
		g_D3D9DynamicVB->Lock (0, 0, &vbPtr, D3DLOCK_DISCARD);
		memcpy (vbPtr, verts, sizeof(verts[0])*NUM_VERTS);
		g_D3D9DynamicVB->Unlock ();
		g_D3D9Device->SetStreamSource (0, g_D3D9DynamicVB, 0, sizeof(MyVertex));

		// Draw!
		g_D3D9Device->DrawPrimitive (D3DPT_TRIANGLELIST, 0, 1);
	}
	#endif


	#if SUPPORT_D3D11
	// D3D11 case
	if (s_DeviceType == kUnityGfxRendererD3D11 &&
		EnsureD3D11ResourcesAreCreated() &&
		g_D3D11VertexShader &&
		g_D3D11PixelShader) {

		ID3D11DeviceContext* ctx = NULL;
		g_D3D11Device->GetImmediateContext (&ctx);
		setupPendingResourcesD3D11(ctx);
		if (updateUniforms)
			updateUniformsD3D11(ctx, uniformIndex);	
		ctx->VSSetShader (g_D3D11VertexShader, NULL, 0);		
		ctx->PSSetShader (g_D3D11PixelShader, NULL, 0);
		ctx->PSSetConstantBuffers(0, 1, &d3d11ConstantBuffer);
		ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
		ctx->Draw(4, 0);
		ctx->Release();
	}
	#endif
	
	#if SUPPORT_D3D12
	// D3D12 case
	if (s_DeviceType == kUnityGfxRendererD3D12)
	{
		ID3D12Device* device = s_D3D12->GetDevice();
		ID3D12CommandQueue* queue = s_D3D12->GetCommandQueue();

		// Wait on the previous job (example only - simplifies resource management)
		if (s_D3D12Fence->GetCompletedValue() < s_D3D12FenceValue)
		{
			s_D3D12Fence->SetEventOnCompletion(s_D3D12FenceValue, s_D3D12Event);
			WaitForSingleObject(s_D3D12Event, INFINITE);
		}
		
		// Insert fence
		++s_D3D12FenceValue;
		queue->Signal(s_D3D12Fence, s_D3D12FenceValue);
	}
	#endif

	
	#if SUPPORT_OPENGL_UNIFIED
	#define BUFFER_OFFSET(i) ((char *)NULL + (i))

	// OpenGL ES / core case
	if (s_DeviceType == kUnityGfxRendererOpenGLES20 ||
		s_DeviceType == kUnityGfxRendererOpenGLES30 ||
		s_DeviceType == kUnityGfxRendererOpenGLCore)
	{
		assert(glGetError() == GL_NO_ERROR); // Make sure no OpenGL error happen before starting rendering
        if (g_Program == 0)
            return;

		glUseProgram(g_Program);
        updateUniformsGL();
        printOpenGLError();

		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        printOpenGLError();
	}
	#endif
}



static bool hasProp(const char* name) {
    return shaderProps.find(name) != shaderProps.end();
}


static ShaderProp* _lookupPropByName(const char* name) {
	auto i = shaderProps.find(name);
	return i != shaderProps.end() ? i->second : nullptr;
}


#define SAFE_DELETE(x) do { if (x) delete (x); } while(0); 

static ShaderProp* propForNameSizeOffset(const char* name, uint16_t size, uint16_t offset) {
	auto prop = _lookupPropByName(name);
	if (!prop || prop->size != size || prop->offset != offset) {
		SAFE_DELETE(prop);
		prop = shaderProps[name] = new ShaderProp(ShaderProp::typeForSize(size), name);
		prop->size = size;
		prop->offset = offset;
	} else {
		assert(prop->size == size);
		assert(prop->offset == offset);
	}

	return prop;
}

static ShaderProp* propForName(const char* name, PropType type) {
    auto prop = _lookupPropByName(name);
	if (!prop || prop->type != type) {
		SAFE_DELETE(prop);
		prop = shaderProps[name] = new ShaderProp(type, name);
	}
    assert(prop->type == type);    
    return prop;
}

static void printUniforms() {
	GUARD_UNIFORMS;

    std::stringstream ss;
    for (auto i = shaderProps.begin(); i != shaderProps.end(); ++i) {
        auto prop = i->second;
        ss << prop->name << " ";
#if SUPPORT_OPENGL_UNIFIED
        if (prop->uniformIndex == ShaderProp::UNIFORM_INVALID)
            ss << "(INVALID) ";
#endif
#if SUPPORT_D3D11
		ss << "(offset: " << prop->offset << ", size: " << prop->size << ") ";
#endif

        switch (prop->type) {
            case Float:
                ss << prop->value(0); break;
            case Vector2:
                ss << prop->value(0) << " " << prop->value(1); break;
            case Vector3:
                ss << prop->value(0) << " " << prop->value(1) << " " << prop->value(2); break;
            case Vector4:
            case Matrix:
				ss << prop->value(0) << " " << prop->value(1) << " " << prop->value(2) << " " << prop->value(3); break;
            default:
                assert(false);
        }
        ss << "\n";
    }
    std::string s(ss.str());
    Debug(s.c_str());
}


static void submitUniforms(int uniformIndex) {
	GUARD_GPU;
	GUARD_UNIFORMS;
	assert(uniformIndex < MAX_GPU_BUFFERS);
	if (gpuBuffer && constantBuffer) {
		memcpy(gpuBuffer + constantBufferSize * uniformIndex, constantBuffer, constantBufferSize);
	}
}

#if SUPPORT_OPENGL_UNIFIED
static void DiscoverGLUniforms(GLuint program) {
  int maxNameLength = 0;
  glGetProgramiv(program, GL_ACTIVE_UNIFORM_MAX_LENGTH, &maxNameLength);
  if (maxNameLength == 0) {
    Debug("max name length was 0");
    return;
  }

  char* name = new char[maxNameLength + 1];

  int numUniforms = 0;
  glGetProgramiv(program, GL_ACTIVE_UNIFORMS, &numUniforms);
  int offset = 0;
  if (!printOpenGLError()) {
      int textureUnit = 0;
      textureUnits.clear();
      uniformLocs.clear();

    for (int i = 0; i < numUniforms; i++) {
      int nameLength = 0;
      int arraysize = 0;
      GLenum type;
      glGetActiveUniform(program, i, maxNameLength, &nameLength, &arraysize, &type, name);
        if (arraysize > 1) {
            string namestr(name);
            size_t s = namestr.size();
            if (s > 3 && namestr[s-1] == ']' && namestr[s-2] == '0' && namestr[s-3] == '[') {
                namestr.resize(s - 3);
                strncpy(name, namestr.c_str(), s - 3);
            }
        }
      assert(arraysize > 0);
        int size = 0;
        PropType propType;
        
        switch (type) {
            case GL_FLOAT:
                size = 1 * sizeof(float) * arraysize;
                propType = Float;
                break;
            case GL_FLOAT_VEC2:
                size = 2 * sizeof(float) * arraysize;
                propType = Vector2;
                break;
            case GL_FLOAT_VEC3:
                size = 3 * sizeof(float) * arraysize;
                propType = Vector3;
                break;
            case GL_FLOAT_VEC4:
                size = 4 * sizeof(float) * arraysize;
                propType = Vector4;
                break;
            case GL_FLOAT_MAT4:
                size = 16 * sizeof(float) * arraysize;
                propType = Matrix;
                break;
            case GL_SAMPLER_2D: {
                // assign texture units in the order we see them here
                textureUnits[name] = textureUnit++;
                uniformLocs.push_back(glGetUniformLocation(g_Program, name));
                continue; // don't make a prop
            }
            default:
                const char* typeName = getGLTypeName(type);
                if (typeName == nullptr) typeName = "unknown";
                DebugSS("unknown gl type " << typeName);
                assert(false);
                continue;
        }
        
        //DebugSS("uniform " << name << " with size " << size << " at offset " << offset);
        auto prop = propForName(name, propType);
        prop->arraySize = arraysize;
        prop->size = size;
        prop->offset = offset;
        prop->uniformIndex = glGetUniformLocation(program, name);

      printOpenGLError();
      offset += size;
    }
      
      textureIDs.clear();
      for (int i = 0; i < textureUnit; ++i)
          textureIDs.push_back(0);
  }

  delete [] name;
  ensureConstantBufferSize(offset);
}
#endif

#if SUPPORT_D3D11
static void updateUniformsD3D11(ID3D11DeviceContext* ctx, int uniformIndex) {
	GUARD_GPU;
	assert(uniformIndex < MAX_GPU_BUFFERS);
	if (d3d11ConstantBuffer && d3d11ConstantBufferSize > 0) {
		ctx->UpdateSubresource(d3d11ConstantBuffer, 0, 0, gpuBuffer + constantBufferSize * uniformIndex, 0, 0);
	}
}

static void setupPendingResourcesD3D11(ID3D11DeviceContext* ctx) {
	GUARD_TEXTURES;

	for (size_t i = 0; i < pendingResources.size(); ++i) {
		auto resource = pendingResources[i].first;
		auto index = pendingResources[i].second;

		auto resourceView = resourceViews[index];

		if (resourceView != nullptr) {
			SAFE_RELEASE(resourceView);
			resourceView = nullptr;
		}

		if (resource) {
			HRESULT hr = g_D3D11Device->CreateShaderResourceView(resource, nullptr, &resourceView);
			if (FAILED(hr)) {
				Debug("Could not CreateShaderResourceView");
				DebugHR(hr);
				resourceView = nullptr;
			}

			SAFE_RELEASE(resource);
		}

		resourceViews[index] = resourceView;
	}

	pendingResources.clear();


	ctx->PSSetShaderResources(0, (UINT)resourceViews.size(), &resourceViews[0]);

	// TODO: is the number of samplers equal to the number of resource views? no idea
	vector<ID3D11SamplerState*> samplers;
	assert(g_D3D11SamplerState);
	const UINT numSamplers = (UINT)resourceViews.size();
	for (UINT i = 0; i < numSamplers; ++i)
		samplers.push_back(g_D3D11SamplerState);
	ctx->PSSetSamplers(0, numSamplers, &samplers[0]);
}
#endif

Shader::Shader(int id_)
	: id(id_)
#if SUPPORT_D3D11
	, d3d11ComputeShader(nullptr)
#endif
{}

Shader::~Shader() {
	source = "";
	entryPoint = "";
	id = -1;
#if SUPPORT_D3D11
	SAFE_RELEASE(d3d11ComputeShader);
#endif
}

void Shader::Dispatch(int threadGroupCountX, int threadGroupCountY, int threadGroupCountZ) {
	if (!IsReady()) {
		DebugSS("ComputeShader not IsReady(), cannot dispatch");
		return;
	}
    
#if SUPPORT_D3D
    

	ID3D11DeviceContext* ctx = nullptr;
	g_D3D11Device->GetImmediateContext(&ctx);
	if (!ctx) {
		DebugSS("Could not obtain an immediate context");
		return;
	}

	ID3D11UnorderedAccessView* ppUAViewNULL[1] = { nullptr };
    ID3D11ShaderResourceView* ppSRVNULL[2] = { nullptr, nullptr };

    ctx->CSSetShader(d3d11ComputeShader, nullptr, 0);
    //ctx->CSSetShaderResources(0, 1, &m_srcDataGPUBufferView);
    //ctx->CSSetUnorderedAccessViews(0, 1, &m_destDataGPUBufferView, nullptr);

    ctx->Dispatch(threadGroupCountX, threadGroupCountY, threadGroupCountZ);

    ctx->CSSetShader(nullptr, nullptr, 0);
    ctx->CSSetUnorderedAccessViews(0, 1, ppUAViewNULL, nullptr);
    ctx->CSSetShaderResources(0, 2, ppSRVNULL);

	ctx->Release();
#endif
}

void Shader::SetSource(const char* source, const char* entryPoint) {
	assert(source && entryPoint);
	this->source = source;
	this->entryPoint = entryPoint;

	CompileTask task(Compute, source, entryPoint);
	task.srcName = shaderIncludePath + "\\compute.hlsl";
	task.id = id;
	submitCompileTasks(vector<CompileTask> { task }, true);
}

ComputeBuffer::ComputeBuffer(int id_)
	: id(id_)
{}

ComputeBuffer::~ComputeBuffer() {
	id = -1;
}


std::map<int, Shader*> computeShaders;
std::map<int, ComputeBuffer*> computeBuffers;

int nextComputeShaderId = 0;
int nextComputeBufferId = 0;

static Shader* findComputeShader(int id) {
	auto iter = computeShaders.find(id);
	if (iter == computeShaders.end()) {
		DebugSS("no compute shader for id " << id);
		return nullptr;
	}
	return iter->second;
}

static ComputeBuffer* findComputeBuffer(int id) {
	auto iter = computeBuffers.find(id);
	if (iter == computeBuffers.end()) {
		DebugSS("no compute buffer for id " << id);
		return nullptr;
	}
	return iter->second;
}

#if SUPPORT_OPENGL_UNIFIED || SUPPORT_OPENGL_CORE
static void updateUniformsGL() {
	assert(isOpenGLDevice(s_DeviceType));

    if (g_Program == 0)
        return;    
    
    for (size_t textureUnit = 0; textureUnit < textureIDs.size(); ++textureUnit) {
        auto uniformLoc = uniformLocs[textureUnit];
        auto textureID = textureIDs[textureUnit];
        if (textureID < 1)
            continue;
		        
		GLenum activeTexture = GL_TEXTURE0 + (GLenum)textureUnit;
        glActiveTexture(activeTexture);
        printOpenGLError();
        glBindTexture(GL_TEXTURE_2D, textureID);
        if (printOpenGLError()) { DebugSS("Error binding texture with id " << textureID); }
        glUniform1i(uniformLoc, (GLint)textureUnit);
        printOpenGLError();

        int w = 0, h = 0;
        int miplevel = 0;
        glGetTexLevelParameteriv(GL_TEXTURE_2D, miplevel, GL_TEXTURE_WIDTH, &w);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, miplevel, GL_TEXTURE_HEIGHT, &h);
        printOpenGLError();

    }

    std::stringstream errors;
    
    for (auto i = shaderProps.begin(); i != shaderProps.end(); i++) {
        auto prop = i->second;
        
        if (prop->uniformIndex == ShaderProp::UNIFORM_UNSET || prop->uniformIndex == prop->UNIFORM_INVALID) {
            //errors << "invalid shader variable " << prop->name << "\n";
            continue;
        }
		switch (prop->type) {
		case Float:
			glUniform1f(prop->uniformIndex, prop->value(0));
			break;
		case Vector2:
			glUniform2f(prop->uniformIndex, prop->value(0), prop->value(1));
			break;
		case Vector3:
            glUniform3f(prop->uniformIndex, prop->value(0), prop->value(1), prop->value(2));
			break;
		case Vector4:
			glUniform4f(prop->uniformIndex, prop->value(0), prop->value(1), prop->value(2), prop->value(3));
			break;
		case Matrix: {
			const int numElements = 1;
			const bool transpose = GL_FALSE;
			glUniformMatrix4fv(prop->uniformIndex, numElements, transpose, ((float*)(constantBuffer + prop->offset)));
			break;
		}
		default:
			assert(false);
		}

    string errorStr(errors.str());
    if (errorStr.size()) Debug(errorStr.c_str());
            
        if (printOpenGLError())
            DebugSS("error setting uniform " << prop->name << " with type " << prop->typeString() << " and uniform index " << prop->uniformIndex);
   
    }
}
#endif

extern "C" {

UNITY_INTERFACE_EXPORT void SetPluginCallback(PluginCallback fp) {
	GUARD_CALLBACK;
	PluginCallbackFunc = fp;
}

UNITY_INTERFACE_EXPORT void SetDebugFunction(FuncPtr fp) {
	GUARD_DEBUG;
	_DebugFunc = fp;
}

UNITY_INTERFACE_EXPORT void ClearDebugFunction() {
	GUARD_DEBUG;
	_DebugFunc = nullptr;
}
    
static void setproparray(const char* name, PropType type, const char* methodName, float* value, int numFloats) {
    GUARD_UNIFORMS;

    if (!constantBuffer)
        return;

    auto prop = propForName(name, type);
    if (!prop) {
      if (verbose) DebugSS("no prop named " << name);
      return;
    }
    
    int numElems = numElemsForPropType(type);
    
    if (numFloats < prop->arraySize * numElems) {
        DebugSS("not enough elements in " << methodName << " array (expected " << (prop->arraySize * numElems) << " but got " << numFloats << ")");
    } else {
        size_t bytesToCopy = (size_t)fmin(sizeof(float) * numFloats, sizeof(float) * numElems * prop->arraySize);
        memcpy(constantBuffer + prop->offset, value, bytesToCopy);
    }
}

UNITY_INTERFACE_EXPORT void SetFloatArray(const char* name, float* value, int numFloats) {
    setproparray(name, PropType::Float, __func__, value, numFloats);
}
    
UNITY_INTERFACE_EXPORT void SetMatrixArray(const char* name, float* value, int numFloats) {
    setproparray(name, PropType::Matrix, __func__, value, numFloats);
}
    
UNITY_INTERFACE_EXPORT void SetVectorArray(const char* name, float* value, int numFloats) {
    setproparray(name, PropType::Vector4, __func__, value, numFloats);
}


UNITY_INTERFACE_EXPORT void SetFloat(const char* name, float value) {
    setproparray(name, PropType::Float, __func__, &value, 1);
}


UNITY_INTERFACE_EXPORT void SetMatrix(const char* name, float* value) {
    setproparray(name, PropType::Matrix, __func__, value, 16);
}
    
UNITY_INTERFACE_EXPORT void SetColor(const char* name, float* value) {
    setproparray(name, PropType::Vector4, __func__, value, 4);
}
    

UNITY_INTERFACE_EXPORT void SetVector4(const char* name, float* value) {
    setproparray(name, PropType::Vector4, __func__, value, 4);
}

static std::map<int, void*> texturePointers;

static void SetTexture(const char* name, void* nativeTexturePointer);

UNITY_INTERFACE_EXPORT bool SetTextureID(const char* name, int id) {
	if (!id) {
		SetTexture(name, nullptr);
		return false;
	}
	
	void* nativeTexturePointer = nullptr;
	{
		GUARD_TEXTURES;

		auto iter = texturePointers.find(id);
		if (iter == texturePointers.end())
			return true; // needs pointer

		nativeTexturePointer = iter->second;
		assert(nativeTexturePointer);
	}
	SetTexture(name, nativeTexturePointer);
	return false;
}
		
	
UNITY_INTERFACE_EXPORT void SetTexturePtr(const char* name, int id, void* nativeTexturePointer) {
	assert(id);
	assert(texturePointers.find(id) == texturePointers.end());
	texturePointers[id] = nativeTexturePointer;
	bool needsSet = SetTextureID(name, id);
	assert(!needsSet);
}

static void SetTexture(const char* name, void* nativeTexturePointer) {
	//DebugSS("SetTexture(" << name << ", " << nativeTexturePointer << ")");

#if SUPPORT_OPENGL_UNIFIED
    if (isOpenGLDevice(s_DeviceType)) {
        if (g_Program) {
            GUARD_UNIFORMS;
            auto iter = textureUnits.find(name);
            if (iter == textureUnits.end())
                return;
            
            auto textureID = (GLint)(size_t)nativeTexturePointer;
            auto textureUnit = iter->second;
            textureIDs[textureUnit] = textureID;           
            
        }
    }
#endif

#ifdef SUPPORT_D3D
if (s_DeviceType == kUnityGfxRendererD3D11) {
	GUARD_TEXTURES;

	auto iter = resourceViewIndexes.find(name);
	if (iter == resourceViewIndexes.end())
		return;

	auto index = iter->second;
	assert(index < resourceViews.size());
	auto resource = (ID3D11Resource*)nativeTexturePointer;
	resource->AddRef();

	pendingResources.push_back(std::pair<ID3D11Resource*, size_t>(resource, index));

}

#endif
}


UNITY_INTERFACE_EXPORT void GetVector4(const char* name, float* value) {
	GUARD_UNIFORMS;
	if (!constantBuffer) return;
	auto prop = propForName(name, Vector4);
	if (!prop) return;
	memcpy(value, constantBuffer + prop->offset, sizeof(float) * 4);
}
    
UNITY_INTERFACE_EXPORT float GetFloat(const char* name) {
	GUARD_UNIFORMS;
	if (!constantBuffer) return 0.0f;
	auto prop = propForName(name, Float);	
	if (!prop) return 0.0f;
	return *((float*)(constantBuffer + prop->offset));
}

UNITY_INTERFACE_EXPORT bool HasProperty(const char* name) { return hasProp(name); }
UNITY_INTERFACE_EXPORT void Reset() { didInit = false; }
UNITY_INTERFACE_EXPORT void PrintUniforms() { printUniforms(); }
UNITY_INTERFACE_EXPORT void DumpUniformsToFile(const char* filename) {
  GUARD_UNIFORMS;
    std::ofstream js(filename);
    js << "{" << endl;

    for (auto i = shaderProps.begin(); i != shaderProps.end(); ++i) {
        auto prop = i->second;

        js << "    \"" << prop->name << "\": ";

        switch (prop->type) {
            case Float:
                js << prop->value(0); break;
            case Vector2:
                js << "[" << prop->value(0) << ", " << prop->value(1) << "]"; break;
            case Vector3:
                js << "[" << prop->value(0) << ", " << prop->value(1) << ", " << prop->value(2) << "]"; break;
            case Vector4:
                js << "[" << prop->value(0) << ", " << prop->value(1) << ", " << prop->value(2) << ", " << prop->value(3) << "]"; break;
            case Matrix:
                js << "[";
                for (int i = 0; i < 16; ++i) {
                  js << prop->value(i);
                  if (i != 15)
                    js << ", ";
                }
                js << "]";
                break;
            default:
                assert(false);
        }

        auto nexti = i;
        nexti++;
        if (nexti != shaderProps.end())
          js << ",";

        js << "\n";
    }

    js << "}";
}

UNITY_INTERFACE_EXPORT void SetShaderIncludePath(const char* includePath) { shaderIncludePath = includePath; }
UNITY_INTERFACE_EXPORT void SetUpdateUniforms(bool update) { updateUniforms = update; }
UNITY_INTERFACE_EXPORT void SetVerbose(bool verboseEnabled) { verbose = verboseEnabled; }
UNITY_INTERFACE_EXPORT void SetOptimizationLevel(int level) { optimizationLevel = level; }
UNITY_INTERFACE_EXPORT void SetShaderDebugging(bool shaderDebuggingEnabled) { shaderDebugging = shaderDebuggingEnabled;  }
UNITY_INTERFACE_EXPORT void SubmitUniforms(int uniformIndex) { submitUniforms(uniformIndex); }
UNITY_INTERFACE_EXPORT Stats GetStats() { return stats; }
UNITY_INTERFACE_EXPORT void SetStats(Stats newStats) { stats = newStats; }

UNITY_INTERFACE_EXPORT int CreateComputeShader() {
	auto id = ++nextComputeShaderId;
	computeShaders[id] = new Shader(id);
	return id;
}

UNITY_INTERFACE_EXPORT bool GetComputeShaderReady(int id) {
	auto computeShader = findComputeShader(id);
	return computeShader && computeShader->IsReady();
}

UNITY_INTERFACE_EXPORT void Dispatch(int id, int ThreadGroupCountX, int ThreadGroupCountY, int ThreadGroupCountZ) {
	auto computeShader = findComputeShader(id);
	if (computeShader)
		computeShader->Dispatch(ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ);
}

UNITY_INTERFACE_EXPORT void SetComputeShaderSource(int id, const char* src, const char* entryPoint) {
	if (!src || !entryPoint) {
		Debug("must give a non-null src and entryPoint");
		return;
	}

	auto computeShader = findComputeShader(id);
	if (computeShader)
		computeShader->SetSource(src, entryPoint);
}

UNITY_INTERFACE_EXPORT void DestroyComputeBuffer(int id) {
	auto computeBuffer = findComputeBuffer(id);
	if (computeBuffer) {
		computeBuffers.erase(id);
		delete computeBuffer;
	}
}

UNITY_INTERFACE_EXPORT void DestroyComputeShader(int id) {
	auto computeShader = findComputeShader(id);
	if (computeShader) {
		computeShaders.erase(id);
		delete computeShader;
	}
}

UNITY_INTERFACE_EXPORT void SetShaderSource(
	const char* fragShader, const char* fragEntryPoint,
	const char* vertexShader, const char* vertEntryPoint,
	const char* computeShader, const char* computeEntryPoint) {

#ifdef SUPPORT_D3D
	if (s_DeviceType == kUnityGfxRendererD3D11) {
		vector<CompileTask> compileTasks;
		if (fragShader && fragEntryPoint) {
		  CompileTask task(Fragment, fragShader, fragEntryPoint);
		  task.srcName = shaderIncludePath + "\\fragment.hlsl";
		  compileTasks.push_back(task);
		}
		if (vertexShader && vertEntryPoint) {
		  CompileTask task(Vertex, vertexShader, vertEntryPoint);
		  task.srcName = shaderIncludePath + "\\vertex.hlsl";
		  compileTasks.push_back(task);
		}
		submitCompileTasks(compileTasks, false);

		return;
  }
#endif 
  ShaderSource shaderSource;
  if (fragShader) shaderSource.fragShader = fragShader;
  if (fragEntryPoint) shaderSource.fragEntryPoint = fragEntryPoint;
  if (vertexShader) shaderSource.vertShader = vertexShader;
  if (vertEntryPoint) shaderSource.vertEntryPoint = vertEntryPoint;

  if (!shaderSourceQueue.write(shaderSource))
      Debug("could not write to shader queue");
}
}
