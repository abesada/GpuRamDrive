/*
GpuRamDrive proxy for ImDisk Virtual Disk Driver.

Copyright (C) 2016 Syahmi Azhar.

Permission is hereby granted, free of charge, to any person
obtaining a copy of this software and associated documentation
files (the "Software"), to deal in the Software without
restriction, including without limitation the rights to use,
copy, modify, merge, publish, distribute, sublicense, and/or
sell copies of the Software, and to permit persons to whom the
Software is furnished to do so, subject to the following
conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
OTHER DEALINGS IN THE SOFTWARE.
*/

#include "stdafx.h"
#include <imdisk/imdisk.h>
#include <imdisk/imdproxy.h>
#include "GpuRamDrive.h"


#if GPU_API == GPU_API_OPENCL
#pragma comment (lib, "opencl.lib")
#endif

#if GPU_API == GPU_API_CUDA
#pragma comment(lib, "cuda.lib")
#include "CudaHandler.h"
#endif

GPURamDrive::GPURamDrive()
	: m_DriveType(EGpuRamDriveType::HD)
	, m_DriveRemovable(false)
	, m_MemSize(0)
	, m_Context(nullptr)
	, m_Queue(nullptr)
	, m_GpuMem(nullptr)
	, m_pBuff(nullptr)
	, m_ImdDrive(INVALID_HANDLE_VALUE)
	, m_ShmHandle(NULL)
	, m_ShmMutexSrv(NULL)
	, m_ShmReqEvent(NULL)
	, m_ShmRespEvent(NULL)
	, m_ShmView(nullptr)
	, m_clPlatformId()
	, m_clDeviceId()
	, m_BufSize()
	, m_BufStart()
	, config(L"GpuRamDrive")
	, debugTools(L"GpuRamDrive")
#if GPU_API == GPU_API_CUDA
	, m_cuDev(0)
	, m_cuCtx(nullptr)
	, m_cuDevPtr()
#endif
{
#if GPU_API == GPU_API_CUDA
	CudaHandler::GetInstance();
#endif
}

GPURamDrive::~GPURamDrive()
{
	ImdiskUnmountDevice();
}

void GPURamDrive::RefreshGPUInfo()
{
	m_Devices.clear();

#if GPU_API == GPU_API_HOSTMEM
	TGPUDevice GpuDevices;
	MEMORYSTATUSEX memStatus = { 0 };

	memStatus.dwLength = sizeof(memStatus);
	GlobalMemoryStatusEx(&memStatus);
	GpuDevices.memsize = memStatus.ullTotalPhys;
	GpuDevices.platform_id = 0;
	GpuDevices.device_id = 0;
	GpuDevices.name = "Host Memory";
	m_Devices.push_back(GpuDevices);
#elif GPU_API == GPU_API_CUDA
	CUresult res;
	int cuDevCount;

	if ((res = cuDeviceGetCount(&cuDevCount)) != CUDA_SUCCESS) {
		throw std::runtime_error("Unable to get cuda device count: " + std::to_string(res));
	}

	for (int i = 0; i < cuDevCount; i++) {
		TGPUDevice GpuDevices;
		CUdevice dev;

		char szPlatformName[64] = { 0 };

		cuDeviceGet(&dev, 0);
		cuDeviceGetName(szPlatformName, sizeof(szPlatformName), dev);
		cuDeviceTotalMem((size_t*)&GpuDevices.memsize, dev);

		GpuDevices.platform_id = 0;
		GpuDevices.device_id = (cl_device_id)(0ui64 | (unsigned int)dev);
		GpuDevices.name = szPlatformName;
		m_Devices.push_back(GpuDevices);
	}
#else
	cl_int clRet;
	cl_platform_id platforms[8];
	cl_uint numPlatforms;

	if ((clRet = clGetPlatformIDs(4, platforms, &numPlatforms)) != CL_SUCCESS) {
		throw std::runtime_error(std::string("Unable to get platform IDs: ") + std::to_string(clRet));
	}

	for (cl_uint i = 0; i < numPlatforms; i++) {
		cl_device_id devices[16];
		cl_uint numDevices;
		char szPlatformName[64] = { 0 };

		if ((clRet = clGetPlatformInfo(platforms[i], CL_PLATFORM_NAME, sizeof(szPlatformName), szPlatformName, nullptr)) != CL_SUCCESS) {
			continue;
		}

		if ((clRet = clGetDeviceIDs(platforms[i], CL_DEVICE_TYPE_GPU | CL_DEVICE_TYPE_ACCELERATOR, 16, devices, &numDevices)) != CL_SUCCESS) {
			continue;
		}

		for (cl_uint j = 0; j < numDevices; j++) {
			TGPUDevice GpuDevices;
			char szDevName[64] = { 0 };

			if ((clRet = clGetDeviceInfo(devices[j], CL_DEVICE_GLOBAL_MEM_SIZE, sizeof(cl_ulong), &GpuDevices.memsize, nullptr)) != CL_SUCCESS) {
				continue;
			}

			if ((clRet = clGetDeviceInfo(devices[j], CL_DEVICE_NAME, sizeof(szDevName), szDevName, nullptr)) != CL_SUCCESS) {
				continue;
			}

			GpuDevices.platform_id = platforms[i];
			GpuDevices.device_id = devices[j];
			GpuDevices.name = szPlatformName + std::string(" - ") + szDevName;
			m_Devices.push_back(GpuDevices);
		}
	}
#endif
}

const std::vector<TGPUDevice>& GPURamDrive::GetGpuDevices()
{
	if (m_Devices.size() == 0) {
		RefreshGPUInfo();
	}
	return m_Devices;
}

void GPURamDrive::SetDriveType(EGpuRamDriveType type)
{
	m_DriveType = type;
}

void GPURamDrive::SetDriveType(const wchar_t* type)
{
	if (type == nullptr) return;

	if (_wcsicmp(type, L"HD") == 0) {
		m_DriveType = EGpuRamDriveType::HD;
	} else if (_wcsicmp(type, L"FD") == 0) {
		m_DriveType = EGpuRamDriveType::FD;
	} else if (_wcsicmp(type, L"CD") == 0) {
		m_DriveType = EGpuRamDriveType::CD;
	} else if (_wcsicmp(type, L"RAW") == 0) {
		m_DriveType = EGpuRamDriveType::RAW;
	}
}

void GPURamDrive::SetRemovable(bool removable)
{
	m_DriveRemovable = removable;
}

void GPURamDrive::CreateRamDevice(cl_platform_id clPlatformId, cl_device_id clDeviceId, const std::wstring& ServiceName, size_t MemSize, const wchar_t* MountPoint, const std::wstring& FormatParam, const std::wstring& LabelParam, bool TempFolderParam)
{
	debugTools.deb(L"Creating the ramdrive '%s'", MountPoint);
	m_clPlatformId = clPlatformId;
	m_clDeviceId = clDeviceId;
	m_MemSize = MemSize;
	m_ServiceName = ServiceName;
	m_DeviceId = IMDISK_AUTO_DEVICE_NUMBER;
	m_TempFolderParam = TempFolderParam;

	std::exception state_ex;
	std::atomic<int> state = 0;

#if GPU_API == GPU_API_CUDA
	m_cuCtx = CudaHandler::GetInstance()->getContext(m_clDeviceId);
#endif

	// Avoid creating ram-device when it is still unmounting, usually when user do fast mount/unmount clicking.
	if (m_GpuThread.joinable()) {
		if (m_StateChangeCallback) m_StateChangeCallback();
		return;
	}

	m_GpuThread = std::thread([&]() {
		try
		{
			debugTools.deb(L"Allocating the memory '%llu'", MemSize);
			GpuAllocateRam();
			debugTools.deb(L"Setting the Imdisk '%s'", ServiceName.c_str());
			ImdiskSetupComm(ServiceName);
			state = 1;
			ImdiskHandleComm();
			Close();
		}
		catch (const std::exception& ex)
		{
			Close();
			state_ex = ex;
			state = 2;
		}
	});

	while (state == 0) {
		Sleep(1);
	}

	if (state == 2) {
		if (m_GpuThread.joinable()) m_GpuThread.join();
		throw state_ex;
	}

	debugTools.deb(L"Mounting the drive on '%s'", MountPoint);
	ImdiskMountDevice(MountPoint);

	if (FormatParam.length()) {
		debugTools.deb(L"Formatting the drive as '%s'", FormatParam.c_str());
		wchar_t formatCommand[128] = { 0 };
		STARTUPINFO StartInfo = { 0 };
		PROCESS_INFORMATION ProcInfo = { 0 };

		_snwprintf_s(formatCommand, sizeof(formatCommand), L"format.com %s %s", MountPoint, FormatParam.c_str());
		if (wcsstr(formatCommand, L"/y") == nullptr && wcsstr(formatCommand, L"/Y") == nullptr) {
			wcscat_s(formatCommand, L" /y");
		}

		CreateProcess(nullptr,
			formatCommand,
			nullptr,
			nullptr,
			TRUE,
			CREATE_NO_WINDOW | NORMAL_PRIORITY_CLASS,
			nullptr,
			nullptr,
			&StartInfo,
			&ProcInfo);

		WaitForSingleObject(ProcInfo.hProcess, INFINITE);
		CloseHandle(ProcInfo.hProcess);
		CloseHandle(ProcInfo.hThread);

		// Set Volumen Label
		if (LabelParam.length()) {
			debugTools.deb(L"Setting volumen name to '%s'", LabelParam.c_str());
			SetVolumeLabel(MountPoint, LabelParam.c_str());
		}

		// Create Temporal directory
		if (TempFolderParam) {
			debugTools.deb(L"Setting temporal environment to '%s\\Temp'", MountPoint);
			wchar_t temporalFolderName[64] = { 0 };
			_snwprintf_s(temporalFolderName, sizeof(temporalFolderName), L"%s\\Temp", MountPoint);
			CreateDirectory(temporalFolderName, NULL);
			config.setMountTempEnvironment(temporalFolderName);
		}

		// Create Icon Drive
		
	}

	if (m_StateChangeCallback) m_StateChangeCallback();
}

void GPURamDrive::ImdiskMountDevice(const wchar_t* MountPoint)
{
	DISK_GEOMETRY dskGeom = { 0 };
	DWORD flags = IMDISK_TYPE_PROXY | IMDISK_PROXY_TYPE_SHM | (DWORD)m_DriveType;
	if (m_DriveRemovable) flags |= IMDISK_OPTION_REMOVABLE;

	ImDiskSetAPIFlags(IMDISK_API_FORCE_DISMOUNT);

	m_MountPoint = MountPoint;
	debugTools.deb(L"ImDiskCreateDeviceEx start");
	if (!ImDiskCreateDeviceEx(NULL, &m_DeviceId, &dskGeom, nullptr, flags, m_ServiceName.c_str(), FALSE, (LPWSTR)MountPoint)) {
		m_GpuThread.detach();
		Close();
		ImdiskUnmountDevice();
		throw std::runtime_error("Unable to create and mount ImDisk drive");
	}
	debugTools.deb(L"ImDiskCreateDeviceEx end");
}

void GPURamDrive::ImdiskUnmountDevice()
{
	if (m_MountPoint.length() == 0) return;

	try
	{
		if (m_TempFolderParam)
			config.restoreOriginalTempEnvironment();

		debugTools.deb(L"Unmounting the ramdrive '%s'", m_MountPoint.c_str());
		//ImDiskRemoveDevice(NULL, 0, m_MountPoint.c_str());
		ImDiskForceRemoveDevice(NULL, m_DeviceId);
		debugTools.deb(L"Unmounted the ramdrive '%s'", m_MountPoint.c_str());
		m_MountPoint.clear();

		if (m_GpuThread.get_id() != std::this_thread::get_id()) {
			if (m_GpuThread.joinable()) m_GpuThread.join();
		}
	}
	catch (const std::exception& ex)
	{
		debugTools.deb(L"Error to unmounting the ramdrive '%s'", ex.what());
	}
}

void GPURamDrive::Close()
{
	if (m_ShmView) UnmapViewOfFile(m_ShmView);
	if (m_ShmHandle) CloseHandle(m_ShmHandle);
	if (m_ShmMutexSrv) CloseHandle(m_ShmMutexSrv);
	if (m_ShmReqEvent) CloseHandle(m_ShmReqEvent);
	if (m_ShmRespEvent) CloseHandle(m_ShmRespEvent);

	if (m_pBuff) delete[] m_pBuff;
#if GPU_API == GPU_API_OPENCL
	if (m_Queue) clFlush(m_Queue);
	if (m_Queue) clFinish(m_Queue);
	if (m_GpuMem) clReleaseMemObject(m_GpuMem);
	if (m_Queue) clReleaseCommandQueue(m_Queue);
	if (m_Context) clReleaseContext(m_Context);
#endif

	m_ShmView = nullptr;
	m_ShmHandle = NULL;
	m_ShmMutexSrv = NULL;
	m_ShmReqEvent = NULL;
	m_ShmRespEvent = NULL;

	m_pBuff = nullptr;
	m_GpuMem = nullptr;
	m_Queue = nullptr;
	m_Context = nullptr;
	m_MemSize = 0;

#if GPU_API == GPU_API_CUDA
	cuCtxPushCurrent(m_cuCtx);
	if (m_cuDevPtr) cuMemFree(m_cuDevPtr);
	CudaHandler::GetInstance()->removeContext(m_clDeviceId);
	m_cuDevPtr = 0;
	cuCtxPopCurrent(&m_cuCtx); 
#endif

	if (m_StateChangeCallback) m_StateChangeCallback();
}

bool GPURamDrive::IsMounted()
{
	return m_MountPoint.size() != 0 && m_ShmView != nullptr;
}

void GPURamDrive::SetStateChangeCallback(const std::function<void()> callback)
{
	m_StateChangeCallback = callback;
}

void GPURamDrive::GpuAllocateRam()
{
#if GPU_API == GPU_API_HOSTMEM
	m_pBuff = new char[m_MemSize];
#elif GPU_API == GPU_API_CUDA
	cuCtxPushCurrent(m_cuCtx);
	CUresult res;
	if ((res = cuMemAlloc(&m_cuDevPtr, m_MemSize)) != CUDA_SUCCESS) {
		if (res == CUDA_ERROR_OUT_OF_MEMORY) {
			size_t free_m, total_m, free_b, total_b;
			CUresult res2 = cuMemGetInfo(&free_b, &total_b);
			free_m = free_b / 1048576;
			total_m = total_b / 1048576;
			debugTools.deb(L"Available free video memory: '%llu' bytes", free_b);
			cuCtxPopCurrent(&m_cuCtx);
			throw std::runtime_error("Not enough memory to alloc, free: '" + std::to_string(free_m) + "' Mb");
		}
		else {
			cuCtxPopCurrent(&m_cuCtx);
			throw std::runtime_error("Unable to allocate memory on device, error code: " + std::to_string(res));
		}
	}
	cuCtxPopCurrent(&m_cuCtx);
#else

	cl_int clRet;

	m_Context = clCreateContext(nullptr, 1, &m_clDeviceId, nullptr, nullptr, &clRet);
	if (m_Context == nullptr) {
		throw std::runtime_error("Unable to create context: " + std::to_string(clRet));
	}

	m_Queue = clCreateCommandQueue(m_Context, m_clDeviceId, 0, &clRet);
	if (m_Queue == nullptr) {
		throw std::runtime_error("Unable to create command queue: " + std::to_string(clRet));
	}
	
	cl_ulong c = 0;
	cl_int ret_code = clGetDeviceInfo(m_clDeviceId, CL_DEVICE_MAX_MEM_ALLOC_SIZE, sizeof(cl_ulong), &c, nullptr);
	if (ret_code != CL_SUCCESS) {
		throw std::runtime_error("Unable to allocate memory: " + std::to_string(ret_code));
	}

	char vendor[1024] = {};
	ret_code = clGetDeviceInfo(m_clDeviceId, CL_DEVICE_VENDOR, sizeof(vendor), vendor, nullptr);
	if (ret_code != CL_SUCCESS) {
		throw std::runtime_error("Unable to get platform: " + std::to_string(ret_code));
	}

	if (strstr(vendor, "Advanced Micro Devices") != NULL)
	{
		m_GpuMem = clCreateBuffer(m_Context, CL_MEM_READ_WRITE, m_MemSize, nullptr, &clRet);
	}
	else 
	{
		m_GpuMem = clCreateBuffer(m_Context, CL_MEM_READ_WRITE | CL_MEM_ALLOC_HOST_PTR, m_MemSize, nullptr, &clRet);
	}

	/*
	// Bzrr version
	m_pBuff = new char[m_MemSize];
	m_GpuMem = clCreateBuffer(m_Context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, m_MemSize, m_pBuff, &clRet);
	free(m_pBuff);
	m_pBuff = nullptr;
	*/

	if (m_GpuMem == nullptr) {
		throw std::runtime_error("Unable to create memory buffer: " + std::to_string(clRet));
	}
#endif
}

safeio_ssize_t GPURamDrive::GpuWrite(void *buf, safeio_size_t size, off_t_64 offset)
{
#if GPU_API == GPU_API_HOSTMEM
	memcpy(m_pBuff + offset, buf, size);
	return size;
#elif GPU_API == GPU_API_CUDA
	cuCtxPushCurrent(m_cuCtx);
	if (cuMemcpyHtoD(m_cuDevPtr + (CUdeviceptr)offset, buf, size) == CUDA_SUCCESS) {
		cuCtxPopCurrent(&m_cuCtx);
		return size;
	}
	cuCtxPopCurrent(&m_cuCtx);
	return 0;
#else
	if (clEnqueueWriteBuffer(m_Queue, m_GpuMem, CL_TRUE, (size_t)offset, (size_t)size, buf, 0, nullptr, nullptr) != CL_SUCCESS) {
		return 0;
	}

	return size;
#endif
}

safeio_ssize_t GPURamDrive::GpuRead(void *buf, safeio_size_t size, off_t_64 offset)
{
#if GPU_API == GPU_API_HOSTMEM
	memcpy(buf, m_pBuff + offset, size);
	return size;
#elif GPU_API == GPU_API_CUDA
	cuCtxPushCurrent(m_cuCtx);
	if (cuMemcpyDtoH(buf, m_cuDevPtr + (CUdeviceptr)offset, size) == CUDA_SUCCESS) {
		cuCtxPopCurrent(&m_cuCtx);
		return size;
	}
	cuCtxPopCurrent(&m_cuCtx);
	return 0;
#else
	if (clEnqueueReadBuffer(m_Queue, m_GpuMem, CL_TRUE, (size_t)offset, (size_t)size, buf, 0, nullptr, nullptr) != CL_SUCCESS) {
		return 0;
	}
	
	return size;
#endif
}

void GPURamDrive::ImdiskSetupComm(const std::wstring& ServiceName)
{
	MEMORY_BASIC_INFORMATION MemInfo;
	ULARGE_INTEGER MapSize;
	DWORD dwErr;
	std::wstring sTemp;
	const std::wstring sPrefix = L"Global\\";

	m_BufSize = (4 << 20);
	MapSize.QuadPart = m_BufSize + IMDPROXY_HEADER_SIZE;

	sTemp = sPrefix + ServiceName;
	m_ShmHandle = CreateFileMapping(INVALID_HANDLE_VALUE,
		NULL,
		PAGE_READWRITE | SEC_COMMIT,
		MapSize.HighPart,
		MapSize.LowPart,
		sTemp.c_str());
	dwErr = GetLastError();
	if (m_ShmHandle == NULL) {
		throw std::runtime_error("Unable to create file mapping: " + std::to_string(dwErr));
	}

	if (dwErr == ERROR_ALREADY_EXISTS) {
		throw std::runtime_error("A service with this name is already running or is still being used by ImDisk");
	}

	m_ShmView = MapViewOfFile(m_ShmHandle, FILE_MAP_WRITE, 0, 0, 0);
	if (m_ShmView == nullptr) {
		dwErr = GetLastError();
		throw std::runtime_error("Unable to map view of shared memory: " + std::to_string(dwErr));
	}

	if (!VirtualQuery(m_ShmView, &MemInfo, sizeof(MemInfo))) {
		dwErr = GetLastError();
		throw std::runtime_error("Unable to query memory info: " + std::to_string(dwErr));
	}

	m_BufStart = (char*)m_ShmView + IMDPROXY_HEADER_SIZE;


	sTemp = sPrefix + ServiceName + L"_Server";
	m_ShmMutexSrv = CreateMutex(NULL, FALSE, sTemp.c_str());
	if (m_ShmMutexSrv == NULL) {
		dwErr = GetLastError();
		throw std::runtime_error("Unable to create mutex object: " + std::to_string(dwErr));
	}

	if (WaitForSingleObject(m_ShmMutexSrv, 0) != WAIT_OBJECT_0) {
		throw std::runtime_error("A service with this name is already running");
	}

	sTemp = sPrefix + ServiceName + L"_Request";
	m_ShmReqEvent = CreateEvent(NULL, FALSE, FALSE, sTemp.c_str());
	if (m_ShmReqEvent == NULL) {
		dwErr = GetLastError();
		throw std::runtime_error("Unable to create request event object: " + std::to_string(dwErr));
	}

	sTemp = sPrefix + ServiceName + L"_Response";
	m_ShmRespEvent = CreateEvent(NULL, FALSE, FALSE, sTemp.c_str());
	if (m_ShmRespEvent == NULL) {
		dwErr = GetLastError();
		throw std::runtime_error("Unable to create response event object: " + std::to_string(dwErr));
	}
}

void GPURamDrive::ImdiskHandleComm()
{
	PIMDPROXY_READ_REQ Req = (PIMDPROXY_READ_REQ)m_ShmView;
	PIMDPROXY_READ_RESP Resp = (PIMDPROXY_READ_RESP)m_ShmView;

	for (;;)
	{
		if (WaitForSingleObject(m_ShmReqEvent, INFINITE) != WAIT_OBJECT_0) {
			return;
		}

		switch (Req->request_code)
		{
			case IMDPROXY_REQ_INFO:
			{
				PIMDPROXY_INFO_RESP resp = (PIMDPROXY_INFO_RESP)m_ShmView;
				resp->file_size = m_MemSize;
				resp->req_alignment = 1;
				resp->flags = 0;
				break;
			}

			case IMDPROXY_REQ_READ:
			{
				Resp->errorno = 0;
				Resp->length = GpuRead(m_BufStart, (safeio_size_t)(Req->length < m_BufSize ? Req->length : m_BufSize), Req->offset);

				break;
			}

			case IMDPROXY_REQ_WRITE:
			{
				Resp->errorno = 0;
				Resp->length = GpuWrite(m_BufStart, (safeio_size_t)(Req->length < m_BufSize ? Req->length : m_BufSize), Req->offset);

				break;
			}

			case IMDPROXY_REQ_CLOSE:
				return;

			default:
				Req->request_code = ENODEV;
		}

		if (!SetEvent(m_ShmRespEvent)) {
			return;
		}
	}
}