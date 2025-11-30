#include <wallet/blswallet.h>

///////////////////////////////////////////////////////////

std::vector<CKeyID> blsKeyID;

void AddBLSRelated(const CKeyID& keyID)
{
     blsKeyID.push_back(keyID);
}

bool IsBLSRelated(const CKeyID& keyID)
{
     for (unsigned int i = 0; i < blsKeyID.size(); i++) {
          if (blsKeyID[i] == keyID) return true;
     }
     return false;
}

///////////////////////////////////////////////////////////

using BLSKeyPair = std::map<CKeyID, CKey>;
BLSKeyPair blsKeyMap;

bool HaveBLSKey(const CKeyID &address) {
    if (blsKeyMap.count(address) > 0)
        return true;
    return false;
}

bool GetBLSKey(const CKeyID &address, CKey& key) {
    if (!HaveBLSKey(address))
        return false;
    key = blsKeyMap[address];
    return true;
}

bool AddBLSKey(const CKeyID &address, CKey& key) {
    blsKeyMap[address] = key;
    return true;
}

