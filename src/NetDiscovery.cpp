#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#include <rpc.h>

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "NetDiscovery.hpp"

namespace
{
	const ULONGLONG ListenMs = 2500;
	const ULONGLONG ResendMs = 400;
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
			if (Sends < 2 && Elapsed >= ResendMs * Sends)
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
}
