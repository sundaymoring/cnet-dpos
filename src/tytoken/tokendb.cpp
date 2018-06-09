#include "tytoken/tokendb.h"
#include "primitives/transaction.h"
#include "script/standard.h"
#include "base58.h"

static const char DB_TOKEN = 'T';

CTokenDB* pTokenInfos;

CTokenDB::CTokenDB(size_t nCacheSize, bool fMemory, bool fWipe) : db(GetDataDir() / "token", nCacheSize, fMemory, fWipe, true)
{
}

bool CTokenDB::GetTokenInfo(const uint272& tokenId, CTokenInfo &tokenInfo)
{
    return db.Read(std::make_pair(DB_TOKEN, tokenId), tokenInfo);
}

bool CTokenDB::WriteTokenInfo(const CTokenInfo &tokenInfo)
{
    return db.Write(std::make_pair(DB_TOKEN, tokenInfo.tokenID), tokenInfo);
}

bool CTokenDB::EraseTokenInfo(const uint272& tokenID)
{
    return db.Erase(std::make_pair(DB_TOKEN, tokenID));
}

const std::vector<CTokenInfo> CTokenDB::ListTokenInfos()
{
    std::vector<CTokenInfo> infos;
    CDBIterator* iter = db.NewIterator();
    for(iter->SeekToFirst(); iter->Valid(); iter->Next()){
        CTokenInfo t;
        if( iter->GetValue(t) )
            infos.push_back(t);
    }
    delete iter;
    return infos;
}

void CTokenInfo::FromTx(const CTransaction& tx)
{
    //TODO extract name
    tokenID = tx.vout[1].tokenID;
    amount = tx.vout[1].nTokenValue;
    CTxDestination destination;
    if (ExtractDestination(tx.vout[1].scriptPubKey, destination) ){
        address = CBitcoinAddress(destination).ToString();
    }
}
