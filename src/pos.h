#ifndef POS_H
#define POS_H

#include "chain.h"
#include "consensus/validation.h"
#include "txdb.h"

static const int nStakeMinConfirmations = 5;
static const unsigned int nStakeMinAge = 10; // 8 hours --> 10 second

// To decrease granularity of timestamp
// Supposed to be 2^n-1
static const int STAKE_TIMESTAMP_MASK = 15;


bool CheckStakeKernelHash( const CBlockIndex* pindexPrev, unsigned int nBits, const CCoins& txPrev, const COutPoint& prevout, unsigned int nTimeTx);
bool IsConfirmedInNPrevBlocks(const CDiskTxPos& txindex, const CBlockIndex* pindexFrom, int nMaxDepth, int& nActualDepth);
bool CheckProofOfStake(CBlockIndex* pindexPrev, const CTransaction& tx, unsigned int nBits, CValidationState &state);
bool VerifySignature(const CMutableTransaction& txFrom, const CTransaction& txTo, unsigned int nIn, unsigned int flags, int nHashType);

#endif // POS_H
