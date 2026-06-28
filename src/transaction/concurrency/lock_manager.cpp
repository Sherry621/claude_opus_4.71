/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "lock_manager.h"

bool LockManager::is_compatible(LockMode req, LockMode held) {
    if (req == LockMode::INTENTION_SHARED) {
        return held != LockMode::EXLUCSIVE;
    }
    if (req == LockMode::INTENTION_EXCLUSIVE) {
        return held == LockMode::INTENTION_SHARED || held == LockMode::INTENTION_EXCLUSIVE;
    }
    if (req == LockMode::SHARED) {
        return held == LockMode::INTENTION_SHARED || held == LockMode::SHARED;
    }
    if (req == LockMode::S_IX) {
        return held == LockMode::INTENTION_SHARED;
    }
    if (req == LockMode::EXLUCSIVE) {
        return false;
    }
    return false;
}

bool LockManager::is_stronger(LockMode held, LockMode req) {
    if (held == LockMode::EXLUCSIVE) return true;
    if (held == req) return true;
    if (held == LockMode::S_IX) {
        return req == LockMode::SHARED || req == LockMode::INTENTION_SHARED || req == LockMode::INTENTION_EXCLUSIVE;
    }
    if (held == LockMode::SHARED) {
        return req == LockMode::INTENTION_SHARED;
    }
    if (held == LockMode::INTENTION_EXCLUSIVE) {
        return req == LockMode::INTENTION_SHARED;
    }
    return false;
}

void LockManager::update_group_lock_mode(LockRequestQueue& queue) {
    if (queue.request_queue_.empty()) {
        queue.group_lock_mode_ = GroupLockMode::NON_LOCK;
        return;
    }
    bool has_x = false;
    bool has_six = false;
    bool has_s = false;
    bool has_ix = false;
    bool has_is = false;
    for (auto &req : queue.request_queue_) {
        if (req.lock_mode_ == LockMode::EXLUCSIVE) has_x = true;
        else if (req.lock_mode_ == LockMode::S_IX) has_six = true;
        else if (req.lock_mode_ == LockMode::SHARED) has_s = true;
        else if (req.lock_mode_ == LockMode::INTENTION_EXCLUSIVE) has_ix = true;
        else if (req.lock_mode_ == LockMode::INTENTION_SHARED) has_is = true;
    }
    if (has_x) queue.group_lock_mode_ = GroupLockMode::X;
    else if (has_six) queue.group_lock_mode_ = GroupLockMode::SIX;
    else if (has_s) queue.group_lock_mode_ = GroupLockMode::S;
    else if (has_ix) queue.group_lock_mode_ = GroupLockMode::IX;
    else if (has_is) queue.group_lock_mode_ = GroupLockMode::IS;
}

bool LockManager::acquire_lock(Transaction* txn, const LockDataId& lock_data_id, LockMode lock_mode) {
    std::unique_lock<std::mutex> lock(latch_);
    
    // Check 2PL state
    if (txn->get_state() == TransactionState::SHRINKING) {
        throw TransactionAbortException(txn->get_transaction_id(), AbortReason::LOCK_ON_SHIRINKING);
    }
    if (txn->get_state() == TransactionState::DEFAULT) {
        txn->set_state(TransactionState::GROWING);
    }

    auto &queue = lock_table_[lock_data_id];

    // Check if the transaction already holds this lock or a stronger lock
    LockRequest* held_req = nullptr;
    for (auto &req : queue.request_queue_) {
        if (req.txn_id_ == txn->get_transaction_id()) {
            held_req = &req;
            break;
        }
    }

    if (held_req != nullptr) {
        if (is_stronger(held_req->lock_mode_, lock_mode)) {
            return true;
        }
        // Upgrade lock!
        for (auto &req : queue.request_queue_) {
            if (req.txn_id_ != txn->get_transaction_id()) {
                if (!is_compatible(lock_mode, req.lock_mode_)) {
                    throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
                }
            }
        }
        held_req->lock_mode_ = lock_mode;
        update_group_lock_mode(queue);
        return true;
    }

    // New lock request
    for (auto &req : queue.request_queue_) {
        if (req.txn_id_ != txn->get_transaction_id()) {
            if (!is_compatible(lock_mode, req.lock_mode_)) {
                throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
            }
        }
    }

    // Add to queue and grant
    LockRequest req(txn->get_transaction_id(), lock_mode);
    req.granted_ = true;
    queue.request_queue_.push_back(req);
    update_group_lock_mode(queue);

    txn->get_lock_set()->insert(lock_data_id);

    return true;
}

bool LockManager::lock_shared_on_record(Transaction* txn, const Rid& rid, int tab_fd) {
    return acquire_lock(txn, LockDataId(tab_fd, rid, LockDataType::RECORD), LockMode::SHARED);
}

bool LockManager::lock_exclusive_on_record(Transaction* txn, const Rid& rid, int tab_fd) {
    return acquire_lock(txn, LockDataId(tab_fd, rid, LockDataType::RECORD), LockMode::EXLUCSIVE);
}

bool LockManager::lock_shared_on_table(Transaction* txn, int tab_fd) {
    return acquire_lock(txn, LockDataId(tab_fd, LockDataType::TABLE), LockMode::SHARED);
}

bool LockManager::lock_exclusive_on_table(Transaction* txn, int tab_fd) {
    return acquire_lock(txn, LockDataId(tab_fd, LockDataType::TABLE), LockMode::EXLUCSIVE);
}

bool LockManager::lock_IS_on_table(Transaction* txn, int tab_fd) {
    return acquire_lock(txn, LockDataId(tab_fd, LockDataType::TABLE), LockMode::INTENTION_SHARED);
}

bool LockManager::lock_IX_on_table(Transaction* txn, int tab_fd) {
    return acquire_lock(txn, LockDataId(tab_fd, LockDataType::TABLE), LockMode::INTENTION_EXCLUSIVE);
}

bool LockManager::unlock(Transaction* txn, LockDataId lock_data_id) {
    std::unique_lock<std::mutex> lock(latch_);

    if (txn->get_state() != TransactionState::COMMITTED && txn->get_state() != TransactionState::ABORTED) {
        txn->set_state(TransactionState::SHRINKING);
    }

    auto it = lock_table_.find(lock_data_id);
    if (it == lock_table_.end()) {
        return false;
    }

    auto &queue = it->second;
    for (auto req_it = queue.request_queue_.begin(); req_it != queue.request_queue_.end(); ++req_it) {
        if (req_it->txn_id_ == txn->get_transaction_id()) {
            queue.request_queue_.erase(req_it);
            update_group_lock_mode(queue);
            if (queue.request_queue_.empty()) {
                lock_table_.erase(it);
            }
            txn->get_lock_set()->erase(lock_data_id);
            return true;
        }
    }
    return false;
}