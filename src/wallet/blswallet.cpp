#include <wallet/blswallet.h>

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
