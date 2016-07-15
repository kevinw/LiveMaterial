using System;
using UnityEngine;

public class ShaderGen : MonoBehaviour {
    [Serializable]
    public struct State {
        public Color backgroundColor;
    }

    public State state;
    State _lastState;
    string generatedShaderText;    
    
    public string FragmentShaderText {
        get {
            GenerateShader();
            return generatedShaderText;
        }
    }
    
    void GenerateShader() {
        if (state.Equals(_lastState)) return;
        _lastState = state;

        generatedShaderText = @"
float4 PS (float4 color : COLOR) : SV_TARGET
{
	return float4( " + state.backgroundColor.r + ", " + state.backgroundColor.g + ", " + state.backgroundColor.b + ", " + state.backgroundColor.a + @" );
}
";
    }
}
