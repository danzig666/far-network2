#include "NetCommon.hpp"
#include "Network.hpp"
#include "guid.hpp"
#include "NetLng.hpp"

Options Opt;

NetResource CommonCurResource;
NetResource* PCommonCurResource{};


BOOL DlgCreateFolder(wchar_t* lpBuffer, size_t nBufferSize)
{
	return PsInfo.InputBox(
		&MainGuid,
		nullptr,
		L"Make Folder",
		L"Folder name:",
		L"NewFolder",
		{},
		lpBuffer,
		nBufferSize,
		{},
		FIB_BUTTONS
	) != FALSE;
}

const wchar_t* GetMsg(int MsgId)
{
	return PsInfo.GetMsg(&MainGuid, MsgId);
}

TSaveScreen::TSaveScreen()
{
	hScreen = PsInfo.SaveScreen(0, 0, -1, -1);
	// No new messages, the language files stay the same: "Esc: Cancel" out of what's already there.
	std::wstring CancelHint = L"Esc: ";
	for (const auto* p = GetMsg(MCancel); *p; ++p)
		if (*p != L'&')
			CancelHint += *p;

	const wchar_t* MsgItems[] = {GetMsg(MWaitForNetworkBrowse1), GetMsg(MWaitForNetworkBrowse2), CancelHint.c_str()};
	PsInfo.Message(&MainGuid, nullptr, 0, {}, MsgItems,ARRAYSIZE(MsgItems), 0);
}

TSaveScreen::~TSaveScreen()
{
	if (hScreen)
	{
		PsInfo.RestoreScreen({});
		PsInfo.RestoreScreen(hScreen);
	}
}
