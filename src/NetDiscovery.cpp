#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#include <rpc.h>
#include <iphlpapi.h>
#include <mstcpip.h>

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "NetDiscovery.hpp"

namespace
{
	// Windows computers often take two seconds or more to answer a probe.
	const ULONGLONG ListenMs = 4000;
	const ULONGLONG ResendMs = 400;
	const int ProbeSends = 3;

	const ULONGLONG NetBiosListenMs = 1500;
	// Subnets bigger than this (a /22) aren't swept: that's no longer a LAN worth a packet per address.
	const unsigned MaxSweepHosts = 1022;
	const size_t MaxReplySize = 256 * 1024;

	const char ProbeTemplate[] =
		"<?xml version=\"1.0\" encoding=\"utf-8\"?>"
		"<soap:Envelope xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\""
		" xmlns:wsa=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\""
		" xmlns:wsd=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\""
		" xmlns:pub=\"http://schemas.microsoft.com/windows/pub/2005/07\">"
		"<soap:Header>"
		"<wsa:To>urn:schemas-xmlsoap-org:ws:2005:04:discovery</wsa:To>"
		"<wsa:Action>http://schemas.xmlsoap.org/ws/2005/04/discovery/Probe</wsa:Action>"
		"<wsa:MessageID>urn:uuid:@ID@</wsa:MessageID>"
		"</soap:Header>"
		"<soap:Body><wsd:Probe><wsd:Types>pub:Computer</wsd:Types></wsd:Probe></soap:Body>"
		"</soap:Envelope>";

	const char GetTemplate[] =
		"<?xml version=\"1.0\" encoding=\"utf-8\"?>"
		"<soap:Envelope xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\""
		" xmlns:wsa=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\">"
		"<soap:Header>"
		"<wsa:To>@TO@</wsa:To>"
		"<wsa:Action>http://schemas.xmlsoap.org/ws/2004/09/transfer/Get</wsa:Action>"
		"<wsa:MessageID>urn:uuid:@ID@</wsa:MessageID>"
		"<wsa:ReplyTo><wsa:Address>http://schemas.xmlsoap.org/ws/2004/08/addressing/role/anonymous</wsa:Address></wsa:ReplyTo>"
		"</soap:Header>"
		"<soap:Body/>"
		"</soap:Envelope>";

	std::string NewGuid()
	{
		UUID Id;
		if (UuidCreate(&Id) != RPC_S_OK && UuidCreateSequential(&Id) != RPC_S_OK)
			return "00000000-0000-0000-0000-000000000000";

		RPC_CSTR Text = nullptr;
		if (UuidToStringA(&Id, &Text) != RPC_S_OK)
			return "00000000-0000-0000-0000-000000000000";

		std::string Result(reinterpret_cast<const char*>(Text));
		RpcStringFreeA(&Text);
		return Result;
	}

	void ReplaceAll(std::string& Text, const std::string& From, const std::string& To)
	{
		for (auto Pos = Text.find(From); Pos != std::string::npos; Pos = Text.find(From, Pos + To.size()))
			Text.replace(Pos, From.size(), To);
	}

	std::wstring ToWide(const std::string& Text)
	{
		if (Text.empty())
			return {};

		const auto Size = MultiByteToWideChar(CP_UTF8, 0, Text.data(), static_cast<int>(Text.size()), nullptr, 0);
		std::wstring Result(Size, L'\0');
		MultiByteToWideChar(CP_UTF8, 0, Text.data(), static_cast<int>(Text.size()), &Result[0], Size);
		return Result;
	}

	std::string ToUtf8(const std::wstring& Text)
	{
		if (Text.empty())
			return {};

		const auto Size = WideCharToMultiByte(CP_UTF8, 0, Text.data(), static_cast<int>(Text.size()), nullptr, 0, nullptr, nullptr);
		std::string Result(Size, '\0');
		WideCharToMultiByte(CP_UTF8, 0, Text.data(), static_cast<int>(Text.size()), &Result[0], Size, nullptr, nullptr);
		return Result;
	}

	// The text of the first element with this name, whatever the namespace prefix is.
	bool ElementText(const std::string& Xml, const char* LocalName, std::string& Text)
	{
		const std::string Name = LocalName;

		for (auto Open = Xml.find('<'); Open != std::string::npos; Open = Xml.find('<', Open + 1))
		{
			const auto NameEnd = Xml.find_first_of(" \t\r\n/>", Open + 1);
			if (NameEnd == std::string::npos)
				return false;

			auto Start = Open + 1;
			const auto Colon = Xml.find(':', Start);
			if (Colon != std::string::npos && Colon < NameEnd)
				Start = Colon + 1;

			if (Xml.compare(Start, NameEnd - Start, Name) != 0)
				continue;

			const auto TagEnd = Xml.find('>', NameEnd);
			if (TagEnd == std::string::npos)
				return false;

			if (Xml[TagEnd - 1] == '/')
			{
				Text.clear();
				return true;
			}

			const auto End = Xml.find('<', TagEnd);
			if (End == std::string::npos)
				return false;

			Text = Xml.substr(TagEnd + 1, End - TagEnd - 1);
			return true;
		}

		return false;
	}

	std::string Trimmed(const std::string& Text)
	{
		const auto Begin = Text.find_first_not_of(" \t\r\n");
		if (Begin == std::string::npos)
			return {};
		return Text.substr(Begin, Text.find_last_not_of(" \t\r\n") - Begin + 1);
	}

	// Only the port and the path: whoever answered is who we'll talk to,
	// not whichever host they might point us to.
	bool ParseXAddr(const std::string& XAddrs, unsigned short& Port, std::string& Path)
	{
		size_t Pos = 0;

		while ((Pos = XAddrs.find_first_not_of(" \t\r\n", Pos)) != std::string::npos)
		{
			auto End = XAddrs.find_first_of(" \t\r\n", Pos);
			if (End == std::string::npos)
				End = XAddrs.size();

			const auto Url = XAddrs.substr(Pos, End - Pos);
			Pos = End;

			static const char Scheme[] = "http://";
			if (Url.compare(0, sizeof(Scheme) - 1, Scheme) != 0)
				continue;

			const auto HostStart = sizeof(Scheme) - 1;

			// IPv6 literal: we've come in through IPv4, wait for the next one.
			if (Url.size() > HostStart && Url[HostStart] == '[')
				continue;

			auto PathStart = Url.find('/', HostStart);
			if (PathStart == std::string::npos)
				PathStart = Url.size();

			const auto PortStart = Url.find(':', HostStart);
			Port = 80;
			if (PortStart != std::string::npos && PortStart < PathStart)
			{
				const auto Value = atoi(Url.substr(PortStart + 1, PathStart - PortStart - 1).c_str());
				if (Value <= 0 || Value > 65535)
					continue;
				Port = static_cast<unsigned short>(Value);
			}

			Path = PathStart < Url.size()? Url.substr(PathStart) : "/";
			return true;
		}

		return false;
	}

	// These end up in UNC paths, and they come from whoever is on the network.
	bool IsSaneName(const std::wstring& Name)
	{
		if (Name.empty() || Name.size() > 63 || Name == L"." || Name == L"..")
			return false;

		return std::none_of(Name.cbegin(), Name.cend(), [](wchar_t c)
		{
			return c < L' ' || wcschr(L"\\/:*?\"<>|", c);
		});
	}

	std::vector<in_addr> LocalAddresses()
	{
		std::vector<in_addr> Result;
		char Host[256];

		if (gethostname(Host, sizeof(Host)) != 0)
			return Result;

		addrinfo Hints{};
		Hints.ai_family = AF_INET;
		addrinfo* Info = nullptr;

		if (getaddrinfo(Host, nullptr, &Hints, &Info) != 0)
			return Result;

		for (auto* i = Info; i; i = i->ai_next)
			Result.push_back(reinterpret_cast<const sockaddr_in*>(i->ai_addr)->sin_addr);

		freeaddrinfo(Info);
		return Result;
	}

	struct Subnet
	{
		in_addr Local;
		unsigned long First; // host order
		unsigned long Last;
	};

	std::vector<Subnet> LocalSubnets()
	{
		std::vector<Subnet> Result;
		ULONG Size = 16 * 1024;
		std::vector<BYTE> Buffer;
		ULONG Error;

		do
		{
			Buffer.resize(Size);
			Error = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr,
				reinterpret_cast<IP_ADAPTER_ADDRESSES*>(Buffer.data()), &Size);
		}
		while (Error == ERROR_BUFFER_OVERFLOW);

		if (Error != NO_ERROR)
			return Result;

		for (auto* Adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(Buffer.data()); Adapter; Adapter = Adapter->Next)
		{
			if (Adapter->OperStatus != IfOperStatusUp || Adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK || Adapter->IfType == IF_TYPE_TUNNEL)
				continue;

			for (auto* Unicast = Adapter->FirstUnicastAddress; Unicast; Unicast = Unicast->Next)
			{
				if (Unicast->Address.lpSockaddr->sa_family != AF_INET)
					continue;

				const auto Prefix = Unicast->OnLinkPrefixLength;
				if (Prefix < 22 || Prefix > 30)
					continue;

				const auto Local = reinterpret_cast<const sockaddr_in*>(Unicast->Address.lpSockaddr)->sin_addr;
				const auto Host = ntohl(Local.s_addr);

				// Link-local: whatever is there isn't reachable by name anyway.
				if ((Host >> 16) == 0xA9FE)
					continue;

				const auto Mask = ~0ul << (32 - Prefix);
				Subnet Net;
				Net.Local = Local;
				Net.First = (Host & Mask) + 1;
				Net.Last = (Host | ~Mask) - 1;

				if (Net.Last >= Net.First && Net.Last - Net.First < MaxSweepHosts)
					Result.push_back(Net);
			}
		}

		return Result;
	}

	// RFC 1002 node status request for the wildcard name "*".
	std::string NodeStatusRequest(unsigned short Id)
	{
		std::string Packet;
		Packet += static_cast<char>(Id >> 8);
		Packet += static_cast<char>(Id & 0xFF);
		Packet.append("\0\0\0\x01\0\0\0\0\0\0", 10);
		Packet += '\x20';
		Packet += "CK";
		Packet.append(30, 'A');
		Packet += '\0';
		Packet.append("\0\x21\0\x01", 4);
		return Packet;
	}

	std::wstring NetBiosName(const unsigned char* Name)
	{
		char Text[16]{};
		memcpy(Text, Name, 15);
		std::string Narrow(Text);
		Narrow.erase(Narrow.find_last_not_of(' ') + 1);

		if (Narrow.empty())
			return {};

		const auto Size = MultiByteToWideChar(CP_OEMCP, 0, Narrow.data(), static_cast<int>(Narrow.size()), nullptr, 0);
		std::wstring Result(Size, L'\0');
		MultiByteToWideChar(CP_OEMCP, 0, Narrow.data(), static_cast<int>(Narrow.size()), &Result[0], Size);
		return Result;
	}

	bool ParseNodeStatus(const char* Data, int Size, unsigned short Id, std::wstring& Name, std::wstring& Group)
	{
		const auto* Bytes = reinterpret_cast<const unsigned char*>(Data);

		// A response to our request, with one answer
		if (Size < 12 || ((Bytes[0] << 8) | Bytes[1]) != Id || !(Bytes[2] & 0x80) || ((Bytes[6] << 8) | Bytes[7]) < 1)
			return false;

		size_t Pos = 12;

		if (Pos < static_cast<size_t>(Size) && (Bytes[Pos] & 0xC0) == 0xC0)
			Pos += 2;
		else
		{
			while (Pos < static_cast<size_t>(Size) && Bytes[Pos])
				Pos += Bytes[Pos] + 1;
			++Pos;
		}

		// type, class, TTL, length, then the number of names
		Pos += 10;
		if (Pos >= static_cast<size_t>(Size))
			return false;

		const auto Count = Bytes[Pos++];
		if (Pos + Count * 18 > static_cast<size_t>(Size))
			return false;

		for (unsigned I = 0; I < Count; ++I, Pos += 18)
		{
			const auto Suffix = Bytes[Pos + 15];
			const bool IsGroup = (Bytes[Pos + 16] & 0x80) != 0;

			if (!IsGroup && Suffix == 0x20 && Name.empty())
				Name = NetBiosName(Bytes + Pos);
			else if (IsGroup && Suffix == 0x00 && Group.empty())
				Group = NetBiosName(Bytes + Pos);
		}

		return !Name.empty();
	}

	std::mutex AddressLock;
	std::map<std::wstring, std::wstring> Addresses; // upper case name -> IPv4 address

	std::wstring UpperCase(std::wstring Text)
	{
		if (!Text.empty())
			CharUpperBuffW(&Text[0], static_cast<DWORD>(Text.size()));
		return Text;
	}

	class WinHttpHandle
	{
	public:
		explicit WinHttpHandle(HINTERNET Handle): m_Handle(Handle) {}
		~WinHttpHandle() { if (m_Handle) WinHttpCloseHandle(m_Handle); }
		WinHttpHandle(const WinHttpHandle&) = delete;
		WinHttpHandle& operator=(const WinHttpHandle&) = delete;
		operator HINTERNET() const { return m_Handle; }
	private:
		HINTERNET m_Handle;
	};
}

namespace Discovery
{
	void Probe(const std::function<bool()>& IsCancelled, const std::function<void(Responder&&)>& Found)
	{
		WSADATA Wsa;
		if (WSAStartup(MAKEWORD(2, 2), &Wsa) != 0)
			return;

		std::string Message = ProbeTemplate;
		ReplaceAll(Message, "@ID@", NewGuid());

		sockaddr_in Multicast{};
		Multicast.sin_family = AF_INET;
		Multicast.sin_port = htons(3702);
		inet_pton(AF_INET, "239.255.255.250", &Multicast.sin_addr);

		std::vector<SOCKET> Sockets;

		for (const auto& Address: LocalAddresses())
		{
			const auto Socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
			if (Socket == INVALID_SOCKET)
				continue;

			sockaddr_in Local{};
			Local.sin_family = AF_INET;
			Local.sin_addr = Address;
			const DWORD Ttl = 1;

			if (bind(Socket, reinterpret_cast<const sockaddr*>(&Local), sizeof(Local)) != 0 ||
				setsockopt(Socket, IPPROTO_IP, IP_MULTICAST_IF, reinterpret_cast<const char*>(&Address), sizeof(Address)) != 0)
			{
				closesocket(Socket);
				continue;
			}

			setsockopt(Socket, IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<const char*>(&Ttl), sizeof(Ttl));
			Sockets.push_back(Socket);
		}

		std::set<std::wstring> Seen;
		std::vector<char> Buffer(64 * 1024);
		const auto Start = GetTickCount64();
		int Sends = 0;

		for (;;)
		{
			const auto Elapsed = GetTickCount64() - Start;

			if (Elapsed >= ListenMs || Sockets.empty() || IsCancelled())
				break;

			// UDP: send again in case the first one got lost. The MessageID is the same, so nobody answers twice.
			if (Sends < ProbeSends && Elapsed >= ResendMs * Sends)
			{
				++Sends;
				for (const auto Socket: Sockets)
					sendto(Socket, Message.data(), static_cast<int>(Message.size()), 0, reinterpret_cast<const sockaddr*>(&Multicast), sizeof(Multicast));
			}

			fd_set Read;
			FD_ZERO(&Read);
			for (const auto Socket: Sockets)
				FD_SET(Socket, &Read);

			timeval Timeout{0, 100 * 1000};
			if (select(0, &Read, nullptr, nullptr, &Timeout) <= 0)
				continue;

			for (const auto Socket: Sockets)
			{
				if (!FD_ISSET(Socket, &Read))
					continue;

				sockaddr_in From{};
				int FromSize = sizeof(From);
				const auto Size = recvfrom(Socket, Buffer.data(), static_cast<int>(Buffer.size()), 0, reinterpret_cast<sockaddr*>(&From), &FromSize);

				if (Size <= 0)
					continue;

				const std::string Reply(Buffer.data(), Size);
				std::string Types, XAddrs, Endpoint;

				if (Reply.find("ProbeMatch") == std::string::npos || !ElementText(Reply, "XAddrs", XAddrs))
					continue;

				// IP cameras and the like answer any probe, whatever it asks for.
				if (!ElementText(Reply, "Types", Types) || Types.find("Computer") == std::string::npos)
					continue;

				unsigned short Port;
				std::string Path;
				if (!ParseXAddr(XAddrs, Port, Path))
					continue;

				char AddressText[INET_ADDRSTRLEN]{};
				if (!inet_ntop(AF_INET, &From.sin_addr, AddressText, sizeof(AddressText)))
					continue;

				Responder Target;
				Target.Address = ToWide(AddressText);
				Target.Port = Port;
				Target.Path = ToWide(Path);
				if (ElementText(Reply, "Address", Endpoint))
					Target.Endpoint = ToWide(Trimmed(Endpoint));

				if (Seen.insert(Target.Address).second)
					Found(std::move(Target));
			}
		}

		for (const auto Socket: Sockets)
			closesocket(Socket);

		WSACleanup();
	}

	Computer Identify(const Responder& Target)
	{
		Computer Result;

		WinHttpHandle Session(WinHttpOpen(L"Far Network", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
		if (!Session)
			return Result;

		WinHttpSetTimeouts(Session, 1500, 1500, 2000, 2500);

		WinHttpHandle Connection(WinHttpConnect(Session, Target.Address.c_str(), Target.Port, 0));
		if (!Connection)
			return Result;

		WinHttpHandle Request(WinHttpOpenRequest(Connection, L"POST", Target.Path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0));
		if (!Request)
			return Result;

		std::string Body = GetTemplate;
		ReplaceAll(Body, "@TO@", ToUtf8(Target.Endpoint));
		ReplaceAll(Body, "@ID@", NewGuid());

		static const wchar_t Headers[] = L"Content-Type: application/soap+xml\r\n";

		if (!WinHttpSendRequest(Request, Headers, static_cast<DWORD>(-1L), Body.data(), static_cast<DWORD>(Body.size()), static_cast<DWORD>(Body.size()), 0) ||
			!WinHttpReceiveResponse(Request, nullptr))
			return Result;

		std::string Reply;
		for (;;)
		{
			char Chunk[8192];
			DWORD Read = 0;

			if (!WinHttpReadData(Request, Chunk, sizeof(Chunk), &Read) || !Read)
				break;

			Reply.append(Chunk, Read);

			if (Reply.size() > MaxReplySize)
				return Result;
		}

		// NAME/Workgroup:GROUP, NAME/Domain:DOMAIN or NAME/NotJoined
		std::string Text;
		if (!ElementText(Reply, "Computer", Text))
			return Result;

		const auto Info = ToWide(Trimmed(Text));
		const auto Slash = Info.find(L'/');
		const auto Name = Info.substr(0, Slash);

		if (!IsSaneName(Name))
			return Result;

		Result.Name = Name;
		RememberAddress(Name, Target.Address);

		if (Slash != std::wstring::npos)
		{
			const auto Colon = Info.find(L':', Slash);
			if (Colon != std::wstring::npos)
			{
				const auto Group = Info.substr(Colon + 1);
				if (IsSaneName(Group))
					Result.Workgroup = Group;
			}
		}

		return Result;
	}

	void NetBiosSweep(const std::function<bool()>& IsCancelled, const std::function<void(Computer&&, std::wstring&& Address)>& Found)
	{
		WSADATA Wsa;
		if (WSAStartup(MAKEWORD(2, 2), &Wsa) != 0)
			return;

		struct Sweep
		{
			SOCKET Socket;
			Subnet Net;
		};

		std::vector<Sweep> Sweeps;
		const auto Id = static_cast<unsigned short>(GetTickCount64() & 0xFFFF);
		const auto Request = NodeStatusRequest(Id);

		for (const auto& Net: LocalSubnets())
		{
			const auto Socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
			if (Socket == INVALID_SOCKET)
				continue;

			sockaddr_in Local{};
			Local.sin_family = AF_INET;
			Local.sin_addr = Net.Local;

			if (bind(Socket, reinterpret_cast<const sockaddr*>(&Local), sizeof(Local)) != 0)
			{
				closesocket(Socket);
				continue;
			}

			// Otherwise every "port unreachable" from a computer without NetBIOS fails the next recvfrom.
			BOOL ReportReset = FALSE;
			DWORD Returned = 0;
			WSAIoctl(Socket, SIO_UDP_CONNRESET, &ReportReset, sizeof(ReportReset), nullptr, 0, &Returned, nullptr, nullptr);

			Sweeps.push_back({Socket, Net});
		}

		std::set<unsigned long> Seen;
		char Buffer[2048];

		const auto Receive = [&](unsigned TimeoutMs)
		{
			fd_set Read;
			FD_ZERO(&Read);
			for (const auto& i: Sweeps)
				FD_SET(i.Socket, &Read);

			timeval Timeout{0, static_cast<long>(TimeoutMs * 1000)};
			if (select(0, &Read, nullptr, nullptr, &Timeout) <= 0)
				return;

			for (const auto& i: Sweeps)
			{
				if (!FD_ISSET(i.Socket, &Read))
					continue;

				sockaddr_in From{};
				int FromSize = sizeof(From);
				const auto Size = recvfrom(i.Socket, Buffer, sizeof(Buffer), 0, reinterpret_cast<sockaddr*>(&From), &FromSize);

				const auto Host = ntohl(From.sin_addr.s_addr);
				if (Size <= 0 || ntohs(From.sin_port) != 137 || Host < i.Net.First || Host > i.Net.Last || !Seen.insert(Host).second)
					continue;

				Computer Result;
				if (!ParseNodeStatus(Buffer, Size, Id, Result.Name, Result.Workgroup) || !IsSaneName(Result.Name))
					continue;

				if (!IsSaneName(Result.Workgroup))
					Result.Workgroup.clear();

				char AddressText[INET_ADDRSTRLEN]{};
				if (!inet_ntop(AF_INET, &From.sin_addr, AddressText, sizeof(AddressText)))
					continue;

				auto Address = ToWide(AddressText);
				RememberAddress(Result.Name, Address);
				Found(std::move(Result), std::move(Address));
			}
		};

		for (const auto& i: Sweeps)
		{
			const auto Self = ntohl(i.Net.Local.s_addr);

			for (auto Host = i.Net.First; Host <= i.Net.Last && !IsCancelled(); ++Host)
			{
				if (Host == Self)
					continue;

				sockaddr_in To{};
				To.sin_family = AF_INET;
				To.sin_port = htons(137);
				To.sin_addr.s_addr = htonl(Host);
				sendto(i.Socket, Request.data(), static_cast<int>(Request.size()), 0, reinterpret_cast<const sockaddr*>(&To), sizeof(To));

				// Don't flood the switch, and pick up the answers as they come.
				if ((Host & 0x3F) == 0)
					Receive(5);
			}
		}

		const auto Start = GetTickCount64();
		while (!Sweeps.empty() && !IsCancelled() && GetTickCount64() - Start < NetBiosListenMs)
			Receive(100);

		for (const auto& i: Sweeps)
			closesocket(i.Socket);

		WSACleanup();
	}

	void RememberAddress(const std::wstring& Name, const std::wstring& Address)
	{
		std::lock_guard<std::mutex> Lock(AddressLock);
		Addresses[UpperCase(Name)] = Address;
	}

	bool IsSmbReachable(const std::wstring& Server, unsigned TimeoutMs)
	{
		// Not a plain computer name: \\server@SSL\..., \\?\..., and the like are someone else's business.
		if (Server.empty() || Server.find_first_of(L"@?") != std::wstring::npos)
			return true;

		WSADATA Wsa;
		if (WSAStartup(MAKEWORD(2, 2), &Wsa) != 0)
			return true;

		std::vector<sockaddr_storage> Targets;

		const auto AddTargets = [&Targets](const wchar_t* Host)
		{
			addrinfoW Hints{};
			Hints.ai_socktype = SOCK_STREAM;
			Hints.ai_protocol = IPPROTO_TCP;
			addrinfoW* Info = nullptr;

			if (GetAddrInfoW(Host, nullptr, &Hints, &Info) != 0)
				return;

			for (auto* i = Info; i; i = i->ai_next)
			{
				if ((i->ai_family != AF_INET && i->ai_family != AF_INET6) || i->ai_addrlen > sizeof(sockaddr_storage))
					continue;

				sockaddr_storage Address{};
				memcpy(&Address, i->ai_addr, i->ai_addrlen);
				Targets.push_back(Address);
			}

			FreeAddrInfoW(Info);
		};

		std::wstring Known;
		{
			std::lock_guard<std::mutex> Lock(AddressLock);
			const auto Found = Addresses.find(UpperCase(Server));
			if (Found != Addresses.end())
				Known = Found->second;
		}

		if (!Known.empty())
			AddTargets(Known.c_str());
		AddTargets(Server.c_str());

		if (Targets.empty())
		{
			WSACleanup();
			return true;
		}

		std::vector<SOCKET> Pending;
		bool Reachable = false;

		for (const auto& Target: Targets)
		{
			for (const unsigned short Port: {445, 139})
			{
				auto Address = Target;
				int AddressSize;

				if (Address.ss_family == AF_INET)
				{
					reinterpret_cast<sockaddr_in*>(&Address)->sin_port = htons(Port);
					AddressSize = sizeof(sockaddr_in);
				}
				else
				{
					reinterpret_cast<sockaddr_in6*>(&Address)->sin6_port = htons(Port);
					AddressSize = sizeof(sockaddr_in6);
				}

				const auto Socket = socket(Address.ss_family, SOCK_STREAM, IPPROTO_TCP);
				if (Socket == INVALID_SOCKET)
					continue;

				u_long NonBlocking = 1;
				ioctlsocket(Socket, FIONBIO, &NonBlocking);

				if (connect(Socket, reinterpret_cast<const sockaddr*>(&Address), AddressSize) == 0)
				{
					Reachable = true;
					closesocket(Socket);
					break;
				}

				if (WSAGetLastError() == WSAEWOULDBLOCK)
					Pending.push_back(Socket);
				else
					closesocket(Socket);
			}

			if (Reachable)
				break;
		}

		const auto Start = GetTickCount64();

		while (!Reachable && !Pending.empty())
		{
			const auto Elapsed = GetTickCount64() - Start;
			if (Elapsed >= TimeoutMs)
				break;

			fd_set Write, Failed;
			FD_ZERO(&Write);
			FD_ZERO(&Failed);
			for (const auto Socket: Pending)
			{
				FD_SET(Socket, &Write);
				FD_SET(Socket, &Failed);
			}

			const auto Left = TimeoutMs - Elapsed;
			timeval Timeout{static_cast<long>(Left / 1000), static_cast<long>(Left % 1000 * 1000)};
			if (select(0, nullptr, &Write, &Failed, &Timeout) <= 0)
				break;

			for (auto i = Pending.begin(); i != Pending.end();)
			{
				if (FD_ISSET(*i, &Write))
					Reachable = true;

				if (FD_ISSET(*i, &Write) || FD_ISSET(*i, &Failed))
				{
					closesocket(*i);
					i = Pending.erase(i);
				}
				else
					++i;
			}
		}

		for (const auto Socket: Pending)
			closesocket(Socket);

		WSACleanup();
		return Reachable;
	}
}
