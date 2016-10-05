using UnityEngine;
using System.Collections;

public class StressLiveMaterial : MonoBehaviour {
    public int NumLiveMaterialsToMake = 10;
    public float delay = 1.0f;
    public bool loop = true;

	void Start () {
        StartCoroutine(Test());
	}

    static public string vertEntry = "vertFullscreenQuad";
    static public string vertSrc = @"
struct FragOutput {
    float4 color : SV_Target;
};

struct FragInput {
  float4 vertex : SV_POSITION;
  float2 uv : TEXCOORD0;
};

// takes a vertex id as input. must be drawn as a triangle strip.
FragInput vertFullscreenQuad(uint vI : SV_VERTEXID) {
	float2 texcoord = float2(vI & 1, vI >> 1);
	FragInput fragInput;
	fragInput.vertex = float4((texcoord.x - 0.5f) * 2, -(texcoord.y - 0.5f) * 2, 0, 1);
	fragInput.uv = texcoord;
	return fragInput;
}
";

    static public string fragEntry = "fragSimple";
    static public string fragSrc = @"
struct FragOutput {
    float4 color : SV_Target;
};

struct FragInput {
  float4 vertex : SV_POSITION;
  float2 uv : TEXCOORD0;
};

float4 color;

FragOutput fragSimple(FragInput fragInput) {
	// test uv inputs
	FragOutput output;
	output.color = float4(fragInput.uv.x, fragInput.uv.y, 1, 1);
    if (fragInput.uv.x > 0.25 && fragInput.uv.x < .75 &&
        fragInput.uv.y > 0.25 && fragInput.uv.y < .75) {
        output.color = color;
    }
    
	return output;
}
";

    IEnumerator Test() {
        int count = 0;
        do {
            int childs = transform.childCount;
            for (int i = childs - 1; i >= 0; i--)
                Destroy(transform.GetChild(i).gameObject);

            yield return new WaitForSeconds(delay);

            var liveMaterials = new LiveMaterial[NumLiveMaterialsToMake];

            for (int i = 0; i < NumLiveMaterialsToMake; ++i) {
                var obj = new GameObject("test " + (++count));
                obj.transform.SetParent(transform);
                var liveMaterial = obj.AddComponent<LiveMaterial>();
                liveMaterials[i] = liveMaterial;
            }

            yield return null; // wait for live materials to register

            for (int i = 0; i < liveMaterials.Length; ++i) {
                liveMaterials[i].SetShaderSource(fragSrc, fragEntry, vertSrc, vertEntry);
            }

            yield return new WaitForSeconds(delay);
        } while (loop);
    }
}
