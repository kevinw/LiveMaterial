// on OpenGL ES there is no way to query texture extents from native texture id
#if (UNITY_IPHONE || UNITY_ANDROID) && !UNITY_EDITOR
	#define UNITY_GLES_RENDERER
#endif

//#define LIVE_RELOAD

using UnityEngine;
using System;
using System.Collections;
using System.Runtime.InteropServices;




public class UseRenderingPlugin : MonoBehaviour
{
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void MyDelegate(string str);

	static void CallBackFunction(string str) {
	    Debug.Log("[plugin] " + str);
	}

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
        if (nativeLibraryPtr != IntPtr.Zero)
            return;

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
        Debug.Log("DebugInfo: " + Marshal.PtrToStringAnsi(debugInfo));
    }

#if LIVE_RELOAD
    void OnApplicationQuit() {
        if (nativeLibraryPtr == IntPtr.Zero) return;
        Debug.Log(Native.FreeLibrary(nativeLibraryPtr)
                      ? "Native library successfully unloaded."
                      : "Native library could not be unloaded.");
    }
#endif

    IEnumerator Start()
	{
#if LIVE_RELOAD
        Native.Invoke<SetUnityStreamingAssetsPath>(nativeLibraryPtr, Application.streamingAssetsPath);
#else
        SetUnityStreamingAssetsPath(Application.streamingAssetsPath);
#endif

		CreateTextureAndPassToPlugin();
		yield return StartCoroutine("CallPluginAtEndOfFrames");
	}

	private void CreateTextureAndPassToPlugin()
	{
		// Create a texture
		Texture2D tex = new Texture2D(256,256,TextureFormat.ARGB32,false);
		// Set point filtering just so we can see the pixels clearly
		tex.filterMode = FilterMode.Point;
		// Call Apply() so it's actually uploaded to the GPU
		tex.Apply();

		// Set texture onto our matrial
		GetComponent<Renderer>().material.mainTexture = tex;

        // Pass texture pointer to the plugin

        var texturePtr = tex.GetNativeTexturePtr();

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

    private IEnumerator CallPluginAtEndOfFrames()
	{
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
            var renderEventFunc = Native.Invoke<IntPtr, GetRenderEventFunc>(nativeLibraryPtr);
#else
            var renderEventFunc = GetRenderEventFunc();
#endif
            GL.IssuePluginEvent(renderEventFunc, 1);
		}
	}
}
