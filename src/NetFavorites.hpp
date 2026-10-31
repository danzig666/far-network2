#ifndef __NETFAVORITES_HPP__
#define __NETFAVORITES_HPP__

#include <string>
#include <vector>
#include <windows.h>
#include "NetClass.hpp"

struct FAVORITEITEM
{
	const wchar_t* lpRemoteName;
	const wchar_t* lpUserName;
	const wchar_t* lpPassword;
};

#define FAVORITES_UPBROWSE_TO_FAVORITES 0x01L
#define FAVORITES_CHECK_RESOURCES       0x02L

#define FAVORITES_DEFAULTS (FAVORITES_UPBROWSE_TO_FAVORITES|FAVORITES_CHECK_RESOURCES)

// Favorites that need the network to resolve are returned in PendingNames for the caller to deal with.
bool EnumFavorites(const NetResource* pNR, NetResourceList* pList, std::vector<std::wstring>* PendingNames);
const wchar_t* FavoritesProvider();
bool CheckFavoriteItem(const NetResource* pNR);
bool InFavoriteExists(const wchar_t* lpRemoteName);
void WriteFavoriteItem(const FAVORITEITEM* lpFavItem);
bool ReadFavoriteItem(FAVORITEITEM* lpFavItem);
bool GetFavoritesParent(const NetResource& SrcRes, NetResource* lpParent);
bool GetFavoriteResource(const wchar_t* SrcName, NetResource* DstNetResource);
bool RemoveFromFavorites(const wchar_t* SrcName);

#endif //__NETFAVORITES_HPP__
