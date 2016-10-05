using UnityEngine;
using System.Collections;

public class StressLiveMaterial : MonoBehaviour {
    public int NumLiveMaterialsToMake = 10;
    public float delay = 1.0f;
	void Start () {
        StartCoroutine(Test());
	}

    IEnumerator Test() {
        int count = 0;
        while (true) {
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
                liveMaterials[i].SetShaderSource("foo", "bar", "meep", "baz");
            }

            yield return new WaitForSeconds(delay);
        }
    }
}
