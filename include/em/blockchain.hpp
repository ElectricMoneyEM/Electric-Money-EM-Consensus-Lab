#pragma once

#include "common.hpp"
#include "transaction.hpp"
#include "mining.hpp"
#include "block.hpp"

struct L2Deposit;
struct L2Withdrawal;
struct L2Transaction;

bool parse_l2_deposit(json_object*o,L2Deposit&d);
bool parse_l2_withdrawal(json_object*o,L2Withdrawal&w);

bool prior_release_exists(
    const std::vector<Block>&prefix,
    const std::string&claim_id
);

bool find_finalized_withdrawal(
    const std::vector<Block>&prefix,
    const std::string&commitment_hash,
    const std::string&claim_id,
    const std::string&recipient,
    i64 amount
);

bool find_finalized_deposit(
    const std::vector<Block>&prefix,
    const L2Deposit&d
);

bool verify_l2_deposit_manifest(
    json_object*o,
    const std::vector<Block>&prefix
);

std::string withdrawal_release_id_fields(
    const std::string&commitment_hash,
    const std::string&recipient,
    const std::string&claim,
    i64 amount
);

bool verify_l2_commitment(
    json_object*o,
    const std::string&expected_prev_root
);

std::string last_l2_state_root(
    const std::vector<Block>&prefix
);


class Blockchain {
public:

    std::vector<Block> chain;

    std::map<std::string,Transaction> pending;

    std::map<std::string,Block> orphans;

    std::map<std::string,i64> balances,nonces;

    i64 total_issued=0;
    i64 total_burned=0;
    i64 treasury_balance=0;

    std::map<std::string,cpp_int> miner_work;

    std::map<std::string,i64> wallet_anchor;

    std::map<std::string,MiningShare> pending_shares;

    mutable std::mutex mu;

    std::string db;


    Blockchain(std::string f="electric_money.json");


    int height()const;

    i64 supply()const;

    cpp_int cumulative_work()const;


    static int share_diff(int d);

    static int expected_diff(
        const std::vector<Block>&c,
        int h
    );

    static i64 mtp(
        const std::vector<Block>&c
    );


    void genesis();


    static void credit(
        std::map<std::string,i64>&b,
        const std::string&a,
        i64 x
    );

    static void debit(
        std::map<std::string,i64>&b,
        const std::string&a,
        i64 x
    );


    static std::pair<i64,i64> annual_split(
        i64 bal
    );


    // V17.1:
    // Apply at most MAX_TAX_CYCLES_PER_BLOCK annual tax
    // cycles per wallet per block.
    // Prevents multi-year DoS loops while remaining deterministic.
    // Missed years are collected gradually over subsequent blocks.
    static void annual(
        std::map<std::string,i64>&b,
        std::map<std::string,i64>&a,
        i64&treas,
        i64&burn,
        i64 ts
    );


    static void distribute(
        std::map<std::string,i64>&b,
        i64&treas,
        std::map<std::string,cpp_int>&work
    );


    static bool apply_tx(
        const Transaction&t,
        std::map<std::string,i64>&b,
        std::map<std::string,i64>&n,
        i64&burn,
        i64&treas
    );


    bool validate_block(
        const Block&b,
        const Block&p,
        const std::vector<Block>&prefix,
        std::map<std::string,i64>bal,
        std::map<std::string,i64>nc,
        i64 issued,
        i64 burned,
        i64 treas,
        std::map<std::string,cpp_int>mw,
        std::map<std::string,i64>anchors,
        std::map<std::string,i64>*outbal=nullptr,
        std::map<std::string,i64>*outnc=nullptr,
        i64*outissued=nullptr,
        i64*outburn=nullptr,
        i64*outtreas=nullptr,
        std::map<std::string,cpp_int>*outmw=nullptr,
        std::map<std::string,i64>*outanchors=nullptr,
        i64 now=now_s()
    )const;


    static bool validate_genesis(
        const Block&b
    );


    bool replay(
        const std::vector<Block>&c,
        std::map<std::string,i64>&ob,
        std::map<std::string,i64>&on,
        i64&oi,
        i64&oburn,
        i64&ot,
        std::map<std::string,cpp_int>&omw,
        std::map<std::string,i64>&oa
    )const;


    bool validate_chain(
        const std::vector<Block>&c
    )const;


    i64 next_nonce(
        const std::string&a
    );


    bool add_transaction(
        const Transaction&t
    );


    Block candidate(
        const std::string&miner,
        const std::vector<std::string>&l2={}
    );


    Block mine_pending(
        const std::string&miner,
        const std::function<bool()>&stop=[](){
            return false;
        }
    );


    Block mine_external(
        const std::string&miner,
        const std::vector<std::string>&extra
    );


    // Accept a block that directly extends the current chain.
    // Blocks whose parent is not currently available are stored
    // in the bounded orphan pool for later recovery.
    bool accept_block(
        const Block&b
    );


    // Attempt deterministic automatic recovery of orphan blocks
    // whose parent has become available.
    //
    // This function is intended to be called while the Blockchain
    // mutex is already held by the caller. It must not call
    // accept_block(), which would attempt to lock the same mutex.
    void process_orphans();


    std::vector<Block> snapshot_chain() const;


    std::vector<Block> snapshot_from(
        size_t start,
        size_t max_count
    ) const;


    bool replace_chain(
        const std::vector<Block>&c
    );


    static cpp_int cumulative(
        const std::vector<Block>&c
    );


    void save()const;

    bool load();
};
