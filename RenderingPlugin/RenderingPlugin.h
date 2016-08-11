#pragma once

#include "Unity/IUnityInterface.h"

#include <string>
using std::string;

// Which platform we are on?
#if _MSC_VER
#define UNITY_WIN 1
#elif defined(__APPLE__)
	#if defined(__arm__)
		#define UNITY_IPHONE 1
	#else
		#define UNITY_OSX 1
	#endif
#elif defined(UNITY_METRO) || defined(UNITY_ANDROID) || defined(UNITY_LINUX)
// these are defined externally
#else
#error "Unknown platform!"
#endif



// Which graphics device APIs we possibly support?
#if UNITY_METRO
	#define SUPPORT_D3D11 1
	#if WINDOWS_UWP
		#define SUPPORT_D3D12 1
	#endif
#elif UNITY_WIN
	#define SUPPORT_D3D9 1
	#define SUPPORT_D3D11 1 // comment this out if you don't have D3D11 header/library files
	#ifdef _MSC_VER
		#if _MSC_VER >= 1900
			#define SUPPORT_D3D12 1
		#endif
	#endif
	#define SUPPORT_OPENGL_LEGACY 1
	#define SUPPORT_OPENGL_UNIFIED 1
	#define SUPPORT_OPENGL_CORE 1
#elif UNITY_IPHONE || UNITY_ANDROID
	#define SUPPORT_OPENGL_UNIFIED 1
	#define SUPPORT_OPENGL_ES 1
#elif UNITY_OSX || UNITY_LINUX
	#define SUPPORT_OPENGL_LEGACY 1
	#define SUPPORT_OPENGL_UNIFIED 1
	#define SUPPORT_OPENGL_CORE 1
#endif

#include <sstream>

enum PluginMessage {
	NeedsSceneViewRepaint
};

typedef void(*PluginCallback)(PluginMessage mssage);


typedef void(*FuncPtr)(const char *);
FuncPtr GetDebugFunc();

#define Debug(m) do { if (GetDebugFunc()) { GetDebugFunc()(m); } else { std::cout << m << std::endl; } } while(0);

#define DebugSS(ssexp) do { \
    if (GetDebugFunc()) { \
        std::stringstream ss; ss << ssexp; std::string s(ss.str()); GetDebugFunc()(s.c_str()); \
    } \
} while(0);

struct ShaderSource {
	string fragShader;
	string fragEntryPoint;
	string vertShader;
	string vertEntryPoint;
};

enum ShaderType {
	Vertex,
	Fragment
};

struct CompileTask {
	ShaderType shaderType;
	string src;
	string srcName;
	string entryPoint;
	void operator()();
};
