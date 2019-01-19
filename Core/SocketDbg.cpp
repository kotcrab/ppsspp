#include "SocketDbg.h"

#include <SDKDDKVer.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <fstream>
#include <vector>
#include <codecvt>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iterator>
#include "Config.h"
#include <mutex>

#include "Core/MemMap.h"
#include "Core/Debugger/Breakpoints.h"
#include "Windows/Debugger/Debugger_Disasm.h"
#include "Core/MIPS/MIPS.h"
#include "Windows/resource.h"
#include "Core/SaveState.h"
#include "Core/MemMapHelpers.h"

#pragma comment (lib, "Ws2_32.lib")

constexpr i32 DEFAULT_BUFLEN = 4096;

namespace SocketDbg
{

#pragma pack(push,1)
struct BasePacket {
	const i16 id;
	BasePacket() : id(0x0) {}
};

struct BreakpointPausedEvent {
	const i16 id;
	const u32 addr;
	BreakpointPausedEvent(u32 addr) : id(0x3330), addr(addr) {}
};

struct BreakpointLoggedEvent {
	const i16 id;
	const u32 addr;
	BreakpointLoggedEvent(u32 addr) : id(0x3331), addr(addr) {}
};

struct RunningSetRequest {
	const i16 id;
	const u8 running;
	RunningSetRequest(u8 running) : id(0x3340), running(running) {}
};

struct RunningSetResponse {
	const i16 id;
	RunningSetResponse() : id(0x3341) {}
};

struct RunningGetRequest {
	const i16 id;
	RunningGetRequest(u8 running) : id(0x3342) {}
};

struct RunningGetResponse {
	const i16 id;
	const u8 running;
	RunningGetResponse(u8 running) : id(0x3343), running(running) {}
};

struct BreakpointAddRequest {
	const i16 id;
	const u32 addr;
	const u8 temp;
	BreakpointAddRequest(u32 addr, u8 temp) : id(0x3351), addr(addr), temp(temp) {}
};

struct BreakpointAddResponse {
	const i16 id;
	BreakpointAddResponse() : id(0x3352) {}
};

struct BreakpointRemoveRequest {
	const i16 id;
	const u32 addr;
	BreakpointRemoveRequest(u32 addr) : id(0x3353), addr(addr) {}
};

struct BreakpointRemoveResponse {
	const i16 id;
	BreakpointRemoveResponse() : id(0x3354) {}
};

struct GprRegistersGetRequest {
	const i16 id;
	GprRegistersGetRequest() : id(0x3360) {}
};

struct GprRegistersGetResponse {
	const i16 id;
	u32 registers[32];
	const u32 pc;
	const u32 hi;
	const u32 lo;
	GprRegistersGetResponse(u32 regs[32], u32 pc, u32 hi, u32 lo) : id(0x3361), pc(pc), hi(hi), lo(lo) {
		memcpy((void*)registers, regs, 32 * sizeof(u32));
	}
};

struct FpuRegistersGetRequest {
	const i16 id;
	FpuRegistersGetRequest() : id(0x3370) {}
};

struct FpuRegistersGetResponse {
	const i16 id;
	u32 registers[32];
	FpuRegistersGetResponse(u32 regs[32]) : id(0x3371) {
		memcpy((void*)registers, regs, 32 * sizeof(u32));
	}
};

struct MemGetRequest {
	const i16 id;
	const u32 addr;
	const u32 size;
	MemGetRequest(u32 addr, u32 size) : id(0x3380), addr(addr), size(size) {}
};

struct MemGetResponse {
	const i16 id;
	const u32 addr;
	const u32 size;
	// memory[size] follows...
	MemGetResponse(u32 addr, u32 size) : id(0x3381), addr(addr), size(size) {}
};

struct MemSetRequest {
	const i16 id;
	const u32 addr;
	const u32 size;
	// memory[size] follows...
	MemSetRequest(u32 addr, u32 size) : id(0x3382), addr(addr), size(size) {}
};

struct MemSetResponse {
	const i16 id;
	const u32 addr;
	const u32 size;
	MemSetResponse(u32 addr, u32 size) : id(0x3383), addr(addr), size(size) {}
};

#pragma pack(pop)

class SocketStreamException : public std::runtime_error {
public:
	SocketStreamException(const std::string& msg, bool cleanDisconnect) : runtime_error(msg), cleanDisconnect(cleanDisconnect) {};
	const bool cleanDisconnect;
};

class SocketStream
{
public:
	SocketStream(SOCKET socket) : m_socket(socket) {}

	i8 readByte() {
		if (m_bufPos >= m_bufCapcity) {
			m_bufPos = 0;
			fillBuf();
		}
		i8 value = m_buf[m_bufPos];
		m_bufPos++;
		return value;
	}

	i16 readShort() {
		i8 b1 = readByte();
		i8 b2 = readByte();
		return (b2 & 0xFF) << 8 | (b1 & 0xFF);
	}

	i32 readInt() {
		i8 b1 = readByte();
		i8 b2 = readByte();
		i8 b3 = readByte();
		i8 b4 = readByte();
		return (b4 & 0xFF) << 24 | (b3 & 0xFF) << 16 | (b2 & 0xFF) << 8 | (b1 & 0xFF);
	}

	float readFloat() {
		i32 data = readInt();
		return *reinterpret_cast<float*>(&data);
	}

	void readFully(u8 buf[], i32 bufSize) {
		for (int i = 0; i < bufSize; i++) {
			buf[i] = readByte();
		}
	}

	void write(const void* buf, i32 bufSize) {
		int result = send(m_socket, (char*)buf, bufSize, 0);
		if (result == SOCKET_ERROR) {
			throw SocketStreamException("send failed", false);
		}
	}
private:
	SOCKET m_socket;
	u8 m_buf[DEFAULT_BUFLEN];
	i32 m_bufCapcity = 0;
	i32 m_bufPos = 0;

	void fillBuf() {
		m_bufCapcity = recv(m_socket, (char*)m_buf, DEFAULT_BUFLEN, 0);
		if (m_bufCapcity == 0) {
			throw SocketStreamException("connection closed", true);
		} else if (m_bufCapcity < 0) {
			throw SocketStreamException("recv failed", false);
		}
	}
};

class RemoteClient;
static RemoteClient* currentRemoteClient = nullptr;

class RemoteClient {
public:
	RemoteClient(u32 connId, SOCKET socket) : m_connectionId(connId), m_clientSocket(socket), m_socketStream(socket) {
		log("accepted connection");
		InitializeCriticalSection(&critSec);
		currentRemoteClient = this;
		const HANDLE thread = CreateThread(NULL, 0, RemoteClientThreadStart, this, 0, NULL);
		if (thread) {
			CloseHandle(thread);
		}
	}
	void breakpointPaused(u32 addr) {
		BreakpointPausedEvent event(addr);
		sendResponse(&event, sizeof(event));
	}

	void breakpointLogged(u32 addr) {
		BreakpointLoggedEvent event(addr);
		sendResponse(&event, sizeof(event));
	}

private:
	CRITICAL_SECTION critSec;
	const u32 m_connectionId;
	const SOCKET m_clientSocket;
	std::vector<u8> m_currentFileBuf;
	SocketStream m_socketStream;

	static DWORD WINAPI RemoteClientThreadStart(LPVOID lpParameter) {
		RemoteClient* client = static_cast<RemoteClient*>(lpParameter);
		client->recvData();
		currentRemoteClient = nullptr;
		delete client;
		return 0;
	}

	void recvData() {
		try {
			while (true) {
				i16 packetId = m_socketStream.readShort();
				handlePacket(packetId);
			}
		}
		catch (const SocketStreamException& e) {
			currentRemoteClient = nullptr;
			if (e.cleanDisconnect) {
				log(e.what());
			} else {
				elog("error: ", e.what(), " ", WSAGetLastError());
				closesocket(m_clientSocket);
			}
		}
		catch (const std::runtime_error& e) {
			currentRemoteClient = nullptr;
			elog("error: ", e.what());
			closesocket(m_clientSocket);
		}
	}

	void handlePacket(i16 packetId) {
		switch (packetId) {
		case 0x3340: { //RunningSetRequest
			const u8 newRunning = m_socketStream.readByte();
			if (newRunning) {
				CBreakPoints::SetSkipFirst(currentMIPS->pc);
			}
			Core_EnableStepping(!newRunning);
			RunningSetResponse response;
			sendResponse(&response, sizeof(response));
			break;
		}
		case 0x3342: { //RunningGetRequest
			RunningGetResponse response(Core_IsActive());
			sendResponse(&response, sizeof(response));
			break;
		}
		case 0x3351: { //BreakpointAddRequest
			const u32 addr = m_socketStream.readInt();
			const u8 temp = m_socketStream.readByte();
			CBreakPoints::AddBreakPoint(addr, temp);
			BreakpointAddResponse response;
			sendResponse(&response, sizeof(response));
			break;
		}
		case 0x3353: { // BreakpointRemoveRequest
			const u32 addr = m_socketStream.readInt();
			CBreakPoints::RemoveBreakPoint(addr);
			BreakpointRemoveResponse response;
			sendResponse(&response, sizeof(response));
			break;
		}
		case 0x3360: { // GprRegistersGetRequest
			u32 registers[32];
			for (auto i = 0; i < 32; i++) {
				registers[i] = currentDebugMIPS->GetRegValue(0, i);
			}
			GprRegistersGetResponse response(registers, currentDebugMIPS->GetPC(), currentDebugMIPS->GetHi(), currentDebugMIPS->GetLo());
			sendResponse(&response, sizeof(response));
			break;
		}
		case 0x3370: { // FpuRegistersGetRequest
			u32 registers[32];
			for (auto i = 0; i < 32; i++) {
				registers[i] = currentDebugMIPS->GetRegValue(1, i);
			}
			FpuRegistersGetResponse response(registers);
			sendResponse(&response, sizeof(response));
			break;
		}
		case 0x3380: { // MemGetRequest
			const u32 addr = m_socketStream.readInt();
			const u32 size = m_socketStream.readInt();
			u8* buf = new u8[size];
			auto memlock = Memory::Lock();
			Memory::MemcpyUnchecked(buf, addr, size);
			MemGetResponse response(addr, size);
			sendResponse(&response, sizeof(response), buf, size);
			delete[] buf;
			break;
		}
		case 0x3382: { // MemSetRequest
			const u32 addr = m_socketStream.readInt();
			const u32 size = m_socketStream.readInt();
			u8* buf = new u8[size];
			m_socketStream.readFully(buf, size);
			auto memlock = Memory::Lock();
			Memory::MemcpyUnchecked(addr, buf, size);
			MemSetResponse response(addr, size);
			sendResponse(&response, sizeof(response));
			delete[] buf;
			break;
		}
		default:
			elog("unsupported packet id ", packetId);
			throw std::runtime_error("unsupported packet type");
			break;
		}
	}

	void sendResponse(const void* data, i32 dataLen) {
		return sendResponse(data, dataLen, nullptr, 0);
	}

	void sendResponse(const void* responseData, i32 responseLen, const void* dataTail, i32 dataTailSize) {
		EnterCriticalSection(&critSec);
		m_socketStream.write(responseData, responseLen);
		if (dataTail != nullptr) {
			m_socketStream.write(dataTail, dataTailSize);
		}
		LeaveCriticalSection(&critSec);
	}

	template <typename Arg, typename... Args>
	void log(Arg&& arg, Args&&... args) {
		std::stringstream msgOut;
		msgOut << "[*] [" << m_connectionId << "] ";
		msgOut << std::forward<Arg>(arg);
		using expander = int[];
		(void)expander {
			0, (void(msgOut << std::forward<Args>(args)), 0)...
		};
		msgOut << std::endl;
		auto str = msgOut.str();
		std::cout << str;
	}

	template <typename Arg, typename... Args>
	void wcharLog(Arg&& arg, Args&&... args) {
		std::wstringstream msgOut;
		msgOut << "[*] [" << m_connectionId << "] ";
		msgOut << std::forward<Arg>(arg);
		using expander = int[];
		(void)expander {
			0, (void(msgOut << std::forward<Args>(args)), 0)...
		};
		msgOut << std::endl;
		auto str = msgOut.str();
		std::wcout << str;
	}

	template <typename Arg, typename... Args>
	void elog(Arg&& arg, Args&&... args) {
		std::stringstream msgOut;
		msgOut << "[-] [" << m_connectionId << "] ";
		msgOut << std::forward<Arg>(arg);
		using expander = int[];
		(void)expander {
			0, (void(msgOut << std::forward<Args>(args)), 0)...
		};
		msgOut << std::endl;
		auto str = msgOut.str();
		std::cout << str;
	}
};

void breakpointPaused(u32 addr) {
	if (currentRemoteClient != nullptr) {
		currentRemoteClient->breakpointPaused(addr);
	}
}

void breakpointLogged(u32 addr) {
	if (currentRemoteClient != nullptr) {
		currentRemoteClient->breakpointLogged(addr);
	}
}

DWORD WINAPI SocketDbgMain(LPVOID lpParameter) {
	i32 iResult;
	struct addrinfo *result = NULL;

	WSADATA wsaData;
	iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
	if (iResult != 0) {
		printf("[-] socket WSAStartup failed with error: %d\n", iResult);
		return 1;
	}

	struct addrinfo hints;
	ZeroMemory(&hints, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	hints.ai_flags = AI_PASSIVE;

	using convert_type = std::codecvt_utf8<wchar_t>;
	std::wstring_convert<convert_type, wchar_t> converter;
	std::string portStr = converter.to_bytes(g_Config.remoteDebuggerPort);
	iResult = getaddrinfo(NULL, portStr.c_str(), &hints, &result);
	if (iResult != 0) {
		printf("[-] socket getaddrinfo failed with error: %d\n", iResult);
		WSACleanup();
		return 1;
	}

	SOCKET listenSocket = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
	if (listenSocket == INVALID_SOCKET) {
		printf("[-] socket failed with error: %ld\n", WSAGetLastError());
		freeaddrinfo(result);
		WSACleanup();
		return 1;
	}

	iResult = bind(listenSocket, result->ai_addr, (int)result->ai_addrlen);
	if (iResult == SOCKET_ERROR) {
		printf("[-] socket bind failed with error: %d\n", WSAGetLastError());
		freeaddrinfo(result);
		closesocket(listenSocket);
		WSACleanup();
		return 1;
	}
	freeaddrinfo(result);

	iResult = listen(listenSocket, SOMAXCONN);
	if (iResult == SOCKET_ERROR) {
		printf("[-] socket listen failed with error: %d\n", WSAGetLastError());
		closesocket(listenSocket);
		WSACleanup();
		return 1;
	}

	printf("[*] server started\n");
	u32 connectionId = 0;
	while (true) {
		SOCKET clientSocket = accept(listenSocket, NULL, NULL);
		if (clientSocket == INVALID_SOCKET) {
			printf("[-] client accept failed with error: %d\n", WSAGetLastError());
			closesocket(listenSocket);
			WSACleanup();
			return 1;
		}
		new RemoteClient(connectionId++, clientSocket);
	}
	return 0;
}


void startServer()
{
	const HANDLE thread = CreateThread(NULL, 0, SocketDbgMain, NULL, 0, NULL);
	if (thread) {
		CloseHandle(thread);
	}
}

}
