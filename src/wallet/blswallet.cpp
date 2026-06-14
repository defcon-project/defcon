#include <wallet/blswallet.h>
#include <sync.h>

#include <algorithm>

namespace {

RecursiveMutex cs_blswallet;

std::vector<CKeyID> blsKeyID;

using BLSKeyPair = std::map<CKeyID, CKey>;
BLSKeyPair blsKeyMap;

} // namespace

void AddBLSRelated(const CKeyID& keyID)
{
    LOCK(cs_blswallet);
    if (std::find(blsKeyID.begin(), blsKeyID.end(), keyID) == blsKeyID.end()) {
        blsKeyID.push_back(keyID);
    }
}

bool IsBLSRelated(const CKeyID& keyID)
{
    LOCK(cs_blswallet);
    for (unsigned int i = 0; i < blsKeyID.size(); i++) {
        if (blsKeyID[i] == keyID) return true;
    }
    return false;
}

bool HaveBLSKey(const CKeyID& address)
{
    LOCK(cs_blswallet);
    if (blsKeyMap.count(address) > 0)
        return true;
    return false;
}

bool GetBLSKey(const CKeyID& address, CKey& key)
{
    LOCK(cs_blswallet);
    if (!HaveBLSKey(address))
        return false;
    key = blsKeyMap[address];
    return true;
}

bool AddBLSKey(const CKeyID& address, CKey& key)
{
    LOCK(cs_blswallet);
    blsKeyMap[address] = key;
    return true;
}

void ClearBLSWalletCache()
{
    LOCK(cs_blswallet);
    blsKeyID.clear();
    blsKeyMap.clear();
}
