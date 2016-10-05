using UnityEngine;
using System;
using System.Collections;
using System.Runtime.InteropServices;


public class LiveMaterial : MonoBehaviour
{
    [UnmanagedFunctionPointer(CallingConvention.StdCall)] delegate void DebugLogFunc(string str);

    struct Native {
        const string PluginName = "RenderingPlugin";
        [DllImport(PluginName)] internal static extern void SetTimeFromUnity(float t);
        [DllImport(PluginName)] internal static extern void SetTextureFromUnity(System.IntPtr texture, int w, int h);
        [DllImport(PluginName)] internal static extern IntPtr GetRenderEventFunc();
        [DllImport(PluginName)] internal static extern void SetCallbackFunctions(IntPtr debugLogFunc);

        [DllImport(PluginName)] internal static extern int CreateLiveMaterial();
        [DllImport(PluginName)] internal static extern void DestroyLiveMaterial(int id);
        [DllImport(PluginName)] internal static extern void SetShaderSource(int id, string fragSrc, string fragEntry, string vertSrc, string vertEntry);
    }

    private static void DebugWrapper(string log) { Debug.Log(log); }
    static readonly DebugLogFunc debugLogFunc = new DebugLogFunc(DebugWrapper);

    public int _nativeId = -1;

    public void SetShaderSource(string fragSrc, string fragEntry, string vertSrc, string vertEntry) {
        Native.SetShaderSource(_nativeId, fragSrc, fragEntry, vertSrc, vertEntry);
    }

#if UNITY_EDITOR
    static bool didInit = false;
#endif

    void Awake() {
        _nativeId = Native.CreateLiveMaterial();

#if UNITY_EDITOR
        if (!didInit) {
            // for reattaching plugin functions when unity reloads scripts
            AppDomain.CurrentDomain.DomainUnload += OnCurrentDomainUnload;
            Native.SetCallbackFunctions(Marshal.GetFunctionPointerForDelegate(debugLogFunc));
            didInit = true;
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
		yield return StartCoroutine("CallPluginAtEndOfFrames");
	}

    void OnDestroy() {
        Native.DestroyLiveMaterial(_nativeId);
        _nativeId = -1;
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
			GL.IssuePluginEvent(Native.GetRenderEventFunc(), 1);
		}
	}
}
