#define _WINSOCK_DEPRECATED_NO_WARNINGS

#include <WinSock2.h>
#include <WS2tcpip.h>
#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

using namespace std;

constexpr uint16_t PKT_CS_LOGIN = 1;
constexpr uint16_t PKT_CS_CHAT = 2;
constexpr uint16_t PKT_SC_CHAT = 3;
constexpr uint16_t PKT_CS_ENTER_ROOM = 4;

constexpr uint16_t PKT_SC_SHUTDOWN = 1000;
constexpr uint16_t PKT_SC_KICK = 1001;

constexpr int HEADER_SIZE = 4;
constexpr int MAX_PACKET_SIZE = 4096;

mutex coutMutex;
string myNickname;

void PrintPrompt()
{
	cout << "chat> " << flush;
}

void ClearCurrentLine()
{
	CONSOLE_SCREEN_BUFFER_INFO info{};
	const int consoleWidth = GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info)
		? info.dwSize.X
		: 120;
	const int clearWidth = consoleWidth > 1 ? consoleWidth - 1 : 119;

	cout << '\r' << string(clearWidth, ' ') << '\r';
}

void PrintLine(const string& text, bool prompt = false)
{
	lock_guard<mutex> lock(coutMutex);
	ClearCurrentLine();
	cout << text << "\n";
	if (prompt)
		PrintPrompt();
}

string Trim(const string& value)
{
	const size_t begin = value.find_first_not_of(" \t\r\n");
	if (begin == string::npos)
		return "";

	const size_t end = value.find_last_not_of(" \t\r\n");
	return value.substr(begin, end - begin + 1);
}

string ToLowerAscii(string value)
{
	for (char& ch : value)
	{
		if (ch >= 'A' && ch <= 'Z')
			ch = static_cast<char>(ch - 'A' + 'a');
	}
	return value;
}

uint16_t ReadUInt16LE(const char* buffer, int offset)
{
	return static_cast<uint16_t>(
		static_cast<unsigned char>(buffer[offset]) |
		(static_cast<unsigned char>(buffer[offset + 1]) << 8));
}

uint32_t ReadUInt32LE(const char* buffer, int offset)
{
	return static_cast<uint32_t>(
		static_cast<unsigned char>(buffer[offset]) |
		(static_cast<unsigned char>(buffer[offset + 1]) << 8) |
		(static_cast<unsigned char>(buffer[offset + 2]) << 16) |
		(static_cast<unsigned char>(buffer[offset + 3]) << 24));
}

void WriteUInt16LE(vector<char>& buffer, int offset, uint16_t value)
{
	buffer[offset] = static_cast<char>(value & 0xFF);
	buffer[offset + 1] = static_cast<char>((value >> 8) & 0xFF);
}

void WriteInt32LE(vector<char>& buffer, int offset, int32_t value)
{
	buffer[offset] = static_cast<char>(value & 0xFF);
	buffer[offset + 1] = static_cast<char>((value >> 8) & 0xFF);
	buffer[offset + 2] = static_cast<char>((value >> 16) & 0xFF);
	buffer[offset + 3] = static_cast<char>((value >> 24) & 0xFF);
}

bool SendAll(SOCKET sock, const char* data, int len)
{
	int sentTotal = 0;
	while (sentTotal < len)
	{
		int sent = send(sock, data + sentTotal, len - sentTotal, 0);
		if (sent == SOCKET_ERROR || sent == 0)
			return false;

		sentTotal += sent;
	}
	return true;
}

bool RecvAll(SOCKET sock, char* buffer, int len)
{
	int recvTotal = 0;
	while (recvTotal < len)
	{
		int recvBytes = recv(sock, buffer + recvTotal, len - recvTotal, 0);
		if (recvBytes <= 0)
			return false;

		recvTotal += recvBytes;
	}
	return true;
}

bool SendPacket(SOCKET sock, uint16_t pktId, const char* body, int bodyLen)
{
	const int packetSize = HEADER_SIZE + bodyLen;
	if (packetSize < HEADER_SIZE || packetSize > MAX_PACKET_SIZE)
	{
		PrintLine("[client] packet size is invalid: " + to_string(packetSize));
		return false;
	}

	vector<char> packet(packetSize);
	WriteUInt16LE(packet, 0, static_cast<uint16_t>(packetSize));
	WriteUInt16LE(packet, 2, pktId);

	if (bodyLen > 0 && body != nullptr)
		memcpy(packet.data() + HEADER_SIZE, body, bodyLen);

	return SendAll(sock, packet.data(), static_cast<int>(packet.size()));
}

bool SendStringPacket(SOCKET sock, uint16_t pktId, const string& text)
{
	return SendPacket(sock, pktId, text.data(), static_cast<int>(text.size()));
}

bool SendLogin(SOCKET sock, const string& nickname)
{
	return SendStringPacket(sock, PKT_CS_LOGIN, nickname);
}

bool SendEnterRoom(SOCKET sock, int32_t roomId)
{
	vector<char> body(4);
	WriteInt32LE(body, 0, roomId);
	return SendPacket(sock, PKT_CS_ENTER_ROOM, body.data(), static_cast<int>(body.size()));
}

bool SendChat(SOCKET sock, const string& message)
{
	if (message.empty())
		return false;

	return SendStringPacket(sock, PKT_CS_CHAT, message);
}

bool ParseServerChat(const string& body, uint32_t& senderId, string& senderName, string& message)
{
	if (body.size() < 8)
		return false;

	const char* data = body.data();
	int offset = 0;
	const int size = static_cast<int>(body.size());

	senderId = ReadUInt32LE(data, offset);
	offset += 4;

	const uint16_t nameLen = ReadUInt16LE(data, offset);
	offset += 2;

	if (offset + nameLen + 2 > size)
		return false;

	senderName.assign(data + offset, data + offset + nameLen);
	offset += nameLen;

	const uint16_t msgLen = ReadUInt16LE(data, offset);
	offset += 2;

	if (offset + msgLen > size)
		return false;

	message.assign(data + offset, data + offset + msgLen);
	return true;
}

void RecvThread(SOCKET sock, atomic<bool>& running)
{
	while (running)
	{
		char header[HEADER_SIZE];
		if (!RecvAll(sock, header, HEADER_SIZE))
		{
			running = false;
			PrintLine("[system] 서버와의 연결이 종료되었습니다.", false);
			return;
		}

		const uint16_t pktSize = ReadUInt16LE(header, 0);
		const uint16_t pktId = ReadUInt16LE(header, 2);

		if (pktSize < HEADER_SIZE || pktSize > MAX_PACKET_SIZE)
		{
			running = false;
			PrintLine("[system] 잘못된 패킷 크기 수신: " + to_string(pktSize), false);
			return;
		}

		const int bodySize = pktSize - HEADER_SIZE;
		string body(bodySize, '\0');

		if (bodySize > 0 && !RecvAll(sock, &body[0], bodySize))
		{
			running = false;
			PrintLine("[system] 패킷 수신 중 연결이 종료되었습니다.", false);
			return;
		}

		switch (pktId)
		{
		case PKT_SC_CHAT:
		{
			uint32_t senderId = 0;
			string senderName;
			string message;

			if (!ParseServerChat(body, senderId, senderName, message))
			{
				PrintLine("[system] 잘못된 채팅 패킷을 수신했습니다.", true);
				break;
			}

			if (senderName.empty())
				senderName = "Player " + to_string(senderId);

			PrintLine("[" + senderName + "] " + message, true);
			break;
		}
		case PKT_SC_SHUTDOWN:
			running = false;
			PrintLine("[system] 서버 종료 패킷을 수신했습니다.", false);
			return;
		case PKT_SC_KICK:
			running = false;
			PrintLine("[system] 서버에서 연결을 종료했습니다.", false);
			return;
		default:
			PrintLine("[system] 알 수 없는 패킷 수신 id=" + to_string(pktId) + ", bodySize=" + to_string(bodySize), true);
			break;
		}
	}
}

void PrintHelp()
{
	lock_guard<mutex> lock(coutMutex);
	cout << "\nCommands\n"
		<< "  /help       명령어 보기\n"
		<< "  /clear      화면 지우기\n"
		<< "  /quit       종료\n"
		<< "\n그 외 입력은 현재 방에 채팅으로 전송됩니다.\n";
}

string AskString(const string& label, const string& defaultValue)
{
	cout << label << " [" << defaultValue << "]: " << flush;

	string input;
	getline(cin, input);
	input = Trim(input);
	return input.empty() ? defaultValue : input;
}

int AskInt(const string& label, int defaultValue)
{
	while (true)
	{
		cout << label << " [" << defaultValue << "]: " << flush;

		string input;
		getline(cin, input);
		input = Trim(input);
		if (input.empty())
			return defaultValue;

		try
		{
			return stoi(input);
		}
		catch (...)
		{
			cout << "숫자로 입력해 주세요.\n";
		}
	}
}

int main(int argc, char* argv[])
{
	SetConsoleOutputCP(CP_UTF8);
	SetConsoleCP(CP_UTF8);

	string serverIp = argc >= 2 ? argv[1] : "127.0.0.1";
	int serverPort = 9000;
	if (argc >= 3)
	{
		try
		{
			serverPort = stoi(argv[2]);
		}
		catch (...)
		{
			cout << "[client] 포트 인자가 잘못되어 기본값 9000을 사용합니다.\n";
		}
	}

	cout << "========================================\n"
		<< " IOCP Chat Console Client\n"
		<< "========================================\n"
		<< "엔터만 누르면 기본값을 사용합니다.\n\n";

	if (argc < 2)
		serverIp = AskString("Server IP", serverIp);
	if (argc < 3)
		serverPort = AskInt("Server Port", serverPort);

	string nickname = AskString("Nickname", "CppClient");
	int roomId = AskInt("Room ID", 1);
	myNickname = nickname;

	WSAData wsa{};
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
	{
		cout << "[client] WSAStartup 실패\n";
		return 1;
	}

	SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock == INVALID_SOCKET)
	{
		cout << "[client] 소켓 생성 실패\n";
		WSACleanup();
		return 1;
	}

	sockaddr_in serverAddr{};
	serverAddr.sin_family = AF_INET;
	serverAddr.sin_port = htons(static_cast<u_short>(serverPort));

	if (InetPtonA(AF_INET, serverIp.c_str(), &serverAddr.sin_addr) != 1)
	{
		cout << "[client] IP 주소가 올바르지 않습니다: " << serverIp << "\n";
		closesocket(sock);
		WSACleanup();
		return 1;
	}

	cout << "\n[system] " << serverIp << ":" << serverPort << " 접속 중...\n";

	if (connect(sock, reinterpret_cast<SOCKADDR*>(&serverAddr), sizeof(serverAddr)) == SOCKET_ERROR)
	{
		cout << "[client] 서버 접속 실패. WSAError=" << WSAGetLastError() << "\n";
		closesocket(sock);
		WSACleanup();
		return 1;
	}

	atomic<bool> running = true;
	thread recvThread(RecvThread, sock, ref(running));

	if (!SendLogin(sock, nickname))
	{
		cout << "[client] 로그인 패킷 전송 실패\n";
		running = false;
	}
	else if (!SendEnterRoom(sock, roomId))
	{
		cout << "[client] 방 입장 패킷 전송 실패\n";
		running = false;
	}
	else
	{
		cout << "[system] 로그인 완료 요청: " << nickname << "\n";
		cout << "[system] Room " << roomId << " 입장 요청 완료\n";
		cout << "[system] /help 를 입력하면 명령어를 볼 수 있습니다.\n\n";
	}

	while (running)
	{
		string msg;
		{
			lock_guard<mutex> lock(coutMutex);
			PrintPrompt();
		}

		if (!getline(cin, msg))
			break;

		msg = Trim(msg);
		if (msg.empty())
			continue;

		const string command = ToLowerAscii(msg);
		if (command == "/quit" || command == "/exit" || command == "quit" || command == "exit")
			break;

		if (command == "/help")
		{
			PrintHelp();
			continue;
		}

		if (command == "/clear" || command == "cls")
		{
			system("cls");
			continue;
		}

		if (msg[0] == '/')
		{
			PrintLine("[system] 알 수 없는 명령어입니다. /help 를 입력해 주세요.");
			continue;
		}

		if (!SendChat(sock, msg))
		{
			PrintLine("[client] 채팅 전송 실패", false);
			break;
		}

		PrintLine("[me] " + msg);
	}

	running = false;
	shutdown(sock, SD_BOTH);
	closesocket(sock);

	if (recvThread.joinable())
		recvThread.join();

	WSACleanup();
	cout << "\n[system] 클라이언트를 종료합니다.\n";
	return 0;
}
