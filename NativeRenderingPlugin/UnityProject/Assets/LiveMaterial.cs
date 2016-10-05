using UnityEngine;
using UnityEngine.Assertions;
using System;
using System.Collections;
using System.Runtime.InteropServices;
#if UNITY_EDITOR
using UnityEditor;
#endif

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
        [DllImport(PluginName)] internal static extern void SetVector4(IntPtr nativePtr, string name, float[] value);
        [DllImport(PluginName)] internal static extern void SetFloatArray(IntPtr nativePtr, string name, float[] value, int numFloats);
        [DllImport(PluginName)] internal static extern void SetMatrix(IntPtr nativePtr, string name, float[] value);
        [DllImport(PluginName)] internal static extern void SetFloat(IntPtr nativePtr, string name, float value);
        [DllImport(PluginName)] internal static extern float GetFloat(IntPtr nativePtr, string name);
        [DllImport(PluginName)] internal static extern void GetVector4(IntPtr nativePtr, string name, float[] value);
        [DllImport(PluginName)] internal static extern void GetMatrix(IntPtr nativePtr, string name, float[] value);
        [DllImport(PluginName)] internal static extern void SubmitUniforms(IntPtr nativePtr, int uniformsIndex);

        [DllImport(PluginName)] internal static extern void PrintUniforms(IntPtr nativePtr);
    }

    private static void DebugWrapper(string log) { Debug.Log(log); }
    static readonly DebugLogFunc debugLogFunc = new DebugLogFunc(DebugWrapper);

    const int ID_UNSET = -1;
    const int ID_DESTROYED = -2;
    int _nativeId = ID_UNSET;
    IntPtr _nativePtr = IntPtr.Zero;

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

    static float[] scratch = new float[16];
    static float[] arrayScratch;
    static void ensureArrayScratch(int numFloats) {
        if (arrayScratch == null || arrayScratch.Length < numFloats)
            arrayScratch = new float[numFloats];
    }

    public void SetShaderSource(string fragSrc, string fragEntry, string vertSrc, string vertEntry) { Native.SetShaderSource(NativePtr, fragSrc, fragEntry, vertSrc, vertEntry); }
    public void SetColor(string name, Color color) { SetVector4(name, color); }
    public void SetFloat(string name, float value) { Native.SetFloat(NativePtr, name, value);  }
    public void SetVectorArray(string name, Vector4[] values) {
        int numFloats = values.Length * 4;
        ensureArrayScratch(numFloats);
        int z = 0;
        for (int i = 0; i < values.Length; ++i) {
            arrayScratch[z++] = values[i].x;
            arrayScratch[z++] = values[i].y;
            arrayScratch[z++] = values[i].z;
            arrayScratch[z++] = values[i].w;
        }
        Assert.AreEqual(numFloats, z);
        Native.SetFloatArray(NativePtr, name, arrayScratch, numFloats);
    }
    public void SetMatrixArray(string name, Matrix4x4[] values) {
        int numFloats = values.Length * 16;
        ensureArrayScratch(numFloats);
        int z = 0;
        for (int i = 0; i < values.Length; ++i)
            for (int j = 0; j < 16; ++j)
                arrayScratch[z++] = values[i][j];
        Assert.AreEqual(numFloats, z);
        Native.SetFloatArray(NativePtr, name, arrayScratch, numFloats);
    }
    public void SetVector4(string name, Vector4 vector) {
        scratch[0] = vector.x;
        scratch[1] = vector.y;
        scratch[2] = vector.z;
        scratch[3] = vector.w;
        Native.SetVector4(NativePtr, name, scratch);
    }
    public void SetMatrix(string name, Matrix4x4 matrix) {
        for (int i = 0; i < 16; ++i)
            scratch[i] = matrix[i];
        Native.SetMatrix(NativePtr, name, scratch);
    }
    
    public Vector4 GetVector4(string name) {
        Native.GetVector4(NativePtr, name, scratch);
        return new Vector4(scratch[0], scratch[1], scratch[2], scratch[3]);
    }

    public float GetFloat(string name) { return Native.GetFloat(NativePtr, name);  }
    public Color GetColor(string name) { return GetVector4(name); }

    public Matrix4x4 GetMatrix(string name) {
        Native.GetMatrix(NativePtr, name, scratch);
        var m = new Matrix4x4();
        for (int j = 0; j < 16; ++j)
            m[j] = scratch[j];
        return m;
    }

    public void PrintUniforms() { Native.PrintUniforms(NativePtr); }

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

                SubmitUniforms(0);
                GL.IssuePluginEvent(Native.GetRenderEventFunc(), packedValue);
            }
		}
	}
}


#if UNITY_EDITOR
[CustomEditor(typeof(LiveMaterial))]
public class LiveMaterialEditor : Editor {
    public override void OnInspectorGUI() {
        DrawDefaultInspector();
        var mat = target as LiveMaterial;
        if (GUILayout.Button("Print Uniforms"))			
            mat.PrintUniforms();

        //var stats = LiveMaterial.GetStats();
        //GUILayout.Label("Instruction Count: " + stats.instructionCount);
        //GUILayout.Label("Last Compile Time: " + stats.compileTimeMs + "ms");      
    }
}

#endif
