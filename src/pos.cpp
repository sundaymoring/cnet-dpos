#include "pos.h"
#include "txdb.h"
#include "script/interpreter.h"
#include "validation.h"
#include "chainparams.h"


// BlackCoin kernel protocol v3
// coinstake must meet hash target according to the protocol:
// kernel (input 0) must meet the formula
//     hash(nStakeModifier + txPrev.nTime + txPrev.vout.hash + txPrev.vout.n + nTime) < bnTarget * nWeight
// this ensures that the chance of getting a coinstake is proportional to the
// amount of coins one owns.
// The reason this hash is chosen is the following:
//   nStakeModifier: scrambles computation to make it very difficult to precompute
//                   future proof-of-stake
//   txPrev.nTime: slightly scrambles computation
//   txPrev.vout.hash: hash of txPrev, to reduce the chance of nodes
//                     generating coinstake at the same time
//   txPrev.vout.n: output number of txPrev, to reduce the chance of nodes
//                  generating coinstake at the same time
//   nTime: current timestamp
//   block/tx hash should not be used here as they can be generated in vast
//   quantities so as to generate blocks faster, degrading the system back into
//   a proof-of-work situation.
//
bool CheckStakeKernelHash( const CBlockIndex* pindexPrev, unsigned int nBits, const CCoins& txPrev, const COutPoint& prevout, unsigned int nTimeTx)
{
    // Weight
    int64_t nValueIn = txPrev.vout[prevout.n].nValue;
    if (nValueIn == 0)
        return false;

    // Base target
    arith_uint256 bnTarget;
    bnTarget.SetCompact(nBits);
    // Calculate hash
    CHashWriter ss(SER_GETHASH, 0);
//    ss << pindexPrev->nStakeModifier << txPrev.nTime << prevout.hash << prevout.n << nTimeTx;
    ss << pindexPrev->GetBlockHash() << pindexPrev->GetBlockTime() << prevout.hash << prevout.n << nTimeTx;
    uint256 hashProofOfStake = ss.GetHash();

    // Now check if proof-of-stake hash meets target protocol
    if (UintToArith256(hashProofOfStake) / nValueIn > bnTarget)
        return false;

    return true;
}

bool IsConfirmedInNPrevBlocks(const CDiskTxPos& txindex, const CBlockIndex* pindexFrom, int nMaxDepth, int& nActualDepth)
{
    for (const CBlockIndex* pindex = pindexFrom; pindex && pindexFrom->nHeight - pindex->nHeight < nMaxDepth; pindex = pindex->pprev)
    {
        if (pindex->nDataPos == txindex.nPos && pindex->nFile == txindex.nFile)
        {
            nActualDepth = pindexFrom->nHeight - pindex->nHeight;
            return true;
        }
    }

    return false;
}


// Check kernel hash target and coinstake signature
bool CheckProofOfStake(CBlockIndex* pindexPrev, const CTransaction& tx, unsigned int nBits, CValidationState &state)
{
    if (!tx.IsCoinStake())
        return error("CheckProofOfStake() : called on non-coinstake %s", tx.GetHash().ToString());

    // Kernel (input 0) must match the stake hash target per coin age (nBits)
    const CTxIn& txin = tx.vin[0];

    // First try finding the previous transaction in database
    CMutableTransaction txPrev;
    CDiskTxPos txindex;

    if (!ReadFromDisk(txPrev, txindex, *pblocktree, txin.prevout))
       return state.DoS(1, error("CheckProofOfStake() : INFO: read txPrev failed"));  // previous transaction not in main chain, may occur during initial download

    // Verify signature
    if (!VerifySignature(txPrev, tx, 0, SCRIPT_VERIFY_NONE, 0))
       return state.DoS(100, error("CheckProofOfStake() : VerifySignature failed on coinstake %s", tx.GetHash().ToString()));

    // Read block header
    CBlock block;
    const CDiskBlockPos& pos = CDiskBlockPos(txindex.nFile, txindex.nPos);
    if (!ReadBlockFromDisk(block, pos, Params().GetConsensus()))
       return fDebug? error("CheckProofOfStake() : read block failed") : false; // unable to read block of previous transaction

    // Min age requirement
    int nDepth;
    if (IsConfirmedInNPrevBlocks(txindex, pindexPrev, nStakeMinConfirmations - 1, nDepth))
       return state.DoS(100, error("CheckProofOfStake() : tried to stake at depth %d", nDepth + 1));

    return true;
}

bool VerifySignature(const CMutableTransaction& txFrom, const CTransaction& txTo, unsigned int nIn, unsigned int flags, int nHashType)
{
    assert(nIn < txTo.vin.size());
    const CTxIn& txin = txTo.vin[nIn];
    if (txin.prevout.n >= txFrom.vout.size())
        return false;
    const CTxOut& txout = txFrom.vout[txin.prevout.n];

    if (txin.prevout.hash != txFrom.GetHash())
        return false;

//    const CAmount& amount = txout.nValue;
    return true;
//    return VerifyScript(txin.scriptSig, txout.scriptPubKey, flags, TransactionSignatureChecker(&txTo, nIn, amount),  NULL);
}
