#include <d3dcompiler.h>
#pragma comment(lib, "d3dcompiler.lib")

#include <memory>
#include <tchar.h>
/*
#include <ppltasks.h>
#include <d3dcompiler.h>
#include <Robuffer.h>

void test()
{
	std::shared_ptr<Microsoft::WRL::ComPtr<ID3DBlob>> blobRef = std::make_shared<Microsoft::WRL::ComPtr<ID3DBlob>>();

	// Load a file and compile it.
	auto fileOp = Windows::ApplicationModel::Package::Current->InstalledLocation->GetFileAsync(L"shader.hlsl");
	create_task(fileOp).then([this](Windows::Storage::StorageFile^ file) -> IAsyncOperation<Windows::Storage::Streams::IBuffer^>^
	{
		// Do file I/O in background thread (use_arbitrary).
		return Windows::Storage::FileIO::ReadBufferAsync(file);
	}, task_continuation_context::use_arbitrary())
		.then([this, blobRef](Windows::Storage::Streams::IBuffer^ buffer)
	{
		// Do compilation in background thread (use_arbitrary).

		// Cast to Object^, then to its underlying IInspectable interface.
		Microsoft::WRL::ComPtr<IInspectable> insp(reinterpret_cast<IInspectable*>(buffer));

		// Query the IBufferByteAccess interface.
		Microsoft::WRL::ComPtr<Windows::Storage::Streams::IBufferByteAccess> bufferByteAccess;
		insp.As(&bufferByteAccess);

		// Retrieve the buffer data.
		byte *pBytes = nullptr;
		bufferByteAccess->Buffer(&pBytes);

		Microsoft::WRL::ComPtr<ID3DBlob> blob;
		Microsoft::WRL::ComPtr<ID3DBlob> errMsgs;
		D3DCompile2(pBytes, buffer->Length, "shader.hlsl", nullptr, nullptr, "main", "ps_5_0", 0, 0, 0, nullptr, 0, blob.GetAddressOf(), errMsgs.GetAddressOf());
		*blobRef = blob;
	}, task_continuation_context::use_arbitrary())
		.then([this, blobRef]()
	{
		// Update UI / use shader on foreground thread.
		wchar_t message[40];
		swprintf_s(message, L"blob is %u bytes long", (unsigned)(*blobRef)->GetBufferSize());
		this->TheButton->Content = ref new Platform::String(message);
	}, task_continuation_context::use_current());
}
*/


HRESULT CompileShader(_In_ const char* src, _In_ LPCSTR srcName, _In_ LPCSTR entryPoint, _In_ LPCSTR profile, const D3D_SHADER_MACRO defines[], _Outptr_ ID3DBlob** blob, _Outptr_ ID3DBlob** errorBlob)
{
	if (!src || !srcName || !entryPoint || !profile || !blob)
		return E_INVALIDARG;

	*blob = nullptr;
	*errorBlob = nullptr;

	UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;// D3DCOMPILE_ENABLE_STRICTNESS;
//#if defined( DEBUG ) || defined( _DEBUG )
//	flags |= D3DCOMPILE_DEBUG;
//#endif



	ID3DBlob* shaderBlob = nullptr;
	HRESULT hr = D3DCompile(src, strlen(src), srcName, defines, D3D_COMPILE_STANDARD_FILE_INCLUDE,
		entryPoint, profile,
		flags, 0, &shaderBlob, errorBlob);

	if (FAILED(hr)) {
		if (shaderBlob) {
			shaderBlob->Release();
		}

		return hr;
	}

	*blob = shaderBlob;
	return hr;
}