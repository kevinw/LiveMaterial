using UnityEngine;
using UnityEngine.Assertions;
using System;
using System.Collections;
using System.Runtime.InteropServices;

using Random = UnityEngine.Random;


public class LiveMaterial : MonoBehaviour
{
    [UnmanagedFunctionPointer(CallingConvention.StdCall)] delegate void DebugLogFunc(string str);

    struct Native {
        const string PluginName = "RenderingPlugin";
        [DllImport(PluginName)] internal static extern void SetTimeFromUnity(float t);
        [DllImport(PluginName)] internal static extern void SetTextureFromUnity(IntPtr texture, int w, int h);
        [DllImport(PluginName)] internal static extern IntPtr GetRenderEventFunc();
        [DllImport(PluginName)] internal static extern void SetCallbackFunctions(IntPtr debugLogFunc);

        [DllImport(PluginName)] internal static extern IntPtr CreateLiveMaterial();
        [DllImport(PluginName)]
        internal static extern int GetLiveMaterialId(IntPtr nativePtr);
        [DllImport(PluginName)] internal static extern void DestroyLiveMaterial(IntPtr nativePtr);
        [DllImport(PluginName)] internal static extern void SetShaderSource(IntPtr nativePtr, string fragSrc, string fragEntry, string vertSrc, string vertEntry);
        [DllImport(PluginName)] internal static extern void SetVector4(IntPtr nativePtr, string name, Vector4 value);
        [DllImport(PluginName)] internal static extern void SubmitUniforms(IntPtr nativePtr, int uniformsIndex);
    }

    private static void DebugWrapper(string log) { Debug.Log(log); }
    static readonly DebugLogFunc debugLogFunc = new DebugLogFunc(DebugWrapper);

    const int ID_UNSET = -1;
    const int ID_DESTROYED = -2;
    public int _nativeId = ID_UNSET;

    public IntPtr _nativePtr = IntPtr.Zero;

    public int NativeId {
        get {
            if (_nativeId == ID_UNSET && _nativePtr != IntPtr.Zero)
                _nativeId = Native.GetLiveMaterialId(_nativePtr);
            return _nativeId;
        }
    }
    
    public IntPtr NativePtr {
        get {
            if (_nativePtr == IntPtr.Zero)
                _nativePtr = Native.CreateLiveMaterial();
            return _nativePtr;
        }
    }

    public void SetShaderSource(string fragSrc, string fragEntry, string vertSrc, string vertEntry) { Native.SetShaderSource(NativePtr, fragSrc, fragEntry, vertSrc, vertEntry); }
    public void SetColor(string name, Color color) { SetVector4(name, color); }
    public void SetVector4(string name, Vector4 vector) { Native.SetVector4(NativePtr, name, vector); }
    public void SubmitUniforms(int uniformsIndex) { Native.SubmitUniforms(NativePtr, uniformsIndex); }

#if UNITY_EDITOR
    static bool didInit = false;
#endif

    void Awake() {
#if UNITY_EDITOR
        if (!didInit) {
            // for reattaching plugin functions when unity reloads scripts
            didInit = true;
            AppDomain.CurrentDomain.DomainUnload += OnCurrentDomainUnload;
            Native.SetCallbackFunctions(Marshal.GetFunctionPointerForDelegate(debugLogFunc));
        }
#endif
    }

    void OnCurrentDomainUnload(object sender, EventArgs e) {
        // when unity reloads scripts, it "recreates" the mono "domain," and
        // the plugin's reference to the Debug.Log function will be made invalid.
        Native.SetCallbackFunctions(IntPtr.Zero);
    }

	IEnumerator Start() {

		//CreateTextureAndPassToPlugin();
		yield return StartCoroutine(CallPluginAtEndOfFrames());
	}

    void OnDestroy() {
        if (_nativePtr != IntPtr.Zero) {
            Native.DestroyLiveMaterial(_nativePtr);
            _nativeId = ID_DESTROYED;
            _nativePtr = new IntPtr(-1);
        }
    }

	private void CreateTextureAndPassToPlugin() {
		//Texture2D tex = new Texture2D(256,256,TextureFormat.ARGB32,false);
		//tex.filterMode = FilterMode.Point;
		//tex.Apply();

        //var renderer = GetComponent<Renderer>();
        //if (renderer)
            //renderer.material.mainTexture = tex;

		//Native.SetTextureFromUnity (tex.GetNativeTexturePtr(), tex.width, tex.height);
	}

	private IEnumerator CallPluginAtEndOfFrames() {
		while (true) {
			yield return new WaitForEndOfFrame();
			Native.SetTimeFromUnity (Time.timeSinceLevelLoad);

            if (_nativePtr != IntPtr.Zero && _nativePtr != new IntPtr(-1)) {
                Assert.IsTrue(NativeId <= Int16.MaxValue);
                Int16 id = (Int16)NativeId;
                Int16 uniformsIndex = 0;
                Int32 packedValue = (id << 16) | (uniformsIndex & 0xffff);


                var color = new Color(Random.Range(0, 1f), Random.Range(0, 1f), Random.Range(0, 1f), 1.0f);
                SetColor("color", color);

                SubmitUniforms(0);
                GL.IssuePluginEvent(Native.GetRenderEventFunc(), packedValue);
            }
		}
	}
}
