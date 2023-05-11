/*M///////////////////////////////////////////////////////////////////////////////////////
//
//  IMPORTANT: READ BEFORE DOWNLOADING, COPYING, INSTALLING OR USING.
//
//  By downloading, copying, installing or using the software you agree to this license.
//  If you do not agree to this license, do not download, install, copy or use the software.
//
//
//                           License Agreement
//                For Open Source Digital Holographic Library
//
// Openholo library is free software;
// you can redistribute it and/or modify it under the terms of the BSD 2-Clause license.
//
// Copyright (C) 2017-2024, Korea Electronics Technology Institute. All rights reserved.
// E-mail : contact.openholo@gmail.com
// Web : http://www.openholo.org
//
// Redistribution and use in source and binary forms, with or without modification,
// are permitted provided that the following conditions are met:
//
//  1. Redistribution's of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//
//  2. Redistribution's in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//
// This software is provided by the copyright holders and contributors "as is" and
// any express or implied warranties, including, but not limited to, the implied
// warranties of merchantability and fitness for a particular purpose are disclaimed.
// In no event shall the copyright holder or contributors be liable for any direct,
// indirect, incidental, special, exemplary, or consequential damages
// (including, but not limited to, procurement of substitute goods or services;
// loss of use, data, or profits; or business interruption) however caused
// and on any theory of liability, whether in contract, strict liability,
// or tort (including negligence or otherwise) arising in any way out of
// the use of this software, even if advised of the possibility of such damage.
//
// This software contains opensource software released under GNU Generic Public License,
// NVDIA Software License Agreement, or CUDA supplement to Software License Agreement.
// Check whether software you use contains licensed software.
//
//M*/

#include "ophPointCloud.h"
#include "ophPointCloud_GPU.h"
#include "CUDA.h"
#ifdef _USE_OPENCL
#include "OpenCL.h"

void ophPointCloud::genCghPointCloudGPU(uint diff_flag)
{
	int nErr;
	auto begin = CUR_TIME;
	OpenCL *cl = OpenCL::getInstance();

	cl_context context = cl->getContext();
	cl_command_queue commands = cl->getCommand();
	cl_mem device_pc_data;
	cl_mem device_amp_data;
	cl_mem device_result;
	cl_mem device_config;

	//threads number
	const ulonglong pnXY = context_.pixel_number[_X] * context_.pixel_number[_Y];
	const ulonglong bufferSize = pnXY * sizeof(Real);

	//Host Memory Location
	const int n_colors = pc_data_.n_colors;
	Real* host_pc_data = nullptr;
	Real* host_amp_data = pc_data_.color;
	Real* host_dst = nullptr;

	// Keep original buffer
	if (is_ViewingWindow) {
		host_pc_data = new Real[n_points * 3];
		transVW(n_points * 3, host_pc_data, pc_data_.vertex);
	}
	else {
		host_pc_data = pc_data_.vertex;
	}

	uint nChannel = context_.waveNum;
	bool bIsGrayScale = n_colors == 1 ? true : false;

	cl->LoadKernel();

	cl_kernel* kernel = cl->getKernel();

	cl_kernel* current_kernel = nullptr;
	if ((diff_flag == PC_DIFF_RS) || (diff_flag == PC_DIFF_FRESNEL)) {
		host_dst = new Real[pnXY * 2];
		memset(host_dst, 0., bufferSize * 2);

		current_kernel = diff_flag == PC_DIFF_RS ? &kernel[0] : &kernel[1];
	}

	device_pc_data = clCreateBuffer(context, CL_MEM_READ_ONLY, sizeof(Real) * n_points * 3, nullptr, &nErr);
	device_amp_data = clCreateBuffer(context, CL_MEM_READ_ONLY, sizeof(Real) * n_points * n_colors, nullptr, &nErr);
	nErr = clEnqueueWriteBuffer(commands, device_pc_data, CL_TRUE, 0, sizeof(Real) * n_points * 3, host_pc_data, 0, nullptr, nullptr);
	nErr = clEnqueueWriteBuffer(commands, device_amp_data, CL_TRUE, 0, sizeof(Real) * n_points * n_colors, host_amp_data, 0, nullptr, nullptr);

	device_result = clCreateBuffer(context, CL_MEM_WRITE_ONLY, sizeof(Real) * pnXY * 2, nullptr, &nErr);


	size_t global[2] = { context_.pixel_number[_X], context_.pixel_number[_Y] };
	size_t local[2] = { 32, 32 };

	clSetKernelArg(*current_kernel, 1, sizeof(cl_mem), &device_pc_data);
	clSetKernelArg(*current_kernel, 2, sizeof(cl_mem), &device_amp_data);
	clSetKernelArg(*current_kernel, 4, sizeof(uint), &n_points);
	for (uint ch = 0; ch < nChannel; ch++)
	{
		uint nAdd = bIsGrayScale ? 0 : ch;
		context_.k = (2 * M_PI) / context_.wave_length[ch];
		Real ratio = 1.0; //context_.wave_length[nChannel - 1] / context_.wave_length[ch];

		GpuConst* host_config = new GpuConst(
			n_points, n_colors, 1,
			pc_config_.scale, pc_config_.distance,
			context_.pixel_number,
			context_.pixel_pitch,
			context_.ss,
			context_.k,
			context_.wave_length[ch],
			ratio
		);

		if (diff_flag == PC_DIFF_RS)
		{
			host_config = new GpuConstNERS(*host_config);
			device_config = clCreateBuffer(context, CL_MEM_READ_ONLY, sizeof(GpuConstNERS), nullptr, &nErr);

			nErr = clEnqueueWriteBuffer(commands, device_result, CL_TRUE, 0, sizeof(Real) * pnXY * 2, host_dst, 0, nullptr, nullptr);
			nErr = clEnqueueWriteBuffer(commands, device_config, CL_TRUE, 0, sizeof(GpuConstNERS), host_config, 0, nullptr, nullptr);
		}
		else if (diff_flag == PC_DIFF_FRESNEL)
		{
			host_config = new GpuConstNEFR(*host_config);
			device_config = clCreateBuffer(context, CL_MEM_READ_ONLY, sizeof(GpuConstNEFR), nullptr, &nErr);

			nErr = clEnqueueWriteBuffer(commands, device_result, CL_TRUE, 0, sizeof(Real) * pnXY * 2, host_dst, 0, nullptr, nullptr);
			nErr = clEnqueueWriteBuffer(commands, device_config, CL_TRUE, 0, sizeof(GpuConstNEFR), host_config, 0, nullptr, nullptr);
		}

		clSetKernelArg(*current_kernel, 0, sizeof(cl_mem), &device_result);
		clSetKernelArg(*current_kernel, 3, sizeof(cl_mem), &device_config);
		clSetKernelArg(*current_kernel, 5, sizeof(uint), &ch);

		nErr = clEnqueueNDRangeKernel(commands, *current_kernel, 2, nullptr, global, nullptr/*local*/, 0, nullptr, nullptr);


		//nErr = clFlush(commands);
		nErr = clFinish(commands);

		if (nErr != CL_SUCCESS) cl->errorCheck(nErr, "Check", __FILE__, __LINE__);

		nErr = clEnqueueReadBuffer(commands, device_result, CL_TRUE, 0, sizeof(Real) * pnXY * 2, complex_H[ch], 0, nullptr, nullptr);

		delete host_config;

		m_nProgress = (ch + 1) * 100 / nChannel;
	}

	clReleaseMemObject(device_result);
	clReleaseMemObject(device_amp_data);
	clReleaseMemObject(device_pc_data);
	if (host_dst)	delete[] host_dst;
	if (is_ViewingWindow && host_pc_data)	delete[] host_pc_data;

	LOG("%s : %.5lf (sec)\n", __FUNCTION__, ELAPSED_TIME(begin, CUR_TIME));
}
#else

using namespace oph;
void ophPointCloud::genCghPointCloudGPU(uint diff_flag)
{
	if ((diff_flag != PC_DIFF_RS) && (diff_flag != PC_DIFF_FRESNEL))
	{
		LOG("<FAILED> Wrong parameters.");
		return;
	}

	//cudaStream_t* streams = nullptr;
	auto begin = CUR_TIME;
	const ulonglong pnXY = context_.pixel_number[_X] * context_.pixel_number[_Y];
	int blockSize = CUDA::getInstance()->getMaxThreads(); //n_threads // blockSize < devProp.maxThreadsPerBlock
	ulonglong gridSize = (pnXY + blockSize - 1) / blockSize; //n_blocks

	cout << ">>> All " << blockSize * gridSize << " threads in CUDA" << endl;
	cout << ">>> " << blockSize << " threads/block, " << gridSize << " blocks/grid" << endl;

	//const int n_streams = OPH_CUDA_N_STREAM;
	int n_streams;
	if (getStream() == 0)
		n_streams = pc_data_.n_points / 300 + 1;
	else if (getStream() < 0)
	{
		LOG("<FAILED> Wrong parameters.");
		return;
	}
	else
		n_streams = getStream();



	// Keep original buffer
	//if (is_ViewingWindow) {
	//	host_pc_data = new Vertex[pc_data_.n_points];
	//	transVW(pc_data_.n_points, host_pc_data, pc_data_.vertices);
	//}
	//else {
	//	host_pc_data = pc_data_.vertices;

	//}

	//threads number
	const ulonglong bufferSize = pnXY * sizeof(cuDoubleComplex);
	int n_points = pc_data_.n_points;

	//Host Memory Location
	Vertex* host_vertex_data = nullptr;
	if (!is_ViewingWindow)
		host_vertex_data = pc_data_.vertices;
	else
	{
		host_vertex_data = new Vertex[n_points];
		std::memcpy(host_vertex_data, pc_data_.vertices, sizeof(Vertex) * n_points);
		transVW(n_points, host_vertex_data, host_vertex_data);
	}
	cuDoubleComplex* host_dst = nullptr;
	host_dst = new cuDoubleComplex[pnXY];
	memset(host_dst, 0, bufferSize);

	Vertex* device_vertex_data;
	HANDLE_ERROR(cudaMalloc((void**)&device_vertex_data, n_points * sizeof(Vertex)));

	cuDoubleComplex* device_dst = nullptr;
	HANDLE_ERROR(cudaMalloc((void**)&device_dst, bufferSize));
	HANDLE_ERROR(cudaMemsetAsync(device_dst, 0., bufferSize));

	uint nChannel = context_.waveNum;
	size_t free, total;
	cudaMemGetInfo(&free, &total);

#ifdef _WIN64
	LOG("CUDA Memory Total: %llu (byte) / Free: %llu (byte)\n", total, free);
#else
	LOG("CUDA Memory Total: %zu (byte) / Free: %zu (byte)\n", total, free);
#endif
	for (uint ch = 0; ch < nChannel; ch++)
	{
		context_.k = (2 * M_PI) / context_.wave_length[ch];

		GpuConst* host_config = new GpuConst(
			n_points, n_streams,
			pc_config_.scale, pc_config_.distance,
			context_.pixel_number,
			context_.pixel_pitch,
			context_.ss,
			context_.k,
			context_.wave_length[ch]
		);


		GpuConst* device_config = nullptr;
		switch (diff_flag) {
		case PC_DIFF_RS: {
			host_config = new GpuConstNERS(*host_config);
			HANDLE_ERROR(cudaMalloc((void**)&device_config, sizeof(GpuConstNERS)));
			HANDLE_ERROR(cudaMemcpyAsync(device_config, host_config, sizeof(GpuConstNERS), cudaMemcpyHostToDevice));
			break;
		}
		case PC_DIFF_FRESNEL: {
			host_config = new GpuConstNEFR(*host_config);
			HANDLE_ERROR(cudaMalloc((void**)&device_config, sizeof(GpuConstNEFR)));
			HANDLE_ERROR(cudaMemcpyAsync(device_config, host_config, sizeof(GpuConstNEFR), cudaMemcpyHostToDevice));
			break;
		}
		}

		int stream_points = n_points / n_streams;
		int remainder = n_points % n_streams;


		int offset = 0;
		for (int i = 0; i < n_streams; ++i) {
			offset = i * stream_points;
			if (i == n_streams - 1) { // ������ ��Ʈ�� ���� ��,
				stream_points += remainder;
			}

			HANDLE_ERROR(cudaMemcpyAsync(device_vertex_data + offset, host_vertex_data + offset, stream_points * sizeof(Vertex), cudaMemcpyHostToDevice));

			switch (diff_flag) {
			case PC_DIFF_RS: {
				cudaPointCloud_RS(gridSize, blockSize, stream_points, device_vertex_data + offset, device_dst, (GpuConstNERS*)device_config, ch, m_mode);
				break;
			}
			case PC_DIFF_FRESNEL: {
				cudaPointCloud_Fresnel(gridSize, blockSize, stream_points, device_vertex_data + offset, device_dst, (GpuConstNEFR*)device_config, ch, m_mode);
				break;
			} // case
			} // switch

			cudaError error = cudaGetLastError();
			if (error != cudaSuccess) {
				LOG("cudaGetLastError(): %s\n", cudaGetErrorName(error));
				if (error == cudaErrorLaunchOutOfResources) {
					i--;
					blockSize /= 2;
					gridSize *= 2;
					continue;
				}
			}
			HANDLE_ERROR(cudaMemcpyAsync(host_dst, device_dst, bufferSize, cudaMemcpyDeviceToHost));
			HANDLE_ERROR(cudaMemsetAsync(device_dst, 0., bufferSize));

			for (ulonglong n = 0; n < pnXY; ++n) {
				complex_H[ch][n][_RE] += host_dst[n].x;
				complex_H[ch][n][_IM] += host_dst[n].y;
			}

			m_nProgress = (int)((Real)(ch*n_streams + i + 1) * 100 / ((Real)n_streams * nChannel));
		} // for

		////free memory
		HANDLE_ERROR(cudaFree(device_config));
		
		delete host_config;
	}

	HANDLE_ERROR(cudaFree(device_vertex_data));
	HANDLE_ERROR(cudaFree(device_dst));
	delete[] host_dst;

	if (is_ViewingWindow) {
		delete[] host_vertex_data;
	}

	LOG("%s : %.5lf (sec)\n", __FUNCTION__, ELAPSED_TIME(begin, CUR_TIME));
}
#endif