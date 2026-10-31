#include <cstdlib>
#include <windows.h>
#include <lm.h>
#include "NetCommon.hpp"
#include "Network.hpp"
#include "NetCfg.hpp"
#include "NetFavorites.hpp"
#include "NetClass.hpp"
#include "NetLng.hpp"
#include "NetScan.hpp"
#include "guid.hpp"
#include <PluginSettings.hpp>
#include <DlgBuilder.hpp>
#include <algorithm>

NetResourceList* CommonRootResources;
bool SavedCommonRootResources = false;

// All open panels: a synchro event may arrive after a panel has gone.
static std::vector<NetBrowser*> Browsers;

// Dialogs and menus pump Far's messages, so a synchro event can arrive while we
// are in the middle of something. Hold the panel refreshes back until we're done.
// Not a member of NetBrowser: FCTL_CLOSEPANEL may delete it under our feet.
static int UiBusy = 0;
static bool UiRefreshDeferred = false;

namespace
{
	class UiBusyScope
	{
	public:
		UiBusyScope() { ++UiBusy; }
		~UiBusyScope()
		{
			if (!--UiBusy && UiRefreshDeferred)
			{
				UiRefreshDeferred = false;
				Scanner::RequestNotify();
			}
		}
	};
}

// -- NetResourceList --------------------------------------------------------
#ifdef NETWORK_LOGGING
FILE* NetBrowser::LogFile{};
int NetBrowser::LogFileRef = 0;

void NetBrowser::OpenLogFile(const wchar_t *lpFileName)
{
	if (!LogFileRef)
		LogFile = _wfopen(lpFileName, L"a+t");

	if (LogFile)
		fwprintf(LogFile, L"Opening plugin\n");

	LogFileRef++;
}

void NetBrowser::CloseLogfile()
{
	LogFileRef--;

	if (!LogFileRef && LogFile)fclose(LogFile),LogFile = {};
}

void NetBrowser::LogData(const wchar_t * Data)
{
	if (LogFile)
	{
		fwprintf(LogFile,L"%s\n", Data);
		wchar_t buffer[MAX_PATH];
		FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM, {}, GetLastError(), 0, buffer, ARRAYSIZE(buffer), {});
		fwprintf(LogFile,L"GetLastError returns: %s\n", buffer);
	}
}
#endif

void NetResourceList::Clear()
{
	ResList.clear();
}

void NetResourceList::Push(const NetResource& Res)
{
	ResList.emplace_back(Res);
}

NetResource* NetResourceList::Top()
{
	if (ResList.empty())
		return {};

	return &ResList.back();
}

void NetResourceList::Pop()
{
	if (!ResList.empty())
	{
		ResList.pop_back();
	}
}

// -- NetBrowser -------------------------------------------------------------

NetBrowser::NetBrowser()
{
	{
		PluginSettings settings(MainGuid, PsInfo.SettingsControl);
		settings.Get(0, StrPanelMode, m_PanelMode,ARRAYSIZE(m_PanelMode), L"3");
	}

	if (SavedCommonRootResources)
	{
		RootResources = *CommonRootResources;
		PCurResource = RootResources.Top();
	}
	else
	{
		CurResource = CommonCurResource;

		if (PCommonCurResource)
			PCurResource = &CurResource;
		else
			PCurResource = {};
	}

	Browsers.push_back(this);
#ifdef NETWORK_LOGGING
	OpenLogFile(L"c:\\network.log");
#endif
}


NetBrowser::~NetBrowser()
{
	Browsers.erase(std::remove(Browsers.begin(), Browsers.end(), this), Browsers.end());

	// Nobody is looking any more, stop bothering the network.
	if (Browsers.empty())
		Scanner::CancelAll();

#ifdef NETWORK_LOGGING
	CloseLogfile();
#endif
}


#ifdef NETWORK_LOGGING

void NetBrowser::LogNetResource(const NetResource& Res)
{
	if (LogFile)
	{
		fwprintf(LogFile, L"dwScope = %u\ndwType = %u\ndwDisplayType = %u\ndwUsage = %u\n", Res.dwScope, Res.dwType, Res.dwDisplayType, Res.dwUsage);
		fwprintf(LogFile, L"lpLocalName = %s\nlpRemoteName = %s\nlpComment = %s\nlpProvider = %s\n\n", Res.lpLocalName.c_str(), Res.lpRemoteName.c_str(), Res.lpComment.c_str(), Res.lpProvider.c_str());
	}
}

#endif

// Takes whatever is known about the current folder right now, which doesn't have
// to be everything: a background scan calls back when it has more.
void NetBrowser::LoadNetList()
{
	const auto Key = Scanner::MakeKey(PCurResource);
	const auto Navigated = !NetListLoaded || Key != NetListKey;

	Scanner::Request Req;
	Req.Key = Key;

	if (PCurResource)
	{
		Req.HasResource = true;
		Req.Resource = *PCurResource;
	}

	NetResourceList Initial;
	Req.IsFavorites = EnumFavorites(PCurResource, &Initial, &Req.FavoriteNames);
	Req.Initial = Initial.Items();

	if (Req.IsFavorites)
	{
		Req.NoNetwork = Req.FavoriteNames.empty();
	}
	else
	{
		Req.NoNetwork = !Opt.ScanNetwork && PCurResource && (PCurResource->dwDisplayType == RESOURCEDISPLAYTYPE_DOMAIN
			|| PCurResource->dwDisplayType == RESOURCEDISPLAYTYPE_NETWORK);

		if (Opt.HiddenShares)
		{
			PanelInfo PInfo = {sizeof(PanelInfo)};
			PsInfo.PanelControl(this, FCTL_GETPANELINFO, 0, &PInfo);
			Req.WantHiddenShares = !Opt.HiddenSharesAsHidden || (PInfo.Flags & PFLAGS_SHOWHIDDEN);
		}
	}

	auto View = Scanner::Acquire(Req, ForceRescan, Navigated);

	if (ForceRescan)
	{
		ForceRescan = false;
		Scanner::RefreshConnectedNow();
	}
	else
		Scanner::RefreshConnected();

	NetList.Assign(std::move(View.Items));
	NetListKey = Key;
	NetListLoaded = true;
	Scanning = View.Running;
	ScanError = View.Error;
	SeenVersion = View.Version;
	SeenConnectedVersion = Scanner::ConnectedVersion();
}

bool NetBrowser::FindInNetList(const wchar_t* Name, NetResource& Result)
{
	for (size_t I = 0; I < NetList.Count(); I++)
	{
		if (FSF.LStricmp(Name, GetRemoteName(&NetList[I]).c_str()) == 0)
		{
			// A copy: NetList can be replaced as soon as Far gets to run again.
			Result = NetList[I];
			return true;
		}
	}

	return false;
}

// If a name isn't listed yet it might just not have been found yet. Names that
// don't belong to this folder won't get there, however long we wait.
bool NetBrowser::WorthWaitingFor(const wchar_t* Name) const
{
	if (Name[0] != L'\\' || Name[1] != L'\\')
		return true;

	if (!PCurResource || PCurResource->lpRemoteName.empty())
		return false;

	const auto& Remote = PCurResource->lpRemoteName;
	return FSF.LStrnicmp(Name, Remote.c_str(), static_cast<int>(Remote.size())) == 0;
}

void NetBrowser::RefreshPanel()
{
	PsInfo.PanelControl(this, FCTL_UPDATEPANEL, 1, {});
	PsInfo.PanelControl(this, FCTL_REDRAWPANEL, 0, {});
}

void NetBrowser::ApplyPendingCursor()
{
	if (!PendingCursor.empty() && (SetCursorToShare(PendingCursor.c_str()) || !Scanning))
		PendingCursor.clear();
}

void NetBrowser::OnScanProgress()
{
	if (Scanner::Version(Scanner::MakeKey(PCurResource)) != SeenVersion || Scanner::ConnectedVersion() != SeenConnectedVersion)
	{
		PsInfo.PanelControl(this, FCTL_UPDATEPANEL, 1, {});

		if (PendingCursor.empty())
			PsInfo.PanelControl(this, FCTL_REDRAWPANEL, 0, {});
	}

	ApplyPendingCursor();
}

void NetBrowser::OnSynchro()
{
	Scanner::NotifyReceived();
	Scanner::Pump();

	if (UiBusy)
	{
		UiRefreshDeferred = true;
		return;
	}

	// Refreshing can run arbitrary Far code, don't trust the list to stay the same.
	const auto Snapshot = Browsers;

	for (auto* Browser: Snapshot)
	{
		if (std::find(Browsers.begin(), Browsers.end(), Browser) != Browsers.end())
			Browser->OnScanProgress();
	}
}

bool NetBrowser::GotoFavorite(wchar_t* lpPath)
{
#ifdef NEWTWORK_LOGGING
	LogData(L"Entered NetBrowser::GotoFavorite")
#endif
	NetResource nr;

	if (GetFavoriteResource(lpPath, &nr))
	{
#ifdef NETWORK_LOGGING
		LogData(L"GetFavoriteResource SUCCEEDED");
		LogNetResource(nr);
#endif
		CurResource = nr;
		PCurResource = &CurResource;
		PsInfo.PanelControl(this, FCTL_UPDATEPANEL, 0, {});
		PsInfo.PanelControl(this, FCTL_REDRAWPANEL, 0, {});
		return true;
	}

	return false;
}

int NetBrowser::GetFindData(PluginPanelItem** pPanelItem, size_t* pItemsNumber, OPERATION_MODES OpMode)
{
#ifdef NETWORK_LOGGING
	LogData(L"Entering NetBrowser::GetFindData");
#endif

	if (OpMode & OPM_FIND)
		return FALSE;

	*pPanelItem = {};
	*pItemsNumber = 0;

	if (ReenterGetFindData)
		return TRUE;

	ReenterGetFindData++;

	if (ChangeDirSuccess)
		LoadNetList();

	// The items may already be there, in which case no worker is going to wake us up.
	if (!PendingCursor.empty())
		Scanner::RequestNotify();

	ChangeDirSuccess = true;
	auto* NewPanelItem = (PluginPanelItem*)malloc(sizeof(PluginPanelItem) * std::max<size_t>(NetList.Count(), 1));
	*pPanelItem = NewPanelItem;

	if (!NewPanelItem)
	{
		ReenterGetFindData--;
		return FALSE;
	}

	int CurItemPos = 0;

	for (unsigned I = 0; I < NetList.Count(); I++)
	{
		if (NetList[I].dwType == RESOURCETYPE_PRINT && !Opt.ShowPrinters)
			continue;

		string Comment;
		string RemoteName = GetRemoteName(&NetList[I]);

		if (!NetList[I].lpComment.empty())
			Comment = NetList[I].lpComment;

		memset(&NewPanelItem[CurItemPos], 0, sizeof(PluginPanelItem));
		string LocalName = GetLocalName(&NetList[I].lpRemoteName);
		auto* CustomColumnData = (LPTSTR*)malloc(sizeof(LPTSTR) * 2);
		CustomColumnData[0] = _wcsdup(LocalName.c_str());
		CustomColumnData[1] = _wcsdup(Comment.c_str());
		NewPanelItem[CurItemPos].CustomColumnData = CustomColumnData;
		NewPanelItem[CurItemPos].CustomColumnNumber = 2;
		NewPanelItem[CurItemPos].FileName = _wcsdup(RemoteName.c_str());
		NewPanelItem[CurItemPos].Description = _wcsdup(Comment.c_str());
		DWORD attr = FILE_ATTRIBUTE_DIRECTORY;

		if (NetList[I].dwType == RESOURCETYPE_PRINT)
			attr = FILE_ATTRIBUTE_VIRTUAL;

		if (Opt.HiddenSharesAsHidden && !RemoteName.empty() && RemoteName.back() == L'$')
			attr |= FILE_ATTRIBUTE_HIDDEN;

		NewPanelItem[CurItemPos].FileAttributes = attr;
		CurItemPos++;
	}

	*pItemsNumber = CurItemPos;
	ReenterGetFindData--;
	return TRUE;
}

void NetBrowser::FreeFindData(PluginPanelItem* PanelItem, size_t ItemsNumber)
{
	for (size_t I = 0; I < ItemsNumber; I++)
	{
		free(const_cast<wchar_t*>(PanelItem[I].CustomColumnData[0]));
		free(const_cast<wchar_t*>(PanelItem[I].CustomColumnData[1]));
		free(const_cast<wchar_t**>(PanelItem[I].CustomColumnData));
		free(const_cast<wchar_t*>(PanelItem[I].FileName));
		free(const_cast<wchar_t*>(PanelItem[I].Description));
	}

	free(PanelItem);
}


int NetBrowser::ProcessEvent(intptr_t Event, void* Param)
{
	if (Event == FE_CLOSE)
	{
		{
			PanelInfo PInfo = {sizeof(PanelInfo)};
			PsInfo.PanelControl(this, FCTL_GETPANELINFO, 0, &PInfo);
			wchar_t Mode[2] = {static_cast<wchar_t>(PInfo.ViewMode + 0x30), 0};
			PluginSettings settings(MainGuid, PsInfo.SettingsControl);
			settings.Set(0, StrPanelMode, Mode);
		}

		if (!PCurResource || IsMSNetResource(*PCurResource))
		{
			CommonCurResource = CurResource;
			PCommonCurResource = PCurResource? &CommonCurResource : nullptr;
			SavedCommonRootResources = false;
		}
		else
		{
			*CommonRootResources = RootResources;
			SavedCommonRootResources = true;
		}
	}
	if (Event == FE_COMMAND)
	{
		if (SetOpenFromCommandLine(static_cast<wchar_t*>(Param)))
		{
			PsInfo.PanelControl(this, FCTL_SETCMDLINE, 0, const_cast<wchar_t*>(L""));
			PsInfo.PanelControl(this, FCTL_UPDATEPANEL, 0, nullptr);
			return TRUE;
		}
		return FALSE;
	}

	return FALSE;
}


int NetBrowser::DeleteFiles(PluginPanelItem* PanelItem, size_t ItemsNumber, OPERATION_MODES /*OpMode*/)
{
	if (ItemsNumber == 0)
		return TRUE;

	UiBusyScope Busy;

	if (CheckFavoriteItem(PCurResource))
	{
		//Deleting from favorites
		RemoveItems();
		Scanner::InvalidateFavorites();
	}
	else
	{
		Scanner::RefreshConnectedNow();

		// unmap disks if exists
		for (size_t I = 0; I < ItemsNumber; I++)
			if (PanelItem[I].CustomColumnNumber == 2 && PanelItem[I].CustomColumnData)
			{
				if (*PanelItem[I].CustomColumnData[0])
					if (!CancelConnection(PanelItem[I].FileName))
						break;
			}

		Scanner::RefreshConnectedNow();
	}

	return TRUE;
}

bool NetBrowser::CancelConnection(const wchar_t* RemoteName)
{
	wchar_t LocalName[MAX_PATH];
	wchar_t szFullName[MAX_PATH];
	szFullName[0] = 0;

	if (Opt.FullPathShares)
		lstrcpy(szFullName, RemoteName);
	else if (PCurResource && !PCurResource->lpRemoteName.empty())
		FSF.sprintf(szFullName, L"%s\\%s", PCurResource->lpRemoteName.c_str(), RemoteName);
	else
		return false;

	if (!GetDriveToDisconnect(szFullName, LocalName))
		return false;

	int UpdateProfile = 0;

	if (!ConfirmCancelConnection(LocalName, szFullName, UpdateProfile))
		return false;

	bool Disconnected = CancelConnectionNet(LocalName, UpdateProfile, false);
	auto LastError = GetLastError();

	if (!Disconnected && LastError == ERROR_CANCELLED)
		return false;

	// if we're on the drive we're disconnecting, set the directory to
	// a different drive and try again
	if (!Disconnected && HandsOffDisconnectDrive(LocalName))
	{
		Disconnected = CancelConnectionNet(LocalName, UpdateProfile, false);
		LastError = GetLastError();

		if (!Disconnected && LastError == ERROR_CANCELLED)
			return false;
	}

	if (!Disconnected)
	{
		bool Failed = false;
		wchar_t MsgText[200];
		FSF.sprintf(MsgText, GetMsg(MNetCannotDisconnect), LocalName);

		if (LastError == ERROR_OPEN_FILES || LastError == ERROR_DEVICE_IN_USE)
		{
			const wchar_t* MsgItems[] = {
				GetMsg(MError), MsgText, L"\x1", GetMsg(MOpenFiles), GetMsg(MAskDisconnect), GetMsg(MOk),
				GetMsg(MCancel)
			};

			if (PsInfo.Message(
				&MainGuid,
				nullptr,
				FMSG_WARNING | FMSG_ERRORTYPE,
				{},
				MsgItems,
				ARRAYSIZE(MsgItems),
				2) == 0)

				// всегда рвать соединение
				if (!CancelConnectionNet(LocalName, UpdateProfile, true))
					Failed = GetLastError() != ERROR_CANCELLED;
		}
		else
			Failed = true;

		if (Failed)
		{
			const wchar_t* MsgItems[] = {GetMsg(MError), MsgText, GetMsg(MOk)};
			PsInfo.Message(&MainGuid, nullptr, FMSG_WARNING | FMSG_ERRORTYPE, {}, MsgItems,ARRAYSIZE(MsgItems), 1);
			return false;
		}
	}

	return true;
}

bool NetBrowser::CancelConnectionNet(const string& LocalName, DWORD Flags, bool Force)
{
	return CallCancellable([LocalName, Flags, Force]{ return NetApi::CancelConnection(LocalName, Flags, Force); }, false);
}

bool NetBrowser::GetDriveToDisconnect(const wchar_t* RemoteName, wchar_t* LocalName)
{
	wchar_t LocalNames[MAX_PATH][10];
	DWORD LocalNameCount = 0;
	DWORD i;

	auto& ConnectedList = Scanner::Connected();

	for (i = 0; i < ConnectedList.Count(); i++)
	{
		auto& connRes = ConnectedList[i];

		if (!connRes.lpRemoteName.empty() &&
			!connRes.lpLocalName.empty() && lstrcmpi(connRes.lpRemoteName.c_str(), RemoteName) == 0)
		{
			if (connRes.dwScope == RESOURCE_CONNECTED ||
				connRes.dwScope == RESOURCE_REMEMBERED)
			{
				lstrcpy(LocalNames[LocalNameCount++], connRes.lpLocalName.c_str());

				if (LocalNameCount == 10)
					break;
			}
		}
	}

	if (!LocalNameCount)
		return false; // hmmm... strange

	if (LocalNameCount == 1)
		lstrcpy(LocalName, LocalNames[0]);
	else
	{
		wchar_t MsgText[512];
		FSF.sprintf(MsgText, GetMsg(MMultipleDisconnect), RemoteName);

		for (i = 0; i < LocalNameCount; i++)
		{
			lstrcat(MsgText, LocalNames[i]);
			lstrcat(MsgText, L"\n");
		}

		int index = (int)PsInfo.Message(
			&MainGuid,
			nullptr,
			FMSG_ALLINONE,
			{},
			(const wchar_t**)MsgText,
			3 + LocalNameCount,
			LocalNameCount);

		if (index < 0)
			return false;

		lstrcpy(LocalName, LocalNames[index]);
	}

	return true;
}

bool NetBrowser::ConfirmCancelConnection(wchar_t* LocalName, wchar_t* RemoteName, int& UpdateProfile)
{
	wchar_t MsgText[MAX_PATH];
	bool IsPersistent = true;
	// Check if this was a permanent connection or not.
	{
		HKEY hKey{};
		FSF.sprintf(MsgText, L"Network\\%c", FSF.LUpper(LocalName[0]));

		if (RegOpenKeyEx(HKEY_CURRENT_USER, MsgText, 0,KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS)
		{
			IsPersistent = false;
		}

		if (hKey)
			RegCloseKey(hKey);
	}
	FSF.sprintf(MsgText, GetMsg(MConfirmDisconnectQuestion), LocalName);
	/*
	wchar_t tmp[MAX_PATH];
	DialogItems[3].Data = tmp;
	{
	  size_t rc = lstrlen(DialogItems[0].Data);
	  if(Len1 < rc) Len1 = rc;
	  rc = lstrlen(DialogItems[5].Data);
	  if(Len1 < rc) Len1 = rc;
	}
	lstrcpy((wchar_t*)DialogItems[3].Data, FSF.TruncPathStr(RemoteName, (int)Len1));
	*/
	PluginDialogBuilder Builder(PsInfo, MainGuid, DisconnectDialogGuid, MConfirmDisconnectTitle, L"DisconnectDrive");
	Builder.AddText(MsgText);
	Builder.AddText(RemoteName);
	Builder.AddSeparator();
	Builder.AddCheckbox(MConfirmDisconnectReconnect, &Opt.DisconnectMode)->Flags |= IsPersistent? 0 : DIF_DISABLE;
	Builder.AddOKCancel(MYes, MCancel);

	if (!NeedConfirmCancelConnection() || Builder.ShowDialog())
	{
		UpdateProfile = Opt.DisconnectMode? 0 : CONNECT_UPDATE_PROFILE;

		if (IsPersistent)
		{
			PluginSettings settings(MainGuid, PsInfo.SettingsControl);
			settings.Set(0, StrDisconnectMode, Opt.DisconnectMode);
		}

		return true;
	}

	return false;
}


bool NetBrowser::NeedConfirmCancelConnection()
{
	return GetSetting(FSSF_CONFIRMATIONS, L"RemoveConnection") != 0;
}


bool NetBrowser::HandsOffDisconnectDrive(const wchar_t* LocalName)
{
	wchar_t DirBuf[MAX_PATH];
	GetCurrentDirectory(ARRAYSIZE(DirBuf) - 1, DirBuf);

	if (FSF.LUpper(DirBuf[0]) != FSF.LUpper(LocalName[0]))
		return false;

	// change to the root of the drive where network.dll resides
	if (!GetModuleFileName({}, DirBuf, ARRAYSIZE(DirBuf) - 1))
		return false;

	DirBuf[3] = L'\0'; // truncate to "X:\\"
	return SetCurrentDirectory(DirBuf);
}

void NetBrowser::GetOpenPanelInfo(OpenPanelInfo* Info)
{
#ifdef NETWORK_LOGGING__

	if (PCurResource)
		LogData(L"Entering NetBrowser::GetOpenPluginInfo. Info->Flags will contain OPIF_ADDDOTS");
	else
		LogData(L"Entering NetBrowser::GetOpenPluginInfo. Info->Flags will NOT contain OPIF_ADDDOTS");

#endif
	Info->StructSize = sizeof(*Info);
	Info->Flags = OPIF_ADDDOTS | OPIF_RAWSELECTION | OPIF_SHOWPRESERVECASE | OPIF_SHORTCUT;
	Info->HostFile = {};

	if (!PCurResource)
	{
		Info->CurDir = L"";

		if (!Opt.RootDoublePoint)
			Info->Flags &= ~OPIF_ADDDOTS;
	}
	else
	{
		if (PCurResource->lpRemoteName.empty())
		{
			if (CheckFavoriteItem(PCurResource))
				PanelCurDir = GetMsg(MFavorites);
			else
				PanelCurDir = PCurResource->lpProvider;
		}
		else
		{
			PanelCurDir = PCurResource->lpRemoteName;
		}

		Info->CurDir = PanelCurDir.c_str();
	}

	Info->Format = const_cast<wchar_t*>(GetMsg(MNetwork));
	PanelTitle = L" ";
	PanelTitle += GetMsg(MNetwork);
	PanelTitle += L": ";
	PanelTitle += Info->CurDir;

	if (Scanning)
	{
		PanelTitle += L" [";
		PanelTitle += GetMsg(MWaitForNetworkBrowse2);
		PanelTitle += L" Esc]";
	}
	else if (ScanError)
	{
		PanelTitle += L" [";
		PanelTitle += GetMsg(MNetCannotBrowse);
		PanelTitle += L"]";
	}

	PanelTitle += L" ";
	Info->PanelTitle = PanelTitle.c_str();
	Info->InfoLines = {};
	Info->InfoLinesNumber = 0;
	Info->DescrFiles = {};
	Info->DescrFilesNumber = 0;
	static PanelMode PanelModesArray[10];
	static const wchar_t* ColumnTitles[3];
	ColumnTitles[0] = GetMsg(MColumnName);
	ColumnTitles[1] = GetMsg(MColumnDisk);
	ColumnTitles[2] = GetMsg(MColumnComment);
	PanelModesArray[3].ColumnTypes = L"N,C0,C1";
	PanelModesArray[3].ColumnWidths = L"0,2,0";
	PanelModesArray[3].ColumnTitles = ColumnTitles;
	PanelModesArray[4].ColumnTypes = L"N,C0";
	PanelModesArray[4].ColumnWidths = L"0,2";
	PanelModesArray[4].ColumnTitles = ColumnTitles;
	PanelModesArray[5].ColumnTypes = L"N,C0,C1";
	PanelModesArray[5].ColumnWidths = L"0,2,0";
	PanelModesArray[5].ColumnTitles = ColumnTitles;
	Info->PanelModesArray = PanelModesArray;
	Info->PanelModesNumber = ARRAYSIZE(PanelModesArray);
	Info->StartPanelMode = m_PanelMode[0];

	if (PCurResource && PCurResource->dwDisplayType == RESOURCEDISPLAYTYPE_SERVER)
	{
		Info->Flags |= OPIF_REALNAMES;
		static WORD FKeys[] =
		{
			VK_F3, 0, 0,
			VK_F4, 0, MF4,
			VK_F5, 0, MF5,
			VK_F6, 0, MF6,
			VK_F7, 0, 0,
			VK_F8, 0, MF8,
			VK_F5,LEFT_CTRL_PRESSED, 0,
			VK_F6,LEFT_CTRL_PRESSED, 0,
			VK_F3,LEFT_ALT_PRESSED, 0,
			VK_F4,LEFT_ALT_PRESSED, 0,
			VK_F5,LEFT_ALT_PRESSED, 0,
			VK_F1,SHIFT_PRESSED, 0,
			VK_F2,SHIFT_PRESSED, 0,
			VK_F3,SHIFT_PRESSED, 0,
			VK_F4,SHIFT_PRESSED, 0,
			VK_F5,SHIFT_PRESSED, MSHIFTF5,
			VK_F6,SHIFT_PRESSED, MSHIFTF6,
			VK_F7,SHIFT_PRESSED, 0,
			VK_F8,SHIFT_PRESSED, 0,
		};
		static KeyBarLabel kbl[ARRAYSIZE(FKeys) / 3];
		static KeyBarTitles kbt = {ARRAYSIZE(kbl), kbl};

		for (size_t j = 0, i = 0; i < ARRAYSIZE(FKeys); i += 3, ++j)
		{
			kbl[j].Key.VirtualKeyCode = FKeys[i];
			kbl[j].Key.ControlKeyState = FKeys[i + 1];

			if (FKeys[i + 2])
			{
				kbl[j].Text = kbl[j].LongText = GetMsg(FKeys[i + 2]);
			}
			else
			{
				kbl[j].Text = kbl[j].LongText = L"";
			}
		}

		Info->KeyBar = &kbt;
	}
	else
	{
		static WORD FKeys[] =
		{
			VK_F3, 0, 0,
			VK_F4, 0, (WORD)-1,
			VK_F5, 0, 0,
			VK_F6, 0, 0,
			VK_F7, 0, 0,
			VK_F8, 0, (WORD)-1,
			VK_F5,LEFT_CTRL_PRESSED, 0,
			VK_F6,LEFT_CTRL_PRESSED, 0,
			VK_F3,LEFT_ALT_PRESSED, 0,
			VK_F4,LEFT_ALT_PRESSED, 0,
			VK_F5,LEFT_ALT_PRESSED, 0,
			VK_F6,LEFT_ALT_PRESSED, 0,
			VK_F1,SHIFT_PRESSED, 0,
			VK_F2,SHIFT_PRESSED, 0,
			VK_F3,SHIFT_PRESSED, 0,
			VK_F4,SHIFT_PRESSED, 0,
			VK_F5,SHIFT_PRESSED, 0,
			VK_F6,SHIFT_PRESSED, 0,
			VK_F7,SHIFT_PRESSED, 0,
			VK_F8,SHIFT_PRESSED, 0,
		};
		static KeyBarLabel kbl[ARRAYSIZE(FKeys) / 3];
		static KeyBarTitles kbt = {ARRAYSIZE(kbl), kbl};

		for (size_t j = 0, i = 0; i < ARRAYSIZE(FKeys); i += 3, ++j)
		{
			kbl[j].Key.VirtualKeyCode = FKeys[i];
			kbl[j].Key.ControlKeyState = FKeys[i + 1];

			switch (FKeys[i + 2])
			{
			case 0:
				kbl[j].Text = kbl[j].LongText = L"";
				break;

			case static_cast<WORD>(-1):
				switch (FKeys[i])
				{
				case VK_F4:
					kbl[j].Text = kbl[j].LongText = (PCurResource && PCurResource->dwDisplayType ==
						                                RESOURCEDISPLAYTYPE_DOMAIN)?
						                                GetMsg(MF4) :
						                                L"";
					break;

				case VK_F8:
					kbl[j].Text = kbl[j].LongText = CheckFavoriteItem(PCurResource)? GetMsg(MF8Fav) : L"";
					break;

				default:
					break;
				}
				break;

			default:
				kbl[j].Text = kbl[j].LongText = GetMsg(FKeys[i + 2]);
				break;
			}
		}
		Info->KeyBar = &kbt;
	}
}


int NetBrowser::SetDirectory(const wchar_t* Dir, OPERATION_MODES OpMode)
{
	if (OpMode & OPM_FIND)
		return TRUE;

	UiBusyScope Busy;

	ChangeDirSuccess = true;
	PendingCursor.clear();

	if (OpenFromFilePanel)
		PCurResource = {};

	bool TmpOpenFromFilePanel = OpenFromFilePanel;
	OpenFromFilePanel = false;

	if (!Dir || lstrcmp(Dir, L"\\") == 0)
	{
		PCurResource = {};
		RootResources.Clear();
		return TRUE;
	}

	if (lstrcmp(Dir, L"..") == 0)
	{
		if (!PCurResource)
			return FALSE;

		// Far looks for the folder we came from, but that may not be listed yet.
		PendingCursor = PCurResource->lpRemoteName.empty()? PCurResource->lpProvider : PCurResource->lpRemoteName;

		if (IsMSNetResource(*PCurResource))
		{
			NetResource nrParent;

			if (!GetResourceParent(*PCurResource, &nrParent))
			{
				if (GetLastError() == ERROR_CANCELLED)
				{
					PendingCursor.clear();
					return FALSE;
				}

				PCurResource = {};
			}
			else
			{
				CurResource = nrParent;
				PCurResource = &CurResource;
			}
		}
		else
		{
			RootResources.Pop();
			PCurResource = RootResources.Top();
		}

		return TRUE;
	}
	else
	{
		// BUGBUG already true
		ChangeDirSuccess = true;

		if (ChangeToDirectory(Dir, OpMode, false))
		{
			// ChangeToDirectory calls FCTL_CLOSEPANEL, which is equivalent to "delete this;"
			// We have to exit ASAP without touching *this.

			// TODO: Perhaps it's better to avoid such patterns if we can.
			return true;
		}

		if (GetLastError() == ERROR_CANCELLED)
			return FALSE;

		string AnsiDir{Dir};

		if (AnsiDir[0] == L'/')
			AnsiDir[0] = L'\\';

		if (AnsiDir[1] == L'/')
			AnsiDir[1] = L'\\';

		// if still haven't found and the name starts with \\, try to jump to a
		// computer in a different domain
		if (AnsiDir.compare(0, 2, L"\\\\") == 0)
		{
			if (!TmpOpenFromFilePanel && AnsiDir.find(L'\\', 2) != string::npos)
			{
				if (!IsReadable(AnsiDir))
				{
#ifdef NETWORK_LOGGING
					wchar_t szErrBuff[MAX_PATH*2];
					_snwprintf(szErrBuff, ARRAYSIZE(szErrBuff), L"GetLastError = %d at line %d, file %S", GetLastError(), __LINE__, __FILE__);
					LogData(szErrBuff);
#endif
					if (GetLastError() == ERROR_CANCELLED)
						return FALSE;

					PsInfo.Message(
						&MainGuid,
						nullptr,
						FMSG_WARNING | FMSG_ERRORTYPE | FMSG_MB_OK | FMSG_ALLINONE,
						{},
						reinterpret_cast<const wchar_t* const*>(GetMsg(MError)),
						0,
						0);
					return FALSE;
				}

				PsInfo.PanelControl(this, FCTL_CLOSEPANEL, 0, const_cast<wchar_t*>(Dir));
				return TRUE;
			}

			ChangeDirSuccess = GotoComputer(AnsiDir.c_str());
			return ChangeDirSuccess;
		}
	}

	return FALSE;
}


bool NetBrowser::ChangeToDirectory(const wchar_t* Dir, OPERATION_MODES opmodes, bool IsExplicit)
{
	bool IsFind = (opmodes & OPM_FIND) != 0;
	bool IsPgDn = (opmodes & OPM_PGDN) != 0;

	wchar_t AnsiDir[MAX_PATH];
	lstrcpyn(AnsiDir, Dir, ARRAYSIZE(AnsiDir));

	if (AnsiDir[0] == L'/')
		AnsiDir[0] = L'\\';

	if (AnsiDir[1] == L'/')
		AnsiDir[1] = L'\\';

	// if we already have the resource list for the current directory,
	// do not scan it again
	if (!NetListLoaded || NetListKey != Scanner::MakeKey(PCurResource))
		LoadNetList();

	NetResource Item;

	if (!FindInNetList(AnsiDir, Item))
	{
		if (!Scanning || !WorthWaitingFor(AnsiDir))
			return false;

		if (!Scanner::WaitForScan(Scanner::MakeKey(PCurResource)))
			return false; // ERROR_CANCELLED is set

		LoadNetList();

		if (!FindInNetList(AnsiDir, Item))
			return false;
	}

	if (CheckFavoriteItem(&Item))
	{
		Scanner::RememberParent(Item, PCurResource);
		CurResource = Item;
		PCurResource = &CurResource;
		return true;
	}

	const auto IsCancelled = []{ return GetLastError() == ERROR_CANCELLED; };

	if ((Item.dwUsage & RESOURCEUSAGE_CONTAINER) == 0 &&
		(Item.dwType & RESOURCETYPE_DISK) &&
		!Item.lpRemoteName.empty())
	{
		if (IsFind)
			return false;

		string NewDir;
		const string LocalName = GetLocalName(&Item.lpRemoteName);

		if (IsPgDn && !LocalName.empty())
			if (IsReadable(LocalName))
			{
				NewDir = LocalName;
			}
			else
			{
				if (!IsCancelled())
					PsInfo.Message(
						&MainGuid,
						nullptr,
						FMSG_WARNING | FMSG_ERRORTYPE | FMSG_MB_OK | FMSG_ALLINONE,
						{},
						reinterpret_cast<const wchar_t* const*>(GetMsg(MError)),
						0,
						0);
				return true;
			}
		else
		{
			bool ConnectError = false;
			NewDir = Item.lpRemoteName;

			if (IsExplicit)
			{
				if (!AddConnectionExplicit(&Item) || !IsReadable(NewDir))
					ConnectError = true;
			}
			else
			{
				if (!IsReadable(NewDir))
					if (IsCancelled() || !AddConnection(Item) || !IsReadable(NewDir))
						ConnectError = true;
			}

			if (ConnectError)
			{
				DWORD res = GetLastError();

				if (!IsExplicit)
					if (res == ERROR_INVALID_PASSWORD || res == ERROR_LOGON_FAILURE || res ==
						ERROR_ACCESS_DENIED || res == ERROR_INVALID_HANDLE)
						ConnectError = !((AddConnectionFromFavorites(&Item) ||
							AddConnectionExplicit(&Item)) && IsReadable(NewDir));

				if (ConnectError)
				{
					ChangeDirSuccess = false;

					if (!IsCancelled())
						PsInfo.Message(
							&MainGuid,
							nullptr,
							FMSG_WARNING | FMSG_ERRORTYPE | FMSG_MB_OK | FMSG_ALLINONE,
							{},
							reinterpret_cast<const wchar_t*const*>(GetMsg(MError)),
							0,
							0);

					return true;
				}
			}
		}

		PsInfo.PanelControl(this, FCTL_CLOSEPANEL, 0, const_cast<void*>(static_cast<const void*>(NewDir.c_str())));
		return true;
	}

	// We've seen what's in there a moment ago, no need to ask if we can look.
	if (IsExplicit || !Scanner::IsFresh(Scanner::MakeKey(&Item)))
	{
		if (IsExplicit? !AddConnectionExplicit(&Item) : !IsResourceReadable(Item))
		{
			auto res = GetLastError();

			if (res == ERROR_INVALID_PASSWORD || res == ERROR_LOGON_FAILURE || res == ERROR_ACCESS_DENIED || res ==
				ERROR_LOGON_TYPE_NOT_GRANTED)
				ChangeDirSuccess = IsExplicit?
					                   false :
					                   (AddConnectionFromFavorites(&Item) || AddConnectionExplicit(
						                   &Item));
			else
				ChangeDirSuccess = false;

			if (!ChangeDirSuccess)
			{
				if (!IsCancelled())
					PsInfo.Message(
						&MainGuid,
						nullptr,
						FMSG_WARNING | FMSG_ERRORTYPE | FMSG_MB_OK | FMSG_ALLINONE,
						{},
						reinterpret_cast<const wchar_t*const*>(GetMsg(MError)),
						0,
						0);

				return false;
			}
		}
	}

	if (IsMSNetResource(Item))
		Scanner::RememberParent(Item, PCurResource);

	CurResource = Item;
	PCurResource = &CurResource;

	if (!IsMSNetResource(CurResource))
	{
#ifdef NETWORK_LOGGING
		LogData(L"Resource is not MSN");
		LogNetResource(CurResource);
#endif
		RootResources.Push(CurResource);
	}

#ifdef NETWORK_LOGGING
	else
	{
		LogData(L"Resource is MSN");
		LogNetResource(CurResource);
	}

#endif
	return true;
}


bool NetBrowser::IsMSNetResource(const NetResource& Res)
{
	if (Res.lpProvider.empty())
		return true;

	return Res.lpProvider.find(L"Microsoft") != string::npos || CheckFavoriteItem(&Res);
}


bool NetBrowser::IsResourceReadable(const NetResource& Res)
{
	const bool ScanNetwork = Opt.ScanNetwork != 0;
	return CallCancellable([Res, ScanNetwork]{ return NetApi::IsResourceReadable(Res, ScanNetwork); }, false);
}

/*DELETING
BOOL NetBrowser::GetDfsParent(const NETRESOURCE &SrcRes, NETRESOURCE &Parent)
{
  if(!FNetDfsGetInfo)
    return FALSE;
  //we should allocate memory for Wide chars
  int nSize = MultiByteToWideChar(CP_ACP, 0, SrcRes.lpRemoteName, -1, {}, 0);
  if(!nSize)
    return FALSE;
  WCHAR *szRes = new WCHAR[nSize++];

  if(!szRes)
    return FALSE;

    int Res = FALSE;
    if(MultiByteToWideChar(CP_ACP, 0, SrcRes.lpRemoteName, -1, szRes, nSize*sizeof(WCHAR)))
    {
    LPDFS_INFO_3 lpData;
    if(ERROR_SUCCESS == FNetDfsGetInfo(szRes, {}, {}, 3, (LPBYTE *) &lpData))
    {
      DWORD dwBuffSize = 32*sizeof(NETRESOURCE);
      NETRESOURCE *resResult = (NETRESOURCE *)malloc(dwBuffSize);
      CHAR *pszSys{};
      for(DWORD i = 0; i < lpData->NumberOfStorages; i++)
      {
        nSize = WideCharToMultiByte(CP_ACP, 0, lpData->Storage[i].ServerName,
          -1, {}, 0, {}, {});
        if(!nSize)
          break;
        nSize += 3;
        CHAR *szServ =new CHAR[nSize];
        szServ[0] = L'\\';
        szServ[1] = L'\\';

        WideCharToMultiByte(CP_ACP, 0, lpData->Storage[i].ServerName,
          -1, &szServ[2], nSize - 2, {}, {});
        NETRESOURCE inRes = {0};
        inRes.dwScope = RESOURCE_CONNECTED;
        inRes.dwType = RESOURCETYPE_ANY;
        inRes.lpRemoteName = szServ;
        DWORD dwRes;
        while((dwRes = WNetGetResourceInformation(&inRes, resResult,
          &dwBuffSize, &pszSys)) == ERROR_MORE_DATA)
        {
          resResult = (NETRESOURCE *)realloc(resResult, dwBuffSize);
        }
        if(dwRes == ERROR_SUCCESS)
        {
          if(IsResourceReadable(*resResult))
          {
            NetResourceList::CopyNetResource(Parent, *resResult);
            Res = TRUE;
          }
        }

        delete(szServ);
        if(Res)
          break;
      }
      free(resResult);
    }
  }

  delete (szRes);
  return Res;
}
*/


bool NetBrowser::GetResourceInfo(wchar_t* SrcName, NetResource& DstNetResource)
{
#ifdef NETWORK_LOGGING

	if (LogFile)
		fwprintf(LogFile, L"GetResourceInfo %s\n", SrcName);

#endif
	struct Result
	{
		bool Succeeded;
		NetResource Res;
	};

	const string Name = SrcName;

	auto Info = CallCancellable([Name]
	{
		Result Value{};
		Value.Succeeded = NetApi::GetResourceInfo(Name, Value.Res);
		return Value;
	}, Result{});

	if (!Info.Succeeded)
		return false;

	const auto Error = GetLastError();
	DstNetResource = std::move(Info.Res);
	SetLastError(Error);
	return true;
}


bool NetBrowser::GetResourceParent(const NetResource& SrcRes, NetResource* DstNetResource)
{
	SetLastError(0);

	if (CheckFavoriteItem(&SrcRes) ||
		Opt.FavoritesFlags & FAVORITES_UPBROWSE_TO_FAVORITES)
	{
		if (GetFavoritesParent(SrcRes, DstNetResource))
			return true;
	}

	// We have probably just come from there.
	bool IsRoot;
	NetResource Parent;

	if (Scanner::FindParent(SrcRes, IsRoot, Parent))
	{
		if (IsRoot)
			return false;

		if (DstNetResource)
			*DstNetResource = std::move(Parent);

		return true;
	}

	// We can work these out by ourselves. Asking would mean waiting for the legacy browser,
	// which on most networks is no longer there to answer.
	if (SrcRes.lpProvider.find(L"Microsoft Windows") != string::npos)
	{
		if (SrcRes.dwDisplayType == RESOURCEDISPLAYTYPE_NETWORK)
			return false;

		if (SrcRes.dwDisplayType == RESOURCEDISPLAYTYPE_DOMAIN)
		{
			if (DstNetResource)
			{
				*DstNetResource = {};
				DstNetResource->dwScope = RESOURCE_GLOBALNET;
				DstNetResource->dwType = RESOURCETYPE_ANY;
				DstNetResource->dwDisplayType = RESOURCEDISPLAYTYPE_NETWORK;
				DstNetResource->dwUsage = RESOURCEUSAGE_CONTAINER;
				DstNetResource->lpRemoteName = SrcRes.lpProvider;
				DstNetResource->lpProvider = SrcRes.lpProvider;
			}

			return true;
		}
	}

	// A server we got to directly, by name. Most likely it's in our own network, and
	// finding out for sure can take a minute.
	if (SrcRes.dwDisplayType == RESOURCEDISPLAYTYPE_SERVER && Scanner::FindLocalNetwork(Parent))
	{
		if (DstNetResource)
			*DstNetResource = std::move(Parent);

		return true;
	}

	if (!Opt.ScanNetwork)
		return false;

#ifdef NETWORK_LOGGING

	if (LogFile)
		fwprintf(LogFile, L"GetResourceParent( for:\n");

	LogNetResource(SrcRes);
#endif
	struct Result
	{
		bool Succeeded;
		NetResource Res;
	};

	auto Found = CallCancellable([SrcRes]
	{
		Result Value{};
		Value.Succeeded = NetApi::GetResourceParent(SrcRes, Value.Res);
		return Value;
	}, Result{});

	if (!Found.Succeeded)
		return false;

	Scanner::RememberParent(SrcRes, &Found.Res);

	if (DstNetResource)
		*DstNetResource = std::move(Found.Res);

	return true;
}

bool NetBrowser::EditFavorites()
{
	if (!PCurResource)
		return true;

	// First we should determine the type of Favorite Item under cursor
	string Path;

	PanelInfo PInfo = {sizeof(PanelInfo)};
	PsInfo.PanelControl(this, FCTL_GETPANELINFO, 0, &PInfo);

	size_t Size = PsInfo.PanelControl(this, FCTL_GETPANELDIRECTORY, 0, nullptr);
	auto* dir = static_cast<FarPanelDirectory*>(malloc(Size));
	dir->StructSize = sizeof(FarPanelDirectory);
	PsInfo.PanelControl(this, FCTL_GETPANELDIRECTORY, static_cast<intptr_t>(Size), dir);
	Path = dir->Name;
	free(dir);
	Size = PsInfo.PanelControl(this, FCTL_GETPANELITEM, static_cast<intptr_t>(PInfo.CurrentItem), {});
	auto* PPI = (PluginPanelItem*)malloc(Size);

	if (PPI)
	{
		FarGetPluginPanelItem gpi = {sizeof(FarGetPluginPanelItem), Size, PPI};
		PsInfo.PanelControl(this, FCTL_GETPANELITEM, static_cast<intptr_t>(PInfo.CurrentItem), &gpi);
		Path += PPI->FileName;
		free(PPI);
	}

	NetResource nr;

	if (GetFavoriteResource(Path.c_str(), &nr))
	{
		Path = nr.lpRemoteName;
		string message{L"Info\n"};

		switch (nr.dwDisplayType)
		{
		case RESOURCEDISPLAYTYPE_DOMAIN:
			message.append(L"This is a domain");
			break;
		case RESOURCEDISPLAYTYPE_SERVER:
			message.append(L"This is a SERVER");
			break;
		default:
			message.append(Path);
		}
		PsInfo.Message(
			&MainGuid,
			nullptr,
			FMSG_ALLINONE,
			L"Data",
			reinterpret_cast<const wchar_t* const *>(message.c_str()),
			0,
			1);

		return true;
	}

	return false;
}


int NetBrowser::ProcessKey(const INPUT_RECORD* Rec)
{
	if ((Rec->EventType & (~0x8000)) != KEY_EVENT || !Rec->Event.KeyEvent.bKeyDown)
		return FALSE;

	int Key = Rec->Event.KeyEvent.wVirtualKeyCode;
	int Shift = Rec->Event.KeyEvent.dwControlKeyState & SHIFT_PRESSED;
	int Ctrl = Rec->Event.KeyEvent.dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED);
	int Alt = Rec->Event.KeyEvent.dwControlKeyState & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED);

	UiBusyScope Busy;

	// The user has taken over, don't move the cursor around any more.
	PendingCursor.clear();

	if (Key == VK_ESCAPE && !Ctrl && !Alt && !Shift)
	{
		// Esc still clears the command line first.
		if (PsInfo.PanelControl(this, FCTL_GETCMDLINE, 0, nullptr) <= 1 && Scanner::Cancel(Scanner::MakeKey(PCurResource)))
		{
			LoadNetList();
			RefreshPanel();
			return TRUE;
		}

		return FALSE;
	}

	if (Key == L'R' && Ctrl && !Alt && !Shift)
	{
		// Far is about to call GetFindData
		ForceRescan = true;
		return FALSE;
	}

	if (!Ctrl && !Alt && (Key == VK_F5 || Key == VK_F6))
	{
		if (PCurResource && PCurResource->dwDisplayType == RESOURCEDISPLAYTYPE_SERVER)
		{
			PanelInfo PInfo = {sizeof(PanelInfo)};
			PsInfo.PanelControl(this, FCTL_GETPANELINFO, 0, &PInfo);

			for (size_t I = 0; I < PInfo.SelectedItemsNumber; ++I)
			{
				const wchar_t* pRemoteName{};
				size_t Size = PsInfo.PanelControl(this, FCTL_GETSELECTEDPANELITEM, static_cast<intptr_t>(I), {});
				auto* PPI = (PluginPanelItem*)malloc(Size);

				if (PPI)
				{
					FarGetPluginPanelItem gpi = {sizeof(FarGetPluginPanelItem), Size, PPI};
					PsInfo.PanelControl(this, FCTL_GETSELECTEDPANELITEM, static_cast<intptr_t>(I), &gpi);
					pRemoteName = PPI->FileName;
					if (!Opt.FullPathShares)
					{
						for (unsigned J = 0; J < NetList.Count(); ++J)
						{
							string RemoteName = GetRemoteName(&NetList[J]);

							if (FSF.LStricmp(PPI->FileName, RemoteName.c_str()) == 0)
							{
								pRemoteName = NetList[J].lpRemoteName.c_str();
								break;
							}
						}
					}
				}

				if (!PPI || !MapNetworkDrive(pRemoteName, (Key == VK_F6), (Shift == 0)))
				{
					free(PPI);
					break;
				}

				free(PPI);
			}

			Scanner::RefreshConnectedNow();
			PsInfo.PanelControl(this, FCTL_UPDATEPANEL, 0, {});
			PsInfo.PanelControl(this, FCTL_REDRAWPANEL, 0, {});
		}

		return TRUE;
	}
	else if (Key == L'F' && Ctrl)
	{
		FileNames2Clipboard(true);
		return TRUE;
	}
	else if (Key == VK_INSERT && Alt && Shift)
	{
		FileNames2Clipboard(false);
		return TRUE;
	}
	else if (Key == VK_INSERT && Alt && Ctrl)
	{
		FileNames2Clipboard(false);
		return TRUE;
	}
	else if (Key == VK_F4 && !Alt && !Ctrl && !Shift)
	{
		PanelInfo PInfo = {sizeof(PanelInfo)};
		PsInfo.PanelControl(this, FCTL_GETPANELINFO, 0, &PInfo);
		size_t Size = PsInfo.PanelControl(this, FCTL_GETSELECTEDPANELITEM, 0, {});
		auto* PPI = (PluginPanelItem*)malloc(Size);

		if (PPI)
		{
			FarGetPluginPanelItem gpi = {sizeof(FarGetPluginPanelItem), Size, PPI};
			PsInfo.PanelControl(this, FCTL_GETSELECTEDPANELITEM, 0, &gpi);
		}

		if (PPI && lstrcmp(PPI->FileName, L".."))
			if (ChangeToDirectory(PPI->FileName, OPM_NONE, true))
				if (FSF.PointToName(PPI->FileName) -
					PPI->FileName <= 2)
				{
					PsInfo.PanelControl(this, FCTL_UPDATEPANEL, 1, {});
					PanelRedrawInfo ri = {sizeof(PanelRedrawInfo)};
					ri.CurrentItem = ri.TopPanelItem = 0;
					PsInfo.PanelControl(this, FCTL_REDRAWPANEL, 0, &ri);
				}

		free(PPI);
		return TRUE;
	}
	else if (Key == VK_F4 && Shift)
	{
		EditFavorites();
		return TRUE;
	}
		// disable processing of F3 - avoid unnecessary slowdown
	else if ((Key == VK_F3 || Key == VK_CLEAR) && !Ctrl && !Shift)
	{
		return TRUE;
	}
	else if ((Key == VK_PRIOR || Key == 0xDC) && Ctrl && !PCurResource && !Opt.RootDoublePoint)
	{
		return TRUE;
	}

	return FALSE;
}


bool NetBrowser::MapNetworkDrive(const wchar_t* RemoteName, bool AskDrive, bool Permanent)
{
	DWORD DriveMask = GetLogicalDrives();
	wchar_t NewLocalName[10];
	*NewLocalName = 0;

	if (!AskDrive)
		GetFreeLetter(DriveMask, NewLocalName);
	else
	{
		if (!AskMapDrive(NewLocalName, Permanent))
			return false;
	}

	if (*NewLocalName)
	{
		string AnsiRemoteName{RemoteName};
		NetResource newnr;
		// wchar_t LocalName[10];
		newnr.dwType = RESOURCETYPE_DISK;
		newnr.lpLocalName = NewLocalName;
		newnr.lpRemoteName = AnsiRemoteName;

		for (;;)
		{
			if (IsReadable(AnsiRemoteName))
			{
				if (AddConnection(newnr, Permanent))
					break;
			}
			else if (ERROR_CANCELLED == GetLastError())
				break;
			else if ((AddConnectionFromFavorites(&newnr, Permanent) || AddConnectionExplicit(&newnr, Permanent)) &&
				IsReadable(newnr.lpLocalName))
				break;
			else if (ERROR_CANCELLED == GetLastError())
				break;

			if (ERROR_CANCELLED == GetLastError())
				break;

			if (GetLastError() == ERROR_DEVICE_ALREADY_REMEMBERED)
			{
				if (!AskDrive)
				{
					GetFreeLetter(DriveMask, NewLocalName);

					if (*NewLocalName == 0)
					{
						const wchar_t* MsgItems[] = {GetMsg(MError), GetMsg(MNoFreeLetters), GetMsg(MOk)};
						PsInfo.Message(
							&MainGuid,
							nullptr,
							FMSG_WARNING | FMSG_ERRORTYPE,
							{},
							MsgItems,
							ARRAYSIZE(MsgItems),
							1);
						return false;
					}
				}
				else
				{
					const wchar_t* MsgItems[] = {GetMsg(MError), GetMsg(MAlreadyRemembered), GetMsg(MOk)};
					PsInfo.Message(
						&MainGuid,
						nullptr,
						FMSG_WARNING | FMSG_ERRORTYPE,
						{},
						MsgItems,
						ARRAYSIZE(MsgItems),
						1);
					return false;
				}
			}
			else
			{
				wchar_t MsgText[300];
				FSF.sprintf(MsgText, GetMsg(MNetCannotConnect), RemoteName, NewLocalName);
				const wchar_t* MsgItems[] = {GetMsg(MError), MsgText, GetMsg(MOk)};
				PsInfo.Message(&MainGuid, nullptr, FMSG_WARNING | FMSG_ERRORTYPE, {}, MsgItems,ARRAYSIZE(MsgItems), 1);
				return false;
			}
		}
	}
	else
	{
		const wchar_t* MsgItems[] = {GetMsg(MError), GetMsg(MNoFreeLetters), GetMsg(MOk)};
		PsInfo.Message(&MainGuid, nullptr, FMSG_WARNING | FMSG_ERRORTYPE, {}, MsgItems,ARRAYSIZE(MsgItems), 1);
		return false;
	}

	return true;
}


bool NetBrowser::AskMapDrive(wchar_t* NewLocalName, bool& Permanent)
{
	int ExitCode = 0;

	for (;;)
	{
		FarMenuItem MenuItems['Z' - 'A' + 1];
		int MenuItemsNumber = 0;
		memset(MenuItems, 0, sizeof(MenuItems));
		wchar_t umname[ARRAYSIZE(MenuItems)][4];
		memset(umname, 0, sizeof(umname));

		for (size_t n = 0; n < ARRAYSIZE(MenuItems); n++)
			MenuItems[n].Text = umname[n];

		DWORD DriveMask = GetLogicalDrives();

		for (int I = 0; I <= 'Z' - 'A'; I++)
			if ((DriveMask & (1 << I)) == 0)
				FSF.sprintf(const_cast<wchar_t*>(MenuItems[MenuItemsNumber++].Text), L"&%c:", L'A' + I);

		MenuItems[ExitCode].Flags = MIF_SELECTED;

		if (!MenuItemsNumber)
			return false;

		const wchar_t* MenuTitle,* MenuBottom;

		if (Permanent)
		{
			MenuTitle = GetMsg(MPermanentTo);
			MenuBottom = GetMsg(MToggleTemporary);
		}
		else
		{
			MenuTitle = GetMsg(MTemporaryTo);
			MenuBottom = GetMsg(MTogglePermanent);
		}

		FarKey BreakKeys[] = {{VK_F6, 0}, {0, 0}};
		intptr_t BreakCode;
		ExitCode = (int)PsInfo.Menu(
			&MainGuid,
			nullptr,
			-1,
			-1,
			0,
			0,
			MenuTitle,
			MenuBottom,
			StrHelpNetBrowse,
			BreakKeys,
			&BreakCode,
			MenuItems,
			MenuItemsNumber);

		if (ExitCode < 0)
			return false;

		if (BreakCode == -1)
		{
			lstrcpy(NewLocalName, MenuItems[ExitCode].Text + 1);
			break;
		}

		Permanent = !Permanent;
	}

	return true;
}


void NetBrowser::GetFreeLetter(DWORD& DriveMask, wchar_t* DiskName)
{
	*DiskName = 0;

	for (wchar_t I = 2; I <= 'Z' - 'A'; I++)
		if ((DriveMask & (1 << I)) == 0)
		{
			DriveMask |= 1 << I;
			DiskName[0] = L'A' + I;
			DiskName[1] = L':';
			DiskName[2] = 0;
			break;
		}
}


bool NetBrowser::AddConnection(const NetResource& nr, bool Remember)
{
	return CallCancellable([nr, Remember]{ return NetApi::AddConnection(nr, Remember); }, false);
}

bool NetBrowser::AddConnectionExplicit(const NetResource* connectnr, bool Remember)
{
	wchar_t Name[256], Password[256];
	/*static*/
	BOOL bSelected = FALSE;
	NameAndPassInfo passInfo = {const_cast<wchar_t*>(connectnr->lpRemoteName.data()), Name, Password, &bSelected};

	if (!GetNameAndPassword(&passInfo))
	{
		SetLastError(ERROR_CANCELLED);
		return false;
	}

	if (AddConnectionWithLogon(connectnr, Name, Password, Remember))
	{
		if (bSelected)
		{
			FAVORITEITEM Item{};
			Item.lpRemoteName = connectnr->lpRemoteName.c_str();
			Item.lpUserName = Name;
			Item.lpPassword = Password;
			WriteFavoriteItem(&Item);
			Scanner::InvalidateFavorites();
		}

		return true;
	}

	return false;
}

bool NetBrowser::AddConnectionWithLogon(const NetResource* nr, wchar_t* Name, const wchar_t* Password, bool Remember)
{
	struct Result
	{
		bool Succeeded;
		bool CredentialConflict;
		string Name;
	};

	const NetResource Res = *nr;
	const string UserName = Name;
	const string UserPassword = Password;

	const auto Logon = CallCancellable([Res, UserName, UserPassword, Remember]
	{
		Result Value{};
		Value.Name = UserName;
		Value.Succeeded = NetApi::AddConnectionWithLogon(Res, Value.Name, UserPassword, Remember, Value.CredentialConflict);
		return Value;
	}, Result{});

	const auto Error = GetLastError();

	if (Logon.CredentialConflict)
	{
		//Let's check the current dir and if it's remote try to change it to %TMP%
		wchar_t lpszPath[MAX_PATH];

		if (GetCurrentDirectory(MAX_PATH, lpszPath) && lpszPath[0] == L'\\')
		{
			ExpandEnvironmentStrings(L"%TMP%", lpszPath, MAX_PATH);
			SetCurrentDirectory(lpszPath);
		}
	}

	// The worker might have turned it into "Computer\User"
	if (Logon.Succeeded)
		lstrcpyn(Name, Logon.Name.c_str(), 256);

	SetLastError(Error);
	return Logon.Succeeded;
}

bool NetBrowser::AddConnectionFromFavorites(const NetResource* nr, bool Remember)
{
	//Try to search login info in registry
	if (nr)
	{
		wchar_t Name[MAX_PATH];
		wchar_t Pass[MAX_PATH];
		Name[0] = Pass[0] = 0;
		FAVORITEITEM Item =
		{
			nr->lpRemoteName.c_str(),
			Name,
			Pass
		};

		if (ReadFavoriteItem(&Item))
		{
			return AddConnectionWithLogon(nr, Name, Pass, Remember);
		}
	}

	return false;
}

string NetBrowser::GetLocalName(const string* RemoteName)
{
	string LocalName;
	if (RemoteName && !RemoteName->empty())
	{
		auto& ConnectedList = Scanner::Connected();

		for (size_t i = 0; i < ConnectedList.Count(); i++)
		{
			if (!ConnectedList[i].lpRemoteName.empty() && !ConnectedList[i].lpLocalName.empty() &&
				ConnectedList[i].lpRemoteName == *RemoteName)
			{
				if (ConnectedList[i].dwScope == RESOURCE_CONNECTED ||
					ConnectedList[i].dwScope == RESOURCE_REMEMBERED)
					LocalName = ConnectedList[i].lpLocalName;

				break;
			}
		}
	}
	return LocalName;
}

bool NetBrowser::GetNameAndPassword(NameAndPassInfo* passInfo)
{
	static wchar_t LastName[256], LastPassword[256];
	PluginDialogBuilder Builder(
		PsInfo,
		MainGuid,
		UserPassDialogGuid,
		passInfo->Title? passInfo->Title : L"",
		StrHelpNetBrowse);
	Builder.AddText(MNetUserName);
	Builder.AddEditField(LastName, ARRAYSIZE(LastName), 60, L"NetworkUser", true);
	Builder.AddText(MNetUserPassword);
	Builder.AddPasswordField(LastPassword, ARRAYSIZE(LastPassword), 60);
	Builder.AddSeparator();
	int disabled = 0;

	if (passInfo->pRemember)
		Builder.AddCheckbox(MRememberPass, passInfo->pRemember);
	else
		Builder.AddCheckbox(MRememberPass, &disabled)->Flags |= DIF_DISABLE;

	Builder.AddOKCancel(MOk, MCancel);

	if (Builder.ShowDialog())
	{
		lstrcpy(passInfo->Name, LastName);
		lstrcpy(passInfo->Password, LastPassword);
		return true;
	}

	return false;
}


void NetBrowser::FileNames2Clipboard(bool ToCommandLine)
{
	PanelInfo PInfo = {sizeof(PanelInfo)};
	PsInfo.PanelControl(this, FCTL_GETPANELINFO, 0, &PInfo);

	wchar_t* CopyData = nullptr;
	long DataSize = 0;

	if (ToCommandLine)
	{
		if (PInfo.ItemsNumber > 0)
		{
			wchar_t CurFile[MAX_PATH]{};
			size_t Size = PsInfo.PanelControl(this, FCTL_GETPANELITEM, static_cast<intptr_t>(PInfo.CurrentItem), {});
			auto* PPI = (PluginPanelItem*)malloc(Size);

			if (PPI)
			{
				FarGetPluginPanelItem gpi = {sizeof(FarGetPluginPanelItem), Size, PPI};
				PsInfo.PanelControl(this, FCTL_GETPANELITEM, static_cast<intptr_t>(PInfo.CurrentItem), &gpi);
				lstrcpy(CurFile, PPI->FileName);
				free(PPI);
			}

			if (!lstrcmp(CurFile, L".."))
			{
				if (!PCurResource)
					lstrcpy(CurFile, L".\\");
				else
					lstrcpy(CurFile, PCurResource->lpRemoteName.c_str());
			}

			FSF.QuoteSpaceOnly(CurFile);

			lstrcat(CurFile, L" ");
			PsInfo.PanelControl(this, FCTL_INSERTCMDLINE, 0, CurFile);
		}

		return;
	}

	for (size_t I = 0; I < PInfo.SelectedItemsNumber; ++I)
	{
		if (DataSize > 0)
		{
			lstrcat(CopyData + DataSize, L"\r\n");
			DataSize += 2;
		}

		size_t Size = PsInfo.PanelControl(this, FCTL_GETSELECTEDPANELITEM, static_cast<intptr_t>(I), {});
		auto* PPI = (PluginPanelItem*)malloc(Size);

		if (PPI)
		{
			FarGetPluginPanelItem gpi = {sizeof(FarGetPluginPanelItem), Size, PPI};
			PsInfo.PanelControl(this, FCTL_GETSELECTEDPANELITEM, static_cast<intptr_t>(I), &gpi);

			wchar_t CurFile[MAX_PATH];
			lstrcpy(CurFile, PPI->FileName);
			if (!lstrcmp(CurFile, L".."))
			{
				if (!PCurResource)
					lstrcpy(CurFile, L".\\");
				else
					lstrcpy(CurFile, PCurResource->lpRemoteName.c_str());
			}
			FSF.QuoteSpaceOnly(CurFile);
			int Length = lstrlen(CurFile);

			auto* NewPtr = (wchar_t*)realloc(CopyData, (DataSize + Length + 3) * sizeof(wchar_t));
			if (!NewPtr)
			{
				if (CopyData)
				{
					free(CopyData);
					CopyData = nullptr;
				}
			}
			else
			{
				CopyData = NewPtr;
				CopyData[DataSize] = 0;
				lstrcpy(CopyData + DataSize, CurFile);
				DataSize += Length;
			}
			free(PPI);

			if (!CopyData)
				break;
		}
	}

	if (CopyData)
	{
		FSF.CopyToClipboard(FCT_STREAM, CopyData);
		free(CopyData);
	}
}


void NetBrowser::ManualConnect()
{
	PanelInfo PInfo = {sizeof(PanelInfo)};
	PsInfo.PanelControl(this, FCTL_GETPANELINFO, 0, &PInfo);

	if (PInfo.ItemsNumber)
	{
		size_t Size = PsInfo.PanelControl(this, FCTL_GETPANELITEM, static_cast<intptr_t>(PInfo.CurrentItem), {});
		auto* PPI = (PluginPanelItem*)malloc(Size);

		if (PPI)
		{
			FarGetPluginPanelItem gpi = {sizeof(FarGetPluginPanelItem), Size, PPI};
			PsInfo.PanelControl(this, FCTL_GETPANELITEM, static_cast<intptr_t>(PInfo.CurrentItem), &gpi);
			ChangeToDirectory(PPI->FileName, OPM_NONE, true);
			free(PPI);
		}
	}
}


string NetBrowser::GetRemoteName(NetResource* NetRes)
{
	string RemoteName;
	if (CheckFavoriteItem(NetRes))
	{
		if (NetRes->lpRemoteName.empty())
		{
			RemoteName = GetMsg(MFavorites);
		}
		else
		{
			NetRes->lpComment = GetMsg(MFavoritesFolder);
			RemoteName = FSF.PointToName(NetRes->lpRemoteName.c_str());
		}
	}
	else if (!NetRes->lpProvider.empty() && (NetRes->lpRemoteName.empty() || NetRes->dwDisplayType ==
		RESOURCEDISPLAYTYPE_NETWORK))
	{
		RemoteName = NetRes->lpProvider;
	}
	else if (NetRes->lpRemoteName.empty())
	{
		//empty string
	}
	else if (Opt.FullPathShares)
	{
		RemoteName = NetRes->lpRemoteName;
	}
	else
	{
		RemoteName = FSF.PointToName(NetRes->lpRemoteName.c_str());
	}

	return RemoteName;
}


bool NetBrowser::IsReadable(const string& Remote)
{
	return CallCancellable([Remote]{ return NetApi::IsReadable(Remote); }, false);
}

bool NetBrowser::SetOpenFromCommandLine(wchar_t* cmd)
{
#ifdef NETWORK_LOGGING
	LogData(L"SetOpenFromCommandLine cmd is");
	LogData(cmd);
#endif
	int I = 0;
	wchar_t* p = wcschr(cmd, L':');

	if (!p || !*p)
	{
		return false;
	}

	*p++ = L'\0';
	bool netg;

	if (!lstrcmpi(cmd, L"netg"))
		netg = true;
	else if (!lstrcmpi(cmd, L"net"))
		netg = false;
	else
	{
		return false;
	}

	cmd = p;

	if (lstrlen(FSF.Trim(cmd)))
	{
		if (cmd[0] == L'/')
			cmd[0] = L'\\';

		if (cmd[1] == L'/')
			cmd[1] = L'\\';

		if (!netg && !Opt.NavigateToDomains)
		{
			if (cmd[0] == L'\\' && cmd[1] != L'\\')
				I = 1;
			else if (cmd[0] != L'\\' && cmd[1] != L'\\')
				I = 2;
		}

		wchar_t Path[MAX_PATH] = L"\\\\";
		lstrcpy(Path + I, cmd);
		FSF.Unquote(Path);
		// Expanding environment variables.
		{
			wchar_t PathCopy[MAX_PATH];
			lstrcpy(PathCopy, Path);
			ExpandEnvironmentStrings(PathCopy, Path, static_cast<DWORD>(std::size(Path)));
		}
		if (!GotoFavorite(Path))
			GotoComputer(Path);
		return true;
	}

	return false;
}

bool NetBrowser::SetOpenFromFilePanel(wchar_t* ShareName)
{
	NetResource nr;

	if (!GetResourceInfo(ShareName, nr))
		return false;

	if (!IsMSNetResource(nr))
		return false;

	OpenFromFilePanel = true;
	return true;
}

bool NetBrowser::GotoComputer(const wchar_t* Dir)
{
#ifdef NETWORK_LOGGING
	LogData(L"Entering GotoComputer");
#endif
	// if there are backslashes in the name, truncate them
	wchar_t ComputerName[MAX_PATH];
	lstrcpy(ComputerName, Dir);
	bool IsShare{false};

	if (auto p = wcschr(ComputerName + 2, L'\\')) // skip past leading backslashes)
	{
		IsShare = true;
		*p = L'\0';
	}
	else
	{
		if (p = wcschr(ComputerName + 2, L'/'); p)
		{
			IsShare = true;
			*p = L'\0';
		}
	}

	CharUpper(ComputerName);
	NetResource res;

	if (!GetResourceInfo(ComputerName, res))
		return false;

	/*
	if (!IsMSNetResource (res))
	  return FALSE;
	*/
	if (!IsResourceReadable(res))
	{
		auto err = GetLastError();

		if (err == ERROR_CANCELLED)
			return false;

		if (err == ERROR_INVALID_PASSWORD || err == ERROR_LOGON_FAILURE || err == ERROR_ACCESS_DENIED || err ==
			ERROR_INVALID_HANDLE || err == ERROR_LOGON_TYPE_NOT_GRANTED)
			if (!((AddConnectionFromFavorites(&res) || AddConnectionExplicit(&res)) && IsResourceReadable(res)))
			{
				if (GetLastError() != ERROR_CANCELLED)
					PsInfo.Message(
						&MainGuid,
						nullptr,
						FMSG_WARNING | FMSG_ERRORTYPE | FMSG_MB_OK | FMSG_ALLINONE,
						{},
						reinterpret_cast<const wchar_t*const*>(GetMsg(MError)),
						0,
						0);

				return false;
			}
	}

	CurResource = res;
	PCurResource = &CurResource;
	/*int result = */
	PsInfo.PanelControl(this, FCTL_UPDATEPANEL, 0, {});

	if (IsShare)
	{
		wchar_t ShareName[MAX_PATH];
		lstrcpyn(ShareName, Dir, ARRAYSIZE(ShareName));

		// replace forward slashes with backslashes
		for (auto p = ShareName; *p; ++p)
			if (*p == L'/')
				*p = L'\\';

		// The share might not be listed yet
		PendingCursor = ShareName;
		ApplyPendingCursor();
	}
	else
		PsInfo.PanelControl(this, FCTL_REDRAWPANEL, 0, {});

	return true;
}

void NetBrowser::GotoLocalNetwork()
{
	// Asking the network who we are is slow, and the answer hardly ever changes:
	// use what we learned last time and check it for the next one in the background.
	NetResource parent;

	if (Scanner::FindLocalNetwork(parent))
	{
		Scanner::RefreshLocalNetwork(Opt.ScanNetwork != 0);
		CurResource = parent;
		PCurResource = &CurResource;
		return;
	}

	wchar_t ComputerName[MAX_PATH];
	lstrcpy(ComputerName, L"\\\\");
	DWORD ComputerNameLength = MAX_PATH - 3;

	if (!GetComputerName(ComputerName + 2, &ComputerNameLength))
		return;

	NetResource res;

	if (!GetResourceInfo(ComputerName, res) || !IsMSNetResource(res))
		return;

	if (!GetResourceParent(res, &parent))
		return;

	Scanner::SetLocalNetwork(parent);
	CurResource = parent;
	PCurResource = &CurResource;
}

bool NetBrowser::SetCursorToShare(const wchar_t* Share)
{
	PanelInfo PInfo = {sizeof(PanelInfo)};
	// this returns the items in sorted order, so we can position correctly
	PsInfo.PanelControl(this, FCTL_GETPANELINFO, 0, &PInfo);

	const auto* LeafName = FSF.PointToName(Share);

	// prevent recursion
	for (size_t i = 0; i < PInfo.ItemsNumber; i++)
	{
		wchar_t szAnsiName[MAX_PATH]{};
		size_t Size = PsInfo.PanelControl(this, FCTL_GETPANELITEM, static_cast<intptr_t>(i), {});
		auto* PPI = (PluginPanelItem*)malloc(Size);

		if (PPI)
		{
			FarGetPluginPanelItem gpi = {sizeof(FarGetPluginPanelItem), Size, PPI};
			PsInfo.PanelControl(this, FCTL_GETPANELITEM, static_cast<intptr_t>(i), &gpi);
			lstrcpyn(szAnsiName, PPI->FileName, ARRAYSIZE(szAnsiName));
			free(PPI);
		}

		if (!FSF.LStricmp(szAnsiName, Share) || !FSF.LStricmp(szAnsiName, LeafName))
		{
			PanelRedrawInfo info = {sizeof(PanelRedrawInfo), i, 0};
			PsInfo.PanelControl(this, FCTL_REDRAWPANEL, 0, &info);
			return true;
		}
	}

	return false;
}

void NetBrowser::RemoveItems()
{
	if (!CheckFavoriteItem(PCurResource))
		return;

	// We are in Favorites folder, so we can remove items from this folder
	PanelInfo PInfo = {sizeof(PanelInfo)};
	PsInfo.PanelControl(this, FCTL_GETPANELINFO, 0, &PInfo);

	wchar_t szConfirmation[MAX_PATH * 2];

	if (PInfo.SelectedItemsNumber == 1)
	{
		size_t Size = PsInfo.PanelControl(this, FCTL_GETSELECTEDPANELITEM, 0, nullptr);
		auto* PPI = (PluginPanelItem*)malloc(Size);

		if (PPI)
		{
			FarGetPluginPanelItem gpi = {sizeof(FarGetPluginPanelItem), Size, PPI};
			PsInfo.PanelControl(this, FCTL_GETSELECTEDPANELITEM, 0, &gpi);
			FSF.sprintf(szConfirmation, GetMsg(MRemoveFavItem), PPI->FileName);
			free(PPI);
		}
	}
	else // PInfo.SelectedItemsNumber > 1
		FSF.sprintf(szConfirmation, GetMsg(MRemoveFavItems), PInfo.SelectedItemsNumber);

	const wchar_t* Msg[4];
	Msg[0] = GetMsg(MRemoveFavCaption);
	Msg[1] = szConfirmation;
	Msg[2] = GetMsg(MOk);
	Msg[3] = GetMsg(MCancel);

	if (0 != PsInfo.Message(&MainGuid, nullptr, FMSG_WARNING, L"RemoveItemFav", Msg, std::size(Msg), 2))
	{
		return; // User canceled deletion
	}

	for (size_t i = 0; i < PInfo.SelectedItemsNumber; i++)
	{
		size_t Size = PsInfo.PanelControl(this, FCTL_GETSELECTEDPANELITEM, static_cast<intptr_t>(i), {});
		auto* PPI = (PluginPanelItem*)malloc(Size);

		if (PPI)
		{
			FarGetPluginPanelItem gpi = {sizeof(FarGetPluginPanelItem), Size, PPI};
			PsInfo.PanelControl(this, FCTL_GETSELECTEDPANELITEM, static_cast<intptr_t>(i), &gpi);
			RemoveFromFavorites(PPI->FileName);
			free(PPI);
		}
	}
}
