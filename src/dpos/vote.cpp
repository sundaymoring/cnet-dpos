#include "dpos/vote.h"
#include "streams.h"
#include "wallet/wallet.h"
#include "util.h"

#include <vector>
#include <set>

typedef boost::shared_lock<boost::shared_mutex> read_lock;
typedef boost::unique_lock<boost::shared_mutex> write_lock;

static CScript CreateDposScript(CKeyID keyID, CScript data){

    time_t t = time(NULL);
    CScriptNum num(t);
    auto vch = num.getvch();

    CKey key;
    pwalletMain->GetKey(keyID, key);
    auto pubkey = key.GetPubKey();

    std::vector<unsigned char> vchSig;
    key.Sign(Hash(vch.begin(), vch.end()), vchSig);

    CScript script;
    script.resize(3);
    script[0] = OP_DPOS;
    script[1] = 0x01;
    script[2] = DPOS_PROTOCOL_VERSION;
    script << ToByteVector(pubkey)
           << num
           << vchSig
           << ToByteVector(data);

    return script;
}

CScript CreateDposRegisterScript(CBitcoinAddress address, const std::string& name){

    CScript registeScript;
    registeScript.push_back(dpos_register);
    auto vName = ToByteVector(name);
    vName.resize(32);
    registeScript << vName;

    CKeyID keyID;
    address.GetKeyID(keyID);

    return CreateDposScript(keyID, registeScript);
}

CScript CreateDposVoteScript(CBitcoinAddress voter, std::set<CBitcoinAddress> setDeletate, bool isVote){

    dposType op_code = dpos_vote;
    if(isVote == false) {
        op_code = dpos_revoke;
    }
    CScript voteScript;
    voteScript.push_back(op_code);

    unsigned char delegateKeys[Vote::MaxNumberOfVotes * 20];
    unsigned char *p_delegateKeys = &delegateKeys[0];
    for (auto& a: setDeletate) {
        CKeyID id;
        a.GetKeyID(id);
        memcpy(p_delegateKeys, id.begin(), 20);
        p_delegateKeys += 20;
    }
    voteScript << std::vector<unsigned char>(&delegateKeys[0], p_delegateKeys);

    CKeyID voterKeyID;
    voter.GetKeyID(voterKeyID);
    return CreateDposScript(voterKeyID, voteScript);
}


#define VOTE_FILE "vote.dat"
#define DELEGATE_FILE "delegate.dat"
#define BALANCE_FILE "balance.dat"
#define CONTROL_FILE "control.dat"
#define INVALIDVOTETX_FILE "invalidvotetx.dat"

bool Vote::Init(int64_t nBlockHeight, const std::string& strBlockHash)
{
    //write_lock w(lockVote);

    strFilePath = (GetDataDir() / "dpos").string();
    strDelegateFileName = strFilePath + "/" + DELEGATE_FILE;
    strVoteFileName = strFilePath + "/" + VOTE_FILE;
    strBalanceFileName = strFilePath + "/" + BALANCE_FILE;
    strControlFileName = strFilePath + "/" + CONTROL_FILE;
    strInvalidVoteTxFileName = strFilePath + "/" + INVALIDVOTETX_FILE;

    if(nBlockHeight == 0) {
        if(!boost::filesystem::is_directory(strFilePath)) {
            boost::filesystem::create_directories(strFilePath);
        }
        return true;
    } else {
        return Load(nBlockHeight, strBlockHash);
    }
}

Vote& Vote::GetInstance(){
    static Vote vote;
    return vote;
}


bool Vote::Store(int64_t nBlockHeight, const std::string& strBlockHash)
{
    if(nBlockHeight == 0) {
        return true;
    }

    if(Write(strBlockHash) == false) {
        return false;
    }

    if(WriteControlFile(nBlockHeight, strBlockHash, strControlFileName + "-temp") == false) {
        Delete(strBlockHash);
        return false;
    }

    int64_t nBlockHeightTemp = 0;
    std::string strBlockHashTemp;
    if(ReadControlFile(nBlockHeightTemp, strBlockHashTemp, strControlFileName) == true) {
        Delete(strBlockHashTemp);
    }

    rename(strControlFileName.c_str(), (strControlFileName + "-old").c_str());
    rename((strControlFileName + "-temp").c_str(), strControlFileName.c_str());
    remove((strControlFileName + "-old").c_str());

    return true;
}

bool Vote::Load(int64_t height, const std::string& strBlockHash)
{
    if(RepairFile(height, strBlockHash) == false)
        return false;

    return Read();
}

bool Vote::ProcessVote(CKeyID& voter, const std::set<CKeyID>& delegates, uint256 hash, uint64_t height, bool fUndo)
{
    if(fUndo) {
        return ProcessUndoVote(voter, delegates, hash, height);
    } else {
        return ProcessVote(voter, delegates, hash, height);
    }
}

bool Vote::ProcessVote(CKeyID& voter, const std::set<CKeyID>& delegates, uint256 hash, uint64_t height)
{
    bool ret = ProcessVote(voter, delegates);
    if(ret == false) {
        AddInvalidVote(hash, height);
        LogPrintf("ProcessVote InvalidTx Hash:%s height:%lld\n", hash.ToString().c_str(), height);
    }

    LogPrint("DPoS", "DPoS ProcessVote Height:%u\n", height);
    for(auto i : delegates) {
        LogPrint("DPoS", "DPoS ProcessVote: %s ---> %s(%s)\n", CBitcoinAddress(voter).ToString().c_str(), Vote::GetInstance().GetDelegate(i).c_str(), CBitcoinAddress(i).ToString().c_str());
    }
    if(ret) {
        LogPrint("DPoS", "DPoS ProcessVote success\n");
    } else {
        LogPrintf("DPoS ProcessVote fail\n");
    }

    return ret;
}

bool Vote::ProcessUndoVote(CKeyID& voter, const std::set<CKeyID>& delegates, uint256 hash, uint64_t height)
{
    if(FindInvalidVote(hash)) {
        LogPrintf("ProcessUndoVote InvalidTx Hash:%s height:%lld\n", hash.ToString().c_str(), height);
        return true;
    }

    bool ret = ProcessCancelVote(voter, delegates);

    LogPrint("DPoS", "DPoS ProcessUndoVote Height:%u\n", height);
    for(auto i : delegates) {
        LogPrint("DPoS", "DPoS ProcessUndoVote: %s ---> %s(%s)\n", CBitcoinAddress(voter).ToString().c_str(), Vote::GetInstance().GetDelegate(i).c_str(), CBitcoinAddress(i).ToString().c_str());
    }
    if(ret) {
        LogPrint("DPoS", "DPoS ProcessUndoVote success\n");
    } else {
        LogPrintf("DPoS ProcessUndoVote fail\n");
    }

    return ret;
}

bool Vote::ProcessVote(CKeyID& voter, const std::set<CKeyID>& delegates)
{
    write_lock w(lockVote);
    uint64_t votes = 0;

    {
        auto it = mapVoterDelegates.find(voter);
        if(it != mapVoterDelegates.end()) {
            votes = it->second.size();
        }
    }

    if((votes + delegates.size()) > Vote::MaxNumberOfVotes) {
        return false;
    }

    for(auto item : delegates)
    {
        if(mapDelegateName.find(item) == mapDelegateName.end())
            return false;

        auto it = mapDelegateVoters.find(item);
        if(it != mapDelegateVoters.end()) {
            if(it->second.find(voter) != it->second.end()) {
                return false;
            }
        }
    }

    for(auto item : delegates)
    {
        mapDelegateVoters[item].insert(voter);
    }

    auto& setVotedDelegates = mapVoterDelegates[voter];
    for(auto item : delegates)
    {
        setVotedDelegates.insert(item);
    }

    return true;
}

bool Vote::ProcessCancelVote(CKeyID& voter, const std::set<CKeyID>& delegates, uint256 hash, uint64_t height, bool fUndo)
{
    if(fUndo) {
        return ProcessUndoCancelVote(voter, delegates, hash, height);
    } else {
        return ProcessCancelVote(voter, delegates, hash, height);
    }
}

bool Vote::ProcessCancelVote(CKeyID& voter, const std::set<CKeyID>& delegates, uint256 hash, uint64_t height)
{
    bool ret = ProcessCancelVote(voter, delegates);
    if(ret == false) {
        AddInvalidVote(hash, height);
        LogPrintf("ProcessCancelVote InvalidTx Hash:%s height:%lld\n", hash.ToString().c_str(), height);
    }

    LogPrint("DPoS", "DPoS ProcessCancelVote Height:%u\n", height);
    for(auto i : delegates) {
        LogPrint("DPoS", "DPoS ProcessCancelVote: %s ---> %s(%s)\n", CBitcoinAddress(voter).ToString().c_str(), Vote::GetInstance().GetDelegate(i).c_str(), CBitcoinAddress(i).ToString().c_str());
    }
    if(ret) {
        LogPrint("DPoS", "DPoS ProcessCancelVote success\n");
    } else {
        LogPrintf("DPoS ProcessCancelVote fail\n");
    }

    return ret;
}

bool Vote::ProcessUndoCancelVote(CKeyID& voter, const std::set<CKeyID>& delegates, uint256 hash, uint64_t height)
{
    if(FindInvalidVote(hash)) {
        LogPrintf("ProcessUndoCancelVote InvalidTx Hash:%s height:%lld\n", hash.ToString().c_str(), height);
        return true;
    }

    bool ret = ProcessVote(voter, delegates);
    LogPrint("DPoS", "DPoS ProcessUndoCancelVote Height:%u\n", height);
    for(auto i : delegates) {
        LogPrint("DPoS", "DPoS ProcessUndoCancelVote: %s ---> %s(%s)\n", CBitcoinAddress(voter).ToString().c_str(), Vote::GetInstance().GetDelegate(i).c_str(), CBitcoinAddress(i).ToString().c_str());
    }
    if(ret) {
        LogPrint("DPoS", "DPoS ProcessUndoCancelVote success\n");
    } else {
        LogPrintf("DPoS ProcessUndoCancelVote fail\n");
    }

    return ret;
}

bool Vote::ProcessCancelVote(CKeyID& voter, const std::set<CKeyID>& delegates)
{
    write_lock w(lockVote);
    if(delegates.size() > Vote::MaxNumberOfVotes) {
        return false;
    }

    for(auto item : delegates)
    {
        auto it = mapDelegateVoters.find(item);
        if(it != mapDelegateVoters.end()) {
            if(it->second.find(voter) == it->second.end()) {
                return false;
            }
        } else {
            return false;
        }
    }

    for(auto item : delegates)
    {
        auto& it = mapDelegateVoters[item];
        if(it.size() != 1) {
            mapDelegateVoters[item].erase(voter);
        } else {
            mapDelegateVoters.erase(item);
        }
    }

    auto& setVotedDelegates = mapVoterDelegates[voter];
    if(setVotedDelegates.size() == delegates.size()) {
        mapVoterDelegates.erase(voter);
    } else {
        for(auto item : delegates)
        {
            setVotedDelegates.erase(item);
        }
    }

    return true;
}

bool Vote::ProcessRegister(CKeyID& delegate, const std::string& strDelegateName, uint256 hash, uint64_t height, bool fUndo)
{
    if(fUndo) {
        return ProcessUndoRegister(delegate, strDelegateName, hash, height);
    } else {
        return ProcessRegister(delegate, strDelegateName, hash, height);
    }
}

bool Vote::ProcessRegister(CKeyID& delegate, const std::string& strDelegateName, uint256 hash, uint64_t height)
{
    bool ret = ProcessRegister(delegate, strDelegateName);
    if(ret == false) {
        AddInvalidVote(hash, height);
        LogPrint("DPoS", "ProcessRegister InvalidTx Hash:%s height:%lld\n", hash.ToString().c_str(), height);
    }

    if(ret) {
        LogPrint("DPoS", "DPoS ProcessRegister: Height:%u %s ---> %s success\n", height, CBitcoinAddress(delegate).ToString().c_str(), strDelegateName.c_str());
    } else {
        LogPrintf("DPoS ProcessRegister: Height:%u %s ---> %s fail\n", height, CBitcoinAddress(delegate).ToString().c_str(), strDelegateName.c_str());
    }

    return ret;
}

bool Vote::ProcessUndoRegister(CKeyID& delegate, const std::string& strDelegateName, uint256 hash, uint64_t height)
{
    if(FindInvalidVote(hash)) {
        LogPrintf("ProcessUndoRegister InvalidTx Hash:%s height:%lld\n", hash.ToString().c_str(), height);
        return true;
    }

    bool ret = ProcessUnregister(delegate, strDelegateName);

    if(ret) {
        LogPrint("DPoS", "DPoS ProcessUndoRegister: Height:%u %s ---> %s success\n", height, CBitcoinAddress(delegate).ToString().c_str(), strDelegateName.c_str());
    } else {
        LogPrintf("DPoS ProcessUndoRegister: Height:%u %s ---> %s fail\n", height, CBitcoinAddress(delegate).ToString().c_str(), strDelegateName.c_str());
    }

    return ret;
}

bool Vote::ProcessRegister(CKeyID& delegate, const std::string& strDelegateName)
{
    write_lock w(lockVote);
    if(mapDelegateName.find(delegate) != mapDelegateName.end())
        return false;

    if(mapNameDelegate.find(strDelegateName) != mapNameDelegate.end())
        return false;

    mapDelegateName.insert(std::make_pair(delegate, strDelegateName));
    mapNameDelegate.insert(std::make_pair(strDelegateName, delegate));
    return true;
}

bool Vote::ProcessUnregister(CKeyID& delegate, const std::string& strDelegateName)
{
    write_lock w(lockVote);
    if(mapDelegateName.find(delegate) == mapDelegateName.end())
        return false;

    if(mapNameDelegate.find(strDelegateName) == mapNameDelegate.end())
        return false;

    mapDelegateName.erase(delegate);
    mapNameDelegate.erase(strDelegateName);
    return true;
}

uint64_t Vote::GetDelegateVotes(const CKeyID& delegate)
{
    read_lock r(lockVote);
    return _GetDelegateVotes(delegate);
}

uint64_t Vote::_GetDelegateVotes(const CKeyID& delegate)
{
    uint64_t votes = 0;
    auto it = mapDelegateVoters.find(delegate);
    if(it != mapDelegateVoters.end()) {
        for(auto item : it->second) {
            votes += _GetAddressBalance(item);
        }
    }

    return votes;
}

std::set<CKeyID> Vote::GetDelegateVoters(CKeyID& delegate)
{
    read_lock r(lockVote);
    std::set<CKeyID> s;
    auto it = mapDelegateVoters.find(delegate);
    if(it != mapDelegateVoters.end()) {
        s = it->second;
    }

    return s;
}

CKeyID Vote::GetDelegate(const std::string& name)
{
    read_lock r(lockVote);
    auto it = mapNameDelegate.find(name);
    if(it != mapNameDelegate.end())
        return it->second;
    return CKeyID();
}

std::string Vote::GetDelegate(CKeyID keyid)
{
    read_lock r(lockVote);
    auto it = mapDelegateName.find(keyid);
    if(it != mapDelegateName.end())
        return it->second;
    return std::string();
}

bool Vote::HaveVote(CKeyID voter, CKeyID delegate)
{
    bool ret = false;
    read_lock r(lockVote);
    auto it = mapDelegateVoters.find(delegate);
    if(it != mapDelegateVoters.end()) {
        if(it->second.find(voter) != it->second.end()) {
            ret = true;
        }
    }

    return ret;
}

bool Vote::HaveDelegate_Unlock(const std::string& name, CKeyID keyid)
{
    read_lock r(lockVote);
    if(mapNameDelegate.find(name) != mapNameDelegate.end()) {
        return false;
    }

    if(mapDelegateName.find(keyid) != mapDelegateName.end()) {
        return false;
    }

    return true;
}

bool Vote::HaveDelegate(const std::string& name, CKeyID keyid)
{
    read_lock r(lockVote);

    bool ret = false;
    auto it = mapDelegateName.find(keyid);
    if(it != mapDelegateName.end() ) {
        if(it->second == name) {
            ret = true;
        }
    }

    return ret;
}

bool Vote::HaveDelegate(std::string name)
{
    read_lock r(lockVote);
    return mapNameDelegate.find(name) != mapNameDelegate.end();
}

bool Vote::HaveDelegate(CKeyID keyID)
{
    read_lock r(lockVote);
    return mapDelegateName.find(keyID) != mapDelegateName.end();
}

std::set<CKeyID> Vote::GetVotedDelegates(CKeyID& delegate)
{
    read_lock r(lockVote);
    std::set<CKeyID> s;
    auto it = mapVoterDelegates.find(delegate);
    if(it != mapVoterDelegates.end()) {
        s = it->second;
    }

    return s;
}

std::vector<Delegate> Vote::GetTopDelegateInfo(uint64_t nMinHoldBalance, uint32_t nDelegateNum)
{
    read_lock r(lockVote);
    std::set<std::pair<uint64_t, CKeyID>> delegates;

    for(auto item : mapDelegateVoters)
    {
        uint64_t votes = _GetDelegateVotes(item.first);
        if(_GetAddressBalance(item.first) >= nMinHoldBalance) {
            delegates.insert(std::make_pair(votes, item.first));
        }
    }

    for(auto it = mapDelegateName.rbegin(); it != mapDelegateName.rend(); ++it)
    {
        if(_GetAddressBalance(it->first) < nMinHoldBalance) {
            continue;
        }

        if(delegates.size() >= nDelegateNum) {
            break;
        }

        if(mapDelegateVoters.find(it->first) == mapDelegateVoters.end())
            delegates.insert(std::make_pair(0, it->first));
    }

    std::vector<Delegate> result;
    for(auto it = delegates.rbegin(); it != delegates.rend(); ++it)
    {
        if(result.size() >= nDelegateNum) {
            break;
        }

        result.push_back(Delegate(it->second, it->first));
    }

    return result;
}

std::map<std::string, CKeyID> Vote::ListDelegates()
{
    read_lock r(lockVote);
    return mapNameDelegate;
}

uint64_t Vote::GetAddressBalance(const CKeyID& address)
{
    read_lock r(lockVote);
    return _GetAddressBalance(address);
}


bool Vote::RepairFile(int64_t nBlockHeight, const std::string& strBlockHash)
{
    if(!boost::filesystem::is_directory(strFilePath)) {
        boost::filesystem::create_directories(strFilePath);
        if(boost::filesystem::exists(GetDataDir() / BALANCE_FILE)) {
            boost::filesystem::copy_file(GetDataDir() / BALANCE_FILE, strBalanceFileName + "-" + strBlockHash);
        }

        if(boost::filesystem::exists(GetDataDir() / VOTE_FILE)) {
            boost::filesystem::copy_file(GetDataDir() / VOTE_FILE, strVoteFileName + "-" + strBlockHash);
        }

        if(boost::filesystem::exists(GetDataDir() / DELEGATE_FILE)) {
            boost::filesystem::copy_file(GetDataDir() / DELEGATE_FILE, strDelegateFileName + "-" + strBlockHash);
        }

        return WriteControlFile(nBlockHeight, strBlockHash, strControlFileName);
    }

    int64_t nBlockHeightTemp = 0;
    std::string strBlockHashTemp;
    std::string strFileName;

    strFileName = strControlFileName + "-temp";
    if(boost::filesystem::exists(strFileName)) {
        if(ReadControlFile(nBlockHeightTemp, strBlockHashTemp, strFileName)) {
            if(boost::filesystem::exists(strVoteFileName + "-" + strBlockHashTemp)
                && boost::filesystem::exists(strDelegateFileName + "-" + strBlockHashTemp)
                && boost::filesystem::exists(strBalanceFileName + "-" + strBlockHashTemp))
            {
                rename(strFileName.c_str(), strControlFileName.c_str());
                return true;
            }
            remove(strFileName.c_str());
        }
    }

    strFileName = strControlFileName + "-old";
    if(boost::filesystem::exists(strFileName)) {
        if(ReadControlFile(nBlockHeightTemp, strBlockHashTemp, strFileName)) {
            if(boost::filesystem::exists(strVoteFileName + "-" + strBlockHashTemp)
                && boost::filesystem::exists(strDelegateFileName + "-" + strBlockHashTemp)
                && boost::filesystem::exists(strBalanceFileName + "-" + strBlockHashTemp))
            {
                rename(strFileName.c_str(), strControlFileName.c_str());
                return true;
            }
            remove(strFileName.c_str());
        }
    }

    strFileName = strControlFileName;
    if(boost::filesystem::exists(strFileName)) {
        if(ReadControlFile(nBlockHeightTemp, strBlockHashTemp, strFileName)) {
            if(boost::filesystem::exists(strVoteFileName + "-" + strBlockHashTemp)
                || boost::filesystem::exists(strDelegateFileName + "-" + strBlockHashTemp)
                || boost::filesystem::exists(strBalanceFileName + "-" + strBlockHashTemp))
            {
                return true;
            } else {
                return false;
            }
        }
    }

    return false;
}

bool Vote::ReadControlFile(int64_t& nBlockHeight, std::string& strBlockHash, const std::string& strFileName)
{
    bool ret = false;
    FILE *file = fopen(strFileName.c_str(), "r");
    if(file) {
        fscanf(file, "%ld\n", &nBlockHeight);
        char buff[128];
        fscanf(file, "%s\n", buff);
        strBlockHash = buff;

        fclose(file);
        ret = true;
    }

    return ret;
}

bool Vote::WriteControlFile(int64_t nBlockHeight, const std::string& strBlockHash, const std::string& strFileName)
{
    bool ret = false;
    FILE *file = fopen(strFileName.c_str(), "w");
    if(file) {
        fprintf(file, "%ld\n", nBlockHeight);
        fprintf(file, "%s\n", strBlockHash.c_str());
        fclose(file);
        ret = true;
    }

    return ret;
}

bool Vote::Read()
{
    FILE* file = NULL;
    unsigned char buff[20];
    uint32_t count = 0;
    uint64_t balance = 0;

    write_lock w(lockVote);

    if(ReadControlFile(nOldBlockHeight, strOldBlockHash, strControlFileName) == false)
        return false;

    file = fopen((strDelegateFileName + "-" + strOldBlockHash).c_str(), "rb");
    if(file) {
    while(1)
    {
        if(fread(&buff[0], sizeof(buff), 1, file) <= 0) {
            break;
        }

        CKeyID delegate(uint160(std::vector<unsigned char>(&buff[0], &buff[0] + sizeof(buff))));
        fread(&count, sizeof(count), 1, file);

        unsigned char name[128];
        memset(name, 0, sizeof(name));
        fread(&name[0], count, 1, file);

        std::string sname((const char*)&name[0], count);
        mapDelegateName[delegate] = sname;
        mapNameDelegate[sname] = delegate;
    }
    fclose(file);
    }

    file = fopen((strVoteFileName + "-" + strOldBlockHash).c_str(), "rb");
    if(file) {
    while(1)
    {
        if(fread(&buff[0], sizeof(buff), 1, file) <= 0) {
            break;
        }

        CKeyID delegate(uint160(std::vector<unsigned char>(&buff[0], &buff[0] + sizeof(buff))));

        fread(&count, sizeof(count), 1, file);
        for(unsigned int i =0; i < count; ++i) {
            fread(&buff[0], sizeof(buff), 1, file);
            CKeyID voter(uint160(std::vector<unsigned char>(&buff[0], &buff[0] + sizeof(buff))));

            mapDelegateVoters[delegate].insert(voter);
            mapVoterDelegates[voter].insert(delegate);
        }
    }
    fclose(file);
    }

    file = fopen((strInvalidVoteTxFileName + "-" + strOldBlockHash).c_str(), "rb");
    if(file) {
    while(1)
    {
        if(fread(&buff[0], 64, 1, file) <= 0) {
            break;
        }
        uint256 hash;
        hash.SetHex((const char*)&buff[0]);

        uint64_t height;
        fread(&height, sizeof(height), 1, file);
        AddInvalidVote(hash, height);
    }
    fclose(file);
    }

    file = fopen((strBalanceFileName + "-" + strOldBlockHash).c_str(), "rb");
    if(file) {
    while(1)
    {
        if(fread(&buff[0], sizeof(buff), 1, file) <= 0) {
            break;
        }

        CKeyID address(uint160(std::vector<unsigned char>(&buff[0], &buff[0] + sizeof(buff))));
        fread(&balance, sizeof(balance), 1, file);

        mapAddressBalance[address] = balance;
    }
    fclose(file);
    }

    return true;
}


bool Vote::Write(const std::string& strBlockHash)
{
    FILE* file = NULL;
    uint32_t count = 0;
    uint64_t balance = 0;

    write_lock w(lockVote);

    file = fopen((strDelegateFileName + "-" + strBlockHash) .c_str(), "wb");
    for(auto item : mapDelegateName)
    {
        fwrite(item.first.begin(), sizeof(item.first), 1, file);
        count = item.second.length();
        fwrite(&count, sizeof(count), 1, file);
        fwrite(item.second.c_str(), item.second.length(), 1, file);
    }
    fclose(file);

    file = fopen((strVoteFileName + "-" + strBlockHash).c_str(), "wb");
    for(auto item : mapDelegateVoters)
    {
        fwrite(item.first.begin(), sizeof(item.first), 1, file);
        count = item.second.size();
        fwrite(&count, sizeof(count), 1, file);
        for(auto i : item.second) {
            fwrite(i.begin(), sizeof(i), 1, file);
        }
    }
    fclose(file);

    file = fopen((strInvalidVoteTxFileName + "-" + strBlockHash).c_str(), "wb");
    for(auto item : mapHashHeightInvalidVote)
    {
        auto strHash = item.first.GetHex();
        fwrite(strHash.c_str(), strHash.length(), 1, file);
        fwrite(&item.second, sizeof(item.second), 1, file);
    }
    fclose(file);

    file = fopen((strBalanceFileName + "-" + strBlockHash).c_str(), "wb");
    for(auto item : mapAddressBalance)
    {
        balance = item.second;
        if(balance == 0)
            continue;
        fwrite(item.first.begin(), sizeof(item.first), 1, file);
        fwrite(&balance, sizeof(balance), 1, file);
    }
    fclose(file);

    return true;
}

void Vote::Delete(const std::string& strBlockHash)
{
    remove((strDelegateFileName + "-" + strBlockHash).c_str());
    remove((strVoteFileName + "-" + strBlockHash) .c_str());
    remove((strBalanceFileName + "-" + strBlockHash) .c_str());
}

uint64_t Vote::_GetAddressBalance(const CKeyID& address)
{
    auto it = mapAddressBalance.find(address);
    if(it != mapAddressBalance.end()) {
        return it->second;
    } else {
        return 0;
    }
}

void Vote::UpdateAddressBalance(const std::vector<std::pair<CKeyID, int64_t>>& vAddressBalance)
{
    write_lock w(lockVote);
    std::map<CKeyID, int64_t> mapBalance;
    for(auto iter : vAddressBalance) {
        if (iter.second == 0)
            continue;

        mapBalance[iter.first] += iter.second;
    }

    for(auto iter : mapBalance)
    {
        _UpdateAddressBalance(iter.first, iter.second);
    }

    return;
}

uint64_t Vote::_UpdateAddressBalance(const CKeyID& address, int64_t value)
{
    int64_t balance = 0;

    auto it = mapAddressBalance.find(address);
    if(it != mapAddressBalance.end()) {
        balance = it->second + value;
        if(balance < 0) {
            abort();
        }

        if(balance == 0) {
            mapAddressBalance.erase(it);
        } else {
            it->second = balance;
        }
        return balance;
    } else {
        if(value < 0) {
            abort();
        }

        if(value == 0) {
            return 0;
        } else {
            mapAddressBalance[address] = value;
        }

        return value;
    }
}

void Vote::DeleteInvalidVote(uint64_t height)
{
    write_lock r(lockMapHashHeightInvalidVote);
    for(auto it =  mapHashHeightInvalidVote.begin(); it != mapHashHeightInvalidVote.end();) {
        if(it->second <= height) {
            LogPrint("DPoS", "DeleteInvalidVote Hash:%s Height:%llu\n", it->first.ToString().c_str(), it->second);
            it = mapHashHeightInvalidVote.erase(it);
        } else {
            ++it;
        }
    }
}

void Vote::AddInvalidVote(uint256 hash, uint64_t height)
{
    write_lock r(lockMapHashHeightInvalidVote);
    mapHashHeightInvalidVote[hash] = height;
    LogPrint("DPoS", "AddInvalidVote Hash:%s Height:%llu\n", hash.ToString().c_str(), height);
}

bool Vote::FindInvalidVote(uint256 hash)
{
    read_lock r(lockMapHashHeightInvalidVote);
    return mapHashHeightInvalidVote.find(hash) != mapHashHeightInvalidVote.end();
}
