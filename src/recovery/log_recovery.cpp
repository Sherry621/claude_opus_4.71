/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "log_recovery.h"
#include "record/rm.h"

/**
 * @description: analyze阶段，需要获得脏页表（DPT）和未完成的事务列表（ATT）
 */
void RecoveryManager::analyze() {
    // Read entire log file at once for performance
    int file_size = disk_manager_->get_file_size(LOG_FILE_NAME);
    if (file_size <= 0) return;
    
    char* all_data = new char[file_size];
    int bytes_read = disk_manager_->read_log(all_data, file_size, 0);
    if (bytes_read <= 0) {
        delete[] all_data;
        return;
    }
    
    int offset = 0;
    lsn_t max_lsn = INVALID_LSN;
    
    while (offset + LOG_HEADER_SIZE <= bytes_read) {
        LogType log_type = *reinterpret_cast<const LogType*>(all_data + offset);
        uint32_t log_tot_len = *reinterpret_cast<const uint32_t*>(all_data + offset + OFFSET_LOG_TOT_LEN);
        txn_id_t log_tid = *reinterpret_cast<const txn_id_t*>(all_data + offset + OFFSET_LOG_TID);
        
        if (log_tot_len < LOG_HEADER_SIZE || offset + (int)log_tot_len > bytes_read) {
            break;
        }
        
        std::shared_ptr<LogRecord> log_rec = nullptr;
        switch (log_type) {
            case LogType::begin:
                log_rec = std::make_shared<BeginLogRecord>();
                break;
            case LogType::commit:
                log_rec = std::make_shared<CommitLogRecord>();
                break;
            case LogType::ABORT:
                log_rec = std::make_shared<AbortLogRecord>();
                break;
            case LogType::INSERT:
                log_rec = std::make_shared<InsertLogRecord>();
                break;
            case LogType::DELETE:
                log_rec = std::make_shared<DeleteLogRecord>();
                break;
            case LogType::UPDATE:
                log_rec = std::make_shared<UpdateLogRecord>();
                break;
            default:
                break;
        }
        
        if (log_rec != nullptr) {
            log_rec->deserialize(all_data + offset);
            log_records_.push_back(log_rec);
            
            if (log_rec->lsn_ > max_lsn) {
                max_lsn = log_rec->lsn_;
            }
            
            if (log_type == LogType::begin) {
                active_txns_.insert(log_tid);
            } else if (log_type == LogType::commit) {
                active_txns_.erase(log_tid);
                committed_txns_.insert(log_tid);
            } else if (log_type == LogType::ABORT) {
                active_txns_.erase(log_tid);
                aborted_txns_.insert(log_tid);
            } else {
                if (committed_txns_.find(log_tid) == committed_txns_.end() &&
                    aborted_txns_.find(log_tid) == aborted_txns_.end()) {
                    active_txns_.insert(log_tid);
                }
            }
        }
        
        offset += log_tot_len;
    }
    
    delete[] all_data;
    
    if (log_manager_ != nullptr && max_lsn != INVALID_LSN) {
        log_manager_->set_global_lsn(max_lsn + 1);
        log_manager_->set_persist_lsn(max_lsn);
    }
}

void RecoveryManager::redo() {
    for (auto& log_rec : log_records_) {
        if (log_rec->log_type_ == LogType::INSERT) {
            auto insert_rec = std::dynamic_pointer_cast<InsertLogRecord>(log_rec);
            std::string tab_name(insert_rec->table_name_, insert_rec->table_name_size_);
            
            if (sm_manager_->fhs_.find(tab_name) == sm_manager_->fhs_.end()) continue;
            auto fh = sm_manager_->fhs_[tab_name].get();
            
            // Extend file if needed
            while (insert_rec->rid_.page_no >= fh->file_hdr_.num_pages) {
                PageId new_page_id = {fh->fd_, INVALID_PAGE_ID};
                Page* new_page = buffer_pool_manager_->new_page(&new_page_id);
                if (new_page == nullptr) break;
                RmPageHandle page_handle(&fh->file_hdr_, new_page);
                Bitmap::init(page_handle.bitmap, fh->file_hdr_.bitmap_size);
                page_handle.page_hdr->num_records = 0;
                page_handle.page_hdr->next_free_page_no = RM_NO_PAGE;
                fh->file_hdr_.num_pages++;
                buffer_pool_manager_->unpin_page(new_page_id, true);
            }
            
            Page* page = buffer_pool_manager_->fetch_page(PageId{fh->GetFd(), insert_rec->rid_.page_no});
            if (page == nullptr) continue;
            lsn_t page_lsn = page->get_page_lsn();
            if (page_lsn < insert_rec->lsn_) {
                buffer_pool_manager_->unpin_page(page->get_page_id(), false);
                
                fh->insert_record(insert_rec->rid_, insert_rec->insert_value_.data);
                
                page = buffer_pool_manager_->fetch_page(PageId{fh->GetFd(), insert_rec->rid_.page_no});
                page->set_page_lsn(insert_rec->lsn_);
                buffer_pool_manager_->unpin_page(page->get_page_id(), true);
                
                if (sm_manager_->db_.is_table(tab_name)) {
                    auto &tab = sm_manager_->db_.get_table(tab_name);
                    for (auto &index : tab.indexes) {
                        auto index_name = sm_manager_->get_ix_manager()->get_index_name(tab_name, index.cols);
                        if (sm_manager_->ihs_.find(index_name) == sm_manager_->ihs_.end()) continue;
                        auto ih = sm_manager_->ihs_.at(index_name).get();
                        char *key = new char[index.col_tot_len];
                        int offset = 0;
                        for (int i = 0; i < index.col_num; ++i) {
                            memcpy(key + offset, insert_rec->insert_value_.data + index.cols[i].offset, index.cols[i].len);
                            offset += index.cols[i].len;
                        }
                        try { ih->insert_entry(key, insert_rec->rid_, nullptr); } catch (...) {}
                        delete[] key;
                    }
                }
            } else {
                buffer_pool_manager_->unpin_page(page->get_page_id(), false);
            }
        } 
        else if (log_rec->log_type_ == LogType::DELETE) {
            auto delete_rec = std::dynamic_pointer_cast<DeleteLogRecord>(log_rec);
            std::string tab_name(delete_rec->table_name_, delete_rec->table_name_size_);
            
            if (sm_manager_->fhs_.find(tab_name) == sm_manager_->fhs_.end()) continue;
            auto fh = sm_manager_->fhs_[tab_name].get();
            
            Page* page = buffer_pool_manager_->fetch_page(PageId{fh->GetFd(), delete_rec->rid_.page_no});
            if (page == nullptr) continue;
            lsn_t page_lsn = page->get_page_lsn();
            if (page_lsn < delete_rec->lsn_) {
                buffer_pool_manager_->unpin_page(page->get_page_id(), false);
                
                try { fh->delete_record(delete_rec->rid_, nullptr); } catch (...) {}
                
                page = buffer_pool_manager_->fetch_page(PageId{fh->GetFd(), delete_rec->rid_.page_no});
                page->set_page_lsn(delete_rec->lsn_);
                buffer_pool_manager_->unpin_page(page->get_page_id(), true);
                
                if (sm_manager_->db_.is_table(tab_name)) {
                    auto &tab = sm_manager_->db_.get_table(tab_name);
                    for (auto &index : tab.indexes) {
                        auto index_name = sm_manager_->get_ix_manager()->get_index_name(tab_name, index.cols);
                        if (sm_manager_->ihs_.find(index_name) == sm_manager_->ihs_.end()) continue;
                        auto ih = sm_manager_->ihs_.at(index_name).get();
                        char *key = new char[index.col_tot_len];
                        int offset = 0;
                        for (int i = 0; i < index.col_num; ++i) {
                            memcpy(key + offset, delete_rec->delete_value_.data + index.cols[i].offset, index.cols[i].len);
                            offset += index.cols[i].len;
                        }
                        try { ih->delete_entry(key, nullptr); } catch (...) {}
                        delete[] key;
                    }
                }
            } else {
                buffer_pool_manager_->unpin_page(page->get_page_id(), false);
            }
        } 
        else if (log_rec->log_type_ == LogType::UPDATE) {
            auto update_rec = std::dynamic_pointer_cast<UpdateLogRecord>(log_rec);
            std::string tab_name(update_rec->table_name_, update_rec->table_name_size_);
            
            if (sm_manager_->fhs_.find(tab_name) == sm_manager_->fhs_.end()) continue;
            auto fh = sm_manager_->fhs_[tab_name].get();
            
            Page* page = buffer_pool_manager_->fetch_page(PageId{fh->GetFd(), update_rec->rid_.page_no});
            if (page == nullptr) continue;
            lsn_t page_lsn = page->get_page_lsn();
            if (page_lsn < update_rec->lsn_) {
                buffer_pool_manager_->unpin_page(page->get_page_id(), false);
                
                fh->update_record(update_rec->rid_, update_rec->new_value_.data, nullptr);
                
                page = buffer_pool_manager_->fetch_page(PageId{fh->GetFd(), update_rec->rid_.page_no});
                page->set_page_lsn(update_rec->lsn_);
                buffer_pool_manager_->unpin_page(page->get_page_id(), true);
                
                if (sm_manager_->db_.is_table(tab_name)) {
                    auto &tab = sm_manager_->db_.get_table(tab_name);
                    for (auto &index : tab.indexes) {
                        auto index_name = sm_manager_->get_ix_manager()->get_index_name(tab_name, index.cols);
                        if (sm_manager_->ihs_.find(index_name) == sm_manager_->ihs_.end()) continue;
                        auto ih = sm_manager_->ihs_.at(index_name).get();
                        
                        char *old_key = new char[index.col_tot_len];
                        int offset = 0;
                        for (int i = 0; i < index.col_num; ++i) {
                            memcpy(old_key + offset, update_rec->old_value_.data + index.cols[i].offset, index.cols[i].len);
                            offset += index.cols[i].len;
                        }
                        try { ih->delete_entry(old_key, nullptr); } catch (...) {}
                        delete[] old_key;
                        
                        char *new_key = new char[index.col_tot_len];
                        offset = 0;
                        for (int i = 0; i < index.col_num; ++i) {
                            memcpy(new_key + offset, update_rec->new_value_.data + index.cols[i].offset, index.cols[i].len);
                            offset += index.cols[i].len;
                        }
                        try { ih->insert_entry(new_key, update_rec->rid_, nullptr); } catch (...) {}
                        delete[] new_key;
                    }
                }
            } else {
                buffer_pool_manager_->unpin_page(page->get_page_id(), false);
            }
        }
    }
}

void RecoveryManager::undo() {
    for (auto it = log_records_.rbegin(); it != log_records_.rend(); ++it) {
        auto log_rec = *it;
        if (active_txns_.find(log_rec->log_tid_) == active_txns_.end() &&
            aborted_txns_.find(log_rec->log_tid_) == aborted_txns_.end()) {
            continue;
        }
        
        if (log_rec->log_type_ == LogType::INSERT) {
            auto insert_rec = std::dynamic_pointer_cast<InsertLogRecord>(log_rec);
            std::string tab_name(insert_rec->table_name_, insert_rec->table_name_size_);
            if (sm_manager_->fhs_.find(tab_name) == sm_manager_->fhs_.end()) continue;
            auto fh = sm_manager_->fhs_.at(tab_name).get();
            
            if (fh->is_record(insert_rec->rid_)) {
                fh->delete_record(insert_rec->rid_, nullptr);
                if (sm_manager_->db_.is_table(tab_name)) {
                    auto &tab = sm_manager_->db_.get_table(tab_name);
                    for (auto &index : tab.indexes) {
                        auto index_name = sm_manager_->get_ix_manager()->get_index_name(tab_name, index.cols);
                        if (sm_manager_->ihs_.find(index_name) == sm_manager_->ihs_.end()) continue;
                        auto ih = sm_manager_->ihs_.at(index_name).get();
                        char *key = new char[index.col_tot_len];
                        int offset = 0;
                        for (int i = 0; i < index.col_num; ++i) {
                            memcpy(key + offset, insert_rec->insert_value_.data + index.cols[i].offset, index.cols[i].len);
                            offset += index.cols[i].len;
                        }
                        try { ih->delete_entry(key, nullptr); } catch (...) {}
                        delete[] key;
                    }
                }
            }
        } 
        else if (log_rec->log_type_ == LogType::DELETE) {
            auto delete_rec = std::dynamic_pointer_cast<DeleteLogRecord>(log_rec);
            std::string tab_name(delete_rec->table_name_, delete_rec->table_name_size_);
            if (sm_manager_->fhs_.find(tab_name) == sm_manager_->fhs_.end()) continue;
            auto fh = sm_manager_->fhs_.at(tab_name).get();
            
            if (!fh->is_record(delete_rec->rid_)) {
                fh->insert_record(delete_rec->rid_, delete_rec->delete_value_.data);
                if (sm_manager_->db_.is_table(tab_name)) {
                    auto &tab = sm_manager_->db_.get_table(tab_name);
                    for (auto &index : tab.indexes) {
                        auto index_name = sm_manager_->get_ix_manager()->get_index_name(tab_name, index.cols);
                        if (sm_manager_->ihs_.find(index_name) == sm_manager_->ihs_.end()) continue;
                        auto ih = sm_manager_->ihs_.at(index_name).get();
                        char *key = new char[index.col_tot_len];
                        int offset = 0;
                        for (int i = 0; i < index.col_num; ++i) {
                            memcpy(key + offset, delete_rec->delete_value_.data + index.cols[i].offset, index.cols[i].len);
                            offset += index.cols[i].len;
                        }
                        try { ih->insert_entry(key, delete_rec->rid_, nullptr); } catch (...) {}
                        delete[] key;
                    }
                }
            }
        } 
        else if (log_rec->log_type_ == LogType::UPDATE) {
            auto update_rec = std::dynamic_pointer_cast<UpdateLogRecord>(log_rec);
            std::string tab_name(update_rec->table_name_, update_rec->table_name_size_);
            if (sm_manager_->fhs_.find(tab_name) == sm_manager_->fhs_.end()) continue;
            auto fh = sm_manager_->fhs_.at(tab_name).get();
            
            if (fh->is_record(update_rec->rid_)) {
                fh->update_record(update_rec->rid_, update_rec->old_value_.data, nullptr);
                if (sm_manager_->db_.is_table(tab_name)) {
                    auto &tab = sm_manager_->db_.get_table(tab_name);
                    for (auto &index : tab.indexes) {
                        auto index_name = sm_manager_->get_ix_manager()->get_index_name(tab_name, index.cols);
                        if (sm_manager_->ihs_.find(index_name) == sm_manager_->ihs_.end()) continue;
                        auto ih = sm_manager_->ihs_.at(index_name).get();
                        
                        char *new_key = new char[index.col_tot_len];
                        int offset = 0;
                        for (int i = 0; i < index.col_num; ++i) {
                            memcpy(new_key + offset, update_rec->new_value_.data + index.cols[i].offset, index.cols[i].len);
                            offset += index.cols[i].len;
                        }
                        try { ih->delete_entry(new_key, nullptr); } catch (...) {}
                        delete[] new_key;
                        
                        char *old_key = new char[index.col_tot_len];
                        offset = 0;
                        for (int i = 0; i < index.col_num; ++i) {
                            memcpy(old_key + offset, update_rec->old_value_.data + index.cols[i].offset, index.cols[i].len);
                            offset += index.cols[i].len;
                        }
                        try { ih->insert_entry(old_key, update_rec->rid_, nullptr); } catch (...) {}
                        delete[] old_key;
                    }
                }
            }
        }
    }
    
    // Reconstruct the free page list for each table to ensure correctness of first_free_page_no
    for (auto &entry : sm_manager_->fhs_) {
        auto fh = entry.second.get();
        fh->file_hdr_.first_free_page_no = RM_NO_PAGE;
        for (int page_no = 1; page_no < fh->file_hdr_.num_pages; ++page_no) {
            Page *page = buffer_pool_manager_->fetch_page(PageId{fh->GetFd(), page_no});
            if (page == nullptr) continue;
            
            RmPageHandle page_handle(&fh->file_hdr_, page);
            int num_records = 0;
            for (int slot_no = 0; slot_no < fh->file_hdr_.num_records_per_page; ++slot_no) {
                if (Bitmap::is_set(page_handle.bitmap, slot_no)) {
                    num_records++;
                }
            }
            page_handle.page_hdr->num_records = num_records;
            
            if (num_records < fh->file_hdr_.num_records_per_page) {
                page_handle.page_hdr->next_free_page_no = fh->file_hdr_.first_free_page_no;
                fh->file_hdr_.first_free_page_no = page_no;
                buffer_pool_manager_->unpin_page(page->get_page_id(), true);
            } else {
                page_handle.page_hdr->next_free_page_no = RM_NO_PAGE;
                buffer_pool_manager_->unpin_page(page->get_page_id(), true);
            }
        }
        disk_manager_->set_fd2pageno(fh->GetFd(), fh->file_hdr_.num_pages);
    }

    // Rebuild all B+ tree indexes from scratch to guarantee correctness and consistency
    for (auto &tab_entry : sm_manager_->db_.tabs_) {
        std::string tab_name = tab_entry.first;
        auto &tab = tab_entry.second;
        if (sm_manager_->fhs_.find(tab_name) == sm_manager_->fhs_.end()) continue;
        auto fh = sm_manager_->fhs_.at(tab_name).get();
        
        for (auto &index : tab.indexes) {
            auto index_name = sm_manager_->get_ix_manager()->get_index_name(tab_name, index.cols);
            
            auto ih_pos = sm_manager_->ihs_.find(index_name);
            if (ih_pos != sm_manager_->ihs_.end()) {
                sm_manager_->get_ix_manager()->close_index(ih_pos->second.get());
                sm_manager_->ihs_.erase(ih_pos);
            }
            
            try {
                sm_manager_->get_ix_manager()->destroy_index(tab_name, index.cols);
            } catch (...) {}
            
            sm_manager_->get_ix_manager()->create_index(tab_name, index.cols);
            auto ih = sm_manager_->get_ix_manager()->open_index(tab_name, index.cols);
            
            char *key = new char[index.col_tot_len];
            for (RmScan rm_scan(fh); !rm_scan.is_end(); rm_scan.next()) {
                auto rec = fh->get_record(rm_scan.rid(), nullptr);
                int offset = 0;
                for (int i = 0; i < index.col_num; ++i) {
                    memcpy(key + offset, rec->data + index.cols[i].offset, index.cols[i].len);
                    offset += index.cols[i].len;
                }
                ih->insert_entry(key, rm_scan.rid(), nullptr);
            }
            delete[] key;
            
            sm_manager_->ihs_.emplace(index_name, std::move(ih));
        }
    }

    // Write all file headers and flush all modified pages to disk
    for (auto &entry : sm_manager_->fhs_) {
        auto fh = entry.second.get();
        disk_manager_->write_page(fh->GetFd(), RM_FILE_HDR_PAGE, (char *)&fh->file_hdr_, sizeof(fh->file_hdr_));
        buffer_pool_manager_->flush_all_pages(fh->GetFd());
    }
    for (auto &entry : sm_manager_->ihs_) {
        auto ih = entry.second.get();
        char* data = new char[ih->file_hdr_->tot_len_];
        ih->file_hdr_->serialize(data);
        disk_manager_->write_page(ih->get_fd(), IX_FILE_HDR_PAGE, data, ih->file_hdr_->tot_len_);
        delete[] data;
        buffer_pool_manager_->flush_all_pages(ih->get_fd());
    }
}