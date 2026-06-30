/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "transaction_manager.h"
#include "record/rm_file_handle.h"
#include "system/sm_manager.h"

std::unordered_map<txn_id_t, Transaction *> TransactionManager::txn_map = {};

/**
 * @description: 事务的开始方法
 * @return {Transaction*} 开始事务的指针
 * @param {Transaction*} txn 事务指针，空指针代表需要创建新事务，否则开始已有事务
 * @param {LogManager*} log_manager 日志管理器指针
 */
Transaction * TransactionManager::begin(Transaction* txn, LogManager* log_manager) {
    std::unique_lock<std::mutex> lock(latch_);
    if (txn == nullptr) {
        txn_id_t new_txn_id = next_txn_id_++;
        txn = new Transaction(new_txn_id);
        txn->set_start_ts(next_timestamp_++);
    }
    txn->set_state(TransactionState::DEFAULT);
    txn_map[txn->get_transaction_id()] = txn;
    if (log_manager != nullptr) {
        BeginLogRecord log_record(txn->get_transaction_id());
        log_record.prev_lsn_ = txn->get_prev_lsn();
        lsn_t lsn = log_manager->add_log_to_buffer(&log_record);
        txn->set_prev_lsn(lsn);
    }
    return txn;
}

/**
 * @description: 事务的提交方法
 * @param {Transaction*} txn 需要提交的事务
 * @param {LogManager*} log_manager 日志管理器指针
 */
void TransactionManager::commit(Transaction* txn, LogManager* log_manager) {
    if (txn == nullptr) {
        return;
    }
    if (log_manager != nullptr) {
        CommitLogRecord log_record(txn->get_transaction_id());
        log_record.prev_lsn_ = txn->get_prev_lsn();
        lsn_t lsn = log_manager->add_log_to_buffer(&log_record);
        txn->set_prev_lsn(lsn);
        log_manager->flush_log_to_disk();
    }
    // 释放该事务持有的所有锁 (拷贝 lock_set 以避免迭代器失效)
    auto lock_set = txn->get_lock_set();
    std::vector<LockDataId> locks(lock_set->begin(), lock_set->end());
    for (auto &lock_id : locks) {
        lock_manager_->unlock(txn, lock_id);
    }
    lock_set->clear();

    // 清空并释放写集中的内存
    auto write_set = txn->get_write_set();
    while (!write_set->empty()) {
        delete write_set->back();
        write_set->pop_back();
    }
    write_set->clear();
    // 更新事务状态
    txn->set_state(TransactionState::COMMITTED);
}

/**
 * @description: 事务的终止（回滚）方法
 * @param {Transaction *} txn 需要回滚的事务
 * @param {LogManager} *log_manager 日志管理器指针
 */
void TransactionManager::abort(Transaction * txn, LogManager *log_manager) {
    if (txn == nullptr) {
        return;
    }
    if (log_manager != nullptr) {
        AbortLogRecord log_record(txn->get_transaction_id());
        log_record.prev_lsn_ = txn->get_prev_lsn();
        lsn_t lsn = log_manager->add_log_to_buffer(&log_record);
        txn->set_prev_lsn(lsn);
        log_manager->flush_log_to_disk();
    }
    // 回滚写集中的所有写操作（逆序撤销）
    auto write_set = txn->get_write_set();
    Context context(lock_manager_, nullptr, txn);
    while (!write_set->empty()) {
        auto &write_record = write_set->back();
        auto &rid = write_record->GetRid();
        auto fh = sm_manager_->fhs_.at(write_record->GetTableName()).get();
        auto &tab = sm_manager_->db_.get_table(write_record->GetTableName());
        switch (write_record->GetWriteType()) {
            case WType::INSERT_TUPLE: {
                // 回滚插入：先在索引上删除对应项，再删除数据文件中的记录
                auto rec = fh->get_record(rid, &context);
                for (auto &index : tab.indexes) {
                    auto index_name = sm_manager_->get_ix_manager()->get_index_name(tab.name, index.cols);
                    auto ih = sm_manager_->ihs_.at(index_name).get();
                    char *key = new char[index.col_tot_len];
                    int offset = 0;
                    for (size_t i = 0; i < index.col_num; ++i) {
                        memcpy(key + offset, rec->data + index.cols[i].offset, index.cols[i].len);
                        offset += index.cols[i].len;
                    }
                    ih->delete_entry(key, txn);
                    delete[] key;
                }
                fh->delete_record(rid, &context);
                break;
            }
            case WType::DELETE_TUPLE: {
                // 回滚删除：重新在数据文件中插入记录，再把对应的索引条目添加回去
                fh->insert_record(rid, write_record->GetRecord().data);
                for (auto &index : tab.indexes) {
                    auto index_name = sm_manager_->get_ix_manager()->get_index_name(tab.name, index.cols);
                    auto ih = sm_manager_->ihs_.at(index_name).get();
                    char *key = new char[index.col_tot_len];
                    int offset = 0;
                    for (size_t i = 0; i < index.col_num; ++i) {
                        memcpy(key + offset, write_record->GetRecord().data + index.cols[i].offset, index.cols[i].len);
                        offset += index.cols[i].len;
                    }
                    ih->insert_entry(key, rid, txn);
                    delete[] key;
                }
                break;
            }
            case WType::UPDATE_TUPLE: {
                // 回滚更新：
                // 1. 删除更新后（新）的索引条目
                auto rec = fh->get_record(rid, &context);
                for (auto &index : tab.indexes) {
                    auto index_name = sm_manager_->get_ix_manager()->get_index_name(tab.name, index.cols);
                    auto ih = sm_manager_->ihs_.at(index_name).get();
                    char *key = new char[index.col_tot_len];
                    int offset = 0;
                    for (size_t i = 0; i < index.col_num; ++i) {
                        memcpy(key + offset, rec->data + index.cols[i].offset, index.cols[i].len);
                        offset += index.cols[i].len;
                    }
                    ih->delete_entry(key, txn);
                    delete[] key;
                }
                // 2. 更新数据文件中的记录为旧值
                fh->update_record(rid, write_record->GetRecord().data, &context);
                // 3. 将回退（旧）后的索引条目插回索引
                for (auto &index : tab.indexes) {
                    auto index_name = sm_manager_->get_ix_manager()->get_index_name(tab.name, index.cols);
                    auto ih = sm_manager_->ihs_.at(index_name).get();
                    char *key = new char[index.col_tot_len];
                    int offset = 0;
                    for (size_t i = 0; i < index.col_num; ++i) {
                        memcpy(key + offset, write_record->GetRecord().data + index.cols[i].offset, index.cols[i].len);
                        offset += index.cols[i].len;
                    }
                    ih->insert_entry(key, rid, txn);
                    delete[] key;
                }
                break;
            }
            default:
                break;
        }
        write_set->pop_back();
        delete write_record;
    }
    write_set->clear();
    // 释放该事务持有的所有锁 (拷贝 lock_set 以避免迭代器失效)
    auto lock_set = txn->get_lock_set();
    std::vector<LockDataId> locks(lock_set->begin(), lock_set->end());
    for (auto &lock_id : locks) {
        lock_manager_->unlock(txn, lock_id);
    }
    lock_set->clear();
    // 更新事务状态
    txn->set_state(TransactionState::ABORTED);
}