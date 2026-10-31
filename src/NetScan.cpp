#include "NetScan.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <map>
#include <mutex>
#include <unordered_set>
#include <lm.h>
#include "NetCommon.hpp"
#include "NetDiscovery.hpp"
#include "Network.hpp"
#include "NetFavorites.hpp"
#include "NetLng.hpp"
#include "guid.hpp"
#include <PluginSettings.hpp>

// A complete listing younger than this is shown without asking the network again.
static const ULONGLONG ScanTtlMs = 30 * 1000;
static const ULONGLONG ConnectedTtlMs = 10 * 1000;
// Don't refresh the panel for every batch.
static const ULONGLONG NotifyIntervalMs = 150;
// How long a blocking call may take before the wait box appears.
static const ULONGLONG WaitBoxDelayMs = 150;
static const size_t MaxRunningScans = 8;
static const size_t MaxFavoriteWorkers = 8;

static const wchar_t StrScanCache[] = L"ScanCache";
static const size_t ScanCacheMaxSize = 1024 * 1024;
static const size_t ScanCacheMaxEntries = 128;
static const unsigned ScanCacheFormat = 2;
// The legacy browser takes its time to tell us that it isn't there, don't ask it every time.
static const unsigned long long LegacyBrowserRetrySeconds = 24 * 60 * 60;
// Should be enough for a LAN, and keeps a misbehaving one from starting thousands of threads.
static const size_t MaxDiscoveredComputers = 256;

// -- Test hooks: compiled into the test build only, see test/README.md ---------
#ifdef NETWORK_TEST_HOOKS
namespace TestHooks
{
	static int Env(const wchar_t* Name)
	{
		wchar_t Buffer[32];
		return GetEnvironmentVariableW(Name, Buffer, ARRAYSIZE(Buffer))? _wtoi(Buffer) : 0;
	}

	static bool Fake() { return Env(L"FARNET_TEST_SERVERS") > 0; }

	static void Delay() { if (const auto Ms = Env(L"FARNET_TEST_DELAY_MS")) Sleep(Ms); }

	static NetResource Make(DWORD DisplayType, DWORD Usage, const wchar_t* Remote)
	{
		NetResource Res;
		Res.dwScope = RESOURCE_GLOBALNET;
		Res.dwType = DisplayType == RESOURCEDISPLAYTYPE_SHARE? RESOURCETYPE_DISK : RESOURCETYPE_ANY;
		Res.dwDisplayType = DisplayType;
		Res.dwUsage = Usage;
		Res.lpRemoteName = Remote;
		Res.lpProvider = L"Microsoft Windows Network";
		return Res;
	}

	static DWORD Enum(DWORD Scope, const NetResource* Parent, const NetApi::ResourceSink& Sink)
	{
		Delay();

		if (Scope != RESOURCE_GLOBALNET)
			return NO_ERROR;

		if (!Parent)
			return Sink({Make(RESOURCEDISPLAYTYPE_NETWORK, RESOURCEUSAGE_CONTAINER, L"")}), NO_ERROR;

		if (Parent->dwDisplayType == RESOURCEDISPLAYTYPE_NETWORK)
			return Sink({Make(RESOURCEDISPLAYTYPE_DOMAIN, RESOURCEUSAGE_CONTAINER, L"WORKGROUP")}), NO_ERROR;

		if (Parent->dwDisplayType == RESOURCEDISPLAYTYPE_DOMAIN)
		{
			const auto Count = Env(L"FARNET_TEST_SERVERS");
			for (int I = 0; I < Count; ++I)
			{
				wchar_t Name[32];
				swprintf(Name, ARRAYSIZE(Name), L"\\\\FAKE%03d", I);
				if (!Sink({Make(RESOURCEDISPLAYTYPE_SERVER, RESOURCEUSAGE_CONTAINER, Name)}))
					return NO_ERROR;
				Delay();
			}
			return NO_ERROR;
		}

		std::vector<NetResource> Shares;
		for (const auto* Share: {L"\\docs", L"\\media"})
			Shares.push_back(Make(RESOURCEDISPLAYTYPE_SHARE, RESOURCEUSAGE_CONNECTABLE, (Parent->lpRemoteName + Share).c_str()));
		Sink(std::move(Shares));
		return NO_ERROR;
	}
}
#endif

// -- NetApi --------------------------------------------------------------------

namespace NetApi
{
	DWORD EnumResources(DWORD Scope, DWORD Type, DWORD Usage, const NetResource* Parent, const ResourceSink& Sink)
	{
#ifdef NETWORK_TEST_HOOKS
		if (TestHooks::Fake())
			return TestHooks::Enum(Scope, Parent, Sink);
		TestHooks::Delay();
#endif

		NETRESOURCE ParentRes;
		if (Parent)
			ParentRes = Parent->getNETRESOURCE();

		HANDLE Enum;
		const auto OpenResult = WNetOpenEnum(Scope, Type, Usage, Parent? &ParentRes : nullptr, &Enum);
		if (OpenResult != NO_ERROR)
			return OpenResult;

		DWORD Result = NO_ERROR;
		std::vector<BYTE> Buffer(64 * 1024);

		for (;;)
		{
			// A bounded count makes the providers return early and often.
			DWORD Count = 64;
			DWORD Size = static_cast<DWORD>(Buffer.size());
			const auto EnumResult = WNetEnumResource(Enum, &Count, Buffer.data(), &Size);

			if (EnumResult == ERROR_MORE_DATA)
			{
				if (Buffer.size() >= 16 * 1024 * 1024)
				{
					Result = EnumResult;
					break;
				}
				Buffer.resize(std::max(static_cast<size_t>(Size), Buffer.size() * 2));
				continue;
			}

			if (EnumResult != NO_ERROR)
			{
				if (EnumResult != ERROR_NO_MORE_ITEMS)
					Result = EnumResult;
				break;
			}

			std::vector<NetResource> Batch;
			Batch.reserve(Count);
			const auto Resources = reinterpret_cast<NETRESOURCE*>(Buffer.data());
			for (DWORD I = 0; I < Count; ++I)
				Batch.emplace_back(Resources[I]);

			if (!Sink(std::move(Batch)))
				break;
		}

		WNetCloseEnum(Enum);
		return Result;
	}

	void EnumHiddenShares(const NetResource& Server, const ResourceSink& Sink)
	{
#ifdef NETWORK_TEST_HOOKS
		if (TestHooks::Fake())
			return;
#endif

		DWORD Resume = 0;
		NET_API_STATUS Status;

		do
		{
			PSHARE_INFO_1 Shares = nullptr;
			DWORD Read = 0, Total = 0;
			Status = NetShareEnum(const_cast<wchar_t*>(Server.lpRemoteName.c_str()), 1, reinterpret_cast<LPBYTE*>(&Shares), MAX_PREFERRED_LENGTH, &Read, &Total, &Resume);

			if (Status != ERROR_SUCCESS && Status != ERROR_MORE_DATA)
				break;

			std::vector<NetResource> Batch;

			for (DWORD I = 0; I < Read; ++I)
			{
				const string Name = Shares[I].shi1_netname;

				if (Name.empty() || Name.back() != L'$' || Name == L"IPC$")
					continue;

				// NetShareEnum has told us all we need, no need for a WNetGetResourceInformation round trip per share.
				NetResource Res;
				Res.dwScope = RESOURCE_GLOBALNET;
				Res.dwType = (Shares[I].shi1_type & STYPE_MASK) == STYPE_PRINTQ? RESOURCETYPE_PRINT : RESOURCETYPE_DISK;
				Res.dwDisplayType = RESOURCEDISPLAYTYPE_SHARE;
				Res.dwUsage = RESOURCEUSAGE_CONNECTABLE;
				Res.lpRemoteName = Server.lpRemoteName + L"\\" + Name;
				Res.lpComment = Shares[I].shi1_remark? Shares[I].shi1_remark : L"";
				Res.lpProvider = Server.lpProvider;
				Batch.emplace_back(std::move(Res));
			}

			NetApiBufferFree(Shares);

			if (!Batch.empty() && !Sink(std::move(Batch)))
				break;
		}
		while (Status == ERROR_MORE_DATA);
	}

	bool GetResourceInfo(const string& Name, NetResource& Result)
	{
#ifdef NETWORK_TEST_HOOKS
		if (TestHooks::Fake())
		{
			TestHooks::Delay();
			Result = TestHooks::Make(RESOURCEDISPLAYTYPE_SERVER, RESOURCEUSAGE_CONTAINER, Name.c_str());
			return true;
		}
		TestHooks::Delay();
#endif

		NETRESOURCE Source{};
		Source.dwDisplayType = RESOURCEDISPLAYTYPE_GENERIC;
		Source.dwScope = RESOURCE_GLOBALNET;
		Source.dwType = RESOURCETYPE_ANY;
		Source.dwUsage = RESOURCEUSAGE_ALL;
		Source.lpRemoteName = const_cast<wchar_t*>(Name.c_str());

		std::vector<BYTE> Buffer(16 * 1024);
		LPWSTR System = nullptr;

		for (;;)
		{
			DWORD Size = static_cast<DWORD>(Buffer.size());
			const auto Error = WNetGetResourceInformation(&Source, Buffer.data(), &Size, &System);

			if (Error == ERROR_MORE_DATA && Size > Buffer.size())
			{
				Buffer.resize(Size);
				continue;
			}

			if (Error != NO_ERROR)
			{
				SetLastError(Error);
				return false;
			}

			Result = NetResource(*reinterpret_cast<NETRESOURCE*>(Buffer.data()));
			return true;
		}
	}

	bool GetResourceParent(const NetResource& Source, NetResource& Result)
	{
#ifdef NETWORK_TEST_HOOKS
		if (TestHooks::Fake())
		{
			TestHooks::Delay();
			if (Source.dwDisplayType == RESOURCEDISPLAYTYPE_SERVER)
			{
				Result = TestHooks::Make(RESOURCEDISPLAYTYPE_DOMAIN, RESOURCEUSAGE_CONTAINER, L"WORKGROUP");
				return true;
			}
			SetLastError(ERROR_BAD_NET_NAME);
			return false;
		}
		TestHooks::Delay();
#endif

		auto Res = Source.getNETRESOURCE();
		Res.dwDisplayType = RESOURCEDISPLAYTYPE_GENERIC;
		Res.dwScope = RESOURCE_GLOBALNET;
		Res.dwUsage = 0;
		Res.dwType = RESOURCETYPE_ANY;

		std::vector<BYTE> Buffer(16 * 1024);
		LPWSTR System = nullptr;
		DWORD Size = static_cast<DWORD>(Buffer.size());
		auto Error = WNetGetResourceInformation(&Res, Buffer.data(), &Size, &System);

		if (Error == ERROR_MORE_DATA && Size > Buffer.size())
		{
			Buffer.resize(Size);
			Size = static_cast<DWORD>(Buffer.size());
			Error = WNetGetResourceInformation(&Res, Buffer.data(), &Size, &System);
		}

		if (Error != NO_ERROR)
		{
			SetLastError(Error);
			return false;
		}

		const string Provider = reinterpret_cast<NETRESOURCE*>(Buffer.data())->lpProvider? reinterpret_cast<NETRESOURCE*>(Buffer.data())->lpProvider : L"";
		Res.lpProvider = const_cast<wchar_t*>(Provider.c_str());

		for (;;)
		{
			Size = static_cast<DWORD>(Buffer.size());
			Error = WNetGetResourceParent(&Res, Buffer.data(), &Size);

			if (Error == ERROR_MORE_DATA && Size > Buffer.size())
			{
				Buffer.resize(Size);
				continue;
			}

			if (Error != NO_ERROR)
			{
				SetLastError(Error);
				return false;
			}

			Result = NetResource(*reinterpret_cast<NETRESOURCE*>(Buffer.data()));
			return true;
		}
	}

	bool IsReadable(const string& Remote)
	{
#ifdef NETWORK_TEST_HOOKS
		if (TestHooks::Fake())
		{
			TestHooks::Delay();
			return true;
		}
		TestHooks::Delay();
#endif

		string Mask;

		if (Remote.compare(0, 2, L"\\\\") == 0)
			Mask = L"\\\\?\\UNC" + Remote.substr(1) + L"\\*";
		else
			Mask = Remote + L"\\*";

		WIN32_FIND_DATA FindData;
		const auto FindHandle = FindFirstFile(Mask.c_str(), &FindData);
		const auto Error = GetLastError();

		if (FindHandle != INVALID_HANDLE_VALUE)
		{
			FindClose(FindHandle);
			SetLastError(0);
			return true;
		}

		if (Error == ERROR_FILE_NOT_FOUND)
		{
			SetLastError(0);
			return true;
		}

		SetLastError(Error);
		return false;
	}

	bool IsResourceReadable(const NetResource& Res, bool ScanNetwork)
	{
		if (CheckFavoriteItem(&Res))
			return true;

		// There is nothing to log on to in a domain or in the Windows network as such. All that asking would
		// do is wait for the legacy browser, the scan is going to find out what's in there anyway.
		if ((Res.dwDisplayType == RESOURCEDISPLAYTYPE_DOMAIN || Res.dwDisplayType == RESOURCEDISPLAYTYPE_NETWORK) &&
			(!ScanNetwork || Res.lpProvider.find(L"Microsoft") != string::npos))
			return true;

#ifdef NETWORK_TEST_HOOKS
		if (TestHooks::Fake())
		{
			TestHooks::Delay();
			return true;
		}
		TestHooks::Delay();
#endif

		auto NetRes = Res.getNETRESOURCE();
		HANDLE Enum = INVALID_HANDLE_VALUE;
		auto Error = WNetOpenEnum(RESOURCE_GLOBALNET, RESOURCETYPE_ANY, 0, &NetRes, &Enum);

		if (Error != NO_ERROR)
		{
			if (!AddConnection(Res, true))
				return false;

			Error = WNetOpenEnum(RESOURCE_GLOBALNET, RESOURCETYPE_ANY, 0, &NetRes, &Enum);

			if (Error != NO_ERROR)
			{
				SetLastError(Error);
				return false;
			}
		}

		WNetCloseEnum(Enum);
		return true;
	}

	bool AddConnection(const NetResource& Res, bool Remember)
	{
		auto NetRes = Res.getNETRESOURCE();
		const auto Error = WNetAddConnection2(&NetRes, nullptr, nullptr, Remember? CONNECT_UPDATE_PROFILE : 0);
		SetLastError(Error);
		return Error == NO_ERROR;
	}

	static size_t LeafOffset(const string& Path)
	{
		const auto Pos = Path.find_last_of(L"\\/");
		return Pos == string::npos? 0 : Pos + 1;
	}

	static void DisconnectFromServer(const NetResource& Res)
	{
		// The server part of \\server or \\server\share
		const auto Leaf = LeafOffset(Res.lpRemoteName);
		const auto Server = Leaf <= 2? Res.lpRemoteName : Res.lpRemoteName.substr(0, Leaf - 1);

		std::vector<string> Connections;
		EnumResources(RESOURCE_CONNECTED, RESOURCETYPE_ANY, 0, nullptr, [&](std::vector<NetResource>&& Batch)
		{
			for (const auto& i: Batch)
				Connections.push_back(i.lpRemoteName);
			return true;
		});

		for (const auto& i: Connections)
		{
			if (lstrcmpi(i.substr(0, Server.size()).c_str(), Server.c_str()) == 0)
				WNetCancelConnection2(i.c_str(), 0, TRUE);
		}

		// Trying harder to disconnect from the server
		WNetCancelConnection2(Server.c_str(), 0, TRUE);
	}

	bool AddConnectionWithLogon(const NetResource& Res, string& Name, const string& Password, bool Remember, bool& CredentialConflict)
	{
		auto NetRes = Res.getNETRESOURCE();
		const DWORD Flags = Remember? CONNECT_UPDATE_PROFILE : 0;

		for (;;)
		{
			auto Error = WNetAddConnection2(&NetRes, Password.c_str(), Name.empty()? nullptr : Name.c_str(), Flags);

			if (Error == NO_ERROR)
				return true;

			if (Error == ERROR_SESSION_CREDENTIAL_CONFLICT)
			{
				CredentialConflict = true;
				// Trying to cancel existing connections
				DisconnectFromServer(Res);
				Error = WNetAddConnection2(&NetRes, Password.c_str(), Name.empty()? nullptr : Name.c_str(), Flags);

				if (Error == NO_ERROR)
					return true;
			}

			if (!Name.empty() && Name.find_first_of(L"\\@") == string::npos)
			{
				// If the specified user name does not look like "ComputerName\UserName" nor "User@Domain"
				// and the plug-in failed to log on to the remote machine, the specified user name can be
				// interpreted as user name of the remote computer, so let's transform it to look like
				// "Computer\User"
				const auto Start = Res.lpRemoteName.find_first_not_of(L'\\');
				const auto End = Start == string::npos? string::npos : Res.lpRemoteName.find(L'\\', Start);
				const auto Server = Start == string::npos? string{} : Res.lpRemoteName.substr(Start, End == string::npos? string::npos : End - Start);
				Name = Server + L"\\" + Name;
				// Try again to log on with the transformed user name.
				continue;
			}

			SetLastError(Error);
			return false;
		}
	}

	bool CancelConnection(const string& LocalName, DWORD Flags, bool Force)
	{
		const auto Error = WNetCancelConnection2(LocalName.c_str(), Flags, Force);
		SetLastError(Error);
		return Error == NO_ERROR;
	}
}

// -- Workers -------------------------------------------------------------------

namespace
{
	struct WorkerStartup
	{
		std::function<void()> Fn;
		HMODULE Self;
	};
}

static DWORD WINAPI WorkerProc(void* Param)
{
	const auto Self = static_cast<WorkerStartup*>(Param)->Self;

	try
	{
		std::unique_ptr<WorkerStartup> Startup(static_cast<WorkerStartup*>(Param));
		Startup->Fn();
	}
	catch (...)
	{
	}

	FreeLibraryAndExitThread(Self, 0);
}

bool StartWorker(std::function<void()> Fn)
{
	HMODULE Self;
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<const wchar_t*>(&WorkerProc), &Self))
		return false;

	auto Startup = std::make_unique<WorkerStartup>();
	Startup->Fn = std::move(Fn);
	Startup->Self = Self;

	const auto Thread = CreateThread(nullptr, 0, WorkerProc, Startup.get(), 0, nullptr);

	if (!Thread)
	{
		FreeLibrary(Self);
		return false;
	}

	Startup.release();
	CloseHandle(Thread);
	return true;
}

// Only looks at Esc: anything else typed in the meantime stays queued for Far.
static bool ConsumeEsc(HANDLE Input)
{
	INPUT_RECORD Records[256];
	DWORD Count = 0;

	if (!PeekConsoleInputW(Input, Records, ARRAYSIZE(Records), &Count) || !Count)
		return false;

	for (DWORD I = 0; I < Count; ++I)
	{
		if (Records[I].EventType == KEY_EVENT && Records[I].Event.KeyEvent.bKeyDown && Records[I].Event.KeyEvent.wVirtualKeyCode == VK_ESCAPE)
		{
			DWORD Read;
			ReadConsoleInputW(Input, Records, I + 1, &Read);
			return true;
		}
	}

	// Don't let a full queue hide an Esc behind it.
	if (Count == ARRAYSIZE(Records))
	{
		DWORD Read;
		ReadConsoleInputW(Input, Records, Count, &Read);
	}

	return false;
}

bool WaitCancellable(const std::function<bool()>& IsDone)
{
	const auto Input = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
	const auto Start = GetTickCount64();
	std::unique_ptr<TSaveScreen> Box;
	bool Result = true;

	for (DWORD Pause = 1; !IsDone(); Pause = std::min(Pause * 2, 20ul))
	{
		if (Input != INVALID_HANDLE_VALUE && ConsumeEsc(Input))
		{
			Result = false;
			break;
		}

		if (!Box && GetTickCount64() - Start >= WaitBoxDelayMs)
			Box = std::make_unique<TSaveScreen>();

		Sleep(Pause);
	}

	Box.reset();

	if (Input != INVALID_HANDLE_VALUE)
		CloseHandle(Input);

	return Result;
}

// -- Scanner -------------------------------------------------------------------

namespace Scanner
{
	static std::atomic<bool> NotifyPending{false};
	static std::mutex PostLock;
	static bool Accepting = false;

	void RequestNotify()
	{
		if (NotifyPending.exchange(true))
			return;

		std::lock_guard<std::mutex> Lock(PostLock);

		if (Accepting)
			PsInfo.AdvControl(&MainGuid, ACTL_SYNCHRO, 0, nullptr);
		else
			NotifyPending = false;
	}

	void NotifyReceived()
	{
		NotifyPending = false;
	}

	static string ItemKey(const NetResource& Res)
	{
		return Res.lpProvider + L"|" + Res.lpRemoteName;
	}

	// The same computer can be reported by more than one source, in more than one spelling.
	static string ItemIdentity(const NetResource& Res)
	{
		auto Identity = ItemKey(Res);
		if (!Identity.empty())
			CharUpperBuffW(&Identity[0], static_cast<DWORD>(Identity.size()));
		return Identity;
	}

	class ScanJob
	{
	public:
		std::atomic<bool> Cancelled{false};
		std::atomic<int> Workers{1};
		std::atomic<size_t> NextFavorite{0};

		void Seed(std::vector<NetResource> Initial)
		{
			for (auto& i: Initial)
				if (m_Known.insert(ItemIdentity(i)).second)
					m_Items.emplace_back(std::move(i));
		}

		bool Add(std::vector<NetResource>&& Batch)
		{
			if (Cancelled)
				return false;

			{
				std::lock_guard<std::mutex> Lock(m_Lock);
				for (auto& i: Batch)
					if (m_Known.insert(ItemIdentity(i)).second)
						m_Items.emplace_back(std::move(i));
				++m_Sequence;
			}

			const auto Now = GetTickCount64();
			if (Now - m_LastNotify.load() >= NotifyIntervalMs)
			{
				m_LastNotify = Now;
				RequestNotify();
			}

			return !Cancelled;
		}

		void Clear()
		{
			std::lock_guard<std::mutex> Lock(m_Lock);
			m_Items.clear();
			m_Known.clear();
			++m_Sequence;
		}

		// Called by each worker when it's done, the last one completes the job.
		void Finish(DWORD Error)
		{
			{
				std::lock_guard<std::mutex> Lock(m_Lock);

				if (Error != NO_ERROR)
					m_Error = Error;

				if (Workers.fetch_sub(1) == 1)
					m_Finished = true;

				++m_Sequence;
			}

			// Even if others are still working: Add might have held back this worker's last results.
			RequestNotify();
		}

		struct Snapshot
		{
			std::vector<NetResource> Items;
			unsigned long long Sequence;
			bool Finished;
			DWORD Error;
		};

		unsigned long long Sequence()
		{
			std::lock_guard<std::mutex> Lock(m_Lock);
			return m_Sequence;
		}

		Snapshot Take()
		{
			std::lock_guard<std::mutex> Lock(m_Lock);
			return {m_Items, m_Sequence, m_Finished, m_Error};
		}

		bool Finished()
		{
			std::lock_guard<std::mutex> Lock(m_Lock);
			return m_Finished;
		}

	private:
		std::mutex m_Lock;
		std::vector<NetResource> m_Items;
		std::unordered_set<string> m_Known;
		unsigned long long m_Sequence = 0;
		bool m_Finished = false;
		DWORD m_Error = 0;
		std::atomic<ULONGLONG> m_LastNotify{0};
	};

	static void ResolveFavorites(const std::shared_ptr<ScanJob>& Job, const std::shared_ptr<const std::vector<string>>& Names)
	{
		for (;;)
		{
			const auto Index = Job->NextFavorite.fetch_add(1);

			if (Job->Cancelled || Index >= Names->size())
				break;

			NetResource Res;
			if (NetApi::GetResourceInfo((*Names)[Index], Res))
				Job->Add({std::move(Res)});
		}

		Job->Finish(NO_ERROR);
	}

	static std::atomic<unsigned long long> LegacyBrowserFailed{0};

	static unsigned long long SecondsNow()
	{
		FILETIME Now;
		GetSystemTimeAsFileTime(&Now);
		return ((static_cast<unsigned long long>(Now.dwHighDateTime) << 32) | Now.dwLowDateTime) / 10000000;
	}

	static bool IsWindowsNetworkFolder(const Request& Req)
	{
#ifdef NETWORK_TEST_HOOKS
		// With the fake network, the fake browser is the scan, unless the test is about discovery.
		if (TestHooks::Fake() && !TestHooks::Env(L"FARNET_TEST_DISCOVERY"))
			return false;
#endif

		return Req.HasResource &&
			(Req.Resource.dwDisplayType == RESOURCEDISPLAYTYPE_DOMAIN || Req.Resource.dwDisplayType == RESOURCEDISPLAYTYPE_NETWORK) &&
			Req.Resource.lpProvider.find(L"Microsoft Windows") != string::npos;
	}

	static NetResource MakeDiscovered(const Request& Req, DWORD DisplayType, const string& RemoteName, const string& Comment)
	{
		NetResource Res;
		Res.dwScope = RESOURCE_GLOBALNET;
		Res.dwType = RESOURCETYPE_ANY;
		Res.dwDisplayType = DisplayType;
		Res.dwUsage = RESOURCEUSAGE_CONTAINER;
		Res.lpRemoteName = RemoteName;
		Res.lpComment = Comment;
		Res.lpProvider = Req.Resource.lpProvider;
		return Res;
	}

	static void IdentifyComputer(const std::shared_ptr<ScanJob>& Job, const std::shared_ptr<const Request>& Req, const Discovery::Responder& Target)
	{
		if (!Job->Cancelled)
		{
			const auto Found = Discovery::Identify(Target);

			if (Req->Resource.dwDisplayType == RESOURCEDISPLAYTYPE_NETWORK)
			{
				if (!Found.Workgroup.empty())
				{
					auto Name = Found.Workgroup;
					CharUpperBuffW(&Name[0], static_cast<DWORD>(Name.size()));
					Job->Add({MakeDiscovered(*Req, RESOURCEDISPLAYTYPE_DOMAIN, Name, {})});
				}
			}
			else if (!Found.Name.empty())
			{
				auto Name = Found.Name;
				CharUpperBuffW(&Name[0], static_cast<DWORD>(Name.size()));
				auto Comment = Target.Address;
				if (!Found.Workgroup.empty())
					Comment += L" (" + Found.Workgroup + L")";
				Job->Add({MakeDiscovered(*Req, RESOURCEDISPLAYTYPE_SERVER, L"\\\\" + Name, Comment)});
			}
		}

		Job->Finish(NO_ERROR);
	}

	static void DiscoverComputers(const std::shared_ptr<ScanJob>& Job, const std::shared_ptr<const Request>& Req)
	{
#ifdef NETWORK_TEST_HOOKS
		if (TestHooks::Fake())
		{
			Job->Add({MakeDiscovered(*Req, RESOURCEDISPLAYTYPE_SERVER, L"\\\\DISC000", L"")});
			Job->Finish(NO_ERROR);
			return;
		}
#endif

		size_t Count = 0;

		Discovery::Probe([&Job]{ return Job->Cancelled.load(); }, [&](Discovery::Responder&& Target)
		{
			if (++Count > MaxDiscoveredComputers)
				return;

			// Don't let one that doesn't answer hold up the others.
			++Job->Workers;
			const auto Started = StartWorker([Job, Req, Target]{ IdentifyComputer(Job, Req, Target); });

			if (!Started)
				Job->Finish(NO_ERROR);
		});

		Job->Finish(NO_ERROR);
	}

	static void ScanFolder(const std::shared_ptr<ScanJob>& Job, const Request& Req)
	{
		const auto Parent = Req.HasResource? &Req.Resource : nullptr;
		const auto Sink = [&Job](std::vector<NetResource>&& Batch) { return Job->Add(std::move(Batch)); };

		auto Error = NetApi::EnumResources(RESOURCE_GLOBALNET, RESOURCETYPE_ANY, 0, Parent, Sink);

		if (IsWindowsNetworkFolder(Req))
		{
			// No browser on this network. Not a problem, discovery is on it, and connecting to a domain wouldn't help.
			if (Error != NO_ERROR)
				LegacyBrowserFailed = SecondsNow();

			Job->Finish(NO_ERROR);
			return;
		}

		if (Error != NO_ERROR)
		{
			if (!Parent)
			{
				Job->Finish(Error);
				return;
			}

			// try again with connection
			NetApi::AddConnection(*Parent, true);

			if (NetApi::EnumResources(RESOURCE_GLOBALNET, RESOURCETYPE_ANY, 0, Parent, Sink) != NO_ERROR)
				Job->Clear();

			Error = NO_ERROR;
		}

		if (!Job->Cancelled && Req.WantHiddenShares && Parent && Req.Resource.dwDisplayType == RESOURCEDISPLAYTYPE_SERVER)
		{
			const auto Items = Job->Take().Items;

			// If there are elements, check that they are shares
			if (Items.empty() || Items.back().dwDisplayType == RESOURCEDISPLAYTYPE_SHARE)
				NetApi::EnumHiddenShares(Req.Resource, Sink);
		}

		Job->Finish(Error);
	}

	struct Entry
	{
		std::vector<NetResource> Items;
		bool Complete = false; // the scan ran to the end
		bool Stale = false; // restored from the previous session
		bool HiddenShares = false;
		DWORD Error = 0;
		ULONGLONG Updated = 0;
		unsigned long long Version = 0;
		std::shared_ptr<ScanJob> Job;
		unsigned long long JobSequence = 0;
		ULONGLONG JobStarted = 0;

		// The legacy browser can take a minute to tell us that it isn't there, and nobody should have
		// to watch that. It doesn't hold up the scan: whatever it finds is added as it turns up.
		std::shared_ptr<ScanJob> LateJob;
		unsigned long long LateSequence = 0;
		std::vector<NetResource> ScanItems; // what the scan itself found
	};

	struct ParentEntry
	{
		bool IsRoot = false;
		NetResource Parent;
	};

	struct ConnectedScan
	{
		std::mutex Lock;
		std::vector<NetResource> Items;
		bool Finished = false;
	};

	struct LocalNetworkScan
	{
		std::mutex Lock;
		bool Finished = false;
		bool Succeeded = false;
		NetResource Result;
	};

	struct State
	{
		std::map<string, Entry> Entries;
		std::map<string, ParentEntry> Parents;
		unsigned long long Revision = 0;

		NetResourceList Connected;
		unsigned long long ConnectedRevision = 0;
		ULONGLONG ConnectedUpdated = 0;
		std::shared_ptr<ConnectedScan> ConnectedJob;

		bool HasLocalNetwork = false;
		NetResource LocalNetwork;
		std::shared_ptr<LocalNetworkScan> LocalNetworkJob;
	};

	static State* S;

	// -- Persistence: lets the first visit after a restart start with something to show.
	// Only resource names and comments are stored, never credentials.

	class Writer
	{
	public:
		void Number(unsigned Value)
		{
			const auto Bytes = reinterpret_cast<const BYTE*>(&Value);
			Data.insert(Data.end(), Bytes, Bytes + sizeof(Value));
		}

		void Text(const string& Value)
		{
			Number(static_cast<unsigned>(Value.size()));
			const auto Bytes = reinterpret_cast<const BYTE*>(Value.data());
			Data.insert(Data.end(), Bytes, Bytes + Value.size() * sizeof(wchar_t));
		}

		void Resource(const NetResource& Res)
		{
			Number(Res.dwScope);
			Number(Res.dwType);
			Number(Res.dwDisplayType);
			Number(Res.dwUsage);
			Text(Res.lpLocalName);
			Text(Res.lpRemoteName);
			Text(Res.lpComment);
			Text(Res.lpProvider);
		}

		std::vector<BYTE> Data;
	};

	class Reader
	{
	public:
		Reader(const BYTE* Data, size_t Size): m_Data(Data), m_Size(Size) {}

		bool Number(unsigned& Value)
		{
			if (m_Size - m_Pos < sizeof(Value))
				return false;
			memcpy(&Value, m_Data + m_Pos, sizeof(Value));
			m_Pos += sizeof(Value);
			return true;
		}

		bool Text(string& Value)
		{
			unsigned Length;
			if (!Number(Length) || Length > (m_Size - m_Pos) / sizeof(wchar_t))
				return false;
			Value.resize(Length);
			if (Length)
				memcpy(&Value[0], m_Data + m_Pos, Length * sizeof(wchar_t));
			m_Pos += Length * sizeof(wchar_t);
			return true;
		}

		bool Resource(NetResource& Res)
		{
			unsigned Scope, Type, DisplayType, Usage;
			if (!Number(Scope) || !Number(Type) || !Number(DisplayType) || !Number(Usage))
				return false;
			Res.dwScope = Scope;
			Res.dwType = Type;
			Res.dwDisplayType = DisplayType;
			Res.dwUsage = Usage;
			return Text(Res.lpLocalName) && Text(Res.lpRemoteName) && Text(Res.lpComment) && Text(Res.lpProvider);
		}

	private:
		const BYTE* m_Data;
		size_t m_Size;
		size_t m_Pos = 0;
	};

	static void Load()
	{
		std::vector<BYTE> Data(ScanCacheMaxSize);
		size_t Size;
		{
			PluginSettings Settings(MainGuid, PsInfo.SettingsControl);
			Size = Settings.Get(0, StrScanCache, Data.data(), Data.size());
		}

		Reader In(Data.data(), Size);
		unsigned Format, HasLocal, Count;

		if (!In.Number(Format) || Format != ScanCacheFormat)
			return;

		unsigned FailedLow, FailedHigh;
		if (!In.Number(FailedLow) || !In.Number(FailedHigh) || !In.Number(HasLocal))
			return;

		LegacyBrowserFailed = (static_cast<unsigned long long>(FailedHigh) << 32) | FailedLow;

		if (HasLocal)
		{
			if (!In.Resource(S->LocalNetwork))
				return;
			S->HasLocalNetwork = true;
		}

		if (!In.Number(Count))
			return;

		for (unsigned I = 0; I < Count; ++I)
		{
			string Key;
			unsigned IsRoot;
			ParentEntry Parent;

			if (!In.Text(Key) || !In.Number(IsRoot) || !In.Resource(Parent.Parent))
				return;

			Parent.IsRoot = IsRoot != 0;
			S->Parents.emplace(std::move(Key), std::move(Parent));
		}

		if (!In.Number(Count))
			return;

		for (unsigned I = 0; I < Count; ++I)
		{
			string Key;
			unsigned HiddenShares, ItemCount;

			if (!In.Text(Key) || !In.Number(HiddenShares) || !In.Number(ItemCount))
				return;

			Entry Item;
			Item.Complete = true;
			Item.Stale = true;
			Item.HiddenShares = HiddenShares != 0;
			Item.Items.reserve(std::min(ItemCount, 4096u));

			for (unsigned J = 0; J < ItemCount; ++J)
			{
				NetResource Res;
				if (!In.Resource(Res))
					return;
				Item.Items.emplace_back(std::move(Res));
			}

			S->Entries.emplace(std::move(Key), std::move(Item));
		}
	}

	static void Save()
	{
		std::vector<const std::pair<const string, Entry>*> Saved;

		for (const auto& i: S->Entries)
		{
			// Favorites depend on the settings and are cheap to find out. Partial listings aren't worth keeping.
			if (i.second.Complete && !i.second.Items.empty() && i.first.compare(0, wcslen(FavoritesProvider()), FavoritesProvider()) != 0)
				Saved.push_back(&i);
		}

		std::sort(Saved.begin(), Saved.end(), [](const auto* a, const auto* b) { return a->second.Updated > b->second.Updated; });

		if (Saved.size() > ScanCacheMaxEntries)
			Saved.resize(ScanCacheMaxEntries);

		Writer Head;
		const auto Failed = LegacyBrowserFailed.load();
		Head.Number(ScanCacheFormat);
		Head.Number(static_cast<unsigned>(Failed));
		Head.Number(static_cast<unsigned>(Failed >> 32));
		Head.Number(S->HasLocalNetwork? 1 : 0);
		if (S->HasLocalNetwork)
			Head.Resource(S->LocalNetwork);

		Head.Number(static_cast<unsigned>(S->Parents.size()));
		for (const auto& i: S->Parents)
		{
			Head.Text(i.first);
			Head.Number(i.second.IsRoot? 1 : 0);
			Head.Resource(i.second.Parent);
		}

		// Drop the oldest listings if this doesn't fit.
		std::vector<std::vector<BYTE>> Blobs;
		size_t Total = Head.Data.size() + sizeof(unsigned);

		for (const auto* i: Saved)
		{
			Writer Item;
			Item.Text(i->first);
			Item.Number(i->second.HiddenShares? 1 : 0);
			Item.Number(static_cast<unsigned>(i->second.Items.size()));
			for (const auto& Res: i->second.Items)
				Item.Resource(Res);

			if (Total + Item.Data.size() > ScanCacheMaxSize)
				break;

			Total += Item.Data.size();
			Blobs.emplace_back(std::move(Item.Data));
		}

		if (Head.Data.size() + sizeof(unsigned) > ScanCacheMaxSize)
			return;

		Head.Number(static_cast<unsigned>(Blobs.size()));
		for (const auto& i: Blobs)
			Head.Data.insert(Head.Data.end(), i.begin(), i.end());

		PluginSettings Settings(MainGuid, PsInfo.SettingsControl);
		Settings.Set(0, StrScanCache, Head.Data.data(), Head.Data.size());
	}

	void Startup()
	{
		{
			std::lock_guard<std::mutex> Lock(PostLock);
			Accepting = true;
		}

		S = new State{};
		Load();
	}

	void Shutdown()
	{
		if (!S)
			return;

		Pump();
		CancelAll();

		{
			// Far must not hear from us after ExitFARW, late workers just finish quietly.
			std::lock_guard<std::mutex> Lock(PostLock);
			Accepting = false;
		}

		Save();
		delete S;
		S = nullptr;
	}

	string MakeKey(const NetResource* Res)
	{
		return Res? ItemKey(*Res) : string{};
	}

	static std::vector<NetResource> Merged(const std::vector<NetResource>& Fresh, const std::vector<NetResource>& Old)
	{
		auto Result = Fresh;
		std::unordered_set<string> Known;
		for (const auto& i: Fresh)
			Known.insert(ItemIdentity(i));
		for (const auto& i: Old)
			if (Known.insert(ItemIdentity(i)).second)
				Result.emplace_back(i);
		return Result;
	}

	static void Touch(Entry& Item)
	{
		Item.Version = ++S->Revision;
	}

	static void CancelEntry(Entry& Item)
	{
		if (Item.LateJob)
		{
			Item.LateJob->Cancelled = true;

			if (!Item.Job)
			{
				Item.Items = Merged(Item.LateJob->Take().Items, Item.Items);
				Touch(Item);
			}

			Item.LateJob.reset();
		}

		if (!Item.Job)
			return;

		// Keep what has been found so far, but stop looking at the job: anything it delivers later is rejected.
		Item.Job->Cancelled = true;
		Item.Items = Merged(Item.Job->Take().Items, Item.Items);
		Item.Job.reset();
		Item.Complete = false;
		Item.Stale = false;
		Item.Error = 0;
		Item.Updated = GetTickCount64();
		Touch(Item);
	}

	static void StartScan(const string& Key, Entry& Item, const Request& Req, bool Force)
	{
		size_t Running = 0;
		Entry* Oldest = nullptr;

		for (auto& i: S->Entries)
		{
			if (!i.second.Job)
				continue;
			++Running;
			if (&i.second != &Item && (!Oldest || i.second.JobStarted < Oldest->JobStarted))
				Oldest = &i.second;
		}

		// Blocked workers pile up on a bad network, give up on the ones nobody is likely to be waiting for.
		if (Running >= MaxRunningScans && Oldest)
			CancelEntry(*Oldest);

		auto Job = std::make_shared<ScanJob>();
		Job->Seed(Req.Initial);

		Item.Job = Job;
		Item.JobSequence = 0;
		Item.JobStarted = GetTickCount64();
		Item.HiddenShares = Req.WantHiddenShares;
		Item.Error = 0;
		Touch(Item);

		bool Started;

		if (Req.IsFavorites)
		{
			auto Names = std::make_shared<const std::vector<string>>(Req.FavoriteNames);
			const auto Count = std::max<size_t>(std::min(MaxFavoriteWorkers, Names->size()), 1);
			Job->Workers = static_cast<int>(Count);
			Started = true;

			// One dead server shouldn't hold up the others.
			for (size_t I = 0; I < Count; ++I)
			{
				if (!StartWorker([Job, Names]{ ResolveFavorites(Job, Names); }))
					Job->Finish(NO_ERROR);
			}
		}
		else if (IsWindowsNetworkFolder(Req))
		{
			auto Shared = std::make_shared<const Request>(Req);
			Started = true;

			if (!Item.LateJob && (Force || SecondsNow() - LegacyBrowserFailed.load() >= LegacyBrowserRetrySeconds))
			{
				auto Late = std::make_shared<ScanJob>();

				if (StartWorker([Late, Shared]{ ScanFolder(Late, *Shared); }))
				{
					Item.LateJob = std::move(Late);
					Item.LateSequence = 0;
				}
			}

			if (!StartWorker([Job, Shared]{ DiscoverComputers(Job, Shared); }))
				Job->Finish(NO_ERROR);
		}
		else
		{
			Started = StartWorker([Job, Req]{ ScanFolder(Job, Req); });
		}

		if (!Started)
			Job->Finish(ERROR_NOT_ENOUGH_MEMORY);
	}

	void Pump()
	{
		// A notification may still have been in the queue when Far called ExitFARW.
		if (!S)
			return;

		for (auto& i: S->Entries)
		{
			auto& Item = i.second;

			if (Item.Job)
			{
				const auto Sequence = Item.Job->Sequence();

				if (Sequence != Item.JobSequence)
				{
					Item.JobSequence = Sequence;
					Touch(Item);
				}

				if (Item.Job->Finished())
				{
					auto Result = Item.Job->Take();

					if (Result.Error != NO_ERROR)
					{
						// Don't throw away a list we have because of a failed refresh.
						Item.Items = Merged(Result.Items, Item.Items);
						Item.Complete = false;
						Item.Error = Result.Error;
					}
					else
					{
						if (Item.LateJob)
						{
							// Things only the legacy browser knows about stay until it has spoken.
							Item.ScanItems = Result.Items;
							Item.Items = Merged(Result.Items, Item.Items);
						}
						else
							Item.Items = std::move(Result.Items);

						Item.Complete = true;
						Item.Error = 0;
					}

					Item.Stale = false;
					Item.Updated = GetTickCount64();
					Item.Job.reset();
					Touch(Item);
				}
			}

			if (Item.LateJob && !Item.Job)
			{
				auto Late = Item.LateJob->Take();

				if (Late.Sequence != Item.LateSequence)
				{
					Item.LateSequence = Late.Sequence;
					Item.Items = Late.Finished? Merged(Item.ScanItems, Late.Items) : Merged(Item.Items, Late.Items);
					Touch(Item);
				}

				if (Late.Finished)
				{
					Item.LateJob.reset();
					Touch(Item);
				}
			}
		}

		if (S->ConnectedJob)
		{
			std::vector<NetResource> Items;
			bool Finished;
			{
				std::lock_guard<std::mutex> Lock(S->ConnectedJob->Lock);
				Finished = S->ConnectedJob->Finished;
				if (Finished)
					Items = std::move(S->ConnectedJob->Items);
			}

			if (Finished)
			{
				S->ConnectedJob.reset();
				S->ConnectedUpdated = GetTickCount64();

				bool Changed = Items.size() != S->Connected.Count();

				for (size_t I = 0; !Changed && I != Items.size(); ++I)
					Changed = Items[I].lpRemoteName != S->Connected[I].lpRemoteName || Items[I].lpLocalName != S->Connected[I].lpLocalName;

				if (Changed)
				{
					S->Connected.Clear();
					for (const auto& i: Items)
						S->Connected.Push(i);
					++S->ConnectedRevision;
				}
			}
		}

		if (S->LocalNetworkJob)
		{
			std::lock_guard<std::mutex> Lock(S->LocalNetworkJob->Lock);

			if (S->LocalNetworkJob->Finished)
			{
				if (S->LocalNetworkJob->Succeeded)
				{
					S->LocalNetwork = S->LocalNetworkJob->Result;
					S->HasLocalNetwork = true;
				}
			}
		}
	}

	// With an empty list the legacy browser is still our only hope, so it does count then.
	static bool IsSearching(const Entry& Item)
	{
		return Item.Job || (Item.LateJob && Item.Items.empty());
	}

	View Acquire(const Request& Req, bool Force, bool Navigated)
	{
		Pump();

		auto& Item = S->Entries[Req.Key];

		if (Req.NoNetwork)
		{
			CancelEntry(Item);
			Item.Items = Req.Initial;
			Item.Complete = true;
			Item.Stale = false;
			Item.Error = 0;
			Item.Updated = GetTickCount64();
		}
		else
		{
			const auto Age = GetTickCount64() - Item.Updated;
			const auto NeverScanned = !Item.Updated && !Item.Stale;

			const auto NeedScan =
				Force ||
				NeverScanned ||
				(Req.WantHiddenShares && !Item.HiddenShares) ||
				Item.Stale ||
				(Item.Complete && Age >= ScanTtlMs) ||
				(!Item.Complete && Navigated); // a cancelled or failed scan is not retried until asked for

			if (Force)
				CancelEntry(Item);

			if (NeedScan && !Item.Job)
				StartScan(Req.Key, Item, Req, Force);
		}

		View Result;
		Result.Running = IsSearching(Item);
		Result.Error = Item.Error;
		Result.Items = Item.Job? Merged(Item.Job->Take().Items, Item.Items) : Item.Items;
		Result.Version = Item.Version;

		if (Item.Job)
			Item.JobSequence = Item.Job->Sequence();

		return Result;
	}

	unsigned long long Version(const string& Key)
	{
		const auto Found = S->Entries.find(Key);
		return Found == S->Entries.end()? 0 : Found->second.Version;
	}

	bool IsRunning(const string& Key)
	{
		const auto Found = S->Entries.find(Key);
		return Found != S->Entries.end() && IsSearching(Found->second);
	}

	bool IsFresh(const string& Key)
	{
		const auto Found = S->Entries.find(Key);
		return Found != S->Entries.end() &&
			Found->second.Complete && !Found->second.Stale && !Found->second.Error &&
			GetTickCount64() - Found->second.Updated < ScanTtlMs;
	}

	bool Cancel(const string& Key)
	{
		const auto Found = S->Entries.find(Key);

		if (Found == S->Entries.end() || !IsSearching(Found->second))
			return false;

		CancelEntry(Found->second);
		return true;
	}

	void CancelAll()
	{
		for (auto& i: S->Entries)
			CancelEntry(i.second);
	}

	void Invalidate(const string& Key)
	{
		const auto Found = S->Entries.find(Key);

		if (Found == S->Entries.end())
			return;

		CancelEntry(Found->second);
		S->Entries.erase(Found);
	}

	void InvalidateFavorites()
	{
		const auto* Provider = FavoritesProvider();
		const auto Length = wcslen(Provider);

		for (auto i = S->Entries.begin(); i != S->Entries.end();)
		{
			if (i->first.compare(0, Length, Provider) == 0)
			{
				CancelEntry(i->second);
				i = S->Entries.erase(i);
			}
			else
				++i;
		}
	}

	bool WaitForScan(const string& Key)
	{
		const auto Found = S->Entries.find(Key);

		if (Found == S->Entries.end() || !IsSearching(Found->second))
			return true;

		const auto Job = Found->second.Job? Found->second.Job : Found->second.LateJob;

		if (!WaitCancellable([&Job]{ return Job->Finished(); }))
		{
			Cancel(Key);
			SetLastError(ERROR_CANCELLED);
			return false;
		}

		Pump();
		return true;
	}

	NetResourceList& Connected()
	{
		return S->Connected;
	}

	unsigned long long ConnectedVersion()
	{
		return S->ConnectedRevision;
	}

	static void ScanConnected(const std::shared_ptr<ConnectedScan>& Job)
	{
		std::vector<NetResource> Items;

		// get the list of connections, so that we can show mapped drive letters
		NetApi::EnumResources(RESOURCE_CONNECTED, RESOURCETYPE_DISK, 0, nullptr, [&Items](std::vector<NetResource>&& Batch)
		{
			Items.insert(Items.end(), std::make_move_iterator(Batch.begin()), std::make_move_iterator(Batch.end()));
			return true;
		});

		{
			std::lock_guard<std::mutex> Lock(Job->Lock);
			Job->Items = std::move(Items);
			Job->Finished = true;
		}

		RequestNotify();
	}

	void RefreshConnected()
	{
		if (S->ConnectedJob || (S->ConnectedUpdated && GetTickCount64() - S->ConnectedUpdated < ConnectedTtlMs))
			return;

		auto Job = std::make_shared<ConnectedScan>();
		if (StartWorker([Job]{ ScanConnected(Job); }))
			S->ConnectedJob = std::move(Job);
	}

	void RefreshConnectedNow()
	{
		const auto PreviousError = GetLastError();
		S->ConnectedUpdated = 0;
		RefreshConnected();

		if (const auto Job = S->ConnectedJob)
		{
			WaitCancellable([&Job]
			{
				std::lock_guard<std::mutex> Lock(Job->Lock);
				return Job->Finished;
			});
			Pump();
		}

		SetLastError(PreviousError);
	}

	void RememberParent(const NetResource& Child, const NetResource* Parent)
	{
		auto& Item = S->Parents[ItemKey(Child)];
		Item.IsRoot = !Parent;
		Item.Parent = Parent? *Parent : NetResource{};
	}

	bool FindParent(const NetResource& Child, bool& IsRoot, NetResource& Parent)
	{
		const auto Found = S->Parents.find(ItemKey(Child));

		if (Found == S->Parents.end())
			return false;

		IsRoot = Found->second.IsRoot;
		Parent = Found->second.Parent;
		return true;
	}

	bool FindLocalNetwork(NetResource& Result)
	{
		if (!S->HasLocalNetwork)
			return false;

		Result = S->LocalNetwork;
		return true;
	}

	void SetLocalNetwork(const NetResource& Res)
	{
		S->LocalNetwork = Res;
		S->HasLocalNetwork = true;
	}

	void RefreshLocalNetwork(bool ScanNetwork)
	{
		if (!ScanNetwork || S->LocalNetworkJob)
			return;

		auto Job = std::make_shared<LocalNetworkScan>();

		const auto Started = StartWorker([Job]
		{
			wchar_t ComputerName[MAX_PATH] = L"\\\\";
			DWORD Length = MAX_PATH - 3;
			NetResource Self, Parent;
			const auto Succeeded = GetComputerName(ComputerName + 2, &Length) &&
				NetApi::GetResourceInfo(ComputerName, Self) &&
				NetApi::GetResourceParent(Self, Parent);

			std::lock_guard<std::mutex> Lock(Job->Lock);
			Job->Succeeded = Succeeded;
			Job->Result = std::move(Parent);
			Job->Finished = true;
		});

		if (Started)
			S->LocalNetworkJob = std::move(Job);
	}
}
