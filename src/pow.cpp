// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2016 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "pow.h"

#include "arith_uint256.h"
#include "chain.h"
#include "primitives/block.h"
#include "uint256.h"
#include "util.h"

//TODO: DO AS BLACKCOIN
unsigned int GetNextWorkRequired(const CBlockIndex* pindexLast, const CBlockHeader *pblock, bool fProofOfStake, const Consensus::Params& params)
{
    unsigned int nTargetLimit = UintToArith256(params.powLimit).GetCompact();

        // Genesis block
        if (pindexLast == NULL)
            return nTargetLimit;

//        if (!fProofOfStake && pindexLast->IsProofOfWork()){
//            return pindexLast->nBits;
//        }

        const CBlockIndex* pindexPrev = GetLastBlockIndex(pindexLast, fProofOfStake);
        if (pindexPrev->pprev == NULL)
            return nTargetLimit; // first block
        const CBlockIndex* pindexPrevPrev = GetLastBlockIndex(pindexPrev->pprev, fProofOfStake);
        if (pindexPrevPrev->pprev == NULL)
            return nTargetLimit; // second block

    return CalculateNextWorkRequired(pindexPrev, pindexPrevPrev->GetBlockTime(), params);
}

unsigned int CalculateNextWorkRequired(const CBlockIndex* pindexLast, int64_t nFirstBlockTime, const Consensus::Params& params)
{
    if (params.fPowNoRetargeting)
            return pindexLast->nBits;

    int64_t nActualSpacing = pindexLast->GetBlockTime() - nFirstBlockTime;
    int64_t nTargetSpacing = params.nTargetSpacing;

    // Limit adjustment step
    if (nActualSpacing < 0)
        nActualSpacing = nTargetSpacing;
    if (nActualSpacing > nTargetSpacing*10)
        nActualSpacing = nTargetSpacing*10;

    // retarget with exponential moving toward target spacing
    const arith_uint256 bnTargetLimit = UintToArith256(pindexLast->IsProofOfStake() ? params.posLimit : params.powLimit);
    arith_uint256 bnNew;
    bnNew.SetCompact(pindexLast->nBits);
    int64_t nInterval = params.nTargetTimespan / nTargetSpacing;
    bnNew *= ((nInterval - 1) * nTargetSpacing + nActualSpacing + nActualSpacing);
    bnNew /= ((nInterval + 1) * nTargetSpacing);

    if (bnNew <= 0 || bnNew > bnTargetLimit)
        bnNew = bnTargetLimit;

    return bnNew.GetCompact();
}

bool CheckProofOfWork(uint256 hash, unsigned int nBits, const Consensus::Params& params)
{
    bool fNegative;
    bool fOverflow;
    arith_uint256 bnTarget;

    bnTarget.SetCompact(nBits, &fNegative, &fOverflow);

    // Check range
    if (fNegative || bnTarget == 0 || fOverflow || bnTarget > UintToArith256(params.powLimit))
        return false;

    // Check proof of work matches claimed amount
    if (UintToArith256(hash) > bnTarget)
        return false;

    return true;
}
