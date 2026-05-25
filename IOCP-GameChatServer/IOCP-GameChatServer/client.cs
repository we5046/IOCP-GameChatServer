using System;
using System.Collections.Generic;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Threading;
using UnityEngine;

public class NetworkClient : MonoBehaviour
{
	public const ushort PKT_CS_LOGIN = 1;
	public const ushort PKT_CS_CHAT = 2;
	public const ushort PKT_SC_CHAT = 3;
	public const ushort PKT_CS_ENTER_ROOM = 4;

	public const ushort PKT_SC_SHUTDOWN = 1000;
	public const ushort PKT_SC_KICK = 1001;

	public const int MAX_PACKET_SIZE = 4096;
	const int HEADER_SIZE = 4;

	public static NetworkClient Instance { get; private set; }

	[Header("Server")]
	public string serverIp = "127.0.0.1";
	public int serverPort = 9000;

	[Header("Auto Flow")]
	public bool autoConnect = true;
	public bool autoLoginAndEnterRoom = true;
	public string autoNickname = "TestUser";
	public int autoRoomId = 1;

	Socket socket;
	Thread recvThread;
	volatile bool running;

	readonly object queueLock = new object();
	readonly Queue<string> chatQueue = new Queue<string>();
	readonly Queue<string> systemQueue = new Queue<string>();
	bool disconnectedQueued;

	public event Action<string> OnChatMessage;
	public event Action<string> OnSystemMessage;
	public event Action OnDisconnected;

	void Awake()
	{
		if (Instance != null && Instance != this)
		{
			Destroy(gameObject);
			return;
		}

		Instance = this;
		DontDestroyOnLoad(gameObject);
	}

	void Start()
	{
		if (!autoConnect)
			return;

		Connect(serverIp, serverPort);

		if (autoLoginAndEnterRoom)
			StartCoroutine(AutoLoginAndEnterRoom());
	}

	System.Collections.IEnumerator AutoLoginAndEnterRoom()
	{
		yield return null;

		SendLogin(autoNickname);
		yield return new WaitForSeconds(0.03f);
		SendEnterRoom(autoRoomId);
	}

	void Update()
	{
		while (true)
		{
			string msg;
			lock (queueLock)
			{
				if (systemQueue.Count == 0)
					break;

				msg = systemQueue.Dequeue();
			}

			OnSystemMessage?.Invoke(msg);
		}

		while (true)
		{
			string msg;
			lock (queueLock)
			{
				if (chatQueue.Count == 0)
					break;

				msg = chatQueue.Dequeue();
			}

			OnChatMessage?.Invoke(msg);
		}

		if (disconnectedQueued)
		{
			disconnectedQueued = false;
			OnDisconnected?.Invoke();
		}
	}

	void OnDestroy()
	{
		Disconnect();

		if (Instance == this)
			Instance = null;
	}

	public bool IsConnected => socket != null && socket.Connected;

	public void Connect(string ip, int port)
	{
		if (IsConnected)
			return;

		try
		{
			socket = new Socket(AddressFamily.InterNetwork, SocketType.Stream, ProtocolType.Tcp);
			socket.NoDelay = true;
			socket.Connect(new IPEndPoint(IPAddress.Parse(ip), port));

			running = true;
			disconnectedQueued = false;
			recvThread = new Thread(RecvLoop) { IsBackground = true };
			recvThread.Start();

			EnqueueSystem($"Connected to {ip}:{port}");
		}
		catch (Exception e)
		{
			EnqueueSystem($"Connect failed: {e.Message}");
			SafeCloseSocket();
		}
	}

	public void Disconnect()
	{
		running = false;
		SafeCloseSocket();

		try
		{
			if (recvThread != null && recvThread.IsAlive)
				recvThread.Join(200);
		}
		catch
		{
		}

		recvThread = null;
		EnqueueSystem("Disconnected");
	}

	public void SendLogin(string nickname)
	{
		if (!IsConnected)
			return;

		byte[] body = Encoding.UTF8.GetBytes(nickname ?? "");
		SendPacket(PKT_CS_LOGIN, body);
	}

	public void SendEnterRoom(int roomId)
	{
		if (!IsConnected)
			return;

		byte[] body = new byte[4];
		WriteInt32LE(body, 0, roomId);
		SendPacket(PKT_CS_ENTER_ROOM, body);
	}

	public void SendChat(string message)
	{
		if (!IsConnected || string.IsNullOrWhiteSpace(message))
			return;

		byte[] body = Encoding.UTF8.GetBytes(message);
		SendPacket(PKT_CS_CHAT, body);
	}

	void SendPacket(ushort pktId, byte[] body)
	{
		try
		{
			int bodyLen = body != null ? body.Length : 0;
			int packetSize = HEADER_SIZE + bodyLen;

			if (packetSize < HEADER_SIZE || packetSize > MAX_PACKET_SIZE)
			{
				EnqueueSystem($"SendPacket invalid size: {packetSize}");
				return;
			}

			byte[] packet = new byte[packetSize];
			WriteUInt16LE(packet, 0, (ushort)packetSize);
			WriteUInt16LE(packet, 2, pktId);

			if (bodyLen > 0)
				Buffer.BlockCopy(body, 0, packet, HEADER_SIZE, bodyLen);

			SendAll(packet);
		}
		catch (Exception e)
		{
			EnqueueSystem($"SendPacket error: {e.Message}");
			ForceDisconnectFromWorker();
		}
	}

	void SendAll(byte[] packet)
	{
		int sent = 0;
		while (sent < packet.Length)
		{
			int n = socket.Send(packet, sent, packet.Length - sent, SocketFlags.None);
			if (n <= 0)
				throw new SocketException();

			sent += n;
		}
	}

	void RecvLoop()
	{
		byte[] recvBuf = new byte[4096];
		List<byte> streamBuf = new List<byte>(8192);

		try
		{
			while (running)
			{
				int recv = socket.Receive(recvBuf);
				if (recv == 0)
				{
					EnqueueSystem("Server closed connection");
					break;
				}

				if (streamBuf.Count + recv > MAX_PACKET_SIZE * 2)
				{
					EnqueueSystem("Client stream buffer overflow");
					break;
				}

				for (int i = 0; i < recv; ++i)
					streamBuf.Add(recvBuf[i]);

				while (true)
				{
					if (streamBuf.Count < HEADER_SIZE)
						break;

					ushort pktSize = ReadUInt16LE(streamBuf, 0);
					ushort pktId = ReadUInt16LE(streamBuf, 2);

					if (pktSize < HEADER_SIZE || pktSize > MAX_PACKET_SIZE)
					{
						EnqueueSystem($"Invalid pktSize={pktSize}");
						running = false;
						break;
					}

					if (streamBuf.Count < pktSize)
						break;

					byte[] packet = streamBuf.GetRange(0, pktSize).ToArray();
					HandlePacket(pktId, packet, HEADER_SIZE, pktSize - HEADER_SIZE);
					streamBuf.RemoveRange(0, pktSize);
				}
			}
		}
		catch (ObjectDisposedException)
		{
		}
		catch (SocketException e)
		{
			EnqueueSystem($"SocketException: {e.SocketErrorCode}");
		}
		catch (Exception e)
		{
			EnqueueSystem($"RecvLoop Exception: {e.Message}");
		}

		ForceDisconnectFromWorker();
	}

	void HandlePacket(ushort pktId, byte[] buffer, int bodyOffset, int bodySize)
	{
		switch (pktId)
		{
			case PKT_SC_CHAT:
				HandleServerChat(buffer, bodyOffset, bodySize);
				break;
			case PKT_SC_SHUTDOWN:
				EnqueueSystem("Server shutdown packet received");
				running = false;
				break;
			case PKT_SC_KICK:
				EnqueueSystem("Kicked by server");
				running = false;
				break;
			default:
				EnqueueSystem($"Unknown packet id={pktId}, bodySize={bodySize}");
				break;
		}
	}

	void HandleServerChat(byte[] buffer, int bodyOffset, int bodySize)
	{
		int offset = bodyOffset;
		int end = bodyOffset + bodySize;

		if (bodySize < 8)
		{
			EnqueueSystem($"Invalid chat body size={bodySize}");
			return;
		}

		uint senderId = ReadUInt32LE(buffer, offset);
		offset += 4;

		ushort nameLen = ReadUInt16LE(buffer, offset);
		offset += 2;

		if (offset + nameLen + 2 > end)
		{
			EnqueueSystem("Invalid chat name length");
			return;
		}

		string senderName = nameLen > 0 ? Encoding.UTF8.GetString(buffer, offset, nameLen) : "";
		offset += nameLen;

		ushort msgLen = ReadUInt16LE(buffer, offset);
		offset += 2;

		if (offset + msgLen > end)
		{
			EnqueueSystem("Invalid chat message length");
			return;
		}

		string msg = msgLen > 0 ? Encoding.UTF8.GetString(buffer, offset, msgLen) : "";
		string displayName = string.IsNullOrEmpty(senderName) ? $"Player {senderId}" : senderName;
		EnqueueChat($"{displayName}: {msg}");
	}

	void EnqueueChat(string msg)
	{
		lock (queueLock)
			chatQueue.Enqueue(msg);
	}

	void EnqueueSystem(string msg)
	{
		lock (queueLock)
			systemQueue.Enqueue(msg);
	}

	void ForceDisconnectFromWorker()
	{
		running = false;
		SafeCloseSocket();

		lock (queueLock)
		{
			systemQueue.Enqueue("Disconnected (worker)");
			disconnectedQueued = true;
		}
	}

	void SafeCloseSocket()
	{
		Socket oldSocket = socket;
		socket = null;

		try
		{
			oldSocket?.Shutdown(SocketShutdown.Both);
		}
		catch
		{
		}

		try
		{
			oldSocket?.Close();
		}
		catch
		{
		}
	}

	static void WriteUInt16LE(byte[] dst, int offset, ushort value)
	{
		dst[offset] = (byte)(value & 0xFF);
		dst[offset + 1] = (byte)((value >> 8) & 0xFF);
	}

	static ushort ReadUInt16LE(IList<byte> src, int offset)
	{
		return (ushort)(src[offset] | (src[offset + 1] << 8));
	}

	static ushort ReadUInt16LE(byte[] src, int offset)
	{
		return (ushort)(src[offset] | (src[offset + 1] << 8));
	}

	static uint ReadUInt32LE(byte[] src, int offset)
	{
		return (uint)(src[offset]
			| (src[offset + 1] << 8)
			| (src[offset + 2] << 16)
			| (src[offset + 3] << 24));
	}

	static void WriteInt32LE(byte[] dst, int offset, int value)
	{
		dst[offset] = (byte)(value & 0xFF);
		dst[offset + 1] = (byte)((value >> 8) & 0xFF);
		dst[offset + 2] = (byte)((value >> 16) & 0xFF);
		dst[offset + 3] = (byte)((value >> 24) & 0xFF);
	}
}
