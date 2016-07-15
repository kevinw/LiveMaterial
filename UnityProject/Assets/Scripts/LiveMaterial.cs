// on OpenGL ES there is no way to query texture extents from native texture id
#if (UNITY_IPHONE || UNITY_ANDROID) && !UNITY_EDITOR
	#define UNITY_GLES_RENDERER
#endif

//#define LIVE_RELOAD

using UnityEngine;
using System;
using System.Collections;
using System.Runtime.InteropServices;

#if false && UNITY_EDITOR
using UnityEditor;
[CustomEditor(typeof(UseRenderingPlugin))]
public class UseRenderingPluginEditor : Editor {
	public override void OnInspectorGUI() {
		DrawDefaultInspector();

        var plugin = (UseRenderingPlugin)target;
        if (GUILayout.Button("Update Shader"))
        {
#if UNITY_EDITOR_OSX
            string frag = @"#version 150
out lowp vec4 fragColor;
in lowp vec4 ocolor;
void main() {
	fragColor = vec4(0, 1, 0, 1);
}";
#else
            string frag = @"
float4 PS (float4 color : COLOR) : SV_TARGET
{
	return float4(1,0,1,1);
}";
#endif
            plugin.SetShader (frag);
        }
	}
}
#endif

[RequireComponent(typeof(AddCommandBuffer))]
public class LiveMaterial : MonoBehaviour
{
    const int EventId = 1; // arbitrary plugin render event id
    public ShaderGen shaderGen;

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void MyDelegate(string str);

	static void CallBackFunction(string str) { Debug.Log("[plugin] " + str); }

#if LIVE_RELOAD
    delegate void SetTimeFromUnity(float t);
    delegate int SetDebugFunction(IntPtr fp);
#if UNITY_GLES_RENDERER
	delegate void SetTextureFromUnity(System.IntPtr texture, int w, int h);
#else
    delegate void SetTextureFromUnity(IntPtr texture);
#endif
    delegate void SetUnityStreamingAssetsPath([MarshalAs(UnmanagedType.LPStr)] string path);
    delegate IntPtr GetRenderEventFunc();
    delegate IntPtr GetDebugInfo();
    delegate void SetShaderSource(
        [MarshalAs(UnmanagedType.LPStr)] string frag,
        [MarshalAs(UnmanagedType.LPStr)] string vert);
#else
    
#if UNITY_IPHONE && !UNITY_EDITOR
	[DllImport ("__Internal")]
#else
    [DllImport ("RenderingPlugin")]
#endif
	private static extern void SetTimeFromUnity(float t);


#if UNITY_IPHONE && !UNITY_EDITOR
	[DllImport ("__Internal")]
#else
	[DllImport ("RenderingPlugin")]
#endif
	public static extern int SetDebugFunction( IntPtr fp );

#if UNITY_IPHONE && !UNITY_EDITOR
	[DllImport ("__Internal")]
#else
	[DllImport ("RenderingPlugin")]
#endif
	private static extern void SetShaderSource(
		[MarshalAs(UnmanagedType.LPStr)] string frag,
		[MarshalAs(UnmanagedType.LPStr)] string vert);

	// We'll also pass native pointer to a texture in Unity.
	// The plugin will fill texture data from native code.
#if UNITY_IPHONE && !UNITY_EDITOR
	[DllImport ("__Internal")]
#else
	[DllImport ("RenderingPlugin")]
#endif
#if UNITY_GLES_RENDERER
	private static extern void SetTextureFromUnity(System.IntPtr texture, int w, int h);
#else
	private static extern void SetTextureFromUnity(IntPtr texture);
#endif
    
#if UNITY_IPHONE && !UNITY_EDITOR
	[DllImport ("__Internal")]
#else
	[DllImport("RenderingPlugin")]
#endif
	private static extern void SetUnityStreamingAssetsPath([MarshalAs(UnmanagedType.LPStr)] string path);
    
#if UNITY_IPHONE && !UNITY_EDITOR
	[DllImport ("__Internal")]
#else
	[DllImport("RenderingPlugin")]
#endif
	private static extern IntPtr GetRenderEventFunc();
    
#if UNITY_IPHONE && !UNITY_EDITOR
	[DllImport ("__Internal")]
#else
    [DllImport("RenderingPlugin")]
#endif
    private static extern IntPtr GetDebugInfo();

#endif

#if LIVE_RELOAD
    static IntPtr nativeLibraryPtr;
#endif
    
    void Awake() {
#if LIVE_RELOAD
        if (nativeLibraryPtr != IntPtr.Zero) return;
        nativeLibraryPtr = Native.LoadLibrary("RenderingPlugin");
        if (nativeLibraryPtr == IntPtr.Zero)
            Debug.LogError("Failed to load native library");
#endif

        MyDelegate callback_delegate = new MyDelegate(CallBackFunction);

        // Convert callback_delegate into a function pointer that can be
        // used in unmanaged code.
        IntPtr intptr_delegate =
            Marshal.GetFunctionPointerForDelegate(callback_delegate);

        // Call the API passing along the function pointer.

#if LIVE_RELOAD
        var result = Native.Invoke<int, SetDebugFunction>(nativeLibraryPtr, intptr_delegate);
        var debugInfo = Native.Invoke<IntPtr, GetDebugInfo>(nativeLibraryPtr);
#else
        SetDebugFunction(intptr_delegate);
        var debugInfo = GetDebugInfo();
#endif
        Debug.Log("native plugin renderer type: " + Marshal.PtrToStringAnsi(debugInfo));

#if LIVE_RELOAD
        Native.Invoke<SetUnityStreamingAssetsPath>(nativeLibraryPtr, Application.streamingAssetsPath);
#else
        SetUnityStreamingAssetsPath(Application.streamingAssetsPath);
#endif
        CreateTextureAndPassToPlugin();

        GetComponent<AddCommandBuffer>().SetGLPluginEvent(GetRenderEventFunc(), 1);
    }

#if LIVE_RELOAD
    void OnApplicationQuit() {
        if (nativeLibraryPtr == IntPtr.Zero) return;
        Debug.Log(Native.FreeLibrary(nativeLibraryPtr)
                      ? "Native library successfully unloaded."
                      : "Native library could not be unloaded.");
    }
#endif

    IEnumerator Start() {        
		yield return StartCoroutine(CallPluginAtEndOfFrames());
	}

    void OnDisable() {
        _lastShader = null;
    }

    [TextArea(3, 20)]
    public string _lastShader;
    void Update() {
        if (!shaderGen)
            return;

        var fragShader = shaderGen.FragmentShaderText;
        if (_lastShader == fragShader)
            return;

        _lastShader = fragShader;
        SetShader(fragShader);
    }

	private void CreateTextureAndPassToPlugin()	{		
		var tex = new Texture2D(256, 256, TextureFormat.ARGB32, false); // Create a texture
        tex.filterMode = FilterMode.Point; // Set point filtering just so we can see the pixels clearly
        tex.Apply(); // Call Apply() so it's actually uploaded to the GPU

        GetComponent<Renderer>().material.mainTexture = tex; // Set texture onto our matrial
                
        var texturePtr = tex.GetNativeTexturePtr(); // Pass texture pointer to the plugin
#if UNITY_GLES_RENDERER
#if LIVE_RELOAD
        Native.Invoke<SetTextureFromUnity>(nativeLibraryPtr, texturePtr, tex.width, tex.height);
#else
		SetTextureFromUnity (texturePtr, tex.width, tex.height);
#endif
#else
#if LIVE_RELOAD
        Native.Invoke<SetTextureFromUnity>(nativeLibraryPtr, texturePtr);
#else
        SetTextureFromUnity (texturePtr);
#endif
#endif
    }

    private IEnumerator CallPluginAtEndOfFrames() {
		while (true) {
			// Wait until all frame rendering is done
			yield return new WaitForEndOfFrame();

            // Set time for the plugin
#if LIVE_RELOAD
            Native.Invoke<SetTimeFromUnity>(nativeLibraryPtr, Time.timeSinceLevelLoad);
#else
            SetTimeFromUnity (Time.timeSinceLevelLoad);
#endif
            // Issue a plugin event with arbitrary integer identifier.
            // The plugin can distinguish between different
            // things it needs to do based on this ID.
            // For our simple plugin, it does not matter which ID we pass here.
#if LIVE_RELOAD
            //var renderEventFunc = Native.Invoke<IntPtr, GetRenderEventFunc>(nativeLibraryPtr);
#else
            //var renderEventFunc = GetRenderEventFunc();
#endif
            //GL.IssuePluginEvent(renderEventFunc, EventId);
		}
	}

	public void SetShader(string frag) {
#if LIVE_RELOAD
            Native.Invoke<SetShaderSource>(nativeLibraryPtr, frag, "");
#else
            SetShaderSource(frag, "");
#endif
	}
}
