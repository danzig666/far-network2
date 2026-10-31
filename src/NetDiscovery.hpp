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
}

#endif // __NETDISCOVERY_HPP__
