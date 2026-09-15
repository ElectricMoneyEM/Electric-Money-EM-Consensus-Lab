#include "em/blockchain.hpp"

Blockchain::Blockchain(std::string f):db(std::move(f)) {
        if(std::filesystem::exists(db)) {
            if(!load()) throw std::runtime_error("database exists but failed validation");
        } else {
            genesis();
        }
    }

int Blockchain::height()const {
        return (int)chain.size()-1;
    }

i64 Blockchain::supply()const {
        return total_issued-total_burned;
    }

cpp_int Blockchain::cumulative_work()const {
        cpp_int x=0;
        for(auto&b:chain)x+=block_work(b.difficulty);
        return x;
    }

int Blockchain::share_diff(int d) {
        return std::max(1,d-SHARE_DIFFICULTY_OFFSET);
    }

int Blockchain::expected_diff(const std::vector<Block>&c,int h) {
        if(h==0)return GENESIS_DIFFICULTY;
        int d=c[h-1].difficulty;
        if(h%DIFFICULTY_INTERVAL)return d;
        auto&first=c[h-DIFFICULTY_INTERVAL];
        auto&last=c[h-1];
        i64 actual=std::max<i64>(1,last.timestamp-first.timestamp),expected=(i64)TARGET_BLOCK_TIME*(DIFFICULTY_INTERVAL-1);
        if(actual<expected/2)++d;
        else if(actual>expected*2)--d;
        return std::clamp(d,MIN_DIFFICULTY,MAX_DIFFICULTY);
    }

i64 Blockchain::mtp(const std::vector<Block>&c) {
        std::vector<i64>t;
        for(int i=std::max(0,(int)c.size()-MTP_WINDOW);i<(int)c.size();++i)t.push_back(c[i].timestamp);
        std::sort(t.begin(),t.end());
        return t[t.size()/2];
    }

void Blockchain::genesis() {
        std::string tid=hash_obj( {
             {
                "amount",std::to_string(BASE_REWARD)
            }
            , {
                "message",json_escape(GENESIS_MESSAGE)
            }
            , {
                "recipient",json_escape("Miner_Genesis")
            }
        }
        );
        std::string tx=canonical( {
             {
                "amount",std::to_string(BASE_REWARD)
            }
            , {
                "memo",json_escape(GENESIS_MESSAGE)
            }
            , {
                "recipient",json_escape("Miner_Genesis")
            }
            , {
                "tx_id",json_escape(tid)
            }
            , {
                "type",json_escape("genesis")
            }
        }
        );
        Block b;
        b.index=0;
        b.previous_hash=ZERO_HASH;
        b.transactions= {
            tx
        };
        b.timestamp=0;
        b.difficulty=GENESIS_DIFFICULTY;
        b.merkle_root=merkle( {
            tid
        }
        );
        b.extra_data=GENESIS_MESSAGE;
        b.mine();
        chain= {
            b
        };
        balances["Miner_Genesis"]=BASE_REWARD;
        total_issued=BASE_REWARD;
        wallet_anchor["Miner_Genesis"]=0;
        save();
    }

void Blockchain::credit(std::map<std::string,i64>&b,const std::string&a,i64 x) {
        if(x<0 || x>MAX_SUPPLY || b[a]<0 || b[a]>MAX_SUPPLY-x)throw std::runtime_error("balance overflow");
        b[a]+=x;
    }

void Blockchain::debit(std::map<std::string,i64>&b,const std::string&a,i64 x) {
        if(x<0||b[a]<x)throw std::runtime_error("insufficient balance");
        b[a]-=x;
    }

std::pair<i64,i64> Blockchain::annual_split(i64 bal) {
        return  {
            (bal*RECEIVER_TAX_BPS)/10000,(bal*TX_BURN_BPS)/10000
        };
    }

// V17.1: apply at most Blockchain::MAX_TAX_CYCLES_PER_BLOCK (default 1) annual tax
    // cycle per wallet per block. Prevents multi-year DoS loops while remaining
    // deterministic. Missed years are collected gradually over subsequent blocks.
    void Blockchain::annual(std::map<std::string,i64>&b,std::map<std::string,i64>&a,i64&treas,i64&burn,i64 ts) {
        for(auto&[addr,anchor0]:a) {
            i64 anchor=anchor0;
            int cycles_applied=0;
            // Avoid anchor + YEAR_SECONDS overflow. Consensus uses subtraction
            // only after establishing ts >= anchor.
            while(cycles_applied<MAX_TAX_CYCLES_PER_BLOCK && ts>=anchor && (ts-anchor)>=YEAR_SECONDS) {
                i64 bal=b[addr];
                if(bal>0) {
                    auto [tt,br]=annual_split(bal);
                    if(tt<0 || br<0 || tt>bal || br>bal-tt) throw std::runtime_error("annual tax arithmetic overflow");
                    i64 tax=tt+br;
                    if(tax>bal) throw std::runtime_error("annual tax exceeds balance");
                    b[addr]=bal-tax;
                    if(treas>MAX_SUPPLY-tt || burn>MAX_SUPPLY-br) throw std::runtime_error("annual treasury/burn overflow");
                    treas+=tt;
                    burn+=br;
                }
                if(anchor>std::numeric_limits<i64>::max()-YEAR_SECONDS) {
                    anchor=std::numeric_limits<i64>::max();
                    cycles_applied++;
                    break;
                }
                anchor+=YEAR_SECONDS;
                cycles_applied++;
            }
            anchor0=anchor;
        }
    }

void Blockchain::distribute(std::map<std::string,i64>&b,i64&treas,std::map<std::string,cpp_int>&work) {
        if(treas<=0)return;
        cpp_int total=0;
        for(auto&[m,w]:work)if(w>0)total+=w;
        if(total<=0)return;
        i64 distributed=0;
        std::pair<std::string,cpp_int>winner {
            "",-1
        };
        for(auto&[m,w]:work)if(w>0) {
            if(w>winner.second||(w==winner.second&&m<winner.first))winner= {
                m,w
            };
            cpp_int share_mp=(cpp_int(treas)*w)/total;
            i64 share=share_mp.convert_to<i64>();
            credit(b,m,share);
            distributed+=share;
        }
        if(treas-distributed>0)credit(b,winner.first,treas-distributed);
        treas=0;
        work.clear();
    }

bool Blockchain::apply_tx(const Transaction&t,std::map<std::string,i64>&b,std::map<std::string,i64>&n,i64&burn,i64&treas) {
        std::string s=t.sender();
        if(n[s]!=t.nonce||b[s]<t.amount||t.nonce==std::numeric_limits<i64>::max())return false;
        const i64 net=t.net_amount(), br=t.burned(), tt=t.receiver_tax();
        if(net<0 || br<0 || tt<0 || net>t.amount || br>t.amount || tt>t.amount ||
           br>MAX_SUPPLY-burn || tt>MAX_SUPPLY-treas)return false;
        debit(b,s,t.amount);
        try { credit(b,t.recipient,net); }
        catch(...) { return false; }
        n[s]=t.nonce+1;
        burn+=br;
        treas+=tt;
        return true;
    }

bool Blockchain::validate_block(const Block&b,const Block&p,const std::vector<Block>&prefix,std::map<std::string,i64>bal,std::map<std::string,i64>nc,i64 issued,i64 burned,i64 treas,std::map<std::string,cpp_int>mw,std::map<std::string,i64>anchors,std::map<std::string,i64>*outbal,std::map<std::string,i64>*outnc,i64*outissued,i64*outburn,i64*outtreas,std::map<std::string,cpp_int>*outmw,std::map<std::string,i64>*outanchors,i64 now)const {
        if(b.index!=p.index+1||b.previous_hash!=p.block_hash||b.difficulty<1||b.difficulty>MAX_DIFFICULTY||b.difficulty!=expected_diff(prefix,b.index)||b.timestamp<mtp(prefix)||b.timestamp>now+MAX_FUTURE_BLOCK_TIME)return false;
        if(b.transactions.empty()||b.transactions.size()>MAX_TX_PER_BLOCK+MAX_SHARES_PER_BLOCK+2||b.json().size()>MAX_BLOCK_BYTES)return false;
        std::vector<std::string>ids;
        for(auto&s:b.transactions) {
            json_object*o=json_tokener_parse(s.c_str());
            if(!o)return false;
            json_object*x=nullptr;
            std::string id;
            if(json_object_object_get_ex(o,"tx_id",&x))id=json_object_get_string(x);
            else if(json_object_object_get_ex(o,"share_id",&x))id=json_object_get_string(x);
            json_object_put(o);
            if(id.empty())return false;
            ids.push_back(id);
        }
        if(b.merkle_root!=merkle(ids)||b.calc_hash()!=b.block_hash||b.block_hash.rfind(std::string(b.difficulty,'0'),0)!=0)return false;
        json_object*r=json_tokener_parse(b.transactions.back().c_str());
        if(!r)return false;
        json_object*x=nullptr;
        if(!json_object_object_get_ex(r,"type",&x)||std::string(json_object_get_string(x))!="reward") {
            json_object_put(r);
            return false;
        }
        if(!json_object_object_get_ex(r,"recipient",&x)) {
            json_object_put(r);
            return false;
        }
        std::string miner=json_object_get_string(x);
        json_object_put(r);
        if(!is_hex(miner,128))return false;
        std::set<std::string>seen;
        int shares=0; size_t releases=0;
        i64 burn=burned;
        // Deterministic Genesis-tax anchor: the first post-genesis block starts
        // the annual-tax clock for Miner_Genesis. Never use wall-clock time here.
        if(b.index==1) {
            auto it=anchors.find("Miner_Genesis");
            if(it==anchors.end() || it->second==0) anchors["Miner_Genesis"]=b.timestamp;
        }
        for(size_t i=0;i+1<b.transactions.size();++i) {
            json_object*o=json_tokener_parse(b.transactions[i].c_str());
            if(!o)return false;
            json_object*t=nullptr;
            json_object_object_get_ex(o,"type",&t);
            std::string typ=t?json_object_get_string(t):"";
            json_object_object_get_ex(o,"tx_id",&x);
            std::string id=x?json_object_get_string(x):"";
            if(id.empty()||!seen.insert(id).second) {
                json_object_put(o);
                return false;
            }
            if(typ=="transfer") {
                Transaction q;
                if(!json_get_string(o,"sender_pubkey",q.sender_pubkey) || !json_get_string(o,"recipient",q.recipient) ||
                   !json_get_i64(o,"amount",q.amount) || !json_get_i64(o,"nonce",q.nonce) ||
                   !json_get_i64(o,"timestamp",q.timestamp) || !json_get_string(o,"signature",q.signature)) {
                    json_object_put(o); return false;
                }
                q.tx_id=id;
                if(!q.valid(b.timestamp,true)||!apply_tx(q,bal,nc,burn,treas)) {
                    json_object_put(o);
                    return false;
                }
                if(!anchors.count(q.recipient))anchors[q.recipient]=b.timestamp;
            } 
            else if(typ=="mining_share") {
                if(++shares>MAX_SHARES_PER_BLOCK) {
                    json_object_put(o);
                    return false;
                }
                MiningShare s;
                if(!json_get_string(o,"miner",s.miner) || !json_get_string(o,"job_id",s.job_id) ||
                   !json_get_int(o,"difficulty",s.difficulty) || !json_get_u64(o,"nonce",s.nonce) ||
                   !json_get_string(o,"share_hash",s.share_hash) || !json_get_string(o,"share_id",s.share_id)) {
                    json_object_put(o); return false;
                }
                if(!s.valid(p.block_hash,share_diff(p.difficulty))) {
                    json_object_put(o);
                    return false;
                }
                mw[s.miner]+=block_work(s.difficulty);
            } 
            else if(typ=="l2_withdrawal_release") {
                if(++releases>MAX_L2_RELEASES_PER_BLOCK) { json_object_put(o); return false; }
                std::string claim,commitment,recipient,id; i64 amount=0;
                if(!json_get_string(o,"claim_id",claim)||!json_get_string(o,"commitment_hash",commitment)||!json_get_string(o,"recipient",recipient)||!json_get_string(o,"tx_id",id)||!json_get_i64(o,"amount",amount) || !is_hex(claim,128)||!is_hex(commitment,128)||!is_hex(recipient,128)||!is_hex(id,128)||amount<=0||amount>MAX_SUPPLY || id!=withdrawal_release_id_fields(commitment,recipient,claim,amount) || prior_release_exists(prefix,claim) || !find_finalized_withdrawal(prefix,commitment,claim,recipient,amount)) { json_object_put(o); return false; }
                if(bal[L2_BRIDGE_ADDRESS]<amount) { json_object_put(o); return false; }
                debit(bal,L2_BRIDGE_ADDRESS,amount);
                credit(bal,recipient,amount);
            }
            else if(typ=="l2_commitment") {
                if(!verify_l2_commitment(o,last_l2_state_root(prefix))) { json_object_put(o); return false; }
                if(!verify_l2_deposit_manifest(o,prefix)) { json_object_put(o); return false; }
            } 
            else  {
                json_object_put(o);
                return false;
            }
            json_object_put(o);
        }
        json_object*ro=json_tokener_parse(b.transactions.back().c_str());
        if(!ro || !json_object_is_type(ro,json_type_object)) { if(ro) json_object_put(ro); return false; }
        std::string rtype,recipient,rid; i64 amount=0,iss=0;
        bool reward_fields=json_get_string(ro,"type",rtype) && rtype=="reward" &&
            json_get_string(ro,"recipient",recipient) && json_get_string(ro,"tx_id",rid) &&
            json_get_i64(ro,"amount",amount) && json_get_i64(ro,"issuance",iss);
        i64 issuance=(issued>=MAX_SUPPLY)?0:std::min<i64>(subsidy(b.index),MAX_SUPPLY-issued);
        json_object_put(ro);
        if(!reward_fields || recipient!=miner) return false;
        if(issuance<0||amount!=issuance||iss!=issuance||rid!=hash_obj( {
             {
                "amount",std::to_string(issuance)
            }
            , {
                "block_index",std::to_string(b.index)
            }
            , {
                "issuance",std::to_string(issuance)
            }
            , {
                "recipient",json_escape(miner)
            }
            , {
                "type",json_escape("reward")
            }
        }
        ))return false;
        issued+=issuance;
        credit(bal,miner,issuance);
        if(!anchors.count(miner))anchors[miner]=b.timestamp;
        mw[miner]+=block_work(b.difficulty);
        annual(bal,anchors,treas,burn,b.timestamp);
        if(issued>MAX_SUPPLY||burn>issued||treas<0||burn<0)return false;
        if(b.index>0&&b.index%MINER_REWARD_EPOCH_BLOCKS==0)distribute(bal,treas,mw);
        if(outbal)*outbal=std::move(bal);
        if(outnc)*outnc=std::move(nc);
        if(outissued)*outissued=issued;
        if(outburn)*outburn=burn;
        if(outtreas)*outtreas=treas;
        if(outmw)*outmw=std::move(mw);
        if(outanchors)*outanchors=std::move(anchors);
        return true;
    }

bool Blockchain::validate_genesis(const Block&b) {
        if(b.index!=0 || b.previous_hash!=ZERO_HASH || b.timestamp!=0 || b.difficulty!=GENESIS_DIFFICULTY || b.nonce!=GENESIS_NONCE || b.extra_data!=GENESIS_MESSAGE || b.block_hash!=GENESIS_HASH || b.merkle_root!=GENESIS_MERKLE) return false;
        if(b.calc_hash()!=GENESIS_HASH || b.block_hash.rfind(std::string(GENESIS_DIFFICULTY,'0'),0)!=0) return false;
        if(b.transactions.size()!=1) return false;
        json_object*o=json_tokener_parse(b.transactions[0].c_str());
        if(!o || !json_object_is_type(o,json_type_object)) { if(o) json_object_put(o); return false; }
        std::string type,recipient,tid,memo; i64 amount=0;
        bool ok=json_get_string(o,"type",type) && type=="genesis" &&
                json_get_string(o,"recipient",recipient) && recipient=="Miner_Genesis" &&
                json_get_string(o,"tx_id",tid) && json_get_string(o,"memo",memo) && memo==GENESIS_MESSAGE &&
                json_get_i64(o,"amount",amount) && amount==BASE_REWARD;
        json_object_put(o);
        if(!ok) return false;
        std::string expected_tid=hash_obj({{"amount",std::to_string(BASE_REWARD)}, {"message",json_escape(GENESIS_MESSAGE)}, {"recipient",json_escape("Miner_Genesis")}});
        std::string expected_tx=canonical({{"amount",std::to_string(BASE_REWARD)}, {"memo",json_escape(GENESIS_MESSAGE)}, {"recipient",json_escape("Miner_Genesis")}, {"tx_id",json_escape(expected_tid)}, {"type",json_escape("genesis")}});
        return tid==expected_tid && merkle({tid})==GENESIS_MERKLE;
    }

bool Blockchain::replay(const std::vector<Block>&c,std::map<std::string,i64>&ob,std::map<std::string,i64>&on,i64&oi,i64&oburn,i64&ot,std::map<std::string,cpp_int>&omw,std::map<std::string,i64>&oa)const {
        if(c.empty() || !validate_genesis(c[0]))return false;
        ob= {
             {
                 {
                    "Miner_Genesis",BASE_REWARD
                }
            }
        };
        on.clear();
        oi=BASE_REWARD;
        oburn=0;
        ot=0;
        omw.clear();
        oa= {
             {
                 {
                    "Miner_Genesis",0
                }
            }
        };
        for(size_t i=1;i<c.size();++i) {
            if(!validate_block(c[i],c[i-1],std::vector<Block>(c.begin(),c.begin()+i),ob,on,oi,oburn,ot,omw,oa,&ob,&on,&oi,&oburn,&ot,&omw,&oa))return false;
        }
        return true;
    }

bool Blockchain::validate_chain(const std::vector<Block>&c)const {
        std::map<std::string,i64>b,n;
        std::map<std::string,cpp_int>m;
        std::map<std::string,i64>a;
        i64 i,br,t;
        return replay(c,b,n,i,br,t,m,a);
    }

i64 Blockchain::next_nonce(const std::string&a) {
        std::lock_guard<std::mutex>g(mu);
        i64 n=nonces[a];
        for(auto&[id,t]:pending)if(t.sender()==a)n=std::max(n,t.nonce+1);
        return n;
    }

bool Blockchain::add_transaction(const Transaction&t) {
        std::lock_guard<std::mutex>g(mu);
        if(pending.count(t.tx_id)||!t.valid())return false;
        std::map<std::string,i64>b=balances,n=nonces;
        std::vector<Transaction> pend;
        for(auto&[id,p]:pending) pend.push_back(p);
        std::sort(pend.begin(),pend.end(),[](const Transaction&a,const Transaction&b) {
            return std::make_tuple(a.timestamp,a.sender(),a.nonce,a.tx_id)<std::make_tuple(b.timestamp,b.sender(),b.nonce,b.tx_id);
        }
        );
        i64 dummy_burn=0,dummy_treas=0;
        for(auto&p:pend) if(p.sender()==t.sender() && p.nonce>=t.nonce) continue;
        else  {
            auto vb=b,vn=n;
            if(p.valid() && vn[p.sender()]==p.nonce && vb[p.sender()]>=p.amount) apply_tx(p,b,n,dummy_burn,dummy_treas);
        }
        if(n[t.sender()]!=t.nonce||b[t.sender()]<t.amount)return false;
        pending[t.tx_id]=t;
        save();
        return true;
    }

Block Blockchain::mine_external(const std::string&miner,const std::vector<std::string>&extra) {
        Block b=candidate(miner,extra); b.mine();
        std::lock_guard<std::mutex>g(mu);
        std::map<std::string,i64>bb,nn,an; std::map<std::string,cpp_int>mw;
        i64 issued,burn,treas;
        if(!validate_block(b,chain.back(),chain,balances,nonces,total_issued,total_burned,treasury_balance,miner_work,wallet_anchor,&bb,&nn,&issued,&burn,&treas,&mw,&an)) throw std::runtime_error("external block rejected");
        chain.push_back(b); balances=std::move(bb); nonces=std::move(nn); total_issued=issued; total_burned=burn; treasury_balance=treas; miner_work=std::move(mw); wallet_anchor=std::move(an);
        process_orphans();
        for(auto it=pending.begin();it!=pending.end();) {
            bool found=false; const std::string needle="\"tx_id\":"+json_escape(it->first);
            for(auto&raw:b.transactions) if(raw.find(needle)!=std::string::npos){found=true;break;}
            if(found)it=pending.erase(it); else ++it;
        }
        pending_shares.clear(); save(); return b;
    }

void Blockchain::process_orphans() {
        // Must be called with mu already locked.
        // Repeatedly attach orphan blocks that now extend the current tip.
        while(true) {
            auto candidate=orphans.end();

            // Deterministic selection if multiple orphan blocks extend
            // the same current tip: choose the lexicographically smallest
            // block hash.
            for(auto it=orphans.begin();it!=orphans.end();++it) {
                const Block&b=it->second;
                if(b.index==chain.back().index+1 && b.previous_hash==chain.back().block_hash) {
                    if(candidate==orphans.end() || it->first<candidate->first) candidate=it;
                }
            }

            if(candidate==orphans.end()) break;

            Block b=candidate->second;
            std::map<std::string,i64>bb,nn,an;
            std::map<std::string,cpp_int>mw;
            i64 issued,burn,treas;

            // If this orphan is invalid now that its parent exists,
            // permanently discard it.
            if(!validate_block(b,chain.back(),chain,balances,nonces,total_issued,total_burned,treasury_balance,miner_work,wallet_anchor,&bb,&nn,&issued,&burn,&treas,&mw,&an)) {
                orphans.erase(candidate);
                continue;
            }

            orphans.erase(candidate);
            chain.push_back(b);
            balances=std::move(bb);
            nonces=std::move(nn);
            total_issued=issued;
            total_burned=burn;
            treasury_balance=treas;
            miner_work=std::move(mw);
            wallet_anchor=std::move(an);

            // Remove transactions confirmed by the recovered block.
            for(auto it=pending.begin();it!=pending.end();) {
                bool found=false;
                const std::string needle="\"tx_id\":"+json_escape(it->first);
                for(const auto&raw:b.transactions) {
                    if(raw.find(needle)!=std::string::npos) {
                        found=true;
                        break;
                    }
                }
                if(found)it=pending.erase(it);
                else ++it;
            }
        }
    }

bool Blockchain::accept_block(const Block&b) {
        std::lock_guard<std::mutex>g(mu);
        if(b.index<=height()) return false;
        if(b.index!=height()+1 || b.previous_hash!=chain.back().block_hash) {
            if(orphans.size()>=MAX_ORPHANS) orphans.erase(orphans.begin());
            orphans[b.block_hash]=b;
            return false;
        }
        std::map<std::string,i64>bb,nn,an; std::map<std::string,cpp_int>mw;
        i64 issued,burn,treas;
        if(!validate_block(b,chain.back(),chain,balances,nonces,total_issued,total_burned,treasury_balance,miner_work,wallet_anchor,&bb,&nn,&issued,&burn,&treas,&mw,&an)) return false;
        chain.push_back(b); balances=std::move(bb); nonces=std::move(nn); total_issued=issued; total_burned=burn; treasury_balance=treas; miner_work=std::move(mw); wallet_anchor=std::move(an);
        for(auto it=pending.begin();it!=pending.end();) {
            bool found=false; const std::string needle="\"tx_id\":"+json_escape(it->first);
            for(auto&s:b.transactions) if(s.find(needle)!=std::string::npos){found=true;break;}
            if(found)it=pending.erase(it); else ++it;
        }
        process_orphans();
        save(); return true;
    }

std::vector<Block> Blockchain::snapshot_chain() const { std::lock_guard<std::mutex>g(mu); return chain; }

std::vector<Block> Blockchain::snapshot_from(size_t start,size_t max_count) const { std::lock_guard<std::mutex>g(mu); if(start>=chain.size()) return {}; size_t end=std::min(chain.size(),start+max_count); return std::vector<Block>(chain.begin()+start,chain.begin()+end); }

bool Blockchain::replace_chain(const std::vector<Block>&c) {
        std::lock_guard<std::mutex>g(mu);
        if(!validate_chain(c)||cumulative(c)<=cumulative(chain))return false;
        std::map<std::string,i64>b,n;
        std::map<std::string,cpp_int>m;
        std::map<std::string,i64>a;
        i64 i,br,t;
        if(!replay(c,b,n,i,br,t,m,a))return false;
        chain=c;
        balances=b;
        nonces=n;
        total_issued=i;
        total_burned=br;
        treasury_balance=t;
        miner_work=m;
        wallet_anchor=a;
        process_orphans();
        save();
        return true;
    }

cpp_int Blockchain::cumulative(const std::vector<Block>&c) {
        cpp_int x=0;
        for(auto&b:c)x+=block_work(b.difficulty);
        return x;
    }

void Blockchain::save()const {
        std::ofstream f(db+".tmp");
        if(!f)return;
        f<<"{\"network_id\":"<<json_escape(NETWORK_ID)<<",\"protocol_version\":"<<PROTOCOL_VERSION<<",\"chain\":[";
        for(size_t i=0;i<chain.size();++i) {
            if(i)f<<',';
            f<<chain[i].json();
        }
        f<<"],\"pending\":[";
        size_t k=0;
        for(auto&[id,t]:pending) {
            if(k++)f<<',';
            f<<t.json();
        }
        f<<"],\"pending_shares\":[";
        k=0;
        for(auto&[id,s]:pending_shares) {
            if(k++)f<<',';
            f<<s.json();
        }
        f<<"]}";
        f.flush();
        if(!f.good()) throw std::runtime_error("database write failed");
        f.close();
        // V18 durability barrier: make the temporary file durable before the
        // atomic rename. This does not make the filesystem universally
        // transactional, but substantially reduces torn-write exposure.
        if(!em_sync_file(db+".tmp")) throw std::runtime_error("database durability sync failed");
        if(!em_atomic_replace(db+".tmp",db)) throw std::runtime_error("database commit failed");
    }

bool Blockchain::load() {
        std::error_code ec;
        const auto sz=std::filesystem::file_size(db,ec);
        if(ec || sz>MAX_DB_BYTES)return false;
        std::ifstream f(db,std::ios::binary);
        if(!f)return false;
        std::string s;
        s.resize(static_cast<size_t>(sz));
        if(sz>0 && !f.read(s.data(),static_cast<std::streamsize>(sz)))return false;
        json_tokener*tok=json_tokener_new();
        if(!tok)return false;
        json_object*r=json_tokener_parse_ex(tok,s.data(),static_cast<int>(s.size()));
        enum json_tokener_error jerr=json_tokener_get_error(tok);
        json_tokener_free(tok);
        if(jerr!=json_tokener_success)return false;
        if(!r || !json_object_is_type(r,json_type_object)) { if(r) json_object_put(r); return false; }
        std::string network; i64 protocol=0;
        json_object*ch=nullptr;
        if(!json_get_string(r,"network_id",network) || network!=NETWORK_ID || !json_get_i64(r,"protocol_version",protocol) || protocol!=PROTOCOL_VERSION ||
           !json_object_object_get_ex(r,"chain",&ch) || !json_object_is_type(ch,json_type_array)) {
            json_object_put(r);
            return false;
        }
        std::vector<Block>c;
        for(size_t i=0;i<json_object_array_length(ch);++i) {
            json_object*o=json_object_array_get_idx(ch,i);
            if(!o || !json_object_is_type(o,json_type_object)) { json_object_put(r); return false; }
            Block b; std::string hash; i64 idx=0,ts=0; int diff=0; u64 nonce=0;
            if(!json_get_i64(o,"index",idx) || idx<0 || idx>INT_MAX || !json_get_string(o,"previous_hash",b.previous_hash) ||
               !json_get_i64(o,"timestamp",ts) || !json_get_u64(o,"nonce",nonce) || !json_get_int(o,"difficulty",diff) ||
               !json_get_string(o,"merkle_root",b.merkle_root) || !json_get_string(o,"extra_data",b.extra_data) ||
               !json_get_string(o,"hash",hash)) { json_object_put(r); return false; }
            json_object*x=nullptr;
            if(!json_object_object_get_ex(o,"transactions",&x) || !x || !json_object_is_type(x,json_type_array)) { json_object_put(r); return false; }
            b.index=static_cast<int>(idx); b.timestamp=ts; b.nonce=nonce; b.difficulty=diff; b.block_hash=hash;
            for(size_t j=0;j<json_object_array_length(x);++j) {
                json_object*tx=json_object_array_get_idx(x,j);
                if(!tx || !json_object_is_type(tx,json_type_object)) { json_object_put(r); return false; }
                b.transactions.push_back(json_object_to_json_string(tx));
            }
            c.push_back(std::move(b));
        }

        // Restore pending transactions and mining shares so a restart does not
        // silently discard locally accepted work. Entries are revalidated before
        // being admitted to memory and bounded by their configured capacities.
        std::map<std::string,Transaction>loaded_pending;
        json_object*pa=nullptr;
        if(json_object_object_get_ex(r,"pending",&pa)) {
            if(!pa || !json_object_is_type(pa,json_type_array) ||
               json_object_array_length(pa)>MAX_PENDING_TX) { json_object_put(r); return false; }
            for(size_t k=0;k<json_object_array_length(pa);++k) {
                json_object*o=json_object_array_get_idx(pa,k);
                if(!o || !json_object_is_type(o,json_type_object)) { json_object_put(r); return false; }
                Transaction t; i64 amount=0,nonce=0,ts=0;
                std::string recipient,pub,sig,id;
                if(!json_get_i64(o,"amount",amount)||!json_get_i64(o,"nonce",nonce)||
                   !json_get_string(o,"recipient",recipient)||!json_get_string(o,"sender_pubkey",pub)||
                   !json_get_string(o,"signature",sig)||!json_get_i64(o,"timestamp",ts)||
                   !json_get_string(o,"tx_id",id)) { json_object_put(r); return false; }
                t.amount=amount; t.nonce=nonce; t.timestamp=ts; t.recipient=recipient;
                t.sender_pubkey=pub; t.signature=sig; t.tx_id=id;
                if(!t.valid(now_s(),false) || loaded_pending.count(id)) { json_object_put(r); return false; }
                loaded_pending.emplace(id,std::move(t));
            }
        }
        std::map<std::string,MiningShare>loaded_shares;
        json_object*ps=nullptr;
        if(json_object_object_get_ex(r,"pending_shares",&ps)) {
            if(!ps || !json_object_is_type(ps,json_type_array) ||
               json_object_array_length(ps)>MAX_PENDING_SHARES) { json_object_put(r); return false; }
            for(size_t k=0;k<json_object_array_length(ps);++k) {
                json_object*o=json_object_array_get_idx(ps,k);
                if(!o || !json_object_is_type(o,json_type_object)) { json_object_put(r); return false; }
                MiningShare sh; i64 diff=0; u64 nonce=0;
                std::string miner,job,hash,id,type;
                if(!json_get_string(o,"miner",miner)||!json_get_string(o,"job_id",job)||
                   !json_get_i64(o,"difficulty",diff)||!json_get_u64(o,"nonce",nonce)||
                   !json_get_string(o,"share_hash",hash)||!json_get_string(o,"share_id",id)||
                   !json_get_string(o,"type",type) || type!="mining_share" ||
                   diff<MINING_SHARE_MIN_DIFFICULTY || diff>MAX_DIFFICULTY) { json_object_put(r); return false; }
                sh.miner=miner; sh.job_id=job; sh.difficulty=static_cast<int>(diff); sh.nonce=nonce;
                sh.share_hash=hash; sh.share_id=id;
                if(!is_hex(sh.miner,128)||!is_hex(sh.job_id,128)||!is_hex(sh.share_hash,128)||
                   !is_hex(sh.share_id,128)||loaded_shares.count(id)) { json_object_put(r); return false; }
                loaded_shares.emplace(id,std::move(sh));
            }
        }
        json_object_put(r);
        std::map<std::string,i64>b,n;
        std::map<std::string,cpp_int>m;
        std::map<std::string,i64>a;
        i64 i,br,t;
        if(c.empty()||!replay(c,b,n,i,br,t,m,a))return false;
        // Reconcile the restored mempool against the reconstructed L1 state.
        // Entries that are stale or no longer spendable are dropped instead of
        // making the entire database unbootable. Valid same-sender nonce chains
        // are admitted in deterministic order.
        std::vector<Transaction> pend;
        for(auto&[id,pt]:loaded_pending) {
            if(pt.timestamp < now_s()-MAX_TX_AGE) continue;
            pend.push_back(pt);
        }
        std::sort(pend.begin(),pend.end(),[](const Transaction&a,const Transaction&b){
            return std::make_tuple(a.timestamp,a.sender(),a.nonce,a.tx_id)<std::make_tuple(b.timestamp,b.sender(),b.nonce,b.tx_id);
        });
        std::map<std::string,i64> pb=b,pn=n;
        std::map<std::string,Transaction>reconciled;
        i64 dummy_burn=0,dummy_treas=0;
        for(auto&pt:pend) {
            auto itn=pn.find(pt.sender());
            i64 expected=(itn==pn.end()?0:itn->second);
            auto itb=pb.find(pt.sender());
            i64 avail=(itb==pb.end()?0:itb->second);
            if(pt.nonce!=expected || avail<pt.amount) continue;
            auto tb=pb,tn=pn;
            i64 xb=dummy_burn,xt=dummy_treas;
            if(apply_tx(pt,tb,tn,xb,xt)) {
                pb=std::move(tb); pn=std::move(tn);
                reconciled.emplace(pt.tx_id,pt);
            }
        }
        loaded_pending=std::move(reconciled);
        chain=c;
        balances=b;
        nonces=n;
        total_issued=i;
        total_burned=br;
        treasury_balance=t;
        miner_work=m;
        wallet_anchor=a;
        pending=std::move(loaded_pending);
        pending_shares=std::move(loaded_shares);
        return true;
    }


Block Blockchain::candidate(const std::string&miner,const std::vector<std::string>&l2
    ) {
        std::lock_guard<std::mutex>g(mu);
        Block b;
        b.index=chain.size();
        b.previous_hash=chain.back().block_hash;
        b.timestamp=std::max(now_s(),mtp(chain));
        b.difficulty=expected_diff(chain,b.index);
        b.extra_data="ELECTRIC-MONEY-PoW-"+NETWORK_ID;
        std::vector<std::string>txs;
        std::map<std::string,i64>bb=balances,nn=nonces;
        i64 burn=0,treas=0;
        std::vector<Transaction>ordered;
        for(auto&[id,t]:pending)ordered.push_back(t);
        std::sort(ordered.begin(),ordered.end(),[](const Transaction&a,const Transaction&b) {
            return std::make_tuple(a.timestamp,a.sender(),a.nonce,a.tx_id)<std::make_tuple(b.timestamp,b.sender(),b.nonce,b.tx_id);
        }
        );
        for(auto&t:ordered) {
            if(t.valid()&&nn[t.sender()]==t.nonce&&bb[t.sender()]>=t.amount&&txs.size()<MAX_TX_PER_BLOCK) {
                apply_tx(t,bb,nn,burn,treas);
                txs.push_back(canonical( {
                     {
                        "amount",std::to_string(t.amount)
                    }
                    , {
                        "nonce",std::to_string(t.nonce)
                    }
                    , {
                        "recipient",json_escape(t.recipient)
                    }
                    , {
                        "sender_pubkey",json_escape(t.sender_pubkey)
                    }
                    , {
                        "signature",json_escape(t.signature)
                    }
                    , {
                        "timestamp",std::to_string(t.timestamp)
                    }
                    , {
                        "tx_id",json_escape(t.tx_id)
                    }
                    , {
                        "type",json_escape("transfer")
                    }
                }
                ));
            }
        }
        for(auto&s:l2)txs.push_back(s);
        for(auto&[id,s]:pending_shares)if(s.valid(chain.back().block_hash,share_diff(chain.back().difficulty))&&txs.size()<MAX_TX_PER_BLOCK+MAX_SHARES_PER_BLOCK)txs.push_back(s.json());
        i64 iss=std::min<i64>(subsidy(b.index),MAX_SUPPLY-total_issued);
        std::string rid=hash_obj( {
             {
                "amount",std::to_string(iss)
            }
            , {
                "block_index",std::to_string(b.index)
            }
            , {
                "issuance",std::to_string(iss)
            }
            , {
                "recipient",json_escape(miner)
            }
            , {
                "type",json_escape("reward")
            }
        }
        );
        txs.push_back(canonical( {
             {
                "amount",std::to_string(iss)
            }
            , {
                "issuance",std::to_string(iss)
            }
            , {
                "recipient",json_escape(miner)
            }
            , {
                "tx_id",json_escape(rid)
            }
            , {
                "type",json_escape("reward")
            }
        }
        ));
        b.transactions=txs;
        std::vector<std::string>ids;
        for(auto&s:txs) {
            json_object*o=json_tokener_parse(s.c_str());
            json_object*x=nullptr;
            json_object_object_get_ex(o,"tx_id",&x);
            ids.push_back(json_object_get_string(x));
            json_object_put(o);
        }
        b.merkle_root=merkle(ids);
        return b;
    }

Block Blockchain::mine_pending(const std::string&miner,const std::function<bool()>&stop) {
        Block b=candidate(miner);
        b.mine(stop);
        std::lock_guard<std::mutex>g(mu);
        std::map<std::string,i64>bb,nn,an;
        std::map<std::string,cpp_int>mw;
        i64 issued,burn,treas;
        if(!validate_block(b,chain.back(),chain,balances,nonces,total_issued,total_burned,treasury_balance,miner_work,wallet_anchor,&bb,&nn,&issued,&burn,&treas,&mw,&an))throw std::runtime_error("self-mined block rejected");
        chain.push_back(b);
        balances=std::move(bb);
        nonces=std::move(nn);
        total_issued=issued;
        total_burned=burn;
        treasury_balance=treas;
        miner_work=std::move(mw);
        wallet_anchor=std::move(an);
        process_orphans();
        for(auto it=pending.begin();it!=pending.end();) {
            bool found=false;
            const std::string needle="\"tx_id\":"+json_escape(it->first);
            for(auto&s:b.transactions) {
                if(s.find(needle)!=std::string::npos) { found=true; break; }
            }
            if(found)it=pending.erase(it);
            else ++it;
        }
        pending_shares.clear();
        save();
        return b;
    }
