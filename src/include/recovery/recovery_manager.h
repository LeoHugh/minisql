#ifndef MINISQL_RECOVERY_MANAGER_H
#define MINISQL_RECOVERY_MANAGER_H

#include <map>
#include <unordered_map>
#include <vector>
#include<queue>
#include "recovery/log_rec.h"

using KvDatabase = std::unordered_map<KeyType, ValType>;
using ATT = std::unordered_map<txn_id_t, lsn_t>;

struct CheckPoint {
    lsn_t checkpoint_lsn_{INVALID_LSN};
    //当前活跃事务
    ATT active_txns_{};
    KvDatabase persist_data_{};

    inline void AddActiveTxn(txn_id_t txn_id, lsn_t last_lsn) { active_txns_[txn_id] = last_lsn; }

    inline void AddData(KeyType key, ValType val) { persist_data_.emplace(std::move(key), val); }
};

class RecoveryManager {
public:
    /**
    * TODO: Student Implement
    */
    void Init(CheckPoint &last_checkpoint) {
        persist_lsn_ = last_checkpoint.checkpoint_lsn_;
        active_txns_ = last_checkpoint.active_txns_;
        data_ = last_checkpoint.persist_data_;
    }

    /**
    * TODO: Student Implement
    */
    void RedoPhase() {
        for (const auto &[lsn, log_rec] : log_recs_) {
            if (lsn < persist_lsn_) {
                continue;
            }

            switch (log_rec->type_) {
                case LogRecType::kBegin:
                    active_txns_[log_rec->txn_id_] = log_rec->lsn_;
                    break;
                case LogRecType::kInsert:
                    data_[log_rec->new_key_] = log_rec->new_val_;
                    active_txns_[log_rec->txn_id_] = log_rec->lsn_;
                    break;
                case LogRecType::kDelete:
                //如果本来就没有，是安全的
                    data_.erase(log_rec->old_key_);
                    active_txns_[log_rec->txn_id_] = log_rec->lsn_;
                    break;
                case LogRecType::kUpdate:
                    if (log_rec->old_key_ != log_rec->new_key_) {
                        data_.erase(log_rec->old_key_);
                    }
                    data_[log_rec->new_key_] = log_rec->new_val_;
                    active_txns_[log_rec->txn_id_] = log_rec->lsn_;
                    break;
                case LogRecType::kCommit:
                    active_txns_.erase(log_rec->txn_id_);
                    break;
                case LogRecType::kAbort: {
                    lsn_t undo_lsn = log_rec->prev_lsn_;
                    while (undo_lsn != INVALID_LSN) {
                        auto undo_it = log_recs_.find(undo_lsn);
                        if (undo_it == log_recs_.end()) {
                            break;
                        }
                        ApplyUndo(undo_it->second);
                        undo_lsn = undo_it->second->prev_lsn_;
                    }
                    active_txns_.erase(log_rec->txn_id_);
                    break;
                }
                case LogRecType::kInvalid:
                default:
                    break;
            }
        }
    }

    /**
    * TODO: Student Implement
    */
    void UndoPhase() {
        std::priority_queue<lsn_t> pq;
        for (const auto &[txn_id, lsn] : active_txns_) {
        pq.push(lsn);
    }

    // 2. 每次取出最大的 LSN（全局最后发生的操作），执行 Undo
    while (!pq.empty()) {
        lsn_t max_lsn = pq.top();
        pq.pop();

        auto undo_it = log_recs_.find(max_lsn);
        if (undo_it == log_recs_.end()) {
            continue;
        }

        // 撤销该操作
        ApplyUndo(undo_it->second);

        // 3. 如果该事务还有上一条日志，把 prev_lsn_ 塞回堆里，参与下一轮比较
        lsn_t prev = undo_it->second->prev_lsn_;
        if (prev != INVALID_LSN) {
            pq.push(prev);
        }
        }
    
        // 清空活跃事务表
        active_txns_.clear();
    }

    // used for test only
    void AppendLogRec(LogRecPtr log_rec) { log_recs_.emplace(log_rec->lsn_, log_rec); }

    // used for test only
    inline KvDatabase &GetDatabase() { return data_; }

private:
    //辅助方法，处理每个reocrd在database
    inline void ApplyUndo(const LogRecPtr &log_rec) {
        switch (log_rec->type_) {
            case LogRecType::kInsert:
                data_.erase(log_rec->new_key_);
                break;
            case LogRecType::kDelete:
                data_[log_rec->old_key_] = log_rec->old_val_;
                break;
            case LogRecType::kUpdate:
                if (log_rec->old_key_ != log_rec->new_key_) {
                    data_.erase(log_rec->new_key_);
                }
                data_[log_rec->old_key_] = log_rec->old_val_;
                break;
            case LogRecType::kBegin:
            case LogRecType::kCommit:
            case LogRecType::kAbort:
            case LogRecType::kInvalid:
            default:
                break;
        }
    }
    //存储所有日志
    std::map<lsn_t, LogRecPtr> log_recs_{};
    //记录持久化的起点
    lsn_t persist_lsn_{INVALID_LSN};
    //维护当前活跃事务表
    ATT active_txns_{};
    //全量快照
    KvDatabase data_{};  // all data in database
};

#endif  // MINISQL_RECOVERY_MANAGER_H
