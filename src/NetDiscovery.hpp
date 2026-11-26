#ifndef __NETDISCOVERY_HPP__
#define __NETDISCOVERY_HPP__

// WS-Discovery: how computers find each other now that the Computer Browser
// service behind WNetEnumResource is gone from most networks. It is what the
// Network folder of Explorer shows, and answers in about a second.
//
// Safe to use from any thread, never touches the Far API.

#include <functional>
#include <string>

namespace Discovery
{
	struct Responder
	{
		std::wstring Endpoint;
		std::wstring Address;
		unsigned short Port;
		std::wstring Path;
	};

	struct Computer
	{
		std::wstring Name; // empty if it isn't really a computer: some devices claim anything
		std::wstring Workgroup;
	};

	// Asks the local network who is out there. Takes a couple of seconds.
	void Probe(const std::function<bool()>& IsCancelled, const std::function<void(Responder&&)>& Found);

	// Asks a responder for its name. Takes a few seconds at most.
	Computer Identify(const Responder& Target);

	// Asks every address of the local IPv4 subnets for its NetBIOS names. Finds the computers
	// that share files but have network discovery turned off, which WS-Discovery can't see.
	// Takes about two seconds. Only computers running the file server service are reported.
	void NetBiosSweep(const std::function<bool()>& IsCancelled, const std::function<void(Computer&&, std::wstring&& Address)>& Found);

	// Where a computer was last seen answering, so that reaching it doesn't depend on name resolution.
	void RememberAddress(const std::wstring& Name, const std::wstring& Address);

	// Whether the computer accepts connections on the SMB ports (445 or 139) within TimeoutMs.
	// A firewall that drops them leaves Windows waiting for half a minute or more per attempt
	// before it says ERROR_BAD_NETPATH. Returns true when it can't tell, such as when the name
	// doesn't resolve: then it's up to Windows.
	bool IsSmbReachable(const std::wstring& Server, unsigned TimeoutMs);
}

#endif // __NETDISCOVERY_HPP__
