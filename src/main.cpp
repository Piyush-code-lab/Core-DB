#include <iostream>
#include <string>
#include <sstream>
#include <vector>
#include <fstream>
#include <cstring>
#include <cstdint>
#include <unordered_map>
#include <list>
using namespace std;

// ===================== ROW =====================
// One row of data: an id, a username, and an email.
// We use fixed-size character arrays (not std::string) so every row
// takes up EXACTLY the same number of bytes. That's what lets us
// calculate "row number 5 lives at byte offset X" instantly.
struct Row {
    uint32_t id;
    char username[33]; // 32 letters + 1 spare byte (a rule for C-style text)
    char email[256];   // 255 letters + 1 spare byte
};

const uint32_t ROW_SIZE = sizeof(Row);
const uint32_t PAGE_SIZE = 4096; // 4 KB -- matches how operating systems read disks
const uint32_t ROWS_PER_PAGE = PAGE_SIZE / ROW_SIZE;
const uint32_t TABLE_MAX_PAGES = 100; // simple fixed limit for now

// ===================== PAGER =====================
// The Pager's only job: read/write 4KB pages to/from a file on disk.

const uint32_t BUFFER_POOL_SIZE = 10; // max pages allowed in memory at once
// ===================== B-TREE LEAF NODE LAYOUT =====================
// A leaf node lives inside one 4KB page. Layout (bytes):
// [0]                : node type (0 = leaf, 1 = internal) -- 1 byte
// [1]                : number of cells currently stored    -- 1 byte (max 255 is plenty)
// [2 ...]            : cells, each cell = {key (4 bytes), Row (ROW_SIZE bytes)}

const uint32_t LEAF_NODE_HEADER_SIZE = 2;
const uint32_t LEAF_NODE_CELL_SIZE = sizeof(uint32_t) + ROW_SIZE; // key + row
const uint32_t LEAF_NODE_MAX_CELLS = (PAGE_SIZE - LEAF_NODE_HEADER_SIZE) / LEAF_NODE_CELL_SIZE;

// Helper functions to read/write specific fields inside a leaf node's raw page bytes
uint8_t* leaf_node_num_cells(char* node) {
    return (uint8_t*)(node + 1);
}

char* leaf_node_cell(char* node, uint32_t cell_num) {
    return node + LEAF_NODE_HEADER_SIZE + cell_num * LEAF_NODE_CELL_SIZE;
}

uint32_t* leaf_node_key(char* node, uint32_t cell_num) {
    return (uint32_t*)leaf_node_cell(node, cell_num);
}

char* leaf_node_value(char* node, uint32_t cell_num) {
    return leaf_node_cell(node, cell_num) + sizeof(uint32_t);
}

void initialize_leaf_node(char* node) {
    node[0] = 0; // node type = leaf
    *leaf_node_num_cells(node) = 0;
}

// Inserts a key+row into a leaf node at the correct sorted position.
// NOTE: this version does NOT handle the node-full case yet -- that's next.
void leaf_node_insert(char* node, uint32_t key, const Row& row) {
    uint32_t num_cells = *leaf_node_num_cells(node);

    // Find the correct sorted position for this key (simple linear search for now)
    uint32_t insert_pos = 0;
    while (insert_pos < num_cells && *leaf_node_key(node, insert_pos) < key) {
        insert_pos++;
    }

    // Shift every cell after insert_pos one slot to the right, to make room
    for (uint32_t i = num_cells; i > insert_pos; i--) {
        memcpy(leaf_node_cell(node, i), leaf_node_cell(node, i - 1), LEAF_NODE_CELL_SIZE);
    }

    // Write the new key + row into the now-empty slot
    *leaf_node_key(node, insert_pos) = key;
    memcpy(leaf_node_value(node, insert_pos), &row, ROW_SIZE);

    *leaf_node_num_cells(node) = num_cells + 1;
}
// Splits a full leaf node into two leaves. The new key+row is inserted
// into whichever half it belongs in during the split.
// new_node must point to a fresh, zeroed PAGE_SIZE block (the new leaf).
void leaf_node_split_and_insert(char* old_node, char* new_node, uint32_t key, const Row& row) {
    initialize_leaf_node(new_node);

    // Build a temporary list of all cells (existing + the new one), sorted.
    // Simple approach: collect everything, insert the new one in sorted position,
    // then distribute the first half to old_node, second half to new_node.

    uint32_t total_cells = LEAF_NODE_MAX_CELLS + 1; // existing (full) + 1 new
    uint32_t split_point = total_cells / 2; // first half stays in old_node

    // Find where the new key belongs among the existing cells
    uint32_t insert_pos = 0;
    while (insert_pos < LEAF_NODE_MAX_CELLS && *leaf_node_key(old_node, insert_pos) < key) {
        insert_pos++;
    }

    // Walk through all "virtual" cell positions (0 to total_cells-1) in order.
    // For each position, figure out if it's the new cell or an existing one,
    // and copy it to the correct destination node (old or new half).
    for (int32_t i = total_cells - 1; i >= 0; i--) {
        char* destination_node = ((uint32_t)i >= split_point) ? new_node : old_node;
        uint32_t index_within_node = i % split_point;

        if ((uint32_t)i == insert_pos) {
            // This position is where the new key goes
            *leaf_node_key(destination_node, index_within_node) = key;
            memcpy(leaf_node_value(destination_node, index_within_node), &row, ROW_SIZE);
        } else if ((uint32_t)i > insert_pos) {
            // Existing cell, shifted right by one because new key was inserted before it
            memcpy(leaf_node_cell(destination_node, index_within_node),
                   leaf_node_cell(old_node, i - 1), LEAF_NODE_CELL_SIZE);
        } else {
            // Existing cell, stays at the same relative position
            memcpy(leaf_node_cell(destination_node, index_within_node),
                   leaf_node_cell(old_node, i), LEAF_NODE_CELL_SIZE);
        }
    }

    *leaf_node_num_cells(old_node) = split_point;
    *leaf_node_num_cells(new_node) = total_cells - split_point;
}

// Searches a leaf node for a given key. Returns the row via output parameter.
// Returns true if found, false if not.
bool leaf_node_search(char* node, uint32_t key, Row* out_row) {
    uint32_t num_cells = *leaf_node_num_cells(node);
    for (uint32_t i = 0; i < num_cells; i++) {
        if (*leaf_node_key(node, i) == key) {
            memcpy(out_row, leaf_node_value(node, i), ROW_SIZE);
            return true;
        }
    }
    return false;
}
// ===================== B-TREE INTERNAL NODE LAYOUT =====================
// [0] node type (1 = internal)
// [1] number of keys
// [2...] : fixed-size children array, then fixed-size keys array (separator keys)

const uint32_t INTERNAL_NODE_HEADER_SIZE = 2;
const uint32_t INTERNAL_NODE_MAX_KEYS = (PAGE_SIZE - INTERNAL_NODE_HEADER_SIZE - sizeof(uint32_t)) / (2 * sizeof(uint32_t));

uint8_t get_node_type(char* node) {
    return (uint8_t)node[0];
}

uint8_t* internal_node_num_keys(char* node) {
    return (uint8_t*)(node + 1);
}

// child pointers live right after the header (fixed-size slots, regardless of current num_keys)
uint32_t* internal_node_child(char* node, uint32_t child_num) {
    return (uint32_t*)(node + INTERNAL_NODE_HEADER_SIZE + child_num * sizeof(uint32_t));
}

// keys live after ALL possible child slots -- keeping this fixed avoids
// shifting offsets every time num_keys changes
uint32_t* internal_node_key(char* node, uint32_t key_num) {
    return (uint32_t*)(node + INTERNAL_NODE_HEADER_SIZE
                        + (INTERNAL_NODE_MAX_KEYS + 1) * sizeof(uint32_t)
                        + key_num * sizeof(uint32_t));
}

void initialize_internal_node(char* node) {
    node[0] = 1; // node type = internal
    *internal_node_num_keys(node) = 0;
}

// Given a search key, decide which child index to descend into.
uint32_t internal_node_find_child(char* node, uint32_t key) {
    uint32_t num_keys = *internal_node_num_keys(node);
    for (uint32_t i = 0; i < num_keys; i++) {
        if (key < *internal_node_key(node, i)) {
            return i;
        }
    }
    return num_keys; // key is >= every separator -> goes in the last child
}
// Inserts a new separator key + child pointer into an internal node,
// right after the child that just split (at child_index).
void internal_node_insert(char* node, uint32_t child_index, uint32_t new_key, uint32_t new_child_page_num) {
    uint32_t num_keys = *internal_node_num_keys(node);

    for (int32_t i = num_keys; i > (int32_t)child_index; i--) {
        *internal_node_child(node, i + 1) = *internal_node_child(node, i);
    }
    *internal_node_child(node, child_index + 1) = new_child_page_num;

    for (int32_t i = num_keys; i > (int32_t)child_index; i--) {
        *internal_node_key(node, i) = *internal_node_key(node, i - 1);
    }
    *internal_node_key(node, child_index) = new_key;

    *internal_node_num_keys(node) = num_keys + 1;
}

class Pager {
public:
    fstream file;
    uint32_t file_length;

    // Doubly linked list of {page_num, page_data}.
    // Front = most recently used. Back = least recently used.
    list<pair<uint32_t, char*>> lru_list;

    // Maps page_num -> its exact position (iterator) inside lru_list.
    // This gives instant lookup instead of searching the list.
    unordered_map<uint32_t, list<pair<uint32_t, char*>>::iterator> page_map;

    Pager(const string& filename) {
        file.open(filename, ios::in | ios::out | ios::binary);
        if (!file.is_open()) {
            file.open(filename, ios::out | ios::binary);
            file.close();
            file.open(filename, ios::in | ios::out | ios::binary);
        }
        file.seekg(0, ios::end);
        file_length = (uint32_t)file.tellg();
    }

    char* get_page(uint32_t page_num) {
        // CASE 1: Page already cached in memory
        auto it = page_map.find(page_num);
        if (it != page_map.end()) {
            // Move to front of list -- mark as "just used"
            lru_list.splice(lru_list.begin(), lru_list, it->second);
            return it->second->second;
        }

        // CASE 2: Not cached. Evict if we're full, then load it.
        if (lru_list.size() >= BUFFER_POOL_SIZE) {
            evict_least_recently_used();
        }

        char* page = new char[PAGE_SIZE];
        memset(page, 0, PAGE_SIZE);

        uint32_t num_pages_on_disk = file_length / PAGE_SIZE;
        if (file_length % PAGE_SIZE != 0) num_pages_on_disk++;

        if (page_num < num_pages_on_disk) {
            file.seekg(page_num * PAGE_SIZE, ios::beg);
            file.read(page, PAGE_SIZE);
        }

        lru_list.push_front({page_num, page});
        page_map[page_num] = lru_list.begin();

        return page;
    }

    void evict_least_recently_used() {
        auto& [evict_page_num, evict_page_data] = lru_list.back();

        // Must save to disk before discarding, or we lose data
        flush_page(evict_page_num, evict_page_data);

        delete[] evict_page_data;
        page_map.erase(evict_page_num);
        lru_list.pop_back();
    }

    void flush_page(uint32_t page_num, char* page_data) {
        file.seekp(page_num * PAGE_SIZE, ios::beg);
        file.write(page_data, PAGE_SIZE);
        file.flush();

        // Keep file_length in sync with what's actually been written to disk
        uint32_t end_byte = (page_num + 1) * PAGE_SIZE;
        if (end_byte > file_length) {
            file_length = end_byte;
        }
    }

    void flush(uint32_t page_num) {
        auto it = page_map.find(page_num);
        if (it != page_map.end()) {
            flush_page(page_num, it->second->second);
        }
    }

    ~Pager() {
        for (auto& [page_num, page_data] : lru_list) {
            flush_page(page_num, page_data);
            delete[] page_data;
        }
        file.close();
    }
};

// ===================== TABLE =====================
// Sits on top of the Pager. Knows how many rows exist and where
// any given row number lives.
class Table {
public:
    Pager* pager;
    uint32_t num_rows;
    uint32_t next_page_num;
    uint32_t root_page_num;
    string meta_filename;

    Table(const string& filename) {
        pager = new Pager(filename);
        meta_filename = filename + ".meta";
        root_page_num = 0;

        ifstream meta_in(meta_filename, ios::binary);
        if (meta_in.is_open()) {
            meta_in.read(reinterpret_cast<char*>(&num_rows), sizeof(num_rows));
            meta_in.read(reinterpret_cast<char*>(&next_page_num), sizeof(next_page_num));
            meta_in.close();
        } else {
            num_rows = 0;
            next_page_num = 1; // page 0 is reserved for the root
            char* root = pager->get_page(0);
            initialize_leaf_node(root);
        }
    }

    uint32_t allocate_new_page() {
        uint32_t p = next_page_num;
        next_page_num++;
        pager->get_page(p); // brings a fresh, zeroed page into cache
        return p;
    }

    void insert_row(uint32_t key, const Row& row) {
        char* root = pager->get_page(root_page_num);

        if (get_node_type(root) == 0) {
            // ROOT IS A LEAF
            if (*leaf_node_num_cells(root) < LEAF_NODE_MAX_CELLS) {
                leaf_node_insert(root, key, row);
            } else {
                // Root leaf is full -> split it, root becomes internal
                uint32_t left_page_num = allocate_new_page();
                uint32_t right_page_num = allocate_new_page();
                char* left = pager->get_page(left_page_num);
                char* right = pager->get_page(right_page_num);

                memcpy(left, root, PAGE_SIZE); // copy current leaf data into left half
                leaf_node_split_and_insert(left, right, key, row);

                initialize_internal_node(root);
                *internal_node_num_keys(root) = 1;
                *internal_node_key(root, 0) = *leaf_node_key(right, 0);
                *internal_node_child(root, 0) = left_page_num;
                *internal_node_child(root, 1) = right_page_num;
            }
        } else {
            // ROOT IS INTERNAL -> route to the correct leaf child
            uint32_t child_index = internal_node_find_child(root, key);
            uint32_t child_page_num = *internal_node_child(root, child_index);
            char* child = pager->get_page(child_page_num);

            if (*leaf_node_num_cells(child) < LEAF_NODE_MAX_CELLS) {
                leaf_node_insert(child, key, row);
            } else {
                uint32_t root_num_keys = *internal_node_num_keys(root);
                if (root_num_keys >= INTERNAL_NODE_MAX_KEYS) {
                    cout << "Error: root is full. Root-splitting is not implemented (scoped out)." << endl;
                    return;
                }
                uint32_t new_leaf_page_num = allocate_new_page();
                char* new_leaf = pager->get_page(new_leaf_page_num);

                leaf_node_split_and_insert(child, new_leaf, key, row);
                internal_node_insert(root, child_index, *leaf_node_key(new_leaf, 0), new_leaf_page_num);
            }
        }

        num_rows++;
    }

    // Recursively prints every row, in sorted order, by visiting leaves left to right
    void select_all_node(uint32_t page_num) {
        char* node = pager->get_page(page_num);

        if (get_node_type(node) == 0) {
            uint32_t num_cells = *leaf_node_num_cells(node);
            for (uint32_t i = 0; i < num_cells; i++) {
                Row row;
                memcpy(&row, leaf_node_value(node, i), ROW_SIZE);
                cout << "(" << row.id << ", " << row.username << ", " << row.email << ")" << endl;
            }
        } else {
            uint32_t num_keys = *internal_node_num_keys(node);
            for (uint32_t i = 0; i <= num_keys; i++) {
                select_all_node(*internal_node_child(node, i));
            }
        }
    }

    void select_all() {
        select_all_node(root_page_num);
    }

    // Real B-Tree point lookup -- O(log n) instead of scanning everything
    bool find_row(uint32_t key, Row* out_row) {
        uint32_t page_num = root_page_num;
        char* node = pager->get_page(page_num);

        while (get_node_type(node) == 1) { // internal -> keep descending
            uint32_t child_index = internal_node_find_child(node, key);
            page_num = *internal_node_child(node, child_index);
            node = pager->get_page(page_num);
        }

        return leaf_node_search(node, key, out_row);
    }

    void close() {
        ofstream meta_out(meta_filename, ios::binary);
        meta_out.write(reinterpret_cast<char*>(&num_rows), sizeof(num_rows));
        meta_out.write(reinterpret_cast<char*>(&next_page_num), sizeof(next_page_num));
        meta_out.close();
    }

    ~Table() {
        delete pager;
    }
};



// ===================== TOKENIZER (from Phase 1) =====================
vector<string> tokenize(const string& input) {
    vector<string> tokens;
    stringstream ss(input);
    string word;
    while (ss >> word) tokens.push_back(word);
    return tokens;
}

// ===================== MAIN =====================
#ifndef TESTING
int main() {
    Table table("coredb.db"); // the actual file on disk where data lives
    string input;

    while (true) {
        cout << "db > ";
        getline(cin, input);

        if (input == ".exit") {
            table.close();
            break;
        }

        vector<string> tokens = tokenize(input);
        if (tokens.empty()) continue;

        string command = tokens[0];

        if (command == "INSERT") {
            if (tokens.size() != 4) {
                cout << "Error: INSERT requires exactly 3 arguments (id, username, email)" << endl;
                continue;
            }
            if (tokens[2].size() > 32 || tokens[3].size() > 255) {
                cout << "Error: username or email too long" << endl;
                continue;
            }

            Row row;
            try {
                row.id = stoul(tokens[1]);
            } catch (...) {
                cout << "Error: id must be a number" << endl;
                continue;
            }
            strncpy(row.username, tokens[2].c_str(), sizeof(row.username) - 1);
            row.username[sizeof(row.username) - 1] = '\0';
            strncpy(row.email, tokens[3].c_str(), sizeof(row.email) - 1);
            row.email[sizeof(row.email) - 1] = '\0';

            table.insert_row(row.id, row);
            cout << "Inserted." << endl;

        } else if (command == "SELECT") {
            table.select_all();

        } else if (command == "FIND") {
            if (tokens.size() != 2) {
                cout << "Error: FIND requires exactly 1 argument (id)" << endl;
                continue;
            }
            uint32_t key;
            try {
                key = stoul(tokens[1]);
            } catch (...) {
                cout << "Error: id must be a number" << endl;
                continue;
            }
            Row found;
            if (table.find_row(key, &found)) {
                cout << "(" << found.id << ", " << found.username << ", " << found.email << ")" << endl;
            } else {
                cout << "Not found." << endl;
            }

        } else {
            cout << "Unrecognized command: " << command << endl;
        }
    }

    return 0;
}
#endif