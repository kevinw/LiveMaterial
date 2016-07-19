#include "RenderingPlugin.h"
#include "Unity/IUnityGraphics.h"

#include <sstream>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>
#include <string>

#define NUM_VERTS 6

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

#if SUPPORT_OPENGL_LEGACY
//#	include "OpenGL/glew.h"
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

typedef void (*FuncPtr)( const char * );
FuncPtr _DebugFunc = nullptr;
#define Debug(m) do { if (_DebugFunc) _DebugFunc(m); } while(0);

static bool didInit = false;
static void updateUniforms();
static void clearUniforms();
static void printUniforms();

extern "C" {
UNITY_INTERFACE_EXPORT void SetMatrix(const char* name, float* value);
}

// --------------------------------------------------------------------------
// Helper utilities



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
    GLenum glErr;
    int retCode = 0;
    const int SIZE = 1024 * 5;
    char buffer [SIZE];
    
    glErr = glGetError();
    if (glErr != GL_NO_ERROR)
    {
        snprintf(buffer, SIZE, "glError in file %s @ line %d: %s\n",
               file, line, gluErrorString(glErr));
        Debug(buffer);
        retCode = 1;
    }
    return retCode;
}

// --------------------------------------------------------------------------
// SetTimeFromUnity, an example function we export which is called by one of the scripts.

static float g_Time;

extern "C" void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API SetTimeFromUnity (float t) { g_Time = t; }

GLuint loadShader(GLenum type, const char *shaderSrc, const char* debugOutPath);


// --------------------------------------------------------------------------
// SetTextureFromUnity, an example function we export which is called by one of the scripts.

static void* g_TexturePointer = NULL;
#ifdef SUPPORT_OPENGL_UNIFIED
static int   g_TexWidth  = 0;
static int   g_TexHeight = 0;
#endif

#if SUPPORT_OPENGL_UNIFIED
extern "C" void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API SetTextureFromUnity(void* texturePtr, int w, int h)
#else
extern "C" void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API SetTextureFromUnity(void* texturePtr)
#endif
{
	// A script calls this at initialization time; just remember the texture pointer here.
	// Will update texture pixels each frame from the plugin rendering event (texture update
	// needs to happen on the rendering thread).
	g_TexturePointer = texturePtr;
#if SUPPORT_OPENGL_UNIFIED
	g_TexWidth = w;
	g_TexHeight = h;
#endif
}

enum
{
	ATTRIB_POSITION = 0,
	ATTRIB_COLOR = 1,
    ATTRIB_UV = 2
};

// --------------------------------------------------------------------------
// SetUnityStreamingAssetsPath, an example function we export which is called by one of the scripts.

static std::string s_UnityStreamingAssetsPath;
extern "C" void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API SetUnityStreamingAssetsPath(const char* path)
{
	s_UnityStreamingAssetsPath = path;
}

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
			DebugLog("OnGraphicsDeviceEvent(Initialize).\n");
			s_DeviceType = s_Graphics->GetRenderer();
			currentDeviceType = s_DeviceType;
			break;
		}

	case kUnityGfxDeviceEventShutdown:
		{
			DebugLog("OnGraphicsDeviceEvent(Shutdown).\n");
			s_DeviceType = kUnityGfxRendererNull;
			g_TexturePointer = NULL;
			break;
		}

	case kUnityGfxDeviceEventBeforeReset:
		{
			DebugLog("OnGraphicsDeviceEvent(BeforeReset).\n");
			break;
		}

	case kUnityGfxDeviceEventAfterReset:
		{
			DebugLog("OnGraphicsDeviceEvent(AfterReset).\n");
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



// --------------------------------------------------------------------------
// OnRenderEvent
// This will be called for GL.IssuePluginEvent script calls; eventID will
// be the integer passed to IssuePluginEvent. In this example, we just ignore
// that value.


struct MyVertex {
	float x, y, z;
	unsigned int color;
    float u, v;
};
static void SetDefaultGraphicsState ();
static void DoRendering (const float* worldMatrix, const float* identityMatrix, float* projectionMatrix, const MyVertex* verts);

static std::string newFragShaderText;
static bool newFragShader = false;

static std::string newVertShaderText;
static bool newVertShader = false;


static GLuint	g_VProg;
static GLuint	g_FShader;
static GLuint	g_Program = 0;
static GLuint	g_VertexArray;
static GLuint	g_ArrayBuffer;

static void LinkProgram();

#if SUPPORT_D3D11
HRESULT CompileShader(_In_ const char* src, _In_ LPCSTR srcName, _In_ LPCSTR entryPoint, _In_ LPCSTR profile, _Outptr_ ID3DBlob** blob, _Outptr_ ID3DBlob** errorBlob);
#endif

static void MaybeLoadNewShaders();

static void UNITY_INTERFACE_API OnRenderEvent(int eventID)
{
	// Unknown graphics device type? Do nothing.
	if (s_DeviceType == kUnityGfxRendererNull)
		return;

	MaybeLoadNewShaders();

	// A colored triangle. Note that colors will come out differently
	// in D3D9/11 and OpenGL, for example, since they expect color bytes
	// in different ordering.
	MyVertex verts[NUM_VERTS] = {
		{  -1.0f, -1.0f,  0, 0xffffffff, 0, 0 },
		{   1.0f,  1.0f,  0, 0xffffffff, 1, 1 },
		{  -1.0,   1.0f ,  0, 0xffffffff, 0, 1 },

		{ -1.0f, -1.0f,  0, 0xffffffff, 0, 0 },
		{ 1.0f,  -1.0f,  0, 0xffffffff, 1, 0 },
		{ 1.0,    1.0f ,  0, 0xffffffff, 1, 1 },

	};


	// Some transformation matrices: rotate around Z axis for world
	// matrix, identity view matrix, and identity projection matrix.
	float worldMatrix[16] = {
		1,0,0,0,
		0,1,0,0,
		0,0,1,0,
		0,0,0.7f,1,
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
	DoRendering (worldMatrix, identityMatrix, projectionMatrix, verts);
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
typedef std::vector<unsigned char> Buffer;
bool LoadFileIntoBuffer(const std::string& fileName, Buffer& data)
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
		std::string errorMessage = "Failed to find ";
		errorMessage += fileName;
		DebugLog(errorMessage.c_str());
		return false;
	}
}
#endif


// -------------------------------------------------------------------
//  Direct3D 9 setup/teardown code


#if SUPPORT_D3D9

static IDirect3DDevice9* g_D3D9Device;

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
static ID3D11Buffer* g_D3D11VB = NULL; // vertex buffer
static ID3D11Buffer* g_D3D11CB = NULL; // constant buffer
static ID3D11VertexShader* g_D3D11VertexShader = NULL;
static ID3D11PixelShader* g_D3D11PixelShader = NULL;
static ID3D11InputLayout* g_D3D11InputLayout = NULL;
static ID3D11RasterizerState* g_D3D11RasterState = NULL;
static ID3D11BlendState* g_D3D11BlendState = NULL;
static ID3D11DepthStencilState* g_D3D11DepthState = NULL;

static D3D11_INPUT_ELEMENT_DESC s_DX11InputElementDesc[] = {
	{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
	{ "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
};

static bool EnsureD3D11ResourcesAreCreated()
{
	if (g_D3D11VertexShader)
		return true;

	// D3D11 has to load resources. Wait for Unity to provide the streaming assets path first.
	if (s_UnityStreamingAssetsPath.empty())
		return false;

	D3D11_BUFFER_DESC desc;
	memset (&desc, 0, sizeof(desc));

	// vertex buffer
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.ByteWidth = 1024;
	desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
	g_D3D11Device->CreateBuffer (&desc, NULL, &g_D3D11VB);

	// constant buffer
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.ByteWidth = 64; // hold 1 matrix
	desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	desc.CPUAccessFlags = 0;
	g_D3D11Device->CreateBuffer (&desc, NULL, &g_D3D11CB);


	HRESULT hr = -1;
	Buffer vertexShader;
	Buffer pixelShader;
	std::string vertexShaderPath(s_UnityStreamingAssetsPath);
	vertexShaderPath += "/Shaders/DX11_9_1/SimpleVertexShader.cso";
	std::string fragmentShaderPath(s_UnityStreamingAssetsPath);
	fragmentShaderPath += "/Shaders/DX11_9_1/SimplePixelShader.cso";
	LoadFileIntoBuffer(vertexShaderPath, vertexShader);
	LoadFileIntoBuffer(fragmentShaderPath, pixelShader);

	if (vertexShader.size() > 0 && pixelShader.size() > 0) {
		hr = g_D3D11Device->CreateVertexShader(&vertexShader[0], vertexShader.size(), nullptr, &g_D3D11VertexShader);
		if (FAILED(hr)) Debug("Failed to create vertex shader.\n");
		hr = g_D3D11Device->CreatePixelShader(&pixelShader[0], pixelShader.size(), nullptr, &g_D3D11PixelShader);
		if (FAILED(hr)) Debug("Failed to create pixel shader.\n");
	}
	else
	{
		Debug("Failed to load vertex or pixel shader.\n");
	}
	// input layout
	if (g_D3D11VertexShader && vertexShader.size() > 0)
	{
		g_D3D11Device->CreateInputLayout (s_DX11InputElementDesc, 2, &vertexShader[0], vertexShader.size(), &g_D3D11InputLayout);
	}

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
	dsdesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
	dsdesc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
	g_D3D11Device->CreateDepthStencilState (&dsdesc, &g_D3D11DepthState);

	D3D11_BLEND_DESC bdesc;
	memset (&bdesc, 0, sizeof(bdesc));
	bdesc.RenderTarget[0].BlendEnable = FALSE;
	bdesc.RenderTarget[0].RenderTargetWriteMask = 0xF;
	g_D3D11Device->CreateBlendState (&bdesc, &g_D3D11BlendState);

	return true;
}






static void ReleaseD3D11Resources()
{
	SAFE_RELEASE(g_D3D11VB);
	SAFE_RELEASE(g_D3D11CB);
	SAFE_RELEASE(g_D3D11VertexShader);
	SAFE_RELEASE(g_D3D11PixelShader);
	SAFE_RELEASE(g_D3D11InputLayout);
	SAFE_RELEASE(g_D3D11RasterState);
	SAFE_RELEASE(g_D3D11BlendState);
	SAFE_RELEASE(g_D3D11DepthState);
}

static void DoEventGraphicsDeviceD3D11(UnityGfxDeviceEventType eventType)
{
	if (eventType == kUnityGfxDeviceEventInitialize)
	{
		IUnityGraphicsD3D11* d3d11 = s_UnityInterfaces->Get<IUnityGraphicsD3D11>();
		g_D3D11Device = d3d11->GetDevice();
		
		EnsureD3D11ResourcesAreCreated();
	}
	else if (eventType == kUnityGfxDeviceEventShutdown)
	{
		ReleaseD3D11Resources();
	}
}

#endif // #if SUPPORT_D3D11

#define isOpenGLDevice(d) (d == kUnityGfxRendererOpenGLES20 || \
    d == kUnityGfxRendererOpenGLES30 || \
    d == kUnityGfxRendererOpenGLCore || \
    d == kUnityGfxRendererOpenGL)

static void MaybeLoadNewShaders() {
    
    if (!newFragShader || !newVertShader)
        return;
    
#if SUPPORT_OPENGL_UNIFIED || SUPPORT_OPENGL_LEGACY
    if (isOpenGLDevice(s_DeviceType)) {
        
        if (newFragShader) {
            newFragShader = false;
            GLuint newFrag = loadShader(GL_FRAGMENT_SHADER, newFragShaderText.c_str(), "/Users/kevin/Desktop/last_shader.frag");
            if (newFrag)
                g_FShader = newFrag;
        }
        
        if (newVertShader) {
            newVertShader = false;
            GLuint newVert = loadShader(GL_VERTEX_SHADER, newVertShaderText.c_str(), "/Users/kevin/Desktop/last_shader.vert");
            if (newVert)
                g_VProg = newVert;
        }

        LinkProgram();
        clearUniforms();
        printOpenGLError();
    }
#endif
    
#if SUPPORT_D3D11
    // D3D11 case
    if (s_DeviceType == kUnityGfxRendererD3D11)
    {
        ID3DBlob *PS = nullptr;
        ID3DBlob *error = nullptr;
        HRESULT hr = -1;
        hr = CompileShader(newFragShaderText.c_str(), "SimplePixelShader.cso", "PS", "ps_5_0", &PS, &error);
        if (FAILED(hr)) {
            Debug("Could not compile shader");
            if (error) {
                std::string errstr((char*)error->GetBufferPointer(), error->GetBufferSize());
                Debug("error blob:");
                Debug(errstr.c_str());
            }
            else {
                Debug("..and no error blob :(");
            }
            
            
        } else {
            if (PS && PS->GetBufferPointer()) {
                hr = g_D3D11Device->CreatePixelShader(PS->GetBufferPointer(), PS->GetBufferSize(), nullptr, &g_D3D11PixelShader);
                if (FAILED(hr)) {
                    Debug("Failed to create pixel shader.\n");
                }
            } else {
                Debug("pointer was null");
            }
        }
    }
#endif
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


#include <fstream>


GLuint loadShader(GLenum type, const char *shaderSrc, const char* debugOutPath)
{
   GLuint shader;
   GLint compiled;
   
   // Create the shader object
   shader = glCreateShader ( type );

   if ( shader == 0 ) {
     Debug("could not create shader object");
   	return 0;
   }
    
    if (debugOutPath) {
        std::ofstream debugOut;
        debugOut.open(debugOutPath);
        debugOut << shaderSrc;
        debugOut.close();
    }
    
    std::string s = "GLSL Version ";
    s += (const char*)glGetString(GL_SHADING_LANGUAGE_VERSION);
    Debug(s.c_str());
    
   // Load the shader source
   glShaderSource(shader, 1, &shaderSrc, NULL);

   // Compile the shader
   glCompileShader(shader);

   // Check the compile status
   glGetShaderiv ( shader, GL_COMPILE_STATUS, &compiled );

   if ( !compiled ) 
   {
       GLint infoLen = 0;
       glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &infoLen);
       Debug("error compiling shader:");
      if (infoLen > 1) {
         char* infoLog = (char*)malloc (sizeof(char) * infoLen);
         glGetShaderInfoLog(shader, infoLen, NULL, infoLog);
         Debug(infoLog);
         free(infoLog);
      }

      glDeleteShader ( shader );
      return 0;
   }

   return shader;

}

static GLuint CreateShader(GLenum type, const char* text)
{
	GLuint shader = glCreateShader(type);
	glShaderSource(shader, 1, &text, NULL);
  Debug("compiling shader:");
  Debug(text);
	glCompileShader(shader);

  GLint isCompiled = 0;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &isCompiled);
  if (isCompiled == GL_FALSE) {
    if (!glIsShader(shader)) {
      Debug("Not a shader.");
      return -1;
    }

    GLint maxLength = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &maxLength);

    Debug("error compiling shader");

    // The maxLength includes the NULL character
    std::vector<GLchar> errorLog(maxLength);
    glGetShaderInfoLog(shader, maxLength, &maxLength, &errorLog[0]);

    Debug(errorLog.data());
    printOpenGLError();

    // Provide the infolog in whatever manor you deem best.
    // Exit with failure.
    glDeleteShader(shader); // Don't leak the shader.
    return -1;
  }

	return shader;
}

static void LinkProgram() {
    GLuint program = glCreateProgram();
    assert(program > 0);
    glBindAttribLocation(program, ATTRIB_POSITION, "xlat_attrib_POSITION");
    glBindAttribLocation(program, ATTRIB_COLOR, "xlat_attrib_COLOR");
    glBindAttribLocation(program, ATTRIB_UV, "xlat_attrib_TEXCOORD0");
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
        g_Program = program;
    } else {
        Debug("failure linking program:");
        
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
        /*
		if (s_DeviceType == kUnityGfxRendererOpenGLES20)
		{
			::printf("OpenGLES 2.0 device\n");
			g_VProg		= CreateShader(GL_VERTEX_SHADER, kGlesVProgTextGLES2);
			g_FShader	= CreateShader(GL_FRAGMENT_SHADER, kGlesFShaderTextGLES2);
		}
		else if(s_DeviceType == kUnityGfxRendererOpenGLES30)
		{
			::printf("OpenGLES 3.0 device\n");
			g_VProg		= CreateShader(GL_VERTEX_SHADER, kGlesVProgTextGLES3);
			g_FShader	= CreateShader(GL_FRAGMENT_SHADER, kGlesFShaderTextGLES3);
		}
#if SUPPORT_OPENGL_CORE
		else if(s_DeviceType == kUnityGfxRendererOpenGLCore)
		{
			::printf("OpenGL Core device\n");
			//glewExperimental = GL_TRUE;
			//glewInit();
			//glGetError(); // Clean up error generated by glewInit

			g_VProg		= CreateShader(GL_VERTEX_SHADER, kGlesVProgTextGLCore);
			g_FShader	= CreateShader(GL_FRAGMENT_SHADER, kGlesFShaderTextGLCore);
		}
#endif
         */
        
		glGenBuffers(1, &g_ArrayBuffer);
		glBindBuffer(GL_ARRAY_BUFFER, g_ArrayBuffer);
		glBufferData(GL_ARRAY_BUFFER, sizeof(MyVertex) * NUM_VERTS, NULL, GL_STREAM_DRAW);

        //LinkProgram();
        
	    
    printOpenGLError();
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


	#if SUPPORT_OPENGL_LEGACY
	// OpenGL 2 legacy case (deprecated)
	if (s_DeviceType == kUnityGfxRendererOpenGL)
	{
		glDisable (GL_CULL_FACE);
		glDisable (GL_LIGHTING);
		glDisable (GL_BLEND);
		glDisable (GL_ALPHA_TEST);
		glDepthFunc (GL_LEQUAL);
		glEnable (GL_DEPTH_TEST);
		glDepthMask (GL_FALSE);
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
		glDepthMask(GL_FALSE);

		assert(glGetError() == GL_NO_ERROR);
	}
	#endif
}


static void FillTextureFromCode (int width, int height, int stride, unsigned char* dst)
{
	const float t = g_Time * 4.0f;

	for (int y = 0; y < height; ++y)
	{
		unsigned char* ptr = dst;
		for (int x = 0; x < width; ++x)
		{
			// Simple oldskool "plasma effect", a bunch of combined sine waves
			int vv = int(
				(127.0f + (127.0f * sinf(x/7.0f+t))) +
				(127.0f + (127.0f * sinf(y/5.0f-t))) +
				(127.0f + (127.0f * sinf((x+y)/6.0f-t))) +
				(127.0f + (127.0f * sinf(sqrtf(float(x*x + y*y))/4.0f-t)))
				) / 4;

			// Write the texture pixel
			ptr[0] = vv;
			ptr[1] = vv;
			ptr[2] = vv;
			ptr[3] = vv;

			// To next pixel (our pixels are 4 bpp)
			ptr += 4;
		}

		// To next image row
		dst += stride;
	}
}


static void DoRendering (const float* worldMatrix, const float* identityMatrix, float* projectionMatrix, const MyVertex* verts)
{
	// Does actual rendering of a simple triangle
  //


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

		// Update native texture from code
		if (g_TexturePointer)
		{
			IDirect3DTexture9* d3dtex = (IDirect3DTexture9*)g_TexturePointer;
			D3DSURFACE_DESC desc;
			d3dtex->GetLevelDesc (0, &desc);
			D3DLOCKED_RECT lr;
			d3dtex->LockRect (0, &lr, NULL, 0);
			FillTextureFromCode (desc.Width, desc.Height, lr.Pitch, (unsigned char*)lr.pBits);
			d3dtex->UnlockRect (0);
		}
	}
	#endif


	#if SUPPORT_D3D11
	// D3D11 case
	if (s_DeviceType == kUnityGfxRendererD3D11 && EnsureD3D11ResourcesAreCreated())
	{
		ID3D11DeviceContext* ctx = NULL;
		g_D3D11Device->GetImmediateContext (&ctx);

		// update constant buffer - just the world matrix in our case
		ctx->UpdateSubresource (g_D3D11CB, 0, NULL, worldMatrix, 64, 0);

		// set shaders
		ctx->VSSetConstantBuffers (0, 1, &g_D3D11CB);
		ctx->VSSetShader (g_D3D11VertexShader, NULL, 0);
		ctx->PSSetShader (g_D3D11PixelShader, NULL, 0);

		// update vertex buffer
		ctx->UpdateSubresource (g_D3D11VB, 0, NULL, verts, sizeof(verts[0])*NUM_VERTS, 0);

		// set input assembler data and draw
		ctx->IASetInputLayout (g_D3D11InputLayout);
		ctx->IASetPrimitiveTopology (D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		UINT stride = sizeof(MyVertex);
		UINT offset = 0;
		ctx->IASetVertexBuffers (0, 1, &g_D3D11VB, &stride, &offset);
		ctx->Draw (NUM_VERTS, 0);
        
        INIT_MESSAGE("LiveMaterial is drawing with D3D11");

		// update native texture from code
		if (g_TexturePointer)
		{
			ID3D11Texture2D* d3dtex = (ID3D11Texture2D*)g_TexturePointer;
			D3D11_TEXTURE2D_DESC desc;
			d3dtex->GetDesc (&desc);

			unsigned char* data = new unsigned char[desc.Width*desc.Height*4];
			FillTextureFromCode (desc.Width, desc.Height, desc.Width*4, data);
			ctx->UpdateSubresource (d3dtex, 0, NULL, data, desc.Width*4, 0);
			delete[] data;
		}

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
		
		// Update native texture from code
		if (g_TexturePointer)
		{
			ID3D12Resource* resource = (ID3D12Resource*)g_TexturePointer;
			D3D12_RESOURCE_DESC desc = resource->GetDesc();

			// Begin a command list
			s_D3D12CmdAlloc->Reset();
			s_D3D12CmdList->Reset(s_D3D12CmdAlloc, nullptr);

			// Fill data
			const UINT64 kDataSize = desc.Width*desc.Height*4;
			ID3D12Resource* upload = GetD3D12UploadResource(kDataSize);
			void* mapped = NULL;
			upload->Map(0, NULL, &mapped);
			if (desc.Width > MAXINT32)
				throw "error";
			if (desc.Height > MAXINT32)
				throw "error";
			int w = static_cast<int>(desc.Width);
			int h = static_cast<int>(desc.Height);
			FillTextureFromCode(w, h, w*4, (unsigned char*)mapped);
			upload->Unmap(0, NULL);

			D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
			srcLoc.pResource = upload;
			srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
			device->GetCopyableFootprints(&desc, 0, 1, 0, &srcLoc.PlacedFootprint, nullptr, nullptr, nullptr);

			D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
			dstLoc.pResource = resource;
			dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			dstLoc.SubresourceIndex = 0;

			// Queue data upload
			const D3D12_RESOURCE_STATES kDesiredState = D3D12_RESOURCE_STATE_COPY_DEST;
			D3D12_RESOURCE_STATES resourceState = kDesiredState;
			s_D3D12->GetResourceState(resource, &resourceState); // Get resource state as it will be after all command lists Unity queued before this plugin call execute.
			if (resourceState != kDesiredState)
			{
				D3D12_RESOURCE_BARRIER barrierDesc = {};
				barrierDesc.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
				barrierDesc.Transition.pResource = resource;
				barrierDesc.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
				barrierDesc.Transition.StateBefore = resourceState;
				barrierDesc.Transition.StateAfter = kDesiredState;
				s_D3D12CmdList->ResourceBarrier(1, &barrierDesc);
				s_D3D12->SetResourceState(resource, kDesiredState); // Set resource state as it will be after this command list is executed.
			}

			s_D3D12CmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

			// Execute the command list
			s_D3D12CmdList->Close();
			ID3D12CommandList* lists[1] = { s_D3D12CmdList };
			queue->ExecuteCommandLists(1, lists);
		}

		// Insert fence
		++s_D3D12FenceValue;
		queue->Signal(s_D3D12Fence, s_D3D12FenceValue);
	}
	#endif

	#if SUPPORT_OPENGL_LEGACY
	// OpenGL 2 legacy case (deprecated)
	if (s_DeviceType == kUnityGfxRendererOpenGL)
	{
		// Transformation matrices
		glMatrixMode (GL_MODELVIEW);
		glLoadMatrixf (worldMatrix);
		glMatrixMode (GL_PROJECTION);
		// Tweak the projection matrix a bit to make it match what identity
		// projection would do in D3D case.
		projectionMatrix[10] = 2.0f;
		projectionMatrix[14] = -1.0f;
		glLoadMatrixf (projectionMatrix);

		// Vertex layout
		glVertexPointer (NUM_VERTS, GL_FLOAT, sizeof(verts[0]), &verts[0].x);
		glEnableClientState (GL_VERTEX_ARRAY);
		glColorPointer (4, GL_UNSIGNED_BYTE, sizeof(verts[0]), &verts[0].color);
		glEnableClientState (GL_COLOR_ARRAY);

		// Draw!
		glDrawArrays (GL_TRIANGLES, 0, NUM_VERTS);

		// update native texture from code
		if (g_TexturePointer)
		{
			GLuint gltex = (GLuint)(size_t)(g_TexturePointer);
			glBindTexture (GL_TEXTURE_2D, gltex);
			int texWidth, texHeight;
			glGetTexLevelParameteriv (GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &texWidth);
			glGetTexLevelParameteriv (GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &texHeight);

			unsigned char* data = new unsigned char[texWidth*texHeight*4];
			FillTextureFromCode (texWidth, texHeight, texHeight*4, data);
			glTexSubImage2D (GL_TEXTURE_2D, 0, 0, 0, texWidth, texHeight, GL_RGBA, GL_UNSIGNED_BYTE, data);
			delete[] data;
		}
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

		// Tweak the projection matrix a bit to make it match what identity projection would do in D3D case.
		projectionMatrix[10] = 2.0f;
		projectionMatrix[14] = -1.0f;

		glUseProgram(g_Program);
//      SetMatrix("worldMatrix", (float*)&worldMatrix[0]);
//      SetMatrix("projMatrix", (float*)&projectionMatrix[0]);
        updateUniforms();
        printOpenGLError();

#if SUPPORT_OPENGL_CORE
		if (s_DeviceType == kUnityGfxRendererOpenGLCore)
		{
			glGenVertexArrays(1, &g_VertexArray);
			glBindVertexArray(g_VertexArray);
		}
        printOpenGLError();
#endif

		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        printOpenGLError();
        
		glBindBuffer(GL_ARRAY_BUFFER, g_ArrayBuffer);
		glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(MyVertex) * NUM_VERTS, &verts[0].x);
        printOpenGLError();

		glEnableVertexAttribArray(ATTRIB_POSITION);
		glVertexAttribPointer(ATTRIB_POSITION, 3, GL_FLOAT, GL_FALSE, sizeof(MyVertex), BUFFER_OFFSET(0));
        printOpenGLError();

		glEnableVertexAttribArray(ATTRIB_COLOR);
		glVertexAttribPointer(ATTRIB_COLOR, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(MyVertex), BUFFER_OFFSET(sizeof(float) * 3));
        printOpenGLError();
        
        glEnableVertexAttribArray(ATTRIB_UV);
        glVertexAttribPointer(ATTRIB_UV, 2, GL_FLOAT, GL_TRUE, sizeof(MyVertex), (void*)offsetof(MyVertex, u));
        

		glDrawArrays(GL_TRIANGLES, 0, NUM_VERTS);
        printOpenGLError();
    
        INIT_MESSAGE("LiveMaterial is drawing with OpenGL ES/Core");
        
		// update native texture from code
		if (g_TexturePointer)
		{
			GLuint gltex = (GLuint)(size_t)(g_TexturePointer);
			glBindTexture(GL_TEXTURE_2D, gltex);
			// The script only pass width and height with OpenGL ES on mobile
#if SUPPORT_OPENGL_CORE
			if (s_DeviceType == kUnityGfxRendererOpenGLCore)
			{
				glGetTexLevelParameteriv (GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &g_TexWidth);
				glGetTexLevelParameteriv (GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &g_TexHeight);
			}
#endif
            
			auto data = new unsigned char[g_TexWidth*g_TexHeight*4];
			FillTextureFromCode(g_TexWidth, g_TexHeight, g_TexHeight*4, data);
			glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, g_TexWidth, g_TexHeight, GL_RGBA, GL_UNSIGNED_BYTE, data);
			delete[] data;
		}

#if SUPPORT_OPENGL_CORE
		if (s_DeviceType == kUnityGfxRendererOpenGLCore)
		{
			glDeleteVertexArrays(1, &g_VertexArray);
		}
#endif

        printOpenGLError();
		//assert(glGetError() == GL_NO_ERROR);
        
	}
	#endif
}

enum PropType {
    Float,
    Vector2,
    Vector3,
    Vector4,
    Matrix
};

struct ShaderProp {
    ShaderProp(PropType type_, std::string name_)
    : type(type_)
    , name(name_)
    {
#if SUPPORT_OPENGL_UNIFIED || SUPPORT_OPENGL_CORE || SUPPORT_OPENGL_LEGACY
        uniformIndex = UNIFORM_UNSET;
#endif
    }
    
    PropType type;
    std::string name;
    float value[16];
    
#if SUPPORT_OPENGL_UNIFIED || SUPPORT_OPENGL_CORE || SUPPORT_OPENGL_LEGACY
    static const int UNIFORM_UNSET = -2;
    static const int UNIFORM_INVALID = -1;
    int uniformIndex;
#endif
    
    
};

#include <map>

typedef std::map<std::string, ShaderProp*> PropMap;
PropMap shaderProps;

static ShaderProp* propForName(const char* name, PropType type) {
    ShaderProp* prop = nullptr;
    PropMap::iterator i = shaderProps.find(name);

    if (i != shaderProps.end())
        prop = i->second;
    
    if (prop == nullptr || prop->type != type)
        prop = shaderProps[name] = new ShaderProp(type, name);
    
    assert(prop->type == type);
    
    return prop;
}

static void printUniforms() {
    std::stringstream ss;
    for (auto i = shaderProps.begin(); i != shaderProps.end(); ++i) {
        auto prop = i->second;
        ss << prop->name << " ";
#if SUPPORT_OPENGL_UNIFIED || SUPPORT_OPENGL_LEGACY
        if (prop->uniformIndex == ShaderProp::UNIFORM_INVALID)
            ss << "(INVALID) ";
#endif
        switch (prop->type) {
            case Float:
                ss << prop->value[0]; break;
            case Vector2:
                ss << prop->value[0] << " " << prop->value[1]; break;
            case Vector3:
                ss << prop->value[0] << " " << prop->value[1] << " " << prop->value[2]; break;
            case Vector4:
            case Matrix:
                ss << prop->value[0] << " " << prop->value[1] << " " << prop->value[2] << " " << prop->value[3]; break;
            default:
                assert(false);
        }
        ss << "\n";
    }
    std::string s(ss.str());
    Debug(s.c_str());
}

static void clearUniforms() {
    for (auto i = shaderProps.begin(); i != shaderProps.end(); i++) {
#if SUPPORT_OPENGL_UNIFIED || SUPPORT_OPENGL_LEGACY
        auto prop = i->second;
        if (isOpenGLDevice(s_DeviceType)) {
            prop->uniformIndex = ShaderProp::UNIFORM_UNSET;
        }
#endif
        
    }
    
    shaderProps.clear();
}

static void updateUniforms() {
    if (g_Program == 0)
        return;
    
    for (auto i = shaderProps.begin(); i != shaderProps.end(); i++) {
        auto prop = i->second;
        
#if SUPPORT_OPENGL_UNIFIED || SUPPORT_OPENGL_CORE || SUPPORT_OPENGL_LEGACY
        if (isOpenGLDevice(s_DeviceType)) {
            if (prop->uniformIndex == ShaderProp::UNIFORM_INVALID)
                continue;
            if (prop->uniformIndex == ShaderProp::UNIFORM_UNSET) {
                prop->uniformIndex = glGetUniformLocation(g_Program, prop->name.c_str());
                if (prop->uniformIndex == ShaderProp::UNIFORM_INVALID) {
                    //std::stringstream ss; ss << "invalid uniform: " << prop->name;
                    //std::string s(ss.str());
                    //Debug(s.c_str());
                    continue;
                }
                assert(prop->uniformIndex != ShaderProp::UNIFORM_UNSET);
            }
            switch (prop->type) {
                case Float:
                    glUniform1f(prop->uniformIndex, prop->value[0]);
                    break;
                case Vector2:
                    glUniform2f(prop->uniformIndex, prop->value[0], prop->value[1]);
                    break;
                case Vector3:
                    glUniform3f(prop->uniformIndex, prop->value[0], prop->value[1], prop->value[2]);
                    break;
                case Vector4:
                    glUniform4f(prop->uniformIndex, prop->value[0], prop->value[1], prop->value[2], prop->value[3]);
                    break;
                case Matrix: {
                    const int numElements = 1;
                    const bool transpose = GL_FALSE;
                    glUniformMatrix4fv(prop->uniformIndex, numElements, transpose, prop->value);
                    break;
                }
                default:
                    assert(false);
            }
            //std::stringstream ss;
            //ss << "updated uniform " << i->second->name << " with index " << i->second->uniformIndex;
            //std::string s = ss.str();
            //Debug(s.c_str());

        }
#endif
    }
}

extern "C" {

UNITY_INTERFACE_EXPORT  int SetDebugFunction(FuncPtr fp) {
    _DebugFunc = fp;
    return 0;
}

UNITY_INTERFACE_EXPORT void SetVector4(const char* name, float* value) {
    auto prop = propForName(name, Vector4);
    memcpy(prop->value, value, sizeof(float) * 4);
}

UNITY_INTERFACE_EXPORT void SetFloat(const char* name, float value) {
    auto prop = propForName(name, Float);
    memcpy(prop->value, &value, sizeof(float) * 1);
}

UNITY_INTERFACE_EXPORT void SetMatrix(const char* name, float* value) {
    auto prop = propForName(name, Matrix);
    memcpy(prop->value, value, sizeof(float) * 16);
}
    
UNITY_INTERFACE_EXPORT void SetColor(const char* name, float* value) {
    memcpy(propForName(name, Vector4)->value, value, sizeof(float) * 4);
}

UNITY_INTERFACE_EXPORT void Reset() {
  didInit = false;
}

UNITY_INTERFACE_EXPORT void PrintUniforms() {
    printUniforms();
}

    
UNITY_INTERFACE_EXPORT void SetShaderSource(const char* pixelShader, const char* vertexShader) {
    // TODO: threadsafe
    if (pixelShader != nullptr) {
        newFragShaderText = pixelShader;
        newFragShader = true;
    }
    if (vertexShader != nullptr) {
        newVertShaderText = vertexShader;
        newVertShader = true;
    }
}

static char buf[2048];
extern "C"  UNITY_INTERFACE_EXPORT  const char* GetDebugInfo() {
	
#define WAT(m) do { snprintf(buf, 2048, "%s", m); return buf; } while (0);

#if SUPPORT_D3D9
		if (s_DeviceType == kUnityGfxRendererD3D9)
			WAT("D3D9");
#endif
#if SUPPORT_D3D11
		if (s_DeviceType == kUnityGfxRendererD3D11)
			WAT("D3D11");
#endif
#if SUPPORT_D3D12
		if (s_DeviceType == kUnityGfxRendererD3D12)
			WAT("D3D12");
#endif
#if SUPPORT_OPENGL_LEGACY
		// OpenGL 2 legacy case (deprecated)
    if (s_DeviceType == kUnityGfxRendererOpenGL)
        WAT("OpenGL 2 Legacy");
#endif
#if SUPPORT_OPENGL_UNIFIED
    if (s_DeviceType == kUnityGfxRendererOpenGLES20)
      WAT("OpenGL ES 2.0");

    if (s_DeviceType == kUnityGfxRendererOpenGLES30)
      WAT("OpenGL ES 3.0");

    if (s_DeviceType == kUnityGfxRendererOpenGLCore)
      WAT("OpenGL Core");
#endif

	WAT("UNKNOWN DEVICE");
}
	

}
