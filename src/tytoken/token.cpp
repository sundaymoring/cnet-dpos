#include "tytoken/token.h"

#include "wallet/coincontrol.h"
#include "wallet/wallet.h"
#include "script/script.h"
#include "consensus/validation.h"
#include "script/standard.h"
#include "net.h"
#include "policy/policy.h"
#include "script/standard.h"

#include "util.h"

#include <vector>


//TODO bitcoin sendfrom assetAddress, and token in assetAddress. now bitcoin sendfrom other address.
bool CTokenIssure::createTokenTransaction(const CBitcoinAddress& tokenAddress, uint256& txid, std::string& strFailReason)
{
    if (pwalletMain == NULL) return false;

    CScript scriptReturn, scriptToken;
    CDataStream ds(SER_NETWORK, PROTOCOL_VERSION);
    ds << *this;
    scriptReturn << OP_RETURN << std::vector<unsigned char>(ds.begin(), ds.end());
    scriptToken = GetScriptForDestination(tokenAddress.Get());

    std::vector<CRecipient> vecRecipients;
    vecRecipients.push_back(CRecipient{scriptReturn, 0, false, TOKENID_ZERO, 0});
    vecRecipients.push_back(CRecipient{scriptToken, GetDustThreshold(scriptToken), false, TOKENID_ZERO, nValue});

    CWalletTx wtxNew;
    int64_t nFeeRet = 0;
    int nChangePosInOut = 2;
    CReserveKey reserveKey(pwalletMain);
    if (!pwalletMain->CreateTransaction(vecRecipients, wtxNew, reserveKey, nFeeRet, nChangePosInOut, strFailReason, NULL, true, TTC_ISSUE)) {
        LogPrintf("%s: ERROR: wallet transaction creation failed: %s\n", __func__, strFailReason);
        return false;
    }

    // Commit the transaction to the wallet and broadcast)
    LogPrintf("%s: %s; nFeeRet = %d\n", __func__, wtxNew.tx->ToString(), nFeeRet);
    CValidationState state;
    if (!pwalletMain->CommitTransaction(wtxNew, reserveKey, g_connman.get(), state)){
        strFailReason = strprintf("Transaction commit failed:: %s", state.GetRejectReason());
        return false;
    }
    txid = wtxNew.GetHash();

    return true;
}

bool CTokenIssure::decodeTokenTransaction(const CTransaction &tx, std::string strFailReason)
{
    if ( TTC_ISSUE != tx.GetTokenCode() ){
        strFailReason = "token procotol is not issure";
        return false;
    }

    txnouttype type;
    std::vector<unsigned char> vReturnData;
    bool ret = ExtractPushDatas(tx.vout[0].scriptPubKey, type, vReturnData);
    if (!ret || type!=TX_NULL_DATA){
        strFailReason = "issure token procotol error";
        return false;
    }
    CDataStream ds(vReturnData, SER_NETWORK, CLIENT_VERSION);
    ds >> *this;

    //TODO check token is already exist
    //TODO check value is equal to vout[1] nTokenvalue

    return true;
}

// TODO can data remove
// This function requests the wallet create an Omni transaction using the supplied parameters and payload
bool WalletTxBuilder(const std::string& assetAddress, int64_t tokenAmount, const std::vector<unsigned char>& data, uint256& txid, std::string& rawHex, bool commit)
{
//#ifdef ENABLE_WALLET

    CWalletTx wtxNew;
    int64_t nFeeRet = 0;
    int nChangePosInOut = 2;
    std::string strFailReason;
    std::vector<std::pair<CScript, int64_t> > vecSend;
    CReserveKey reserveKey(pwalletMain);

    //TODO bitcoin sendfrom assetAddress, and token in assetAddress. now bitcoin sendfrom other address.

    // Encode the data outputs
    CBitcoinAddress addr = CBitcoinAddress(assetAddress);
    CScript scptReturn, scptToken;
    scptReturn << OP_RETURN << data;
    scptToken = GetScriptForDestination(addr.Get());

    std::vector<CRecipient> vecRecipients;
    vecRecipients.push_back(CRecipient{scptReturn, 0, false, TOKENID_ZERO, 0});
    vecRecipients.push_back(CRecipient{scptToken, GetDustThreshold(scptToken), false, TOKENID_ZERO, tokenAmount});

    // Ask the wallet to create the transaction (note mining fee determined by Bitcoin Core params)
     if (!pwalletMain->CreateTransaction(vecRecipients, wtxNew, reserveKey, nFeeRet, nChangePosInOut, strFailReason, NULL, true, TTC_ISSUE)) {
        LogPrintf("%s: ERROR: wallet transaction creation failed: %s\n", __func__, strFailReason);
        return false;
    }

    // If this request is only to create, but not commit the transaction then display it and exit
    if (!commit) {
//        rawHex = EncodeHexTx(wtxNew);
        return true;
    } else {
        // Commit the transaction to the wallet and broadcast)
        LogPrintf("%s: %s; nFeeRet = %d\n", __func__, wtxNew.tx->ToString(), nFeeRet);
        CValidationState state;
        if (!pwalletMain->CommitTransaction(wtxNew, reserveKey, g_connman.get(), state)) return false;
        txid = wtxNew.GetHash();
        return true;
    }
//#else
//    return MP_ERR_WALLET_ACCESS;
//#endif
}

//TODO is TTC_BITCOIN  need define again
tokencode GetTxTokenCode(const CTransaction& tx)
{
    assert(tx.vout.size()>0);
    if (tx.IsCoinBase() || tx.IsCoinStake())
//        return TTC_BITCOIN;
        return TTC_NONE;
    // vout[0] op_return
    // vout[1] dust bitcoin, token own address
    // vout[2] charge address
    if (tx.vout.size() < 2)
//        return TTC_BITCOIN;
        return TTC_NONE;

    if (tx.vout[0].nValue >0)
//        return TTC_BITCOIN;
        return TTC_NONE;

    txnouttype whichType;
    std::vector<unsigned char> vPushData;
    // get the scriptPubKey corresponding to this input:
    if (!ExtractPushDatas(tx.vout[0].scriptPubKey, whichType, vPushData)){
//        return TTC_UNKNOW;
        return TTC_NONE;
    }

    if (whichType != TX_NULL_DATA){
        for (const auto& out:tx.vout){
            if (out.tokenID != TOKENID_ZERO && out.nTokenValue >0)
                return TTC_SEND;
        }
//        return TTC_BITCOIN;
        return TTC_NONE;
    } else {
        if (vPushData[0] == 'T'&&
                vPushData[1] == 'T' &&
                vPushData[2] == 1){

            return TTC_ISSUE;
        }
    }

//    return TTC_UNKNOW;
    return TTC_NONE;
}

