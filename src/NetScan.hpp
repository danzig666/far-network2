#ifndef __NETSCAN_HPP__
#define __NETSCAN_HPP__

// Background network access for the Network plugin.
//
// The WNet*/NetShareEnum APIs have no timeout and no way to be cancelled, so a
// blocked call cannot be interrupted, only abandoned. Everything that can touch
// the network therefore runs on a worker thread that owns all of its data: when
// the user cancels, Far carries on at once, and the worker's late result is
// dropped when the blocked call eventually returns.

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <windows.h>
#include "NetClass.hpp"

// -- NetApi: safe to call from any thread, never touches the Far API -----------
namespace NetApi
{
	// Called with each batch as it arrives; return false to stop enumerating.
	using ResourceSink = std::function<bool(std::vector<NetResource>&&)>;

	// Returns a Win32 error code.
	DWORD EnumResources(DWORD Scope, DWORD Type, DWORD Usage, const NetResource* Parent, const ResourceSink& Sink);
	void EnumHiddenShares(const NetResource& Server, const ResourceSink& Sink);

	// These report failures through SetLastError.
	bool GetResourceInfo(const string& Name, NetResource& Result);
	bool GetResourceParent(const NetResource& Source, NetResource& Result);
	bool IsReadable(const string& Remote);
	bool IsResourceReadable(const NetResource& Res, bool ScanNetwork);
	bool AddConnection(const NetResource& Res, bool Remember);
	bool AddConnectionWithLogon(const NetResource& Res, string& Name, const string& Password, bool Remember, bool& CredentialConflict);
	bool CancelConnection(const string& LocalName, DWORD Flags, bool Force);
}

// Runs Fn on a detached worker thread that keeps the plugin DLL loaded until it ends.
bool StartWorker(std::function<void()> Fn);

// Blocks until IsDone returns true or the user presses Esc (returns false).
// A wait box with an Esc hint appears if this takes noticeably long.
bool WaitCancellable(const std::function<bool()>& IsDone);

// Runs Fn on a worker and waits for it. Fn must own everything it uses: if the
// user cancels, this returns Cancelled with ERROR_CANCELLED set while Fn keeps running.
// Otherwise the last error set by Fn is preserved.
template <typename Callable, typename Result>
Result CallCancellable(Callable Fn, Result Cancelled)
{
	struct State
	{
		Result Value;
		DWORD Error{};
		volatile LONG Done{};
		explicit State(Result Init): Value(std::move(Init)) {}
	};

	auto Shared = std::make_shared<State>(Cancelled);

	auto Body = [Shared, Fn = std::move(Fn)]() mutable
	{
		SetLastError(0);
		Shared->Value = Fn();
		Shared->Error = GetLastError();
		InterlockedExchange(&Shared->Done, 1);
	};

	if (!StartWorker(Body))
		Body();

	if (!WaitCancellable([&Shared]{ return InterlockedCompareExchange(&Shared->Done, 0, 0) != 0; }))
	{
		SetLastError(ERROR_CANCELLED);
		return Cancelled;
	}

	SetLastError(Shared->Error);
	return Shared->Value;
}

// -- Scanner: folder listings and their cache. Main thread only ----------------
namespace Scanner
{
	struct Request
	{
		string Key;
		bool HasResource = false;
		NetResource Resource;
		std::vector<NetResource> Initial; // known without touching the network
		std::vector<string> FavoriteNames; // favorites still to be resolved
		bool IsFavorites = false;
		bool NoNetwork = false; // the Initial list is the whole answer
		bool WantHiddenShares = false;
	};

	struct View
	{
		std::vector<NetResource> Items;
		bool Running = false;
		DWORD Error = 0;
		unsigned long long Version = 0;
	};

	void Startup();
	void Shutdown();

	string MakeKey(const NetResource* Res);

	// Returns what is known about a folder right now and starts a scan if needed.
	View Acquire(const Request& Req, bool Force, bool Navigated);

	// Collects worker progress into the cache.
	void Pump();

	unsigned long long Version(const string& Key);
	bool IsRunning(const string& Key);
	bool IsFresh(const string& Key);

	// Stops the scan and rejects whatever it would still deliver.
	bool Cancel(const string& Key);
	void CancelAll();
	void Invalidate(const string& Key);
	void InvalidateFavorites();

	// Modal: waits for the running scan. Returns false if the user pressed Esc.
	bool WaitForScan(const string& Key);

	// Drive mappings for the "disk" column.
	NetResourceList& Connected();
	unsigned long long ConnectedVersion();
	void RefreshConnected(); // in the background, throttled
	void RefreshConnectedNow(); // modal

	// Remembered parents, so that ".." doesn't have to ask the network.
	void RememberParent(const NetResource& Child, const NetResource* Parent);
	bool FindParent(const NetResource& Child, bool& IsRoot, NetResource& Parent);

	// The domain of this computer, remembered across sessions.
	bool FindLocalNetwork(NetResource& Result);
	void SetLocalNetwork(const NetResource& Res);
	void RefreshLocalNetwork(bool ScanNetwork);

	// Asks Far to call ProcessSynchroEventW. Safe from any thread.
	void RequestNotify();
	void NotifyReceived();
}

#endif // __NETSCAN_HPP__
