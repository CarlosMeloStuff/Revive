#include "CompositorBase.h"
#include "OVR_CAPI.h"
#include "REV_Math.h"
#include "Settings.h"
#include "Session.h"
#include "SessionDetails.h"
#include "microprofile.h"
#include "rcu_ptr.h"

#include <openvr.h>
#include <vector>
#include <algorithm>

#define REV_LAYER_BIAS 0.0001f

MICROPROFILE_DEFINE(WaitToBeginFrame, "Compositor", "WaitFrame", 0x00ff00);
MICROPROFILE_DEFINE(BeginFrame, "Compositor", "BeginFrame", 0x00ff00);
MICROPROFILE_DEFINE(EndFrame, "Compositor", "EndFrame", 0x00ff00);
MICROPROFILE_DEFINE(SubmitFovLayer, "Compositor", "SubmitFovLayer", 0x00ff00);
MICROPROFILE_DEFINE(SubmitSceneLayer, "Compositor", "SubmitSceneLayer", 0x00ff00);

ovrResult rev_CompositorErrorToOvrError(vr::EVRCompositorError error)
{
	switch (error)
	{
	case vr::VRCompositorError_None: return ovrSuccess;
	case vr::VRCompositorError_IncompatibleVersion: return ovrError_ServiceError;
	case vr::VRCompositorError_DoNotHaveFocus: return ovrSuccess_NotVisible;
	case vr::VRCompositorError_InvalidTexture: return ovrError_TextureSwapChainInvalid;
	case vr::VRCompositorError_IsNotSceneApplication: return ovrError_InvalidSession;
	case vr::VRCompositorError_TextureIsOnWrongDevice: return ovrError_TextureSwapChainInvalid;
	case vr::VRCompositorError_TextureUsesUnsupportedFormat: return ovrError_TextureSwapChainInvalid;
	case vr::VRCompositorError_SharedTexturesNotSupported: return ovrError_TextureSwapChainInvalid;
	case vr::VRCompositorError_IndexOutOfRange: return ovrError_InvalidParameter;
	case vr::VRCompositorError_AlreadySubmitted: return ovrSuccess_NotVisible;
	case vr::VRCompositorError_InvalidBounds: return ovrError_InvalidParameter;
	default: return ovrError_RuntimeException;
	}
}


CompositorBase::CompositorBase()
	: m_MirrorTexture(nullptr)
	, m_ChainCount(0)
{
}

CompositorBase::~CompositorBase()
{
	if (m_MirrorTexture)
		delete m_MirrorTexture;
}

ovrResult CompositorBase::CreateTextureSwapChain(const ovrTextureSwapChainDesc* desc, ovrTextureSwapChain* out_TextureSwapChain)
{
	ovrTextureSwapChain swapChain = new ovrTextureSwapChainData(*desc);
	swapChain->Identifier = m_ChainCount++;

	// FIXME: A bug in OpenVR causes Asynchronous Reprojection to fail with swapchains
	if (GetAPI() == vr::TextureType_OpenGL)
		swapChain->Length = 1;

	for (int i = 0; i < swapChain->Length; i++)
	{
		TextureBase* texture = CreateTexture();
		bool success = texture->Init(desc->Type, desc->Width, desc->Height, desc->MipLevels,
			desc->ArraySize, desc->Format, desc->MiscFlags, desc->BindFlags);
		if (!success)
			return ovrError_RuntimeException;
		swapChain->Textures[i].reset(texture);
	}

	*out_TextureSwapChain = swapChain;
	return ovrSuccess;
}

ovrResult CompositorBase::CreateMirrorTexture(const ovrMirrorTextureDesc* desc, ovrMirrorTexture* out_MirrorTexture)
{
	// There can only be one mirror texture at a time
	if (m_MirrorTexture)
		return ovrError_RuntimeException;

	// TODO: Support ovrMirrorOptions
	ovrMirrorTexture mirrorTexture = new ovrMirrorTextureData(*desc);
	TextureBase* texture = CreateTexture();
	bool success = texture->Init(ovrTexture_2D, desc->Width, desc->Height, 1, 1, desc->Format,
		desc->MiscFlags | ovrTextureMisc_AllowGenerateMips, ovrTextureBind_DX_RenderTarget);
	if (!success)
		return ovrError_RuntimeException;
	mirrorTexture->Texture.reset(texture);

	m_MirrorTexture = mirrorTexture;
	*out_MirrorTexture = mirrorTexture;
	return ovrSuccess;
}

ovrResult CompositorBase::WaitToBeginFrame(ovrSession session, long long frameIndex)
{
	MICROPROFILE_SCOPE(WaitToBeginFrame);

	vr::EVRCompositorError err = vr::VRCompositorError_None;
	for (long long index = session->FrameIndex; index < frameIndex; index++)
	{
		// Call WaitGetPoses to block until the running start, also known as queue-ahead in the Oculus SDK.
		err = vr::VRCompositor()->WaitGetPoses(nullptr, 0, nullptr, 0);
	}
	return rev_CompositorErrorToOvrError(err);
}

ovrResult CompositorBase::BeginFrame(ovrSession session, long long frameIndex)
{
	MICROPROFILE_SCOPE(BeginFrame);

	session->FrameIndex = frameIndex;
	return ovrSuccess;
}

ovrResult CompositorBase::EndFrame(ovrSession session, ovrLayerHeader const * const * layerPtrList, unsigned int layerCount)
{
	MICROPROFILE_SCOPE(EndFrame);

	if (layerCount == 0 || !layerPtrList)
		return ovrError_InvalidParameter;

	// Flush all pending draw calls.
	Flush();

	ovrLayerEyeFov baseLayer;
	bool baseLayerFound = false;
	std::vector<vr::VROverlayHandle_t> activeOverlays;
	for (uint32_t i = 0; i < layerCount; i++)
	{
		if (layerPtrList[i] == nullptr)
			continue;

		// TODO: Support ovrLayerType_Cylinder and ovrLayerType_Cube
		if (layerPtrList[i]->Type == ovrLayerType_Quad)
		{
			ovrLayerQuad* layer = (ovrLayerQuad*)layerPtrList[i];
			ovrTextureSwapChain chain = layer->ColorTexture;

			// Every overlay is associated with a swapchain.
			// This is necessary because the position of the layer may change in the array,
			// which would otherwise cause flickering between overlays.
			// TODO: Support multiple overlays using the same texture.
			vr::VROverlayHandle_t overlay = layer->ColorTexture->Overlay;
			if (overlay == vr::k_ulOverlayHandleInvalid)
			{
				overlay = CreateOverlay();
				layer->ColorTexture->Overlay = overlay;

				vr::VRTextureWithPose_t texture = chain->Textures[chain->SubmitIndex]->ToVRTexture();
				vr::VROverlay()->SetOverlayTexture(chain->Overlay, &texture);
			}
			activeOverlays.push_back(overlay);

			// Set the layer rendering order.
			vr::VROverlay()->SetOverlaySortOrder(overlay, i);

			// Transform the overlay.
			vr::HmdMatrix34_t transform = REV::Matrix4f(layer->QuadPoseCenter);
			vr::VROverlay()->SetOverlayWidthInMeters(overlay, layer->QuadSize.x);
			if (layer->Header.Flags & ovrLayerFlag_HeadLocked)
				vr::VROverlay()->SetOverlayTransformTrackedDeviceRelative(overlay, vr::k_unTrackedDeviceIndex_Hmd, &transform);
			else
				vr::VROverlay()->SetOverlayTransformAbsolute(overlay, session->TrackingOrigin, &transform);

			// Set the texture and show the overlay.
			vr::VRTextureBounds_t bounds = ViewportToTextureBounds(layer->Viewport, layer->ColorTexture, layer->Header.Flags);
			vr::Texture_t texture = chain->Textures[chain->SubmitIndex]->ToVRTexture();
			vr::VROverlay()->SetOverlayTextureBounds(overlay, &bounds);

			// Show the overlay, unfortunately we have no control over the order in which
			// overlays are drawn.
			// TODO: Support ovrLayerFlag_HighQuality for overlays with anisotropic sampling.
			// TODO: Handle overlay errors.
			vr::VROverlay()->ShowOverlay(overlay);
			chain->Submit();
		}
		else if (layerPtrList[i]->Type == ovrLayerType_EyeFov ||
			layerPtrList[i]->Type == ovrLayerType_EyeFovDepth ||
			layerPtrList[i]->Type == ovrLayerType_EyeFovMultires)
		{
			ovrLayerEyeFov* layer = (ovrLayerEyeFov*)layerPtrList[i];

			// We can only submit one eye layer, so once we have a base layer we blit the others.
			if (!baseLayerFound)
				baseLayer = *layer;
			else
				BlitFovLayers(&baseLayer, layer);
			baseLayerFound = true;
		}
		else if (layerPtrList[i]->Type == ovrLayerType_EyeMatrix)
		{
			ovrLayerEyeFov layer = ToFovLayer((ovrLayerEyeMatrix*)layerPtrList[i]);

			// We can only submit one eye layer, so once we have a base layer we blit the others.
			if (!baseLayerFound)
				baseLayer = layer;
			else
				BlitFovLayers(&baseLayer, &layer);
			baseLayerFound = true;
		}
	}

	// Hide previous overlays that are not part of the current layers.
	for (vr::VROverlayHandle_t overlay : m_ActiveOverlays)
	{
		// Find the overlay in the current active overlays, if it was not found then hide it.
		// TODO: Handle overlay errors.
		if (std::find(activeOverlays.begin(), activeOverlays.end(), overlay) == activeOverlays.end())
			vr::VROverlay()->HideOverlay(overlay);
	}
	m_ActiveOverlays = activeOverlays;

	vr::EVRCompositorError error = vr::VRCompositorError_None;
	if (baseLayerFound)
		error = SubmitFovLayer(session, &baseLayer);

	if (m_MirrorTexture && error == vr::VRCompositorError_None)
		RenderMirrorTexture(m_MirrorTexture);

	// Flip the profiler.
	MicroProfileFlip();

	return rev_CompositorErrorToOvrError(error);
}

vr::VROverlayHandle_t CompositorBase::CreateOverlay()
{
	// Each overlay needs a unique key, so just count how many overlays we've created until now.
	char keyName[vr::k_unVROverlayMaxKeyLength];
	snprintf(keyName, vr::k_unVROverlayMaxKeyLength, "revive.runtime.layer%d", m_OverlayCount++);

	vr::VROverlayHandle_t handle = vr::k_ulOverlayHandleInvalid;
	vr::VROverlay()->CreateOverlay((const char*)keyName, "Revive Layer", &handle);
	return handle;
}

vr::VRTextureBounds_t CompositorBase::ViewportToTextureBounds(ovrRecti viewport, ovrTextureSwapChain swapChain, unsigned int flags)
{
	vr::VRTextureBounds_t bounds;
	float w = (float)swapChain->Desc.Width;
	float h = (float)swapChain->Desc.Height;
	bounds.uMin = viewport.Pos.x / w;
	bounds.vMin = viewport.Pos.y / h;

	// Sanity check for the viewport size.
	// Workaround for Defense Grid 2, which leaves these variables uninitialized.
	if (viewport.Size.w > 0 && viewport.Size.h > 0)
	{
		bounds.uMax = (viewport.Pos.x + viewport.Size.w) / w;
		bounds.vMax = (viewport.Pos.y + viewport.Size.h) / h;
	}
	else
	{
		bounds.uMax = 1.0f;
		bounds.vMax = 1.0f;
	}

	if (flags & ovrLayerFlag_TextureOriginAtBottomLeft)
	{
		bounds.vMin = 1.0f - bounds.vMin;
		bounds.vMax = 1.0f - bounds.vMax;
	}

	if (GetAPI() == vr::TextureType_OpenGL)
	{
		bounds.vMin = 1.0f - bounds.vMin;
		bounds.vMax = 1.0f - bounds.vMax;
	}

	return bounds;
}

ovrLayerEyeFov CompositorBase::ToFovLayer(ovrLayerEyeMatrix* matrix)
{
	ovrLayerEyeFov layer = { ovrLayerType_EyeFov };
	layer.Header.Flags = matrix->Header.Flags;
	layer.SensorSampleTime = matrix->SensorSampleTime;

	for (int i = 0; i < ovrEye_Count; i++)
	{
		layer.Fov[i].LeftTan = layer.Fov[i].RightTan = .5f / matrix->Matrix[i].M[0][0];
		layer.Fov[i].UpTan = layer.Fov[i].DownTan = -.5f / matrix->Matrix[i].M[1][1];
		layer.ColorTexture[i] = matrix->ColorTexture[i];
		layer.Viewport[i] = matrix->Viewport[i];
		layer.RenderPose[i] = matrix->RenderPose[i];
	}

	return layer;
}

void CompositorBase::BlitFovLayers(ovrLayerEyeFov* dstLayer, ovrLayerEyeFov* srcLayer)
{
	MICROPROFILE_SCOPE(SubmitFovLayer);

	ovrTextureSwapChain swapChain[ovrEye_Count] = {
		srcLayer->ColorTexture[ovrEye_Left],
		srcLayer->ColorTexture[ovrEye_Right]
	};

	// If the right eye isn't set use the left eye for both
	if (!swapChain[ovrEye_Right])
		swapChain[ovrEye_Right] = swapChain[ovrEye_Left];

	MICROPROFILE_META_CPU("SwapChain Right", swapChain[ovrEye_Right]->Identifier);
	MICROPROFILE_META_CPU("SwapChain Left", swapChain[ovrEye_Left]->Identifier);

	// Render the scene layer
	for (int i = 0; i < ovrEye_Count; i++)
	{
		// Get the scene fov
		ovrFovPort sceneFov = dstLayer->Fov[i];

		// Calculate the fov quad
		vr::HmdVector4_t quad;
		quad.v[0] = srcLayer->Fov[i].LeftTan / -sceneFov.LeftTan;
		quad.v[1] = srcLayer->Fov[i].RightTan / sceneFov.RightTan;
		quad.v[2] = srcLayer->Fov[i].UpTan / sceneFov.UpTan;
		quad.v[3] = srcLayer->Fov[i].DownTan / -sceneFov.DownTan;

		// Calculate the texture bounds
		vr::VRTextureBounds_t bounds = ViewportToTextureBounds(srcLayer->Viewport[i], swapChain[i], srcLayer->Header.Flags);

		// Composit the layer
		RenderTextureSwapChain((vr::EVREye)i, swapChain[i], dstLayer->ColorTexture[i], dstLayer->Viewport[i], bounds, quad);
	}

	swapChain[ovrEye_Left]->Submit();
	if (swapChain[ovrEye_Left] != swapChain[ovrEye_Right])
		swapChain[ovrEye_Right]->Submit();
}

vr::VRCompositorError CompositorBase::SubmitFovLayer(ovrSession session, ovrLayerEyeFov* fovLayer)
{
	MICROPROFILE_SCOPE(SubmitSceneLayer);

	ovrTextureSwapChain swapChain[ovrEye_Count] = {
		fovLayer->ColorTexture[ovrEye_Left],
		fovLayer->ColorTexture[ovrEye_Right]
	};

	// If the right eye isn't set use the left eye for both
	if (!swapChain[ovrEye_Right])
		swapChain[ovrEye_Right] = swapChain[ovrEye_Left];

	MICROPROFILE_META_CPU("SwapChain Right", swapChain[ovrEye_Right]->Identifier);
	MICROPROFILE_META_CPU("Right Submit", swapChain[ovrEye_Right]->SubmitIndex);
	MICROPROFILE_META_CPU("SwapChain Left", swapChain[ovrEye_Left]->Identifier);
	MICROPROFILE_META_CPU("Left Submit", swapChain[ovrEye_Left]->SubmitIndex);

	// Submit the scene layer.
	vr::VRCompositorError err;
	for (int i = 0; i < ovrEye_Count; i++)
	{
		ovrTextureSwapChain chain = swapChain[i];
		vr::VRTextureBounds_t bounds = ViewportToTextureBounds(fovLayer->Viewport[i], swapChain[i], fovLayer->Header.Flags);

		// Get the descriptor for this eye
		rcu_ptr<ovrEyeRenderDesc> desc = session->Details->RenderDesc[i];

		// Shrink the bounds to account for the overlapping fov
		vr::VRTextureBounds_t fovBounds = FovPortToTextureBounds(desc->Fov, fovLayer->Fov[i]);

		// Combine the fov bounds with the viewport bounds
		bounds.uMin += fovBounds.uMin * bounds.uMax;
		bounds.uMax *= fovBounds.uMax;
		bounds.vMin += fovBounds.vMin * bounds.vMax;
		bounds.vMax *= fovBounds.vMax;

		vr::VRTextureWithPose_t texture = chain->Textures[chain->SubmitIndex]->ToVRTexture();

		// Add the pose data to the eye texture
		REV::Matrix4f pose(fovLayer->RenderPose[i]);
		if (session->TrackingOrigin == vr::TrackingUniverseSeated)
		{
			REV::Matrix4f offset(vr::VRSystem()->GetSeatedZeroPoseToStandingAbsoluteTrackingPose());
			texture.mDeviceToAbsoluteTracking = REV::Matrix4f(offset * pose);
		}
		else
		{
			texture.mDeviceToAbsoluteTracking = pose;
		}

		err = vr::VRCompositor()->Submit((vr::EVREye)i, (vr::Texture_t*)&texture, &bounds, vr::Submit_TextureWithPose);
		if (err != vr::VRCompositorError_None)
			break;
	}

	swapChain[ovrEye_Left]->Submit();
	if (swapChain[ovrEye_Left] != swapChain[ovrEye_Right])
		swapChain[ovrEye_Right]->Submit();

	return err;
}

void CompositorBase::SetMirrorTexture(ovrMirrorTexture mirrorTexture)
{
	m_MirrorTexture = mirrorTexture;
}

vr::VRTextureBounds_t CompositorBase::FovPortToTextureBounds(ovrFovPort eyeFov, ovrFovPort fov)
{
	vr::VRTextureBounds_t result;

	// Adjust the bounds based on the field-of-view in the game
	result.uMin = 0.5f - 0.5f * eyeFov.LeftTan / fov.LeftTan;
	result.uMax = 0.5f + 0.5f * eyeFov.RightTan / fov.RightTan;
	result.vMin = 0.5f - 0.5f * eyeFov.UpTan / fov.UpTan;
	result.vMax = 0.5f + 0.5f * eyeFov.DownTan / fov.DownTan;

	return result;
}
