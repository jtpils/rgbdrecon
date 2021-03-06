#include "stdafx.h"

#include "CUDARGBDAdapter.h"
#include "TimingLog.h"

extern "C" void copyFloat4Map(float4* d_output, float4* d_input, unsigned int width, unsigned int height);

extern "C" void convertDepthRawToFloat(float* d_output, unsigned short* d_input, unsigned int width, unsigned int height, float minDepth, float maxDepth);

// (GPU) Scale the raw RGBA data in byte between 0-255 to float values between 0.0-1.0 by dividing each byte value by 255.
extern "C" void convertColorRawToFloat4(float4* d_output, BYTE* d_input, unsigned int width, unsigned int height);

// (GPU) Re-sample the input image map into the output image map with scaling and bilinear interpolation
extern "C" void resampleFloatMap(float* d_colorMapResampledFloat, unsigned int outputWidth, unsigned int outputHeight, float* d_colorMapFloat, unsigned int inputWidth, unsigned int inputHeight, float* d_depthMaskMap);


extern "C" void resampleFloat4Map(float4* d_colorMapResampledFloat4, unsigned int outputWidth, unsigned int outputHeight, float4* d_colorMapFloat4, unsigned int inputWidth, unsigned int inputHeight);

CUDARGBDAdapter::CUDARGBDAdapter()
{			
	// depth
	d_depthMapFloat = NULL;
	d_depthMapResampledFloat = NULL;

	// color
	d_colorMapRaw = NULL;
	d_colorMapFloat4 = NULL;
	d_colorMapResampledFloat4 = NULL;

	m_frameNumber = 0;
}

CUDARGBDAdapter::~CUDARGBDAdapter()
{
}

void CUDARGBDAdapter::OnD3D11DestroyDevice() 
{		
	// depth
	cutilSafeCall(cudaFree(d_depthMapFloat));
	cutilSafeCall(cudaFree(d_depthMapResampledFloat));
	
	// color
	cutilSafeCall(cudaFree(d_colorMapRaw));
	cutilSafeCall(cudaFree(d_colorMapFloat4));
	cutilSafeCall(cudaFree(d_colorMapResampledFloat4));
}

HRESULT CUDARGBDAdapter::OnD3D11CreateDevice(ID3D11Device* device, RGBDSensor* RGBDSensor, unsigned int width, unsigned int height)
{
	HRESULT hr = S_OK;

	m_RGBDSensor = RGBDSensor;

	m_width = width;
	m_height = height;

	const unsigned int widthColor  = m_RGBDSensor->getColorWidth();
	const unsigned int heightColor = m_RGBDSensor->getColorHeight();
	
	const unsigned int widthDepth  = m_RGBDSensor->getDepthWidth();
	const unsigned int heightDepth = m_RGBDSensor->getDepthHeight();

	const float scaleWidthColor = (float)width/(float)widthColor;
	const float scaleHeightColor = (float)height/(float)heightColor;
	
	const float scaleWidthDepth = (float)width/(float)widthDepth;
	const float scaleHeightDepth = (float)height/(float)heightDepth;

	// adapt intrinsics
	m_depthIntrinsics = m_RGBDSensor->getDepthIntrinsics();
	m_depthIntrinsics._m00 *= scaleWidthDepth;  m_depthIntrinsics._m02 *= scaleWidthDepth;
	m_depthIntrinsics._m11 *= scaleHeightDepth; m_depthIntrinsics._m12 *= scaleHeightDepth;

	m_depthIntrinsicsInv = m_RGBDSensor->getDepthIntrinsicsInv();
	m_depthIntrinsicsInv._m00 /= scaleWidthDepth; m_depthIntrinsicsInv._m11 /= scaleHeightDepth;

	m_colorIntrinsics = m_RGBDSensor->getColorIntrinsics();
	m_colorIntrinsics._m00 *= scaleWidthColor;  m_colorIntrinsics._m02 *= scaleWidthColor;
	m_colorIntrinsics._m11 *= scaleHeightColor; m_colorIntrinsics._m12 *= scaleHeightColor;

	m_colorIntrinsicsInv = m_RGBDSensor->getColorIntrinsicsInv();
	m_colorIntrinsicsInv._m00 /= scaleWidthColor; m_colorIntrinsicsInv._m11 /= scaleHeightColor;

	// adapt extrinsics
	m_depthExtrinsics	 =  m_RGBDSensor->getDepthExtrinsics();
	m_depthExtrinsicsInv =  m_RGBDSensor->getDepthExtrinsicsInv();

	m_colorExtrinsics	 =  m_RGBDSensor->getColorExtrinsics();
	m_colorExtrinsicsInv =  m_RGBDSensor->getColorExtrinsicsInv();

	// allocate memory
	const unsigned int bufferDimDepthInput = m_RGBDSensor->getDepthWidth()*m_RGBDSensor->getDepthHeight();
	const unsigned int bufferDimColorInput = m_RGBDSensor->getColorWidth()*m_RGBDSensor->getColorHeight();

	const unsigned int bufferDimOutput = width*height;
	
	// depth
	cutilSafeCall(cudaMalloc(&d_depthMapFloat,			sizeof(float)			* bufferDimDepthInput));
	cutilSafeCall(cudaMalloc(&d_depthMapResampledFloat,	sizeof(float)			* bufferDimOutput));

	// color
	cutilSafeCall(cudaMalloc(&d_colorMapRaw,			 sizeof(unsigned int)*bufferDimColorInput));
	cutilSafeCall(cudaMalloc(&d_colorMapFloat4,			 4*sizeof(float)*bufferDimColorInput));
	cutilSafeCall(cudaMalloc(&d_colorMapResampledFloat4, 4*sizeof(float)*bufferDimOutput));
	
	return hr;
}

HRESULT CUDARGBDAdapter::process(ID3D11DeviceContext* context)
{
	HRESULT hr = S_OK;

	if (m_RGBDSensor->processDepth() != S_OK)	return S_FALSE; // Order is important!
	if (m_RGBDSensor->processColor() != S_OK)	return S_FALSE;

	//Start Timing
	if (GlobalAppState::get().s_timingsDetailledEnabled) { cutilSafeCall(cudaDeviceSynchronize()); m_timer.start(); }

	////////////////////////////////////////////////////////////////////////////////////
	// Process Color
	////////////////////////////////////////////////////////////////////////////////////
	
	// Firstly copy the raw color data from CPU to GPU
	const unsigned int bufferDimColorInput = m_RGBDSensor->getColorWidth()*m_RGBDSensor->getColorHeight();
	cutilSafeCall(cudaMemcpy(d_colorMapRaw, m_RGBDSensor->getColorRGBX(), sizeof(unsigned int)*bufferDimColorInput, cudaMemcpyHostToDevice));
	
	// Scale the color raw data in bytes between 0-255 to ones in floats between 0.0-1.0
	convertColorRawToFloat4(d_colorMapFloat4, d_colorMapRaw, m_RGBDSensor->getColorWidth(), m_RGBDSensor->getColorHeight());
	
	//writeDataToLocalFile(d_colorMapFloat4, bufferDimColorInput, 4);

	// CHAO NOTE:
	// If the resolution from the RGBDSensor is different from the one in adapter, we re-sample the RGBDSensor data to fit the resolution in adapter.
	// If they are the same, just copy the data. That is, the adapter resolution is the final RGBD data resolution we use for computation
	// (there are some other resolutions, such as for rendering.)
	//
	// Note: the resolution (width and height) in the adapter is defined and loaded from the outside file "zParametersDefault.txt", while the 
	// resolution in the RGBDSensor is the size of the loaded color image, either from the real-time depth camera or from local RGBD data.
	if ((m_RGBDSensor->getColorWidth() == m_width) && (m_RGBDSensor->getColorHeight() == m_height)) {
		copyFloat4Map(d_colorMapResampledFloat4, d_colorMapFloat4, m_width, m_height);
	} 
	else {
		resampleFloat4Map(d_colorMapResampledFloat4, m_width, m_height, d_colorMapFloat4, m_RGBDSensor->getColorWidth(), m_RGBDSensor->getColorHeight());
	}

	////////////////////////////////////////////////////////////////////////////////////
	// Process Depth
	////////////////////////////////////////////////////////////////////////////////////
	
	// Copy depth data to GPU
	const unsigned int bufferDimDepthInput = m_RGBDSensor->getDepthWidth()*m_RGBDSensor->getDepthHeight();
	cutilSafeCall(cudaMemcpy(d_depthMapFloat, m_RGBDSensor->getDepthFloat(), sizeof(float)*m_RGBDSensor->getDepthWidth()* m_RGBDSensor->getDepthHeight(), cudaMemcpyHostToDevice));
	
	// Re-sample the input depth image into the output image with adapter's resolution. If the input depth image resolution is the same as the adapther one,
	// then this function only copies the data.
	resampleFloatMap(d_depthMapResampledFloat, m_width, m_height, d_depthMapFloat, m_RGBDSensor->getDepthWidth(), m_RGBDSensor->getDepthHeight(), NULL);

	// Stop Timing
	if (GlobalAppState::get().s_timingsDetailledEnabled) { 
		cutilSafeCall(cudaDeviceSynchronize());
		m_timer.stop();
		TimingLog::totalTimeRGBDAdapter += m_timer.getElapsedTimeMS(); 
		TimingLog::countTimeRGBDAdapter++;
	}
	m_frameNumber++;
	return hr;
}

