/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "ix_index_handle.h"

#include "ix_scan.h"

/**
 * @brief 在当前node中查找第一个>=target的key_idx
 *
 * @return key_idx，范围为[0,num_key)，如果返回的key_idx=num_key，则表示target大于最后一个key
 * @note 返回key index（同时也是rid index），作为slot no
 */
int IxNodeHandle::lower_bound(const char *target) const {
    int left = 0, right = page_hdr->num_key;
    while (left < right) {
        int mid = (left + right) / 2;
        if (ix_compare(get_key(mid), target, file_hdr->col_types_, file_hdr->col_lens_) < 0) {
            left = mid + 1;
        } else {
            right = mid;
        }
    }
    return left;
}

/**
 * @brief 在当前node中查找第一个>target的key_idx
 *
 * @return key_idx，范围为[1,num_key)，如果返回的key_idx=num_key，则表示target大于等于最后一个key
 * @note 注意此处的范围从1开始（用于内部节点查找，key[0]为最小哨兵）
 */
int IxNodeHandle::upper_bound(const char *target) const {
    int left = 1, right = page_hdr->num_key;
    while (left < right) {
        int mid = (left + right) / 2;
        if (ix_compare(get_key(mid), target, file_hdr->col_types_, file_hdr->col_lens_) <= 0) {
            left = mid + 1;
        } else {
            right = mid;
        }
    }
    return left;
}

/**
 * @brief 用于叶子结点根据key来查找该结点中的键值对
 * @return 目标key是否存在
 */
bool IxNodeHandle::leaf_lookup(const char *key, Rid **value) {
    int pos = lower_bound(key);
    if (pos == page_hdr->num_key ||
        ix_compare(get_key(pos), key, file_hdr->col_types_, file_hdr->col_lens_) != 0) {
        return false;
    }
    *value = get_rid(pos);
    return true;
}

/**
 * 用于内部结点（非叶子节点）查找目标key所在的孩子结点（子树）
 * @return page_id_t 目标key所在的孩子节点（子树）的存储页面编号
 */
page_id_t IxNodeHandle::internal_lookup(const char *key) {
    int pos = upper_bound(key);
    return value_at(pos - 1);
}

/**
 * @brief 在指定位置插入n个连续的键值对
 */
void IxNodeHandle::insert_pairs(int pos, const char *key, const Rid *rid, int n) {
    if (pos < 0 || pos > page_hdr->num_key) return;
    int num = page_hdr->num_key;
    int klen = file_hdr->col_tot_len_;
    // 将[pos, num)的key和rid整体后移n位
    memmove(get_key(pos + n), get_key(pos), (num - pos) * klen);
    memmove(get_rid(pos + n), get_rid(pos), (num - pos) * sizeof(Rid));
    // 将待插入的n个key/rid拷贝到pos位置
    memcpy(get_key(pos), key, n * klen);
    memcpy(get_rid(pos), rid, n * sizeof(Rid));
    page_hdr->num_key += n;
}

/**
 * @brief 用于在结点中插入单个键值对，返回插入后的键值对数量
 */
int IxNodeHandle::insert(const char *key, const Rid &value) {
    int pos = lower_bound(key);
    // key重复则不插入
    if (pos < page_hdr->num_key &&
        ix_compare(get_key(pos), key, file_hdr->col_types_, file_hdr->col_lens_) == 0) {
        return page_hdr->num_key;
    }
    insert_pair(pos, key, value);
    return page_hdr->num_key;
}

/**
 * @brief 用于在结点中的指定位置删除单个键值对
 */
void IxNodeHandle::erase_pair(int pos) {
    if (pos < 0 || pos >= page_hdr->num_key) return;
    int num = page_hdr->num_key;
    int klen = file_hdr->col_tot_len_;
    memmove(get_key(pos), get_key(pos + 1), (num - pos - 1) * klen);
    memmove(get_rid(pos), get_rid(pos + 1), (num - pos - 1) * sizeof(Rid));
    page_hdr->num_key--;
}

/**
 * @brief 用于在结点中删除指定key的键值对。返回删除后的键值对数量
 */
int IxNodeHandle::remove(const char *key) {
    int pos = lower_bound(key);
    if (pos < page_hdr->num_key &&
        ix_compare(get_key(pos), key, file_hdr->col_types_, file_hdr->col_lens_) == 0) {
        erase_pair(pos);
    }
    return page_hdr->num_key;
}

IxIndexHandle::IxIndexHandle(DiskManager *disk_manager, BufferPoolManager *buffer_pool_manager, int fd)
    : disk_manager_(disk_manager), buffer_pool_manager_(buffer_pool_manager), fd_(fd) {
    // init file_hdr_
    disk_manager_->read_page(fd, IX_FILE_HDR_PAGE, (char *)&file_hdr_, sizeof(file_hdr_));
    char* buf = new char[PAGE_SIZE];
    memset(buf, 0, PAGE_SIZE);
    disk_manager_->read_page(fd, IX_FILE_HDR_PAGE, buf, PAGE_SIZE);
    file_hdr_ = new IxFileHdr();
    file_hdr_->deserialize(buf);
    delete[] buf;

    // disk_manager管理的fd对应的文件中，设置从file_hdr_->num_pages开始分配page_no
    int now_page_no = disk_manager_->get_fd2pageno(fd);
    disk_manager_->set_fd2pageno(fd, now_page_no + 1);
}

/**
 * @brief 用于查找指定键所在的叶子结点
 * @note need to Unlatch and unpin the leaf node outside!
 */
std::pair<IxNodeHandle *, bool> IxIndexHandle::find_leaf_page(const char *key, Operation operation,
                                                            Transaction *transaction, bool find_first) {
    IxNodeHandle *node = fetch_node(file_hdr_->root_page_);
    while (!node->is_leaf_page()) {
        page_id_t child_page_no;
        if (find_first) {
            child_page_no = node->value_at(0);
        } else {
            child_page_no = node->internal_lookup(key);
        }
        buffer_pool_manager_->unpin_page(node->get_page_id(), false);
        delete node;
        node = fetch_node(child_page_no);
    }
    return std::make_pair(node, false);
}

/**
 * @brief 用于查找指定键在叶子结点中的对应的值result
 * @return bool 返回目标键值对是否存在
 */
bool IxIndexHandle::get_value(const char *key, std::vector<Rid> *result, Transaction *transaction) {
    std::scoped_lock lock{root_latch_};
    auto [leaf, root_is_latched] = find_leaf_page(key, Operation::FIND, transaction);
    Rid *rid = nullptr;
    bool found = leaf->leaf_lookup(key, &rid);
    if (found) {
        result->push_back(*rid);
    }
    buffer_pool_manager_->unpin_page(leaf->get_page_id(), false);
    delete leaf;
    return found;
}

/**
 * @brief 将传入的一个node拆分(Split)成两个结点，在node的右边生成一个新结点new node
 * @note need to unpin the new node outside
 */
IxNodeHandle *IxIndexHandle::split(IxNodeHandle *node) {
    IxNodeHandle *new_node = create_node();
    // 初始化新节点的page_hdr
    new_node->page_hdr->is_leaf = node->page_hdr->is_leaf;
    new_node->page_hdr->num_key = 0;
    new_node->page_hdr->parent = node->page_hdr->parent;
    new_node->page_hdr->next_free_page_no = IX_NO_PAGE;

    int pos = node->get_size() / 2;
    int num_move = node->get_size() - pos;
    // 将原结点右半部分键值对移动到新结点
    new_node->insert_pairs(0, node->get_key(pos), node->get_rid(pos), num_move);
    node->set_size(pos);

    if (node->is_leaf_page()) {
        // 更新叶子结点的前驱/后继指针
        new_node->set_prev_leaf(node->get_page_no());
        new_node->set_next_leaf(node->get_next_leaf());
        IxNodeHandle *old_next = fetch_node(node->get_next_leaf());
        old_next->set_prev_leaf(new_node->get_page_no());
        buffer_pool_manager_->unpin_page(old_next->get_page_id(), true);
        delete old_next;
        node->set_next_leaf(new_node->get_page_no());
    } else {
        // 更新新结点所有孩子的父指针
        for (int i = 0; i < new_node->get_size(); i++) {
            maintain_child(new_node, i);
        }
    }
    return new_node;
}

/**
 * @brief 拆分(Split)后，向上将new_node的第一个key插入到父结点
 * @note 本函数执行完毕后，new node和old node都需要在函数外面进行unpin
 */
void IxIndexHandle::insert_into_parent(IxNodeHandle *old_node, const char *key, IxNodeHandle *new_node,
                                     Transaction *transaction) {
    if (old_node->is_root_page()) {
        // 原结点是根结点，需要新建根结点
        IxNodeHandle *new_root = create_node();
        new_root->page_hdr->is_leaf = false;
        new_root->page_hdr->num_key = 0;
        new_root->page_hdr->parent = IX_NO_PAGE;
        new_root->page_hdr->next_free_page_no = IX_NO_PAGE;
        new_root->insert_pair(0, old_node->get_key(0), Rid{old_node->get_page_no(), -1});
        new_root->insert_pair(1, key, Rid{new_node->get_page_no(), -1});

        update_root_page_no(new_root->get_page_no());
        old_node->set_parent_page_no(new_root->get_page_no());
        new_node->set_parent_page_no(new_root->get_page_no());
        buffer_pool_manager_->unpin_page(new_root->get_page_id(), true);
        delete new_root;
    } else {
        IxNodeHandle *parent = fetch_node(old_node->get_parent_page_no());
        int child_idx = parent->find_child(old_node);
        parent->insert_pair(child_idx + 1, key, Rid{new_node->get_page_no(), -1});
        new_node->set_parent_page_no(parent->get_page_no());
        if (parent->get_size() >= parent->get_max_size()) {
            IxNodeHandle *new_parent = split(parent);
            insert_into_parent(parent, new_parent->get_key(0), new_parent, transaction);
            buffer_pool_manager_->unpin_page(new_parent->get_page_id(), true);
            delete new_parent;
        }
        buffer_pool_manager_->unpin_page(parent->get_page_id(), true);
        delete parent;
    }
}

/**
 * @brief 将指定键值对插入到B+树中
 * @return page_id_t 插入到的叶结点的page_no
 */
page_id_t IxIndexHandle::insert_entry(const char *key, const Rid &value, Transaction *transaction) {
    std::scoped_lock lock{root_latch_};
    auto [leaf, root_is_latched] = find_leaf_page(key, Operation::INSERT, transaction);
    int size_before = leaf->get_size();
    int size_after = leaf->insert(key, value);
    page_id_t leaf_page_no = leaf->get_page_no();
    if (size_after == size_before) {
        // key重复，未插入
        buffer_pool_manager_->unpin_page(leaf->get_page_id(), false);
        delete leaf;
        return leaf_page_no;
    }
    // 结点已满，需要分裂
    if (leaf->get_size() >= leaf->get_max_size()) {
        IxNodeHandle *new_node = split(leaf);
        if (file_hdr_->last_leaf_ == leaf->get_page_no()) {
            file_hdr_->last_leaf_ = new_node->get_page_no();
        }
        insert_into_parent(leaf, new_node->get_key(0), new_node, transaction);
        buffer_pool_manager_->unpin_page(new_node->get_page_id(), true);
        delete new_node;
    }
    buffer_pool_manager_->unpin_page(leaf->get_page_id(), true);
    delete leaf;
    return leaf_page_no;
}

/**
 * @brief 用于删除B+树中含有指定key的键值对
 */
bool IxIndexHandle::delete_entry(const char *key, Transaction *transaction) {
    std::scoped_lock lock{root_latch_};
    auto [leaf, root_is_latched] = find_leaf_page(key, Operation::DELETE, transaction);
    int size_before = leaf->get_size();
    int size_after = leaf->remove(key);
    bool deleted = (size_after != size_before);
    if (!deleted) {
        buffer_pool_manager_->unpin_page(leaf->get_page_id(), false);
        delete leaf;
        return false;
    }
    // 若删除的是首key，更新父节点中的索引key
    if (leaf->get_size() > 0) {
        maintain_parent(leaf);
    }
    // 处理可能的下溢；coalesce_or_redistribute不会unpin/delete输入的leaf，由本函数负责
    bool leaf_should_delete = coalesce_or_redistribute(leaf, transaction, nullptr);
    buffer_pool_manager_->unpin_page(leaf->get_page_id(), true);
    if (leaf_should_delete) {
        buffer_pool_manager_->delete_page(leaf->get_page_id());
    }
    delete leaf;
    return true;
}

/**
 * @brief 用于处理合并和重分配的逻辑
 * @return 是否需要删除（输入的node）结点，由调用者负责unpin并delete其页面
 * @note 本函数不会unpin输入的node
 */
bool IxIndexHandle::coalesce_or_redistribute(IxNodeHandle *node, Transaction *transaction, bool *root_is_latched) {
    if (node->is_root_page()) {
        return adjust_root(node);
    }
    // 未发生下溢，无需处理
    if (node->get_size() >= node->get_min_size()) {
        return false;
    }
    IxNodeHandle *parent = fetch_node(node->get_parent_page_no());
    int index = parent->find_child(node);
    // 优先选取前驱结点作为兄弟结点
    IxNodeHandle *neighbor;
    if (index == 0) {
        neighbor = fetch_node(parent->value_at(1));
    } else {
        neighbor = fetch_node(parent->value_at(index - 1));
    }

    if (node->get_size() + neighbor->get_size() >= node->get_min_size() * 2) {
        // 重新分配，node得以保留
        redistribute(neighbor, node, parent, index);
        buffer_pool_manager_->unpin_page(parent->get_page_id(), true);
        buffer_pool_manager_->unpin_page(neighbor->get_page_id(), true);
        delete parent;
        delete neighbor;
        return false;
    }
    // 合并：始终把输入node合并到neighbor中，node需要被删除
    bool parent_should_delete = coalesce(&neighbor, &node, &parent, index, transaction, root_is_latched);
    buffer_pool_manager_->unpin_page(parent->get_page_id(), true);
    if (parent_should_delete) {
        buffer_pool_manager_->delete_page(parent->get_page_id());
    }
    buffer_pool_manager_->unpin_page(neighbor->get_page_id(), true);
    delete parent;
    delete neighbor;
    return true;
}

/**
 * @brief 用于当根结点被删除了一个键值对之后的处理
 * @return bool 根结点是否需要被删除
 */
bool IxIndexHandle::adjust_root(IxNodeHandle *old_root_node) {
    // 1. 根是内部结点且只剩一个孩子，将其孩子设为新的根
    if (!old_root_node->is_leaf_page() && old_root_node->get_size() == 1) {
        page_id_t child_page_no = old_root_node->remove_and_return_only_child();
        IxNodeHandle *new_root = fetch_node(child_page_no);
        new_root->set_parent_page_no(IX_NO_PAGE);
        update_root_page_no(child_page_no);
        buffer_pool_manager_->unpin_page(new_root->get_page_id(), true);
        delete new_root;
        release_node_handle(*old_root_node);
        return true;
    }
    // 2. 根是叶子结点且为空，更新root为IX_NO_PAGE（整棵树为空）
    if (old_root_node->is_leaf_page() && old_root_node->get_size() == 0) {
        // 保留空叶子作为根，避免后续插入找不到根；这里不删除根页
        return false;
    }
    return false;
}

/**
 * @brief 重新分配node和兄弟结点neighbor_node的键值对
 */
void IxIndexHandle::redistribute(IxNodeHandle *neighbor_node, IxNodeHandle *node, IxNodeHandle *parent, int index) {
    if (index == 0) {
        // neighbor是node的后继结点：node(left) neighbor(right)
        // 从neighbor头部移动一个键值对到node尾部
        node->insert_pair(node->get_size(), neighbor_node->get_key(0), *neighbor_node->get_rid(0));
        neighbor_node->erase_pair(0);
        maintain_child(node, node->get_size() - 1);
        // 更新parent中指向neighbor的key为neighbor的新首key
        int neighbor_idx = parent->find_child(neighbor_node);
        parent->set_key(neighbor_idx, neighbor_node->get_key(0));
    } else {
        // neighbor是node的前驱结点：neighbor(left) node(right)
        // 从neighbor尾部移动一个键值对到node头部
        int last = neighbor_node->get_size() - 1;
        node->insert_pair(0, neighbor_node->get_key(last), *neighbor_node->get_rid(last));
        neighbor_node->erase_pair(last);
        maintain_child(node, 0);
        // 更新parent中指向node的key为node的新首key
        parent->set_key(index, node->get_key(0));
    }
}

/**
 * @brief 合并node和其前驱neighbor_node（保证node在右边，合并到左边的neighbor）
 * @return true means parent node should be deleted
 */
bool IxIndexHandle::coalesce(IxNodeHandle **neighbor_node, IxNodeHandle **node, IxNodeHandle **parent, int index,
                             Transaction *transaction, bool *root_is_latched) {
    IxNodeHandle *neigh = *neighbor_node;
    IxNodeHandle *n = *node;  // 始终把n合并掉（由调用者删除其页面）
    if (index == 0) {
        // neighbor是node的后继(右兄弟)：把n的键值对前插到neighbor头部
        neigh->insert_pairs(0, n->get_key(0), n->get_rid(0), n->get_size());
        if (!neigh->is_leaf_page()) {
            for (int i = 0; i < n->get_size(); i++) {
                maintain_child(neigh, i);
            }
        } else {
            erase_leaf(n);
            if (file_hdr_->first_leaf_ == n->get_page_no()) {
                file_hdr_->first_leaf_ = neigh->get_page_no();
            }
        }
        // 从parent删除n的entry(下标0)，neighbor移动到下标0，更新其key为合并后的首key
        (*parent)->erase_pair(0);
        (*parent)->set_key(0, neigh->get_key(0));
    } else {
        // neighbor是node的前驱(左兄弟)：把n的键值对追加到neighbor尾部
        int neigh_size = neigh->get_size();
        neigh->insert_pairs(neigh_size, n->get_key(0), n->get_rid(0), n->get_size());
        if (!neigh->is_leaf_page()) {
            for (int i = neigh_size; i < neigh->get_size(); i++) {
                maintain_child(neigh, i);
            }
        } else {
            erase_leaf(n);
            if (file_hdr_->last_leaf_ == n->get_page_no()) {
                file_hdr_->last_leaf_ = neigh->get_page_no();
            }
        }
        // 从parent删除n的entry
        (*parent)->erase_pair(index);
    }
    release_node_handle(*n);
    // 递归处理parent是否下溢
    return coalesce_or_redistribute(*parent, transaction, root_is_latched);
}

Rid IxIndexHandle::get_rid(const Iid &iid) const {
    IxNodeHandle *node = fetch_node(iid.page_no);
    if (iid.slot_no >= node->get_size()) {
        delete node;
        throw IndexEntryNotFoundError();
    }
    Rid rid = *node->get_rid(iid.slot_no);
    buffer_pool_manager_->unpin_page(node->get_page_id(), false);  // unpin it!
    delete node;
    return rid;
}

/**
 * @brief FindLeafPage + lower_bound，返回第一个>=key的位置
 */
Iid IxIndexHandle::lower_bound(const char *key) {
    std::scoped_lock lock{root_latch_};
    auto [leaf, root_is_latched] = find_leaf_page(key, Operation::FIND, nullptr);
    int pos = leaf->lower_bound(key);
    Iid iid;
    if (pos >= leaf->get_size()) {
        if (leaf->get_page_no() == file_hdr_->last_leaf_) {
            iid = {.page_no = leaf->get_page_no(), .slot_no = pos};
        } else {
            iid = {.page_no = leaf->get_next_leaf(), .slot_no = 0};
        }
    } else {
        iid = {.page_no = leaf->get_page_no(), .slot_no = pos};
    }
    buffer_pool_manager_->unpin_page(leaf->get_page_id(), false);
    delete leaf;
    return iid;
}

/**
 * @brief FindLeafPage + upper_bound，返回第一个>key的位置
 */
Iid IxIndexHandle::upper_bound(const char *key) {
    std::scoped_lock lock{root_latch_};
    auto [leaf, root_is_latched] = find_leaf_page(key, Operation::FIND, nullptr);
    int pos = leaf->lower_bound(key);
    // 跳过等于key的项（唯一索引下至多一个）得到第一个>key的位置
    if (pos < leaf->get_size() &&
        ix_compare(leaf->get_key(pos), key, file_hdr_->col_types_, file_hdr_->col_lens_) == 0) {
        pos++;
    }
    Iid iid;
    if (pos >= leaf->get_size()) {
        if (leaf->get_page_no() == file_hdr_->last_leaf_) {
            iid = {.page_no = leaf->get_page_no(), .slot_no = pos};
        } else {
            iid = {.page_no = leaf->get_next_leaf(), .slot_no = 0};
        }
    } else {
        iid = {.page_no = leaf->get_page_no(), .slot_no = pos};
    }
    buffer_pool_manager_->unpin_page(leaf->get_page_id(), false);
    delete leaf;
    return iid;
}

/**
 * @brief 指向最后一个叶子的最后一个结点的后一个
 */
Iid IxIndexHandle::leaf_end() const {
    IxNodeHandle *node = fetch_node(file_hdr_->last_leaf_);
    Iid iid = {.page_no = file_hdr_->last_leaf_, .slot_no = node->get_size()};
    buffer_pool_manager_->unpin_page(node->get_page_id(), false);  // unpin it!
    delete node;
    return iid;
}

/**
 * @brief 指向第一个叶子的第一个结点
 */
Iid IxIndexHandle::leaf_begin() const {
    Iid iid = {.page_no = file_hdr_->first_leaf_, .slot_no = 0};
    return iid;
}

/**
 * @brief 获取一个指定结点
 * @note pin the page, remember to unpin it outside!
 */
IxNodeHandle *IxIndexHandle::fetch_node(int page_no) const {
    Page *page = buffer_pool_manager_->fetch_page(PageId{fd_, page_no});
    IxNodeHandle *node = new IxNodeHandle(file_hdr_, page);
    return node;
}

/**
 * @brief 创建一个新结点
 * @note pin the page, remember to unpin it outside!
 */
IxNodeHandle *IxIndexHandle::create_node() {
    IxNodeHandle *node;
    file_hdr_->num_pages_++;

    PageId new_page_id = {.fd = fd_, .page_no = INVALID_PAGE_ID};
    Page *page = buffer_pool_manager_->new_page(&new_page_id);
    node = new IxNodeHandle(file_hdr_, page);
    return node;
}

/**
 * @brief 从node开始更新其父节点的第一个key，一直向上更新直到根节点
 */
void IxIndexHandle::maintain_parent(IxNodeHandle *node) {
    IxNodeHandle *curr = node;
    while (curr->get_parent_page_no() != IX_NO_PAGE) {
        IxNodeHandle *parent = fetch_node(curr->get_parent_page_no());
        int rank = parent->find_child(curr);
        char *parent_key = parent->get_key(rank);
        char *child_first_key = curr->get_key(0);
        if (memcmp(parent_key, child_first_key, file_hdr_->col_tot_len_) == 0) {
            assert(buffer_pool_manager_->unpin_page(parent->get_page_id(), true));
            delete parent;
            break;
        }
        memcpy(parent_key, child_first_key, file_hdr_->col_tot_len_);  // 修改了parent node
        if (curr != node) {
            delete curr;
        }
        curr = parent;

        assert(buffer_pool_manager_->unpin_page(parent->get_page_id(), true));
    }
    if (curr != node) {
        delete curr;
    }
}

/**
 * @brief 要删除leaf之前调用此函数，更新leaf前驱结点的next指针和后继结点的prev指针
 */
void IxIndexHandle::erase_leaf(IxNodeHandle *leaf) {
    assert(leaf->is_leaf_page());

    IxNodeHandle *prev = fetch_node(leaf->get_prev_leaf());
    prev->set_next_leaf(leaf->get_next_leaf());
    buffer_pool_manager_->unpin_page(prev->get_page_id(), true);
    delete prev;

    IxNodeHandle *next = fetch_node(leaf->get_next_leaf());
    next->set_prev_leaf(leaf->get_prev_leaf());  // 注意此处是SetPrevLeaf()
    buffer_pool_manager_->unpin_page(next->get_page_id(), true);
    delete next;
}

/**
 * @brief 删除node时，更新file_hdr_.num_pages
 */
void IxIndexHandle::release_node_handle(IxNodeHandle &node) {
    file_hdr_->num_pages_--;
}

/**
 * @brief 将node的第child_idx个孩子结点的父节点置为node
 */
void IxIndexHandle::maintain_child(IxNodeHandle *node, int child_idx) {
    if (!node->is_leaf_page()) {
        int child_page_no = node->value_at(child_idx);
        IxNodeHandle *child = fetch_node(child_page_no);
        child->set_parent_page_no(node->get_page_no());
        buffer_pool_manager_->unpin_page(child->get_page_id(), true);
        delete child;
    }
}
