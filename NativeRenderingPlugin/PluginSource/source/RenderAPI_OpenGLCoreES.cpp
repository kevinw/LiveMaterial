#include "RenderAPI.h"
#include "PlatformBase.h"

#include <iostream>
#include <fstream>

// OpenGL Core profile (desktop) or OpenGL ES (mobile) implementation of RenderAPI.
// Supports several flavors: Core, ES2, ES3


#if SUPPORT_OPENGL_UNIFIED


#include <assert.h>
#if UNITY_IPHONE
#	include <OpenGLES/ES2/gl.h>
#elif UNITY_ANDROID
#	include <GLES2/gl2.h>
#else
#	include "GLEW/glew.h"
#endif

#define printOpenGLError() printOglError(__FILE__, __LINE__)

static const char* myGLErrorString(GLenum error) {
    switch (error) {
        case GL_NO_ERROR: return nullptr;
        case GL_INVALID_ENUM: return "An unacceptable value is specified for an enumerated argument. The offending command is ignored and has no other side effect than to set the error flag.";
        case GL_INVALID_VALUE: return "A numeric argument is out of range. The offending command is ignored and has no other side effect than to set the error flag.";
        case GL_INVALID_OPERATION: return "The specified operation is not allowed in the current state. The offending command is ignored and has no other side effect than to set the error flag.";
        case GL_INVALID_FRAMEBUFFER_OPERATION: return "The framebuffer object is not complete. The offending command is ignored and has no other side effect than to set the error flag.";
        case GL_OUT_OF_MEMORY: return "There is not enough memory left to execute the command. The state of the GL is undefined, except for the state of the error flags, after this error is recorded.";
        case GL_STACK_UNDERFLOW: return "An attempt has been made to perform an operation that would cause an internal stack to underflow.";
        case GL_STACK_OVERFLOW: return "An attempt has been made to perform an operation that would cause an internal stack to overflow.";
        default:
            return "Unrecognized glGetError error code";
    }
}

int printOglError(const char *file, int line) {
    GLenum glErr = glGetError();
    if (glErr == GL_NO_ERROR)
        return 0;
    
    const int SIZE = 1024 * 5;
    char buffer [SIZE];

    snprintf(buffer, SIZE, "glError in %s:%d: %s\n",
           file, line, myGLErrorString(glErr));
    Debug(buffer);
    return 1;
}

struct CompileOutput {
  ShaderType shaderType;
  GLint program;
  int inputId;
  bool success;
};

class LiveMaterial_GL : public LiveMaterial {
public:
    LiveMaterial_GL(RenderAPI* renderAPI, int id)
        : LiveMaterial(renderAPI, id)
          , _vertexShader(0)
          , _fragmentShader(0)
          , _program(0)
    {
    }

    virtual ~LiveMaterial_GL() {
    }

    virtual void Draw(int uniformIndex);
    virtual bool NeedsRender();
    virtual void _SetTexture(const char* name, void* nativeTexturePtr);

protected:
    virtual void _QueueCompileTasks(vector<CompileTask> tasks);
    void _discoverUniforms(GLuint program);

    void updateUniforms(int uniformsIndex);
    void compileNewShaders();
    void LinkProgram();

	GLuint _vertexShader;
	GLuint _fragmentShader;
	GLuint _program;

    // Textures
    vector<GLint> textureIDs;
    vector<GLint> uniformLocs;
    map<string, size_t> textureUnits;

	// Compile outputs
	mutex compileOutputMutex;
	vector<CompileOutput> compileOutput;
    
    // Compile inputs
    mutex compileTaskMutex;
    vector<CompileTask> compileTasks;
};


class RenderAPI_OpenGLCoreES : public RenderAPI
{
public:
	RenderAPI_OpenGLCoreES(UnityGfxRenderer apiType);
	virtual ~RenderAPI_OpenGLCoreES() { }

	virtual void ProcessDeviceEvent(UnityGfxDeviceEventType type, IUnityInterfaces* interfaces);

	virtual void DrawSimpleTriangles(const float worldMatrix[16], int triangleCount, const void* verticesFloat3Byte4);

	virtual void* BeginModifyTexture(void* textureHandle, int textureWidth, int textureHeight, int* outRowPitch);
	virtual void EndModifyTexture(void* textureHandle, int textureWidth, int textureHeight, int rowPitch, void* dataPtr);
    
    bool IsOpenGLCore() const { return m_APIType == kUnityGfxRendererOpenGLCore; }
    virtual LiveMaterial* _newLiveMaterial(int id);
    

protected:
    virtual bool supportsBackgroundCompiles();

private:
	void CreateResources();

private:
	UnityGfxRenderer m_APIType;
	GLuint m_VertexShader;
	GLuint m_FragmentShader;
	GLuint m_Program;
	GLuint m_VertexArray;
	GLuint m_VertexBuffer;
	int m_UniformWorldMatrix;
	int m_UniformProjMatrix;
};


bool LiveMaterial_GL::NeedsRender() {
	lock_guard<mutex> guard(compileOutputMutex);
	for (size_t i = 0; i < compileOutput.size(); ++i)
		if (compileOutput[i].success)
			return true;
	return false;
}



void LiveMaterial_GL::Draw(int uniformIndex) {
    assert(glGetError() == GL_NO_ERROR); // Make sure no OpenGL error happen before starting rendering
    
    compileNewShaders();
    if (_program == 0)
        return;

    glUseProgram(_program);
    updateUniforms(uniformIndex);
    printOpenGLError();

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    printOpenGLError();
}

void LiveMaterial_GL::_SetTexture(const char* name, void* nativeTexturePtr) {
    if (_program == 0)
        return;

    lock_guard<mutex> guard(texturesMutex);
    auto iter = textureUnits.find(name);
    if (iter == textureUnits.end())
        return;
    
    auto textureID = (GLint)(size_t)nativeTexturePtr;
    auto textureUnit = iter->second;
    textureIDs[textureUnit] = textureID;           
}

void LiveMaterial_GL::_QueueCompileTasks(vector<CompileTask> tasks) {
    lock_guard<mutex> guard(compileTaskMutex);
    for (size_t i = 0; i < tasks.size(); ++i)
        compileTasks.push_back(tasks[i]);
}

void LiveMaterial_GL::LinkProgram() {
    GLuint program = glCreateProgram();
    assert(program > 0);
    //glBindAttribLocation(program, ATTRIB_POSITION, "xlat_attrib_POSITION");
    //glBindAttribLocation(program, ATTRIB_COLOR, "xlat_attrib_COLOR");
    //glBindAttribLocation(program, ATTRIB_UV, "xlat_attrib_TEXCOORD0");
    glAttachShader(program, _vertexShader);
    glAttachShader(program, _fragmentShader);
#if SUPPORT_OPENGL_CORE
    if (((RenderAPI_OpenGLCoreES*)_renderAPI)->IsOpenGLCore())
        glBindFragDataLocationEXT(program, 0, "fragColor");
#endif
    glLinkProgram(program);
    
    GLint status = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &status);
    
    if (status == GL_TRUE) {
        if (_program)
            glDeleteProgram(_program);
        _program = program;
        //stats.compileState = CompileState::Success;
    } else {
        Debug("failure linking program:");
        //stats.compileState = CompileState::Error;
        
        GLint infoLen = 0;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &infoLen);
        if (infoLen > 0) {
            char* infoLog = (char*)malloc (sizeof(char) * infoLen);
            glGetProgramInfoLog(program, infoLen, nullptr, infoLog);
            Debug(infoLog);
            free(infoLog);
        }
    }
}

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
    
    
    //if (type == GL_FRAGMENT_SHADER)
        //stats.compileState = CompileState::Compiling;
    glShaderSource(shader, 1, &shaderSrc, NULL);
    glCompileShader(shader);
    
    GLint compiled;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        GLint infoLen = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &infoLen);
        Debug("error compiling glsl shader:");
        //if (type == GL_FRAGMENT_SHADER)
            //stats.compileState = CompileState::Error;
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


void LiveMaterial_GL::compileNewShaders() {
    bool needsUpdate = false;
    vector<CompileTask> tasks;
    {
        lock_guard<mutex> guard(compileTaskMutex);
        tasks = compileTasks;
        compileTasks.clear();
    }
    
    bool error = false;

    for (size_t i = 0; i < tasks.size(); ++i) {
        auto compileTask = tasks[i];
        GLenum glType;
        GLuint* storedProgram;
        switch (compileTask.shaderType) {
            case Fragment:
                glType = GL_FRAGMENT_SHADER;
                storedProgram = &_fragmentShader;
                break;
            case Vertex:
                glType = GL_VERTEX_SHADER;
                storedProgram = &_vertexShader;
                break;
            default:
                assert(false);
                continue;
        }
        GLuint newShader = loadShader(glType, compileTask.src.c_str(), nullptr);
        if (newShader) {
            if (*storedProgram)
                glDeleteShader(*storedProgram);
            *storedProgram = newShader;
            needsUpdate = true;
        } else {
            error = true;
        }
    }

    if (needsUpdate) {
        LinkProgram();
        if (_program) {
            _discoverUniforms(_program);
        }
    }
    
    _stats.compileState = error ? CompileState::Error : CompileState::Success;
    
    printOpenGLError();
}

void LiveMaterial_GL::_discoverUniforms(GLuint program) {
    lock_guard<mutex> uniformsGuard(uniformsMutex);
    lock_guard<mutex> texturesGuard(texturesMutex);
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
#ifdef WIN32
                        strncpy_s(name, maxNameLength, namestr.c_str(), s - 3);
#else
                        strncpy(name, namestr.c_str(), s - 3);
#endif
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
                        uniformLocs.push_back(glGetUniformLocation(program, name));
                        continue; // don't make a prop
                    }
                    default:
                        const char* typeName = nullptr; //getGLTypeName(type);
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

void LiveMaterial_GL::updateUniforms(int uniformIndex) {

    // Bind textures
    {
        lock_guard<mutex> guard(texturesMutex);
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
    }

    // Set uniforms
    {
        lock_guard<mutex> guard(uniformsMutex);
        for (auto i = shaderProps.begin(); i != shaderProps.end(); i++) {
            auto prop = i->second;
            
            if (prop->uniformIndex == ShaderProp::UNIFORM_UNSET || prop->uniformIndex == prop->UNIFORM_INVALID) {
                //errors << "invalid shader variable " << prop->name << "\n";
                continue;
            }

            auto data = (float*)(_constantBuffer + prop->offset);

            switch (prop->type) {
            case Float:
                glUniform1fv(prop->uniformIndex, prop->arraySize, data);
                break;
            case Vector2:
                glUniform2fv(prop->uniformIndex, prop->arraySize, data);
                break;
            case Vector3:
                glUniform3fv(prop->uniformIndex, prop->arraySize, data);
                break;
            case Vector4:
                glUniform4fv(prop->uniformIndex, prop->arraySize, data);
                break;
            case Matrix: {
                const int numElements = prop->arraySize;
                const bool transpose = GL_FALSE;
                glUniformMatrix4fv(prop->uniformIndex, numElements, transpose, data);
                break;
            }
            default:
                assert(false);
            }

            //string errorStr(errors.str());
            //if (errorStr.size()) Debug(errorStr.c_str());
                    
            if (printOpenGLError())
                DebugSS("error setting uniform " << prop->name << " with type " << prop->typeString() << " and uniform index " << prop->uniformIndex);
        }
    }
}


RenderAPI* CreateRenderAPI_OpenGLCoreES(UnityGfxRenderer apiType)
{
	return new RenderAPI_OpenGLCoreES(apiType);
}

bool RenderAPI_OpenGLCoreES::supportsBackgroundCompiles() {
    return false;
}

enum VertexInputs
{
	kVertexInputPosition = 0,
	kVertexInputColor = 1
};


// Simple vertex shader source
#define VERTEX_SHADER_SRC(ver, attr, varying)						\
	ver																\
	attr " highp vec3 pos;\n"										\
	attr " lowp vec4 color;\n"										\
	"\n"															\
	varying " lowp vec4 ocolor;\n"									\
	"\n"															\
	"uniform highp mat4 worldMatrix;\n"								\
	"uniform highp mat4 projMatrix;\n"								\
	"\n"															\
	"void main()\n"													\
	"{\n"															\
	"	gl_Position = (projMatrix * worldMatrix) * vec4(pos,1);\n"	\
	"	ocolor = color;\n"											\
	"}\n"															\

static const char* kGlesVProgTextGLES2 = VERTEX_SHADER_SRC("\n", "attribute", "varying");
static const char* kGlesVProgTextGLES3 = VERTEX_SHADER_SRC("#version 300 es\n", "in", "out");
#if SUPPORT_OPENGL_CORE
static const char* kGlesVProgTextGLCore = VERTEX_SHADER_SRC("#version 150\n", "in", "out");
#endif

#undef VERTEX_SHADER_SRC


// Simple fragment shader source
#define FRAGMENT_SHADER_SRC(ver, varying, outDecl, outVar)	\
	ver												\
	outDecl											\
	varying " lowp vec4 ocolor;\n"					\
	"\n"											\
	"void main()\n"									\
	"{\n"											\
	"	" outVar " = ocolor;\n"						\
	"}\n"											\

static const char* kGlesFShaderTextGLES2 = FRAGMENT_SHADER_SRC("\n", "varying", "\n", "gl_FragColor");
static const char* kGlesFShaderTextGLES3 = FRAGMENT_SHADER_SRC("#version 300 es\n", "in", "out lowp vec4 fragColor;\n", "fragColor");
#if SUPPORT_OPENGL_CORE
static const char* kGlesFShaderTextGLCore = FRAGMENT_SHADER_SRC("#version 150\n", "in", "out lowp vec4 fragColor;\n", "fragColor");
#endif

#undef FRAGMENT_SHADER_SRC


static GLuint CreateShader(GLenum type, const char* sourceText)
{
	GLuint ret = glCreateShader(type);
	glShaderSource(ret, 1, &sourceText, nullptr);
	glCompileShader(ret);
	return ret;
}


void RenderAPI_OpenGLCoreES::CreateResources()
{
	// Create shaders
	if (m_APIType == kUnityGfxRendererOpenGLES20)
	{
		m_VertexShader = CreateShader(GL_VERTEX_SHADER, kGlesVProgTextGLES2);
		m_FragmentShader = CreateShader(GL_FRAGMENT_SHADER, kGlesFShaderTextGLES2);
	}
	else if (m_APIType == kUnityGfxRendererOpenGLES30)
	{
		m_VertexShader = CreateShader(GL_VERTEX_SHADER, kGlesVProgTextGLES3);
		m_FragmentShader = CreateShader(GL_FRAGMENT_SHADER, kGlesFShaderTextGLES3);
	}
#	if SUPPORT_OPENGL_CORE
	else if (m_APIType == kUnityGfxRendererOpenGLCore)
	{
		glewExperimental = GL_TRUE;
		glewInit();
		glGetError(); // Clean up error generated by glewInit

		m_VertexShader = CreateShader(GL_VERTEX_SHADER, kGlesVProgTextGLCore);
		m_FragmentShader = CreateShader(GL_FRAGMENT_SHADER, kGlesFShaderTextGLCore);
	}
#	endif // if SUPPORT_OPENGL_CORE


	// Link shaders into a program and find uniform locations
	m_Program = glCreateProgram();
	glBindAttribLocation(m_Program, kVertexInputPosition, "pos");
	glBindAttribLocation(m_Program, kVertexInputColor, "color");
	glAttachShader(m_Program, m_VertexShader);
	glAttachShader(m_Program, m_FragmentShader);
#	if SUPPORT_OPENGL_CORE
	if (m_APIType == kUnityGfxRendererOpenGLCore)
		glBindFragDataLocation(m_Program, 0, "fragColor");
#	endif // if SUPPORT_OPENGL_CORE
	glLinkProgram(m_Program);

	GLint status = 0;
	glGetProgramiv(m_Program, GL_LINK_STATUS, &status);
	assert(status == GL_TRUE);

	m_UniformWorldMatrix = glGetUniformLocation(m_Program, "worldMatrix");
	m_UniformProjMatrix = glGetUniformLocation(m_Program, "projMatrix");

	// Create vertex buffer
	glGenBuffers(1, &m_VertexBuffer);
	glBindBuffer(GL_ARRAY_BUFFER, m_VertexBuffer);
	glBufferData(GL_ARRAY_BUFFER, 1024, nullptr, GL_STREAM_DRAW);

	assert(glGetError() == GL_NO_ERROR);
}


RenderAPI_OpenGLCoreES::RenderAPI_OpenGLCoreES(UnityGfxRenderer apiType)
	: m_APIType(apiType)
{
}


void RenderAPI_OpenGLCoreES::ProcessDeviceEvent(UnityGfxDeviceEventType type, IUnityInterfaces* interfaces)
{
	if (type == kUnityGfxDeviceEventInitialize)
	{
		CreateResources();
	}
	else if (type == kUnityGfxDeviceEventShutdown)
	{
		//@TODO: release resources
	}
}


void RenderAPI_OpenGLCoreES::DrawSimpleTriangles(const float worldMatrix[16], int triangleCount, const void* verticesFloat3Byte4)
{
	// Set basic render state
	glDisable(GL_CULL_FACE);
	glDisable(GL_BLEND);
	glDepthFunc(GL_LEQUAL);
	glEnable(GL_DEPTH_TEST);
	glDepthMask(GL_FALSE);

	// Tweak the projection matrix a bit to make it match what identity projection would do in D3D case.
	float projectionMatrix[16] = {
		1,0,0,0,
		0,1,0,0,
		0,0,2,0,
		0,0,-1,1,
	};

	// Setup shader program to use, and the matrices
	glUseProgram(m_Program);
	glUniformMatrix4fv(m_UniformWorldMatrix, 1, GL_FALSE, worldMatrix);
	glUniformMatrix4fv(m_UniformProjMatrix, 1, GL_FALSE, projectionMatrix);

	// Core profile needs VAOs, setup one
#	if SUPPORT_OPENGL_CORE
	if (m_APIType == kUnityGfxRendererOpenGLCore)
	{
		glGenVertexArrays(1, &m_VertexArray);
		glBindVertexArray(m_VertexArray);
	}
#	endif // if SUPPORT_OPENGL_CORE

	// Bind a vertex buffer, and update data in it
	const int kVertexSize = 12 + 4;
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ARRAY_BUFFER, m_VertexBuffer);
	glBufferSubData(GL_ARRAY_BUFFER, 0, kVertexSize * triangleCount * 3, verticesFloat3Byte4);

	// Setup vertex layout
	glEnableVertexAttribArray(kVertexInputPosition);
	glVertexAttribPointer(kVertexInputPosition, 3, GL_FLOAT, GL_FALSE, kVertexSize, (char*)NULL + 0);
	glEnableVertexAttribArray(kVertexInputColor);
	glVertexAttribPointer(kVertexInputColor, 4, GL_UNSIGNED_BYTE, GL_TRUE, kVertexSize, (char*)NULL + 12);

	// Draw
	glDrawArrays(GL_TRIANGLES, 0, triangleCount * 3);

	// Cleanup VAO
#	if SUPPORT_OPENGL_CORE
	if (m_APIType == kUnityGfxRendererOpenGLCore)
	{
		glDeleteVertexArrays(1, &m_VertexArray);
	}
#	endif
}


void* RenderAPI_OpenGLCoreES::BeginModifyTexture(void* textureHandle, int textureWidth, int textureHeight, int* outRowPitch)
{
	const int rowPitch = textureWidth * 4;
	// Just allocate a system memory buffer here for simplicity
	unsigned char* data = new unsigned char[rowPitch * textureHeight];
	*outRowPitch = rowPitch;
	return data;
}


void RenderAPI_OpenGLCoreES::EndModifyTexture(void* textureHandle, int textureWidth, int textureHeight, int rowPitch, void* dataPtr)
{
	GLuint gltex = (GLuint)(size_t)(textureHandle);
	// Update texture data, and free the memory buffer
	glBindTexture(GL_TEXTURE_2D, gltex);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, textureWidth, textureHeight, GL_RGBA, GL_UNSIGNED_BYTE, dataPtr);
	delete[](unsigned char*)dataPtr;
}

LiveMaterial* RenderAPI_OpenGLCoreES::_newLiveMaterial(int id) { return new LiveMaterial_GL(this, id); }


#endif // #if SUPPORT_OPENGL_UNIFIED
