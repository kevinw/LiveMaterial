using UnityEngine;
using UnityEngine.Assertions;
using System.Collections;

public class StressLiveMaterial : MonoBehaviour {
    public int NumLiveMaterialsToMake = 10;
    public Texture2D testTexture;
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
float floatVal;
float4 vectorVal;
float4 vectors[50];
sampler2D testTexture;

FragOutput fragSimple(FragInput fragInput) {
	// test uv inputs
	FragOutput output;
    float2 uv = fragInput.uv;
	output.color = float4(uv.x, uv.y, 1, 1);

    output.color *= tex2D(testTexture, uv);

    for (int i = 0; i < 50; ++i) {
        float2 pos = vectors[i].xy;
        if (length(pos - uv) < 0.05)
            output.color = color;
    }

    if (fragInput.uv.x > floatVal && fragInput.uv.x < vectorVal.x &&
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
                var m = liveMaterials[i];
                m.SetShaderSource(fragSrc, fragEntry, vertSrc, vertEntry);
            }

            yield return new WaitForSeconds(delay);

            for (int i = 0; i < liveMaterials.Length; ++i) {
                var m = liveMaterials[i];
                var color = new Color(Random.Range(0, 1f), Random.Range(0, 1f), Random.Range(0, 1f), 1.0f);
                m.SetColor("color", color);
                Assert.IsTrue(m.HasProperty("color"));
                Assert.AreEqual(color, m.GetColor("color"));

                var floatVal = Random.Range(0, 0.4f);
                m.SetFloat("floatVal", floatVal);
                Assert.AreEqual(floatVal, m.GetFloat("floatVal"));

                Vector4 vec4Val = Random.insideUnitSphere + Vector3.one * 0.5f;
                m.SetVector4("vectorVal", vec4Val);
                Assert.AreEqual(vec4Val, m.GetVector4("vectorVal"));

                Vector4[] vectors = new Vector4[50];
                for (int j = 0; j < vectors.Length; ++j)
                    vectors[j] = Random.insideUnitCircle + Vector2.one * 0.5f;
                m.SetVectorArray("vectors", vectors);

                if (testTexture != null)
                    m.SetTexture("testTexture", testTexture);
            }

            yield return new WaitForSeconds(delay);
        } while (loop);
    }
}
