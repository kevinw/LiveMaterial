using UnityEngine;
using UnityEngine.Assertions;
using UnityEngine.Rendering;
using System;
using System.Collections;
using System.Runtime.InteropServices;

public class AddCommandBuffer : MonoBehaviour {
    public string CommandBufferName = "Manual CommandBuffer";
    public CameraEvent cameraEvent = CameraEvent.AfterForwardOpaque;

    public IntPtr NativeCallback;
    public int EventId;

    public void SetGLPluginEvent(IntPtr NativeCallback, int EventId) {
        this.NativeCallback = NativeCallback;
        this.EventId = EventId;
    }

    CommandBuffer createCommandBuffer() {
        Assert.AreNotEqual(NativeCallback, IntPtr.Zero);

        var commandBuffer = new CommandBuffer();
        commandBuffer.name = CommandBufferName;
        commandBuffer.IssuePluginEvent(NativeCallback, EventId);
        return commandBuffer;
    }

    CommandBuffer activeCommandBuffer;

    void Start() {
        activeCommandBuffer = createCommandBuffer();
        Camera.main.AddCommandBuffer(cameraEvent, activeCommandBuffer);	
	}

    void OnDisable() {
        if (activeCommandBuffer != null) {
            if (Camera.main)
                Camera.main.RemoveCommandBuffer(cameraEvent, activeCommandBuffer);
            activeCommandBuffer = null;
        }
    }
}
