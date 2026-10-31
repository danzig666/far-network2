#ifndef __NETCLASS_HPP__
#define __NETCLASS_HPP__

#include <memory>
#include <string>
#include <vector>
#include <plugin.hpp>

#ifdef NETWORK_LOGGING
#include <cstdio>
#endif

using string = std::wstring;

class NetResource
{
public:
	DWORD dwScope{};
	DWORD dwType{};
	DWORD dwDisplayType{};
	DWORD dwUsage{};
	string lpLocalName;
	string lpRemoteName;
	string lpComment;
	string lpProvider;

public:
	NetResource() = default;

	explicit NetResource(NETRESOURCE& other)
	{
		dwScope = other.dwScope;
		dwType = other.dwType;
		dwDisplayType = other.dwDisplayType;
		dwUsage = other.dwUsage;
		lpLocalName = other.lpLocalName? other.lpLocalName : L"";
		lpRemoteName = other.lpRemoteName? other.lpRemoteName : L"";
		lpComment = other.lpComment? other.lpComment : L"";
		lpProvider = other.lpProvider? other.lpProvider : L"";
	}

	NETRESOURCE getNETRESOURCE() const
	{
		NETRESOURCE tmp{};

		tmp.dwScope = dwScope;
		tmp.dwType = dwType;
		tmp.dwDisplayType = dwDisplayType;
		tmp.dwUsage = dwUsage;
		tmp.lpLocalName = lpLocalName.empty()?
			                  nullptr :
			                  const_cast<wchar_t*>(lpLocalName.data());
		tmp.lpRemoteName = lpRemoteName.empty()?
			                   nullptr :
			                   const_cast<wchar_t*>(lpRemoteName.data());
		tmp.lpComment = lpComment.empty()? nullptr : const_cast<wchar_t*>(lpComment.data());
		tmp.lpProvider = lpProvider.empty()? nullptr : const_cast<wchar_t*>(lpProvider.data());
		return tmp;
	}
};

class NetResourceList
{
private:
	std::vector<NetResource> ResList;

public:
	NetResourceList() = default;

	void Clear();

	void Assign(std::vector<NetResource>&& Items) { ResList = std::move(Items); }
	const std::vector<NetResource>& Items() const { return ResList; }

	void Push(const NetResource& Res);
	NetResource* Top();
	void Pop();

	size_t Count() { return ResList.size(); }

	NetResource& operator[](size_t index) { return ResList[index]; }
};

struct NameAndPassInfo
{
	wchar_t* Title;
	wchar_t* Name;
	wchar_t* Password;
	LPBOOL pRemember;
	wchar_t* szFavoritePath;
};

class NetBrowser
{
private:
	NetResourceList NetList; // list of resources in the current folder
	string NetListKey; // scanner key of the folder stored in NetList
	bool NetListLoaded = false;
	bool Scanning = false; // NetList is still being filled in the background
	DWORD ScanError = 0;
	bool ForceRescan = false; // the user asked for a refresh
	unsigned long long SeenVersion = 0; // what the panel is showing
	unsigned long long SeenConnectedVersion = 0;
	string PendingCursor; // item to put the cursor on as soon as it shows up
	string PanelCurDir;
	string PanelTitle;
	NetResourceList RootResources; // stack of resources above the current level
	// (used in non-MS Windows networks only)
	NetResource CurResource; // NETRESOURCE describing the current location
	NetResource* PCurResource{}; // points to CurResource or nullptr (if at root)

	bool ChangeDirSuccess = true;
	bool OpenFromFilePanel = false;
	int ReenterGetFindData = 0;
	wchar_t m_PanelMode[2]; // current start panel mode

	void RemoveItems();
	bool ChangeToDirectory(const wchar_t* Dir, OPERATION_MODES opmodes, bool IsExplicit);
	void ManualConnect();
	bool CancelConnection(const wchar_t* RemoteName);
	bool GetDriveToDisconnect(const wchar_t* RemoteName, wchar_t* LocalName);
	static bool ConfirmCancelConnection(wchar_t* LocalName, wchar_t* RemoteName, int& UpdateProfile);
	static bool NeedConfirmCancelConnection();
	static bool HandsOffDisconnectDrive(const wchar_t* LocalName);
	string GetLocalName(const std::wstring* RemoteName);
	static bool GetNameAndPassword(NameAndPassInfo* passInfo);
	static string GetRemoteName(NetResource* NetRes);
	void LoadNetList();
	bool FindInNetList(const wchar_t* Name, NetResource& Result);
	bool WorthWaitingFor(const wchar_t* Name) const;
	void RefreshPanel();
	void ApplyPendingCursor();
	void OnScanProgress();
	static void GetFreeLetter(DWORD& DriveMask, wchar_t* DiskName);
	static bool IsReadable(const string& Remote);
	static bool CancelConnectionNet(const string& LocalName, DWORD Flags, bool Force);
	bool GotoComputer(const wchar_t* Dir);
	bool SetCursorToShare(const wchar_t* Share);
	static bool MapNetworkDrive(const wchar_t* RemoteName, bool AskDrive, bool Permanent);
	static bool AskMapDrive(wchar_t* NewLocalName, bool& Permanent);
	void FileNames2Clipboard(bool ToCommandLine);

#ifdef NETWORK_LOGGING
	static FILE* LogFile;
	static int LogFileRef;
	static void LogNetResource(const NetResource& Res);
	static void OpenLogFile(const wchar_t* lpFileName);
	static void CloseLogfile();
	static void LogData(const wchar_t* Data);
#endif

public:
	NetBrowser();
	~NetBrowser();

	int GetFindData(PluginPanelItem** pPanelItem, size_t* pItemsNumber, OPERATION_MODES OpMode);
	static void FreeFindData(PluginPanelItem* PanelItem, size_t ItemsNumber);
	void GetOpenPanelInfo(OpenPanelInfo* Info);
	int SetDirectory(const wchar_t* Dir, OPERATION_MODES OpMode);
	int DeleteFiles(PluginPanelItem* PanelItem, size_t ItemsNumber, OPERATION_MODES OpMode);
	int ProcessKey(const INPUT_RECORD* Rec);
	int ProcessEvent(intptr_t Event, void* Param);
	bool SetOpenFromCommandLine(wchar_t* cmd);
	bool SetOpenFromFilePanel(wchar_t* ShareName);
	void GotoLocalNetwork();

	// ACTL_SYNCHRO arrived: a worker has something to show.
	static void OnSynchro();

	bool GotoFavorite(wchar_t* lpPath);
	bool EditFavorites();

	static bool AddConnection(const NetResource& nr, bool Remember = true);
	static bool AddConnectionExplicit(const NetResource* connectnr, bool Remember = true);
	static bool AddConnectionWithLogon(const NetResource* nr, wchar_t* Name, const wchar_t* Password, bool Remember = true);
	static bool AddConnectionFromFavorites(const NetResource* nr, bool Remember = true);

	static bool GetResourceInfo(wchar_t* SrcName, NetResource& DstNetResource);
	static bool GetResourceParent(const NetResource& SrcRes, NetResource* DstNetResource);
	static bool IsMSNetResource(const NetResource& Res);
	static bool IsResourceReadable(const NetResource& Res);
	//static BOOL GetDfsParent(const NETRESOURCE &SrcRes, NETRESOURCE &Parent);
};

extern NetResourceList* CommonRootResources;
extern bool SavedCommonRootResources;

#endif // __NETCLASS_HPP__
